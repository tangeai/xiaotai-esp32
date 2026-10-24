#include "xmini_business.h"

#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define PLATFORM_SLOTS 4
#define PLATFORM_BYTES 8192
#define PLATFORM_BUDGET (12U * 1024U)
#define INTENT_SLOTS 8
#define AI_COMMAND 0x2100U
#define CONNECT_COMMAND 0x2000U
#define HANGUP_COMMAND 0x2001U

typedef struct {
    platform_event_t event;
    char *json;
    size_t bytes;
} platform_item_t;
typedef struct {
    xmini_business_intent_t intent;
    bool session_bound;
    uint32_t expected_generation;
    char target[65];
    bool group;
    xmini_group_action_t group_action;
    char room_code[7], password[65];
    bool contact;
    xmini_contact_action_t contact_action;
    char *value;
    unsigned page;
    xmini_contact_filter_t filter;
    uint8_t page_kind;
} intent_item_t;

static const char *TAG = "xmini_business";
static QueueHandle_t s_platform_queue, s_intent_queue;
static StaticQueue_t s_platform_queue_control, s_intent_queue_control;
static uint8_t s_platform_storage[PLATFORM_SLOTS * sizeof(platform_item_t)];
static uint8_t s_intent_storage[INTENT_SLOTS * sizeof(intent_item_t)];
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static xmini_business_status_t s_state, s_snapshot;
static xmini_business_command_diagnostics_t s_command_diagnostics;
_Static_assert(sizeof(s_command_diagnostics) == 24, "Command diagnostics must stay within 24 bytes");
static xmini_business_callbacks_t s_callbacks;
static atomic_size_t s_platform_bytes;
static atomic_uint s_dropped, s_audio_generation, s_audio_stream, s_audio_rx_stream;
static atomic_bool s_audio_enabled;
static atomic_bool s_group_audio, s_group_transmit, s_ptt_wanted;
static atomic_bool s_maintenance;
static TaskHandle_t s_owner_task;
static uint32_t s_next_generation, s_idle_generation;
static int64_t s_deadline, s_ending_deadline;
static bool s_action_pending, s_cleanup_pending, s_ending;
static bool s_cleanup_failed;
static bool s_release_deferred, s_reconcile_deferred;
static xmini_business_phase_t s_release_previous;
static bool s_reconcile_release;
static char *s_cleanup_room;
static platform_operation_t s_cleanup_operation;
static uint32_t s_cleanup_generation;
static unsigned s_cleanup_epoch;
static bool s_release_not_found;
static int s_release_error;
static uint32_t s_room_released_generation;
static unsigned s_room_released_epoch;
static bool s_rtc_session, s_disconnect_requested, s_stop_requested, s_media_requested;
static bool s_wx_room_joined, s_wx_hangup_deferred;
static bool s_profile_pending, s_contacts_pending, s_contacts_dirty, s_platform_unbound;
static bool s_pending_pending, s_contact_mutating;
static bool s_profile_deferred, s_pending_deferred;
static unsigned s_contacts_selection, s_contacts_requested_selection;
static unsigned s_pending_selection, s_pending_requested_selection;
static bool s_outgoing, s_peer_answered, s_shutdown_sdk, s_session_started, s_ai_request_sent;
static int s_final_error;
static platform_operation_t s_action_operation;
static char s_device_id[65], s_role[65], s_rpc_id[40], s_session_id[97];
static char *s_wx_room_json;
static bool s_wx_reject_deferred, s_wx_reject_attempted;
static unsigned s_wx_reject_reason;
static char s_wx_call_id[65], s_wx_target_app[65];
static char s_closed_wx_calls[4][65], s_closed_wx_rooms[4][129];
static unsigned s_closed_wx_index;
static int s_caption_type = -1, s_caption_utterance = -1;
static int64_t now_ms(void);
static void phase(xmini_business_phase_t value, int error, const char *reason);
static void activate_media(void);
static void begin_end(int error, const char *reason, bool notify_server);
static void begin_end_notifications(int error, const char *reason, bool notify_server, bool notify_peer);

static int group_request(platform_operation_t operation, uint32_t ticket, const char *json, void *context)
{
    (void)context;
    return platform_request(operation, ticket, json);
}
static int group_connect(uint32_t generation, const char *peer, const char *token, void *context)
{
    (void)context;
    if (s_state.owner != XMINI_BUSINESS_GROUP || generation != s_state.generation || s_ending)
        return ESP_ERR_INVALID_STATE;
    int error = xmini_rtc_connect(XMINI_RTC_WHIP, generation, peer, token);
    if (!error) { s_rtc_session = true; s_deadline = now_ms() + 20000; phase(XMINI_BUSINESS_CONNECTING, 0, ""); }
    return error;
}
static int group_signal(uint32_t generation, uint32_t command, const char *json, void *context)
{
    (void)context;
    int result = xmini_rtc_send_command(generation, command, json, strlen(json));
    return result > 0 ? 0 : result ? result : ESP_ERR_INVALID_RESPONSE;
}
static int group_profile(uint32_t generation, uint8_t tx_stream, uint8_t rx_stream, void *context)
{
    (void)context;
    if (s_state.owner != XMINI_BUSINESS_GROUP || generation != s_state.generation ||
        tx_stream != 1 || rx_stream != 1 || s_ending)
        return ESP_ERR_INVALID_STATE;
    int error = xmini_rtc_subscribe(generation, rx_stream, true);
    if (!error) { s_state.tx_stream = tx_stream; activate_media(); }
    return error;
}
static void group_finish(uint32_t generation, int error, void *context)
{
    (void)context;
    if (generation == s_state.generation && s_state.owner == XMINI_BUSINESS_GROUP)
        begin_end(error, "group connection ended", false);
}
static uint32_t group_random(void *context) { (void)context; return esp_random(); }

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void clear_memory(void *data, size_t length)
{
    volatile unsigned char *bytes = data;
    while (length--)
        *bytes++ = 0;
}

static void release_json(char **json)
{
    if (*json) {
        clear_memory(*json, strlen(*json));
        free(*json);
        *json = NULL;
    }
}

static void publish(void)
{
    s_state.platform_dropped = atomic_load(&s_dropped);
    s_state.revision++;
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = s_state;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void phase(xmini_business_phase_t value, int error, const char *reason)
{
    s_state.phase = value;
    s_state.error = error;
    if (reason)
        snprintf(s_state.reason, sizeof(s_state.reason), "%s", reason);
    ESP_LOGI(TAG, "phase=%d owner=%d gen=%lu error=%d", value, s_state.owner,
             (unsigned long)s_state.generation, error);
    publish();
}

static void metadata_result(int error)
{
    bool retained = s_ending || s_state.phase == XMINI_BUSINESS_ERROR;
    if (error)
        ESP_LOGW(TAG, "metadata failed=%d owner=%d phase=%d shared_error=%d retained=%d",
                 error, s_state.owner, s_state.phase, s_state.error, retained);
    if (!retained)
        s_state.error = error;
}

static uint32_t new_generation(void)
{
    if (++s_next_generation == 0)
        ++s_next_generation;
    return s_next_generation;
}

static const char *string_field(const cJSON *object, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

static bool copy_field(const cJSON *object, const char *name, char *output, size_t size, bool required)
{
    const char *value = string_field(object, name);
    size_t length = strlen(value);
    if ((required && !length) || length >= size)
        return false;
    memcpy(output, value, length + 1);
    return true;
}

static bool copy_utf8(char *destination, size_t capacity, const char *source)
{
    size_t used = 0;
    while (*source && used + 1 < capacity) {
        unsigned char first = (unsigned char)*source;
        size_t bytes = first < 0x80 ? 1 : first >= 0xC2 && first <= 0xDF ? 2 :
                       first >= 0xE0 && first <= 0xEF ? 3 : first >= 0xF0 && first <= 0xF4 ? 4 : 0;
        if (!bytes || used + bytes >= capacity)
            break;
        bool valid = true;
        for (size_t i = 1; i < bytes; ++i)
            if (!source[i] || ((unsigned char)source[i] & 0xC0) != 0x80) { valid = false; break; }
        if (!valid)
            break;
        memcpy(destination + used, source, bytes);
        used += bytes;
        source += bytes;
    }
    destination[used] = 0;
    return *source != 0;
}

static bool response_ok(const cJSON *root)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    return cJSON_IsNumber(code) && (code->valueint == 0 || code->valueint == 200);
}

static int response_error(const cJSON *root, int transport)
{
    if (transport)
        return transport;
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    return cJSON_IsNumber(code) && code->valueint ? code->valueint : ESP_ERR_INVALID_RESPONSE;
}

static int request(platform_operation_t operation, const cJSON *body, bool action)
{
    char *json = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body && !json)
        return ESP_ERR_NO_MEM;
    bool cleanup = action && (operation == PLATFORM_DEVICE_CALL_CANCEL ||
        operation == PLATFORM_DEVICE_CALL_REJECT || operation == PLATFORM_DEVICE_CALL_HANGUP ||
        operation == PLATFORM_DEVICE_CALL_ROOM);
    unsigned accepted_epoch = 0;
    int error = cleanup ? platform_request_epoch(operation, s_state.generation, json, &accepted_epoch) :
        platform_request(operation, action ? s_state.generation : 0, json);
    release_json(&json);
    if (!error && action) {
        s_action_pending = true;
        s_action_operation = operation;
        if (cleanup) {
            s_cleanup_generation = s_state.generation;
            s_cleanup_epoch = accepted_epoch;
            s_cleanup_operation = operation;
        }
    }
    return error;
}

static cJSON *room_body(const char *reason)
{
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "room_id", s_state.room) ||
        (reason && !cJSON_AddStringToObject(body, "reason", reason))) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

static bool metadata_idle(void)
{
    if (s_state.maintenance || !s_state.platform_online ||
        s_state.owner != XMINI_BUSINESS_NONE || s_state.phase != XMINI_BUSINESS_IDLE || s_ending)
        return false;
    /* SDK retirement can outlive the visible session state. */
    xmini_rtc_status_t rtc;
    xmini_rtc_status(&rtc);
    return rtc.initialized && rtc.online && !rtc.connecting && !rtc.connected &&
        !rtc.closing && !rtc.stopping && !rtc.sdk_pending && !rtc.faulted;
}

static void refresh_contacts(void)
{
    if (!metadata_idle() || !s_state.profile_ready || s_profile_pending ||
        s_profile_deferred || s_contacts_pending) {
        s_contacts_dirty = true;
        return;
    }
    s_contacts_dirty = false;
    int error = request(PLATFORM_DEVICE_CONTACTS, NULL, false);
    if (!error) {
        s_contacts_pending = true;
        s_contacts_requested_selection = s_contacts_selection;
        s_state.contacts_loading = true;
        s_state.contacts_error = 0;
        s_contacts_dirty = false;
    } else {
        s_state.contacts_loading = false;
        s_state.contacts_count = s_state.contacts_total = s_state.contacts_filtered_total = 0;
        clear_memory(s_state.contacts, sizeof(s_state.contacts));
        metadata_result(error);
        s_state.contacts_error = error;
    }
    publish();
}

static void refresh_pending(void)
{
    if (!metadata_idle() || !s_state.profile_ready || s_profile_pending || s_profile_deferred ||
        s_contacts_pending || s_contacts_dirty || s_pending_pending) {
        s_pending_deferred = true;
        return;
    }
    s_pending_deferred = false;
    int error = request(PLATFORM_DEVICE_CONTACTS_PENDING, NULL, false);
    if (!error) {
        s_pending_pending = true;
        s_pending_requested_selection = s_pending_selection;
        s_state.pending_loading = true;
        s_state.pending_error = 0;
    } else {
        s_state.pending_loading = false;
        s_state.pending_count = s_state.pending_total = 0;
        clear_memory(s_state.pending, sizeof(s_state.pending));
        metadata_result(error);
        s_state.pending_error = error;
    }
    publish();
}

static void request_profile(void)
{
    if (!metadata_idle()) {
        s_profile_deferred = true;
        return;
    }
    if (s_profile_pending) return;
    s_profile_deferred = false;
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *body = cJSON_Parse(
        "{\"hardware\":{\"chip_model\":\"ESP32-C3\",\"board_model\":\"Xmini-C3-V3\"},"
        "\"profiles\":{\"stream\":{\"up_audio_streamid\":10,\"down_audio_streamid\":11,"
        "\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],\"up_video_mt\":[],"
        "\"down_video_mt\":[],\"audio_rate\":8000,\"audio_channels\":1,\"no_video\":true},"
        "\"call\":{\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],"
        "\"up_video_mt\":[],\"down_video_mt\":[],\"audio_rate\":8000,"
        "\"audio_channels\":1,\"no_video\":true},"
        "\"voip\":{\"screen_width\":128,\"screen_height\":64,\"audio_rate\":8000,"
        "\"audio_channels\":1,\"down_audio_mt\":\"alaw\",\"up_video_mt\":\"none\","
        "\"down_video_mt\":\"none\",\"no_video\":true,\"calling_timeout_sec\":30}}}");
    if (!body || !cJSON_AddStringToObject(body, "firmware_version", app ? app->version : "unknown")) {
        cJSON_Delete(body);
        metadata_result(ESP_ERR_NO_MEM);
        publish();
        return;
    }
    int error = request(PLATFORM_PROFILE, body, false);
    cJSON_Delete(body);
    s_profile_pending = error == 0;
    if (error) {
        metadata_result(error);
        publish();
    }
}

static void arm_idle_listener(void)
{
    if (!metadata_idle() || !s_state.rtc_online || !s_state.profile_ready || s_platform_unbound ||
        s_profile_pending || s_profile_deferred || s_contacts_pending || s_contacts_dirty ||
        s_pending_pending || s_pending_deferred)
        return;
    s_idle_generation = new_generation();
    int error = xmini_rtc_arm_inbound(s_idle_generation, true);
    if (error)
        ESP_LOGW(TAG, "idle listener admission failed=%d", error);
}

static void reset_session(void)
{
    if (s_state.owner == XMINI_BUSINESS_GROUP)
        xmini_group_on_closed(s_state.generation, s_final_error, now_ms());
    atomic_store(&s_audio_enabled, false);
    atomic_store(&s_group_audio, false);
    atomic_store(&s_group_transmit, false);
    atomic_store(&s_ptt_wanted, false);
    s_state.media_active = s_state.speaking = false;
    if (s_state.owner == XMINI_BUSINESS_WECHAT && (s_wx_call_id[0] || s_state.room[0])) {
        unsigned index = s_closed_wx_index++ % 4;
        snprintf(s_closed_wx_calls[index], sizeof(s_closed_wx_calls[index]), "%s", s_wx_call_id);
        snprintf(s_closed_wx_rooms[index], sizeof(s_closed_wx_rooms[index]), "%s", s_state.room);
    }
    s_state.owner = XMINI_BUSINESS_NONE;
    s_state.peer[0] = s_state.room[0] = s_state.caption[0] = 0;
    s_state.generation = 0;
    s_state.tx_stream = 0;
    s_role[0] = s_rpc_id[0] = s_session_id[0] = 0;
    s_wx_call_id[0] = s_wx_target_app[0] = 0;
    release_json(&s_wx_room_json);
    s_wx_reject_deferred = s_wx_reject_attempted = false;
    s_wx_reject_reason = 0;
    s_action_pending = s_cleanup_pending = s_ending = false;
    s_cleanup_failed = false;
    s_release_deferred = s_reconcile_deferred = false;
    s_reconcile_release = false;
    release_json(&s_cleanup_room);
    s_cleanup_generation = s_cleanup_epoch = 0;
    s_release_not_found = false;
    s_release_error = 0;
    s_room_released_generation = s_room_released_epoch = 0;
    s_rtc_session = s_disconnect_requested = s_stop_requested = s_media_requested = false;
    s_wx_room_joined = s_wx_hangup_deferred = false;
    s_session_started = s_peer_answered = s_ai_request_sent = false;
    s_deadline = s_ending_deadline = 0;
    phase(s_final_error ? XMINI_BUSINESS_ERROR : XMINI_BUSINESS_IDLE, s_final_error,
          s_final_error ? NULL : "");
    if (!s_final_error)
        arm_idle_listener();
}

static int send_json(uint32_t command, const cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    if (!text)
        return ESP_ERR_NO_MEM;
    int result = xmini_rtc_send_command(s_state.generation, command, text, strlen(text));
    release_json(&text);
    return result > 0 ? 0 : result ? result : ESP_ERR_INVALID_RESPONSE;
}

static int ai_notification(const char *method)
{
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "jsonrpc", "2.0") ||
        !cJSON_AddStringToObject(json, "method", method)) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    int error = send_json(AI_COMMAND, json);
    cJSON_Delete(json);
    return error;
}

static bool rtc_released(void)
{
    xmini_rtc_status_t rtc;
    xmini_rtc_status(&rtc);
    return !s_rtc_session && !rtc.connecting && !rtc.connected && !rtc.closing &&
           !rtc.sdk_pending && !s_callbacks.media_busy(s_state.generation, s_callbacks.context);
}

static bool room_release_confirmed(void)
{
    return s_state.generation && s_room_released_generation == s_state.generation &&
        s_room_released_epoch == platform_epoch();
}

static void request_device_release(xmini_business_phase_t previous)
{
    if (!s_state.room[0] || s_cleanup_pending)
        return;
    if (!rtc_released()) {
        s_release_deferred = true;
        s_release_previous = previous;
        return;
    }
    s_release_deferred = false;
    platform_operation_t operation = PLATFORM_DEVICE_CALL_HANGUP;
    const char *reason = "hangup";
    if (s_outgoing && (previous == XMINI_BUSINESS_OUTGOING || previous == XMINI_BUSINESS_FETCHING)) {
        operation = PLATFORM_DEVICE_CALL_CANCEL;
        reason = NULL;
    } else if (!s_outgoing && previous == XMINI_BUSINESS_RINGING) {
        operation = PLATFORM_DEVICE_CALL_REJECT;
        reason = "decline";
    }
    size_t room_bytes = strlen(s_state.room) + 1;
    char *room = malloc(room_bytes);
    if (room) memcpy(room, s_state.room, room_bytes);
    cJSON *body = room ? room_body(reason) : NULL;
    int error = body ? request(operation, body, true) : ESP_ERR_NO_MEM;
    cJSON_Delete(body);
    if (!error) {
        release_json(&s_cleanup_room);
        s_cleanup_room = room;
        s_release_not_found = false;
        s_release_error = 0;
        s_cleanup_pending = true;
    } else {
        release_json(&room);
        s_cleanup_failed = true;
        s_reconcile_release = true;
        if (!s_final_error) s_final_error = error;
        s_state.error = s_final_error;
        ESP_LOGE(TAG, "room cleanup request failed=%d", error);
    }
}

static void reconcile_room(bool release_if_present)
{
    s_reconcile_release = release_if_present;
    if (!rtc_released()) {
        s_reconcile_deferred = true;
        return;
    }
    s_reconcile_deferred = false;
    int error = request(PLATFORM_DEVICE_CALL_ROOM, NULL, true);
    if (!error) {
        s_cleanup_pending = true;
        s_cleanup_failed = false;
    } else {
        s_cleanup_failed = true;
        if (!s_final_error) s_final_error = error;
        ESP_LOGE(TAG, "room reconciliation unavailable=%d; ownership retained", error);
    }
}

static int reject_wechat(const cJSON *payload, unsigned reason)
{
    if (!*string_field(payload, "wx_app_id") || !*string_field(payload, "wx_model_id") ||
        !*string_field(payload, "wx_room_id") || !*string_field(payload, "wx_server_token"))
        return ESP_ERR_INVALID_RESPONSE;
    cJSON *body = cJSON_CreateObject();
    bool valid = body &&
        cJSON_AddStringToObject(body, "wx_app_id", string_field(payload, "wx_app_id")) &&
        cJSON_AddStringToObject(body, "wx_model_id", string_field(payload, "wx_model_id")) &&
        cJSON_AddStringToObject(body, "wx_room_id", string_field(payload, "wx_room_id")) &&
        cJSON_AddStringToObject(body, "wx_session_token", string_field(payload, "wx_server_token")) &&
        cJSON_AddStringToObject(body, "wx_payload", string_field(payload, "wx_payload")) &&
        cJSON_AddNumberToObject(body, "hangup_reason", reason);
    char *json = valid ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (!json)
        return ESP_ERR_NO_MEM;
    int result = xmini_rtc_service_request("/v1/wxvoip/reject", json, NULL, NULL);
    release_json(&json);
    ESP_LOGI(TAG, "WX reject reason=%u completed=%d", reason, result);
    return result;
}

static void defer_wechat_reject(unsigned reason)
{
    if (!s_wx_reject_deferred) {
        s_wx_reject_deferred = true;
        s_wx_reject_attempted = false;
        s_wx_reject_reason = reason;
    }
}

static void finish_wechat_reject(void)
{
    if (!s_wx_reject_deferred || s_wx_reject_attempted || !s_state.platform_online ||
        !s_state.rtc_online || !rtc_released())
        return;
    s_wx_reject_attempted = true;
    cJSON *payload = cJSON_Parse(s_wx_room_json);
    int error = reject_wechat(payload, s_wx_reject_reason);
    cJSON_Delete(payload);
    if (!error) {
        s_wx_reject_deferred = false;
        s_cleanup_failed = false;
    } else {
        s_cleanup_failed = true;
        if (!s_final_error) s_final_error = error;
        s_ending_deadline = 0;
        phase(XMINI_BUSINESS_ERROR, s_final_error, "WX rejection not confirmed");
    }
}

static void begin_end(int error, const char *reason, bool notify_server)
{
    begin_end_notifications(error, reason, notify_server, notify_server);
}

static void begin_end_notifications(int error, const char *reason, bool notify_server, bool notify_peer)
{
    if (s_ending)
        return;
    xmini_business_phase_t previous = s_state.phase;
    s_ending = true;
    s_final_error = error;
    s_ending_deadline = now_ms() + 15000;
    atomic_store(&s_audio_enabled, false);
    atomic_store(&s_group_transmit, false);
    atomic_store(&s_ptt_wanted, false);
    if (s_state.owner != XMINI_BUSINESS_NONE && s_state.owner != XMINI_BUSINESS_LIVE)
        xmini_rtc_authorize_audio(s_state.generation, s_state.tx_stream, false);
    if (s_state.owner == XMINI_BUSINESS_GROUP)
        xmini_group_suspend(s_state.generation, error, now_ms());
    xmini_rtc_arm_inbound(0, false);
    if (s_callbacks.media_stop && (s_state.media_active ||
        (s_callbacks.media_busy && s_callbacks.media_busy(s_state.generation, s_callbacks.context)))) {
        int stopped = s_callbacks.media_stop(s_state.generation, s_callbacks.context);
        s_stop_requested = true;
        if (stopped && !s_final_error)
            s_final_error = stopped;
    }
    if (s_state.owner == XMINI_BUSINESS_AI && (s_session_started || s_ai_request_sent) && notify_server) {
        int ended = ai_notification("end_session");
        if (ended && !s_final_error)
            s_final_error = ended;
    }
    if (s_state.owner == XMINI_BUSINESS_DEVICE && notify_server && !s_action_pending)
        request_device_release(previous);
    if (s_state.owner == XMINI_BUSINESS_WECHAT && notify_server) {
        if (s_wx_room_joined) s_wx_hangup_deferred = true;
        else if (s_wx_room_json) defer_wechat_reject(previous == XMINI_BUSINESS_RINGING ? 7 : 1);
    }
    if (s_state.owner == XMINI_BUSINESS_DEVICE && s_peer_answered && notify_peer) {
        static const char hangup[] = "{\"reason\":1}";
        int sent = xmini_rtc_send_command(s_state.generation, HANGUP_COMMAND, hangup, sizeof(hangup) - 1);
        if (sent <= 0 && !s_final_error)
            s_final_error = sent ? sent : ESP_ERR_INVALID_RESPONSE;
    }
    phase(XMINI_BUSINESS_ENDING, s_final_error, reason);
}

static bool receive_audio(uint32_t generation, uint8_t stream, uint32_t pts,
                           const uint8_t *data, size_t length, void *context)
{
    (void)context;
    if (!atomic_load(&s_audio_enabled) || generation != atomic_load(&s_audio_generation))
        return false;
    unsigned expected = atomic_load(&s_audio_rx_stream);
    if (expected != 16 && stream != expected)
        return false;
    return s_callbacks.receive && s_callbacks.receive(generation, stream, pts, data, length,
                                                       s_callbacks.context);
}

static void activate_media(void)
{
    if (s_state.media_active || s_media_requested || s_ending)
        return;
    if (s_state.owner != XMINI_BUSINESS_LIVE) {
        int error = xmini_rtc_authorize_audio(s_state.generation, s_state.tx_stream, true);
        if (error) {
            begin_end(error, "audio business permission failed", true);
            return;
        }
    }
    s_media_requested = true;
    s_deadline = now_ms() + 3000;
}

static void media_tick(void)
{
    if (!s_media_requested || s_ending)
        return;
    int error = s_callbacks.media_start(s_state.generation, s_state.tx_stream, s_callbacks.context);
    if (!error) {
        s_media_requested = false;
        s_state.media_active = true;
        atomic_store(&s_audio_generation, s_state.generation);
        atomic_store(&s_audio_stream, s_state.tx_stream);
        atomic_store(&s_audio_rx_stream, s_state.owner == XMINI_BUSINESS_LIVE ? 11 : s_state.tx_stream);
        atomic_store(&s_group_audio, s_state.owner == XMINI_BUSINESS_GROUP);
        atomic_store(&s_audio_enabled, true);
        s_deadline = 0;
        phase(XMINI_BUSINESS_ACTIVE, 0, "");
    } else if (error != ESP_ERR_INVALID_STATE) {
        begin_end(error, "media start failed", true);
    } else if (now_ms() >= s_deadline) {
        begin_end(error, "media admission timeout", true);
    }
}

static void begin_owner(xmini_business_owner_t owner, bool outgoing)
{
    xmini_rtc_arm_inbound(0, false);
    s_state.owner = owner;
    s_state.generation = new_generation();
    s_state.tx_stream = owner == XMINI_BUSINESS_AI ? 1 : owner == XMINI_BUSINESS_WECHAT ? 0 : 10;
    s_state.error = s_final_error = 0;
    s_room_released_generation = s_room_released_epoch = 0;
    s_state.caption[0] = s_state.reason[0] = 0;
    s_state.caption_truncated = false;
    s_caption_type = s_caption_utterance = -1;
    s_outgoing = outgoing;
    s_peer_answered = s_session_started = false;
    s_deadline = now_ms() + 20000;
}

static void connect_wechat(void)
{
    cJSON *payload = s_wx_room_json ? cJSON_Parse(s_wx_room_json) : NULL;
    const char *peer = string_field(payload, "peer_id"), *token = string_field(payload, "token");
    int error = *peer && *token ? xmini_rtc_connect(XMINI_RTC_WHIP, s_state.generation, peer, token)
                               : ESP_ERR_INVALID_RESPONSE;
    cJSON_Delete(payload);
    if (error) begin_end(error, "WX connect submission failed", true);
    else { s_rtc_session = true; s_deadline = now_ms() + 30000; phase(XMINI_BUSINESS_CONNECTING, 0, ""); }
}

static int submit_ai_start(void)
{
    snprintf(s_rpc_id, sizeof(s_rpc_id), "c3-%lu-%llu", (unsigned long)s_state.generation,
             (unsigned long long)now_ms());
    cJSON *json = cJSON_CreateObject(), *params = cJSON_CreateObject();
    if (!json || !params || !cJSON_AddStringToObject(json, "jsonrpc", "2.0") ||
        !cJSON_AddStringToObject(json, "id", s_rpc_id) ||
        !cJSON_AddStringToObject(json, "method", "start_session") ||
        !cJSON_AddItemToObject(json, "params", params)) {
        cJSON_Delete(json);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    cJSON *input = cJSON_Parse("{\"codec\":\"alaw\",\"sample_rate\":8000,\"channels\":1}");
    cJSON *output = cJSON_Duplicate(input, true);
    if (!cJSON_AddStringToObject(params, "device_id", s_device_id) ||
        !cJSON_AddStringToObject(params, "role_id", s_role) || !input || !output) {
        cJSON_Delete(input); cJSON_Delete(output); cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToObject(params, "input_audio", input)) {
        cJSON_Delete(input); cJSON_Delete(output); cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToObject(params, "output_audio", output)) {
        cJSON_Delete(output); cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    int error = send_json(AI_COMMAND, json);
    cJSON_Delete(json);
    if (!error) s_ai_request_sent = true;
    return error;
}

static bool audio_format(const cJSON *format)
{
    const char *codec = string_field(format, "codec");
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(format, "sample_rate");
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(format, "channels");
    return (strcmp(codec, "alaw") == 0 || strcmp(codec, "g711a") == 0) &&
           cJSON_IsNumber(rate) && rate->valueint == 8000 && rate->valuedouble == 8000 &&
           cJSON_IsNumber(channels) && channels->valueint == 1 && channels->valuedouble == 1;
}

static void handle_ai_command(const xmini_rtc_event_t *event)
{
    cJSON *root = cJSON_ParseWithLength(xmini_rtc_event_payload(event), event->length);
    if (!root) {
        begin_end(ESP_ERR_INVALID_RESPONSE, "AI JSON invalid", true);
        return;
    }
    const char *id = string_field(root, "id");
    const cJSON *id_value = cJSON_GetObjectItemCaseSensitive(root, "id");
    const char *method = string_field(root, "method");
    if (*id && strcmp(id, s_rpc_id) == 0 && s_state.phase == XMINI_BUSINESS_SESSION) {
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        bool valid = !cJSON_GetObjectItemCaseSensitive(root, "error") && cJSON_IsObject(result) &&
                     copy_field(result, "session_id", s_session_id, sizeof(s_session_id), true) &&
                     audio_format(cJSON_GetObjectItemCaseSensitive(result, "input_audio")) &&
                     audio_format(cJSON_GetObjectItemCaseSensitive(result, "output_audio"));
        if (valid) {
            ESP_LOGI(TAG, "AI profile verified codec=alaw rate=8000 channels=1 gen=%lu",
                     (unsigned long)s_state.generation);
            s_session_started = true;
            int error = xmini_rtc_subscribe(s_state.generation, 1, true);
            if (error)
                begin_end(error, "AI subscribe failed", true);
            else
                activate_media();
        } else {
            begin_end(ESP_ERR_NOT_SUPPORTED, "AI format/session rejected", true);
        }
    } else if (*method) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        if (strcmp(method, "caption") == 0) {
            const char *text = string_field(params, "text");
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(params, "caption_type");
            const cJSON *utterance = cJSON_GetObjectItemCaseSensitive(params, "utterance_id");
            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(params, "mode");
            int caption_type = cJSON_IsNumber(type) ? type->valueint : -1;
            int utterance_id = cJSON_IsNumber(utterance) ? utterance->valueint : -1;
            bool same_group = caption_type == s_caption_type && utterance_id == s_caption_utterance;
            size_t offset = same_group && cJSON_IsNumber(mode) && mode->valueint == 1 ? strlen(s_state.caption) : 0;
            bool truncated = copy_utf8(s_state.caption + offset, sizeof(s_state.caption) - offset, text);
            s_state.caption_truncated = (offset && s_state.caption_truncated) || truncated;
            s_caption_type = caption_type; s_caption_utterance = utterance_id;
            publish();
        } else if (strcmp(method, "round_start") == 0 || strcmp(method, "round_end") == 0) {
            s_state.speaking = strcmp(method, "round_start") == 0;
            publish();
        } else if (strcmp(method, "interrupt") == 0) {
            if (s_callbacks.media_flush)
                s_callbacks.media_flush(s_state.generation, s_callbacks.context);
            s_state.speaking = false;
            publish();
        } else if (strcmp(method, "end_session") == 0) {
            begin_end(0, "AI ended", false);
        } else if (strcmp(method, "device_action") == 0 &&
                   (cJSON_IsString(id_value) || cJSON_IsNumber(id_value))) {
            cJSON *response = cJSON_CreateObject();
            cJSON *error = cJSON_CreateObject();
            if (response && error) {
                cJSON_AddStringToObject(response, "jsonrpc", "2.0");
                cJSON_AddItemToObject(response, "id", cJSON_Duplicate(id_value, true));
                cJSON_AddNumberToObject(error, "code", -32601);
                cJSON_AddStringToObject(error, "message", "unsupported device action");
                cJSON_AddItemToObject(response, "error", error);
                error = NULL;
                int sent = send_json(AI_COMMAND, response);
                if (sent)
                    ESP_LOGW(TAG, "AI action rejection send failed=%d", sent);
            }
            cJSON_Delete(error); cJSON_Delete(response);
        }
    }
    cJSON_Delete(root);
}

static bool contact_matches(bool wechat)
{
    return s_state.contacts_filter == XMINI_CONTACT_ALL ||
        (wechat ? s_state.contacts_filter == XMINI_CONTACT_WECHAT :
                  s_state.contacts_filter == XMINI_CONTACT_DEVICE);
}

static bool valid_contact_entry(const cJSON *entry, bool wechat)
{
    const char *id = string_field(entry, "device_id");
    const char *app = string_field(entry, "wx_app_id");
    return *id && strnlen(id, 65) < 65 && (!wechat || (*app && strnlen(app, 65) < 65));
}

static bool handle_contacts(const cJSON *data)
{
    s_state.contacts_count = s_state.contacts_total = 0;
    s_state.contacts_filtered_total = 0;
    clear_memory(s_state.contacts, sizeof(s_state.contacts));
    const cJSON *contacts = cJSON_GetObjectItemCaseSensitive(data, "contacts");
    if (!cJSON_IsArray(contacts)) return false;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, contacts) {
        const char *type = string_field(entry, "type");
        bool wechat = strcmp(type, "voip") == 0;
        if (!wechat && strcmp(type, "device") != 0) continue;
        if (!valid_contact_entry(entry, wechat)) {
            s_state.contacts_total = s_state.contacts_filtered_total = 0;
            return false;
        }
        ++s_state.contacts_total;
        if (contact_matches(wechat)) ++s_state.contacts_filtered_total;
    }
    unsigned last_page = s_state.contacts_filtered_total ?
        (s_state.contacts_filtered_total - 1) / XMINI_BUSINESS_CONTACTS : 0;
    if (s_state.contacts_page > last_page) s_state.contacts_page = last_page;
    unsigned offset = s_state.contacts_page * XMINI_BUSINESS_CONTACTS, selected = 0;
    cJSON_ArrayForEach(entry, contacts) {
        const char *type = string_field(entry, "type");
        bool wechat = strcmp(type, "voip") == 0;
        if ((!wechat && strcmp(type, "device") != 0) || !contact_matches(wechat)) continue;
        if (selected++ < offset) continue;
        if (s_state.contacts_count == XMINI_BUSINESS_CONTACTS) break;
        xmini_business_contact_t *contact = &s_state.contacts[s_state.contacts_count];
        copy_field(entry, "device_id", contact->id, sizeof(contact->id), true);
        contact->wechat = wechat;
        const char *name = string_field(entry, "remark");
        if (!*name) name = string_field(entry, "name");
        if (!*name) name = contact->id;
        copy_utf8(contact->name, sizeof(contact->name), name);
        if (wechat)
            copy_field(entry, "wx_app_id", contact->app_id, sizeof(contact->app_id), true);
        ++s_state.contacts_count;
    }
    return true;
}

static bool handle_pending(const cJSON *data)
{
    s_state.pending_count = s_state.pending_total = 0;
    clear_memory(s_state.pending, sizeof(s_state.pending));
    const cJSON *pending = cJSON_GetObjectItemCaseSensitive(data, "pending"), *entry;
    if (!cJSON_IsArray(pending)) return false;
    cJSON_ArrayForEach(entry, pending) {
        if (strcmp(string_field(entry, "type"), "device") != 0) continue;
        const char *id = string_field(entry, "peer_device_id");
        if (!*id || strnlen(id, 65) >= 65) { s_state.pending_total = 0; return false; }
        ++s_state.pending_total;
    }
    unsigned last_page = s_state.pending_total ? (s_state.pending_total - 1) / 4 : 0;
    if (s_state.pending_page > last_page) s_state.pending_page = last_page;
    unsigned offset = s_state.pending_page * 4, selected = 0;
    cJSON_ArrayForEach(entry, pending) {
        if (strcmp(string_field(entry, "type"), "device") != 0) continue;
        if (selected++ < offset) continue;
        if (s_state.pending_count == 4) break;
        copy_field(entry, "peer_device_id", s_state.pending[s_state.pending_count++],
                   sizeof(s_state.pending[0]), true);
    }
    return true;
}

static void handle_response(const platform_event_t *event)
{
    if (event->operation >= PLATFORM_GROUP_ASSIGNMENT && event->operation <= PLATFORM_GROUP_PRESENCE) {
        xmini_group_on_http(event->operation, event->generation, event->error, event->json, now_ms());
        return;
    }
    cJSON *root = event->json ? cJSON_Parse(event->json) : NULL;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    bool ok = !event->error && response_ok(root);
    int error = ok ? 0 : response_error(root, event->error);
    if (event->generation == 0) {
        if (event->epoch != platform_epoch()) {
            s_state.stale_events++;
            publish();
            cJSON_Delete(root);
            return;
        }
        if (event->operation == PLATFORM_PROFILE) {
            s_profile_pending = false;
            s_state.profile_ready = ok;
            metadata_result(error);
            if (ok) refresh_contacts();
        } else if (event->operation == PLATFORM_DEVICE_CONTACTS) {
            s_contacts_pending = false;
            if (s_contacts_requested_selection != s_contacts_selection) s_contacts_dirty = true;
            else {
                s_state.contacts_loading = false;
                s_state.contacts_error = ok ? 0 : error;
                if (ok && !handle_contacts(data)) {
                    s_state.contacts_error = ESP_ERR_INVALID_RESPONSE;
                    metadata_result(ESP_ERR_INVALID_RESPONSE);
                } else if (!ok) {
                    metadata_result(error);
                    s_state.contacts_count = s_state.contacts_total = s_state.contacts_filtered_total = 0;
                    clear_memory(s_state.contacts, sizeof(s_state.contacts));
                }
                refresh_pending();
            }
        } else if (event->operation == PLATFORM_DEVICE_CONTACTS_PENDING) {
            s_pending_pending = false;
            if (s_pending_requested_selection != s_pending_selection) s_pending_deferred = true;
            else {
                s_state.pending_loading = false;
                s_state.pending_error = ok ? 0 : error;
                if (ok && !handle_pending(data)) {
                    s_state.pending_error = ESP_ERR_INVALID_RESPONSE;
                    metadata_result(ESP_ERR_INVALID_RESPONSE);
                } else if (!ok) {
                    metadata_result(error);
                    s_state.pending_count = s_state.pending_total = 0;
                    clear_memory(s_state.pending, sizeof(s_state.pending));
                }
            }
        } else if (event->operation >= PLATFORM_DEVICE_CONTACT_REQUEST && event->operation <= PLATFORM_DEVICE_CONTACT_DELETE) {
            s_contact_mutating = false;
            metadata_result(error);
            if (ok) refresh_contacts();
        }
        arm_idle_listener();
        publish();
        cJSON_Delete(root);
        return;
    }
    if (event->generation != s_state.generation) {
        s_state.stale_events++;
        publish();
        cJSON_Delete(root);
        return;
    }
    bool release = event->operation == PLATFORM_DEVICE_CALL_CANCEL ||
        event->operation == PLATFORM_DEVICE_CALL_REJECT || event->operation == PLATFORM_DEVICE_CALL_HANGUP;
    if (release || event->operation == PLATFORM_DEVICE_CALL_ROOM) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        ok = !event->error && cJSON_IsNumber(code) && code->valuedouble == 200;
        error = ok ? ESP_OK : response_error(root, event->error);
    }
    if ((release || event->operation == PLATFORM_DEVICE_CALL_ROOM) &&
        (s_state.owner != XMINI_BUSINESS_DEVICE || !s_ending || !s_cleanup_pending ||
         !s_action_pending || event->operation != s_action_operation ||
         event->operation != s_cleanup_operation || event->generation != s_cleanup_generation ||
         event->epoch != s_cleanup_epoch ||
         ((release || s_release_not_found) &&
          (!s_cleanup_room || (s_state.room[0] && strcmp(s_cleanup_room, s_state.room) != 0))))) {
        s_state.stale_events++;
        publish();
        cJSON_Delete(root);
        return;
    }
    if ((release || event->operation == PLATFORM_DEVICE_CALL_ROOM) && event->epoch != platform_epoch()) {
        s_action_pending = s_cleanup_pending = false;
        s_release_not_found = false;
        s_release_error = 0;
        s_state.stale_events++;
        if (room_release_confirmed()) {
            s_cleanup_failed = false;
            release_json(&s_cleanup_room);
            ESP_LOGW(TAG, "old-epoch room cleanup completion retired after current room_cancel confirmation");
        } else {
            s_cleanup_failed = true;
            if (!s_final_error) s_final_error = ESP_ERR_INVALID_STATE;
            ESP_LOGW(TAG, "room cleanup completion belongs to an old platform epoch; ownership unconfirmed");
            if (release && s_state.platform_online) reconcile_room(false);
            else if (release) s_reconcile_deferred = true;
        }
        publish();
        cJSON_Delete(root);
        return;
    }
    if (s_action_pending && event->operation == s_action_operation)
        s_action_pending = false;
    if (release) {
        s_cleanup_pending = false;
        if (room_release_confirmed()) {
            if (!ok) ESP_LOGW(TAG, "room release failed=%d after matching room_cancel; confirmation retained", error);
            s_cleanup_failed = false;
            s_release_not_found = false;
            s_release_error = 0;
            release_json(&s_cleanup_room);
        } else if (!ok) {
            const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
            s_release_not_found = !event->delivery_failed && cJSON_IsNumber(code) &&
                code->valuedouble == 40400 && (event->error == ESP_OK || event->error == ESP_FAIL);
            s_release_error = error;
            if (!s_release_not_found && !s_final_error) s_final_error = error;
            s_cleanup_failed = true;
            if (s_release_not_found)
                ESP_LOGW(TAG, "room release failed=%d code=40400; verifying absence before completion", error);
            else
                ESP_LOGE(TAG, "room release failed=%d code=%d; remote cleanup not confirmed",
                         error, cJSON_IsNumber(code) ? code->valueint : 0);
            if (s_release_not_found || event->delivery_failed || event->error)
                reconcile_room(false);
        } else {
            s_state.room[0] = 0;
            s_cleanup_failed = false;
            release_json(&s_cleanup_room);
        }
    } else if (event->operation == PLATFORM_DEVICE_CALL_ROOM) {
        s_cleanup_pending = false;
        if (room_release_confirmed()) {
            if (!ok) ESP_LOGW(TAG, "room query failed=%d after matching room_cancel; confirmation retained", error);
            else if (data && !cJSON_IsNull(data))
                ESP_LOGW(TAG, "room query after matching room_cancel is nonempty; not adopted by the retired call");
            s_cleanup_failed = false;
            s_release_not_found = false;
            s_release_error = 0;
            release_json(&s_cleanup_room);
        } else if (!ok) {
            s_cleanup_failed = true;
            if (!s_final_error) s_final_error = error;
        } else if (!data || cJSON_IsNull(data)) {
            if (s_release_not_found)
                ESP_LOGI(TAG, "room release 40400 confirmed empty gen=%lu", (unsigned long)s_state.generation);
            s_state.room[0] = 0;
            s_cleanup_failed = false;
            s_release_not_found = false;
            s_release_error = 0;
            release_json(&s_cleanup_room);
        } else {
            const char *room = string_field(data, "room_id");
            const char *role = string_field(data, "role");
            bool owned = *room && strlen(room) < sizeof(s_state.room) &&
                ((s_state.room[0] && strcmp(room, s_state.room) == 0) ||
                 (!s_state.room[0] && strcmp(role, "caller") == 0 &&
                  strcmp(string_field(data, "caller"), s_device_id) == 0));
            const char *remote_status = string_field(data, "status");
            bool known_status = strcmp(remote_status, "answered") == 0 || strcmp(remote_status, "active") == 0;
            if (owned && s_reconcile_release && known_status) {
                strcpy(s_state.room, room);
                s_outgoing = strcmp(role, "caller") == 0;
                xmini_business_phase_t previous = strcmp(remote_status, "answered") == 0 ? XMINI_BUSINESS_ACTIVE :
                    s_outgoing ? XMINI_BUSINESS_OUTGOING : XMINI_BUSINESS_RINGING;
                s_reconcile_release = false;
                request_device_release(previous);
            } else {
                s_cleanup_failed = true;
                if (!s_final_error) s_final_error = s_release_not_found ? s_release_error : ESP_ERR_INVALID_STATE;
                ESP_LOGE(TAG, "room reconciliation found an uncleared/nonowned room; no automatic retry");
            }
        }
    } else if (event->operation == PLATFORM_CALL && s_state.owner == XMINI_BUSINESS_WECHAT) {
        const char *call = string_field(data, "call_id");
        if (!ok || !*call || strlen(call) >= sizeof(s_wx_call_id) ||
            (s_wx_call_id[0] && strcmp(s_wx_call_id, call) != 0)) {
            begin_end(error ? error : ESP_ERR_INVALID_RESPONSE, "WX request failed", false);
        } else {
            strcpy(s_wx_call_id, call);
            if (!s_ending && !s_rtc_session) { s_deadline = now_ms() + 35000; phase(XMINI_BUSINESS_OUTGOING, 0, ""); }
        }
    } else if (event->operation == PLATFORM_AI_TOKEN && !s_ending) {
        const char *peer = string_field(data, "peer_id"), *token = string_field(data, "token");
        if (!ok || !*peer || !*token || !copy_field(data, "role_id", s_role, sizeof(s_role), true)) {
            begin_end(error ? error : ESP_ERR_INVALID_RESPONSE, "AI token failed", false);
        } else {
            int connected = xmini_rtc_connect(XMINI_RTC_WHIP, s_state.generation, peer, token);
            if (connected) begin_end(connected, "AI connect submission failed", false);
            else { s_rtc_session = true; s_deadline = now_ms() + 20000; phase(XMINI_BUSINESS_CONNECTING, 0, ""); }
        }
    } else if (event->operation == PLATFORM_DEVICE_CALL_REQUEST) {
        bool got_room = response_ok(root) && copy_field(data, "room_id", s_state.room, sizeof(s_state.room), true);
        if (got_room && !ok) {
            if (s_ending) request_device_release(XMINI_BUSINESS_OUTGOING);
            else begin_end(error, "call creation crossed network epoch", true);
        } else if (!got_room && (event->delivery_failed || (!root && event->error))) {
            begin_end(error, "call request result not delivered", false);
            reconcile_room(true);
        } else if (!ok || !got_room) {
            begin_end(error ? error : ESP_ERR_INVALID_RESPONSE, "call request failed", false);
        } else if (s_ending) {
            request_device_release(XMINI_BUSINESS_OUTGOING);
        } else {
            int armed = xmini_rtc_arm_inbound(s_state.generation, true);
            if (armed) begin_end(armed, "call listener admission failed", true);
            else { s_deadline = now_ms() + 35000; phase(XMINI_BUSINESS_OUTGOING, 0, ""); }
        }
    } else if (event->operation == PLATFORM_DEVICE_CALL_INFO && !s_ending) {
        const char *peer = string_field(data, "device_id"), *token = string_field(data, "token");
        if (!ok || strcmp(peer, s_state.peer) != 0 || !*token) {
            begin_end(error ? error : ESP_ERR_INVALID_RESPONSE, "call token failed", true);
        } else {
            int connected = xmini_rtc_connect(XMINI_RTC_P2P, s_state.generation, peer, token);
            if (connected) begin_end(connected, "call connect submission failed", true);
            else { s_rtc_session = true; s_deadline = now_ms() + 20000; phase(XMINI_BUSINESS_CONNECTING, 0, ""); }
        }
    }
    cJSON_Delete(root);
}

static void reject_busy_device(const cJSON *payload)
{
    const char *room = string_field(payload, "room_id");
    cJSON *body = cJSON_CreateObject();
    if (body && *room && cJSON_AddStringToObject(body, "room_id", room) &&
        cJSON_AddStringToObject(body, "reason", "busy")) {
        int error = request(PLATFORM_DEVICE_CALL_REJECT, body, false);
        if (error)
            ESP_LOGE(TAG, "busy reject request failed=%d", error);
    }
    cJSON_Delete(body);
}

static void incoming_wechat(const cJSON *payload)
{
    const char *room = string_field(payload, "wx_room_id");
    const char *call = string_field(payload, "wx_call_id");
    const char *from = string_field(payload, "wx_from");
    const char *openid = string_field(payload, "wx_user_openid");
    const char *app = string_field(payload, "wx_app_id");
    if (s_state.owner == XMINI_BUSINESS_WECHAT && s_state.room[0] && strcmp(s_state.room, room) == 0)
        return; /* MQTT duplicate must never reject its own live room. */
    bool stale = false;
    for (unsigned i = 0; i < 4; ++i)
        if ((*call && strcmp(call, s_closed_wx_calls[i]) == 0) ||
            (*room && strcmp(room, s_closed_wx_rooms[i]) == 0)) stale = true;
    bool outbound = s_state.owner == XMINI_BUSINESS_WECHAT && s_outgoing &&
                    strcmp(s_state.peer, openid) == 0 &&
                    (!*s_wx_target_app || !*app || strcmp(s_wx_target_app, app) == 0) &&
                    ((!*s_wx_call_id && strcmp(from, s_device_id) == 0) ||
                     (*s_wx_call_id && *call && strcmp(s_wx_call_id, call) == 0));
    bool idle = s_state.phase == XMINI_BUSINESS_IDLE && s_state.rtc_online && s_state.profile_ready && !s_state.maintenance;
    if (stale || (!outbound && (!idle || strcmp(from, s_device_id) == 0)) || s_ending) {
        int error = reject_wechat(payload, 5);
        if (error) ESP_LOGE(TAG, "WX busy/stale reject failed=%d", error);
        return;
    }
    if (!outbound) begin_owner(XMINI_BUSINESS_WECHAT, false);
    if (!copy_field(payload, "wx_room_id", s_state.room, sizeof(s_state.room), true) ||
        !copy_field(payload, "wx_user_openid", s_state.peer, sizeof(s_state.peer), true) ||
        !copy_field(payload, "wx_call_id", s_wx_call_id, sizeof(s_wx_call_id), false)) {
        begin_end(ESP_ERR_INVALID_RESPONSE, "WX incoming invalid", false);
        return;
    }
    release_json(&s_wx_room_json);
    s_wx_room_json = cJSON_PrintUnformatted(payload);
    if (!s_wx_room_json) { begin_end(ESP_ERR_NO_MEM, "WX room allocation failed", false); return; }
    if (outbound) connect_wechat();
    else { s_deadline = now_ms() + 30000; phase(XMINI_BUSINESS_RINGING, 0, ""); }
}

static void handle_platform_message(const char *text, unsigned epoch)
{
    cJSON *root = cJSON_Parse(text);
    const char *type = string_field(root, "type");
    const char *channel = string_field(root, "channel");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (strcmp(type, "callers_update") == 0) {
        s_state.contacts_count = s_state.contacts_total = s_state.contacts_filtered_total = 0;
        s_state.pending_count = s_state.pending_total = 0;
        clear_memory(s_state.contacts, sizeof(s_state.contacts));
        clear_memory(s_state.pending, sizeof(s_state.pending));
        refresh_contacts();
    } else if (strcmp(type, "room_assignment_changed") == 0) {
        xmini_group_assignment_changed(now_ms());
    } else if (strcmp(type, "call_incoming") == 0 && strcmp(channel, "wx") == 0) {
        incoming_wechat(payload);
    } else if (strcmp(type, "call_cancel") == 0 && strcmp(channel, "wx") == 0 &&
               s_state.owner == XMINI_BUSINESS_WECHAT && *string_field(payload, "wx_room_id") &&
               strcmp(string_field(payload, "wx_room_id"), s_state.room) == 0) {
        begin_end(0, "WX remote cancelled", false);
    } else if (strcmp(type, "call_incoming") == 0 && strcmp(channel, "device") == 0) {
        if (s_state.owner == XMINI_BUSINESS_DEVICE && s_state.room[0] &&
            strcmp(string_field(payload, "room_id"), s_state.room) == 0) {
            cJSON_Delete(root);
            return; /* QoS redelivery is the same offer, not a competing caller. */
        }
        if (s_state.phase != XMINI_BUSINESS_IDLE || !s_state.rtc_online || !s_state.profile_ready || s_state.maintenance) {
            reject_busy_device(payload);
        } else {
            begin_owner(XMINI_BUSINESS_DEVICE, false);
            if (!copy_field(payload, "room_id", s_state.room, sizeof(s_state.room), true) ||
                !copy_field(payload, "caller_id", s_state.peer, sizeof(s_state.peer), true)) {
                begin_end(ESP_ERR_INVALID_RESPONSE, "incoming call invalid", false);
            } else {
                s_deadline = now_ms() + 30000;
                phase(XMINI_BUSINESS_RINGING, 0, "");
            }
        }
    } else if (strcmp(type, "room_cancel") == 0 && strcmp(channel, "device") == 0 &&
               s_state.owner == XMINI_BUSINESS_DEVICE && s_state.room[0] &&
               strcmp(string_field(payload, "room_id"), s_state.room) == 0) {
        s_state.room[0] = 0;
        s_cleanup_failed = false;
        s_room_released_generation = s_state.generation;
        s_room_released_epoch = epoch;
        s_release_deferred = s_reconcile_deferred = false;
        s_reconcile_release = false;
        begin_end(0, string_field(payload, "reason"), false);
    } else if (strcmp(type, "call_reject") == 0 && s_state.owner == XMINI_BUSINESS_DEVICE &&
               strcmp(string_field(payload, "room_id"), s_state.room) == 0) {
        snprintf(s_state.reason, sizeof(s_state.reason), "%s", string_field(payload, "reason"));
        publish(); /* One target reject does not release the whole room. */
    }
    cJSON_Delete(root);
}

static bool signal_word_matches(uint32_t word, uint32_t identifier)
{
    /* AI/GROUP gateways accept legacy words and SDK-packed signaling. Device
     * and WeChat 0x2000/0x2001 remain exact wire words, not this convention. */
    return GET_CMD(word) == identifier || ((word & 0xffffU) & ~RESPONSE_BIT) == identifier;
}

static void command_diagnostic(const xmini_rtc_event_t *event, uint32_t disposition, int error)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    if (++s_command_diagnostics.sequence == 0) s_command_diagnostics.sequence = 1;
    s_command_diagnostics.generation = event->generation;
    s_command_diagnostics.connection32 = (uint32_t)(uintptr_t)event->connection;
    s_command_diagnostics.word = event->command;
    s_command_diagnostics.disposition = disposition;
    s_command_diagnostics.error = (int32_t)error;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void handle_rtc(const xmini_rtc_event_t *event)
{
    if (event->kind == XMINI_RTC_STARTED) {
        s_state.rtc_online = true;
        request_profile();
        arm_idle_listener();
        publish();
        return;
    }
    if (event->kind == XMINI_RTC_STOPPED) {
        s_state.rtc_online = false;
        s_rtc_session = false;
        int error = xmini_rtc_uninit();
        if (error) ESP_LOGE(TAG, "RTC stopped cleanup failed=%d", error);
        s_shutdown_sdk = false;
        publish();
        return;
    }
    if (event->kind == XMINI_RTC_OVERFLOW || event->kind == XMINI_RTC_ACCESS_HIJACKING) {
        begin_end(event->error, "RTC control fault", false);
        return;
    }
    if (event->kind == XMINI_RTC_ACCEPTED && s_state.phase == XMINI_BUSINESS_IDLE &&
        event->generation == s_idle_generation) {
        s_state.owner = XMINI_BUSINESS_LIVE;
        s_state.generation = event->generation;
        s_state.tx_stream = 10;
        s_rtc_session = true;
        int error = xmini_rtc_subscribe(s_state.generation, 11, true);
        if (error) begin_end(error, "live subscribe failed", false);
        else activate_media();
        return;
    }
    if (event->generation != s_state.generation || !s_state.generation) {
        if (event->kind == XMINI_RTC_COMMAND)
            command_diagnostic(event, XMINI_COMMAND_DISPATCH_STALE_GENERATION, 0);
        s_state.stale_events++;
        publish();
        return;
    }
    if (event->kind == XMINI_RTC_CLOSED || event->kind == XMINI_RTC_RETIRED) {
        s_rtc_session = false;
        if (!s_ending) begin_end(event->error, "remote disconnected", s_state.owner != XMINI_BUSINESS_WECHAT);
    } else if (event->kind == XMINI_RTC_ERROR || event->kind == XMINI_RTC_FORMAT_ERROR) {
        bool live_remote_close = s_state.owner == XMINI_BUSINESS_LIVE &&
            event->kind == XMINI_RTC_ERROR && event->error == TIRTC_E_CONN_REMOTECLOSE;
        begin_end(live_remote_close ? 0 : event->error,
                  live_remote_close ? "remote disconnected" : "RTC/media failed", !live_remote_close);
    } else if (event->kind == XMINI_RTC_REJECTED) {
        if (!s_ending) begin_end(ESP_ERR_INVALID_STATE, "unexpected connection", true);
    } else if (event->kind == XMINI_RTC_CONNECTED || event->kind == XMINI_RTC_ACCEPTED) {
        if (!event->error && s_state.owner == XMINI_BUSINESS_WECHAT) {
            s_wx_room_joined = true;
            if (s_ending && s_wx_reject_deferred && !s_wx_reject_attempted) {
                s_wx_reject_deferred = false;
                s_wx_hangup_deferred = true;
            }
        }
        if (event->error) {
            s_rtc_session = !event->retired;
            begin_end(event->error, "connection failed", true);
        } else if (!s_ending) {
            s_rtc_session = true;
            if (s_state.owner == XMINI_BUSINESS_GROUP) {
                xmini_group_on_connected(event->generation, event->error, now_ms());
                phase(XMINI_BUSINESS_SESSION, 0, "");
            } else if (s_state.owner == XMINI_BUSINESS_AI) {
                int error = submit_ai_start();
                if (error) begin_end(error, "start_session send failed", false);
                else { s_deadline = now_ms() + 10000; phase(XMINI_BUSINESS_SESSION, 0, ""); }
            } else if (s_state.owner == XMINI_BUSINESS_WECHAT) {
                s_deadline = now_ms() + 35000;
                phase(XMINI_BUSINESS_SESSION, 0, ""); /* 0x2000 is the answer barrier. */
            } else if (s_state.owner == XMINI_BUSINESS_DEVICE && !s_outgoing) {
                int error = xmini_rtc_subscribe(s_state.generation, 10, true);
                cJSON *body = room_body(NULL);
                if (!error) error = body ? send_json(CONNECT_COMMAND, body) : ESP_ERR_NO_MEM;
                cJSON_Delete(body);
                if (error) begin_end(error, "connected notice failed", true);
                else { s_peer_answered = true; activate_media(); }
            }
        }
    } else if (event->kind == XMINI_RTC_COMMAND && !s_ending) {
        uint32_t disposition = XMINI_COMMAND_DISPATCH_OTHER;
        int command_error = 0;
        if (s_state.owner == XMINI_BUSINESS_GROUP && signal_word_matches(event->command, 0x2200)) {
            xmini_group_on_signal(event->generation, xmini_rtc_event_payload(event), event->length, now_ms());
        } else if (s_state.owner == XMINI_BUSINESS_AI && signal_word_matches(event->command, AI_COMMAND)) {
            handle_ai_command(event);
        } else if (s_state.owner == XMINI_BUSINESS_DEVICE && event->command == CONNECT_COMMAND && s_outgoing) {
            cJSON *body = cJSON_ParseWithLength(xmini_rtc_event_payload(event), event->length);
            bool matching = strcmp(string_field(body, "room_id"), s_state.room) == 0;
            cJSON_Delete(body);
            disposition = XMINI_COMMAND_DISPATCH_INVALID_OR_ROOM_MISMATCH;
            if (matching) {
                int error = xmini_rtc_subscribe(s_state.generation, 10, true);
                if (error) {
                    disposition = XMINI_COMMAND_DISPATCH_SUBSCRIBE_ERROR;
                    command_error = error;
                    begin_end(error, "caller subscribe failed", true);
                } else {
                    s_peer_answered = true;
                    activate_media();
                    disposition = XMINI_COMMAND_DISPATCH_ACTIVATION_BRANCH;
                }
            }
        } else if (s_state.owner == XMINI_BUSINESS_WECHAT && event->command == CONNECT_COMMAND) {
            /* The platform's 0x2000 answer has data=NULL, len=0. */
            cJSON *body = event->length ?
                cJSON_ParseWithLength(xmini_rtc_event_payload(event), event->length) : NULL;
            if (!event->length || cJSON_IsObject(body)) {
                ESP_LOGI(TAG, "WX answer gen=%lu bytes=%u payload=%s",
                         (unsigned long)event->generation, event->length, body ? "object" : "empty");
                int error = xmini_rtc_select_audio(s_state.generation, 0, true);
                if (error) begin_end(error, "WX receive selection failed", true);
                else { s_peer_answered = true; activate_media(); }
            } else ESP_LOGW(TAG, "invalid WX answer ignored gen=%lu bytes=%u",
                            (unsigned long)event->generation, event->length);
            cJSON_Delete(body);
        } else if ((s_state.owner == XMINI_BUSINESS_DEVICE || s_state.owner == XMINI_BUSINESS_WECHAT) &&
                   event->command == HANGUP_COMMAND) {
            cJSON *body = cJSON_ParseWithLength(xmini_rtc_event_payload(event), event->length);
            const cJSON *reason = cJSON_GetObjectItemCaseSensitive(body, "reason");
            bool valid = cJSON_IsObject(body) && (s_state.owner == XMINI_BUSINESS_DEVICE ||
                (cJSON_IsNumber(reason) && reason->valuedouble == reason->valueint &&
                 reason->valueint >= 0 && reason->valueint <= 20));
            cJSON_Delete(body);
            if (valid) begin_end_notifications(0, "remote hangup", s_state.owner == XMINI_BUSINESS_DEVICE, false);
            else ESP_LOGW(TAG, "invalid hangup command ignored");
        }
        command_diagnostic(event, disposition, command_error);
    } else if (event->kind == XMINI_RTC_COMMAND) {
        command_diagnostic(event, XMINI_COMMAND_DISPATCH_ENDING, 0);
    }
    if (s_state.owner == XMINI_BUSINESS_WECHAT && event->kind == XMINI_RTC_SUBSCRIBE && !s_ending) {
        if (event->stream != 0) {
            ESP_LOGW(TAG, "WX audio subscribe ignored stream=%u expected=0", event->stream);
            return;
        }
        uint8_t previous = s_state.tx_stream;
        s_state.tx_stream = event->stream;
        atomic_store(&s_audio_stream, event->stream);
        if (previous != event->stream && (s_state.media_active || s_media_requested)) {
            xmini_rtc_authorize_audio(s_state.generation, previous, false);
            int error = xmini_rtc_authorize_audio(s_state.generation, event->stream, true);
            if (error) begin_end(error, "WX stream permission failed", true);
        }
        publish();
    }
}

static void start_wechat_call(const xmini_business_contact_t *contact)
{
    begin_owner(XMINI_BUSINESS_WECHAT, true);
    strcpy(s_state.peer, contact->id);
    strcpy(s_wx_target_app, contact->app_id);
    cJSON *body = cJSON_CreateObject();
    bool valid = body && cJSON_AddStringToObject(body, "device_id", s_device_id) &&
        cJSON_AddStringToObject(body, "wx_user_openid", contact->id) &&
        cJSON_AddStringToObject(body, "wx_app_id", contact->app_id) &&
        cJSON_AddStringToObject(body, "wx_room_type", "voice") &&
        cJSON_AddNumberToObject(body, "wx_version_type", 0);
    int error = valid ? request(PLATFORM_CALL, body, true) : ESP_ERR_NO_MEM;
    cJSON_Delete(body);
    if (error) begin_end(error, "WX request submission failed", false);
    else { s_deadline = now_ms() + 40000; phase(XMINI_BUSINESS_FETCHING, 0, ""); }
}

static void start_device_call(const char *target)
{
    begin_owner(XMINI_BUSINESS_DEVICE, true);
    snprintf(s_state.peer, sizeof(s_state.peer), "%s", target);
    cJSON *body = cJSON_CreateObject(), *targets = cJSON_CreateStringArray(&target, 1);
    if (!body || !targets || !cJSON_AddItemToObject(body, "targets", targets)) {
        cJSON_Delete(body); cJSON_Delete(targets);
        begin_end(ESP_ERR_NO_MEM, "call allocation failed", false);
        return;
    }
    cJSON_AddStringToObject(body, "call_type", "audio");
    int error = request(PLATFORM_DEVICE_CALL_REQUEST, body, true);
    cJSON_Delete(body);
    if (error) begin_end(error, "call request submission failed", false);
    else phase(XMINI_BUSINESS_FETCHING, 0, "");
}

static void handle_intent(const intent_item_t *item)
{
    if (item->session_bound && item->expected_generation != s_state.generation) {
        ++s_state.stale_events;
        publish();
        return;
    }
    if (s_state.maintenance) {
        ESP_LOGW(TAG, "intent rejected: maintenance owns device");
        return;
    }
    if (item->page_kind) {
        if (item->page_kind == 1) {
            ++s_contacts_selection;
            s_state.contacts_filter = item->filter;
            s_state.contacts_page = item->page;
            s_state.contacts_count = s_state.contacts_total = s_state.contacts_filtered_total = 0;
            s_state.contacts_loading = true;
            s_state.contacts_error = 0;
            clear_memory(s_state.contacts, sizeof(s_state.contacts));
            refresh_contacts();
        } else {
            ++s_pending_selection;
            s_state.pending_page = item->page;
            s_state.pending_count = s_state.pending_total = 0;
            s_state.pending_loading = true;
            s_state.pending_error = 0;
            clear_memory(s_state.pending, sizeof(s_state.pending));
            refresh_pending();
        }
        publish();
        return;
    }
    if (item->contact) {
        if (item->contact_action == XMINI_CONTACT_REFRESH) { refresh_contacts(); return; }
        if (!s_state.platform_online || s_contact_mutating) {
            metadata_result(ESP_ERR_INVALID_STATE); publish(); return;
        }
        cJSON *body = cJSON_CreateObject();
        platform_operation_t operation = PLATFORM_DEVICE_CONTACT_REQUEST;
        bool valid = body != NULL;
        if (item->contact_action == XMINI_CONTACT_REQUEST) {
            valid = valid && cJSON_AddStringToObject(body, "target_device_id", item->target);
        } else if (item->contact_action == XMINI_CONTACT_ACCEPT || item->contact_action == XMINI_CONTACT_REJECT) {
            operation = PLATFORM_DEVICE_CONTACT_RESPOND;
            valid = valid && cJSON_AddStringToObject(body, "peer_device_id", item->target) &&
                cJSON_AddStringToObject(body, "action", item->contact_action == XMINI_CONTACT_ACCEPT ? "accept" : "reject");
        } else if (item->contact_action == XMINI_CONTACT_REMARK) {
            operation = PLATFORM_DEVICE_CONTACT_REMARK;
            valid = valid && cJSON_AddStringToObject(body, "peer_id", item->target) &&
                cJSON_AddStringToObject(body, "remark", item->value ? item->value : "");
        } else if (item->contact_action == XMINI_CONTACT_DELETE) {
            operation = PLATFORM_DEVICE_CONTACT_DELETE;
            valid = valid && cJSON_AddStringToObject(body, "peer_id", item->target);
        }
        int error = valid ? request(operation, body, false) : ESP_ERR_NO_MEM;
        cJSON_Delete(body);
        if (!error) s_contact_mutating = true;
        metadata_result(error);
        publish();
        return;
    }
    if (item->group) {
        if (s_state.owner != XMINI_BUSINESS_GROUP) {
            if (s_state.phase != XMINI_BUSINESS_IDLE || !s_state.platform_online ||
                !s_state.rtc_online || !s_state.profile_ready) {
                ESP_LOGW(TAG, "group action rejected: busy/not online");
                return;
            }
            begin_owner(XMINI_BUSINESS_GROUP, true);
            s_state.tx_stream = 1;
            int opened = xmini_group_open(s_state.generation, s_device_id, now_ms());
            if (opened) { begin_end(opened, "group open failed", false); return; }
            phase(XMINI_BUSINESS_FETCHING, 0, "");
        }
        if (!s_ending) {
            int error = xmini_group_action(item->group_action, s_state.generation,
                                            item->room_code, item->password, now_ms());
            if (error) { s_state.error = error; publish(); }
        }
        return;
    }
    if (item->intent == XMINI_INTENT_HANGUP || item->intent == XMINI_INTENT_REJECT) {
        if (s_ending && s_state.owner == XMINI_BUSINESS_DEVICE && s_cleanup_failed && !s_action_pending) {
            s_cleanup_failed = false;
            if (s_state.room[0]) request_device_release(s_peer_answered ? XMINI_BUSINESS_ACTIVE :
                s_outgoing ? XMINI_BUSINESS_OUTGOING : XMINI_BUSINESS_RINGING);
            else reconcile_room(true);
            return;
        }
        if (s_state.owner != XMINI_BUSINESS_NONE) begin_end(0, "local hangup", true);
        return;
    }
    if (item->intent == XMINI_INTENT_REFRESH_CONTACTS) {
        if (!s_state.profile_ready) request_profile();
        refresh_contacts();
        return;
    }
    if (item->intent == XMINI_INTENT_DISMISS_ERROR) {
        if (s_state.phase == XMINI_BUSINESS_ERROR && s_state.owner == XMINI_BUSINESS_NONE && !s_ending) {
            s_final_error = 0;
            phase(XMINI_BUSINESS_IDLE, 0, "");
            arm_idle_listener();
        }
        return;
    }
    if (item->intent == XMINI_INTENT_ACCEPT && s_state.phase == XMINI_BUSINESS_RINGING &&
        s_state.owner == XMINI_BUSINESS_WECHAT) {
        connect_wechat();
        return;
    }
    if (item->intent == XMINI_INTENT_ACCEPT && s_state.phase == XMINI_BUSINESS_RINGING &&
        s_state.owner == XMINI_BUSINESS_DEVICE) {
        cJSON *body = room_body(NULL);
        if (body) { cJSON_AddStringToObject(body, "device_id", s_state.peer); cJSON_AddStringToObject(body, "purpose", "call"); }
        int error = body ? request(PLATFORM_DEVICE_CALL_INFO, body, true) : ESP_ERR_NO_MEM;
        cJSON_Delete(body);
        if (error) begin_end(error, "accept submission failed", true);
        else { s_deadline = now_ms() + 20000; phase(XMINI_BUSINESS_FETCHING, 0, ""); }
        return;
    }
    if ((item->intent == XMINI_INTENT_AI_INTERRUPT || item->intent == XMINI_INTENT_AI_SUBMIT_SPEECH) &&
        s_state.owner == XMINI_BUSINESS_AI && s_state.phase == XMINI_BUSINESS_ACTIVE) {
        int error = ai_notification(item->intent == XMINI_INTENT_AI_INTERRUPT ? "interrupt" : "submit_speech");
        if (item->intent == XMINI_INTENT_AI_INTERRUPT && s_callbacks.media_flush)
            s_callbacks.media_flush(s_state.generation, s_callbacks.context);
        if (error) begin_end(error, "AI command failed", true);
        return;
    }
    if (s_state.phase != XMINI_BUSINESS_IDLE || !s_state.platform_online ||
        !s_state.rtc_online || !s_state.profile_ready) {
        ESP_LOGW(TAG, "intent=%d rejected: phase=%d platform=%d rtc=%d profile=%d", item->intent,
                 s_state.phase, s_state.platform_online, s_state.rtc_online, s_state.profile_ready);
        return;
    }
    if ((item->intent == XMINI_INTENT_DEVICE_CALL || item->intent == XMINI_INTENT_WECHAT_CALL) &&
        (s_state.contacts_loading || s_contacts_pending || s_contacts_dirty)) {
        s_state.error = ESP_ERR_INVALID_STATE;
        publish();
        return;
    }
    if (item->intent == XMINI_INTENT_AI_START) {
        begin_owner(XMINI_BUSINESS_AI, true);
        int error = request(PLATFORM_AI_TOKEN, NULL, true);
        if (error) begin_end(error, "AI request submission failed", false);
        else phase(XMINI_BUSINESS_FETCHING, 0, "");
    } else if (item->intent == XMINI_INTENT_DEVICE_CALL) {
        bool contact = false;
        for (unsigned i = 0; i < s_state.contacts_count; ++i)
            if (!s_state.contacts[i].wechat && strcmp(s_state.contacts[i].id, item->target) == 0) contact = true;
        if (contact) start_device_call(item->target);
        else { s_final_error = ESP_ERR_NOT_FOUND; phase(XMINI_BUSINESS_ERROR, ESP_ERR_NOT_FOUND, "accepted contact required"); }
    } else if (item->intent == XMINI_INTENT_WECHAT_CALL) {
        const xmini_business_contact_t *contact = NULL;
        for (unsigned i = 0; i < s_state.contacts_count; ++i)
            if (s_state.contacts[i].wechat && strcmp(s_state.contacts[i].id, item->target) == 0) contact = &s_state.contacts[i];
        if (contact) start_wechat_call(contact);
        else { s_final_error = ESP_ERR_NOT_FOUND; phase(XMINI_BUSINESS_ERROR, ESP_ERR_NOT_FOUND, "authorized WeChat contact required"); }
    }
}

int xmini_business_init(const xmini_business_callbacks_t *callbacks)
{
    if (!callbacks || !callbacks->receive || !callbacks->media_start ||
        !callbacks->media_stop || !callbacks->media_busy || s_platform_queue)
        return ESP_ERR_INVALID_ARG;
    s_callbacks = *callbacks;
    s_owner_task = xTaskGetCurrentTaskHandle();
    s_platform_queue = xQueueCreateStatic(PLATFORM_SLOTS, sizeof(platform_item_t), s_platform_storage,
                                           &s_platform_queue_control);
    s_intent_queue = xQueueCreateStatic(INTENT_SLOTS, sizeof(intent_item_t), s_intent_storage,
                                         &s_intent_queue_control);
    if (!s_platform_queue || !s_intent_queue)
        return ESP_ERR_NO_MEM;
    int error = xmini_rtc_init(receive_audio, NULL);
    if (error)
        return error;
    xmini_group_callbacks_t group_callbacks = {.request = group_request, .connect = group_connect,
        .signal = group_signal, .set_profile = group_profile, .finish = group_finish, .random = group_random};
    error = xmini_group_init(&group_callbacks);
    if (error)
        return error;
    phase(XMINI_BUSINESS_IDLE, 0, "");
    return 0;
}

bool xmini_business_platform_event(const platform_event_t *event)
{
    if (!event || !s_platform_queue)
        return false;
    platform_item_t item = {.event = *event};
    if (event->json) {
        size_t length = strnlen(event->json, PLATFORM_BYTES);
        if (length >= PLATFORM_BYTES) { atomic_fetch_add(&s_dropped, 1); return false; }
        item.bytes = length + 1;
        size_t old = atomic_fetch_add(&s_platform_bytes, item.bytes);
        if (old + item.bytes > PLATFORM_BUDGET) {
            atomic_fetch_sub(&s_platform_bytes, item.bytes);
            atomic_fetch_add(&s_dropped, 1);
            return false;
        }
        item.json = malloc(item.bytes);
        if (!item.json) {
            atomic_fetch_sub(&s_platform_bytes, item.bytes);
            atomic_fetch_add(&s_dropped, 1);
            return false;
        }
        memcpy(item.json, event->json, item.bytes);
        item.event.json = item.json;
    }
    if (xQueueSend(s_platform_queue, &item, 0) == pdTRUE)
        return true;
    if (item.json) { clear_memory(item.json, item.bytes); free(item.json); atomic_fetch_sub(&s_platform_bytes, item.bytes); }
    atomic_fetch_add(&s_dropped, 1);
    return false;
}

static bool session_intent(xmini_business_intent_t intent)
{
    return intent == XMINI_INTENT_ACCEPT || intent == XMINI_INTENT_REJECT ||
        intent == XMINI_INTENT_HANGUP || intent == XMINI_INTENT_AI_INTERRUPT ||
        intent == XMINI_INTENT_AI_SUBMIT_SPEECH || intent == XMINI_INTENT_DISMISS_ERROR;
}

int xmini_business_post_session_intent(xmini_business_intent_t intent, uint32_t expected_generation)
{
    if (atomic_load(&s_maintenance)) return ESP_ERR_INVALID_STATE;
    if (!s_intent_queue || !session_intent(intent) ||
        (!expected_generation && intent != XMINI_INTENT_DISMISS_ERROR)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_snapshot_lock);
    bool current = s_snapshot.generation == expected_generation;
    portEXIT_CRITICAL(&s_snapshot_lock);
    if (!current) return ESP_ERR_INVALID_STATE;
    intent_item_t item = {.intent = intent, .session_bound = true, .expected_generation = expected_generation};
    return xQueueSend(s_intent_queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

int xmini_business_post_intent(xmini_business_intent_t intent, const char *target)
{
    if (atomic_load(&s_maintenance))
        return ESP_ERR_INVALID_STATE;
    if (!s_intent_queue || intent < XMINI_INTENT_AI_START || intent > XMINI_INTENT_DISMISS_ERROR ||
        (target && strlen(target) >= sizeof(((intent_item_t *)0)->target)))
        return ESP_ERR_INVALID_ARG;
    if (session_intent(intent)) {
        portENTER_CRITICAL(&s_snapshot_lock);
        uint32_t generation = s_snapshot.generation;
        portEXIT_CRITICAL(&s_snapshot_lock);
        if (!generation && intent != XMINI_INTENT_DISMISS_ERROR) return ESP_ERR_INVALID_STATE;
        return xmini_business_post_session_intent(intent, generation);
    }
    intent_item_t item = {.intent = intent};
    if (target) strcpy(item.target, target);
    return xQueueSend(s_intent_queue, &item, 0) == pdTRUE ? 0 : ESP_ERR_NO_MEM;
}

int xmini_business_post_contacts_page(xmini_contact_filter_t filter, unsigned page)
{
    if (atomic_load(&s_maintenance)) return ESP_ERR_INVALID_STATE;
    if (!s_intent_queue || filter < XMINI_CONTACT_ALL || filter > XMINI_CONTACT_WECHAT ||
        page > UINT_MAX / XMINI_BUSINESS_CONTACTS) return ESP_ERR_INVALID_ARG;
    intent_item_t item = {.page_kind = 1, .filter = filter, .page = page};
    return xQueueSend(s_intent_queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

int xmini_business_post_pending_page(unsigned page)
{
    if (atomic_load(&s_maintenance)) return ESP_ERR_INVALID_STATE;
    if (!s_intent_queue || page > UINT_MAX / 4) return ESP_ERR_INVALID_ARG;
    intent_item_t item = {.page_kind = 2, .page = page};
    return xQueueSend(s_intent_queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

int xmini_business_post_group(xmini_group_action_t action, const char *room_code, const char *password)
{
    if (atomic_load(&s_maintenance))
        return ESP_ERR_INVALID_STATE;
    if (!s_intent_queue || action < XMINI_GROUP_REFRESH || action > XMINI_GROUP_PTT_UP ||
        (room_code && strlen(room_code) >= 7) || (password && strlen(password) >= 65))
        return ESP_ERR_INVALID_ARG;
    if (action == XMINI_GROUP_PTT_UP) atomic_store(&s_ptt_wanted, false);
    intent_item_t item = {.group = true, .group_action = action};
    if (room_code) strcpy(item.room_code, room_code);
    if (password) strcpy(item.password, password);
    int error = xQueueSend(s_intent_queue, &item, 0) == pdTRUE ? 0 : ESP_ERR_NO_MEM;
    if (!error && action == XMINI_GROUP_PTT_DOWN) atomic_store(&s_ptt_wanted, true);
    return error;
}

int xmini_business_post_contact(xmini_contact_action_t action, const char *target, const char *value)
{
    if (atomic_load(&s_maintenance))
        return ESP_ERR_INVALID_STATE;
    if (!s_intent_queue || action < XMINI_CONTACT_REQUEST || action > XMINI_CONTACT_REFRESH ||
        (action != XMINI_CONTACT_REFRESH && (!target || !*target || strlen(target) >= 65)))
        return ESP_ERR_INVALID_ARG;
    intent_item_t item = {.contact = true, .contact_action = action};
    if (target) {
        if (strlen(target) >= sizeof(item.target)) return ESP_ERR_INVALID_SIZE;
        strcpy(item.target, target);
    }
    if (action == XMINI_CONTACT_REMARK && value) {
        size_t bytes = strnlen(value, 257), count = 0;
        if (bytes > 256) return ESP_ERR_INVALID_SIZE;
        for (size_t i = 0; i < bytes;) {
            unsigned char c = (unsigned char)value[i];
            size_t width = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 :
                           c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
            if (!width || i + width > bytes) return ESP_ERR_INVALID_ARG;
            for (size_t j = 1; j < width; ++j)
                if (((unsigned char)value[i + j] & 0xC0) != 0x80) return ESP_ERR_INVALID_ARG;
            if ((c == 0xE0 && (unsigned char)value[i + 1] < 0xA0) ||
                (c == 0xED && (unsigned char)value[i + 1] >= 0xA0) ||
                (c == 0xF0 && (unsigned char)value[i + 1] < 0x90) ||
                (c == 0xF4 && (unsigned char)value[i + 1] >= 0x90)) return ESP_ERR_INVALID_ARG;
            i += width;
            if (++count > 64) return ESP_ERR_INVALID_SIZE;
        }
        item.value = malloc(bytes + 1);
        if (!item.value) return ESP_ERR_NO_MEM;
        memcpy(item.value, value, bytes + 1);
    }
    int error = xQueueSend(s_intent_queue, &item, 0) == pdTRUE ? 0 : ESP_ERR_NO_MEM;
    if (error) release_json(&item.value);
    return error;
}

int xmini_business_maintenance(bool active)
{
    if (xTaskGetCurrentTaskHandle() != s_owner_task || !s_owner_task)
        return ESP_ERR_INVALID_STATE;
    if (active) {
        xmini_rtc_status_t rtc;
        xmini_rtc_status(&rtc);
        if (s_state.phase != XMINI_BUSINESS_IDLE || s_state.owner != XMINI_BUSINESS_NONE ||
            s_ending || s_action_pending || s_cleanup_pending || rtc.connecting || rtc.connected ||
            rtc.closing || rtc.stopping || rtc.faulted ||
            uxQueueMessagesWaiting(s_intent_queue))
            return ESP_ERR_INVALID_STATE;
        int error = xmini_rtc_arm_inbound(0, false);
        if (error)
            return error;
        xmini_rtc_status(&rtc);
        if (rtc.connecting || rtc.connected || rtc.closing)
            return ESP_ERR_INVALID_STATE;
    }
    s_state.maintenance = active;
    atomic_store(&s_maintenance, active);
    publish();
    if (!active)
        arm_idle_listener();
    return 0;
}

static void ending_tick(void)
{
    if (!s_ending)
        return;
    bool busy = s_callbacks.media_busy(s_state.generation, s_callbacks.context);
    if (busy && !s_stop_requested) {
        int error = s_callbacks.media_stop(s_state.generation, s_callbacks.context);
        s_stop_requested = true;
        if (error && !s_final_error) s_final_error = error;
    }
    xmini_rtc_status_t rtc;
    xmini_rtc_status(&rtc);
    if (!busy && s_wx_hangup_deferred) {
        static const char hangup[] = "{\"reason\":0}";
        s_wx_hangup_deferred = false;
        int sent = rtc.connected && rtc.generation == s_state.generation ?
            xmini_rtc_send_command(s_state.generation, HANGUP_COMMAND, hangup, sizeof(hangup) - 1) :
            TIRTC_E_INVALID_HANDLE;
        if (sent <= 0 && !s_final_error)
            s_final_error = sent ? sent : ESP_ERR_INVALID_RESPONSE;
    }
    if (!busy && (s_rtc_session || rtc.sdk_pending) && !s_disconnect_requested) {
        int error = xmini_rtc_cancel(s_state.generation);
        s_disconnect_requested = true;
        if (!error) s_rtc_session = true;
        if (error && error != TIRTC_E_INVALID_HANDLE) {
            if (!s_final_error) s_final_error = error;
            ESP_LOGE(TAG, "session cancel failed=%d", error);
        }
    }
    if (!busy && rtc_released() && !s_action_pending && !s_cleanup_pending) {
        if (s_release_deferred) request_device_release(s_release_previous);
        else if (s_reconcile_deferred) reconcile_room(s_reconcile_release);
        else if (s_state.owner == XMINI_BUSINESS_WECHAT) finish_wechat_reject();
    }
    if (!busy && rtc_released() && !s_action_pending && !s_cleanup_pending && !s_cleanup_failed &&
        !s_release_deferred && !s_reconcile_deferred && !s_wx_reject_deferred) {
        reset_session();
    } else if (s_ending_deadline && now_ms() >= s_ending_deadline) {
        s_ending_deadline = 0;
        if (!s_final_error) s_final_error = ESP_ERR_TIMEOUT;
        phase(XMINI_BUSINESS_ERROR, s_final_error, "cleanup not completed");
    }
}

static void handle_lifecycle(const platform_event_t *event)
{
    if (event->kind == PLATFORM_ONLINE) {
        bool reconnected = !s_state.platform_online;
        s_state.platform_online = true;
        s_platform_unbound = false;
        if (s_state.rtc_online) request_profile();
        if (s_ending && s_state.owner == XMINI_BUSINESS_DEVICE && s_cleanup_failed && !s_action_pending)
            reconcile_room(s_reconcile_release);
        if (reconnected && s_ending && s_state.owner == XMINI_BUSINESS_WECHAT &&
            s_wx_reject_deferred)
            s_wx_reject_attempted = false;
    } else {
        s_state.platform_online = false;
        s_state.profile_ready = false;
        s_state.contacts_loading = s_state.pending_loading = false;
        s_profile_pending = s_contacts_pending = s_pending_pending = s_contact_mutating = false;
        s_profile_deferred = s_pending_deferred = s_contacts_dirty = true;
        xmini_rtc_arm_inbound(0, false);
        if (s_state.owner != XMINI_BUSINESS_NONE) {
            begin_end(event->error ? event->error : ESP_ERR_INVALID_STATE, "platform offline", false);
            if (s_state.owner == XMINI_BUSINESS_DEVICE && s_state.room[0]) {
                s_cleanup_failed = true;
                s_reconcile_release = true;
            }
            if (s_state.owner == XMINI_BUSINESS_WECHAT && s_wx_room_json && !s_wx_room_joined) {
                defer_wechat_reject(1);
                s_cleanup_failed = true;
            }
        }
        if (event->kind == PLATFORM_UNBOUND) {
            s_platform_unbound = true;
            s_shutdown_sdk = true;
            s_state.contacts_count = s_state.contacts_total = 0;
            s_state.contacts_filtered_total = 0;
            s_state.contacts_filter = XMINI_CONTACT_ALL;
            s_state.contacts_page = s_state.pending_page = 0;
            s_state.contacts_error = s_state.pending_error = 0;
            ++s_contacts_selection;
            ++s_pending_selection;
            s_state.pending_count = s_state.pending_total = 0;
            clear_memory(s_state.contacts, sizeof(s_state.contacts));
            clear_memory(s_state.pending, sizeof(s_state.pending));
        }
    }
    publish();
}

void xmini_business_process(void)
{
    if (!s_platform_queue)
        return;
    platform_event_t lifecycle;
    for (unsigned i = 0; i < 3 && platform_take_lifecycle(&lifecycle); ++i)
        handle_lifecycle(&lifecycle);
    platform_event_t delivery_failure;
    while (platform_take_response_failure(&delivery_failure))
        handle_response(&delivery_failure);
    xmini_rtc_process();
    xmini_rtc_event_t rtc;
    for (unsigned i = 0; i < 8 && xmini_rtc_poll(&rtc); ++i) {
        handle_rtc(&rtc);
        xmini_rtc_event_release(&rtc);
    }
    platform_item_t item;
    for (unsigned i = 0; i < PLATFORM_SLOTS && xQueueReceive(s_platform_queue, &item, 0) == pdTRUE; ++i) {
        if (item.event.kind == PLATFORM_ONLINE || item.event.kind == PLATFORM_OFFLINE || item.event.kind == PLATFORM_UNBOUND) {
            handle_lifecycle(&item.event);
        } else if (item.event.kind == PLATFORM_RESPONSE) {
            handle_response(&item.event);
        } else if (item.event.kind == PLATFORM_MESSAGE && item.event.json) {
            if (item.event.epoch == platform_epoch()) handle_platform_message(item.event.json, item.event.epoch);
            else s_state.stale_events++;
        }
        if (item.json) { clear_memory(item.json, item.bytes); free(item.json); atomic_fetch_sub(&s_platform_bytes, item.bytes); }
        publish();
    }
    xmini_rtc_status_t status;
    xmini_rtc_status(&status);
    if (!s_state.maintenance && s_state.platform_online && !status.initialized &&
        !s_shutdown_sdk && !s_platform_unbound && !status.faulted) {
        platform_identity_t identity;
        platform_identity_snapshot(&identity);
        if (identity.device_id[0] && identity.device_key[0]) {
            snprintf(s_device_id, sizeof(s_device_id), "%s", identity.device_id);
            int error = xmini_rtc_start(identity.device_id, identity.device_key, platform_client_id(), platform_rtc_endpoint());
            clear_memory(&identity, sizeof(identity));
            if (error) { s_shutdown_sdk = true; phase(XMINI_BUSINESS_ERROR, error, "RTC startup failed"); }
        }
    }
    if (s_shutdown_sdk && !s_ending && status.initialized && !status.stopping) {
        int error = xmini_rtc_stop();
        if (error) phase(XMINI_BUSINESS_ERROR, error, "RTC shutdown failed");
    }
    intent_item_t intent;
    for (unsigned i = 0; i < INTENT_SLOTS && xQueueReceive(s_intent_queue, &intent, 0) == pdTRUE; ++i) {
        handle_intent(&intent);
        release_json(&intent.value);
        clear_memory(&intent, sizeof(intent));
    }
    media_tick();
    if (s_state.media_active && !s_ending &&
        !s_callbacks.media_busy(s_state.generation, s_callbacks.context))
        begin_end(ESP_ERR_INVALID_STATE, "active media stopped unexpectedly", true);
    if (!s_state.maintenance) xmini_group_tick(now_ms());
    xmini_group_status_t group = xmini_group_status();
    atomic_store(&s_group_transmit, group.active && group.speaking);
    if (s_state.owner == XMINI_BUSINESS_GROUP && !s_rtc_session &&
        (group.phase == XMINI_GROUP_ASSIGNED || group.phase == XMINI_GROUP_EMPTY))
        s_deadline = 0;
    if (s_state.group.revision != group.revision) { s_state.group = group; publish(); }
    ending_tick();
    if (!s_ending && s_deadline && now_ms() >= s_deadline && s_state.phase != XMINI_BUSINESS_ACTIVE)
        begin_end(ESP_ERR_TIMEOUT, "session timeout", true);
    if (metadata_idle()) {
        if (s_profile_deferred && !s_profile_pending) request_profile();
        else if (s_contacts_dirty && !s_contacts_pending && !s_profile_pending) refresh_contacts();
        else if (s_pending_deferred && !s_pending_pending && !s_contacts_pending && !s_profile_pending) refresh_pending();
    }
}

void xmini_business_status(xmini_business_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_snapshot_lock);
    *status = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

void xmini_business_command_diagnostics(xmini_business_command_diagnostics_t *diagnostics)
{
    if (!diagnostics) return;
    portENTER_CRITICAL(&s_snapshot_lock);
    *diagnostics = s_command_diagnostics;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

int xmini_business_send_audio(uint32_t generation, const uint8_t *pcma, size_t length, uint32_t pts)
{
    if (!atomic_load(&s_audio_enabled) || generation != atomic_load(&s_audio_generation))
        return TIRTC_E_INVALID_HANDLE;
    if (atomic_load(&s_group_audio) && (!atomic_load(&s_group_transmit) || !atomic_load(&s_ptt_wanted)))
        return TIRTC_E_INVALID_HANDLE;
    return xmini_rtc_enqueue_audio(generation, atomic_load(&s_audio_stream), pcma, length, pts);
}
