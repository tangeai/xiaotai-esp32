#include "rtc_adapter.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#define PEER_CAPACITY 2048
#define TOKEN_CAPACITY 1024
#define TIRTC_SDK_DEFAULT_ENDPOINT "http://ep-tirtc.tange365.com"
#define TIRTC_SDK_DEFAULT_ENDPOINT_HTTPS_ALIAS "https://ep-tirtc.tange365.com"
static rtc_observer_t s_observer;
static rtc_audio_receiver_t s_audio;
static bool s_initialized;
static atomic_uint s_generation;
static char *s_peer, *s_token;
static size_t s_peer_size, s_token_size;
static void secure_clear(void *memory, size_t size)
{
    volatile unsigned char *p = memory;
    while (size--)
        *p++ = 0;
}
void rtc_connect_inputs_release(void)
{
    if (s_peer) {
        secure_clear(s_peer, s_peer_size);
        free(s_peer);
    }
    if (s_token) {
        secure_clear(s_token, s_token_size);
        free(s_token);
    }
    s_peer = s_token = NULL;
    s_peer_size = s_token_size = 0;
}
static bool emit(rtc_event_t event)
{
    if (!s_observer || !s_observer(&event)) {
        ESP_LOGE("rtc", "control event overflow: kind=%d", event.kind);
        return false;
    }
    return true;
}
static uint32_t generation(tirtc_conn_t h)
{
    uint32_t g = (uint32_t)(uintptr_t)TiRtcConnGetUserData(h);
    return g ? g : atomic_load(&s_generation);
}
static void event(int type, const void *data, int len)
{
    (void)data;
    (void)len;
    if (type == TIRTC_EVENT_SYS_STARTED)
        emit((rtc_event_t){.kind = RTC_STARTED});
    if (type == TIRTC_EVENT_SYS_STOPPED)
        emit((rtc_event_t){.kind = RTC_STOPPED});
}
static void connected(int error, tirtc_conn_t h, void *user)
{
    uint32_t g = (uint32_t)(uintptr_t)user;
    if (!error && h)
        TiRtcConnSetUserData(h, user);
    emit((rtc_event_t){.kind = RTC_CONNECTED, .generation = g, .connection = h, .error = error});
}
static void accepted(tirtc_conn_t h)
{
    /* This sample advertises VoIP only. Release unsupported inbound P2P on the
     * control task rather than invoking SDK lifecycle calls from a callback. */
    TiRtcConnSetUserData(h, (void *)(uintptr_t)UINT32_MAX);
    emit((rtc_event_t){.kind = RTC_UNEXPECTED, .connection = h});
}
static void failed(tirtc_conn_t h, int error)
{
    emit((rtc_event_t){
        .kind = RTC_ERROR, .connection = h, .generation = generation(h), .error = error});
}
static void closed(tirtc_conn_t h)
{
    emit((rtc_event_t){.kind = RTC_CLOSED, .connection = h, .generation = generation(h)});
}
static void audio(tirtc_conn_t h, const TIRTCFRAMEINFO *frame, void *data)
{
    if (s_audio)
        s_audio(h, frame, data);
}
static void command(tirtc_conn_t h, uint32_t cmd, const void *data, uint32_t length)
{
    rtc_event_t e = {.kind = RTC_COMMAND,
                     .connection = h,
                     .generation = generation(h),
                     .command = cmd,
                     .length = length};
    if (length >= sizeof(e.data) || (length && !data)) {
        ESP_LOGW("rtc", "command payload too large/invalid: cmd=0x%lx len=%lu", (unsigned long)cmd,
                 (unsigned long)length);
        return;
    }
    if (length)
        memcpy(e.data, data, length);
    emit(e);
}
static int subscribe(tirtc_conn_t h, uint8_t stream)
{
    if (stream > 15)
        return TIRTC_E_INVALID_PARAMETER;
    return emit((rtc_event_t){.kind = RTC_SUBSCRIBE,
                              .connection = h,
                              .generation = generation(h),
                              .stream = stream})
               ? 0
               : TIRTC_E_BUSY;
}
static void unsubscribe(tirtc_conn_t h, uint8_t stream)
{
    emit((rtc_event_t){
        .kind = RTC_UNSUBSCRIBE, .connection = h, .generation = generation(h), .stream = stream});
}
static int no_video(tirtc_conn_t h, uint8_t stream)
{
    (void)h;
    (void)stream;
    return -1;
}
/* Unused callbacks stay NULL, as required by the current C API reference. */
static const TIRTCCALLBACKS s_callbacks = {.on_event = event,
                                           .on_conn_accepted = accepted,
                                           .on_conn_error = failed,
                                           .on_disconnected = closed,
                                           .on_audio = audio,
                                           .on_command = command,
                                           .on_subscribe_audio = subscribe,
                                           .on_unsubscribe_audio = unsubscribe,
                                           .on_subscribe_video = no_video};
void rtc_adapter_init(rtc_observer_t observer, rtc_audio_receiver_t receiver)
{
    s_observer = observer;
    s_audio = receiver;
}
static int option(TIRTCOPTION opt, const char *value)
{
    return TiRtcSetOption(opt, value, strlen(value));
}
int rtc_start(const char *id, const char *key, const char *client, const char *endpoint)
{
    if (s_initialized)
        return TIRTC_E_BUSY;
    /* Pure audio: bounded network queue, not the video 512 KiB default.
     * Application additionally limits queued live audio to 100 ms. */
    uint32_t send_limit = 16 * 1024;
    int connections = 1;
    int rc = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER, &send_limit, sizeof(send_limit));
    if (!rc)
        rc = TiRtcSetOption(TIRTC_OPT_MAX_CONNECTIONS, &connections, sizeof(connections));
    if (!rc)
        rc = TiRtcInit();
    if (rc)
        return rc;
    s_initialized = true;
    TiRtcLogSetLevel(0);
    bool use_compiled_endpoint =
        strcmp(endpoint, TIRTC_SDK_DEFAULT_ENDPOINT) == 0 ||
        strcmp(endpoint, TIRTC_SDK_DEFAULT_ENDPOINT_HTTPS_ALIAS) == 0;
    if (!(rc = option(TIRTC_OPT_DEVICE_SECRET_KEY, key)) &&
        !(rc = option(TIRTC_OPT_CLIENT_ID, client)) && !use_compiled_endpoint)
        rc = option(TIRTC_OPT_SERVICE_ENDPOINT, endpoint);
    if (!rc)
        rc = TiRtcStart(id, &s_callbacks);
    if (rc) {
        TiRtcUninit();
        s_initialized = false;
    }
    ESP_LOGI("rtc", "start submitted: SDK=%s result=%d", TiRtcGetVersion(), rc);
    return rc;
}
int rtc_connect(uint32_t gen, const char *peer, const char *token)
{
    if (!peer || !token || !peer[0] || !token[0] || strlen(peer) >= PEER_CAPACITY ||
        strlen(token) >= TOKEN_CAPACITY)
        return TIRTC_E_INVALID_PARAMETER;
    if (s_peer || s_token)
        return TIRTC_E_BUSY;
    s_peer_size = strlen(peer) + 1;
    s_token_size = strlen(token) + 1;
    s_peer = malloc(s_peer_size);
    s_token = malloc(s_token_size);
    if (!s_peer || !s_token) {
        rtc_connect_inputs_release();
        return TIRTC_E_LACK_OF_RESOURCE;
    }
    memcpy(s_peer, peer, s_peer_size);
    memcpy(s_token, token, s_token_size);
    atomic_store(&s_generation, gen);
    int rc = TiRtcWhipConnect(s_peer, s_token, connected, (void *)(uintptr_t)gen);
    if (rc)
        rtc_connect_inputs_release();
    return rc;
}
int rtc_disconnect(tirtc_conn_t connection)
{
    return TiRtcDisconnect(connection);
}
int rtc_stop(void)
{
    return s_initialized ? TiRtcStop() : TIRTC_E_NOT_INITIALIZED;
}
void rtc_uninit(void)
{
    if (s_initialized)
        TiRtcUninit();
    s_initialized = false;
    rtc_connect_inputs_release();
}
int rtc_send_command(tirtc_conn_t connection, uint32_t command, const char *json)
{
    return TiRtcSendCommand(connection, command, json, strlen(json));
}
int rtc_service_request(const char *path, const char *json, TIRTCSERVICEREQUESTCALLBACK callback)
{
    return TiRtcServiceRequest(path, json, NULL, callback, NULL);
}
int rtc_send_audio(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, const void *data)
{
    return TiRtcSendAudioStream(connection, frame, data);
}
size_t rtc_send_buffer_used(tirtc_conn_t connection)
{
    return TiRtcGetSendBufferUsed(connection);
}
