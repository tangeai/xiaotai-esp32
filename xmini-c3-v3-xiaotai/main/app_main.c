// SPDX-License-Identifier: MIT
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xmini_audio.h"
#include "xmini_board.h"
#include "xmini_ui.h"
#include "xmini_command.h"
#include "xmini_product.h"
#include "xmini_runtime.h"
#include "xmini_business.h"
#include "xmini_rtc.h"
#include "xmini_settings.h"
#include "xmini_audio_control.h"
#include "xmini_memtrace.h"
#include "xmini_heap_failure.h"
#include "xmini_feedback.h"
#include "wifi_provisioning.h"
#include "demo_platform.h"

static const char *TAG = "xmini_app";
// Only the main task uses this scratch: consume the status before rendering.
static union {
    xmini_business_status_t business;
    uint8_t frame[XMINI_FRAMEBUFFER_BYTES];
} s_workspace;
#define s_business s_workspace.business
#define s_frame s_workspace.frame
static uint8_t s_previous[XMINI_FRAMEBUFFER_BYTES];
static xmini_feedback_identity_t s_previous_feedback;
static bool s_display_fault;
static bool s_redraw_requested;
static unsigned s_display_frames;
static xmini_command_kind_t s_key_injection;
static uint32_t s_key_started;
static uint32_t s_input_last, s_input_max_gap, s_input_late_polls;
#define CONSOLE_REPLY_SLOTS 4U
static struct { unsigned sequence; int result, stream_error, error_number; bool connected; int64_t elapsed_us; } s_replies[CONSOLE_REPLY_SLOTS];
static unsigned s_reply_sequence;

static void submit(xmini_intent_t intent, const xmini_ui_t *ui)
{
    if (intent.kind == XMINI_INTENT_NONE) return;
    if (intent.kind == XMINI_UI_GUIDANCE) {
        int error = xmini_settings_schedule_guidance(intent.value != 0);
        if (error) ESP_LOGW(TAG, "guidance setting rejected: %d", error);
        return;
    }
    if (intent.kind == XMINI_UI_WIFI_SETUP || intent.kind == XMINI_UI_WIFI_CANCEL) {
        int error = intent.kind == XMINI_UI_WIFI_CANCEL ? xmini_runtime_wifi_cancel() : xmini_runtime_wifi_setup();
        if (error) ESP_LOGW(TAG, "Wi-Fi setup rejected: %d", error);
        return;
    }
    if (intent.kind == XMINI_UI_CONTACT_PAGE || intent.kind == XMINI_UI_PENDING_PAGE) {
        if (!xmini_runtime_ready()) return;
        int error = intent.kind == XMINI_UI_PENDING_PAGE ? xmini_business_post_pending_page((unsigned)intent.value) :
            xmini_business_post_contacts_page(ui->page == XMINI_PAGE_WECHAT ? XMINI_CONTACT_WECHAT : XMINI_CONTACT_DEVICE,
                (unsigned)intent.value);
        if (error) ESP_LOGW(TAG, "contact page rejected: %d", error);
        return;
    }
    if (intent.kind >= XMINI_UI_CONTACT_REFRESH && intent.kind <= XMINI_UI_CONTACT_REJECT) {
        if (!xmini_runtime_ready()) return;
        bool present = false;
        xmini_business_status(&s_business);
        for (unsigned i = 0; i < s_business.pending_count; ++i)
            present |= !strcmp(ui->pending_peer, s_business.pending[i]);
        if (intent.kind != XMINI_UI_CONTACT_REFRESH && !present) return;
        xmini_contact_action_t action = intent.kind == XMINI_UI_CONTACT_REFRESH ? XMINI_CONTACT_REFRESH :
            intent.kind == XMINI_UI_CONTACT_ACCEPT ? XMINI_CONTACT_ACCEPT : XMINI_CONTACT_REJECT;
        int error = action == XMINI_CONTACT_REFRESH ? xmini_business_post_pending_page(0) :
            xmini_business_post_contact(action, ui->pending_peer, NULL);
        if (error) ESP_LOGW(TAG, "contact action=%u rejected: %d", action, error);
        return;
    }
    if (intent.kind >= XMINI_UI_GROUP_REFRESH && intent.kind <= XMINI_UI_GROUP_TALK) {
        if (!xmini_runtime_ready()) return;
        static const xmini_group_action_t actions[] = {XMINI_GROUP_REFRESH, XMINI_GROUP_CREATE,
            XMINI_GROUP_JOIN, XMINI_GROUP_CONNECT, XMINI_GROUP_DISCONNECT, XMINI_GROUP_LEAVE, XMINI_GROUP_PTT_DOWN};
        xmini_group_action_t action = actions[intent.kind - XMINI_UI_GROUP_REFRESH];
        if (intent.kind == XMINI_UI_GROUP_TALK && ui->group_speaking) action = XMINI_GROUP_PTT_UP;
        int error = xmini_business_post_group(action, action == XMINI_GROUP_JOIN ? ui->join_code : NULL,
            ui->group_use_password && (action == XMINI_GROUP_JOIN || action == XMINI_GROUP_CREATE) ? ui->group_password : NULL);
        if (error) ESP_LOGW(TAG, "group action=%u rejected: %d", action, error);
        return;
    }
    xmini_business_intent_t business;
    bool business_action = true;
    const char *target = NULL;
    switch (intent.kind) {
    case XMINI_UI_AI_START: business = XMINI_INTENT_AI_START; break;
    case XMINI_UI_REFRESH_CONTACTS: business = XMINI_INTENT_REFRESH_CONTACTS; break;
    case XMINI_UI_ACCEPT: business = XMINI_INTENT_ACCEPT; break;
    case XMINI_UI_REJECT: business = XMINI_INTENT_REJECT; break;
    case XMINI_UI_HANGUP: business = XMINI_INTENT_HANGUP; break;
    case XMINI_UI_INTERRUPT: business = XMINI_INTENT_AI_INTERRUPT; break;
    case XMINI_UI_SUBMIT: business = XMINI_INTENT_AI_SUBMIT_SPEECH; break;
    case XMINI_UI_DISMISS: business = XMINI_INTENT_DISMISS_ERROR; break;
    case XMINI_UI_CALL_CONTACT: {
        business = ui->page == XMINI_PAGE_WECHAT ? XMINI_INTENT_WECHAT_CALL : XMINI_INTENT_DEVICE_CALL;
        for (unsigned i = 0; i < s_business.contacts_count; ++i) {
            if (s_business.contacts[i].wechat != (ui->page == XMINI_PAGE_WECHAT)) continue;
            if (!strcmp(ui->contact_peer, s_business.contacts[i].id)) { target = s_business.contacts[i].id; break; }
        }
        if (!target) return;
        break;
    }
    default: business_action = false; break;
    }
    if (business_action) {
        if (!xmini_runtime_ready()) return;
        int error = intent.kind >= XMINI_UI_ACCEPT && intent.kind <= XMINI_UI_DISMISS ?
            xmini_business_post_session_intent(business, ui->session_generation) :
            xmini_business_post_intent(business, target);
        if (error) ESP_LOGW(TAG, "business intent=%u rejected: %d", business, error);
        return;
    }
    xmini_audio_request_t request = {.value = intent.value};
    switch (intent.kind) {
    case XMINI_INTENT_VOLUME: request.kind = XMINI_AUDIO_VOLUME; break;
    case XMINI_INTENT_GAIN: request.kind = XMINI_AUDIO_GAIN; break;
    case XMINI_INTENT_TONE: request.kind = XMINI_AUDIO_TONE; break;
    case XMINI_INTENT_RECORD: request.kind = XMINI_AUDIO_RECORD; break;
    case XMINI_INTENT_PLAY: request.kind = XMINI_AUDIO_PLAY; break;
    case XMINI_INTENT_STOP: request.kind = XMINI_AUDIO_STOP; break;
    default: return;
    }
    if (request.kind == XMINI_AUDIO_STOP && xmini_audio_stream_busy(ui->session_generation)) {
        int error = xmini_business_post_session_intent(XMINI_INTENT_HANGUP, ui->session_generation);
        if (error) ESP_LOGW(TAG, "audio stop session rejected: %d", error);
        return;
    }
    esp_err_t err = xmini_request_audio(request);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio request=%d rejected: %s", intent.kind, esp_err_to_name(err));
    }
}

static void update_business_ui(xmini_ui_t *ui, uint32_t now)
{
    static xmini_guide_phase_t previous_guide_phase;
    xmini_feedback_identity_t feedback_identity = {s_business.owner, s_business.phase, s_business.generation};
    bool feedback_changed = xmini_feedback_identity_changed(s_previous_feedback, feedback_identity);
    int wifi_phase = xmini_runtime_wifi_phase();
    bool wifi_pending = wifi_phase != XMINI_WIFI_SETUP_IDLE;
    int wifi_error = wifi_phase == XMINI_WIFI_SETUP_ERROR_HELD || !wifi_pending ? xmini_runtime_wifi_error() : 0;
    if (ui->wifi_setup_pending != wifi_pending || ui->wifi_setup_error != wifi_error) ++ui->epoch;
    if (ui->page == XMINI_PAGE_WIFI_CONFIRM && ui->wifi_setup_pending && !wifi_pending && !wifi_error) {
        ui->page = XMINI_PAGE_SETTINGS; ui->selected = 0; ++ui->epoch;
    }
    ui->wifi_setup_pending = wifi_pending;
    ui->wifi_setup_error = wifi_error;
    xmini_settings_status_t settings = xmini_settings_status();
    if (ui->guidance_enabled != settings.guidance_enabled) ++ui->epoch;
    ui->guidance_enabled = settings.guidance_enabled;
    ui->settings_saved = settings.saved; ui->settings_pending = settings.pending; ui->settings_error = settings.error;
    ui->platform_online = s_business.platform_online;
    ui->rtc_online = s_business.rtc_online;
    ui->maintenance = s_business.maintenance;
    xmini_ui_session_identity(ui, s_business.owner, s_business.generation, s_business.peer);
    ui->session_error = s_business.error;
    ui->session_phase = s_business.phase == XMINI_BUSINESS_IDLE ? XMINI_UI_IDLE :
        s_business.phase == XMINI_BUSINESS_RINGING ? XMINI_UI_RINGING :
        s_business.phase == XMINI_BUSINESS_ACTIVE ? XMINI_UI_ACTIVE :
        s_business.phase == XMINI_BUSINESS_ENDING ? XMINI_UI_ENDING :
        s_business.phase == XMINI_BUSINESS_ERROR ? XMINI_UI_ERROR : XMINI_UI_CONNECTING;
    memcpy(ui->caption, s_business.caption, sizeof(ui->caption));
    ui->scroll_tick = now / 100;
    ui->group_assigned = s_business.group.assigned;
    ui->group_active = s_business.group.active;
    ui->group_speaking = s_business.group.speaking;
    ui->contact_page = s_business.contacts_page;
    ui->contact_pages = (s_business.contacts_filtered_total + XMINI_BUSINESS_CONTACTS - 1) / XMINI_BUSINESS_CONTACTS;
    ui->contacts_loading = s_business.contacts_loading;
    ui->contacts_error = s_business.contacts_error;
    ui->pending_page = s_business.pending_page;
    ui->pending_pages = (s_business.pending_total + 3) / 4;
    ui->pending_loading = s_business.pending_loading;
    ui->pending_error = s_business.pending_error;
    memcpy(ui->room_code, s_business.group.room_code, sizeof(ui->room_code));
    char previous_contact[65], previous_pending[65];
    memcpy(previous_contact, ui->contact_peer, sizeof(previous_contact));
    memcpy(previous_pending, ui->pending_name, sizeof(previous_pending));
    ui->contact_count = 0;
    ui->contact_name[0] = 0;
    ui->contact_peer[0] = 0;
    if (ui->page == XMINI_PAGE_WECHAT || ui->page == XMINI_PAGE_DEVICE) {
        bool matching_filter = s_business.contacts_filter == (ui->page == XMINI_PAGE_WECHAT ? XMINI_CONTACT_WECHAT : XMINI_CONTACT_DEVICE);
        if (!matching_filter) { ui->contact_page = 0; ui->contacts_loading = true; }
        for (unsigned i = 0; matching_filter && i < s_business.contacts_count; ++i) {
            if (s_business.contacts[i].wechat != (ui->page == XMINI_PAGE_WECHAT)) continue;
            if (ui->contact_count == ui->selected) {
                snprintf(ui->contact_name, sizeof(ui->contact_name), "%s",
                    s_business.contacts[i].name[0] ? s_business.contacts[i].name : s_business.contacts[i].id);
                memcpy(ui->contact_peer, s_business.contacts[i].id, sizeof(ui->contact_peer));
            }
            ++ui->contact_count;
        }
        unsigned last = ui->contact_count + (ui->page == XMINI_PAGE_DEVICE ? 5 : 2);
        if (ui->selected > last) { ui->selected = 0; ++ui->epoch; }
    }
    ui->pending_count = s_business.pending_count;
    bool pending_present = false;
    for (unsigned i = 0; i < s_business.pending_count; ++i)
        pending_present |= !strcmp(ui->pending_peer, s_business.pending[i]);
    if (ui->page == XMINI_PAGE_CONTACT_DECISION && ui->pending_present != pending_present) ++ui->epoch;
    ui->pending_present = pending_present;
    ui->pending_name[0] = 0;
    if (ui->page == XMINI_PAGE_PENDING) {
        if (ui->selected > ui->pending_count + 2) { ui->selected = 0; ++ui->epoch; }
        if (ui->selected < ui->pending_count)
            memcpy(ui->pending_name, s_business.pending[ui->selected], sizeof(ui->pending_name));
    }
    if (strcmp(previous_contact, ui->contact_peer) || strcmp(previous_pending, ui->pending_name)) ++ui->epoch;
    xmini_guide_phase_t guide_phase = xmini_product_guide().phase;
    bool guide_recovered = guide_phase == XMINI_GUIDE_ONLINE && previous_guide_phase != XMINI_GUIDE_ONLINE;
    previous_guide_phase = guide_phase;
    if (feedback_changed || (guide_recovered && s_business.phase != XMINI_BUSINESS_IDLE)) {
        if (s_business.phase != XMINI_BUSINESS_IDLE && xmini_product_guide().phase == XMINI_GUIDE_ONLINE) {
            ui->page = s_business.owner == XMINI_BUSINESS_GROUP ? XMINI_PAGE_GROUP : XMINI_PAGE_SESSION;
            ui->selected = 0;
            ++ui->epoch;
        } else if (ui->page == XMINI_PAGE_SESSION) {
            ui->page = XMINI_PAGE_HOME;
            ui->selected = 0;
            ++ui->epoch;
        }
    }
    if (feedback_changed) {
        int feedback = xmini_session_feedback(s_previous_feedback.owner, s_business.owner, s_business.phase);
        if (s_business.platform_online) xmini_product_feedback(feedback);
    }
    s_previous_feedback = feedback_identity;
}

static void show_status(void)
{
    xmini_audio_status_t s = xmini_audio_status();
    ESP_LOGI(TAG, "board=Xmini-C3-V3 display_fault=%d audio_ready=%d busy=%d error=%d recorded=%d",
             s_display_fault, s.ready, s.busy, s.error, s.recording_ready);
    ESP_LOGI(TAG, "PCMA 8k mono 20ms: frames=%u peak=%lu volume=%d gain_db=%d",
             s.frames, (unsigned long)s.peak, s.volume, s.gain_db);
    ESP_LOGI(TAG, "internal: free=%u largest=%u minimum=%u; PCMA product runtime",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void print_ui(const xmini_ui_t *ui, const xmini_key_t *key)
{
    printf("+UI: page=%u selected=%u epoch=%lu key_active=%d gpio_down=%d display_fault=%d display_frames=%u\n",
           ui->page, ui->selected, (unsigned long)ui->epoch,
           s_key_injection != XMINI_CMD_INVALID || key->raw_down || key->stable_down || key->clicks,
           xmini_board_button_down(),
           s_display_fault, s_display_frames);
    printf("+INPUT: max_gap_ms=%lu late_polls=%lu raw=%d stable=%d clicks=%u\n",
           (unsigned long)s_input_max_gap, (unsigned long)s_input_late_polls,
           key->raw_down, key->stable_down, key->clicks);
}

static bool injected_button_down(uint32_t now)
{
    uint32_t elapsed = now - s_key_started;
    if (s_key_injection == XMINI_CMD_INVALID) return false;
    if (elapsed >= (s_key_injection == XMINI_CMD_HOLD ? 1600U : 700U)) {
        s_key_injection = XMINI_CMD_INVALID;
        return false;
    }
    if (s_key_injection == XMINI_CMD_HOLD) return elapsed < 1400U;
    return elapsed < 100U || (s_key_injection == XMINI_CMD_DOUBLE && elapsed >= 180U && elapsed < 280U);
}

static void print_transport(const char *label, const xmini_rtc_transport_snapshot_t *snapshot)
{
    const tgtrp_internal_diag_t *t = &snapshot->transport;
    printf("+TG_%s: seq=%lu generation=%lu connection=%" PRIxPTR " identity=%" PRIxPTR
           " now=%lu frame=%lu minimum=%lu frames=%lu segments=%lu bytes=%lu auxiliary_frames=%lu"
           " auxiliary_bytes=%lu pacer_packets=%lu pacer_bytes=%lu rx_allocation_failures=%lu"
           " first_failed_frame=%lu version=%u stage=%u kind=%u stream=%u\n",
           label, (unsigned long)snapshot->capture_sequence, (unsigned long)snapshot->generation,
           snapshot->connection, t->identity, (unsigned long)t->now_ms, (unsigned long)t->frame_id,
           (unsigned long)t->minimum_frame_id, (unsigned long)t->frames, (unsigned long)t->segments,
           (unsigned long)t->payload_bytes, (unsigned long)t->auxiliary_frames, (unsigned long)t->auxiliary_bytes,
           (unsigned long)t->pacer_packets, (unsigned long)t->pacer_bytes,
           (unsigned long)t->rx_allocation_failures, (unsigned long)t->first_failed_frame_id,
           t->version, t->stage, t->kind, t->stream_id);
}

static void print_command_trace_rx(unsigned slot)
{
    xmini_rtc_command_diagnostic_t row;
    bool available = xmini_rtc_command_diagnostic(slot, NULL, &row);
    unsigned disposition = (row.flags & XMINI_RTC_CMD_DISPOSITION_MASK) >> XMINI_RTC_CMD_DISPOSITION_SHIFT;
    printf("+CMD_RX: slot=%u available=%d seq=%lu at_ms=%lu conn=%08lx word=%08lx length=%lu"
           " observed_gen=%lu current_gen=%lu pending_gen=%lu inbound_gen=%lu flags=%08lx"
           " disposition=%u queue_known=%d queued=%d\n",
           slot, available, (unsigned long)row.sequence, (unsigned long)row.at_ms,
           (unsigned long)row.connection, (unsigned long)row.command, (unsigned long)row.length,
           (unsigned long)row.observed_generation, (unsigned long)row.current_generation,
           (unsigned long)row.pending_generation, (unsigned long)row.inbound_generation,
           (unsigned long)row.flags, disposition, disposition != XMINI_RTC_CMD_ENTRY,
           (row.flags & XMINI_RTC_CMD_QUEUED) != 0);
}

static void print_command_trace_tx(void)
{
    xmini_rtc_command_tx_diagnostic_t row;
    xmini_rtc_command_tx_diagnostic(&row);
    printf("+CMD_TX: available=%d seq=%lu at_ms=%lu generation=%lu conn=%08lx word=%08lx"
           " length=%lu result=%ld sdk_called=%d saturated=%d\n",
           row.sequence != 0, (unsigned long)row.sequence, (unsigned long)row.at_ms,
           (unsigned long)row.generation, (unsigned long)row.connection, (unsigned long)row.command,
           (unsigned long)row.length, (long)row.result, (row.flags & 1U) != 0, (row.flags & 2U) != 0);
}

static void print_command_trace_dispatch(void)
{
    xmini_business_command_diagnostics_t row;
    xmini_business_command_diagnostics(&row);
    printf("+CMD_DISPATCH: available=%d seq=%lu generation=%lu conn=%08lx word=%08lx disposition=%lu error=%ld\n",
           row.sequence != 0, (unsigned long)row.sequence, (unsigned long)row.generation,
           (unsigned long)row.connection32, (unsigned long)row.word,
           (unsigned long)row.disposition, (long)row.error);
}

static void print_command_trace(void)
{
    xmini_rtc_command_diagnostic_summary_t before, after;
    xmini_rtc_command_diagnostic(XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS, &before, NULL);
    printf("+CMDTRACE: captures=%lu overwritten=%lu slots=%u saturated=%d snapshot=per_slot\n",
           (unsigned long)before.captures, (unsigned long)before.overwritten,
           XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS, before.captures == UINT32_MAX || before.overwritten == UINT32_MAX);
    for (unsigned slot = 0; slot < XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS; ++slot) print_command_trace_rx(slot);
    print_command_trace_tx();
    print_command_trace_dispatch();
    xmini_rtc_command_diagnostic(XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS, &after, NULL);
    printf("+CMDTRACE_END: captures=%lu overwritten=%lu counts_stable=%d saturated=%d\n",
           (unsigned long)after.captures, (unsigned long)after.overwritten,
           before.captures == after.captures && before.overwritten == after.overwritten,
           after.captures == UINT32_MAX || after.overwritten == UINT32_MAX);
}

static void command(const char *line, const xmini_ui_t *ui, const xmini_key_t *key)
{
    if (!*line) return;
    xmini_command_t parsed = xmini_command_parse(line);
    xmini_audio_request_t request = {.value = parsed.value};
    esp_err_t result = ESP_OK;
    bool needs_runtime = (parsed.kind >= XMINI_CMD_AI_START && parsed.kind <= XMINI_CMD_DISMISS) ||
        (parsed.kind >= XMINI_CMD_GROUP_REFRESH && parsed.kind <= XMINI_CMD_GROUP_UP) ||
        parsed.kind == XMINI_CMD_MEDIA_TONE ||
        (parsed.kind >= XMINI_CMD_PENDING && parsed.kind <= XMINI_CMD_PENDING_PAGE);
    if (needs_runtime && !xmini_runtime_ready()) {
        result = ESP_ERR_INVALID_STATE;
        goto command_done;
    }
    switch (parsed.kind) {
    case XMINI_CMD_AT: break;
    case XMINI_CMD_CMDTRACE: print_command_trace(); break;
    case XMINI_CMD_NETWORK: {
        esp_netif_ip_info_t ip = {0};
        wifi_ap_record_t ap = {0};
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        result = netif ? esp_netif_get_ip_info(netif, &ip) : ESP_ERR_INVALID_STATE;
        if (result == ESP_OK) result = esp_wifi_sta_get_ap_info(&ap);
        if (result == ESP_OK) printf("+NETWORK: ip=" IPSTR " gateway=" IPSTR " rssi=%d channel=%u\n",
            IP2STR(&ip.ip), IP2STR(&ip.gw), ap.rssi, ap.primary);
        break;
    }
    case XMINI_CMD_MEMTRACE: {
        xmini_heap_failure_status_t heap_failure;
        xmini_heap_failure_status(&heap_failure);
        printf("+HEAPFAIL: installed=%d registration_error=%d failures=%lu first=%d at_us=%lld bytes=%u caps=%08lx task_handle=%08lx function=%s task=%s function_truncated=%d task_truncated=%d in_isr=%d cache_enabled=%d task_available=%d\n",
            heap_failure.installed, heap_failure.registration_error, (unsigned long)heap_failure.failures,
            heap_failure.has_first, (long long)heap_failure.first_at_us, (unsigned)heap_failure.requested_size,
            (unsigned long)heap_failure.caps, (unsigned long)heap_failure.task_handle,
            heap_failure.function, heap_failure.task, heap_failure.function_truncated, heap_failure.task_truncated,
            heap_failure.in_isr, heap_failure.cache_enabled, heap_failure.task_available);
        xmini_memtrace_status_t memory;
        xmini_memtrace_status(&memory);
        printf("+MEMTRACE: compiled=%d running=%d seq=%lu allocations=%lu frees=%lu failures=%lu lost=%lu live=%u peak=%u slots=%u peak_slots=%u\n",
            memory.compiled, memory.running, (unsigned long)memory.sequence, (unsigned long)memory.allocations,
            (unsigned long)memory.frees, (unsigned long)memory.failures, (unsigned long)memory.lost,
            (unsigned)memory.live_bytes, (unsigned)memory.peak_bytes, memory.live_slots, memory.peak_slots);
        for (unsigned i = 0; i < XMINI_MEMTRACE_SLOTS; ++i) {
            xmini_memtrace_record_t record;
            if (xmini_memtrace_record(i, &record))
                printf("+MEMLIVE: slot=%u ptr=%08lx bytes=%u caller=%08lx\n", i, (unsigned long)record.address,
                    (unsigned)record.size, (unsigned long)record.caller);
        }
        for (unsigned i = 0; i < XMINI_MEMTRACE_FAILURES; ++i) {
            xmini_memtrace_failure_t failure;
            if (xmini_memtrace_failure(i, &failure))
                printf("+MEMFAIL: slot=%u seq=%lu op=%u bytes=%u caller=%08lx free=%u largest=%u\n", i,
                    (unsigned long)failure.sequence, failure.operation, (unsigned)failure.size,
                    (unsigned long)failure.caller, (unsigned)failure.free_bytes, (unsigned)failure.largest_block);
        }
        break;
    }
    case XMINI_CMD_CONSOLE: {
        uint32_t count;
        uint32_t pattern = usb_serial_jtag_get_last_disconnect_sof(&count);
        printf("+SOF: disconnects=%lu last_pattern=%08lx\n", (unsigned long)count, (unsigned long)pattern);
        usb_serial_jtag_sof_diagnostic_t timing;
        if (usb_serial_jtag_get_sof_diagnostic(&timing)) {
            printf("+SOF_TIME: at_us=%llu disconnects=%lu since_sof_us=%lu missing_ticks=%lu\n",
                (unsigned long long)timing.timestamp_us, (unsigned long)timing.disconnect_count,
                (unsigned long)timing.since_sof_us, (unsigned long)timing.missing_ticks);
            printf("+SOF_DELTAS:");
            for (unsigned i = 0; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i)
                printf(" %lu", (unsigned long)timing.delta_us[i]);
            putchar('\n');
            printf("+SOF_FRAMES: valid=%u", timing.frame_index_valid);
            for (unsigned i = 0; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i)
                printf(" %u", timing.frame_index[i]);
            putchar('\n');
            printf("+SOF_FLAGS:");
            for (unsigned i = 0; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i)
                printf(" %u", timing.sample_flags[i]);
            putchar('\n');
        }
        for (unsigned i = 0; i < CONSOLE_REPLY_SLOTS; ++i) {
            unsigned index = (s_reply_sequence + i) % CONSOLE_REPLY_SLOTS;
            if (!s_replies[index].sequence) continue;
            printf("+CONSOLE: seq=%u ret=%d ferror=%d errno=%d connected=%d elapsed_us=%lld\n",
                s_replies[index].sequence, s_replies[index].result, s_replies[index].stream_error,
                s_replies[index].error_number, s_replies[index].connected, (long long)s_replies[index].elapsed_us);
        }
        break;
    }
    case XMINI_CMD_HELP:
        puts("AT | AT+VERSION | AT+STATUS | AT+UI | AT+SCREEN | AT+DISPLAY");
        puts("AT+KEY CLICK|DOUBLE|HOLD | AT+AUDIO TONE|RECORD|PLAY|STOP");
        puts("AT+VOL 0..100 | AT+MIC 0..36; OK=accepted, query status for completion");
        puts("AT+GUIDANCE [0|1] (automatic voice only; text, alarms and explicit replay stay on)");
        puts("AT+PRODUCT | AT+BIND REPLAY | AT+RETRY | AT+GUIDE 0..12 [six-digit-code for 5]");
        puts("AT+WIFI <ssid-hex>,<password-hex> (active portal only; verifies before saving)");
        puts("AT+WIFI SETUP (idle-only restart into provisioning; binding and old credentials retained)");
        puts("AT+WIFI CANCEL (cancel pending setup before restart)");
        puts("AT+RTC | AT+CMDTRACE | AT+CONTACTS | AT+AI START|STOP|INTERRUPT|SUBMIT");
        puts("AT+CONTACTS ALL|DEV|WX [zero-based-page] | AT+PENDING [zero-based-page]");
        puts("AT+PENDING | AT+CONTACT REQUEST|ACCEPT|REJECT|DELETE <peer-id>");
        puts("AT+CONTACT REMARK <peer-id> <UTF8-hex or - to clear>");
        puts("AT+CALL DEVICE <id> | AT+CALL WX <openid> | AT+CALL ACCEPT|REJECT|HANGUP|DISMISS");
        puts("AT+GROUP REFRESH|CREATE [four-digit-password]|CONNECT|DISCONNECT|LEAVE|PTT_DOWN|PTT_UP");
        puts("AT+GROUP JOIN <six-digit-room> [four-digit-password]");
        puts("AT+MEDIA TONE (500ms stream diagnostic) | AT+AUDIO DUMP (idle PCMA snapshot)");
        break;
    case XMINI_CMD_VERSION: {
        const esp_app_desc_t *app = esp_app_get_description();
        uint8_t mac[6];
        result = esp_efuse_mac_get_default(mac);
        if (result == ESP_OK) {
            printf("+VERSION: board=Xmini-C3-V3 version=%s idf=%s mac=" MACSTR "\n",
                   app->version, app->idf_ver, MAC2STR(mac));
            char sha[65];
            for (unsigned i = 0; i < sizeof(app->app_elf_sha256); ++i)
                snprintf(sha + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
            printf("+BUILD: elf_sha256=%s video=0 opus=0 network=1 rtc=1\n", sha);
        }
        break;
    }
    case XMINI_CMD_STATUS: {
        xmini_audio_status_t audio = xmini_audio_status();
        print_ui(ui, key);
        printf("+AUDIO: ready=%d busy=%d error=%d recorded=%d frames=%u peak=%lu volume=%d gain=%d prompt=%d expected=%u stopped=%d owner=%u device_open=%d\n",
               audio.ready, audio.busy, audio.error, audio.recording_ready, audio.frames,
               (unsigned long)audio.peak, audio.volume, audio.gain_db, audio.prompt, audio.expected_frames, audio.stopped, audio.owner, audio.device_open);
        printf("+HEAP: free=%u largest=%u minimum=%u integrity=%d main_hwm=%u uptime_ms=%llu\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               heap_caps_check_integrity_all(true), (unsigned)uxTaskGetStackHighWaterMark(NULL),
               (unsigned long long)(esp_timer_get_time() / 1000));
        printf("+HEAPCAP: internal_caps=%08x internal_free=%u internal_largest=%u internal_minimum=%u dma_caps=%08x dma_free=%u dma_largest=%u dma_minimum=%u rtc_caps=%08x rtc_free=%u rtc_largest=%u rtc_minimum=%u unit=bytes\n",
               (unsigned)(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA),
               (unsigned)MALLOC_CAP_RTCRAM,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_RTCRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_RTCRAM));
        printf("+STREAM: generation=%lu tx=%lu rx=%lu tx_dropped=%lu rx_dropped=%lu underruns=%lu injected=%lu source=%u control_hwm=%u\n",
               (unsigned long)audio.generation, (unsigned long)audio.tx_frames, (unsigned long)audio.rx_frames,
               (unsigned long)audio.tx_dropped, (unsigned long)audio.rx_dropped, (unsigned long)audio.underruns,
               (unsigned long)audio.injected_frames, audio.recording_source,
               xmini_runtime_stack_watermark());
        xmini_settings_status_t settings = xmini_settings_status();
        printf("+SETTINGS: loaded=%d saved=%d pending=%d error=%d revision=%u quiesced=%d attempt_pending=%d guidance=%d\n",
               settings.loaded, settings.saved, settings.pending, settings.error, settings.revision,
               settings.quiesced, settings.attempt_pending, settings.guidance_enabled);
        unsigned audio_hwm = 0, platform_hwm = 0;
        bool audio_available = xmini_audio_stack_watermark(&audio_hwm);
        bool platform_available = platform_stack_watermark(&platform_hwm);
        printf("+TASKS: audio_available=%d audio_hwm=%u platform_available=%d platform_hwm=%u unit=bytes\n",
               audio_available, audio_hwm, platform_available, platform_hwm);
        break;
    }
    case XMINI_CMD_PRODUCT: {
        xmini_guide_t guide = xmini_product_guide();
        platform_identity_t identity;
        platform_identity_snapshot(&identity);
        printf("+PRODUCT: phase=%u error=%u code=%s seconds=%u utc=%lld\n", guide.phase, guide.error,
               guide.code[0] ? guide.code : "-",
               xmini_guide_seconds_left(&guide, (uint32_t)(esp_timer_get_time() / 1000)), (long long)time(NULL));
        xmini_rtc_status_t rtc;
        xmini_rtc_status(&rtc);
        printf("+DEVICE: id=%s platform_online=%d rtc_ready=%d\n", identity.device_id[0] ? identity.device_id : "-", platform_is_online(), rtc.online);
        platform_http_exclusive_status_t http = platform_http_exclusive_status();
        printf("+WIFI_SETUP: phase=%d command_error=%d http_held=%d http_ready=%d http_in_flight=%d http_pending=%u\n",
            xmini_runtime_wifi_phase(), xmini_runtime_wifi_error(), http.held, http.ready, http.in_flight, http.pending);
        memset(&identity, 0, sizeof(identity));
        break;
    }
    case XMINI_CMD_GUIDANCE_SET:
        result = xmini_settings_schedule_guidance(parsed.value != 0);
        break;
    case XMINI_CMD_GUIDANCE: {
        xmini_settings_status_t settings = xmini_settings_status();
        printf("+GUIDANCE: enabled=%d loaded=%d saved=%d pending=%d error=%d quiesced=%d attempt_pending=%d\n",
            settings.guidance_enabled, settings.loaded, settings.saved, settings.pending,
            settings.error, settings.quiesced, settings.attempt_pending);
        break;
    }
    case XMINI_CMD_GUIDE: {
        xmini_audio_request_t diagnostic = {.kind = XMINI_AUDIO_PROMPT, .value = parsed.value, .owner = XMINI_AUDIO_DIAGNOSTIC};
        memcpy(diagnostic.code, parsed.code, sizeof(diagnostic.code));
        result = xmini_audio_request(diagnostic);
        if (result == ESP_OK) puts("+GUIDE: diagnostic=1 binding_state_unchanged=1");
        break;
    }
    case XMINI_CMD_RTC: {
        xmini_rtc_status_t rtc;
        xmini_rtc_transport_diagnostics_t transport;
        xmini_rtc_status(&rtc);
        xmini_rtc_transport_diagnostics(&transport);
        xmini_business_status(&s_business);
        printf("+RTC: initialized=%d online=%d connecting=%d connected=%d closing=%d stopping=%d faulted=%d generation=%lu subscribed=%04x permit=%04x error=%d sdk_pending=%u\n",
               rtc.initialized, rtc.online, rtc.connecting, rtc.connected, rtc.closing, rtc.stopping, rtc.faulted,
               (unsigned long)rtc.generation, rtc.subscribed, rtc.business_permit, rtc.last_error, rtc.sdk_pending);
        printf("+MEDIA: tx=%lu rx=%lu tx_dropped=%lu rx_dropped=%lu stale=%lu oversize=%lu overflow=%lu format_errors=%lu events=%u pending=%u rx_carry_bytes=%lu rx_partial_dropped=%lu rx_bytes_dropped=%lu rx_unselected_frames=%lu\n",
               (unsigned long)rtc.tx_frames, (unsigned long)rtc.rx_frames, (unsigned long)rtc.tx_dropped,
               (unsigned long)rtc.rx_dropped, (unsigned long)rtc.stale_frames, (unsigned long)rtc.command_oversize,
               (unsigned long)rtc.event_overflow, (unsigned long)rtc.format_errors, rtc.events_pending, rtc.tx_pending,
               (unsigned long)rtc.rx_carry_bytes, (unsigned long)rtc.rx_partial_dropped,
               (unsigned long)rtc.rx_bytes_dropped, (unsigned long)rtc.rx_unselected_frames);
        printf("+RX_AUDIO: valid=%u generation=%lu chunks=%lu last_bytes=%lu media=%u flags=%u stream=%u\n",
               rtc.rx_input_valid, (unsigned long)rtc.rx_input_generation,
               (unsigned long)rtc.rx_input_chunks, (unsigned long)rtc.rx_input_length,
               rtc.rx_input_media, rtc.rx_input_flags, rtc.rx_input_stream);
        printf("+SESSION: owner=%u phase=%u generation=%lu media_active=%d error=%d peer=%s room=%s profile_ready=%d maintenance=%d reason=%s\n",
               s_business.owner, s_business.phase, (unsigned long)s_business.generation, s_business.media_active,
               s_business.error, s_business.peer, s_business.room, s_business.profile_ready, s_business.maintenance, s_business.reason);
        printf("+CAPTION: %s\n", s_business.caption);
        printf("+GROUP: phase=%u known=%d assigned=%d active=%d speaking=%d room_code=%s room_id=%s version=%lu snapshot_ready=%d participants=%u error=%d\n",
               s_business.group.phase, s_business.group.assignment_known, s_business.group.assigned,
               s_business.group.active, s_business.group.speaking, s_business.group.room_code, s_business.group.room_id,
               (unsigned long)s_business.group.assignment_version, s_business.group.snapshot_ready,
               s_business.group.participant_count, s_business.group.error);
        printf("+TG: captures=%lu\n", (unsigned long)rtc.transport_capture_sequence);
        if (transport.tx.capture_sequence) print_transport("TX", &transport.tx);
        if (transport.rx.capture_sequence) print_transport("RX", &transport.rx);
        if (transport.queue.capture_sequence) print_transport("QUEUE", &transport.queue);
        break;
    }
    case XMINI_CMD_CONTACTS:
        xmini_business_status(&s_business);
        printf("+CONTACTS: cached=%u total=%u filter=%u page=%u filtered_total=%u loading=%d error=%d\n",
            s_business.contacts_count, s_business.contacts_total, s_business.contacts_filter,
            s_business.contacts_page, s_business.contacts_filtered_total, s_business.contacts_loading, s_business.contacts_error);
        for (unsigned i = 0; i < s_business.contacts_count; ++i)
            printf("+CONTACT: index=%u wechat=%d id=%s name=%s\n", i, s_business.contacts[i].wechat,
                   s_business.contacts[i].id, s_business.contacts[i].name);
        break;
    case XMINI_CMD_CONTACTS_PAGE:
        result = xmini_business_post_contacts_page((xmini_contact_filter_t)parsed.value, parsed.page);
        break;
    case XMINI_CMD_PENDING_PAGE: result = xmini_business_post_pending_page(parsed.page); break;
    case XMINI_CMD_PENDING:
        xmini_business_status(&s_business);
        printf("+PENDING: cached=%u total=%u error=%d revision=%u page=%u loading=%d\n", s_business.pending_count,
            s_business.pending_total, s_business.pending_error, s_business.revision, s_business.pending_page, s_business.pending_loading);
        for (unsigned i = 0; i < s_business.pending_count; ++i)
            printf("+PENDING: index=%u id=%s\n", i, s_business.pending[i]);
        break;
    case XMINI_CMD_CONTACT_REQUEST:
    case XMINI_CMD_CONTACT_ACCEPT:
    case XMINI_CMD_CONTACT_REJECT:
    case XMINI_CMD_CONTACT_DELETE:
    case XMINI_CMD_CONTACT_REMARK: {
        static const xmini_contact_action_t actions[] = {XMINI_CONTACT_REQUEST, XMINI_CONTACT_ACCEPT,
            XMINI_CONTACT_REJECT, XMINI_CONTACT_DELETE, XMINI_CONTACT_REMARK};
        result = xmini_business_post_contact(actions[parsed.kind - XMINI_CMD_CONTACT_REQUEST], parsed.target,
            parsed.kind == XMINI_CMD_CONTACT_REMARK ? parsed.remark : NULL);
        break;
    }
    case XMINI_CMD_WIFI_SETUP: result = xmini_runtime_wifi_setup(); break;
    case XMINI_CMD_WIFI_CANCEL: result = xmini_runtime_wifi_cancel(); break;
    case XMINI_CMD_MEDIA_TONE:
        xmini_business_status(&s_business);
        result = s_business.media_active ? xmini_audio_stream_tone(s_business.generation) : ESP_ERR_INVALID_STATE;
        break;
    case XMINI_CMD_AUDIO_DUMP: {
        xmini_audio_status_t audio = xmini_audio_status();
        if (audio.busy || !audio.recording_ready) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        uint8_t *snapshot = malloc(8000);
        result = snapshot ? xmini_audio_recording_copy(snapshot, 8000) : ESP_ERR_NO_MEM;
        if (result == ESP_OK) {
            printf("+PCMA: bytes=8000 rate=8000 channels=1 source=%u\n", xmini_audio_status().recording_source);
            static const char digits[] = "0123456789abcdef";
            for (unsigned offset = 0; offset < 8000; offset += 32) {
                char hex[65];
                for (unsigned i = 0; i < 32; ++i) {
                    hex[2 * i] = digits[snapshot[offset + i] >> 4];
                    hex[2 * i + 1] = digits[snapshot[offset + i] & 15];
                }
                hex[64] = 0;
                printf("+PCMA: offset=%u hex=%s\n", offset, hex);
            }
        }
        free(snapshot);
        break;
    }
    case XMINI_CMD_AI_START: result = xmini_business_post_intent(XMINI_INTENT_AI_START, NULL); break;
    case XMINI_CMD_AI_INTERRUPT: result = xmini_business_post_intent(XMINI_INTENT_AI_INTERRUPT, NULL); break;
    case XMINI_CMD_AI_SUBMIT: result = xmini_business_post_intent(XMINI_INTENT_AI_SUBMIT_SPEECH, NULL); break;
    case XMINI_CMD_DEVICE_CALL: result = xmini_business_post_intent(XMINI_INTENT_DEVICE_CALL, parsed.target); break;
    case XMINI_CMD_WECHAT_CALL: result = xmini_business_post_intent(XMINI_INTENT_WECHAT_CALL, parsed.target); break;
    case XMINI_CMD_ACCEPT: result = xmini_business_post_intent(XMINI_INTENT_ACCEPT, NULL); break;
    case XMINI_CMD_REJECT: result = xmini_business_post_intent(XMINI_INTENT_REJECT, NULL); break;
    case XMINI_CMD_HANGUP: result = xmini_business_post_intent(XMINI_INTENT_HANGUP, NULL); break;
    case XMINI_CMD_DISMISS: result = xmini_business_post_intent(XMINI_INTENT_DISMISS_ERROR, NULL); break;
    case XMINI_CMD_USB_PROBE:
#if CONFIG_XMINI_USB_TIMING_PROBE
        result = usb_serial_jtag_test_delay_tick_once();
#else
        result = ESP_ERR_NOT_SUPPORTED;
#endif
        break;
    case XMINI_CMD_GROUP_REFRESH:
    case XMINI_CMD_GROUP_CREATE:
    case XMINI_CMD_GROUP_JOIN:
    case XMINI_CMD_GROUP_CONNECT:
    case XMINI_CMD_GROUP_DISCONNECT:
    case XMINI_CMD_GROUP_LEAVE:
    case XMINI_CMD_GROUP_DOWN:
    case XMINI_CMD_GROUP_UP: {
        static const xmini_group_action_t actions[] = {XMINI_GROUP_REFRESH, XMINI_GROUP_CREATE, XMINI_GROUP_JOIN,
            XMINI_GROUP_CONNECT, XMINI_GROUP_DISCONNECT, XMINI_GROUP_LEAVE, XMINI_GROUP_PTT_DOWN, XMINI_GROUP_PTT_UP};
        result = xmini_business_post_group(actions[parsed.kind - XMINI_CMD_GROUP_REFRESH],
            parsed.kind == XMINI_CMD_GROUP_JOIN ? parsed.code : NULL,
            (parsed.kind == XMINI_CMD_GROUP_JOIN || parsed.kind == XMINI_CMD_GROUP_CREATE) && parsed.password[0] ? parsed.password : NULL);
        break;
    }
    case XMINI_CMD_WIFI:
        result = wifi_provisioning_submit_credentials(parsed.ssid, parsed.password);
        memset(parsed.password, 0, sizeof(parsed.password));
        if (result == ESP_OK) puts("+WIFI: accepted=1 verified=0 saved=0");
        break;
    case XMINI_CMD_BIND_REPLAY:
        result = xmini_product_guide().phase == XMINI_GUIDE_BINDING ? xmini_product_confirm() : ESP_ERR_INVALID_STATE;
        break;
    case XMINI_CMD_RETRY: {
        xmini_guide_t guide = xmini_product_guide();
        bool expired = guide.phase == XMINI_GUIDE_BINDING &&
            !xmini_guide_seconds_left(&guide, (uint32_t)(esp_timer_get_time() / 1000));
        result = guide.phase == XMINI_GUIDE_ERROR || guide.phase == XMINI_GUIDE_OFFLINE || expired ?
                 xmini_product_confirm() : ESP_ERR_INVALID_STATE;
        break;
    }
    case XMINI_CMD_UI: print_ui(ui, key); break;
    case XMINI_CMD_SCREEN:
        if (s_display_fault || !s_display_frames) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        puts("+SCREEN: width=128 height=64 format=SSD1306_PAGE bytes=1024 source=last_sent");
        for (unsigned offset = 0; offset < sizeof(s_previous); offset += 32) {
            char hex[65];
            static const char digits[] = "0123456789abcdef";
            for (unsigned i = 0; i < 32; ++i) {
                hex[2 * i] = digits[s_previous[offset + i] >> 4];
                hex[2 * i + 1] = digits[s_previous[offset + i] & 15];
            }
            hex[64] = '\0';
            printf("+SCREEN: offset=%u hex=%s\n", offset, hex);
        }
        break;
    case XMINI_CMD_DISPLAY: s_redraw_requested = true; break;
    case XMINI_CMD_TONE: request.kind = XMINI_AUDIO_TONE; break;
    case XMINI_CMD_RECORD: request.kind = XMINI_AUDIO_RECORD; break;
    case XMINI_CMD_PLAY: request.kind = XMINI_AUDIO_PLAY; break;
    case XMINI_CMD_STOP: request.kind = XMINI_AUDIO_STOP; break;
    case XMINI_CMD_VOLUME: request.kind = XMINI_AUDIO_VOLUME; break;
    case XMINI_CMD_GAIN: request.kind = XMINI_AUDIO_GAIN; break;
    case XMINI_CMD_CLICK:
    case XMINI_CMD_DOUBLE:
    case XMINI_CMD_HOLD:
        if (s_key_injection || key->raw_down || key->stable_down || key->clicks || xmini_board_button_down()) {
            result = ESP_ERR_INVALID_STATE;
        } else {
            // Inject timed levels through the same debounce/gesture path as GPIO9.
            s_key_injection = parsed.kind;
            s_key_started = (uint32_t)(esp_timer_get_time() / 1000);
        }
        break;
    default: result = ESP_ERR_INVALID_ARG; break;
    }
    if (request.kind) {
        xmini_business_status(&s_business);
        result = request.kind == XMINI_AUDIO_STOP && xmini_audio_stream_busy(s_business.generation) ?
            xmini_business_post_session_intent(XMINI_INTENT_HANGUP, s_business.generation) : xmini_request_audio(request);
    }
command_done:
    ;
    unsigned reply = s_reply_sequence++ % CONSOLE_REPLY_SLOTS;
    int64_t started = esp_timer_get_time();
    s_replies[reply].sequence = s_reply_sequence;
    s_replies[reply].connected = usb_serial_jtag_is_connected();
    errno = 0;
    s_replies[reply].result = result == ESP_OK ? puts("OK") : printf("ERROR: %s\n", esp_err_to_name(result));
    s_replies[reply].error_number = errno;
    s_replies[reply].stream_error = ferror(stdout);
    s_replies[reply].elapsed_us = esp_timer_get_time() - started;
}

static void console_poll(const xmini_ui_t *ui, const xmini_key_t *key)
{
    static char line[768];
    static size_t length;
    static bool overflow;
    for (unsigned budget = 0; budget < 16; ++budget) {
        int ch = getchar();
        if (ch == EOF) {
            clearerr(stdin);
            return;
        }
        if (ch == '\r' || ch == '\n') {
            line[length] = '\0';
            if (overflow) puts("ERROR: LINE_TOO_LONG");
            else command(line, ui, key);
            memset(line, 0, sizeof(line));
            length = 0;
            overflow = false;
        } else if (!overflow) {
            if (ch == '\b' || ch == 127) {
                if (length) --length;
            } else if (ch == '\t' || (ch >= 32 && ch < 127)) {
                size_t limit = length >= 8 && !strncmp(line, "AT+WIFI ", 8) ? 256 :
                    length >= 11 && !strncmp(line, "AT+CONTACT ", 11) ? sizeof(line) : 96;
                if (length + 1 < limit) line[length++] = (char)ch;
                else overflow = true;
            }
        }
    }
}

void app_main(void)
{
    esp_err_t heap_hook_error = xmini_heap_failure_init();
    xmini_memtrace_start();
    if (heap_hook_error != ESP_OK)
        ESP_LOGE(TAG, "global heap failure observer registration failed: %s", esp_err_to_name(heap_hook_error));
    ESP_LOGI(TAG, "Xmini-C3-V3 PCMA product; no video, no Opus");
    esp_err_t err = xmini_board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %s; check board identity/wiring before continuing",
                 esp_err_to_name(err));
        return;
    }
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    xmini_key_t key = {0};
    int volume = 40, gain_db = 24;
    err = xmini_settings_load(&volume, &gain_db);
    if (err != ESP_OK) ESP_LOGE(TAG, "settings load: %s; storage retained", esp_err_to_name(err));
    xmini_audio_initial_settings(volume, gain_db);
    err = xmini_audio_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(err));
    // IDF 5.5.5 nonblocking VFS reads require the driver's RX ring buffer.
    usb_serial_jtag_driver_config_t console_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    err = usb_serial_jtag_driver_install(&console_config);
    bool console_ready = err == ESP_OK;
    if (console_ready) {
        usb_serial_jtag_vfs_use_driver();
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
        setvbuf(stdin, NULL, _IONBF, 0);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        console_ready = flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0;
    }
    if (!console_ready) ESP_LOGE(TAG, "nonblocking console unavailable; BOOT/OLED remain active");
    err = xmini_runtime_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "runtime init: %s", esp_err_to_name(err));
    err = xmini_product_init(err);
    if (err != ESP_OK) ESP_LOGE(TAG, "product init: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "BOOT: click=next double=confirm hold=back; no destructive key actions");
    show_status();
    uint32_t last_render = 0;
    bool first_frame = true;
    // TLS runs at priority 4; input scanning must remain schedulable during crypto.
    vTaskPrioritySet(NULL, 5);
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        xmini_business_status(&s_business);
        xmini_feedback_identity_t feedback_identity = {s_business.owner, s_business.phase, s_business.generation};
        if (xmini_feedback_identity_changed(s_previous_feedback, feedback_identity) ||
            !s_business.platform_online ||
            (s_business.phase != XMINI_BUSINESS_RINGING &&
             s_business.phase != XMINI_BUSINESS_OUTGOING &&
             s_business.phase != XMINI_BUSINESS_IDLE &&
             s_business.phase != XMINI_BUSINESS_ERROR))
            xmini_product_feedback(-1);
        xmini_product_poll(&ui);
        update_business_ui(&ui, now);
        xmini_audio_status_t audio = xmini_audio_status();
        ui.audio_ready = audio.ready;
        ui.audio_busy = audio.busy;
        ui.recording_ready = audio.recording_ready;
        ui.audio_error = audio.error;
        ui.frames = audio.frames;
        ui.peak = audio.peak;
        ui.volume = audio.volume;
        ui.gain_db = audio.gain_db;
        now = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_input_last) {
            uint32_t gap = now - s_input_last;
            if (gap > s_input_max_gap) s_input_max_gap = gap;
            if (gap > 40) ++s_input_late_polls;
        }
        s_input_last = now;
        bool synthetic_down = injected_button_down(now);
        xmini_key_event_t event = xmini_key_poll(&key, synthetic_down || xmini_board_button_down(), now, ui.epoch);
        bool guide_confirm = ui.page == XMINI_PAGE_GUIDE && event == XMINI_KEY_CONFIRM;
        submit(xmini_ui_key(&ui, event), &ui);
        if (guide_confirm) {
            esp_err_t accepted = xmini_product_confirm();
            if (accepted != ESP_OK) ESP_LOGW(TAG, "guide action rejected: %s", esp_err_to_name(accepted));
        }
        if (event != XMINI_KEY_NONE) {
            ESP_LOGI(TAG, "key source=%s event=%u page=%u selected=%u",
                     s_key_injection ? "AT" : "GPIO9", event, ui.page, ui.selected);
        }
        if (console_ready) console_poll(&ui, &key);
        if (first_frame || event != XMINI_KEY_NONE || s_redraw_requested || now - last_render >= 100) {
            if (ui.page == XMINI_PAGE_GUIDE) {
                xmini_guide_t guide = xmini_product_guide();
                xmini_guide_render(&guide, now, s_frame);
            } else xmini_ui_render(&ui, s_frame);
            if (s_redraw_requested || (!s_display_fault &&
                (first_frame || memcmp(s_frame, s_previous, sizeof(s_frame)) != 0))) {
                err = xmini_board_display(s_frame);
                s_display_fault = err != ESP_OK;
                if (err == ESP_OK) {
                    memcpy(s_previous, s_frame, sizeof(s_frame));
                    ++s_display_frames;
                }
                else ESP_LOGE(TAG, "OLED transfer failed: %s; redraw suspended, serial display retries once",
                              esp_err_to_name(err));
            }
            s_redraw_requested = false;
            first_frame = false;
            last_render = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
