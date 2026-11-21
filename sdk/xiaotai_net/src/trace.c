// SPDX-License-Identifier: MIT
#include "trace_internal.h"

#if XIAOTAI_NET_PATH_TRACE
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

#define TRACE_FIELDS(F) \
    F(net_wan) F(net_lan) F(net_wan_bytes) F(socket_wan) F(socket_lan) F(socket_wan_bytes) \
    F(audio) F(video) F(tp_audio) F(tp_video) \
    F(net_gap_ms) F(socket_gap_ms) F(audio_gap_ms) F(video_gap_ms) \
    F(rtt_ms) F(nack) F(retx) F(send_loss) F(receive_loss) \
    F(probe) F(probe_gaps) F(probe_duplicate) F(probe_last)

#define ENUM(field) field,
enum { TRACE_FIELDS(ENUM) TRACE_COLUMNS };
#undef ENUM
#define CSV(field) "," #field
static const char s_columns[] = "+NET-TRACE: columns=second" TRACE_FIELDS(CSV);
#undef CSV
_Static_assert(TRACE_COLUMNS == 23, "trace wire layout");
_Static_assert(sizeof(s_columns) <= NET_TRACE_LINE_BYTES, "trace line buffer");
_Static_assert(XIAOTAI_NET_PATH_TRACE_SECONDS > 0 && XIAOTAI_NET_PATH_TRACE_SECONDS <= UINT32_MAX / 1000U,
               "trace duration must fit the framework timer");

typedef uint32_t trace_row_t[TRACE_COLUMNS];

typedef enum { PATH_NET, PATH_SOCKET, PATH_AUDIO, PATH_VIDEO, PATH_COUNT } trace_path_t;

static xiaotai_critical_t s_lock;
static bool s_lock_ready;
static atomic_uint s_recording;
static atomic_bool s_exporting;
static unsigned s_generation;
static trace_row_t *s_rows;
static uint64_t s_start_us;
static uint32_t s_used;
static uint32_t s_timer;
static uint32_t s_stop_notice_timer;
static bool s_stop_notice;
static unsigned s_stop_retries;
static uint64_t s_last_ms[PATH_COUNT];
static bool s_path_seen[PATH_COUNT];
static bool s_probe_seen;
static uint32_t s_probe_highest;
static uint32_t s_output_line;

static uint64_t elapsed_us(void)
{
    return (uint64_t)xiaotai_time_us() - s_start_us;
}

/* Caller holds s_lock; return NULL if this observation crossed a capture or its deadline. */
static uint32_t *current_row(unsigned ticket, uint64_t *ms)
{
    if (ticket != atomic_load_explicit(&s_recording, memory_order_relaxed)) return NULL;
    const uint64_t elapsed = elapsed_us();
    const uint64_t second = elapsed / 1000000U;
    if (second >= XIAOTAI_NET_PATH_TRACE_SECONDS) return NULL;
    if (s_used <= second) s_used = (uint32_t)second + 1U;
    *ms = elapsed / 1000U;
    return s_rows[second];
}

static void max_into(uint32_t *value, uint32_t candidate)
{
    if (*value < candidate) *value = candidate;
}

static void note_gap(trace_path_t path, uint64_t now_ms, uint32_t *value)
{
    if (s_path_seen[path]) max_into(value, (uint32_t)(now_ms - s_last_ms[path]));
    s_last_ms[path] = now_ms;
    s_path_seen[path] = true;
}

static xiaotai_err_t report_state(bool recording)
{
    const xiaotai_args_t args = {.value = recording};
    return xiaotai_event_emit(XIAOTAI_EVENT_NET_TRACE, &args, NULL, 0);
}

static void retry_stop_notice(void *ctx)
{
    (void)ctx;
    s_stop_notice_timer = 0;
    if (!s_stop_notice) return;
    s_stop_notice = report_state(false) != XIAOTAI_OK;
    /* The owner cannot wait for room in its own event queue. If retries run
     * out, the next START must deliver STOP before it can arm a new capture. */
    if (s_stop_notice && s_stop_retries) {
        --s_stop_retries;
        (void)xiaotai_timer_start(10, retry_stop_notice, NULL, &s_stop_notice_timer);
    }
}

static void finish_capture(void *ctx)
{
    (void)ctx;
    s_timer = 0;
    (void)net_trace_stop();
}

static bool busy(void)
{
    return xiaotai_activity_current(NULL) != 0 || xiaotai_quiesced();
}

xiaotai_err_t net_trace_start(void)
{
    if (busy() || atomic_load(&s_exporting) || atomic_load(&s_recording)) return XIAOTAI_ERR_BUSY;
    if (s_stop_notice) {
        if (s_stop_notice_timer) return XIAOTAI_ERR_BUSY;
        retry_stop_notice(NULL);
        if (s_stop_notice) return XIAOTAI_ERR_BUSY;
    }

    trace_row_t *rows = s_rows;
    if (!rows) rows = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_NET_PATH_TRACE_SECONDS, sizeof(*rows));
    if (!rows) return XIAOTAI_ERR_NO_MEM;
    const uint64_t start = (uint64_t)xiaotai_time_us();
    xiaotai_err_t error = xiaotai_timer_start(XIAOTAI_NET_PATH_TRACE_SECONDS * 1000U,
                                             finish_capture, NULL, &s_timer);
    if (error == XIAOTAI_OK) error = report_state(true);
    if (error != XIAOTAI_OK) {
        if (s_timer) xiaotai_timer_cancel(s_timer);
        s_timer = 0;
        if (rows != s_rows) xiaotai_mem_free(rows);
        return error;
    }

    if (!s_lock_ready) {
        xiaotai_critical_init(&s_lock);
        s_lock_ready = true;
    }
    if (++s_generation == 0) ++s_generation;
    if (rows == s_rows) memset(rows, 0, XIAOTAI_NET_PATH_TRACE_SECONDS * sizeof(*rows));
    xiaotai_critical_enter(&s_lock);
    s_rows = rows;
    s_start_us = start;
    s_used = 0;
    memset(s_path_seen, 0, sizeof(s_path_seen));
    s_probe_seen = false;
    atomic_store(&s_recording, s_generation);
    xiaotai_critical_exit(&s_lock);
    return XIAOTAI_OK;
}

xiaotai_err_t net_trace_stop(void)
{
    if (!atomic_load(&s_recording)) return XIAOTAI_OK;
    xiaotai_critical_enter(&s_lock);
    atomic_store(&s_recording, 0);
    const uint64_t elapsed = elapsed_us();
    const uint64_t seconds = elapsed / 1000000U + (elapsed % 1000000U != 0);
    const uint32_t bins = seconds > XIAOTAI_NET_PATH_TRACE_SECONDS ? XIAOTAI_NET_PATH_TRACE_SECONDS :
                          (uint32_t)seconds;
    if (bins > s_used) s_used = bins;
    xiaotai_critical_exit(&s_lock);
    if (s_timer) xiaotai_timer_cancel(s_timer);
    s_timer = 0;
    s_stop_notice = true;
    s_stop_retries = 3;
    retry_stop_notice(NULL);
    return XIAOTAI_OK;
}

bool xiaotai_net_trace_active(void)
{
    const unsigned ticket = atomic_load(&s_recording);
    if (!ticket) return false;
    xiaotai_critical_enter(&s_lock);
    const bool active = ticket == atomic_load_explicit(&s_recording, memory_order_relaxed) &&
                        elapsed_us() < (uint64_t)XIAOTAI_NET_PATH_TRACE_SECONDS * 1000000U;
    xiaotai_critical_exit(&s_lock);
    return active;
}

bool xiaotai_net_trace_transport_active(void)
{
    return XIAOTAI_NET_PATH_TRACE_TRANSPORT && xiaotai_net_trace_active();
}

void xiaotai_net_trace_media(bool is_video)
{
    const unsigned ticket = atomic_load(&s_recording);
    if (!ticket) return;
    xiaotai_critical_enter(&s_lock);
    uint64_t ms;
    uint32_t *row = current_row(ticket, &ms);
    if (row) {
        row[is_video ? video : audio]++;
        note_gap(is_video ? PATH_VIDEO : PATH_AUDIO, ms, &row[is_video ? video_gap_ms : audio_gap_ms]);
    }
    xiaotai_critical_exit(&s_lock);
}

void xiaotai_net_trace_transport(const xiaotai_net_trace_transport_t *sample)
{
    if (!XIAOTAI_NET_PATH_TRACE_TRANSPORT || !sample) return;
    const unsigned ticket = atomic_load(&s_recording);
    if (!ticket) return;
    xiaotai_critical_enter(&s_lock);
    uint64_t ms;
    uint32_t *row = current_row(ticket, &ms);
    if (row) {
        if (sample->media == 1) row[tp_audio]++;
        if (sample->media == 2) row[tp_video]++;
        max_into(&row[rtt_ms], sample->rtt_ms);
        max_into(&row[nack], sample->nack);
        max_into(&row[retx], sample->retransmitted);
        max_into(&row[send_loss], sample->send_loss);
        max_into(&row[receive_loss], sample->receive_loss);
    }
    xiaotai_critical_exit(&s_lock);
}

static uint16_t be16(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

void net_trace_frame(const void *ethernet, size_t size)
{
    const unsigned ticket = atomic_load(&s_recording);
    const uint8_t *frame = ethernet;
    if (!ticket || !frame || size < 14 + 20 || be16(frame + 12) != 0x0800) return;
    const uint8_t *ip = frame + 14;
    const size_t header = (ip[0] & 0x0FU) * 4U;
    const size_t ip_size = be16(ip + 2);
    if ((ip[0] >> 4) != 4 || header < 20 || ip_size < header + 8 || ip_size > size - 14 ||
        ip[9] != 17 || (be16(ip + 6) & 0x3fffU)) return;
    const uint8_t *udp = ip + header;
    const size_t udp_size = be16(udp + 4);
    if (udp_size < 8 || udp_size > ip_size - header) return;

    const bool lan = net_diag_private_ipv4(ip + 12);
    /* Offsets 12..15 are relative to the UDP datagram, including its header. */
    const bool is_probe = be16(udp + 2) == 49162 && udp_size >= 16 && memcmp(udp + 8, "XTLN", 4) == 0;
    const uint32_t sequence = is_probe ? net_diag_be32(udp + 12) : 0;
    xiaotai_critical_enter(&s_lock);
    uint64_t ms;
    uint32_t *row = current_row(ticket, &ms);
    if (row) {
        row[lan ? net_lan : net_wan]++;
        if (!lan) {
            row[net_wan_bytes] += (uint32_t)ip_size;
            note_gap(PATH_NET, ms, &row[net_gap_ms]);
        }
        if (is_probe) {
            row[probe]++;
            row[probe_last] = sequence;
            const uint32_t delta = sequence - s_probe_highest;
            if (s_probe_seen && (!delta || delta >= UINT32_C(0x80000000))) {
                row[probe_duplicate]++;
            } else {
                if (s_probe_seen) row[probe_gaps] += delta - 1;
                s_probe_highest = sequence;
                s_probe_seen = true;
            }
        }
    }
    xiaotai_critical_exit(&s_lock);
}

void net_trace_socket(const uint8_t from[4], size_t received)
{
    const unsigned ticket = atomic_load(&s_recording);
    if (!ticket || !from || received == 0) return;
    const bool lan = net_diag_private_ipv4(from);
    xiaotai_critical_enter(&s_lock);
    uint64_t ms;
    uint32_t *row = current_row(ticket, &ms);
    if (row) {
        row[lan ? socket_lan : socket_wan]++;
        if (!lan) {
            row[socket_wan_bytes] += (uint32_t)received;
            note_gap(PATH_SOCKET, ms, &row[socket_gap_ms]);
        }
    }
    xiaotai_critical_exit(&s_lock);
}

xiaotai_err_t net_trace_export_begin(void)
{
    if (busy() || xiaotai_net_trace_active()) return XIAOTAI_ERR_BUSY;
    if (atomic_exchange(&s_exporting, true)) return XIAOTAI_ERR_BUSY;
    (void)net_trace_stop();
    s_output_line = 0;
    return XIAOTAI_OK;
}

bool net_trace_export_next(char line[NET_TRACE_LINE_BYTES])
{
    if (!atomic_load(&s_exporting)) return false;
    const uint32_t ordinal = s_output_line++;
    if (ordinal == 0) {
        (void)snprintf(line, NET_TRACE_LINE_BYTES, "+NET-TRACE: BEGIN,start_ms=%" PRIu64 ",bins=%" PRIu32
                       ",bytes=%zu,transport=%u", s_start_us / 1000U, s_used,
                       sizeof(trace_row_t) * XIAOTAI_NET_PATH_TRACE_SECONDS, (unsigned)XIAOTAI_NET_PATH_TRACE_TRANSPORT);
    } else if (ordinal == 1) {
        memcpy(line, s_columns, sizeof(s_columns));
    } else if (ordinal - 2U < s_used) {
        const uint32_t index = ordinal - 2U;
        size_t offset = (size_t)snprintf(line, NET_TRACE_LINE_BYTES, "+NET-TRACE: %" PRIu32, index);
        for (size_t i = 0; i < TRACE_COLUMNS; ++i) {
            offset += (size_t)snprintf(line + offset, NET_TRACE_LINE_BYTES - offset, ",%" PRIu32, s_rows[index][i]);
        }
    } else if (ordinal - 2U == s_used) {
        memcpy(line, "+NET-TRACE: END", sizeof("+NET-TRACE: END"));
    } else {
        return false;
    }
    return true;
}

void net_trace_export_end(void)
{
    atomic_store(&s_exporting, false);
}

#else
bool xiaotai_net_trace_active(void) { return false; }
bool xiaotai_net_trace_transport_active(void) { return false; }
void xiaotai_net_trace_media(bool is_video) { (void)is_video; }
void xiaotai_net_trace_transport(const xiaotai_net_trace_transport_t *sample) { (void)sample; }
#endif
