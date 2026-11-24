// SPDX-License-Identifier: MIT
#include <limits.h>
#include <string.h>
#include "tirtc_internal.h"
#include "xiaotai/port/log.h"

typedef struct {
    xiaotai_link_media_kind_t kind;
    xiaotai_link_format_t format;
    uint8_t media, flags;
} frame_format_t;

static const frame_format_t formats[] = {
    {XIAOTAI_LINK_AUDIO, {XIAOTAI_LINK_CODEC_G711A, 8000, 1}, TIRTC_AUDIO_ALAW, TIRTC_AUDIOSAMPLE_8K16B1C},
    {XIAOTAI_LINK_AUDIO, {XIAOTAI_LINK_CODEC_OPUS, 16000, 1}, TIRTC_AUDIO_OPUS, TIRTC_AUDIOSAMPLE_16K16B1C},
    {XIAOTAI_LINK_VIDEO, {XIAOTAI_LINK_CODEC_JPEG, 0, 0}, TIRTC_VIDEO_JPEG, 0},
    {XIAOTAI_LINK_VIDEO, {XIAOTAI_LINK_CODEC_H264, 0, 0}, TIRTC_VIDEO_H264, 0},
};

static const frame_format_t *find_format(xiaotai_link_media_kind_t kind, const xiaotai_link_format_t *f)
{
    if (!f) return NULL;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
        if (formats[i].kind == kind && formats[i].format.codec == f->codec &&
            formats[i].format.rate_hz == f->rate_hz && formats[i].format.channels == f->channels)
            return &formats[i];
    return NULL;
}

bool xiaotai_rtc_format_supported(xiaotai_link_media_kind_t kind, const xiaotai_link_format_t *format)
{
    return find_format(kind, format) != NULL;
}

#define STRINGIFY_RAW(v) #v
#define STRINGIFY(v) STRINGIFY_RAW(v)

const xiaotai_rtc_caps_t *xiaotai_rtc_caps(void)
{
    static const xiaotai_rtc_caps_t caps = {
        .version = "TiRTC " STRINGIFY(TIRTC_VERSION_MAJOR) "." STRINGIFY(TIRTC_VERSION_MINOR) "." STRINGIFY(TIRTC_VERSION_PATCH),
        .https = TIRTC_SDK_SSL_SUPPORT, .cancel_pending = XT_TIRTC_CANCEL,
#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER) && XT_TIRTC_CANCEL
        .published_retirement_barrier = true,
#endif
        .media_transport = XT_TIRTC_MEDIA_TRANSPORT, .stack_log = true, .video = true,
        .statistics = XT_TIRTC_TGTRP, .send_stack_bytes = XIAOTAI_RTC_SEND_STACK_UNMEASURED,
    };
    return &caps;
}

static xiaotai_link_end_t end_reason(int rc, bool connecting)
{
    if (rc == TIRTC_E_CONN_REMOTECLOSE) return XIAOTAI_LINK_END_PEER;
    const xiaotai_err_t status = xt_tirtc_status(rc);
    if (status == XIAOTAI_ERR_TIMEOUT) return XIAOTAI_LINK_END_TIMEOUT;
    if (status == XIAOTAI_ERR_NO_MEM) return XIAOTAI_LINK_END_NO_MEMORY;
    return connecting ? XIAOTAI_LINK_END_CONNECT_FAILED : XIAOTAI_LINK_END_NETWORK;
}

static void system_event(int code, const void *data, int length)
{
    (void)data;
    (void)length;
    switch (code) {
    case TIRTC_EVENT_SYS_STARTED: xiaotai_rtc_on_system(XIAOTAI_RTC_STARTED, 0); break;
    case TIRTC_EVENT_SYS_STOPPED: xiaotai_rtc_on_system(XIAOTAI_RTC_STOPPED, 0); break;
    case TIRTC_EVENT_ACCESS_HIJACKING: xiaotai_rtc_on_system(XIAOTAI_RTC_ALERT, TIRTC_E_UNEXPECTED_RESPONSE); break;
    default: break;
    }
}

#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER) && XT_TIRTC_CANCEL
static bool tag_peer_retirement(xiaotai_rtc_conn_t conn, uint32_t generation)
{
    return TiRtcConnSetUserData((tirtc_conn_t)conn, (void *)(uintptr_t)generation) == 0;
}

static void accepted(tirtc_conn_t conn)
{
    xiaotai_rtc_on_accepted_peer((uintptr_t)conn, tag_peer_retirement);
}
#else
static void accepted(tirtc_conn_t conn) { xiaotai_rtc_on_accepted((uintptr_t)conn); }
#endif
static void closed(tirtc_conn_t conn)
{
#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER) && XT_TIRTC_CANCEL
    /* The handle stays valid only until this SDK callback returns. */
    const uint32_t generation = conn ? (uint32_t)(uintptr_t)TiRtcConnGetUserData(conn) : 0;
    if (generation) {
        xiaotai_rtc_on_closed_for((uintptr_t)conn, generation);
        return;
    }
#endif
    xiaotai_rtc_on_closed((uintptr_t)conn);
}
static void error(tirtc_conn_t conn, int rc) { xiaotai_rtc_on_error((uintptr_t)conn, end_reason(rc, false), rc); }

static void connected(int rc, tirtc_conn_t conn, void *user)
{
    xiaotai_rtc_on_connected((uint32_t)(uintptr_t)user, (uintptr_t)conn, end_reason(rc, true), rc);
}

#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER) && XT_TIRTC_CANCEL
static void connected_peer(int rc, tirtc_conn_t conn, void *user)
{
    xiaotai_rtc_on_connected_peer((uint32_t)(uintptr_t)user, (uintptr_t)conn,
                                 end_reason(rc, true), rc, tag_peer_retirement);
}

static void connected_whip(int rc, tirtc_conn_t conn, void *user)
{
    /* Use the SDK callback's object lifetime, never a saved handle. A
     * missing tag cannot certify release in the transport worker. */
    if (rc == 0 && conn && user) (void)TiRtcConnSetUserData(conn, user);
    connected(rc, conn, user);
}
#else
#define connected_peer connected
#define connected_whip connected
#endif

static void media(tirtc_conn_t conn, const TIRTCFRAMEINFO *wire, void *data, xiaotai_link_media_kind_t kind)
{
    if (!wire || !data || !wire->length) return;
    xiaotai_link_frame_t frame = {.kind = kind, .stream = wire->stream_id, .timestamp_ms = wire->ts,
        .key_frame = kind == XIAOTAI_LINK_VIDEO && TIRTC_IS_KEY_FRAME(wire->flags)};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        if (formats[i].kind == kind && formats[i].media == wire->media &&
            (kind == XIAOTAI_LINK_VIDEO || formats[i].flags == wire->flags)) {
            frame.format = formats[i].format;
            break;
        }
    }
    xiaotai_rtc_on_frame((uintptr_t)conn, &frame, data, wire->length);
}

static void audio(tirtc_conn_t c, const TIRTCFRAMEINFO *f, void *data) { media(c, f, data, XIAOTAI_LINK_AUDIO); }
static void video(tirtc_conn_t c, const TIRTCFRAMEINFO *f, void *data) { media(c, f, data, XIAOTAI_LINK_VIDEO); }

static void message(tirtc_conn_t conn, const TIRTCFRAMEINFO *frame, void *data)
{
    (void)conn;
    (void)frame;
    (void)data;
    xt_tirtc_log_message();
}

static uint32_t connection_request(tirtc_conn_t conn)
{
    /* The stack may not publish user_data yet on a pre-result callback. */
    return conn ? (uint32_t)(uintptr_t)TiRtcConnGetUserData(conn) : 0;
}

static void command(tirtc_conn_t conn, uint32_t word, const void *data, uint32_t size)
{
    xiaotai_rtc_on_command((uintptr_t)conn, connection_request(conn), word, data, size);
}

static int subscribe(tirtc_conn_t conn, uint8_t stream, xiaotai_link_media_kind_t kind, bool on)
{
    const xiaotai_err_t rc = xiaotai_rtc_on_subscribe((uintptr_t)conn, connection_request(conn), kind, stream, on);
    if (rc == XIAOTAI_ERR_BUSY) return TIRTC_E_BUSY;
    return rc == XIAOTAI_OK ? 0 : TIRTC_E_INVALID_PARAMETER;
}

static int subscribe_audio(tirtc_conn_t c, uint8_t stream) { return subscribe(c, stream, XIAOTAI_LINK_AUDIO, true); }
static int subscribe_video(tirtc_conn_t c, uint8_t stream) { return subscribe(c, stream, XIAOTAI_LINK_VIDEO, true); }
static void unsubscribe_audio(tirtc_conn_t c, uint8_t stream) { (void)subscribe(c, stream, XIAOTAI_LINK_AUDIO, false); }
static void unsubscribe_video(tirtc_conn_t c, uint8_t stream) { (void)subscribe(c, stream, XIAOTAI_LINK_VIDEO, false); }
static void key_frame(tirtc_conn_t c, uint8_t stream) { xiaotai_rtc_on_key_frame_request((uintptr_t)c, stream); }
static void bitrate(tirtc_conn_t c, uint8_t stream, uint32_t bps) { xiaotai_rtc_on_video_bitrate((uintptr_t)c, stream, bps); }

const TIRTCCALLBACKS xt_tirtc_callbacks = {
    .on_event = system_event, .on_conn_accepted = accepted, .on_conn_error = error, .on_disconnected = closed,
    .on_audio = audio, .on_video = video, .on_message = message, .on_command = command,
    .on_request_key_frame = key_frame, .on_subscribe_video = subscribe_video, .on_unsubscribe_video = unsubscribe_video,
    .on_subscribe_audio = subscribe_audio, .on_unsubscribe_audio = unsubscribe_audio, .on_update_bitrate = bitrate,
};

xiaotai_err_t xiaotai_rtc_connect(xiaotai_link_kind_t kind, const char *peer, const char *token, uint32_t request,
                                  int *detail)
{
    int rc = TIRTC_E_INVALID_PARAMETER;
    if (request && peer && *peer) {
        if (kind == XIAOTAI_LINK_WHIP) rc = TiRtcWhipConnect(peer, token, connected_whip, (void *)(uintptr_t)request);
        else if (kind == XIAOTAI_LINK_P2P) rc = TiRtcConnect(peer, token, connected_peer, (void *)(uintptr_t)request);
    }
    if (detail) *detail = rc;
    return xt_tirtc_status(rc);
}

xiaotai_err_t xiaotai_rtc_cancel(uint32_t request)
{
#if XT_TIRTC_CANCEL
    if (!request) return XIAOTAI_ERR_INVALID_ARG;
    const int rc = TiRtcCancelPending((void *)(uintptr_t)request, 0);
    return rc < 0 ? xt_tirtc_status(rc) : XIAOTAI_OK;
#else
    (void)request;
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}

uint32_t xiaotai_rtc_pending(void)
{
#if XT_TIRTC_CANCEL
    return TiRtcPendingConnections();
#else
    return 0;
#endif
}

xiaotai_err_t xiaotai_rtc_disconnect(xiaotai_rtc_conn_t conn, int *detail)
{
    const int rc = conn ? TiRtcDisconnect((tirtc_conn_t)conn) : TIRTC_E_INVALID_HANDLE;
    if (detail) *detail = rc;
    return xt_tirtc_status(rc);
}

xiaotai_err_t xiaotai_rtc_send(xiaotai_rtc_conn_t conn, const xiaotai_link_frame_t *frame, const uint8_t *data,
                               size_t size, int *detail)
{
    int rc = TIRTC_E_INVALID_PARAMETER;
    const frame_format_t *format = frame ? find_format(frame->kind, &frame->format) : NULL;
    if (!conn) rc = TIRTC_E_INVALID_HANDLE;
    else if (format && frame->stream <= XIAOTAI_LINK_STREAM_MAX && data && size && size <= UINT32_MAX) {
        const TIRTCFRAMEINFO wire = {.stream_id = frame->stream, .media = format->media,
            .flags = frame->kind == XIAOTAI_LINK_AUDIO ? format->flags : frame->key_frame ? TIRTC_FRAME_FLAG_KEY_FRAME : 0,
            .ts = frame->timestamp_ms, .length = (uint32_t)size};
        rc = frame->kind == XIAOTAI_LINK_AUDIO ? TiRtcSendAudioStream((tirtc_conn_t)conn, &wire, data)
                                            : TiRtcSendVideoStream((tirtc_conn_t)conn, &wire, data);
    }
    if (rc > 0) {
        if (detail) *detail = 0;
        return XIAOTAI_OK;
    }
    if (!rc) rc = TIRTC_E_UNEXPECTED_RESPONSE;
    if (detail) *detail = rc;
    if (rc == TIRTC_E_BUSY) return XIAOTAI_ERR_BUSY;
    if (rc == TIRTC_E_INVALID_HANDLE || rc == TIRTC_E_NOT_INITIALIZED) return XIAOTAI_ERR_INVALID_STATE;
    return XIAOTAI_FAIL;
}

size_t xiaotai_rtc_backlog(xiaotai_rtc_conn_t conn)
{
    return conn ? TiRtcGetSendBufferUsed((tirtc_conn_t)conn) : 0;
}

xiaotai_err_t xiaotai_rtc_command(xiaotai_rtc_conn_t conn, uint32_t word, const void *data, size_t size)
{
    if (!conn) return XIAOTAI_ERR_INVALID_STATE;
    if (size > UINT32_MAX || (size && !data)) return XIAOTAI_ERR_INVALID_ARG;
    const int rc = TiRtcSendCommand((tirtc_conn_t)conn, word, data, (uint32_t)size);
    return rc > 0 ? XIAOTAI_OK : rc ? xt_tirtc_status(rc) : XIAOTAI_FAIL;
}

xiaotai_err_t xiaotai_rtc_subscribe(xiaotai_rtc_conn_t conn, xiaotai_link_media_kind_t kind, uint8_t stream, bool on)
{
    if (!conn) return XIAOTAI_ERR_INVALID_STATE;
    if (stream > XIAOTAI_LINK_STREAM_MAX) return XIAOTAI_ERR_INVALID_ARG;
    tirtc_conn_t handle = (tirtc_conn_t)conn;
    const bool audio = kind == XIAOTAI_LINK_AUDIO;
    if (!audio && kind != XIAOTAI_LINK_VIDEO) return XIAOTAI_ERR_INVALID_ARG;
    const int rc = audio ? (on ? TiRtcSubscribeAudio(handle, stream) : TiRtcUnsubscribeAudio(handle, stream))
                         : (on ? TiRtcSubscribeVideo(handle, stream) : TiRtcUnsubscribeVideo(handle, stream));
    const char *operation = on ? "subscribe" : "unsubscribe";
    const char *media = audio ? "audio" : "video";
    if (rc < 0) {
        XIAOTAI_LOGW("transport", "%s %s stream=%u failed sdk_rc=%d", operation, media, (unsigned)stream, rc);
        return xt_tirtc_status(rc);
    }
    /* The subscription APIs define nonnegative results as success. */
    if (rc > 0)
        XIAOTAI_LOGD("transport", "%s %s stream=%u accepted sdk_rc=%d", operation, media, (unsigned)stream, rc);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_rtc_request_key_frame(xiaotai_rtc_conn_t conn, uint8_t stream)
{
    if (!conn) return XIAOTAI_ERR_INVALID_STATE;
    if (stream > XIAOTAI_LINK_STREAM_MAX) return XIAOTAI_ERR_INVALID_ARG;
    const int rc = TiRtcRequestKeyFrame((tirtc_conn_t)conn, stream);
    return rc < 0 ? xt_tirtc_status(rc) : XIAOTAI_OK;
}

xiaotai_err_t xiaotai_rtc_set_video_bitrate(xiaotai_rtc_conn_t conn, uint8_t stream, uint32_t min, uint32_t start, uint32_t max)
{
    if (!conn) return XIAOTAI_ERR_INVALID_STATE;
    if (stream > XIAOTAI_LINK_STREAM_MAX || !min || min > start || start > max) return XIAOTAI_ERR_INVALID_ARG;
    return xt_tirtc_status(TiRtcConnSetVideoBitrateParams((tirtc_conn_t)conn, stream, min, max, start));
}

typedef struct { char *body; size_t capacity, size; } service_reply_t;

static void service_reply(const char *body, void *ctx)
{
    service_reply_t *reply = ctx;
    if (!body || !reply->capacity) return;
    const size_t n = strnlen(body, reply->capacity);
    if (n < reply->capacity) {
        memcpy(reply->body, body, n + 1U);
        reply->size = n;
    }
}

xiaotai_err_t xiaotai_rtc_service_request(const char *path, const char *json, int *status, char *body,
                                          size_t capacity, size_t *size)
{
    if (size) *size = 0;
    if (status) *status = 0;
    if (!path || !*path || (capacity && !body)) return XIAOTAI_ERR_INVALID_ARG;
    if (capacity) body[0] = '\0';
    service_reply_t reply = {.body = body, .capacity = capacity};
    const int rc = TiRtcServiceRequest(path, json, NULL, service_reply, &reply);
    if (status) *status = rc;
    if (rc) {
        if (capacity) body[0] = '\0';
        return XIAOTAI_FAIL;
    }
    if (size) *size = reply.size;
    return XIAOTAI_OK;
}
