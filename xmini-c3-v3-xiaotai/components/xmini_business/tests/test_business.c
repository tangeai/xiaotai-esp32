#include "xmini_business.h"
#include "esp_app_desc.h"
#include "freertos/queue.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "../../demo_platform/platform_control_state.h"

static int64_t s_now;
static unsigned s_starts, s_stops, s_disconnects, s_flushes, s_media_starts;
static bool s_media_busy, s_prompt_busy;
static bool s_hold_retirement;
static unsigned s_wx_rejects, s_wx_reject_reason;
static int s_wx_reject_result;
static uint32_t s_wx_reject_generation;
static char s_wx_reject_room[129];
static unsigned s_wx_hangups;
static int s_next_wx_hangup_error;
static uint32_t s_media_generation;
static xmini_rtc_status_t s_rtc;
static xmini_rtc_event_t s_events[32];
static unsigned s_head, s_count;
static struct { platform_operation_t operation; uint32_t generation; unsigned epoch; char body[2048]; } s_requests[256];
static unsigned s_request_count;
static esp_err_t s_next_request_error;
static char s_command[2048];
static unsigned s_command_code;
static unsigned s_group_signals;
static xmini_rtc_receiver_t s_receiver;
static unsigned s_platform_epoch;
static platform_lifecycle_pending_t s_lifecycle;
static QueueHandle_t s_test_intent_queue;
static bool s_hold_test_intents;
static bool s_advance_epoch_on_cleanup;
static bool s_fail_cleanup_copy, s_track_cleanup_copy;
static void *s_cleanup_copy;
static unsigned s_cleanup_copy_frees;
static uint32_t s_inbound_generation;
static unsigned s_media_stops;
static int s_next_media_stop_error;
static uint32_t s_subscription_generation;
static uint8_t s_subscription_stream;
static unsigned s_rx_select_calls, s_audio_allow_calls, s_audio_order;
static unsigned s_rx_select_order, s_audio_allow_order, s_audio_start_order;
static uint32_t s_rx_selected_generation;
static uint8_t s_rx_selected_stream;
static int s_next_rx_select_error;
static int s_next_subscribe_error;
static unsigned s_device_hangups;
static bool s_device_send_override;
static int s_device_send_result;

void *__real_malloc(size_t size);
void __real_free(void *pointer);
void *__wrap_malloc(size_t size)
{
    if (s_track_cleanup_copy && size == sizeof("release-race")) {
        s_track_cleanup_copy = false;
        if (s_fail_cleanup_copy) return NULL;
        void *pointer = __real_malloc(size);
        assert(pointer && !s_cleanup_copy);
        s_cleanup_copy = pointer;
        return pointer;
    }
    return __real_malloc(size);
}
void __wrap_free(void *pointer)
{
    if (pointer && pointer == s_cleanup_copy) {
        s_cleanup_copy = NULL;
        s_cleanup_copy_frees++;
    }
    __real_free(pointer);
}

QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t size, uint8_t *storage, StaticQueue_t *queue)
{
    *queue = (StaticQueue_t){.capacity = capacity, .size = size, .data = storage};
    if (capacity == 8) s_test_intent_queue = queue;
    return queue;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks)
{
    assert(!ticks);
    if (queue->count == queue->capacity) return pdFALSE;
    memcpy(queue->data + ((queue->head + queue->count) % queue->capacity) * queue->size, item, queue->size);
    queue->count++; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks)
{
    assert(!ticks);
    if (s_hold_test_intents && queue == s_test_intent_queue) return pdFALSE;
    if (!queue->count) return pdFALSE;
    memcpy(item, queue->data + queue->head * queue->size, queue->size);
    queue->head = (queue->head + 1) % queue->capacity; queue->count--; return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t queue) { queue->head = queue->count = 0; return pdTRUE; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->count; }
int64_t esp_timer_get_time(void) { return s_now * 1000; }
void *xTaskGetCurrentTaskHandle(void) { return (void *)1; }
uint32_t esp_random(void) { return 0x12345678; }
void test_log(const char *level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; }
const esp_app_desc_t *esp_app_get_description(void)
{ static const esp_app_desc_t description = {.version = "0.1.0-test"}; return &description; }

static void rtc_event(xmini_rtc_event_kind_t kind, uint32_t generation, unsigned command, const char *json)
{
    assert(s_count < 32);
    xmini_rtc_event_t *event = &s_events[(s_head + s_count++) % 32];
    *event = (xmini_rtc_event_t){.kind = kind, .generation = generation, .command = command};
    if (json) { assert(strlen(json) < sizeof(event->data)); strcpy(event->data, json); event->length = strlen(json); }
}
int xmini_rtc_init(xmini_rtc_receiver_t receiver, void *context) { (void)context; s_receiver = receiver; return 0; }
int xmini_rtc_start(const char *id, const char *key, const char *client, const char *endpoint)
{
    assert(!strcmp(id, "device-a") && !strcmp(key, "test-key") && !strcmp(client, "stable-client"));
    assert(!strcmp(endpoint, "http://sample.invalid"));
    s_rtc.initialized = s_rtc.online = true; s_starts++; rtc_event(XMINI_RTC_STARTED, 0, 0, NULL); return 0;
}
int xmini_rtc_arm_inbound(uint32_t generation, bool accept)
{ s_inbound_generation = accept ? generation : 0; return 0; }
int xmini_rtc_connect(xmini_rtc_mode_t mode, uint32_t generation, const char *peer, const char *token)
{
    assert(mode == XMINI_RTC_WHIP || mode == XMINI_RTC_P2P);
    assert(*peer && *token && generation);
    s_rtc.generation = generation; s_rtc.connecting = true;
    s_rtc.subscribed = s_rtc.business_permit = 0;
    s_rx_selected_generation = 0;
    return 0;
}
int xmini_rtc_cancel(uint32_t generation)
{
    assert(generation == s_rtc.generation);
    assert(!s_media_busy); /* Hardware ownership must be retired first. */
    s_disconnects++;
    if (s_hold_retirement) { s_rtc.closing = true; return 0; }
    s_rtc.connecting = s_rtc.connected = false;
    rtc_event(XMINI_RTC_CLOSED, generation, 0, NULL); return 0;
}
int xmini_rtc_disconnect(uint32_t generation) { return xmini_rtc_cancel(generation); }
void xmini_rtc_process(void) {}
bool xmini_rtc_poll(xmini_rtc_event_t *event)
{
    if (!s_count) return false;
    *event = s_events[s_head]; s_head = (s_head + 1) % 32; s_count--; return true;
}
const char *xmini_rtc_event_payload(const xmini_rtc_event_t *event) { return event->data; }
void xmini_rtc_event_release(xmini_rtc_event_t *event) { assert(!event->payload); }
int xmini_rtc_send_command(uint32_t generation, uint32_t command, const void *data, size_t length)
{
    assert(generation && length < sizeof(s_command));
    memcpy(s_command, data, length); s_command[length] = 0; s_command_code = command;
    xmini_business_status_t state;
    xmini_business_status(&state);
    if (state.owner == XMINI_BUSINESS_DEVICE && command == 0x2001) {
        ++s_device_hangups;
        if (s_device_send_override) return s_device_send_result;
        if (!s_rtc.connected || generation != s_rtc.generation) return TIRTC_E_INVALID_HANDLE;
    }
    if (state.owner == XMINI_BUSINESS_WECHAT && command == 0x2001) {
        assert(s_rtc.connected && generation == s_rtc.generation && !s_media_busy);
        assert(!strcmp(s_command, "{\"reason\":0}"));
        ++s_wx_hangups;
        if (s_next_wx_hangup_error) {
            int error = s_next_wx_hangup_error;
            s_next_wx_hangup_error = 0;
            return error;
        }
    }
    cJSON *json = cJSON_Parse(s_command); assert(json); cJSON_Delete(json); return (int)length;
}
int xmini_rtc_subscribe(uint32_t generation, uint8_t stream, bool subscribe)
{
    assert(generation && stream <= 15 && subscribe);
    s_subscription_generation = generation;
    s_subscription_stream = stream;
    int error = s_next_subscribe_error;
    s_next_subscribe_error = 0;
    return error;
}
int xmini_rtc_select_audio(uint32_t generation, uint8_t stream, bool selected)
{
    assert(generation && stream <= 15);
    ++s_rx_select_calls;
    s_rx_select_order = ++s_audio_order;
    if (generation != s_rtc.generation || !s_rtc.connected) return TIRTC_E_INVALID_HANDLE;
    if (s_next_rx_select_error) {
        int error = s_next_rx_select_error;
        s_next_rx_select_error = 0;
        return error;
    }
    if (selected) {
        s_rx_selected_generation = generation;
        s_rx_selected_stream = stream;
    } else if (s_rx_selected_generation == generation && s_rx_selected_stream == stream) {
        s_rx_selected_generation = 0;
    }
    return 0;
}
int xmini_rtc_authorize_audio(uint32_t generation, uint8_t stream, bool allow)
{
    assert(generation && stream <= 15);
    if (allow) { ++s_audio_allow_calls; s_audio_allow_order = ++s_audio_order; }
    if (generation != s_rtc.generation || (allow && !s_rtc.connected)) return TIRTC_E_INVALID_HANDLE;
    if (allow) s_rtc.business_permit |= (uint16_t)(1U << stream);
    else s_rtc.business_permit &= (uint16_t)~(1U << stream);
    return 0;
}
int xmini_rtc_service_request(const char *path, const char *json, TIRTCSERVICEREQUESTCALLBACK callback, void *context)
{
    (void)callback; (void)context;
    assert(!strcmp(path, "/v1/wxvoip/reject"));
    assert(!s_rtc.connected && !s_rtc.connecting && !s_rtc.closing && !s_rtc.sdk_pending && !s_media_busy);
    xmini_business_status_t state;
    xmini_business_status(&state);
    assert(state.owner == XMINI_BUSINESS_WECHAT && state.generation && state.room[0]);
    cJSON *body = cJSON_Parse(json); assert(body);
    assert(!strcmp(cJSON_GetObjectItem(body, "wx_room_id")->valuestring, state.room));
    assert(!strcmp(cJSON_GetObjectItem(body, "wx_session_token")->valuestring, "secret"));
    s_wx_reject_reason = (unsigned)cJSON_GetObjectItem(body, "hangup_reason")->valueint;
    s_wx_reject_generation = state.generation;
    strcpy(s_wx_reject_room, state.room);
    cJSON_Delete(body);
    ++s_wx_rejects;
    return s_wx_reject_result;
}
int xmini_rtc_enqueue_audio(uint32_t generation, uint8_t stream, const uint8_t *pcma, size_t length, uint32_t pts)
{
    (void)pts;
    assert(generation == s_rtc.generation && stream <= 15 && pcma && length == 160);
    return ((s_rtc.subscribed | s_rtc.business_permit) & (1U << stream)) ? 0 : TIRTC_E_INVALID_HANDLE;
}
int xmini_rtc_stop(void) { s_stops++; s_rtc.online = false; rtc_event(XMINI_RTC_STOPPED, 0, 0, NULL); return 0; }
int xmini_rtc_uninit(void) { s_rtc.initialized = false; return 0; }
void xmini_rtc_status(xmini_rtc_status_t *status) { *status = s_rtc; }

esp_err_t platform_request(platform_operation_t operation, uint32_t generation, const char *json)
{
    if (s_next_request_error) {
        esp_err_t error = s_next_request_error;
        s_next_request_error = ESP_OK;
        return error;
    }
    if (operation == PLATFORM_DEVICE_CALL_CANCEL || operation == PLATFORM_DEVICE_CALL_REJECT ||
        operation == PLATFORM_DEVICE_CALL_HANGUP || operation == PLATFORM_DEVICE_CALL_ROOM)
        assert(!s_rtc.connected && !s_rtc.connecting && !s_rtc.closing && !s_rtc.sdk_pending && !s_media_busy);
    if (s_advance_epoch_on_cleanup && operation == PLATFORM_DEVICE_CALL_HANGUP) {
        s_advance_epoch_on_cleanup = false;
        ++s_platform_epoch;
    }
    assert(s_request_count < 256);
    s_requests[s_request_count].operation = operation;
    s_requests[s_request_count].generation = generation;
    s_requests[s_request_count].epoch = s_platform_epoch;
    if (json) {
        assert(strlen(json) < sizeof(s_requests[0].body));
        strcpy(s_requests[s_request_count].body, json);
        cJSON *body = cJSON_Parse(json); assert(cJSON_IsObject(body)); cJSON_Delete(body);
    }
    ++s_request_count; return 0;
}
esp_err_t platform_request_epoch(platform_operation_t operation, uint32_t generation,
                                 const char *json, unsigned *accepted_epoch)
{
    esp_err_t error = platform_request(operation, generation, json);
    if (!error && accepted_epoch) *accepted_epoch = s_requests[s_request_count - 1].epoch;
    return error;
}
bool platform_take_response_failure(platform_event_t *event) { (void)event; return false; }
unsigned platform_epoch(void) { return s_platform_epoch; }
bool platform_take_lifecycle(platform_event_t *event) { return platform_lifecycle_take(&s_lifecycle, event); }
void platform_identity_snapshot(platform_identity_t *identity)
{ *identity = (platform_identity_t){.device_id = "device-a", .device_key = "test-key"}; }
const char *platform_client_id(void) { return "stable-client"; }
const char *platform_rtc_endpoint(void) { return "http://sample.invalid"; }

int xmini_group_init(const xmini_group_callbacks_t *callbacks) { assert(callbacks); return 0; }
int xmini_group_open(uint32_t generation, const char *id, int64_t now) { (void)generation; (void)id; (void)now; return 0; }
int xmini_group_action(xmini_group_action_t action, uint32_t generation, const char *code, const char *password, int64_t now)
{ (void)action; (void)generation; (void)code; (void)password; (void)now; return 0; }
void xmini_group_on_http(platform_operation_t operation, uint32_t ticket, int error, const char *json, int64_t now)
{ (void)operation; (void)ticket; (void)error; (void)json; (void)now; }
void xmini_group_on_connected(uint32_t generation, int error, int64_t now) { (void)generation; (void)error; (void)now; }
void xmini_group_on_signal(uint32_t generation, const char *json, size_t length, int64_t now)
{ (void)now; assert(generation && json && length == 2 && !memcmp(json, "{}", 2)); ++s_group_signals; }
void xmini_group_on_closed(uint32_t generation, int error, int64_t now) { (void)generation; (void)error; (void)now; }
void xmini_group_suspend(uint32_t generation, int error, int64_t now) { (void)generation; (void)error; (void)now; }
void xmini_group_assignment_changed(int64_t now) { (void)now; }
void xmini_group_tick(int64_t now) { (void)now; }
xmini_group_status_t xmini_group_status(void) { return (xmini_group_status_t){0}; }

static bool receive(uint32_t generation, uint8_t stream, uint32_t pts, const uint8_t *pcma, size_t length, void *context)
{ (void)generation; (void)stream; (void)pts; (void)pcma; (void)length; (void)context; return true; }
static int media_start(uint32_t generation, uint8_t stream, void *context)
{
    (void)context; assert(generation && stream <= 15);
    if (s_prompt_busy) return ESP_ERR_INVALID_STATE;
    s_audio_start_order = ++s_audio_order;
    s_media_generation = generation; s_media_busy = true; s_media_starts++; return 0;
}
static int media_stop(uint32_t generation, void *context)
{
    (void)context; assert(generation == s_media_generation);
    ++s_media_stops;
    int error = s_next_media_stop_error;
    s_next_media_stop_error = 0;
    return error;
}
static bool media_busy(uint32_t generation, void *context)
{ (void)context; return generation == s_media_generation && s_media_busy; }
static void media_flush(uint32_t generation, void *context)
{ (void)context; assert(generation == s_media_generation); s_flushes++; }
static xmini_business_status_t status(void)
{ xmini_business_status_t state; xmini_business_status(&state); return state; }
static void platform_event(platform_event_kind_t kind, platform_operation_t operation, uint32_t generation,
                             int error, const char *json)
{
    platform_event_t event = {.kind = kind, .operation = operation, .generation = generation,
        .epoch = s_platform_epoch, .error = error, .json = json};
    assert(xmini_business_platform_event(&event)); xmini_business_process();
}
static void response(platform_operation_t operation, uint32_t generation, const char *json)
{ platform_event(PLATFORM_RESPONSE, operation, generation, 0, json); }
static void intent(xmini_business_intent_t action, const char *target)
{ assert(xmini_business_post_intent(action, target) == 0); xmini_business_process(); }
static void connected(uint32_t generation)
{ s_rtc.connected = true; s_rtc.connecting = false; rtc_event(XMINI_RTC_CONNECTED, generation, 0, NULL); xmini_business_process(); }

static void settle_default_contacts(void)
{
    /* A callers_update/mutation during a GET schedules one newer snapshot. */
    for (unsigned i = 0; i < 2; ++i) {
        response(PLATFORM_DEVICE_CONTACTS, 0,
            "{\"code\":200,\"data\":{\"contacts\":[{\"type\":\"device\",\"device_id\":\"device-b\"},"
            "{\"type\":\"voip\",\"device_id\":\"openid\",\"wx_app_id\":\"wxapp\"}]}}");
        response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, "{\"code\":200,\"data\":{\"pending\":[]}}");
    }
    assert(!status().contacts_loading);
}

static void test_session_intent_generation(void)
{
    platform_event(PLATFORM_MESSAGE, 0, 0, 0,
        "{\"type\":\"call_incoming\",\"channel\":\"device\",\"payload\":{\"room_id\":\"intent-a\",\"caller_id\":\"device-b\"}}");
    uint32_t generation = status().generation;
    assert(status().phase == XMINI_BUSINESS_RINGING && generation);
    const xmini_business_intent_t actions[] = {XMINI_INTENT_ACCEPT, XMINI_INTENT_REJECT,
        XMINI_INTENT_HANGUP, XMINI_INTENT_AI_INTERRUPT, XMINI_INTENT_AI_SUBMIT_SPEECH, XMINI_INTENT_DISMISS_ERROR};
    s_hold_test_intents = true;
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
        assert(xmini_business_post_intent(actions[i], NULL) == ESP_OK);
    /* Models an enqueue after this turn's intent drain, before retirement. */
    platform_event(PLATFORM_MESSAGE, 0, 0, 0,
        "{\"type\":\"room_cancel\",\"channel\":\"device\",\"payload\":{\"room_id\":\"intent-a\"}}");
    assert(status().phase == XMINI_BUSINESS_IDLE);
    platform_event(PLATFORM_MESSAGE, 0, 0, 0,
        "{\"type\":\"call_incoming\",\"channel\":\"device\",\"payload\":{\"room_id\":\"intent-b\",\"caller_id\":\"device-b\"}}");
    assert(status().phase == XMINI_BUSINESS_RINGING && status().generation != generation);
    assert(xmini_business_post_session_intent(XMINI_INTENT_ACCEPT, generation) == ESP_ERR_INVALID_STATE);
    assert(xmini_business_post_session_intent(XMINI_INTENT_ACCEPT, 0) == ESP_ERR_INVALID_ARG);
    assert(xmini_business_post_session_intent(XMINI_INTENT_AI_START, status().generation) == ESP_ERR_INVALID_ARG);
    unsigned requests = s_request_count, stale = status().stale_events;
    s_hold_test_intents = false;
    xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_RINGING && !strcmp(status().room, "intent-b"));
    assert(s_request_count == requests && status().stale_events == stale + 6);
    generation = status().generation;
    assert(xmini_business_post_session_intent(XMINI_INTENT_REJECT, generation) == ESP_OK);
    xmini_business_process();
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CALL_REJECT);
    response(PLATFORM_DEVICE_CALL_REJECT, generation, "{\"code\":200}");
    assert(status().phase == XMINI_BUSINESS_IDLE);
}

static void test_group_command_words(void)
{
    assert(xmini_business_post_group(XMINI_GROUP_REFRESH, NULL, NULL) == 0);
    xmini_business_process();
    uint32_t generation = status().generation;
    assert(status().owner == XMINI_BUSINESS_GROUP && generation);
    const uint32_t accepted[] = {0x2200, 0x2201, MAKE_CMDW(0x2200, 0),
        MAKE_CMDW(0x2200, 0xffff), MAKE_CMDW(0x2200, 7) | RESPONSE_BIT, 0xabcd2200};
    for (unsigned i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        rtc_event(XMINI_RTC_COMMAND, generation, accepted[i], "{}"); xmini_business_process();
        assert(s_group_signals == i + 1);
    }
    unsigned count = s_group_signals;
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2001, "{}");
    rtc_event(XMINI_RTC_COMMAND, generation, MAKE_CMDW(0x2100, 7), "{}");
    xmini_business_process();
    assert(s_group_signals == count && status().owner == XMINI_BUSINESS_GROUP);
    intent(XMINI_INTENT_HANGUP, NULL);
    assert(status().phase == XMINI_BUSINESS_IDLE);
}

static uint32_t wechat_incoming(const char *room)
{
    char json[512];
    snprintf(json, sizeof(json),
        "{\"type\":\"call_incoming\",\"channel\":\"wx\",\"payload\":{\"wx_room_id\":\"%s\","
        "\"wx_from\":\"phone\",\"wx_user_openid\":\"openid\",\"wx_app_id\":\"wxapp\","
        "\"peer_id\":\"whips://wx\",\"token\":\"token\",\"wx_model_id\":\"model\",\"wx_server_token\":\"secret\"}}", room);
    platform_event(PLATFORM_MESSAGE, 0, 0, 0, json);
    assert(status().phase == XMINI_BUSINESS_RINGING);
    return status().generation;
}

static void wechat_retired(uint32_t generation)
{
    s_rtc.connecting = s_rtc.connected = s_rtc.closing = false;
    s_rtc.sdk_pending = 0;
    s_hold_retirement = false;
    rtc_event(XMINI_RTC_RETIRED, generation, 0, NULL);
    xmini_business_process();
}

static void restore_metadata(void)
{
    response(PLATFORM_PROFILE, 0, "{\"code\":200}");
    settle_default_contacts();
}

static void test_wechat_reject_retirement(void)
{
    unsigned rejected = s_wx_rejects;
    uint32_t generation = wechat_incoming("wx-decline");
    intent(XMINI_INTENT_REJECT, NULL);
    assert(s_wx_rejects == ++rejected && s_wx_reject_reason == 7);
    assert(s_wx_reject_generation == generation && !strcmp(s_wx_reject_room, "wx-decline"));
    assert(status().phase == XMINI_BUSINESS_IDLE);

    generation = wechat_incoming("wx-pending-cancel");
    intent(XMINI_INTENT_ACCEPT, NULL);
    assert(status().phase == XMINI_BUSINESS_CONNECTING);
    s_hold_retirement = true; s_rtc.sdk_pending = 1;
    s_media_generation = generation; s_media_busy = true;
    unsigned disconnected = s_disconnects;
    intent(XMINI_INTENT_HANGUP, NULL);
    assert(status().phase == XMINI_BUSINESS_ENDING && s_disconnects == disconnected);
    assert(s_wx_rejects == rejected);
    s_media_busy = false;
    xmini_business_process();
    assert(s_disconnects == disconnected + 1 && s_rtc.closing);
    intent(XMINI_INTENT_HANGUP, NULL);
    intent(XMINI_INTENT_REJECT, NULL);
    assert(s_wx_rejects == rejected && s_disconnects == disconnected + 1);
    s_rtc.connecting = s_rtc.closing = false;
    rtc_event(XMINI_RTC_RETIRED, generation, 0, NULL); xmini_business_process();
    assert(s_rtc.sdk_pending == 1 && s_wx_rejects == rejected);
    wechat_retired(generation);
    assert(s_wx_rejects == ++rejected && s_wx_reject_reason == 1);
    assert(s_wx_reject_generation == generation && !strcmp(s_wx_reject_room, "wx-pending-cancel"));
    assert(status().phase == XMINI_BUSINESS_IDLE);

    generation = wechat_incoming("wx-connect-failed-retired");
    intent(XMINI_INTENT_ACCEPT, NULL);
    s_rtc.connecting = false;
    rtc_event(XMINI_RTC_CONNECTED, generation, 0, NULL);
    xmini_rtc_event_t *failed = &s_events[(s_head + s_count - 1) % 32];
    failed->error = TIRTC_E_TIMEOUTED; failed->retired = true;
    xmini_business_process();
    assert(s_wx_rejects == ++rejected && s_wx_reject_generation == generation);
    assert(status().owner == XMINI_BUSINESS_NONE && status().phase == XMINI_BUSINESS_ERROR);
    assert(status().error == TIRTC_E_TIMEOUTED);
    intent(XMINI_INTENT_DISMISS_ERROR, NULL);

    generation = wechat_incoming("wx-offline-pending");
    intent(XMINI_INTENT_ACCEPT, NULL);
    s_hold_retirement = true; s_rtc.sdk_pending = 1;
    intent(XMINI_INTENT_HANGUP, NULL);
    platform_event(PLATFORM_OFFLINE, 0, 0, ESP_ERR_INVALID_STATE, NULL);
    platform_event(PLATFORM_ONLINE, 0, 0, 0, NULL);
    assert(s_wx_rejects == rejected && status().owner == XMINI_BUSINESS_WECHAT);
    assert(status().generation == generation && !strcmp(status().room, "wx-offline-pending"));
    wechat_retired(generation);
    assert(s_wx_rejects == ++rejected && s_wx_reject_generation == generation);
    assert(status().phase == XMINI_BUSINESS_IDLE);
    restore_metadata();

    generation = wechat_incoming("wx-reject-failed");
    s_wx_reject_result = TIRTC_E_LACK_OF_RESOURCE;
    intent(XMINI_INTENT_REJECT, NULL);
    assert(s_wx_rejects == ++rejected && s_wx_reject_reason == 7);
    assert(status().phase == XMINI_BUSINESS_ERROR && status().owner == XMINI_BUSINESS_WECHAT);
    assert(status().error == TIRTC_E_LACK_OF_RESOURCE && status().generation == generation);
    assert(!strcmp(status().room, "wx-reject-failed"));
    intent(XMINI_INTENT_HANGUP, NULL);
    intent(XMINI_INTENT_REJECT, NULL);
    intent(XMINI_INTENT_DISMISS_ERROR, NULL);
    platform_event(PLATFORM_ONLINE, 0, 0, 0, NULL); /* A duplicate ONLINE is not a recovery edge. */
    for (unsigned i = 0; i < 20; ++i) xmini_business_process();
    assert(s_wx_rejects == rejected && status().owner == XMINI_BUSINESS_WECHAT);
    s_rtc.generation = generation; s_rtc.sdk_pending = 1; s_hold_retirement = true;
    platform_event(PLATFORM_OFFLINE, 0, 0, ESP_ERR_INVALID_STATE, NULL);
    platform_event(PLATFORM_ONLINE, 0, 0, 0, NULL);
    assert(s_wx_rejects == rejected && status().error == TIRTC_E_LACK_OF_RESOURCE);
    s_wx_reject_result = 0;
    wechat_retired(generation);
    assert(s_wx_rejects == ++rejected && s_wx_reject_reason == 7);
    assert(status().owner == XMINI_BUSINESS_NONE && status().phase == XMINI_BUSINESS_ERROR);
    assert(status().error == TIRTC_E_LACK_OF_RESOURCE);
    intent(XMINI_INTENT_DISMISS_ERROR, NULL);
    restore_metadata();
}

static uint32_t wechat_join(const char *room)
{
    uint32_t generation = wechat_incoming(room);
    intent(XMINI_INTENT_ACCEPT, NULL);
    s_rtc.connecting = false;
    s_rtc.connected = true;
    rtc_event(XMINI_RTC_CONNECTED, generation, 0, NULL);
    xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_SESSION && !s_media_busy);
    return generation;
}

static void test_wechat_joined_hangup(void)
{
    unsigned rejected = s_wx_rejects, hungup = s_wx_hangups;
    uint32_t generation = wechat_join("wx-joined-not-answered");
    s_hold_retirement = true;
    intent(XMINI_INTENT_HANGUP, NULL);
    assert(s_wx_hangups == ++hungup && s_wx_rejects == rejected);
    assert(s_command_code == 0x2001 && s_rtc.closing);
    intent(XMINI_INTENT_HANGUP, NULL);
    intent(XMINI_INTENT_REJECT, NULL);
    assert(s_wx_hangups == hungup && s_wx_rejects == rejected);
    wechat_retired(generation);
    assert(status().phase == XMINI_BUSINESS_IDLE && s_wx_rejects == rejected);

    generation = wechat_incoming("wx-late-join-before-cancel");
    intent(XMINI_INTENT_ACCEPT, NULL);
    s_media_generation = generation;
    s_media_busy = true;
    intent(XMINI_INTENT_HANGUP, NULL);
    s_rtc.connecting = false;
    s_rtc.connected = true;
    rtc_event(XMINI_RTC_CONNECTED, generation, 0, NULL);
    xmini_business_process();
    assert(s_wx_hangups == hungup && s_wx_rejects == rejected);
    s_media_busy = false;
    xmini_business_process();
    xmini_business_process();
    assert(s_wx_hangups == ++hungup && s_wx_rejects == rejected);
    assert(status().phase == XMINI_BUSINESS_IDLE);

    generation = wechat_join("wx-active-stop-before-hangup");
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, "{}");
    xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ACTIVE && s_media_busy);
    unsigned disconnects = s_disconnects;
    intent(XMINI_INTENT_HANGUP, NULL);
    assert(s_wx_hangups == hungup && s_disconnects == disconnects);
    s_media_busy = false;
    xmini_business_process();
    xmini_business_process();
    assert(s_wx_hangups == ++hungup && s_disconnects == disconnects + 1);
    assert(status().phase == XMINI_BUSINESS_IDLE && s_wx_rejects == rejected);

    generation = wechat_join("wx-peer-closed");
    s_rtc.connected = false;
    rtc_event(XMINI_RTC_CLOSED, generation, 0, NULL);
    xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_IDLE);
    assert(s_wx_hangups == hungup && s_wx_rejects == rejected);

    (void)wechat_join("wx-hangup-send-failed");
    s_next_wx_hangup_error = TIRTC_E_LACK_OF_RESOURCE;
    intent(XMINI_INTENT_HANGUP, NULL);
    xmini_business_process();
    assert(s_wx_hangups == ++hungup && s_wx_rejects == rejected);
    assert(status().phase == XMINI_BUSINESS_ERROR && status().owner == XMINI_BUSINESS_NONE);
    assert(status().error == TIRTC_E_LACK_OF_RESOURCE);
    intent(XMINI_INTENT_DISMISS_ERROR, NULL);
}

static char *contact_fixture(bool pending)
{
    cJSON *root = cJSON_CreateObject(), *data = cJSON_AddObjectToObject(root, "data");
    cJSON_AddNumberToObject(root, "code", 200);
    cJSON *entries = cJSON_AddArrayToObject(data, pending ? "pending" : "contacts");
    for (unsigned i = 0; i < (pending ? 9 : 20); ++i) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddItemToArray(entries, entry);
        char id[65];
        snprintf(id, sizeof(id), "%s-%u", pending ? "pending" : i < 10 ? "device" : "wx", i < 10 || pending ? i : i - 10);
        cJSON_AddStringToObject(entry, pending ? "peer_device_id" : "device_id", id);
        cJSON_AddStringToObject(entry, "type", !pending && i >= 10 ? "voip" : "device");
        if (!pending && i >= 10) {
            snprintf(id, sizeof(id), "authorized-app-%u", i - 10);
            cJSON_AddStringToObject(entry, "wx_app_id", id);
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    assert(json && strlen(json) < PLATFORM_JSON_CAPACITY);
    cJSON_Delete(root);
    return json;
}

static void test_contact_pages(void)
{
    char *contacts = contact_fixture(false), *pending = contact_fixture(true);
    assert(status().phase == XMINI_BUSINESS_IDLE);
    assert(xmini_business_post_contacts_page((xmini_contact_filter_t)99, 0) == ESP_ERR_INVALID_ARG);
    assert(xmini_business_post_contacts_page(XMINI_CONTACT_ALL, UINT32_MAX) == ESP_ERR_INVALID_ARG);
    assert(xmini_business_post_pending_page(UINT32_MAX) == ESP_ERR_INVALID_ARG);
    assert(xmini_business_post_contacts_page(XMINI_CONTACT_WECHAT, 1) == ESP_OK);
    xmini_business_process();
    assert(status().contacts_loading && !status().contacts_count);
    unsigned requests = s_request_count;
    intent(XMINI_INTENT_WECHAT_CALL, "openid");
    assert(s_request_count == requests && status().phase == XMINI_BUSINESS_IDLE);
    response(PLATFORM_DEVICE_CONTACTS, 0, contacts);
    assert(!status().contacts_loading && status().contacts_total == 20 && status().contacts_filtered_total == 10);
    assert(status().contacts_page == 1 && status().contacts_count == 2);
    assert(!strcmp(status().contacts[0].id, "wx-8") && !strcmp(status().contacts[0].app_id, "authorized-app-8"));
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    intent(XMINI_INTENT_WECHAT_CALL, "wx-8");
    uint32_t generation = status().generation;
    assert(s_requests[s_request_count - 1].operation == PLATFORM_CALL);
    cJSON *body = cJSON_Parse(s_requests[s_request_count - 1].body);
    assert(!strcmp(cJSON_GetObjectItem(body, "wx_app_id")->valuestring, "authorized-app-8"));
    assert(!strcmp(cJSON_GetObjectItem(body, "wx_user_openid")->valuestring, "wx-8"));
    cJSON_Delete(body);
    response(PLATFORM_CALL, generation, "{\"code\":40205}");
    xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ERROR);
    intent(XMINI_INTENT_DISMISS_ERROR, NULL);

    assert(xmini_business_post_contacts_page(XMINI_CONTACT_DEVICE, 1) == ESP_OK);
    xmini_business_process(); response(PLATFORM_DEVICE_CONTACTS, 0, contacts);
    assert(status().contacts_count == 2 && !strcmp(status().contacts[1].id, "device-9"));
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    intent(XMINI_INTENT_DEVICE_CALL, "device-9"); generation = status().generation;
    assert(status().phase == XMINI_BUSINESS_FETCHING);
    intent(XMINI_INTENT_HANGUP, NULL);
    response(PLATFORM_DEVICE_CALL_REQUEST, generation, "{\"code\":200,\"data\":{\"room_id\":\"page-room\"}}");
    response(PLATFORM_DEVICE_CALL_CANCEL, generation, "{\"code\":200}");
    assert(status().phase == XMINI_BUSINESS_IDLE);

    assert(xmini_business_post_contacts_page(XMINI_CONTACT_ALL, 999) == ESP_OK);
    xmini_business_process(); response(PLATFORM_DEVICE_CONTACTS, 0, contacts);
    assert(status().contacts_page == 2 && status().contacts_count == 4 && status().contacts_filtered_total == 20);
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    assert(xmini_business_post_pending_page(999) == ESP_OK);
    xmini_business_process(); response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    assert(status().pending_page == 2 && status().pending_count == 1 && status().pending_total == 9);
    assert(!strcmp(status().pending[0], "pending-8"));

    assert(xmini_business_post_pending_page(0) == ESP_OK); xmini_business_process();
    requests = s_request_count;
    assert(xmini_business_post_pending_page(1) == ESP_OK); xmini_business_process();
    assert(s_request_count == requests && status().pending_loading && !status().pending_count);
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    assert(status().pending_loading && !status().pending_count && s_request_count == requests + 1);
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    assert(!status().pending_loading && status().pending_page == 1 && status().pending_count == 4);
    assert(!strcmp(status().pending[0], "pending-4"));

    assert(xmini_business_post_contacts_page(XMINI_CONTACT_WECHAT, 0) == ESP_OK); xmini_business_process();
    requests = s_request_count;
    assert(xmini_business_post_contacts_page(XMINI_CONTACT_DEVICE, 0) == ESP_OK); xmini_business_process();
    assert(s_request_count == requests && status().contacts_loading && !status().contacts_count);
    response(PLATFORM_DEVICE_CONTACTS, 0, contacts);
    assert(status().contacts_loading && !status().contacts_count && s_request_count == requests + 1);
    response(PLATFORM_DEVICE_CONTACTS, 0, contacts);
    assert(!status().contacts_loading && status().contacts_filter == XMINI_CONTACT_DEVICE);
    assert(status().contacts_count == 8 && !strcmp(status().contacts[0].id, "device-0"));
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);

    assert(xmini_business_post_contacts_page(XMINI_CONTACT_WECHAT, 0) == ESP_OK); xmini_business_process();
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CONTACTS, 0, ESP_ERR_TIMEOUT, NULL);
    assert(!status().contacts_loading && !status().contacts_count && status().contacts_error == ESP_ERR_TIMEOUT);
    requests = s_request_count;
    for (unsigned i = 0; i < 20; ++i) xmini_business_process();
    assert(s_request_count == requests);
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    assert(xmini_business_post_contacts_page(XMINI_CONTACT_WECHAT, 0) == ESP_OK); xmini_business_process();
    response(PLATFORM_DEVICE_CONTACTS, 0, "{\"code\":200,\"data\":{\"contacts\":[{\"type\":\"voip\",\"device_id\":\"wx-invalid\"}]}}");
    assert(!status().contacts_count && status().contacts_error == ESP_ERR_INVALID_RESPONSE);
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    assert(xmini_business_post_pending_page(0) == ESP_OK); xmini_business_process();
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CONTACTS_PENDING, 0, ESP_FAIL, NULL);
    assert(!status().pending_count && !status().pending_loading && status().pending_error == ESP_FAIL);
    assert(status().contacts_error == ESP_ERR_INVALID_RESPONSE);
    assert(xmini_business_post_contacts_page(XMINI_CONTACT_DEVICE, 0) == ESP_OK); xmini_business_process();
    response(PLATFORM_DEVICE_CONTACTS, 0, contacts);
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    assert(status().contacts_error == ESP_OK && status().pending_error == ESP_OK);
    assert(status().error == ESP_FAIL); /* Page success must not erase unrelated failure evidence. */
    assert(xmini_business_maintenance(true) == ESP_OK);
    assert(xmini_business_post_contacts_page(XMINI_CONTACT_DEVICE, 0) == ESP_ERR_INVALID_STATE);
    assert(xmini_business_post_pending_page(0) == ESP_ERR_INVALID_STATE);
    assert(xmini_business_maintenance(false) == ESP_OK);
    s_next_request_error = ESP_ERR_NO_MEM;
    assert(xmini_business_post_contacts_page(XMINI_CONTACT_DEVICE, 0) == ESP_OK);
    xmini_business_process();
    assert(!status().contacts_loading && !status().contacts_count && status().contacts_error == ESP_ERR_NO_MEM);
    requests = s_request_count;
    for (unsigned i = 0; i < 20; ++i) xmini_business_process();
    assert(s_request_count == requests);
    for (unsigned i = 0; i < 8; ++i) assert(xmini_business_post_pending_page(i) == ESP_OK);
    assert(xmini_business_post_pending_page(8) == ESP_ERR_NO_MEM);
    xmini_business_process();
    assert(status().pending_page == 7 && status().pending_loading && !status().pending_count);
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    assert(status().pending_loading && !status().pending_count);
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, pending);
    assert(status().pending_page == 2 && status().pending_count == 1 && !status().pending_loading);
    free(contacts); free(pending);
}

static void test_platform_maintenance_lifecycle(void)
{
    assert(status().phase == XMINI_BUSINESS_IDLE);
    assert(xmini_business_maintenance(true) == ESP_OK);
    unsigned requests = s_request_count;
    platform_event(PLATFORM_MESSAGE, 0, 0, ESP_OK, "{\"type\":\"callers_update\"}");
    response(PLATFORM_PROFILE, 0, "{\"code\":200}");
    response(PLATFORM_DEVICE_CONTACTS, 0, "{\"code\":200,\"data\":{\"contacts\":[]}}");
    for (unsigned i = 0; i < 20; ++i) xmini_business_process();
    assert(s_request_count == requests && status().maintenance);
    assert(xmini_business_maintenance(false) == ESP_OK);
    xmini_business_process();
    assert(s_request_count == requests + 1 && s_requests[requests].operation == PLATFORM_DEVICE_CONTACTS);
    response(PLATFORM_DEVICE_CONTACTS, 0, "{\"code\":200,\"data\":{\"contacts\":[]}}");
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, "{\"code\":200,\"data\":{\"pending\":[]}}");

    response(PLATFORM_DEVICE_CONTACTS, 0,
        "{\"code\":200,\"data\":{\"contacts\":[{\"type\":\"device\",\"device_id\":\"old-account-peer\"}]}}");
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0,
        "{\"code\":200,\"data\":{\"pending\":[{\"type\":\"device\",\"peer_device_id\":\"old-account-pending\"}]}}");
    assert(status().contacts_count == 1 && status().pending_count == 1);
    assert(xmini_business_maintenance(true) == ESP_OK);
    requests = s_request_count;
    unsigned old_epoch = s_platform_epoch++;
    unsigned stale = status().stale_events;
    platform_event_t queued = {.kind = PLATFORM_MESSAGE, .epoch = old_epoch,
        .json = "{\"type\":\"callers_update\"}"};
    for (unsigned i = 0; i < 4; ++i) assert(xmini_business_platform_event(&queued));
    assert(!xmini_business_platform_event(&queued));
    platform_lifecycle_latch(&s_lifecycle, PLATFORM_UNBOUND, ESP_OK, s_platform_epoch);
    platform_lifecycle_latch(&s_lifecycle, PLATFORM_OFFLINE, ESP_ERR_TIMEOUT, s_platform_epoch);
    xmini_business_process();
    assert(!status().platform_online && !status().profile_ready && status().maintenance);
    assert(!status().contacts_count && !status().pending_count);
    assert(status().stale_events == stale + 4 && s_request_count == requests);
    queued = (platform_event_t){.kind = PLATFORM_RESPONSE, .operation = PLATFORM_PROFILE,
        .epoch = old_epoch, .json = "{\"code\":200}"};
    assert(xmini_business_platform_event(&queued));
    xmini_business_process();
    assert(!status().profile_ready && status().stale_events == stale + 5);
    platform_lifecycle_latch(&s_lifecycle, PLATFORM_ONLINE, ESP_OK, s_platform_epoch);
    xmini_business_process();
    assert(status().platform_online && status().maintenance && s_request_count == requests);
    assert(xmini_business_maintenance(false) == ESP_OK);
    for (unsigned i = 0; i < 4; ++i) xmini_business_process();
    assert(s_request_count > requests && s_requests[requests].operation == PLATFORM_PROFILE);
    response(PLATFORM_PROFILE, 0, "{\"code\":200}");
    response(PLATFORM_DEVICE_CONTACTS, 0, "{\"code\":200,\"data\":{\"contacts\":[]}}");
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0, "{\"code\":200,\"data\":{\"pending\":[]}}");
}

static uint32_t device_release_pending(int original_error)
{
    platform_event(PLATFORM_MESSAGE, 0, 0, ESP_OK,
        "{\"type\":\"call_incoming\",\"channel\":\"device\","
        "\"payload\":{\"room_id\":\"release-race\",\"caller_id\":\"device-b\",\"call_type\":\"audio\"}}");
    uint32_t generation = status().generation;
    assert(status().phase == XMINI_BUSINESS_RINGING && generation);
    intent(XMINI_INTENT_ACCEPT, NULL);
    response(PLATFORM_DEVICE_CALL_INFO, generation,
        "{\"code\":200,\"data\":{\"device_id\":\"device-b\",\"token\":\"token\"}}");
    connected(generation);
    assert(status().phase == XMINI_BUSINESS_ACTIVE);
    s_hold_retirement = true; s_rtc.sdk_pending = 1;
    unsigned requests = s_request_count, disconnected = s_disconnects;
    if (original_error) {
        rtc_event(XMINI_RTC_ERROR, generation, 0, NULL);
        s_events[(s_head + s_count - 1) % 32].error = original_error;
        xmini_business_process();
    } else intent(XMINI_INTENT_HANGUP, NULL);
    assert(status().phase == XMINI_BUSINESS_ENDING && s_request_count == requests);
    intent(XMINI_INTENT_HANGUP, NULL);
    assert(s_disconnects == disconnected && s_request_count == requests);
    s_media_busy = false; xmini_business_process();
    assert(s_disconnects == disconnected + 1 && s_rtc.closing && s_request_count == requests);
    unsigned stale = status().stale_events;
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    assert(status().stale_events == stale + 1 && s_request_count == requests);
    s_rtc.connecting = s_rtc.connected = s_rtc.closing = false;
    s_rtc.sdk_pending = 0; s_hold_retirement = false;
    s_track_cleanup_copy = true;
    rtc_event(XMINI_RTC_RETIRED, generation, 0, NULL); xmini_business_process();
    assert(!s_track_cleanup_copy);
    if (s_fail_cleanup_copy) {
        s_fail_cleanup_copy = false;
        assert(!s_cleanup_copy && s_request_count == requests && status().owner == XMINI_BUSINESS_DEVICE);
        return generation;
    }
    assert(s_cleanup_copy);
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CALL_HANGUP);
    assert(s_request_count == requests + 1 && !strcmp(status().room, "release-race"));
    intent(XMINI_INTENT_HANGUP, NULL);
    assert(s_request_count == requests + 1);
    return generation;
}

static void finish_release_test(void)
{
    if (status().owner == XMINI_BUSINESS_DEVICE) {
        platform_event(PLATFORM_MESSAGE, 0, 0, ESP_OK,
            "{\"type\":\"room_cancel\",\"channel\":\"device\",\"payload\":{\"room_id\":\"release-race\"}}");
        xmini_business_process();
    }
    assert(status().owner == XMINI_BUSINESS_NONE);
    assert(!s_cleanup_copy);
    if (status().phase == XMINI_BUSINESS_ERROR) intent(XMINI_INTENT_DISMISS_ERROR, NULL);
    assert(status().phase == XMINI_BUSINESS_IDLE);
}

static void test_device_release_reconciliation(void)
{
    const char *empty = "{\"code\":200,\"msg\":\"ok\"}";
    uint32_t generation = device_release_pending(0);
    unsigned requests = s_request_count, stale = status().stale_events;
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation - 1, ESP_FAIL, "{\"code\":40400}");
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_CANCEL, generation, ESP_FAIL, "{\"code\":40400}");
    platform_event_t wrong_epoch = {.kind = PLATFORM_RESPONSE, .operation = PLATFORM_DEVICE_CALL_HANGUP,
        .generation = generation, .epoch = s_platform_epoch + 1, .error = ESP_FAIL, .json = "{\"code\":40400}"};
    assert(xmini_business_platform_event(&wrong_epoch)); xmini_business_process();
    assert(status().stale_events == stale + 3 && s_request_count == requests && status().owner == XMINI_BUSINESS_DEVICE);
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CALL_ROOM && s_request_count == requests + 1);
    assert(status().phase == XMINI_BUSINESS_ENDING && !strcmp(status().room, "release-race"));
    requests = s_request_count;
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    response(PLATFORM_DEVICE_CALL_ROOM, generation - 1, empty);
    assert(status().phase == XMINI_BUSINESS_ENDING && s_request_count == requests);
    response(PLATFORM_DEVICE_CALL_ROOM, generation, empty);
    assert(status().phase == XMINI_BUSINESS_IDLE && status().error == ESP_OK);
    assert(!s_cleanup_copy);
    response(PLATFORM_DEVICE_CALL_ROOM, generation, empty);
    assert(status().phase == XMINI_BUSINESS_IDLE && s_request_count == requests);

    generation = device_release_pending(TIRTC_E_INVALID_PARAMETER);
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    response(PLATFORM_DEVICE_CALL_ROOM, generation, "{\"code\":200,\"data\":null}");
    assert(status().phase == XMINI_BUSINESS_ERROR && status().owner == XMINI_BUSINESS_NONE &&
        status().error == TIRTC_E_INVALID_PARAMETER);
    finish_release_test();

    generation = device_release_pending(0);
    platform_event(PLATFORM_MESSAGE, 0, 0, ESP_OK,
        "{\"type\":\"room_cancel\",\"channel\":\"device\",\"payload\":{\"room_id\":\"release-race\"}}");
    assert(status().phase == XMINI_BUSINESS_ENDING && !status().room[0]);
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    response(PLATFORM_DEVICE_CALL_ROOM, generation, empty);
    assert(status().phase == XMINI_BUSINESS_IDLE && status().error == ESP_OK);

    generation = device_release_pending(0);
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    platform_event(PLATFORM_MESSAGE, 0, 0, ESP_OK,
        "{\"type\":\"room_cancel\",\"channel\":\"device\",\"payload\":{\"room_id\":\"release-race\"}}");
    assert(status().phase == XMINI_BUSINESS_ENDING && status().owner == XMINI_BUSINESS_DEVICE && s_cleanup_copy);
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_ROOM, generation, ESP_ERR_TIMEOUT, NULL);
    assert(status().phase == XMINI_BUSINESS_IDLE && status().owner == XMINI_BUSINESS_NONE && !s_cleanup_copy);

    generation = device_release_pending(TIRTC_E_INVALID_PARAMETER);
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    platform_event(PLATFORM_MESSAGE, 0, 0, ESP_OK,
        "{\"type\":\"room_cancel\",\"channel\":\"device\",\"payload\":{\"room_id\":\"release-race\"}}");
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_ROOM, generation, ESP_ERR_TIMEOUT, NULL);
    assert(status().phase == XMINI_BUSINESS_ERROR && status().owner == XMINI_BUSINESS_NONE &&
        status().error == TIRTC_E_INVALID_PARAMETER && !s_cleanup_copy);
    finish_release_test();

    generation = device_release_pending(0);
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2001, "{}"); xmini_business_process();
    platform_event(PLATFORM_MESSAGE, 0, 0, ESP_OK,
        "{\"type\":\"room_cancel\",\"channel\":\"device\",\"payload\":{\"room_id\":\"different-room\"}}");
    platform_event_t stale_cancel = {.kind = PLATFORM_MESSAGE, .epoch = s_platform_epoch - 1u,
        .json = "{\"type\":\"room_cancel\",\"channel\":\"device\",\"payload\":{\"room_id\":\"release-race\"}}"};
    assert(xmini_business_platform_event(&stale_cancel)); xmini_business_process();
    assert(!strcmp(status().room, "release-race"));
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_ROOM, generation, ESP_ERR_TIMEOUT, NULL);
    s_now += 16000; xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ERROR && status().owner == XMINI_BUSINESS_DEVICE && s_cleanup_copy);
    finish_release_test();

    s_advance_epoch_on_cleanup = true;
    generation = device_release_pending(0);
    assert(!s_advance_epoch_on_cleanup && s_requests[s_request_count - 1].epoch == s_platform_epoch);
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CALL_ROOM);
    response(PLATFORM_DEVICE_CALL_ROOM, generation, empty);
    assert(status().phase == XMINI_BUSINESS_IDLE && status().error == ESP_OK && !s_cleanup_copy);

    const struct { const char *release; const char *room; bool retained; } failures[] = {
        {"{\"code\":40400}", "{\"code\":200,\"data\":{\"room_id\":\"release-race\",\"status\":\"answered\",\"role\":\"callee\"}}", true},
        {"{\"code\":40400}", "{\"code\":200,\"data\":{\"room_id\":\"different\",\"status\":\"answered\",\"role\":\"callee\"}}", true},
        {"{\"code\":40400}", "{\"code\":50000}", true},
        {"{\"code\":40400}", "{\"code\":0}", true},
        {"{\"code\":40400}", "{\"code\":200.5}", true},
        {"{\"code\":40300}", "{\"code\":200}", false},
        {"{\"code\":50000}", "{\"code\":200}", false},
        {"{\"code\":40400.5}", "{\"code\":200}", false},
        {"{\"code\":\"40400\"}", "{\"code\":200}", false},
    };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        generation = device_release_pending(0);
        platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, failures[i].release);
        requests = s_request_count;
        assert(s_requests[requests - 1].operation == PLATFORM_DEVICE_CALL_ROOM);
        response(PLATFORM_DEVICE_CALL_ROOM, generation, failures[i].room);
        if (failures[i].retained) { s_now += 16000; xmini_business_process(); }
        assert(status().phase == XMINI_BUSINESS_ERROR && status().error != ESP_OK && s_request_count == requests);
        if (failures[i].retained)
            assert(status().owner == XMINI_BUSINESS_DEVICE && !strcmp(status().room, "release-race"));
        finish_release_test();
    }

    generation = device_release_pending(0);
    unsigned old_epoch = s_platform_epoch++;
    platform_event_t old_response = {.kind = PLATFORM_RESPONSE, .operation = PLATFORM_DEVICE_CALL_HANGUP,
        .generation = generation, .epoch = old_epoch, .error = ESP_FAIL, .json = "{\"code\":40400}"};
    assert(xmini_business_platform_event(&old_response)); xmini_business_process();
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CALL_ROOM);
    response(PLATFORM_DEVICE_CALL_ROOM, generation, empty);
    assert(status().phase == XMINI_BUSINESS_ERROR && status().owner == XMINI_BUSINESS_NONE &&
        status().error == ESP_ERR_INVALID_STATE);
    finish_release_test();

    generation = device_release_pending(0);
    platform_event(PLATFORM_RESPONSE, PLATFORM_DEVICE_CALL_HANGUP, generation, ESP_FAIL, "{\"code\":40400}");
    old_epoch = s_platform_epoch++;
    requests = s_request_count;
    old_response = (platform_event_t){.kind = PLATFORM_RESPONSE, .operation = PLATFORM_DEVICE_CALL_ROOM,
        .generation = generation, .epoch = old_epoch, .error = ESP_ERR_INVALID_STATE, .json = empty};
    assert(xmini_business_platform_event(&old_response)); xmini_business_process();
    for (unsigned i = 0; i < 20; ++i) xmini_business_process();
    assert(s_request_count == requests && status().owner == XMINI_BUSINESS_DEVICE && s_cleanup_copy);
    s_now += 16000; xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ERROR && status().error == ESP_ERR_INVALID_STATE);
    finish_release_test();

    unsigned freed = s_cleanup_copy_frees;
    s_fail_cleanup_copy = true;
    generation = device_release_pending(0);
    requests = s_request_count;
    for (unsigned i = 0; i < 20; ++i) xmini_business_process();
    assert(s_request_count == requests && !s_cleanup_copy && s_cleanup_copy_frees == freed);
    s_now += 16000; xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ERROR && status().owner == XMINI_BUSINESS_DEVICE &&
        status().error == ESP_ERR_NO_MEM && !strcmp(status().room, "release-race"));
    finish_release_test();
}

static uint32_t live_incoming(void)
{
    assert(status().phase == XMINI_BUSINESS_IDLE && s_inbound_generation);
    uint32_t generation = s_inbound_generation;
    s_rtc.generation = generation;
    s_rtc.connected = true;
    s_rtc.connecting = s_rtc.closing = false;
    s_rtc.subscribed = s_rtc.business_permit = 0;
    s_rtc.sdk_pending = 1;
    s_hold_retirement = true;
    rtc_event(XMINI_RTC_ACCEPTED, generation, 0, NULL);
    xmini_business_process();
    assert(status().owner == XMINI_BUSINESS_LIVE && status().generation == generation);
    assert(status().phase == XMINI_BUSINESS_ACTIVE && status().media_active && s_media_busy);
    assert(status().tx_stream == 10 && s_subscription_generation == generation && s_subscription_stream == 11);
    return generation;
}

static void connection_event(xmini_rtc_event_kind_t kind, uint32_t generation, int error)
{
    rtc_event(kind, generation, 0, NULL);
    s_events[(s_head + s_count - 1) % 32].error = error;
    if (kind == XMINI_RTC_ERROR && generation == s_rtc.generation) {
        s_rtc.connected = false;
        s_rtc.subscribed = s_rtc.business_permit = 0;
    }
    xmini_business_process();
}

static void live_retired(uint32_t generation, int error)
{
    assert(status().owner == XMINI_BUSINESS_LIVE && status().phase == XMINI_BUSINESS_ENDING);
    s_media_busy = false;
    xmini_business_process();
    assert(status().owner == XMINI_BUSINESS_LIVE && s_rtc.sdk_pending == 1);
    s_rtc.connecting = s_rtc.connected = s_rtc.closing = false;
    s_rtc.sdk_pending = 0;
    s_hold_retirement = false;
    rtc_event(XMINI_RTC_RETIRED, generation, 0, NULL);
    xmini_business_process();
    assert(status().owner == XMINI_BUSINESS_NONE && !status().media_active);
    assert(status().phase == (error ? XMINI_BUSINESS_ERROR : XMINI_BUSINESS_IDLE));
    assert(status().error == error);
    if (error) intent(XMINI_INTENT_DISMISS_ERROR, NULL);
    assert(status().phase == XMINI_BUSINESS_IDLE && s_inbound_generation);
}

static void test_live_remote_close(void)
{
    uint32_t generation = live_incoming();
    unsigned stops = s_media_stops, disconnects = s_disconnects, stale = status().stale_events;
    connection_event(XMINI_RTC_ERROR, generation - 1, TIRTC_E_CONN_REMOTECLOSE);
    connection_event(XMINI_RTC_CLOSED, generation - 1, 0);
    assert(status().phase == XMINI_BUSINESS_ACTIVE && status().error == 0);
    assert(s_media_stops == stops && status().stale_events == stale + 2);
    connection_event(XMINI_RTC_ERROR, generation, TIRTC_E_CONN_REMOTECLOSE);
    assert(status().phase == XMINI_BUSINESS_ENDING && status().error == 0);
    assert(s_media_stops == stops + 1 && s_disconnects == disconnects);
    connection_event(XMINI_RTC_ERROR, generation, TIRTC_E_CONN_REMOTECLOSE);
    connection_event(XMINI_RTC_CLOSED, generation, 0);
    assert(status().owner == XMINI_BUSINESS_LIVE && status().phase == XMINI_BUSINESS_ENDING);
    assert(status().error == 0 && s_media_stops == stops + 1);
    live_retired(generation, 0);
    connection_event(XMINI_RTC_ERROR, generation, TIRTC_E_CONN_REMOTECLOSE);
    connection_event(XMINI_RTC_CLOSED, generation, 0);
    assert(status().phase == XMINI_BUSINESS_IDLE && status().error == 0);

    generation = live_incoming();
    connection_event(XMINI_RTC_CLOSED, generation, 0);
    live_retired(generation, 0);

    generation = live_incoming();
    intent(XMINI_INTENT_HANGUP, NULL);
    connection_event(XMINI_RTC_ERROR, generation, TIRTC_E_CONN_REMOTECLOSE);
    live_retired(generation, 0);

    const struct { xmini_rtc_event_kind_t kind; int error; } faults[] = {
        {XMINI_RTC_ERROR, TIRTC_E_TIMEOUTED},
        {XMINI_RTC_ERROR, TIRTC_E_CONN_TIMEOUTCLOSE},
        {XMINI_RTC_ERROR, TIRTC_E_CONN_OTHER_ERROR},
        {XMINI_RTC_ERROR, TIRTC_E_LACK_OF_RESOURCE},
        {XMINI_RTC_FORMAT_ERROR, ESP_ERR_INVALID_RESPONSE},
        {XMINI_RTC_FORMAT_ERROR, TIRTC_E_CONN_REMOTECLOSE},
        {XMINI_RTC_CLOSED, TIRTC_E_CONN_OTHER_ERROR},
    };
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        generation = live_incoming();
        connection_event(faults[i].kind, generation, faults[i].error);
        assert(status().phase == XMINI_BUSINESS_ENDING && status().error == faults[i].error);
        connection_event(XMINI_RTC_ERROR, generation, TIRTC_E_CONN_REMOTECLOSE);
        connection_event(XMINI_RTC_CLOSED, generation, 0);
        assert(status().error == faults[i].error);
        live_retired(generation, faults[i].error);
    }
    generation = live_incoming();
    s_next_media_stop_error = ESP_FAIL;
    connection_event(XMINI_RTC_ERROR, generation, TIRTC_E_CONN_REMOTECLOSE);
    assert(status().phase == XMINI_BUSINESS_ENDING && status().error == ESP_FAIL);
    connection_event(XMINI_RTC_CLOSED, generation, 0);
    live_retired(generation, ESP_FAIL);
    puts("LIVE close: remote normal / duplicate / stale / local cancel / real first faults / media stop failure PASS");
}

static void test_remote_close_owner_isolation(void)
{
    const xmini_business_owner_t owners[] = {XMINI_BUSINESS_AI, XMINI_BUSINESS_DEVICE,
        XMINI_BUSINESS_WECHAT, XMINI_BUSINESS_GROUP};
    for (unsigned i = 0; i < sizeof(owners) / sizeof(owners[0]); ++i) {
        assert(status().phase == XMINI_BUSINESS_IDLE);
        uint32_t generation;
        if (owners[i] == XMINI_BUSINESS_AI) {
            intent(XMINI_INTENT_AI_START, NULL);
            generation = status().generation;
            response(PLATFORM_AI_TOKEN, generation,
                "{\"code\":200,\"data\":{\"peer_id\":\"whips://ai\",\"token\":\"test-token\",\"role_id\":\"role\"}}");
            connected(generation);
        } else if (owners[i] == XMINI_BUSINESS_DEVICE) {
            platform_event(PLATFORM_MESSAGE, 0, 0, 0,
                "{\"type\":\"call_incoming\",\"channel\":\"device\",\"payload\":{\"room_id\":\"live-isolation\",\"caller_id\":\"device-b\"}}");
            generation = status().generation;
            intent(XMINI_INTENT_ACCEPT, NULL);
            response(PLATFORM_DEVICE_CALL_INFO, generation,
                "{\"code\":200,\"data\":{\"device_id\":\"device-b\",\"token\":\"token\"}}");
            connected(generation);
        } else if (owners[i] == XMINI_BUSINESS_WECHAT) {
            generation = wechat_join("wx-live-isolation");
        } else {
            assert(xmini_business_post_group(XMINI_GROUP_REFRESH, NULL, NULL) == 0);
            xmini_business_process();
            generation = status().generation;
        }
        assert(status().owner == owners[i]);
        connection_event(XMINI_RTC_ERROR, generation, TIRTC_E_CONN_REMOTECLOSE);
        assert(status().error == TIRTC_E_CONN_REMOTECLOSE);
        s_media_busy = false;
        xmini_business_process();
        xmini_business_process();
        if (owners[i] == XMINI_BUSINESS_DEVICE) {
            assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CALL_HANGUP);
            response(PLATFORM_DEVICE_CALL_HANGUP, generation, "{\"code\":200}");
        }
        assert(status().phase == XMINI_BUSINESS_ERROR && status().owner == XMINI_BUSINESS_NONE);
        assert(status().error == TIRTC_E_CONN_REMOTECLOSE);
        intent(XMINI_INTENT_DISMISS_ERROR, NULL);
    }
    puts("LIVE close isolation: AI / DEV / WX / GROUP retain REMOTECLOSE failure semantics PASS");
}

static void test_wechat_local_rx_selection(void)
{
    for (unsigned outgoing = 0; outgoing < 2; ++outgoing) {
        for (unsigned failing = 0; failing < 2; ++failing) {
            assert(status().phase == XMINI_BUSINESS_IDLE && status().profile_ready);
            assert(xmini_business_post_contacts_page(XMINI_CONTACT_WECHAT, 0) == ESP_OK);
            xmini_business_process();
            settle_default_contacts();
            assert(status().contacts_filter == XMINI_CONTACT_WECHAT && status().contacts_count == 1);
            unsigned calls = s_rx_select_calls, allows = s_audio_allow_calls, starts = s_media_starts;
            uint32_t generation;
            char room[48], call[48], json[768];
            snprintf(room, sizeof(room), "rx-select-%u-%u", outgoing, failing);
            if (outgoing) {
                snprintf(call, sizeof(call), "rx-select-call-%u", failing);
                intent(XMINI_INTENT_WECHAT_CALL, "openid");
                assert(status().owner == XMINI_BUSINESS_WECHAT && status().phase == XMINI_BUSINESS_FETCHING);
                generation = status().generation;
                snprintf(json, sizeof(json), "{\"code\":0,\"data\":{\"call_id\":\"%s\"}}", call);
                response(PLATFORM_CALL, generation, json);
                snprintf(json, sizeof(json),
                    "{\"type\":\"call_incoming\",\"channel\":\"wx\",\"payload\":{\"wx_room_id\":\"%s\","
                    "\"wx_from\":\"device-a\",\"wx_call_id\":\"%s\",\"wx_user_openid\":\"openid\",\"wx_app_id\":\"wxapp\","
                    "\"peer_id\":\"whips://wx\",\"token\":\"token\",\"wx_model_id\":\"model\",\"wx_server_token\":\"secret\"}}", room, call);
                platform_event(PLATFORM_MESSAGE, 0, 0, 0, json);
            } else {
                generation = wechat_incoming(room);
                intent(XMINI_INTENT_ACCEPT, NULL);
            }
            connected(generation);
            assert(status().phase == XMINI_BUSINESS_SESSION && !status().media_active);
            assert(s_rx_select_calls == calls && s_audio_allow_calls == allows && s_media_starts == starts);
            assert(!s_rx_selected_generation && !s_rtc.business_permit);
            const char *invalid[] = {"not-json", "null", "[]", "1", "\"string\""};
            for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
                rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, invalid[i]);
            rtc_event(XMINI_RTC_COMMAND, generation, MAKE_CMDW(0x2000, 7), "{}");
            rtc_event(XMINI_RTC_COMMAND, generation, 0x2000 | RESPONSE_BIT, "{}");
            rtc_event(XMINI_RTC_COMMAND, generation - 1, 0x2000, NULL);
            xmini_business_process();
            assert(status().phase == XMINI_BUSINESS_SESSION && !status().media_active);
            assert(s_rx_select_calls == calls && s_audio_allow_calls == allows && s_media_starts == starts);
            if (failing) {
                s_next_rx_select_error = ESP_ERR_NO_MEM;
                s_hold_retirement = true;
                s_rtc.sdk_pending = 1;
            }
            rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, outgoing ? "{}" : NULL);
            xmini_business_process();
            assert(s_rx_select_calls == calls + 1 && "WX_LOCAL_RX_REQUIRED_AFTER_ANSWER");
            if (failing) {
                assert(!status().media_active && !s_rtc.business_permit);
                assert(s_audio_allow_calls == allows && s_media_starts == starts);
                assert(status().phase == XMINI_BUSINESS_ENDING && status().error == ESP_ERR_NO_MEM);
                rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, "{}");
                xmini_business_process();
                assert(s_rx_select_calls == calls + 1 && s_audio_allow_calls == allows && s_media_starts == starts);
                platform_event(PLATFORM_ONLINE, 0, 0, ESP_OK, NULL);
                response(PLATFORM_PROFILE, 0, "{\"code\":200}");
                assert(status().phase == XMINI_BUSINESS_ENDING && status().error == ESP_ERR_NO_MEM);
                wechat_retired(generation);
                assert(status().owner == XMINI_BUSINESS_NONE && status().phase == XMINI_BUSINESS_ERROR);
                assert(status().error == ESP_ERR_NO_MEM && !strcmp(status().reason, "WX receive selection failed"));
                assert(!s_rtc.business_permit && s_audio_allow_calls == allows && s_media_starts == starts);
                intent(XMINI_INTENT_DISMISS_ERROR, NULL);
                assert(status().phase == XMINI_BUSINESS_IDLE);
                assert(s_requests[s_request_count - 1].operation == PLATFORM_PROFILE);
                response(PLATFORM_PROFILE, 0, "{\"code\":200}");
                settle_default_contacts();
            } else {
                assert(s_rx_selected_generation == generation && s_rx_selected_stream == 0);
                assert(status().phase == XMINI_BUSINESS_ACTIVE && status().media_active);
                assert(s_audio_allow_calls == allows + 1 && s_media_starts == starts + 1 && s_rtc.business_permit == 1);
                assert(s_rx_select_order < s_audio_allow_order && s_audio_allow_order < s_audio_start_order);
                rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, outgoing ? NULL : "{}");
                xmini_business_process();
                assert(s_rx_select_calls == calls + 2 && s_rx_selected_generation == generation);
                assert(s_audio_allow_calls == allows + 1 && s_media_starts == starts + 1);
                rtc_event(XMINI_RTC_COMMAND, generation, 0x2001, "{\"reason\":1}");
                xmini_business_process();
                assert(status().phase == XMINI_BUSINESS_ENDING);
                s_media_busy = false;
                xmini_business_process();
                xmini_business_process();
                assert(status().phase == XMINI_BUSINESS_IDLE && status().owner == XMINI_BUSINESS_NONE);
            }
            calls = s_rx_select_calls;
            rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, NULL);
            xmini_business_process();
            assert(s_rx_select_calls == calls && !status().media_active);
        }
    }
    puts("WX local RX: both answer directions, select-before-media, duplicate/invalid/stale and failure retirement PASS");
}

static void test_metadata_first_error(void)
{
    const int rtc_history = TIRTC_E_CONN_REMOTECLOSE;
    s_rtc.last_error = rtc_history;
    for (unsigned held = 0; held < 2; ++held) {
        assert(status().phase == XMINI_BUSINESS_IDLE && status().profile_ready);
        settle_default_contacts();
        if (held) {
            intent(XMINI_INTENT_AI_START, NULL);
            response(PLATFORM_AI_TOKEN, status().generation,
                "{\"code\":200,\"data\":{\"peer_id\":\"whips://ai\",\"token\":\"test-token\",\"role_id\":\"role\"}}");
            assert(s_rtc.connecting);
            s_hold_retirement = true;
            s_rtc.sdk_pending = 1;
        } else {
            assert(xmini_business_post_group(XMINI_GROUP_REFRESH, NULL, NULL) == ESP_OK);
            xmini_business_process();
        }
        uint32_t generation = status().generation;
        assert(generation);
        platform_event(PLATFORM_OFFLINE, 0, 0, ESP_FAIL, NULL);
        xmini_business_phase_t expected = held ? XMINI_BUSINESS_ENDING : XMINI_BUSINESS_ERROR;
        assert(status().phase == expected && status().error == ESP_FAIL);
        assert(!strcmp(status().reason, "platform offline"));

        unsigned requests = s_request_count;
        platform_event(PLATFORM_ONLINE, 0, 0, ESP_OK, NULL);
        platform_event(PLATFORM_ONLINE, 0, 0, ESP_OK, NULL);
        intent(XMINI_INTENT_REFRESH_CONTACTS, NULL);
        assert(xmini_business_post_pending_page(0) == ESP_OK);
        xmini_business_process();
        assert(s_request_count == requests && !s_inbound_generation);

        unsigned stale_events = status().stale_events;
        platform_event_t stale = {.kind = PLATFORM_RESPONSE, .operation = PLATFORM_PROFILE,
            .epoch = s_platform_epoch + 1, .error = ESP_ERR_TIMEOUT};
        assert(xmini_business_platform_event(&stale));
        xmini_business_process();
        assert(status().stale_events == stale_events + 1);

        /* Already-admitted completions must retain the original session fault. */
        response(PLATFORM_PROFILE, 0, "{\"code\":503}");
        assert(!status().profile_ready && status().error == ESP_FAIL && status().phase == expected);
        response(PLATFORM_PROFILE, 0, "{\"code\":200}");
        assert(status().profile_ready && status().error == ESP_FAIL && status().phase == expected);
        assert(s_request_count == requests && !s_inbound_generation);

        if (held) {
            intent(XMINI_INTENT_DISMISS_ERROR, NULL);
            assert(status().phase == XMINI_BUSINESS_ENDING && status().generation == generation);
            s_rtc.connecting = s_rtc.connected = s_rtc.closing = false;
            s_rtc.sdk_pending = 0;
            s_hold_retirement = false;
            rtc_event(XMINI_RTC_RETIRED, generation, 0, NULL);
            xmini_business_process();
        }
        assert(status().owner == XMINI_BUSINESS_NONE && status().phase == XMINI_BUSINESS_ERROR);
        assert(status().error == ESP_FAIL && !strcmp(status().reason, "platform offline"));
        assert(xmini_business_post_group(XMINI_GROUP_REFRESH, NULL, NULL) == ESP_OK);
        xmini_business_process();
        assert(status().owner == XMINI_BUSINESS_NONE && status().phase == XMINI_BUSINESS_ERROR);
        assert(s_request_count == requests && s_rtc.last_error == rtc_history);

        intent(XMINI_INTENT_DISMISS_ERROR, NULL);
        assert(status().phase == XMINI_BUSINESS_IDLE && !status().error && s_rtc.last_error == rtc_history);
        assert(s_request_count == requests + 1 && s_requests[requests].operation == PLATFORM_PROFILE);
        response(PLATFORM_PROFILE, 0, "{\"code\":503}");
        assert(status().error == 503 && !status().profile_ready && !s_inbound_generation);
        platform_event(PLATFORM_ONLINE, 0, 0, ESP_OK, NULL);
        response(PLATFORM_PROFILE, 0, "{\"code\":200}");
        assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CONTACTS && !s_inbound_generation);
        settle_default_contacts();
        assert(status().phase == XMINI_BUSINESS_IDLE && !status().error && status().profile_ready);
        assert(!status().contacts_error && !status().pending_error && s_rtc.last_error == rtc_history);
        assert(s_inbound_generation);
    }
    puts("Metadata: deferred ENDING/ERROR refresh, sticky first fault, stale epoch and explicit idle recovery PASS");
}

static void test_active_metadata_deferred(void)
{
    settle_default_contacts();
    uint32_t generation = wechat_join("wx-metadata-deferred");
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, NULL);
    xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ACTIVE && status().media_active);
    unsigned requests = s_request_count;
    platform_event(PLATFORM_ONLINE, 0, 0, ESP_OK, NULL);
    platform_event(PLATFORM_MESSAGE, 0, 0, 0, "{\"type\":\"callers_update\"}");
    assert(xmini_business_post_contacts_page(XMINI_CONTACT_WECHAT, 0) == ESP_OK);
    assert(xmini_business_post_pending_page(0) == ESP_OK);
    xmini_business_process();
    for (unsigned i = 0; i < 4; ++i) xmini_business_process();
    assert(s_request_count == requests);
    assert(status().phase == XMINI_BUSINESS_ACTIVE && status().generation == generation);

    intent(XMINI_INTENT_HANGUP, NULL);
    assert(s_request_count == requests && status().phase == XMINI_BUSINESS_ENDING);
    s_media_busy = false;
    xmini_business_process();
    assert(s_request_count == requests);
    xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_IDLE && status().owner == XMINI_BUSINESS_NONE);
    assert(s_request_count == requests + 1 && s_requests[requests].operation == PLATFORM_PROFILE);
    assert(!s_inbound_generation);
    response(PLATFORM_PROFILE, 0, "{\"code\":200}");
    assert(s_requests[requests + 1].operation == PLATFORM_DEVICE_CONTACTS && !s_inbound_generation);
    settle_default_contacts();
    assert(status().profile_ready && !status().contacts_loading && !status().pending_loading);
    assert(s_inbound_generation);
    puts("Metadata: ACTIVE and true-retirement gate; idle profile -> contacts -> pending -> listener PASS");
}

static void recorded_command(uint32_t generation, uint32_t word, const char *json,
                               xmini_business_command_disposition_t disposition, int error)
{
    xmini_business_command_diagnostics_t before, after;
    xmini_business_command_diagnostics(&before);
    rtc_event(XMINI_RTC_COMMAND, generation, word, json);
    s_events[(s_head + s_count - 1) % 32].connection = (tirtc_conn_t)(uintptr_t)0x1234abcd;
    xmini_business_process();
    xmini_business_command_diagnostics(&after);
    assert(after.sequence == (before.sequence == UINT32_MAX ? 1 : before.sequence + 1));
    assert(after.generation == generation && after.connection32 == 0x1234abcd && after.word == word);
    assert(after.disposition == (uint32_t)disposition && after.error == error);
}

static void test_command_dispatch_metadata(void)
{
    for (unsigned failure = 0; failure < 3; ++failure) {
        assert(status().phase == XMINI_BUSINESS_IDLE);
        assert(xmini_business_post_contacts_page(XMINI_CONTACT_DEVICE, 0) == ESP_OK);
        xmini_business_process(); settle_default_contacts();
        intent(XMINI_INTENT_DEVICE_CALL, "device-b");
        uint32_t generation = status().generation;
        response(PLATFORM_DEVICE_CALL_REQUEST, generation,
            "{\"code\":200,\"data\":{\"room_id\":\"command-observation\"}}");
        s_rtc.generation = generation;
        connected(generation);
        assert(status().phase == XMINI_BUSINESS_OUTGOING);
        unsigned starts = s_media_starts, stale = status().stale_events;
        recorded_command(generation + 1, 0x2000, "not-json", XMINI_COMMAND_DISPATCH_STALE_GENERATION, 0);
        assert(status().stale_events == stale + 1 && status().phase == XMINI_BUSINESS_OUTGOING);
        recorded_command(generation, MAKE_CMDW(0x2000, 7), "not-json", XMINI_COMMAND_DISPATCH_OTHER, 0);
        recorded_command(generation, 0x2000, "{\"room_id\":\"foreign\"}", XMINI_COMMAND_DISPATCH_INVALID_OR_ROOM_MISMATCH, 0);
        assert(!status().media_active && s_media_starts == starts);
        int expected = failure == 1 ? TIRTC_E_INVALID_HANDLE : failure == 2 ? ESP_ERR_NO_MEM : 0;
        if (failure) {
            s_hold_retirement = true;
            s_rtc.sdk_pending = 1;
            if (failure == 1) s_rtc.connected = false;
            else s_next_subscribe_error = expected;
        } else s_prompt_busy = true;
        recorded_command(generation, 0x2000, "{\"room_id\":\"command-observation\"}",
            failure == 2 ? XMINI_COMMAND_DISPATCH_SUBSCRIBE_ERROR : XMINI_COMMAND_DISPATCH_ACTIVATION_BRANCH,
            failure == 2 ? expected : 0);
        assert(!status().media_active && s_media_starts == starts); /* Branch called does not mean ACTIVE. */
        if (failure) {
            assert(status().phase == XMINI_BUSINESS_ENDING && status().error == expected);
            recorded_command(generation, 0x2000, "not-json", XMINI_COMMAND_DISPATCH_ENDING, 0);
            assert(status().phase == XMINI_BUSINESS_ENDING && status().error == expected);
            s_hold_retirement = false; s_rtc.sdk_pending = 0;
            s_rtc.connecting = s_rtc.connected = s_rtc.closing = false;
            rtc_event(XMINI_RTC_RETIRED, generation, 0, NULL);
        } else {
            s_prompt_busy = false; xmini_business_process();
            assert(status().phase == XMINI_BUSINESS_ACTIVE && s_media_starts == starts + 1);
            recorded_command(generation, 0x2000, "{\"room_id\":\"command-observation\"}", XMINI_COMMAND_DISPATCH_ACTIVATION_BRANCH, 0);
            assert(status().phase == XMINI_BUSINESS_ACTIVE && s_media_starts == starts + 1);
            intent(XMINI_INTENT_HANGUP, NULL);
        }
        s_media_busy = false; xmini_business_process(); xmini_business_process();
        platform_operation_t operation = s_requests[s_request_count - 1].operation;
        assert(operation == (failure ? PLATFORM_DEVICE_CALL_CANCEL : PLATFORM_DEVICE_CALL_HANGUP));
        response(operation, generation, "{\"code\":200}");
        assert(status().phase == (failure ? XMINI_BUSINESS_ERROR : XMINI_BUSINESS_IDLE) && status().error == expected);
        if (failure) intent(XMINI_INTENT_DISMISS_ERROR, NULL);
        assert(status().phase == XMINI_BUSINESS_IDLE);
    }
    puts("COMMAND dispatch: existing stale/word/room gates, called-not-active, subscribe failure and ENDING first error PASS");
}

static void test_device_remote_hangup_intent(void)
{
    for (unsigned test = 0; test < 4; ++test) {
        bool remote = test < 2, outgoing = test == 0 || test == 2;
        assert(status().phase == XMINI_BUSINESS_IDLE);
        assert(xmini_business_post_contacts_page(XMINI_CONTACT_DEVICE, 0) == ESP_OK);
        xmini_business_process(); settle_default_contacts();
        if (outgoing) {
            intent(XMINI_INTENT_DEVICE_CALL, "device-b");
            uint32_t generation = status().generation;
            response(PLATFORM_DEVICE_CALL_REQUEST, generation,
                "{\"code\":200,\"data\":{\"room_id\":\"hangup-intent\"}}");
            s_rtc.generation = generation;
            connected(generation);
            rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, "{\"room_id\":\"hangup-intent\"}");
            xmini_business_process();
        } else {
            platform_event(PLATFORM_MESSAGE, 0, 0, 0,
                "{\"type\":\"call_incoming\",\"channel\":\"device\","
                "\"payload\":{\"room_id\":\"hangup-intent\",\"caller_id\":\"device-b\"}}");
            uint32_t generation = status().generation;
            intent(XMINI_INTENT_ACCEPT, NULL);
            response(PLATFORM_DEVICE_CALL_INFO, generation,
                "{\"code\":200,\"data\":{\"device_id\":\"device-b\",\"token\":\"token\"}}");
            connected(generation);
        }
        uint32_t generation = status().generation;
        assert(status().phase == XMINI_BUSINESS_ACTIVE && status().media_active);
        unsigned sent = s_device_hangups, requests = s_request_count;
        int expected = remote ? 0 : test == 2 ? TIRTC_E_INVALID_HANDLE : ESP_ERR_INVALID_RESPONSE;
        if (remote) {
            rtc_event(XMINI_RTC_COMMAND, generation, 0x2001, "{\"reason\":1}");
            /* A valid command can be queued before the SDK retires its handle. */
            s_rtc.connected = false;
            s_rtc.last_error = TIRTC_E_CONN_REMOTECLOSE;
            xmini_business_process();
        } else {
            s_device_send_override = true;
            s_device_send_result = test == 2 ? TIRTC_E_INVALID_HANDLE : 0;
            intent(XMINI_INTENT_HANGUP, NULL);
            s_device_send_override = false;
        }
        assert(status().phase == XMINI_BUSINESS_ENDING && status().error == expected);
        assert(s_device_hangups == sent + (remote ? 0U : 1U));
        assert(s_request_count == requests); /* The codec still owns the session. */
        s_media_busy = false;
        s_rtc.connecting = s_rtc.connected = s_rtc.closing = false;
        s_rtc.sdk_pending = 0;
        rtc_event(XMINI_RTC_RETIRED, generation, 0, NULL);
        xmini_business_process();
        assert(s_request_count == requests + 1);
        assert(s_requests[requests].operation == PLATFORM_DEVICE_CALL_HANGUP);
        assert(s_requests[requests].generation == generation);
        cJSON *body = cJSON_Parse(s_requests[requests].body);
        assert(!strcmp(cJSON_GetObjectItem(body, "room_id")->valuestring, "hangup-intent"));
        cJSON_Delete(body);
        response(PLATFORM_DEVICE_CALL_HANGUP, generation, "{\"code\":200}");
        assert(status().owner == XMINI_BUSINESS_NONE && status().error == expected);
        assert(status().phase == (remote ? XMINI_BUSINESS_IDLE : XMINI_BUSINESS_ERROR));
        if (!remote) intent(XMINI_INTENT_DISMISS_ERROR, NULL);
        assert(status().phase == XMINI_BUSINESS_IDLE);
        assert(s_rtc.last_error == TIRTC_E_CONN_REMOTECLOSE);
    }
    puts("DEV hangup: remote notification has no echo, cloud cleanup retained, local send failures preserved PASS");
}

int main(void)
{
    xmini_business_callbacks_t callbacks = {.receive = receive, .media_start = media_start,
        .media_stop = media_stop, .media_busy = media_busy, .media_flush = media_flush};
    assert(xmini_business_init(&callbacks) == 0);
    assert(status().phase == XMINI_BUSINESS_IDLE && s_starts == 0);
    platform_event(PLATFORM_ONLINE, 0, 0, 0, NULL);
    xmini_business_process();
    assert(s_starts == 1 && s_requests[0].operation == PLATFORM_PROFILE);
    assert(strlen(s_requests[0].body) < 768);
    printf("profile_bytes=%zu\n", strlen(s_requests[0].body));
    cJSON *profile = cJSON_Parse(s_requests[0].body);
    const cJSON *scenes = cJSON_GetObjectItem(profile, "profiles");
    const cJSON *stream = cJSON_GetObjectItem(scenes, "stream");
    assert(cJSON_GetObjectItem(stream, "down_audio_streamid")->valueint == 11);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(stream, "no_video"))); cJSON_Delete(profile);
    assert(!s_inbound_generation);
    response(PLATFORM_PROFILE, 0, "{\"code\":200}");
    assert(!s_inbound_generation && s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CONTACTS);
    response(PLATFORM_DEVICE_CONTACTS, 0,
        "{\"code\":200,\"data\":{\"contacts\":[{\"type\":\"device\",\"device_id\":\"device-b\",\"remark\":\"peer\"},"
        "{\"type\":\"voip\",\"device_id\":\"openid\",\"wx_app_id\":\"wxapp\",\"remark\":\"phone\"}]}}");
    assert(status().contacts_count == 2 && status().profile_ready);
    assert(!s_inbound_generation && s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CONTACTS_PENDING);
    response(PLATFORM_DEVICE_CONTACTS_PENDING, 0,
        "{\"code\":200,\"data\":{\"pending\":[{\"type\":\"device\",\"peer_device_id\":\"foreign-peer\"}]}}");
    assert(status().pending_count == 1);
    assert(s_inbound_generation);
    assert(xmini_business_maintenance(true) == 0 && status().maintenance);
    assert(xmini_business_post_intent(XMINI_INTENT_AI_START, NULL) == ESP_ERR_INVALID_STATE);
    assert(xmini_business_post_contact(XMINI_CONTACT_REFRESH, NULL, NULL) == ESP_ERR_INVALID_STATE);
    assert(xmini_business_post_group(XMINI_GROUP_REFRESH, NULL, NULL) == ESP_ERR_INVALID_STATE);
    assert(xmini_business_maintenance(false) == 0 && !status().maintenance);
    unsigned before_approval = s_request_count;
    xmini_business_process();
    assert(s_request_count == before_approval); /* Reading/dismissing a notice is not an approval. */
    assert(xmini_business_post_contact(XMINI_CONTACT_REJECT, "foreign-peer", NULL) == 0);
    xmini_business_process();
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CONTACT_RESPOND);
    cJSON *approval = cJSON_Parse(s_requests[s_request_count - 1].body);
    assert(!strcmp(cJSON_GetObjectItem(approval, "action")->valuestring, "reject"));
    cJSON_Delete(approval);
    response(PLATFORM_DEVICE_CONTACT_RESPOND, 0, "{\"code\":200,\"data\":{\"status\":\"rejected\"}}");
    char long_remark[66]; memset(long_remark, 'a', 65); long_remark[65] = 0;
    assert(xmini_business_post_contact(XMINI_CONTACT_REMARK, "device-b", long_remark) == ESP_ERR_INVALID_SIZE);
    assert(xmini_business_post_contact(XMINI_CONTACT_REMARK, "device-b", "\xED\xA0\x80") == ESP_ERR_INVALID_ARG);
    static const char *invalid_remarks[] = {
        "\xff", "\x80", "\xc0\xaf", "\xc2", "\xe0\x80\x80", "\xed\xa0\x80",
        "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xe5\xb0", "\xf0\x9f\x98",
    };
    unsigned before_invalid_remark = s_request_count;
    for (unsigned i = 0; i < sizeof(invalid_remarks) / sizeof(invalid_remarks[0]); ++i)
        assert(xmini_business_post_contact(XMINI_CONTACT_REMARK, "device-b", invalid_remarks[i]) == ESP_ERR_INVALID_ARG);
    xmini_business_process();
    assert(s_request_count == before_invalid_remark);
    char unicode_remark[258];
    for (unsigned i = 0; i < 64; ++i) memcpy(unicode_remark + 4 * i, "\xf0\x9f\x98\x80", 4);
    unicode_remark[256] = 'a'; unicode_remark[257] = '\0';
    assert(xmini_business_post_contact(XMINI_CONTACT_REMARK, "device-b", unicode_remark) == ESP_ERR_INVALID_SIZE);
    unicode_remark[256] = '\0';
    assert(xmini_business_post_contact(XMINI_CONTACT_REMARK, "device-b", unicode_remark) == 0);
    xmini_business_process();
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CONTACT_REMARK);
    cJSON *remark_body = cJSON_Parse(s_requests[s_request_count - 1].body);
    assert(!strcmp(cJSON_GetObjectItem(remark_body, "peer_id")->valuestring, "device-b"));
    assert(!strcmp(cJSON_GetObjectItem(remark_body, "remark")->valuestring, unicode_remark));
    cJSON_Delete(remark_body);
    response(PLATFORM_DEVICE_CONTACT_REMARK, 0, "{\"code\":200,\"data\":{}}");
    assert(xmini_business_post_contact(XMINI_CONTACT_REMARK, "device-b", "") == 0);
    xmini_business_process();
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CONTACT_REMARK);
    remark_body = cJSON_Parse(s_requests[s_request_count - 1].body);
    assert(cJSON_IsString(cJSON_GetObjectItem(remark_body, "remark")));
    assert(!*cJSON_GetObjectItem(remark_body, "remark")->valuestring);
    cJSON_Delete(remark_body);
    response(PLATFORM_DEVICE_CONTACT_REMARK, 0, "{\"code\":200,\"data\":{}}");
    settle_default_contacts();
    test_group_command_words();
    intent(XMINI_INTENT_AI_START, NULL);
    uint32_t generation = status().generation;
    assert(status().phase == XMINI_BUSINESS_FETCHING && generation);
    response(PLATFORM_AI_TOKEN, generation,
        "{\"code\":200,\"data\":{\"peer_id\":\"whips://ai\",\"token\":\"test-token\",\"role_id\":\"role\"}}");
    connected(generation);
    assert(status().phase == XMINI_BUSINESS_SESSION && s_command_code == 0x2100);
    uint8_t upstream[160] = {0};
    assert(xmini_business_send_audio(generation, upstream, sizeof(upstream), 0) < 0);
    assert(!s_rtc.subscribed && !s_rtc.business_permit);
    cJSON *start = cJSON_Parse(s_command);
    const char *id = cJSON_GetObjectItem(start, "id")->valuestring;
    const cJSON *params = cJSON_GetObjectItem(start, "params");
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(params, "input_audio"), "codec")->valuestring, "alaw"));
    char accepted[512];
    snprintf(accepted, sizeof(accepted),
        "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":{\"session_id\":\"session\","
        "\"input_audio\":{\"codec\":\"alaw\",\"sample_rate\":8000,\"channels\":1},"
        "\"output_audio\":{\"codec\":\"alaw\",\"sample_rate\":8000,\"channels\":1}}}", id);
    cJSON_Delete(start);
    s_prompt_busy = true;
    rtc_event(XMINI_RTC_COMMAND, generation, MAKE_CMDW(0x2100, 0xffff) | RESPONSE_BIT, accepted);
    xmini_business_process();
    assert(!status().media_active);
    s_prompt_busy = false; xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ACTIVE && s_media_starts == 1);
    assert(!s_rtc.subscribed && s_rtc.business_permit == (1U << 1));
    assert(xmini_business_send_audio(generation, upstream, sizeof(upstream), 0) == 0);
    assert(xmini_business_send_audio(generation - 1, upstream, sizeof(upstream), 0) < 0);
    const uint32_t ai_words[] = {0x2100, 0x2101, MAKE_CMDW(0x2100, 0),
        MAKE_CMDW(0x2100, 7), MAKE_CMDW(0x2100, 7) | RESPONSE_BIT, 0xabcd2100};
    for (unsigned i = 0; i < sizeof(ai_words) / sizeof(ai_words[0]); ++i) {
        rtc_event(XMINI_RTC_COMMAND, generation, ai_words[i],
            "{\"method\":\"caption\",\"params\":{\"text\":\"caption\",\"caption_type\":0,\"utterance_id\":1,\"mode\":0}}");
        xmini_business_process(); assert(!strcmp(status().caption, "caption"));
    }
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2001, "{}"); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ACTIVE);
    assert(xmini_business_maintenance(true) == ESP_ERR_INVALID_STATE);
    uint8_t packet[160] = {0};
    assert(s_receiver(generation, 1, 0, packet, 160, NULL));
    assert(!s_receiver(generation, 10, 0, packet, 160, NULL));
    intent(XMINI_INTENT_AI_INTERRUPT, NULL);
    assert(s_flushes == 1);
    intent(XMINI_INTENT_HANGUP, NULL);
    assert(status().phase == XMINI_BUSINESS_ENDING && s_disconnects == 0);
    assert(!s_rtc.business_permit);
    assert(xmini_business_send_audio(generation, upstream, sizeof(upstream), 0) < 0);
    s_media_busy = false; xmini_business_process(); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_IDLE && s_disconnects == 1 && s_stops == 0);
    intent(XMINI_INTENT_DEVICE_CALL, "device-b");
    generation = status().generation;
    assert(status().phase == XMINI_BUSINESS_FETCHING);
    intent(XMINI_INTENT_HANGUP, NULL);
    assert(status().phase == XMINI_BUSINESS_ENDING);
    response(PLATFORM_DEVICE_CALL_REQUEST, generation, "{\"code\":200,\"data\":{\"room_id\":\"d_room\"}}");
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CALL_CANCEL);
    response(PLATFORM_DEVICE_CALL_CANCEL, generation, "{\"code\":200}");
    assert(status().phase == XMINI_BUSINESS_IDLE);
    platform_event(PLATFORM_MESSAGE, 0, 0, 0,
        "{\"type\":\"call_incoming\",\"channel\":\"device\",\"payload\":{\"room_id\":\"d_room2\",\"caller_id\":\"device-b\",\"call_type\":\"audio\"}}");
    generation = status().generation;
    assert(status().phase == XMINI_BUSINESS_RINGING);
    intent(XMINI_INTENT_ACCEPT, NULL);
    response(PLATFORM_DEVICE_CALL_INFO, generation, "{\"code\":200,\"data\":{\"device_id\":\"device-b\",\"token\":\"token\"}}");
    connected(generation);
    assert(status().phase == XMINI_BUSINESS_ACTIVE && s_command_code == 0x2000);
    platform_event(PLATFORM_MESSAGE, 0, 0, 0,
        "{\"type\":\"room_cancel\",\"channel\":\"device\",\"payload\":{\"room_id\":\"other-room\"}}");
    assert(status().phase == XMINI_BUSINESS_ACTIVE);
    rtc_event(XMINI_RTC_COMMAND, generation, MAKE_CMDW(0x2001, 7), "{}"); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ACTIVE);
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2001, "{}"); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ENDING); /* Bare 2001 is a hangup, not a response. */
    s_media_busy = false; xmini_business_process(); xmini_business_process();
    assert(s_requests[s_request_count - 1].operation == PLATFORM_DEVICE_CALL_HANGUP);
    response(PLATFORM_DEVICE_CALL_HANGUP, generation, "{\"code\":200}");
    assert(status().phase == XMINI_BUSINESS_IDLE && s_stops == 0);
    intent(XMINI_INTENT_WECHAT_CALL, "openid");
    generation = status().generation;
    response(PLATFORM_CALL, generation, "{\"code\":0,\"data\":{\"call_id\":\"call-1\"}}");
    platform_event(PLATFORM_MESSAGE, 0, 0, 0,
        "{\"type\":\"call_incoming\",\"channel\":\"wx\",\"payload\":{\"wx_room_id\":\"wxroom\","
        "\"wx_from\":\"device-a\",\"wx_call_id\":\"call-1\",\"wx_user_openid\":\"openid\",\"wx_app_id\":\"wxapp\","
        "\"peer_id\":\"whips://wx\",\"token\":\"token\",\"wx_model_id\":\"model\",\"wx_server_token\":\"secret\"}}");
    connected(generation);
    assert(status().phase == XMINI_BUSINESS_SESSION && !status().media_active);
    rtc_event(XMINI_RTC_COMMAND, generation, MAKE_CMDW(0x2000, 7), "{}"); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_SESSION && !status().media_active);
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, "not-json"); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_SESSION && !status().media_active);
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, "null"); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_SESSION && !status().media_active);
    rtc_event(XMINI_RTC_COMMAND, generation - 1, 0x2000, NULL); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_SESSION && !status().media_active);
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, NULL); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ACTIVE && status().media_active);
    assert(status().tx_stream == 0 && s_rtc.business_permit == 1);
    assert(xmini_business_send_audio(generation, packet, sizeof(packet), 0) == 0);
    assert(s_receiver(generation, 0, 0, packet, sizeof(packet), NULL));
    assert(!s_receiver(generation, 1, 0, packet, sizeof(packet), NULL));
    assert(!s_receiver(generation, 10, 0, packet, sizeof(packet), NULL));
    rtc_event(XMINI_RTC_SUBSCRIBE, generation, 0, NULL);
    s_events[(s_head + s_count - 1) % 32].stream = 1;
    xmini_business_process();
    assert(status().tx_stream == 0 && s_rtc.business_permit == 1);
    unsigned starts_after_empty_answer = s_media_starts;
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2000, "{}"); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ACTIVE && s_media_starts == starts_after_empty_answer);
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2001, NULL); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ACTIVE);
    rtc_event(XMINI_RTC_COMMAND, generation, MAKE_CMDW(0x2001, 7), "{\"reason\":1}"); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ACTIVE);
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2001, "{\"reason\":1}"); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ENDING);
    s_media_busy = false; xmini_business_process(); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_IDLE);
    test_wechat_reject_retirement();
    test_wechat_joined_hangup();
    unsigned old_media_starts = s_media_starts;
    intent(XMINI_INTENT_AI_START, NULL);
    generation = status().generation;
    response(PLATFORM_AI_TOKEN, generation,
        "{\"code\":200,\"data\":{\"peer_id\":\"whips://ai\",\"token\":\"test-token\",\"role_id\":\"role\"}}");
    connected(generation);
    start = cJSON_Parse(s_command); id = cJSON_GetObjectItem(start, "id")->valuestring;
    snprintf(accepted, sizeof(accepted),
        "{\"id\":\"%s\",\"result\":{\"session_id\":\"bad-session\","
        "\"input_audio\":{\"codec\":\"alaw\",\"sample_rate\":8000,\"channels\":1},"
        "\"output_audio\":{\"codec\":\"alaw\",\"sample_rate\":16000,\"channels\":1}}}", id);
    cJSON_Delete(start);
    rtc_event(XMINI_RTC_COMMAND, generation, 0x2100, accepted); xmini_business_process(); xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ERROR && !status().media_active && s_media_starts == old_media_starts);
    intent(XMINI_INTENT_DISMISS_ERROR, NULL);
    intent(XMINI_INTENT_DEVICE_CALL, "device-b"); generation = status().generation;
    response(PLATFORM_DEVICE_CALL_REQUEST, generation, "{\"code\":200,\"data\":{\"room_id\":\"d_failed_release\"}}");
    intent(XMINI_INTENT_HANGUP, NULL);
    response(PLATFORM_DEVICE_CALL_CANCEL, generation, "{\"code\":500,\"msg\":\"failure\"}");
    assert(status().owner == XMINI_BUSINESS_DEVICE && !strcmp(status().room, "d_failed_release"));
    s_now += 16000; xmini_business_process();
    assert(status().phase == XMINI_BUSINESS_ERROR && status().owner == XMINI_BUSINESS_DEVICE);
    intent(XMINI_INTENT_DISMISS_ERROR, NULL);
    assert(status().owner == XMINI_BUSINESS_DEVICE); /* A dismissed popup cannot hide an owned room. */
    platform_event(PLATFORM_MESSAGE, 0, 0, 0,
        "{\"type\":\"room_cancel\",\"channel\":\"device\",\"payload\":{\"room_id\":\"d_failed_release\"}}");
    assert(status().owner == XMINI_BUSINESS_NONE);
    intent(XMINI_INTENT_DISMISS_ERROR, NULL);
    assert(status().phase == XMINI_BUSINESS_IDLE);
    char oversized[8193]; memset(oversized, 'a', sizeof(oversized)); oversized[sizeof(oversized) - 1] = 0;
    platform_event_t bad = {.kind = PLATFORM_MESSAGE, .json = oversized};
    assert(!xmini_business_platform_event(&bad));
    test_platform_maintenance_lifecycle();
    test_contact_pages();
    test_session_intent_generation();
    test_device_release_reconciliation();
    test_live_remote_close();
    test_remote_close_owner_isolation();
    test_metadata_first_error();
    test_active_metadata_deferred();
    test_wechat_local_rx_selection();
    test_command_dispatch_metadata();
    test_device_remote_hangup_intent();
    puts("Business FSM: strict AI, true retirement, room cleanup, WX app identity, sticky lifecycle/epochs, maintenance, bounded contacts/pending pages PASS");
    return 0;
}
