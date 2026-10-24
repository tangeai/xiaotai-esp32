#include "xmini_rtc.h"
#include "xmini_rtc_limits.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define EVENT_COUNT 8
#define TX_COUNT 5
#define SEND_LIMIT (16U * 1024U)
#define SEND_WATERMARK (5U * (XMINI_RTC_PCMA_BYTES + sizeof(TIRTCFRAMEINFO)))
#define DEFAULT_ENDPOINT "http://ep-tirtc.tange365.com"
#define DEFAULT_HTTPS_ALIAS "https://ep-tirtc.tange365.com"

typedef struct {
    uint32_t generation, pts, subscription_epoch;
    int64_t queued_us;
    uint8_t stream;
    uint8_t data[XMINI_RTC_PCMA_BYTES];
} tx_packet_t;

typedef struct {
    uint32_t generation, pts, epoch;
    uint16_t used;
    uint8_t stream;
    bool selected, busy;
    uint8_t data[XMINI_RTC_PCMA_BYTES];
} rx_assembler_t;

static const char *TAG = "xmini_rtc";
static StaticQueue_t s_event_queue_control, s_tx_queue_control;
static uint8_t s_event_storage[EVENT_COUNT * sizeof(xmini_rtc_event_t)];
static uint8_t s_tx_storage[TX_COUNT * sizeof(tx_packet_t)];
static QueueHandle_t s_events, s_tx;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xmini_rtc_receiver_t s_receiver;
static void *s_receiver_context;
static TaskHandle_t s_owner;
static xmini_rtc_status_t s_status;
static xmini_rtc_transport_diagnostics_t s_transport_diagnostics;
_Static_assert(sizeof(s_transport_diagnostics) + sizeof(uint32_t) < 256,
               "Transport snapshots must remain bounded without a heap table");
static tirtc_conn_t s_connection, s_closing, s_rejected, s_rejected_closing;
static uint32_t s_closing_generation, s_pending_generation, s_inbound_generation;
static uint32_t s_retiring_generation;
static uint32_t s_early_command_warned_generation;
static xmini_rtc_command_diagnostic_t s_command_diagnostics[XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS];
static xmini_rtc_command_diagnostic_summary_t s_command_diagnostic_summary;
static xmini_rtc_command_tx_diagnostic_t s_command_tx_diagnostic;
_Static_assert(sizeof(s_command_diagnostics) + sizeof(s_command_diagnostic_summary) == 168,
               "Command entry metadata must remain fixed at 168 bytes");
_Static_assert(sizeof(s_command_tx_diagnostic) == 32, "Command TX metadata must remain fixed at 32 bytes");
static bool s_accept_inbound, s_cancelled, s_pending, s_sdk_stopped, s_stopped_reported;
static bool s_overflow_reported, s_first_tx, s_first_rx, s_rx_reported, s_format_reported, s_tx_error_reported;
static bool s_enqueue_reject_reported, s_enqueue_full_reported;
static bool s_rx_pressure_reported;
static uint32_t s_subscription_epoch[16];
static rx_assembler_t s_rx;
_Static_assert(sizeof(s_rx) <= 180, "RX reblocking must not add a heap or stream table");
static char *s_peer, *s_token;
static size_t s_peer_size, s_token_size;
static atomic_size_t s_command_bytes;

/* Caller owns s_lock. Never reset busy while an admitted callback is outside it. */
static void rx_clear_locked(bool deselect)
{
    s_status.rx_partial_dropped += s_rx.used;
    s_status.rx_bytes_dropped += s_rx.used;
    s_rx.used = 0;
    ++s_rx.epoch;
    if (deselect) s_rx.selected = false;
}

void tgtrp_internal_diag_hook(const tgtrp_internal_diag_t *record)
{
    if (!record || record->version != TGTRP_INTERNAL_DIAG_VERSION ||
        record->stage < TGTRP_INTERNAL_DIAG_TX || record->stage > TGTRP_INTERNAL_DIAG_QUEUE)
        return;
    portENTER_CRITICAL(&s_lock);
    if (s_status.initialized) {
        xmini_rtc_transport_snapshot_t *snapshot = record->stage == TGTRP_INTERNAL_DIAG_TX
            ? &s_transport_diagnostics.tx : record->stage == TGTRP_INTERNAL_DIAG_RX
            ? &s_transport_diagnostics.rx : &s_transport_diagnostics.queue;
        snapshot->capture_sequence = ++s_status.transport_capture_sequence;
        snapshot->generation = s_connection ? s_status.generation :
            s_closing ? s_closing_generation : 0;
        snapshot->connection = (uintptr_t)(s_connection ? s_connection : s_closing);
        snapshot->transport = *record;
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool is_owner(void)
{
    return s_owner && xTaskGetCurrentTaskHandle() == s_owner;
}

static void clear_secret(void *memory, size_t length)
{
    volatile unsigned char *bytes = memory;
    while (length--)
        *bytes++ = 0;
}

static void release_inputs(void)
{
    if (s_peer) {
        clear_secret(s_peer, s_peer_size);
        free(s_peer);
    }
    if (s_token) {
        clear_secret(s_token, s_token_size);
        free(s_token);
    }
    s_peer = s_token = NULL;
    s_peer_size = s_token_size = 0;
}

static uint32_t connection_generation(tirtc_conn_t connection)
{
    return (uint32_t)(uintptr_t)TiRtcConnGetUserData(connection);
}

static bool emit(const xmini_rtc_event_t *event)
{
    if (s_events && xQueueSend(s_events, event, 0) == pdTRUE)
        return true;
    portENTER_CRITICAL(&s_lock);
    s_status.event_overflow++;
    s_status.faulted = true;
    s_status.last_error = TIRTC_E_BUSY;
    s_status.subscribed = 0;
    s_status.business_permit = 0;
    rx_clear_locked(true);
    portEXIT_CRITICAL(&s_lock);
    return false;
}

static void event_callback(int type, const void *data, int length)
{
    (void)data;
    (void)length;
    xmini_rtc_event_t event = {0};
    portENTER_CRITICAL(&s_lock);
    if (type == TIRTC_EVENT_SYS_STARTED) {
        s_status.online = true;
        event.kind = XMINI_RTC_STARTED;
    } else if (type == TIRTC_EVENT_SYS_STOPPED) {
        s_status.online = false;
        s_status.connected = false;
        s_status.connecting = false;
        s_status.subscribed = 0;
        s_status.business_permit = 0;
        s_connection = s_closing = s_rejected = s_rejected_closing = NULL;
        s_pending = false;
        s_retiring_generation = 0;
        s_status.sdk_pending = 0;
        rx_clear_locked(true);
        s_sdk_stopped = true;
        event.kind = XMINI_RTC_STOPPED;
    } else {
        event.kind = XMINI_RTC_ACCESS_HIJACKING;
        event.error = TIRTC_E_UNEXPECTED_RESPONSE;
        s_status.last_error = event.error;
    }
    portEXIT_CRITICAL(&s_lock);
    emit(&event);
}

static void connected_callback(int error, tirtc_conn_t connection, void *user)
{
    uint32_t generation = (uint32_t)(uintptr_t)user;
    if (connection)
        TiRtcConnSetUserData(connection, user);
    xmini_rtc_event_t event = {.kind = XMINI_RTC_CONNECTED,
                              .generation = generation,
                              .connection = connection,
                              .error = error};
#ifdef TIRTC_PENDING_CLOSE_CONTRACT
    event.retired = error && !connection;
#endif
    portENTER_CRITICAL(&s_lock);
    bool current = s_pending && generation == s_pending_generation;
    bool usable = current && !s_cancelled && !s_status.stopping && !s_status.faulted;
    if (current) {
        s_pending = false;
        s_status.connecting = false;
    }
    if (!error && connection && usable && !s_connection && !s_closing) {
        s_connection = connection;
        s_status.generation = generation;
        s_status.connected = true;
        s_status.business_permit = 0;
        s_first_rx = s_first_tx = s_rx_reported = false;
        s_format_reported = false;
        s_tx_error_reported = false;
        s_enqueue_reject_reported = s_enqueue_full_reported = false;
        s_rx_pressure_reported = false;
        rx_clear_locked(true);
    } else if (!error && connection) {
        s_rejected = connection;
        event.kind = XMINI_RTC_REJECTED;
    }
    if (error)
        s_status.last_error = error;
    portEXIT_CRITICAL(&s_lock);
    emit(&event);
}

static void accepted_callback(tirtc_conn_t connection)
{
    xmini_rtc_event_t event = {.kind = XMINI_RTC_REJECTED, .connection = connection};
    portENTER_CRITICAL(&s_lock);
    bool usable = s_accept_inbound && s_inbound_generation && s_status.online &&
                  !s_status.stopping && !s_status.faulted && !s_pending && !s_connection && !s_closing;
    uint32_t generation = usable ? s_inbound_generation : 0;
    if (usable) {
        s_accept_inbound = false;
        s_connection = connection;
        s_status.generation = generation;
        s_status.connected = true;
        s_status.business_permit = 0;
        s_first_rx = s_first_tx = s_rx_reported = false;
        s_format_reported = false;
        s_tx_error_reported = false;
        s_enqueue_reject_reported = s_enqueue_full_reported = false;
        s_rx_pressure_reported = false;
        rx_clear_locked(true);
        event.kind = XMINI_RTC_ACCEPTED;
        event.generation = generation;
    } else {
        s_rejected = connection;
    }
    portEXIT_CRITICAL(&s_lock);
    TiRtcConnSetUserData(connection, (void *)(uintptr_t)generation);
    emit(&event);
}

static void error_callback(tirtc_conn_t connection, int error)
{
    uint32_t generation = connection_generation(connection);
    portENTER_CRITICAL(&s_lock);
    if (s_connection == connection) {
        s_status.connected = false;
        s_status.subscribed = 0;
        s_status.business_permit = 0;
        rx_clear_locked(true);
    }
    s_status.last_error = error;
    portEXIT_CRITICAL(&s_lock);
    emit(&(xmini_rtc_event_t){.kind = XMINI_RTC_ERROR, .generation = generation,
                              .connection = connection, .error = error});
}

static void closed_callback(tirtc_conn_t connection)
{
    uint32_t generation = connection_generation(connection);
    portENTER_CRITICAL(&s_lock);
    if (s_connection == connection) {
        s_connection = NULL;
        s_status.connected = false;
        s_status.subscribed = 0;
        s_status.business_permit = 0;
        rx_clear_locked(true);
    }
    if (s_closing == connection) {
        s_closing = NULL;
        s_closing_generation = 0;
    }
    if (s_rejected == connection)
        s_rejected = NULL;
    if (s_rejected_closing == connection)
        s_rejected_closing = NULL;
    portEXIT_CRITICAL(&s_lock);
    emit(&(xmini_rtc_event_t){.kind = XMINI_RTC_CLOSED, .generation = generation,
                              .connection = connection});
}

static bool active_connection_locked(tirtc_conn_t connection, uint32_t generation)
{
    return s_connection == connection && s_status.generation == generation &&
           s_status.connected && !s_status.stopping && !s_status.faulted;
}

static bool active_connection(tirtc_conn_t connection, uint32_t generation)
{
    bool active;
    portENTER_CRITICAL(&s_lock);
    active = active_connection_locked(connection, generation);
    portEXIT_CRITICAL(&s_lock);
    return active;
}

static uint32_t command_entry_locked(tirtc_conn_t connection, uint32_t generation,
                                     uint32_t command, uint32_t length, uint32_t at_ms, bool active)
{
    if (s_command_diagnostic_summary.captures == UINT32_MAX) {
        s_command_diagnostic_summary.overwritten = UINT32_MAX;
        return 0;
    }
    uint32_t sequence = ++s_command_diagnostic_summary.captures;
    if (sequence > XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS)
        ++s_command_diagnostic_summary.overwritten;
    xmini_rtc_command_diagnostic_t *record = &s_command_diagnostics[(sequence - 1) % XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS];
    record->sequence = sequence;
    record->at_ms = at_ms;
    record->connection = (uint32_t)(uintptr_t)connection;
    record->command = command;
    record->length = length;
    record->observed_generation = generation;
    record->current_generation = s_status.generation;
    record->pending_generation = s_pending_generation;
    record->inbound_generation = s_inbound_generation;
    record->flags = (s_status.online ? XMINI_RTC_CMD_ONLINE : 0) |
        (s_status.connected ? XMINI_RTC_CMD_CONNECTED : 0) | (s_status.stopping ? XMINI_RTC_CMD_STOPPING : 0) |
        (s_status.faulted ? XMINI_RTC_CMD_FAULTED : 0) | (s_pending ? XMINI_RTC_CMD_PENDING : 0) |
        (s_cancelled ? XMINI_RTC_CMD_CANCELLED : 0) | (s_accept_inbound ? XMINI_RTC_CMD_ARMED : 0) |
        (s_connection == connection ? XMINI_RTC_CMD_CONNECTION_MATCH : 0) |
        (s_status.generation == generation ? XMINI_RTC_CMD_GENERATION_MATCH : 0) |
        (s_status.sdk_pending ? XMINI_RTC_CMD_SDK_PENDING : 0) | (s_closing ? XMINI_RTC_CMD_CLOSING_CONNECTION : 0) |
        (s_retiring_generation ? XMINI_RTC_CMD_RETIRING : 0) |
        (active ? XMINI_RTC_CMD_ACTIVE : 0) |
        ((uint32_t)(active ? XMINI_RTC_CMD_ENTRY : XMINI_RTC_CMD_INACTIVE) << XMINI_RTC_CMD_DISPOSITION_SHIFT);
    return sequence;
}

static void command_finished(uint32_t sequence, xmini_rtc_command_disposition_t disposition, bool queued)
{
    if (!sequence)
        return;
    portENTER_CRITICAL(&s_lock);
    xmini_rtc_command_diagnostic_t *record = &s_command_diagnostics[(sequence - 1) % XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS];
    if (record->sequence == sequence)
        record->flags = (record->flags & ~XMINI_RTC_CMD_DISPOSITION_MASK) |
            ((uint32_t)disposition << XMINI_RTC_CMD_DISPOSITION_SHIFT) | (queued ? XMINI_RTC_CMD_QUEUED : 0);
    portEXIT_CRITICAL(&s_lock);
}

static void audio_callback(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, void *data)
{
    uint32_t generation = connection_generation(connection);
    if (!active_connection(connection, generation)) {
        portENTER_CRITICAL(&s_lock);
        s_status.stale_frames++;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (!frame || !data || frame->media != TIRTC_AUDIO_ALAW ||
        frame->flags != TIRTC_AUDIOSAMPLE_8K16B1C || frame->stream_id > 15 ||
        !frame->length) {
        bool report;
        portENTER_CRITICAL(&s_lock);
        if (s_connection != connection || s_status.generation != generation ||
            !s_status.connected || s_status.stopping || s_status.faulted) {
            ++s_status.stale_frames;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        report = !s_format_reported;
        s_format_reported = true;
        s_status.format_errors++;
        s_status.last_error = TIRTC_E_INVALID_PARAMETER;
        if (s_rx.generation == generation && (!frame || frame->stream_id == s_rx.stream))
            rx_clear_locked(false);
        portEXIT_CRITICAL(&s_lock);
        if (report) {
            ESP_LOGE(TAG, "RX FORMAT gen=%lu frame=%u data=%u media=%u flags=%u stream=%u bytes=%lu expected=%u/%u/nonempty",
                     (unsigned long)generation, frame != NULL, data != NULL,
                     frame ? frame->media : 255U, frame ? frame->flags : 255U,
                     frame ? frame->stream_id : 255U, (unsigned long)(frame ? frame->length : 0),
                     (unsigned)TIRTC_AUDIO_ALAW, (unsigned)TIRTC_AUDIOSAMPLE_8K16B1C);
            emit(&(xmini_rtc_event_t){.kind = XMINI_RTC_FORMAT_ERROR,
                                      .generation = generation, .connection = connection,
                                      .error = TIRTC_E_INVALID_PARAMETER});
        }
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_connection != connection || s_status.generation != generation ||
        !s_status.connected || s_status.stopping || s_status.faulted) {
        ++s_status.stale_frames;
        s_status.rx_bytes_dropped += frame->length;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (!s_rx.selected || s_rx.generation != generation || s_rx.stream != frame->stream_id) {
        ++s_status.rx_unselected_frames;
        s_status.rx_bytes_dropped += frame->length;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (s_status.rx_input_generation != generation)
        s_status.rx_input_chunks = 0;
    s_status.rx_input_generation = generation;
    ++s_status.rx_input_chunks;
    s_status.rx_input_length = frame->length;
    s_status.rx_input_media = frame->media;
    s_status.rx_input_flags = frame->flags;
    s_status.rx_input_stream = frame->stream_id;
    s_status.rx_input_valid = 1;
    if (s_rx.busy) {
        rx_clear_locked(false);
        s_status.rx_dropped += frame->length / XMINI_RTC_PCMA_BYTES;
        s_status.rx_partial_dropped += frame->length % XMINI_RTC_PCMA_BYTES;
        s_status.rx_bytes_dropped += frame->length;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_rx.busy = true;
    uint32_t epoch = s_rx.epoch;
    if (!s_rx.used) s_rx.pts = frame->ts;
    portEXIT_CRITICAL(&s_lock);

    const uint8_t *payload = data;
    uint32_t remaining = frame->length, delivered = 0, dropped = 0;
    bool rejected_block = false;
    const char *reason = "callback_budget";
    uint8_t block[XMINI_RTC_PCMA_BYTES];
    while (remaining && delivered < XMINI_RTC_RX_BLOCKS_PER_CALLBACK) {
        uint32_t pts;
        bool complete;
        portENTER_CRITICAL(&s_lock);
        bool current = s_connection == connection && s_status.generation == generation &&
            s_status.connected && !s_status.stopping && !s_status.faulted &&
            s_rx.selected && s_rx.generation == generation &&
            s_rx.stream == frame->stream_id && s_rx.epoch == epoch;
        if (!current) {
            s_status.stale_frames += (remaining - 1U) / XMINI_RTC_PCMA_BYTES + 1U;
            s_status.rx_bytes_dropped += remaining;
            portEXIT_CRITICAL(&s_lock);
            remaining = 0;
            break;
        }
        uint32_t take = XMINI_RTC_PCMA_BYTES - s_rx.used;
        if (take > remaining) take = remaining;
        memcpy(s_rx.data + s_rx.used, payload, take);
        s_rx.used += take;
        payload += take;
        remaining -= take;
        complete = s_rx.used == XMINI_RTC_PCMA_BYTES;
        pts = s_rx.pts;
        if (complete) {
            memcpy(block, s_rx.data, sizeof(block));
            s_rx.used = 0;
            s_rx.pts += 20U;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!complete) break;
        if (!s_receiver || !s_receiver(generation, frame->stream_id, pts, block,
                                       sizeof(block), s_receiver_context)) {
            reason = "receiver";
            /* The rejected block and the rest of this borrowed chunk are lost.
             * Do not splice a later chunk across that known hole. */
            rejected_block = true;
            break;
        }
        ++delivered;
    }
    portENTER_CRITICAL(&s_lock);
    bool current_owner = s_status.generation == generation && s_rx.epoch == epoch;
    if (remaining || rejected_block) {
        dropped = remaining / XMINI_RTC_PCMA_BYTES + (rejected_block ? 1U : 0U);
        s_status.rx_dropped += dropped;
        s_status.rx_partial_dropped += remaining % XMINI_RTC_PCMA_BYTES;
        s_status.rx_bytes_dropped += remaining;
        if (rejected_block) s_status.rx_bytes_dropped += XMINI_RTC_PCMA_BYTES;
        if (s_rx.epoch == epoch && s_rx.generation == generation)
            rx_clear_locked(false);
    }
    s_rx.busy = false;
    s_status.rx_frames += delivered;
    if (delivered && current_owner) s_first_rx = true;
    bool report = (remaining || rejected_block) && current_owner && !s_rx_pressure_reported;
    if (report) s_rx_pressure_reported = true;
    portEXIT_CRITICAL(&s_lock);
    if (report)
        ESP_LOGW(TAG, "RX BACKPRESSURE gen=%lu stream=%u bytes=%lu delivered=%lu dropped=%lu reason=%s",
                 (unsigned long)generation, frame->stream_id, (unsigned long)frame->length,
                 (unsigned long)delivered, (unsigned long)dropped,
                 reason);
}

static void payload_callback(tirtc_conn_t connection, uint32_t command,
                             const void *data, uint32_t length, xmini_rtc_event_kind_t kind)
{
    uint32_t generation = connection_generation(connection);
    uint32_t command_sequence = 0;
    bool active;
    if (kind == XMINI_RTC_COMMAND) {
        portENTER_CRITICAL(&s_lock);
        active = active_connection_locked(connection, generation);
        uint32_t at_ms = (uint32_t)(esp_timer_get_time() / 1000);
        command_sequence = command_entry_locked(connection, generation, command, length, at_ms, active);
        portEXIT_CRITICAL(&s_lock);
    } else {
        active = active_connection(connection, generation);
    }
    if (!active) {
        uint32_t waiting_generation = 0, current_generation = 0, pending_generation = 0, inbound_generation = 0;
        tirtc_conn_t current_connection = NULL;
        unsigned flags = 0;
        portENTER_CRITICAL(&s_lock);
        if (kind == XMINI_RTC_COMMAND && s_status.online && !s_status.connected &&
            !s_status.stopping && !s_status.faulted) {
            uint32_t awaiting = s_pending && !s_cancelled ? s_pending_generation :
                s_accept_inbound ? s_inbound_generation : 0;
            if (awaiting && s_early_command_warned_generation != awaiting) {
                s_early_command_warned_generation = waiting_generation = awaiting;
                current_connection = s_connection;
                current_generation = s_status.generation;
                pending_generation = s_pending_generation;
                inbound_generation = s_inbound_generation;
                flags = (unsigned)s_pending | ((unsigned)s_accept_inbound << 1) |
                    ((unsigned)s_cancelled << 2) | ((unsigned)s_status.connected << 3) |
                    ((unsigned)s_status.stopping << 4) | ((unsigned)s_status.faulted << 5);
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if (waiting_generation)
            ESP_LOGW(TAG, "EARLY COMMAND dropped kind=%u word=%08lx length=%lu conn=%p observed_gen=%lu waiting_gen=%lu current=%p current_gen=%lu pending_gen=%lu inbound_gen=%lu pending=%u armed=%u cancelled=%u connected=%u stopping=%u faulted=%u",
                     (unsigned)kind, (unsigned long)command, (unsigned long)length, (void *)connection,
                     (unsigned long)generation, (unsigned long)waiting_generation, (void *)current_connection,
                     (unsigned long)current_generation, (unsigned long)pending_generation,
                     (unsigned long)inbound_generation, flags & 1U, (flags >> 1) & 1U,
                     (flags >> 2) & 1U, (flags >> 3) & 1U, (flags >> 4) & 1U, (flags >> 5) & 1U);
        return;
    }
    xmini_rtc_event_t event = {.kind = kind, .generation = generation,
                              .connection = connection, .length = length};
    if (kind == XMINI_RTC_COMMAND) {
        event.command = command;
        event.command_id = GET_CMD(command);
        event.command_sequence = GET_SN(command);
        event.command_response = (command & RESPONSE_BIT) != 0;
    } else {
        event.command = command;
    }
    if (length > XMINI_RTC_COMMAND_MAX_BYTES || (length && !data)) {
        portENTER_CRITICAL(&s_lock);
        s_status.command_oversize++;
        s_status.last_error = TIRTC_E_INVALID_PARAMETER;
        portEXIT_CRITICAL(&s_lock);
        event.kind = XMINI_RTC_ERROR;
        event.error = TIRTC_E_INVALID_PARAMETER;
        bool queued = emit(&event);
        command_finished(command_sequence, XMINI_RTC_CMD_INVALID_INPUT, queued);
        return;
    }
    if (length >= sizeof(event.data)) {
        size_t bytes = (size_t)length + 1;
        size_t previous = atomic_fetch_add(&s_command_bytes, bytes);
        if (previous + bytes <= XMINI_RTC_COMMAND_BUDGET)
            event.payload = malloc(bytes);
        if (!event.payload) {
            atomic_fetch_sub(&s_command_bytes, bytes);
            portENTER_CRITICAL(&s_lock);
            s_status.last_error = TIRTC_E_LACK_OF_RESOURCE;
            portEXIT_CRITICAL(&s_lock);
            event.kind = XMINI_RTC_ERROR;
            event.error = TIRTC_E_LACK_OF_RESOURCE;
            bool queued = emit(&event);
            command_finished(command_sequence, XMINI_RTC_CMD_NO_MEMORY, queued);
            return;
        }
        memcpy(event.payload, data, length);
        event.payload[length] = 0;
    } else if (length) {
        memcpy(event.data, data, length);
    }
    bool queued = emit(&event);
    if (!queued)
        xmini_rtc_event_release(&event);
    command_finished(command_sequence, XMINI_RTC_CMD_EVENT, queued);
}

const char *xmini_rtc_event_payload(const xmini_rtc_event_t *event)
{
    return event ? event->payload ? event->payload : event->data : NULL;
}

void xmini_rtc_event_release(xmini_rtc_event_t *event)
{
    if (event && event->payload) {
        size_t bytes = (size_t)event->length + 1;
        clear_secret(event->payload, bytes);
        free(event->payload);
        event->payload = NULL;
        atomic_fetch_sub(&s_command_bytes, bytes);
    }
}

static void command_callback(tirtc_conn_t connection, uint32_t command,
                             const void *data, uint32_t length)
{
    payload_callback(connection, command, data, length, XMINI_RTC_COMMAND);
}

static void message_callback(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, void *data)
{
    if (frame)
        payload_callback(connection, frame->stream_id, data, frame->length, XMINI_RTC_MESSAGE);
}

static int subscribe_callback(tirtc_conn_t connection, uint8_t stream)
{
    uint32_t generation = connection_generation(connection);
    if (stream > 15 || !active_connection(connection, generation))
        return TIRTC_E_INVALID_PARAMETER;
    if (!emit(&(xmini_rtc_event_t){.kind = XMINI_RTC_SUBSCRIBE, .generation = generation,
                                   .connection = connection, .stream = stream}))
        return TIRTC_E_BUSY;
    portENTER_CRITICAL(&s_lock);
    s_status.subscribed |= (uint16_t)(1U << stream);
    portEXIT_CRITICAL(&s_lock);
    return 0;
}

static void unsubscribe_callback(tirtc_conn_t connection, uint8_t stream)
{
    uint32_t generation = connection_generation(connection);
    if (stream > 15 || !active_connection(connection, generation))
        return;
    portENTER_CRITICAL(&s_lock);
    s_status.subscribed &= (uint16_t)~(1U << stream);
    s_status.business_permit &= (uint16_t)~(1U << stream);
    s_subscription_epoch[stream]++;
    portEXIT_CRITICAL(&s_lock);
    emit(&(xmini_rtc_event_t){.kind = XMINI_RTC_UNSUBSCRIBE, .generation = generation,
                              .connection = connection, .stream = stream});
}

static int reject_video(tirtc_conn_t connection, uint8_t stream)
{
    (void)connection;
    (void)stream;
    return TIRTC_E_INVALID_PARAMETER;
}

static const TIRTCCALLBACKS s_callbacks = {
    .on_event = event_callback,
    .on_conn_accepted = accepted_callback,
    .on_conn_error = error_callback,
    .on_disconnected = closed_callback,
    .on_audio = audio_callback,
    .on_message = message_callback,
    .on_command = command_callback,
    .on_subscribe_audio = subscribe_callback,
    .on_unsubscribe_audio = unsubscribe_callback,
    .on_subscribe_video = reject_video,
};

int xmini_rtc_init(xmini_rtc_receiver_t receiver, void *context)
{
    if (s_owner)
        return TIRTC_E_BUSY;
    s_events = xQueueCreateStatic(EVENT_COUNT, sizeof(xmini_rtc_event_t), s_event_storage,
                                  &s_event_queue_control);
    s_tx = xQueueCreateStatic(TX_COUNT, sizeof(tx_packet_t), s_tx_storage, &s_tx_queue_control);
    if (!s_events || !s_tx)
        return TIRTC_E_LACK_OF_RESOURCE;
    s_receiver = receiver;
    s_receiver_context = context;
    s_early_command_warned_generation = 0;
    s_owner = xTaskGetCurrentTaskHandle();
    return 0;
}

static int set_string(TIRTCOPTION option, const char *value)
{
    return TiRtcSetOption(option, value, strlen(value));
}

int xmini_rtc_start(const char *device_id, const char *key, const char *client_id,
                    const char *endpoint)
{
    if (!is_owner() || !device_id || !*device_id || !key || !*key ||
        !client_id || !*client_id || !endpoint || !*endpoint)
        return TIRTC_E_INVALID_PARAMETER;
    if (s_status.initialized)
        return TIRTC_E_BUSY;
    uint32_t send_limit = SEND_LIMIT;
    int connections = 1, cache = 0;
    int result = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER, &send_limit, sizeof(send_limit));
    if (!result)
        result = TiRtcSetOption(TIRTC_OPT_MAX_CONNECTIONS, &connections, sizeof(connections));
    if (!result)
        result = TiRtcSetOption(TIRTC_OPT_CONNECT_CACHE, &cache, sizeof(cache));
    if (!result)
        result = TiRtcInit();
    if (result)
        return result;
    portENTER_CRITICAL(&s_lock);
    s_status.initialized = true;
    s_status.stopping = false;
    s_sdk_stopped = false;
    s_stopped_reported = false;
    portEXIT_CRITICAL(&s_lock);
    /* This SDK may log bearer/session fields at verbose levels. Keep SDK level
     * at its existing MZ28 default; stage/errors/counters remain owned here. */
    TiRtcLogSetLevel(0);
    result = set_string(TIRTC_OPT_DEVICE_SECRET_KEY, key);
    if (!result)
        result = set_string(TIRTC_OPT_CLIENT_ID, client_id);
    bool compiled_endpoint = strcmp(endpoint, DEFAULT_ENDPOINT) == 0 ||
                             strcmp(endpoint, DEFAULT_HTTPS_ALIAS) == 0;
    if (!result && !compiled_endpoint)
        result = set_string(TIRTC_OPT_SERVICE_ENDPOINT, endpoint);
    if (!result)
        result = TiRtcStart(device_id, &s_callbacks);
    if (result) {
        TiRtcUninit();
        portENTER_CRITICAL(&s_lock);
        s_status.initialized = false;
        s_status.online = false;
        s_status.last_error = result;
        portEXIT_CRITICAL(&s_lock);
    }
    ESP_LOGI(TAG, "start SDK=%s submitted=%d PCMA=8000/1/20ms video=0", TiRtcGetVersion(), result);
    return result;
}

int xmini_rtc_arm_inbound(uint32_t generation, bool accept)
{
    if (!is_owner() || (accept && !generation))
        return TIRTC_E_INVALID_PARAMETER;
    portENTER_CRITICAL(&s_lock);
    bool busy = accept && (s_pending || s_connection || s_closing || s_retiring_generation ||
                           s_status.sdk_pending || s_status.stopping || s_status.faulted);
    if (!busy) {
        s_inbound_generation = generation;
        s_accept_inbound = accept;
    }
    portEXIT_CRITICAL(&s_lock);
    return busy ? TIRTC_E_BUSY : 0;
}

int xmini_rtc_connect(xmini_rtc_mode_t mode, uint32_t generation,
                      const char *peer, const char *token)
{
    if (!is_owner() || !generation || !peer || !token || !*peer || !*token ||
        strlen(peer) >= XMINI_RTC_PEER_CAPACITY || strlen(token) >= XMINI_RTC_TOKEN_CAPACITY ||
        (mode != XMINI_RTC_P2P && mode != XMINI_RTC_WHIP))
        return TIRTC_E_INVALID_PARAMETER;
    portENTER_CRITICAL(&s_lock);
    bool ready = s_status.online && !s_status.stopping && !s_status.faulted;
    bool busy = s_pending || s_connection || s_closing || s_rejected || s_rejected_closing ||
                s_retiring_generation || s_status.sdk_pending;
    portEXIT_CRITICAL(&s_lock);
    if (!ready)
        return TIRTC_E_NOT_INITIALIZED;
    if (busy || s_peer || s_token)
        return TIRTC_E_BUSY;
    s_peer_size = strlen(peer) + 1;
    s_token_size = strlen(token) + 1;
    s_peer = malloc(s_peer_size);
    s_token = malloc(s_token_size);
    if (!s_peer || !s_token) {
        release_inputs();
        return TIRTC_E_LACK_OF_RESOURCE;
    }
    memcpy(s_peer, peer, s_peer_size);
    memcpy(s_token, token, s_token_size);
    portENTER_CRITICAL(&s_lock);
    s_accept_inbound = false;
    s_pending = true;
    s_cancelled = false;
    s_pending_generation = generation;
    s_status.generation = generation;
    s_status.connecting = true;
    s_status.subscribed = 0;
    s_status.business_permit = 0;
    portEXIT_CRITICAL(&s_lock);
    int result = mode == XMINI_RTC_WHIP
                     ? TiRtcWhipConnect(s_peer, s_token, connected_callback, (void *)(uintptr_t)generation)
                     : TiRtcConnect(s_peer, s_token, connected_callback, (void *)(uintptr_t)generation);
    if (result) {
        portENTER_CRITICAL(&s_lock);
        s_pending = false;
        s_status.connecting = false;
        s_status.last_error = result;
        portEXIT_CRITICAL(&s_lock);
        release_inputs();
    }
    ESP_LOGI(TAG, "connect mode=%d gen=%lu submitted=%d", mode, (unsigned long)generation, result);
    return result;
}

int xmini_rtc_disconnect(uint32_t generation)
{
    if (!is_owner() || !generation)
        return TIRTC_E_INVALID_PARAMETER;
    portENTER_CRITICAL(&s_lock);
    tirtc_conn_t connection = generation == s_status.generation ? s_connection : NULL;
    if (connection) {
        s_connection = NULL;
        s_closing = connection;
        s_closing_generation = generation;
        s_status.connected = false;
        s_status.subscribed = 0;
        s_status.business_permit = 0;
        rx_clear_locked(true);
    }
    bool already_closing = s_closing && s_closing_generation == generation;
    portEXIT_CRITICAL(&s_lock);
    if (!connection)
        return already_closing ? TIRTC_E_BUSY : TIRTC_E_INVALID_HANDLE;
    xQueueReset(s_tx);
    int result = TiRtcDisconnect(connection);
    if (result) {
        portENTER_CRITICAL(&s_lock);
        s_status.last_error = result;
        s_status.faulted = true;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "disconnect gen=%lu failed=%d", (unsigned long)generation, result);
    }
    return result;
}

int xmini_rtc_cancel(uint32_t generation)
{
    if (!is_owner() || !generation)
        return TIRTC_E_INVALID_PARAMETER;
    portENTER_CRITICAL(&s_lock);
    bool pending = s_pending && s_pending_generation == generation;
    if (pending)
        s_cancelled = true;
    bool armed = s_accept_inbound && s_inbound_generation == generation;
    if (armed)
        s_accept_inbound = false;
    bool connected = s_connection && s_status.generation == generation;
    if (connected) rx_clear_locked(true);
    portEXIT_CRITICAL(&s_lock);
#ifdef TIRTC_PENDING_CLOSE_CONTRACT
    int cancelled = TiRtcCancelPending((void *)(uintptr_t)generation, 1);
    if (cancelled)
        return cancelled;
    if (!pending && !connected) {
        portENTER_CRITICAL(&s_lock);
        s_retiring_generation = generation;
        portEXIT_CRITICAL(&s_lock);
        return 0;
    }
#else
    (void)connected;
#endif
    return pending || armed ? 0 : xmini_rtc_disconnect(generation);
}

int xmini_rtc_enqueue_audio(uint32_t generation, uint8_t stream,
                            const uint8_t *pcma, size_t length, uint32_t pts)
{
    if (!pcma || length != XMINI_RTC_PCMA_BYTES || stream > 15 || !generation || !s_tx)
        return TIRTC_E_INVALID_PARAMETER;
    portENTER_CRITICAL(&s_lock);
    const char *reason = !s_connection ? "no_connection" :
        generation != s_status.generation ? "stale_generation" :
        !s_status.connected ? "not_connected" : s_status.stopping ? "stopping" :
        s_status.faulted ? "faulted" : !((s_status.subscribed | s_status.business_permit) & (1U << stream)) ?
        "not_authorized" : NULL;
    bool allowed = reason == NULL;
    bool report = !allowed && !s_enqueue_reject_reported;
    if (report) s_enqueue_reject_reported = true;
    uint32_t active_generation = s_status.generation;
    unsigned subscribed = s_status.subscribed;
    unsigned permitted = s_status.business_permit;
    uint32_t epoch = s_subscription_epoch[stream];
    portEXIT_CRITICAL(&s_lock);
    if (!allowed) {
        if (report)
            ESP_LOGW(TAG, "TX ENQUEUE rejected gen=%lu active=%lu stream=%u subscribed=0x%04x permit=0x%04x reason=%s",
                     (unsigned long)generation, (unsigned long)active_generation, stream, subscribed, permitted, reason);
        return TIRTC_E_INVALID_HANDLE;
    }
    tx_packet_t packet = {.generation = generation, .pts = pts, .stream = stream,
                          .subscription_epoch = epoch, .queued_us = esp_timer_get_time()};
    memcpy(packet.data, pcma, length);
    if (xQueueSend(s_tx, &packet, 0) == pdTRUE)
        return 0;
    portENTER_CRITICAL(&s_lock);
    s_status.tx_dropped++;
    report = !s_enqueue_full_reported;
    s_enqueue_full_reported = true;
    portEXIT_CRITICAL(&s_lock);
    if (report)
        ESP_LOGW(TAG, "TX ENQUEUE rejected gen=%lu stream=%u reason=queue_full capacity=%u",
                 (unsigned long)generation, stream, (unsigned)TX_COUNT);
    return TIRTC_E_BUSY;
}

void xmini_rtc_process(void)
{
    if (!is_owner())
        return;
#ifdef TIRTC_PENDING_CLOSE_CONTRACT
    unsigned sdk_pending = s_status.initialized ? TiRtcPendingConnections() : 0;
    portENTER_CRITICAL(&s_lock);
    s_status.sdk_pending = sdk_pending;
    uint32_t retired = !sdk_pending ? s_retiring_generation : 0;
    if (retired) s_retiring_generation = 0;
    portEXIT_CRITICAL(&s_lock);
    if (retired)
        emit(&(xmini_rtc_event_t){.kind = XMINI_RTC_RETIRED,
            .generation = retired, .retired = true});
#endif
    portENTER_CRITICAL(&s_lock);
    tirtc_conn_t rejected = s_rejected;
    s_rejected = NULL;
    if (rejected)
        s_rejected_closing = rejected;
    bool release = !s_pending && !s_connection && !s_closing && !s_rejected_closing;
    bool fault_stop = s_status.faulted && s_status.initialized && !s_status.stopping && !s_sdk_stopped;
    bool first_rx = s_first_rx && !s_rx_reported;
    if (first_rx)
        s_rx_reported = true;
    portEXIT_CRITICAL(&s_lock);
    if (rejected) {
        int result = TiRtcDisconnect(rejected);
        if (result) {
            portENTER_CRITICAL(&s_lock);
            s_status.last_error = result;
            s_status.faulted = true;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGE(TAG, "late/unexpected connection disconnect failed=%d", result);
        }
    }
    if (release)
        release_inputs();
    if (fault_stop) {
        ESP_LOGE(TAG, "stopping SDK after control fault; failure remains latched");
        xmini_rtc_stop();
    }
    if (first_rx)
        ESP_LOGI(TAG, "RX FIRST PCMA8000/1 160B");
    tx_packet_t packet;
    for (unsigned count = 0; count < TX_COUNT && xQueueReceive(s_tx, &packet, 0) == pdTRUE; ++count) {
        portENTER_CRITICAL(&s_lock);
        tirtc_conn_t connection = s_connection;
        bool allowed = connection && packet.generation == s_status.generation && s_status.connected &&
                       !s_status.stopping && !s_status.faulted &&
                       ((s_status.subscribed | s_status.business_permit) & (1U << packet.stream)) &&
                       packet.subscription_epoch == s_subscription_epoch[packet.stream];
        portEXIT_CRITICAL(&s_lock);
        if (!allowed) {
            portENTER_CRITICAL(&s_lock);
            s_status.stale_frames++;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }
        int result;
        if (esp_timer_get_time() - packet.queued_us >= 100000 ||
            TiRtcGetSendBufferUsed(connection) >= SEND_WATERMARK) {
            result = TIRTC_E_BUSY;
        } else {
            TIRTCFRAMEINFO frame = {.stream_id = packet.stream, .media = TIRTC_AUDIO_ALAW,
                                    .flags = TIRTC_AUDIOSAMPLE_8K16B1C,
                                    .ts = packet.pts, .length = sizeof(packet.data)};
            result = TiRtcSendAudioStream(connection, &frame, packet.data);
        }
        bool first_tx = false, report_error = false;
        portENTER_CRITICAL(&s_lock);
        if (result > 0) {
            s_status.tx_frames++;
            first_tx = !s_first_tx;
            s_first_tx = true;
        } else {
            s_status.tx_dropped++;
            s_status.last_error = result ? result : TIRTC_E_UNEXPECTED_RESPONSE;
            report_error = result != TIRTC_E_BUSY && !s_tx_error_reported;
            if (report_error) {
                s_tx_error_reported = true;
                s_status.connected = false;
                s_status.subscribed = 0;
                s_status.business_permit = 0;
                rx_clear_locked(true);
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if (first_tx)
            ESP_LOGI(TAG, "TX FIRST gen=%lu stream=%u PCMA8000/1 160B", (unsigned long)packet.generation,
                     packet.stream);
        if (report_error) {
            int error = result ? result : TIRTC_E_UNEXPECTED_RESPONSE;
            ESP_LOGE(TAG, "audio send failed gen=%lu error=%d", (unsigned long)packet.generation, error);
            emit(&(xmini_rtc_event_t){.kind = XMINI_RTC_ERROR, .generation = packet.generation,
                                      .connection = connection, .error = error});
        }
    }
}

bool xmini_rtc_poll(xmini_rtc_event_t *event)
{
    if (!is_owner() || !event)
        return false;
    portENTER_CRITICAL(&s_lock);
    bool overflow = s_status.event_overflow && !s_overflow_reported;
    if (overflow)
        s_overflow_reported = true;
    portEXIT_CRITICAL(&s_lock);
    if (overflow) {
        *event = (xmini_rtc_event_t){.kind = XMINI_RTC_OVERFLOW, .error = TIRTC_E_BUSY};
        return true;
    }
    if (xQueueReceive(s_events, event, 0) == pdTRUE) {
        if (event->kind == XMINI_RTC_STOPPED) {
            portENTER_CRITICAL(&s_lock);
            s_stopped_reported = true;
            portEXIT_CRITICAL(&s_lock);
        }
        return true;
    }
    /* Stop completion is a sticky fact, not a lossy queue notification. This
     * lets the owner release resources even when overflow caused that Stop. */
    portENTER_CRITICAL(&s_lock);
    bool stopped = s_sdk_stopped && !s_stopped_reported;
    if (stopped)
        s_stopped_reported = true;
    portEXIT_CRITICAL(&s_lock);
    if (stopped)
        *event = (xmini_rtc_event_t){.kind = XMINI_RTC_STOPPED};
    return stopped;
}

static tirtc_conn_t get_connection(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    tirtc_conn_t connection = generation == s_status.generation && s_status.connected &&
                                      !s_status.stopping && !s_status.faulted
                                  ? s_connection : NULL;
    portEXIT_CRITICAL(&s_lock);
    return connection;
}

int xmini_rtc_send_command(uint32_t generation, uint32_t command, const void *data, size_t length)
{
    if (!is_owner() || (length && !data) || length > UINT32_MAX)
        return TIRTC_E_INVALID_PARAMETER;
    tirtc_conn_t connection = get_connection(generation);
    int result = connection ? TiRtcSendCommand(connection, command, data, length) : TIRTC_E_INVALID_HANDLE;
    if (command == 0x2000U) {
        uint32_t at_ms = (uint32_t)(esp_timer_get_time() / 1000);
        portENTER_CRITICAL(&s_lock);
        if (s_command_tx_diagnostic.sequence != UINT32_MAX) {
            ++s_command_tx_diagnostic.sequence;
            s_command_tx_diagnostic.at_ms = at_ms;
            s_command_tx_diagnostic.generation = generation;
            s_command_tx_diagnostic.connection = (uint32_t)(uintptr_t)connection;
            s_command_tx_diagnostic.command = command;
            s_command_tx_diagnostic.length = (uint32_t)length;
            s_command_tx_diagnostic.result = result;
            s_command_tx_diagnostic.flags = connection ? 1U : 0U;
        } else {
            s_command_tx_diagnostic.flags |= 2U;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    return result;
}

static int select_audio_connection(tirtc_conn_t connection, uint32_t generation,
                                    uint8_t stream, bool selected)
{
    portENTER_CRITICAL(&s_lock);
    bool active = connection && s_connection == connection && s_status.generation == generation &&
                  s_status.connected && !s_status.stopping && !s_status.faulted;
    if (active) {
        if (selected && (!s_rx.selected || s_rx.generation != generation || s_rx.stream != stream)) {
            rx_clear_locked(false);
            s_rx.generation = generation;
            s_rx.stream = stream;
            s_rx.selected = true;
        } else if (!selected && s_rx.selected && s_rx.generation == generation && s_rx.stream == stream) {
            rx_clear_locked(true);
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return active ? 0 : TIRTC_E_INVALID_HANDLE;
}

int xmini_rtc_select_audio(uint32_t generation, uint8_t stream, bool selected)
{
    if (!is_owner() || !generation || stream > 15)
        return TIRTC_E_INVALID_PARAMETER;
    return select_audio_connection(get_connection(generation), generation, stream, selected);
}

int xmini_rtc_subscribe(uint32_t generation, uint8_t stream, bool subscribe)
{
    if (!is_owner() || stream > 15)
        return TIRTC_E_INVALID_PARAMETER;
    tirtc_conn_t connection = get_connection(generation);
    if (!connection)
        return TIRTC_E_INVALID_HANDLE;
    int result = subscribe ? TiRtcSubscribeAudio(connection, stream) : TiRtcUnsubscribeAudio(connection, stream);
    return result < 0 ? result : select_audio_connection(connection, generation, stream, subscribe);
}

int xmini_rtc_authorize_audio(uint32_t generation, uint8_t stream, bool allow)
{
    if (!is_owner() || !generation || stream > 15)
        return TIRTC_E_INVALID_PARAMETER;
    portENTER_CRITICAL(&s_lock);
    bool matching = generation == s_status.generation;
    bool active = matching && s_connection && s_status.connected &&
                  !s_status.stopping && !s_status.faulted;
    if (matching && (!allow || active)) {
        if (allow) s_status.business_permit |= (uint16_t)(1U << stream);
        else s_status.business_permit &= (uint16_t)~(1U << stream);
        s_subscription_epoch[stream]++;
    }
    portEXIT_CRITICAL(&s_lock);
    return matching && (!allow || active) ? 0 : TIRTC_E_INVALID_HANDLE;
}

int xmini_rtc_service_request(const char *path, const char *json,
                               TIRTCSERVICEREQUESTCALLBACK callback, void *context)
{
    if (!is_owner() || !path || path[0] != '/')
        return TIRTC_E_INVALID_PARAMETER;
    portENTER_CRITICAL(&s_lock);
    bool ready = s_status.online && !s_status.stopping && !s_status.faulted;
    portEXIT_CRITICAL(&s_lock);
    return ready ? TiRtcServiceRequest(path, json, NULL, callback, context) : TIRTC_E_NOT_INITIALIZED;
}

int xmini_rtc_stop(void)
{
    if (!is_owner())
        return TIRTC_E_INVALID_PARAMETER;
    portENTER_CRITICAL(&s_lock);
    bool initialized = s_status.initialized;
    bool stopping = s_status.stopping;
    if (initialized && !stopping) {
        s_status.stopping = true;
        s_accept_inbound = false;
        s_cancelled = true;
        s_status.subscribed = 0;
        s_status.business_permit = 0;
        rx_clear_locked(true);
    }
    portEXIT_CRITICAL(&s_lock);
    if (!initialized)
        return TIRTC_E_NOT_INITIALIZED;
    if (stopping)
        return TIRTC_E_BUSY;
    xQueueReset(s_tx);
    int result = TiRtcStop();
    if (result) {
        portENTER_CRITICAL(&s_lock);
        s_status.last_error = result;
        s_status.faulted = true;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "stop failed=%d; resources not declared released", result);
    }
    return result;
}

int xmini_rtc_uninit(void)
{
    if (!is_owner())
        return TIRTC_E_INVALID_PARAMETER;
    portENTER_CRITICAL(&s_lock);
    bool initialized = s_status.initialized;
    bool stopped = s_sdk_stopped;
    portEXIT_CRITICAL(&s_lock);
    if (!initialized)
        return TIRTC_E_NOT_INITIALIZED;
    if (!stopped)
        return TIRTC_E_BUSY;
    TiRtcUninit();
    release_inputs();
    xmini_rtc_event_t event;
    while (xQueueReceive(s_events, &event, 0) == pdTRUE)
        xmini_rtc_event_release(&event);
    portENTER_CRITICAL(&s_lock);
    s_status.initialized = false;
    s_status.stopping = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "uninit complete");
    return 0;
}

void xmini_rtc_status(xmini_rtc_status_t *status)
{
    if (!status)
        return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    status->closing = s_closing || s_rejected || s_rejected_closing ||
                      s_retiring_generation || s_status.sdk_pending;
    status->rx_carry_bytes = s_rx.used;
    portEXIT_CRITICAL(&s_lock);
    status->events_pending = s_events ? uxQueueMessagesWaiting(s_events) : 0;
    status->tx_pending = s_tx ? uxQueueMessagesWaiting(s_tx) : 0;
    status->command_bytes_pending = atomic_load(&s_command_bytes);
}

bool xmini_rtc_command_diagnostic(unsigned slot, xmini_rtc_command_diagnostic_summary_t *summary,
                                   xmini_rtc_command_diagnostic_t *out)
{
    portENTER_CRITICAL(&s_lock);
    if (summary)
        *summary = s_command_diagnostic_summary;
    if (out) {
        if (slot < XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS)
            *out = s_command_diagnostics[slot];
        else
            memset(out, 0, sizeof(*out));
    }
    bool valid = slot < XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS && s_command_diagnostics[slot].sequence != 0;
    portEXIT_CRITICAL(&s_lock);
    return valid;
}

void xmini_rtc_command_tx_diagnostic(xmini_rtc_command_tx_diagnostic_t *out)
{
    if (!out)
        return;
    portENTER_CRITICAL(&s_lock);
    *out = s_command_tx_diagnostic;
    portEXIT_CRITICAL(&s_lock);
}

void xmini_rtc_transport_diagnostics(xmini_rtc_transport_diagnostics_t *out)
{
    if (!out)
        return;
    portENTER_CRITICAL(&s_lock);
    *out = s_transport_diagnostics;
    portEXIT_CRITICAL(&s_lock);
}
