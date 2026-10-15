/* Run actual callback dispatch with SDK/network calls replaced at the boundary. */
#include "../components/wechat_voip/voip.c"
#include <assert.h>

unsigned test_critical_depth;
static unsigned allocations, rejects, hangups, profile_requests, contact_requests;
static bool online;
static bool realtime_media;
static tirtc_conn_t handle = (void *)(uintptr_t)42;
static void *json_alloc(size_t bytes)
{
    void *p = malloc(bytes);
    assert(p);
    ++allocations;
    return p;
}
static void json_free(void *p)
{
    if (p) {
        assert(allocations);
        --allocations;
        free(p);
    }
}
void test_log(const char *level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; assert(!test_critical_depth); }
int64_t esp_timer_get_time(void) { return 1000000; }
void sample_audio_end(void) {}
void sample_audio_subscribed(uint8_t stream, bool enabled) { (void)stream; (void)enabled; }
void sample_audio_status(void) {}
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 65536; }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 49152; }
size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 32768; }
unsigned uxTaskGetStackHighWaterMark(void *task) { (void)task; return 8192; }
void rtc_uninit(void) {}
void rtc_connect_inputs_release(void) {}
int rtc_connect(uint32_t generation, const char *peer, const char *token)
{ (void)generation; (void)peer; (void)token; return 0; }
int rtc_disconnect(tirtc_conn_t h) { assert(h == handle); return 0; }
int rtc_send_command(tirtc_conn_t h, uint32_t cmd, const char *body)
{ assert(h == handle && cmd == 0x2001 && body); ++hangups; return 16; }
int rtc_service_request(const char *path, const char *body, TIRTCSERVICEREQUESTCALLBACK cb)
{
    assert(!strcmp(path, "/v1/wxvoip/reject") && !cb);
    assert(strstr(body, "session-secret") && strstr(body, "room"));
    ++rejects;
    return 0;
}
bool platform_is_online(void) { return online; }
void platform_set_realtime_media(bool active) { realtime_media = active; }
esp_err_t platform_request(platform_operation_t operation, uint32_t generation, const char *json)
{
    (void)generation;
    if (operation == PLATFORM_PROFILE) {
        assert(json == s_profile);
        ++profile_requests;
    } else if (operation == PLATFORM_CONTACTS) {
        assert(!json);
        ++contact_requests;
    } else {
        assert(false);
    }
    return ESP_OK;
}
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test"; }
void platform_identity_snapshot(platform_identity_t *id) { memset(id, 0, sizeof(*id)); }
const char *platform_client_id(void) { return "test"; }
const char *platform_rtc_endpoint(void) { return "https://example.invalid"; }
int rtc_start(const char *a, const char *b, const char *c, const char *d)
{ (void)a; (void)b; (void)c; (void)d; return 0; }

static void begin(bool outgoing)
{
    assert(!allocations && !s_payload && s_call.stage == CALL_IDLE);
    assert(call_begin(&s_call, outgoing));
    if (outgoing)
        assert(call_join_outgoing(&s_call));
    else
        assert(call_accept(&s_call));
    s_payload = cJSON_Parse("{\"wx_app_id\":\"app\",\"wx_model_id\":\"model\","
                           "\"wx_server_token\":\"session-secret\",\"wx_room_id\":\"room\"}");
    assert(s_payload && allocations);
    s_connect_pending = true;
}
static void connected_event(int error)
{
    rtc_event_t e = {.kind = RTC_CONNECTED, .generation = s_call.generation,
                     .connection = error ? NULL : handle, .error = error};
    rtc_dispatch(&e);
}
static void close_event(void)
{
    rtc_event_t e = {.kind = RTC_CLOSED, .generation = s_call.generation, .connection = handle};
    rtc_dispatch(&e);
    assert(!s_payload && !allocations && s_call.stage == CALL_IDLE);
}
int main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_alloc, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
    s_sdk_ready = true;
    for (unsigned i = 0; i < 100; ++i) {
        begin(i % 2 == 0);
        rtc_event_t stale = {.kind = RTC_CONNECTED, .generation = s_call.generation + 1};
        rtc_dispatch(&stale);
        assert(s_payload && allocations); /* Stale events must not discard this join. */
        connected_event(0);
        assert(!s_payload && !allocations && s_call.stage == CALL_WAITING_PEER);
        rtc_event_t answer = {.kind = RTC_COMMAND, .generation = s_call.generation,
                              .connection = handle, .command = 0x2000};
        rtc_dispatch(&answer);
        assert(s_call.stage == CALL_ACTIVE);
        assert(realtime_media);
        if (i % 2 == 0)
            end_call(true, 1);
        else {
            rtc_event_t remote = {.kind = RTC_COMMAND, .generation = s_call.generation,
                                  .connection = handle, .command = 0x2001,
                                  .data = "{\"reason\":1}"};
            rtc_dispatch(&remote);
        }
        assert(s_call.stage == CALL_CLOSING && !rejects);
        close_event();
        assert(!realtime_media);
    }
    assert(hangups == 50);
    begin(false);
    connected_event(TIRTC_E_SERVER_ERROR);
    assert(rejects == 1 && !s_payload && !allocations && s_call.stage == CALL_IDLE);
    begin(true);
    end_call(true, 7); /* Cancel before the asynchronous connect callback. */
    assert(s_payload && allocations && s_call.stage == CALL_CLOSING);
    connected_event(0);
    assert(!s_payload && !allocations && hangups == 51 && rejects == 1);
    close_event();

    assert(call_begin(&s_call, false));
    s_payload = cJSON_Parse("{\"wx_app_id\":\"app\",\"wx_model_id\":\"model\","
                           "\"wx_server_token\":\"session-secret\",\"wx_room_id\":\"room\"}");
    assert(s_payload && allocations);
    double_action();
    assert(rejects == 2 && !s_payload && !allocations && s_call.stage == CALL_IDLE);
    double_action();
    assert(rejects == 2); /* An idle double click must not repeat rejection. */

    online = true;
    begin(true);
    connected_event(0);
    rtc_event_t answer = {.kind = RTC_COMMAND, .generation = s_call.generation,
                          .connection = handle, .command = 0x2000};
    rtc_dispatch(&answer);
    request_profile();
    assert(s_profile_dirty && profile_requests == 0);
    end_call(false, 1);
    close_event();
    assert(!s_profile_dirty && s_profile_pending && profile_requests == 1);
    s_profile_pending = false;

    begin(true);
    connected_event(0);
    answer.generation = s_call.generation;
    rtc_dispatch(&answer);
    s_contact_count = 1;
    signal("{\"channel\":\"wx\",\"type\":\"callers_update\",\"payload\":{}}");
    assert(!s_contact_count && s_contacts_dirty && contact_requests == 0);
    end_call(false, 1);
    close_event();
    assert(!s_contacts_dirty && s_contacts_pending && contact_requests == 1);
    cJSON_InitHooks(NULL);
    puts("Join ownership and deferred metadata refresh passed");
}
