#include "rtc_adapter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const TIRTCCALLBACKS *callbacks;
static TIRTCCONNECTCALLBACK connect_callback;
static void *connect_user, *handle_user;
struct _tirtc_conn { unsigned mock_id; };
static struct _tirtc_conn handle_storage;
static tirtc_conn_t handle = &handle_storage;
static rtc_event_t last_event;
static unsigned step, events, disconnects, uninitializations;
static bool accept_event = true, in_sdk_call;
static int option_failure, init_result, start_result;

static bool observe(const rtc_event_t *event)
{
    ++events;
    last_event = *event;
    if (event->kind == RTC_STARTED || event->kind == RTC_STOPPED)
        assert(in_sdk_call); /* These events can precede API return. */
    return accept_event;
}

void test_log(const char *level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    (void)format;
}

int TiRtcSetOption(TIRTCOPTION option, const void *data, uint32_t length)
{
    switch (option) {
    case TIRTC_OPT_MAX_SEND_BUFFER:
        assert(step++ == 0 && length == sizeof(uint32_t));
        assert(*(const uint32_t *)data == 16 * 1024);
        break;
    case TIRTC_OPT_MAX_CONNECTIONS:
        assert(step++ == 1 && length == sizeof(int));
        assert(*(const int *)data == 1);
        break;
    case TIRTC_OPT_DEVICE_SECRET_KEY:
        assert(step++ == 4 && length == strlen("test-key"));
        assert(memcmp(data, "test-key", length) == 0);
        break;
    case TIRTC_OPT_CLIENT_ID:
        assert(step++ == 5 && length == strlen("stable-client"));
        assert(memcmp(data, "stable-client", length) == 0);
        break;
    case TIRTC_OPT_SERVICE_ENDPOINT:
        assert(step++ == 6 && length == strlen("http://example.invalid"));
        assert(memcmp(data, "http://example.invalid", length) == 0);
        break;
    default:
        assert(!"unexpected SDK option");
    }
    return (int)option == option_failure ? TIRTC_E_INVALID_PARAMETER : 0;
}

int TiRtcInit(void)
{
    assert(step++ == 2);
    return init_result;
}
void TiRtcLogSetLevel(int level)
{
    assert(step++ == 3 && level == 0);
}
const char *TiRtcGetVersion(void) { return "2.5.0-test"; }
int TiRtcStart(const char *id, const TIRTCCALLBACKS *cbs)
{
    assert(step++ == 7 && strcmp(id, "test-device") == 0);
    callbacks = cbs;
    if (!start_result) {
        in_sdk_call = true;
        callbacks->on_event(TIRTC_EVENT_SYS_STARTED, NULL, 0);
        in_sdk_call = false;
    }
    return start_result;
}
int TiRtcStop(void)
{
    in_sdk_call = true;
    callbacks->on_event(TIRTC_EVENT_SYS_STOPPED, NULL, 0);
    in_sdk_call = false;
    return 0;
}
void TiRtcUninit(void)
{
    assert(!in_sdk_call);
    ++uninitializations;
}
int TiRtcWhipConnect(const char *peer, const char *token, TIRTCCONNECTCALLBACK cb, void *user)
{
    assert(strcmp(peer, "whips://test?room=sample") == 0);
    assert(strcmp(token, "test-token") == 0);
    connect_callback = cb;
    connect_user = user;
    return 0;
}
int TiRtcConnSetUserData(tirtc_conn_t h, void *user)
{
    assert(h == handle);
    handle_user = user;
    return 0;
}
void *TiRtcConnGetUserData(tirtc_conn_t h)
{
    assert(h == handle);
    return handle_user;
}
int TiRtcDisconnect(tirtc_conn_t h)
{
    assert(h == handle && !in_sdk_call);
    ++disconnects;
    return 0;
}

static int start(void)
{
    step = 0;
    return rtc_start("test-device", "test-key", "stable-client", "http://example.invalid");
}

int main(void)
{
    rtc_adapter_init(observe, NULL);
    assert(start() == 0 && step == 8);
    assert(last_event.kind == RTC_STARTED);
    assert(callbacks->on_event && callbacks->on_audio && callbacks->on_command);
    assert(callbacks->on_conn_error && callbacks->on_disconnected);
    assert(!callbacks->on_video && !callbacks->on_message && !callbacks->on_update_bitrate);
    assert(!callbacks->on_request_key_frame && !callbacks->on_unsubscribe_video);
    assert(!callbacks->on_sleep_wakeup_info);

    unsigned before = events;
    assert(rtc_connect(7, "whips://test?room=sample", "test-token") == 0);
    assert(events == before); /* Submission alone is not a connected event. */
    connect_callback(0, handle, connect_user);
    assert(last_event.kind == RTC_CONNECTED && last_event.generation == 7);
    assert(last_event.connection == handle && last_event.error == 0);

    char json[] = "{\"reason\":1}";
    callbacks->on_command(handle, 0x2001, json, strlen(json));
    memset(json, 'x', strlen(json));
    assert(last_event.kind == RTC_COMMAND && last_event.generation == 7);
    assert(strcmp(last_event.data, "{\"reason\":1}") == 0);
    before = events;
    callbacks->on_command(handle, 0x2001, NULL, 1);
    char oversized[sizeof(last_event.data)] = {0};
    callbacks->on_command(handle, 0x2001, oversized, sizeof(oversized));
    assert(events == before);

    assert(callbacks->on_subscribe_audio(handle, 0) == 0);
    assert(last_event.stream == 0 && last_event.kind == RTC_SUBSCRIBE);
    assert(callbacks->on_subscribe_audio(handle, 15) == 0);
    assert(last_event.stream == 15);
    before = events;
    assert(callbacks->on_subscribe_audio(handle, 16) != 0 && events == before);
    accept_event = false;
    assert(callbacks->on_subscribe_audio(handle, 10) == TIRTC_E_BUSY);
    accept_event = true;
    assert(callbacks->on_subscribe_video(handle, 1) != 0);
    callbacks->on_unsubscribe_audio(handle, 10);
    assert(last_event.kind == RTC_UNSUBSCRIBE && last_event.stream == 10);

    in_sdk_call = true;
    callbacks->on_conn_error(handle, TIRTC_E_CONN_REMOTECLOSE);
    in_sdk_call = false;
    assert(last_event.kind == RTC_ERROR && disconnects == 0);
    assert(rtc_disconnect(handle) == 0 && disconnects == 1);
    in_sdk_call = true;
    callbacks->on_disconnected(handle);
    in_sdk_call = false;
    assert(last_event.kind == RTC_CLOSED && last_event.generation == 7);
    rtc_connect_inputs_release();
    assert(rtc_stop() == 0 && last_event.kind == RTC_STOPPED);
    assert(uninitializations == 0);
    rtc_uninit();
    assert(uninitializations == 1 && rtc_stop() == TIRTC_E_NOT_INITIALIZED);

    option_failure = TIRTC_OPT_MAX_SEND_BUFFER;
    assert(start() == TIRTC_E_INVALID_PARAMETER && step == 1);
    assert(uninitializations == 1);
    option_failure = 0;
    init_result = TIRTC_E_LACK_OF_RESOURCE;
    assert(start() == init_result && step == 3 && uninitializations == 1);
    init_result = 0;
    option_failure = TIRTC_OPT_CLIENT_ID;
    assert(start() == TIRTC_E_INVALID_PARAMETER && step == 6 && uninitializations == 2);
    option_failure = 0;
    start_result = TIRTC_E_SERVER_ERROR;
    assert(start() == start_result && step == 8 && uninitializations == 3);
    assert(rtc_stop() == TIRTC_E_NOT_INITIALIZED);
    puts("RTC API contract tests passed");
}
