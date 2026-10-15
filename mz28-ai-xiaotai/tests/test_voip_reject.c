/* Exercise the actual private rejection path, not a parallel state-machine model. */
#include "../components/wechat_voip/voip.c"
#include <assert.h>
#include <stdio.h>

unsigned test_critical_depth;
static unsigned requests;
static int request_result;
static unsigned expected_reason;

int64_t esp_timer_get_time(void) { return 1000000; }
void test_log(const char *level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    (void)format;
    assert(!test_critical_depth);
}
int rtc_service_request(const char *path, const char *body,
                        TIRTCSERVICEREQUESTCALLBACK callback)
{
    assert(strcmp(path, "/v1/wxvoip/reject") == 0);
    assert(callback == NULL); /* Successful responses need not contain any data. */
    cJSON *json = cJSON_Parse(body);
    assert(json);
    assert(strcmp(string(json, "wx_session_token"), "server-session-token") == 0);
    assert(!cJSON_GetObjectItemCaseSensitive(json, "token"));
    assert(cJSON_GetObjectItemCaseSensitive(json, "hangup_reason")->valueint ==
           (int)expected_reason);
    cJSON_Delete(json);
    ++requests;
    return request_result;
}
static void check_secure_clear(void)
{
    char secret[] = "server-session-token";
    secure_clear(secret, sizeof(secret));
    for (size_t i = 0; i < sizeof(secret); ++i)
        assert(secret[i] == 0);
}
int main(void)
{
    cJSON *payload = cJSON_Parse("{\"wx_app_id\":\"app\",\"wx_model_id\":\"model\","
                                "\"wx_server_token\":\"server-session-token\","
                                "\"wx_room_id\":\"room\",\"token\":\"not-this-token\"}");
    assert(payload);
    check_secure_clear();
    assert(reject_payload(payload, 7) != 0 && requests == 0);
    s_sdk_ready = true;
    expected_reason = 7;
    for (unsigned i = 0; i < 3; ++i) {
        assert(reject_payload(payload, 7) == 0);
        assert(requests == i + 1); /* No completion callback, still reusable. */
    }
    const int errors[] = {TIRTC_E_SERVER_ERROR, 503, 50002};
    expected_reason = 5;
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        request_result = errors[i];
        assert(reject_payload(payload, 5) == errors[i]);
    }
    request_result = 0;
    assert(reject_payload(payload, 5) == 0 && requests == 7);
    char oversized[2200];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = 0;
    assert(cJSON_AddStringToObject(payload, "wx_payload", oversized));
    assert(reject_payload(payload, 5) != 0 && requests == 7);
    cJSON_DeleteItemFromObjectCaseSensitive(payload, "wx_server_token");
    assert(reject_payload(payload, 5) != 0 && requests == 7);
    cJSON_Delete(payload);
    puts("Actual VoIP rejection completion and repeat-call tests passed");
}
