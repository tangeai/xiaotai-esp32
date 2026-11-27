// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_rtc/test_rtc.c::rtc
// Replaces legacy: tests/native/p4/test_tirtc_endpoint.c
// Replaces legacy: tests/regression/test_s3_discovered_endpoint.py
// Replaces legacy: tests/regression/test_call_hangup.py
// Replaces legacy: tests/native/cores3/test_h5_video_send.py
// Replaces legacy: tests/regression/test_h5_video_send.py
/* Use shipped SDK headers for exact ABI types but define independent, strict
 * function doubles. The Mini cancel variant links the real transport core. */
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "transport_backend.h"
#include "xiaotai/hal.h"
#include "xiaotai/net.h"
#include "tirtc/tiRTC.h"

/* Where the port is not linked into the test, every line passes the gate. */
__attribute__((weak)) bool xiaotai_log_enabled(uint8_t level, const char *tag)
{
    (void)level;
    (void)tag;
    return true;
}
#if __has_include("tirtc/tgtrp.h")
#include "tirtc/tgtrp.h"
#define FIXTURE_TGTRP 1
#else
#define FIXTURE_TGTRP 0
#endif

enum {INIT = 90, START, STOP, UNINIT, ENABLE};
static struct {
    unsigned trace[32], count;
    unsigned fail_step;
    int fail_code, result, mode, log_level;
    int connection_limit, cache, network, poll;
    uint32_t buffer, key_size, client_size, endpoint_size;
    char endpoint[128];
    bool initialized, started, stopping;
    bool close_pending, close_inline;
    const TIRTCCALLBACKS *cb;
    TIRTCCONNECTCALLBACK connected;
    void *tag;
    TIRTCLOGCALLBACK log;
#if FIXTURE_TGTRP
    tgtrp_log_cb_t trace_log;
#endif
    unsigned disconnects, closed, system, accepted, failures, connection_results, frames, commands;
    unsigned subscriptions, key_requests, rate_updates, sends, downcall, traces;
    uint32_t request, closed_generation, word, minimum, maximum, initial;
#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER)
    uint32_t inbound_peer_generation;
    bool outbound_peer_tracking, reject_peer_tag, peer_tag_written;
    unsigned peer_tag_calls;
#endif
    int detail;
    xiaotai_link_end_t reason;
    xiaotai_rtc_system_t event;
    xiaotai_rtc_conn_t conn;
    xiaotai_link_frame_t frame;
    TIRTCFRAMEINFO wire;
    xiaotai_net_trace_transport_t sample;
    const void *payload;
    size_t payload_size;
    xiaotai_err_t admit;
} f;

#ifdef XIAOTAI_TEST_MINI_CANCEL
static atomic_uint mini_pending, mini_cancels, mini_polls, mini_connects;
#endif

struct _tirtc_conn { void *tag; } conn_a, conn_b;
static const uint8_t packet[] = {3, 1, 4};
#ifdef XIAOTAI_TEST_S3_JPEG
static const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
static uint8_t expected_jpeg_stream;
#endif

static int call(unsigned step)
{
    assert(f.count < sizeof(f.trace) / sizeof(f.trace[0]));
    f.trace[f.count++] = step;
    return step == f.fail_step ? f.fail_code : 0;
}

int TiRtcInit(void)
{
    assert(!f.initialized);
    const int rc = call(INIT);
    f.initialized = rc == 0;
    return rc;
}

int TiRtcStart(const char *id, const TIRTCCALLBACKS *callbacks)
{
    assert(f.initialized && !f.started && !strcmp(id, "fixture-device") && callbacks);
    const int rc = call(START);
    if (rc) {
        if (f.log && f.mode) f.log("/v1/start, status 503", 21);
        return rc;
    }
    f.cb = callbacks;
    f.started = true;
    callbacks->on_event(TIRTC_EVENT_SYS_STARTED, NULL, 0);
    return 0;
}

static void finish_stop(void);

int TiRtcStop(void)
{
    assert(f.initialized && f.started);
    const int rc = call(STOP);
    f.stopping = rc == 0;
#ifdef XIAOTAI_TEST_MINI_CANCEL
    if (f.stopping) finish_stop();
#endif
    return rc;
}

static void finish_stop(void)
{
    assert(f.stopping);
    f.stopping = f.started = false;
    f.cb->on_event(TIRTC_EVENT_SYS_STOPPED, NULL, 0);
}

void TiRtcUninit(void)
{
    assert(f.initialized && !f.started && !f.stopping);
    (void)call(UNINIT);
    f.initialized = false;
}

int TiRtcSetOption(TIRTCOPTION opt, const void *data, uint32_t len)
{
    assert(data && len && !f.started);
    int *field = NULL;
    switch (opt) {
    case TIRTC_OPT_MAX_CONNECTIONS: assert(!f.initialized); field = &f.connection_limit; break;
    case TIRTC_OPT_CONNECT_CACHE: assert(!f.initialized); field = &f.cache; break;
    case TIRTC_OPT_NETWORK_TYPE: assert(f.initialized); field = &f.network; break;
    case TIRTC_OPT_TGTRP_POLL_TIMEOUT: assert(f.initialized); field = &f.poll; break;
    case TIRTC_OPT_MAX_SEND_BUFFER: assert(!f.initialized && len == sizeof(f.buffer)); memcpy(&f.buffer, data, len); break;
    case TIRTC_OPT_DEVICE_SECRET_KEY: assert(f.initialized && !strcmp(data, "fixture-secret")); f.key_size = len; break;
    case TIRTC_OPT_CLIENT_ID: assert(f.initialized && !strcmp(data, "fixture-client")); f.client_size = len; break;
    case TIRTC_OPT_SERVICE_ENDPOINT:
        assert(f.initialized && len < sizeof(f.endpoint));
        f.endpoint_size = len;
        memcpy(f.endpoint, data, len);
        break;
    default: assert(false);
    }
    if (field) { assert(len == sizeof(*field)); memcpy(field, data, len); }
    const int rc = call((unsigned)opt);
    return !rc && opt == TIRTC_OPT_TGTRP_POLL_TIMEOUT ? f.poll + 1 : rc;
}

void TiRtcLogSetLevel(int level) { f.mode = level; }
void TiRtcLogSetCallback(TIRTCLOGCALLBACK callback) { f.log = callback; }
#if FIXTURE_TGTRP
void tgtrp_set_log_level(int level) { f.log_level = level; }
void tgtrp_set_log_callback(tgtrp_log_cb_t callback) { f.trace_log = callback; }
#ifndef TIRTC_PENDING_CLOSE_CONTRACT
void tgtrp_enable_media_transport(void) { (void)call(ENABLE); }
#endif
#endif
#ifdef TIRTC_PENDING_CLOSE_CONTRACT
unsigned TiRtcPendingConnections(void)
{
#ifdef XIAOTAI_TEST_MINI_CANCEL
    atomic_fetch_add(&mini_polls, 1U);
    return atomic_load(&mini_pending);
#else
    return f.request;
#endif
}
int TiRtcCancelPending(void *user, int cancel_inbound)
{
    assert(user == f.tag && cancel_inbound == 0);
#ifdef XIAOTAI_TEST_MINI_CANCEL
    atomic_fetch_add(&mini_cancels, 1U);
#endif
    return f.result;
}
#endif

static int connect_as(unsigned method, const char *peer, const char *token, TIRTCCONNECTCALLBACK cb, void *user)
{
    assert(f.started && !strcmp(peer, "fixture-peer") && !strcmp(token, "fixture-token") && cb && user);
    f.downcall = method;
    f.tag = user;
    f.connected = cb;
#ifdef XIAOTAI_TEST_MINI_CANCEL
    atomic_fetch_add(&mini_connects, 1U);
#endif
    return f.result;
}

int TiRtcWhipConnect(const char *peer, const char *token, TIRTCCONNECTCALLBACK cb, void *user)
{ return connect_as(1, peer, token, cb, user); }
int TiRtcConnect(const char *peer, const char *token, TIRTCCONNECTCALLBACK cb, void *user)
{ return connect_as(2, peer, token, cb, user); }
static void finish_disconnect(void)
{
    assert(f.close_pending);
    f.cb->on_disconnected(&conn_a);
    f.close_pending = false;
    conn_a.tag = NULL; /* The SDK's connection is no longer accessible. */
}

int TiRtcDisconnect(tirtc_conn_t conn)
{
    /* An otherwise valid connection may have no application tag. */
    assert(conn == &conn_a && !f.close_pending);
    ++f.disconnects;
    if (!f.result) {
        f.close_pending = true;
        if (f.close_inline) finish_disconnect();
    }
    return f.result;
}
size_t TiRtcGetSendBufferUsed(tirtc_conn_t conn) { assert(conn == &conn_a); return 123; }
void *TiRtcConnGetUserData(tirtc_conn_t conn) { return conn->tag; }
int TiRtcConnSetUserData(tirtc_conn_t conn, void *tag)
{
    assert(conn == &conn_a);
#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER)
    ++f.peer_tag_calls;
    if (f.reject_peer_tag) return TIRTC_E_INVALID_HANDLE;
#endif
    conn->tag = tag;
    return 0;
}

static int send(unsigned method, tirtc_conn_t conn, const TIRTCFRAMEINFO *wire, const void *data)
{
    assert(f.started && conn == &conn_a && !f.close_pending && wire);
#ifdef XIAOTAI_TEST_S3_JPEG
    if (data == jpeg) {
        assert(method == 4 && wire->stream_id == expected_jpeg_stream && wire->media == TIRTC_VIDEO_JPEG);
        assert(wire->flags == TIRTC_FRAME_FLAG_KEY_FRAME && wire->ts == 123 && wire->length == sizeof(jpeg));
    } else
#endif
    {
        assert(data == packet);
    }
    f.downcall = method;
    f.wire = *wire;
    ++f.sends;
    return f.result;
}

int TiRtcSendAudioStream(tirtc_conn_t conn, const TIRTCFRAMEINFO *wire, const void *data)
{ return send(3, conn, wire, data); }
int TiRtcSendVideoStream(tirtc_conn_t conn, const TIRTCFRAMEINFO *wire, const void *data)
{ return send(4, conn, wire, data); }
int TiRtcSendCommand(tirtc_conn_t conn, uint32_t word, const void *data, uint32_t size)
{
    assert(conn == &conn_a && word == 0xdead0001U && data == packet && size == sizeof(packet));
    return f.result;
}

static int control(unsigned method, tirtc_conn_t conn, uint8_t stream)
{
    assert(conn == &conn_a && stream == 5);
    f.downcall = method;
    return f.result;
}

int TiRtcSubscribeAudio(tirtc_conn_t conn, uint8_t stream) { return control(5, conn, stream); }
int TiRtcUnsubscribeAudio(tirtc_conn_t conn, uint8_t stream) { return control(6, conn, stream); }
int TiRtcSubscribeVideo(tirtc_conn_t conn, uint8_t stream) { return control(7, conn, stream); }
int TiRtcUnsubscribeVideo(tirtc_conn_t conn, uint8_t stream) { return control(8, conn, stream); }
int TiRtcRequestKeyFrame(tirtc_conn_t conn, uint8_t stream) { return control(9, conn, stream); }
int TiRtcConnSetVideoBitrateParams(tirtc_conn_t conn, uint8_t stream, uint32_t min, uint32_t max, uint32_t start)
{
    f.minimum = min; f.maximum = max; f.initial = start;
    return control(10, conn, stream);
}

int TiRtcServiceRequest(const char *path, const char *json, const char *token,
                       TIRTCSERVICEREQUESTCALLBACK cb, void *ctx)
{
    assert(!strcmp(path, "/fixture") && !json && !token && cb && ctx);
    char reply[] = "{\"code\":0}";
    cb(reply, ctx);
    memset(reply, '?', sizeof(reply));
    return f.result;
}

#ifdef XIAOTAI_TEST_MINI_CANCEL
#include "mini_cancel_fixture.h"
#else
void xiaotai_rtc_on_system(xiaotai_rtc_system_t event, int detail)
{ ++f.system; f.event = event; f.detail = detail; }
void xiaotai_rtc_on_accepted(xiaotai_rtc_conn_t conn) { ++f.accepted; f.conn = conn; }
void xiaotai_rtc_on_closed(xiaotai_rtc_conn_t conn) { ++f.closed; f.conn = conn; f.closed_generation = 0; }
void xiaotai_rtc_on_closed_for(xiaotai_rtc_conn_t conn, uint32_t generation)
{ ++f.closed; f.conn = conn; f.closed_generation = generation; }
void xiaotai_rtc_on_error(xiaotai_rtc_conn_t conn, xiaotai_link_end_t reason, int detail)
{ ++f.failures; f.conn = conn; f.reason = reason; f.detail = detail; }
void xiaotai_rtc_on_connected(uint32_t req, xiaotai_rtc_conn_t conn, xiaotai_link_end_t reason, int detail)
{ ++f.connection_results; f.request = req; f.conn = conn; f.reason = reason; f.detail = detail; }
#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER)
void xiaotai_rtc_on_accepted_peer(xiaotai_rtc_conn_t conn, xiaotai_rtc_peer_tag_fn tag)
{
    xiaotai_rtc_on_accepted(conn);
    assert(tag);
    f.peer_tag_written = f.inbound_peer_generation != 0 && tag(conn, f.inbound_peer_generation);
}

void xiaotai_rtc_on_connected_peer(uint32_t req, xiaotai_rtc_conn_t conn, xiaotai_link_end_t reason, int detail,
                                  xiaotai_rtc_peer_tag_fn tag)
{
    xiaotai_rtc_on_connected(req, conn, reason, detail);
    assert(tag);
    f.peer_tag_written = f.outbound_peer_tracking && req && conn && !detail && tag(conn, req);
}
#endif
void xiaotai_rtc_on_frame(xiaotai_rtc_conn_t conn, const xiaotai_link_frame_t *frame, const uint8_t *data, size_t size)
{ ++f.frames; f.conn = conn; f.frame = *frame; f.payload = data; f.payload_size = size; }
void xiaotai_rtc_on_command(xiaotai_rtc_conn_t conn, uint32_t req, uint32_t word, const uint8_t *data, size_t size)
{ ++f.commands; f.conn = conn; f.request = req; f.word = word; f.payload = data; f.payload_size = size; }
xiaotai_err_t xiaotai_rtc_on_subscribe(xiaotai_rtc_conn_t conn, uint32_t req, xiaotai_link_media_kind_t kind,
                                       uint8_t stream, bool subscribed)
{
    ++f.subscriptions; f.conn = conn; f.request = req; f.frame.kind = kind; f.frame.stream = stream;
    f.frame.key_frame = subscribed;
    return f.admit;
}
void xiaotai_rtc_on_key_frame_request(xiaotai_rtc_conn_t conn, uint8_t stream)
{ ++f.key_requests; f.conn = conn; f.frame.stream = stream; }
void xiaotai_rtc_on_video_bitrate(xiaotai_rtc_conn_t conn, uint8_t stream, uint32_t bps)
{ ++f.rate_updates; f.conn = conn; f.frame.stream = stream; f.initial = bps; }
void xiaotai_net_trace_transport(const xiaotai_net_trace_transport_t *sample)
{ ++f.traces; f.sample = *sample; }

static xiaotai_rtc_start_t defaults(void)
{
    return (xiaotai_rtc_start_t){.device_id = "fixture-device", .device_secret = "fixture-secret",
        .client_id = "fixture-client", .endpoint = "http://test.invalid", .send_buffer_bytes = 16384,
        .links = 2, .connect_cache = XIAOTAI_TRANSPORT_OFF, .stack_log_level = 3, .poll_timeout_ms = 25};
}

static void shutdown(void)
{
    int detail;
    const unsigned sys = f.system;
    assert(xiaotai_rtc_stop(&detail) == XIAOTAI_OK && detail == 0 && f.system == sys);
    finish_stop();
    assert(f.system == sys + 1 && f.event == XIAOTAI_RTC_STOPPED);
    xiaotai_rtc_uninit();
    assert(!f.initialized && f.trace[f.count - 1] == UNINIT);
    const unsigned count = f.count;
    xiaotai_rtc_uninit();
    assert(f.count == count);
}

static void options(void)
{
    const xiaotai_rtc_caps_t *cap = xiaotai_rtc_caps();
    assert(cap->https == TIRTC_SDK_SSL_SUPPORT && cap->video && cap->stack_log && cap->statistics == FIXTURE_TGTRP);
    assert(cap->send_stack_bytes == 6656 && cap->version && *cap->version);
#ifdef TIRTC_PENDING_CLOSE_CONTRACT
    assert(cap->cancel_pending && !cap->media_transport);
#else
    assert(!cap->cancel_pending && cap->media_transport == FIXTURE_TGTRP);
#endif
    for (unsigned combo = 0; combo < 2; ++combo) {
        memset(&f, 0, sizeof(f));
        xiaotai_rtc_start_t config = defaults();
        config.option_strings_without_nul = combo;
        config.media_transport_before_init = combo;
        config.media_transport = cap->media_transport ? XIAOTAI_TRANSPORT_ON : XIAOTAI_TRANSPORT_OFF;
        config.network_kind = XIAOTAI_NETWORK_WIFI_COPROCESSOR;
        int detail;
        assert(xiaotai_rtc_start(&config, &detail) == XIAOTAI_OK && !detail);
        const unsigned head[] = {TIRTC_OPT_MAX_SEND_BUFFER, TIRTC_OPT_MAX_CONNECTIONS, TIRTC_OPT_CONNECT_CACHE};
        assert(!memcmp(head, f.trace, sizeof(head)));
        unsigned at = 3;
        if (cap->media_transport && combo) assert(f.trace[at++] == ENABLE);
        assert(f.trace[at++] == INIT);
        if (cap->media_transport && !combo) assert(f.trace[at++] == ENABLE);
        const unsigned tail[] = {TIRTC_OPT_SERVICE_ENDPOINT, TIRTC_OPT_DEVICE_SECRET_KEY,
            TIRTC_OPT_CLIENT_ID, TIRTC_OPT_NETWORK_TYPE, TIRTC_OPT_TGTRP_POLL_TIMEOUT, START};
        assert(f.count == at + sizeof(tail) / sizeof(tail[0]) && !memcmp(tail, f.trace + at, sizeof(tail)));
        assert(f.buffer == 16384 && f.connection_limit == 2 && !f.cache && f.poll == 25 && f.network == TIRTC_NETCONN_WIFI);
        assert(f.key_size == strlen(config.device_secret) + !combo && f.client_size == strlen(config.client_id) + !combo);
        assert(f.endpoint_size == strlen(config.endpoint) + !combo);
        assert(f.system == 1 && f.event == XIAOTAI_RTC_STARTED);
        assert(xiaotai_rtc_start(&config, &detail) == XIAOTAI_ERR_BUSY && detail == TIRTC_E_BUSY);
        shutdown();
    }

    const struct {const char *in, *http;} urls[] = {
        {"https://ep-tirtc.tange365.com", "http://ep-tirtc.tange365.com"},
        {"https://ep-tirtc.tange365.com/", "http://ep-tirtc.tange365.com/"},
        {"HTTPS://EP-TIRTC.TANGE365.COM", "http://ep-tirtc.tange365.com"},
        {"HTTPS://EP-TIRTC.TANGE365.COM/", "http://ep-tirtc.tange365.com/"},
        {"https://ep-pre-tirtc.tange365.com", "http://ep-pre-tirtc.tange365.com"},
        {"https://ep-pre-tirtc.tange365.com/", "http://ep-pre-tirtc.tange365.com/"},
        {NULL, NULL}, {"", NULL},
        {"http://rtc.invalid:8080/tenant", NULL}, {"http://[::1]:8090/tenant", NULL},
        {"http://ep-tirtc.tange365.com", NULL}, {"http://ep-pre-tirtc.tange365.com/", NULL},
        {"https://custom.example", NULL}, {"ftp://ep-tirtc.tange365.com", NULL},
        {" https://ep-tirtc.tange365.com", NULL},
        {"https://ep-tirtc.tange365.com:443", NULL}, {"https://ep-tirtc.tange365.com/path", NULL},
        {"https://ep-tirtc.tange365.com?x=1", NULL}, {"https://name@ep-tirtc.tange365.com", NULL},
        {"https://ep-tirtc.tange365.com@custom.example", NULL},
        {"https://ep-tirtc.tange365.com.invalid", NULL}, {"https://ep-tirtc.tange365.com/#x", NULL},
        {"https://ep-tirtc.tange365.com#x", NULL},
    };
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); ++i) {
        xiaotai_rtc_start_t config = defaults();
        config.endpoint = urls[i].in;
        for (unsigned rewrite = 0; rewrite < 2; ++rewrite) {
            memset(&f, 0, sizeof(f));
            config.endpoint_http_rewrite = rewrite;
            assert(xiaotai_rtc_start(&config, NULL) == XIAOTAI_OK);
            if (urls[i].in && *urls[i].in)
                assert(!strcmp(f.endpoint, rewrite && !cap->https && urls[i].http ? urls[i].http : urls[i].in));
            else assert(!f.endpoint_size);
            shutdown();
        }
    }
    const char *builtin[] = {"", "http://ep-tirtc.tange365.com", "https://ep-tirtc.tange365.com"};
    for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); ++i) {
        memset(&f, 0, sizeof(f));
        xiaotai_rtc_start_t config = defaults();
        config.endpoint = builtin[i];
        config.endpoint_default_unset = true;
        config.stack_log_level = 0;
        assert(xiaotai_rtc_start(&config, NULL) == XIAOTAI_OK && !f.endpoint_size && !f.mode);
        shutdown();
    }

    const unsigned stages[] = {TIRTC_OPT_MAX_SEND_BUFFER, INIT, TIRTC_OPT_SERVICE_ENDPOINT,
        TIRTC_OPT_DEVICE_SECRET_KEY, TIRTC_OPT_TGTRP_POLL_TIMEOUT, START};
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        memset(&f, 0, sizeof(f));
        f.fail_step = stages[i];
        f.fail_code = TIRTC_E_LACK_OF_RESOURCE;
        const xiaotai_rtc_start_t config = defaults();
        int detail;
        assert(xiaotai_rtc_start(&config, &detail) == XIAOTAI_ERR_NO_MEM && detail == f.fail_code);
        assert(!f.initialized && !f.started && !f.system);
        assert(f.trace[f.count - 1] == (i < 2 ? stages[i] : UNINIT));
        f.count = f.fail_step = 0;
        assert(xiaotai_rtc_start(&config, NULL) == XIAOTAI_OK);
        shutdown();
    }
    memset(&f, 0, sizeof(f));
    xiaotai_rtc_start_t legacy_failure = defaults();
    legacy_failure.endpoint = "http://[::1]:8090/tenant";
    f.fail_step = TIRTC_OPT_SERVICE_ENDPOINT;
    f.fail_code = -9;
    int detail;
    assert(xiaotai_rtc_start(&legacy_failure, &detail) != XIAOTAI_OK && detail == -9);
    const unsigned expected_failure[] = {TIRTC_OPT_MAX_SEND_BUFFER, TIRTC_OPT_MAX_CONNECTIONS,
        TIRTC_OPT_CONNECT_CACHE, INIT, TIRTC_OPT_SERVICE_ENDPOINT, UNINIT};
    assert(f.count == sizeof(expected_failure) / sizeof(expected_failure[0]) &&
           !memcmp(f.trace, expected_failure, sizeof(expected_failure)));
    assert(!f.initialized && !f.started && !f.system);
    f.count = f.fail_step = 0;
    assert(xiaotai_rtc_start(&legacy_failure, &detail) == XIAOTAI_OK && !detail);
    assert(!strcmp(f.endpoint, legacy_failure.endpoint) && f.endpoint_size == strlen(legacy_failure.endpoint) + 1U);
    assert(f.trace[f.count - 1U] == START && f.system == 1);
    shutdown();
    if (!cap->media_transport) {
        memset(&f, 0, sizeof(f));
        xiaotai_rtc_start_t config = defaults();
        config.media_transport = XIAOTAI_TRANSPORT_ON;
        assert(xiaotai_rtc_start(&config, NULL) == XIAOTAI_ERR_NOT_SUPPORTED && !f.count);
    }
}

static void backend(void)
{
    const xiaotai_rtc_start_t config = defaults();
    int detail;
    const uintptr_t a = (uintptr_t)&conn_a, b = (uintptr_t)&conn_b;
    assert(xiaotai_rtc_start(&config, NULL) == XIAOTAI_OK);
    assert(!f.cb->on_sleep_wakeup_info);
    f.cb->on_event(TIRTC_EVENT_ACCESS_HIJACKING, NULL, 0);
    assert(f.system == 2 && f.event == XIAOTAI_RTC_ALERT && f.detail == TIRTC_E_UNEXPECTED_RESPONSE);
    f.cb->on_event(123, NULL, 0);
    assert(f.system == 2);
    f.cb->on_conn_accepted(&conn_b);
    assert(f.accepted == 1 && f.conn == b && !f.disconnects);
    assert(xiaotai_rtc_connect(XIAOTAI_LINK_WHIP, "fixture-peer", "fixture-token", 71, &detail) == XIAOTAI_OK);
    assert(f.downcall == 1 && !detail && !f.connection_results);
    conn_a.tag = f.tag;
    f.cb->on_command(&conn_a, 0xabcd0001U, packet, sizeof(packet));
    assert(f.commands == 1 && f.request == 71 && f.conn == a && f.word == 0xabcd0001U && f.payload == packet);
    f.cb->on_command(&conn_b, 1, NULL, 0);
    assert(f.commands == 2 && f.request == 0 && f.conn == b && !f.payload_size);
    assert(f.cb->on_subscribe_audio(&conn_a, 0) == 0 && f.request == 71 && f.frame.kind == XIAOTAI_LINK_AUDIO);
    assert(f.frame.stream == 0 && f.frame.key_frame);
    assert(f.cb->on_subscribe_audio(&conn_a, 15) == 0 && f.frame.stream == 15);
    f.admit = XIAOTAI_ERR_INVALID_ARG;
    assert(f.cb->on_subscribe_audio(&conn_a, 16) == TIRTC_E_INVALID_PARAMETER);
    f.admit = XIAOTAI_ERR_BUSY;
    const int busy = f.cb->on_subscribe_audio(&conn_a, 10);
    if (busy != TIRTC_E_BUSY) fprintf(stderr, "audio subscribe backpressure: got %d, expected %d\n", busy, TIRTC_E_BUSY);
    assert(busy == TIRTC_E_BUSY && f.frame.stream == 10 && f.frame.key_frame);
    assert(f.cb->on_subscribe_video(&conn_a, 5) == TIRTC_E_BUSY && f.frame.kind == XIAOTAI_LINK_VIDEO && f.frame.stream == 5);
    f.admit = XIAOTAI_ERR_NOT_SUPPORTED;
    assert(f.cb->on_subscribe_video(&conn_a, 5) == TIRTC_E_INVALID_PARAMETER);
    f.admit = XIAOTAI_OK;
    f.cb->on_unsubscribe_video(&conn_a, 5);
    f.cb->on_unsubscribe_audio(&conn_a, 10);
    assert(f.subscriptions == 8 && f.frame.kind == XIAOTAI_LINK_AUDIO &&
           f.frame.stream == 10 && !f.frame.key_frame);
    conn_a.tag = NULL;
    f.connected(0, &conn_a, f.tag);
#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER)
    assert(xiaotai_rtc_caps()->published_retirement_barrier && conn_a.tag == f.tag);
#else
    assert(!xiaotai_rtc_caps()->published_retirement_barrier && !conn_a.tag);
#endif
    assert(f.connection_results == 1 && f.request == 71 && f.conn == a && !f.detail);
    assert(xiaotai_rtc_connect(XIAOTAI_LINK_P2P, "fixture-peer", "fixture-token", UINT32_MAX, NULL) == XIAOTAI_OK);
    assert(f.downcall == 2);
    const int why[] = {TIRTC_E_CONN_REMOTECLOSE, TIRTC_E_TIMEOUTED, TIRTC_E_CONN_TIMEOUTCLOSE,
        TIRTC_E_HTTP_TIMEOUT, TIRTC_E_LACK_OF_RESOURCE, TIRTC_E_HTTP_NOMEM, TIRTC_E_SERVER_ERROR};
    const xiaotai_link_end_t endings[] = {XIAOTAI_LINK_END_PEER, XIAOTAI_LINK_END_TIMEOUT, XIAOTAI_LINK_END_TIMEOUT,
        XIAOTAI_LINK_END_TIMEOUT, XIAOTAI_LINK_END_NO_MEMORY, XIAOTAI_LINK_END_NO_MEMORY, XIAOTAI_LINK_END_CONNECT_FAILED};
    for (size_t i = 0; i < sizeof(why) / sizeof(why[0]); ++i) {
        f.connected(why[i], NULL, f.tag);
        assert(f.request == UINT32_MAX && !f.conn && f.reason == endings[i] && f.detail == why[i]);
        f.cb->on_conn_error(&conn_a, why[i]);
        assert(f.conn == a && f.detail == why[i] && !f.closed && !f.disconnects);
        assert(f.reason == (endings[i] == XIAOTAI_LINK_END_CONNECT_FAILED ? XIAOTAI_LINK_END_NETWORK : endings[i]));
    }
    f.result = TIRTC_E_BUSY;
    assert(xiaotai_rtc_connect(XIAOTAI_LINK_WHIP, "fixture-peer", "fixture-token", 71, &detail) == XIAOTAI_ERR_BUSY);
    assert(detail == TIRTC_E_BUSY && f.connection_results == 8);
    f.result = 0;
#ifdef TIRTC_PENDING_CLOSE_CONTRACT
    f.request = 2;
    assert(xiaotai_rtc_pending() == 2);
    f.result = 1;
    assert(xiaotai_rtc_cancel(71) == XIAOTAI_OK && !f.closed);
    f.result = TIRTC_E_INVALID_HANDLE;
    assert(xiaotai_rtc_cancel(71) == XIAOTAI_ERR_INVALID_STATE && !f.closed);
#else
    assert(xiaotai_rtc_cancel(71) == XIAOTAI_ERR_NOT_SUPPORTED && !xiaotai_rtc_pending());
#endif
    TIRTCFRAMEINFO wire = {.stream_id = 5, .media = TIRTC_AUDIO_ALAW, .flags = TIRTC_AUDIOSAMPLE_8K16B1C,
        .ts = UINT32_MAX, .length = sizeof(packet)};
    f.cb->on_audio(&conn_a, &wire, (void *)packet);
    assert(f.frames == 1 && f.frame.kind == XIAOTAI_LINK_AUDIO && f.frame.format.codec == XIAOTAI_LINK_CODEC_G711A);
    assert(f.frame.stream == 5 && f.frame.format.rate_hz == 8000 && f.frame.format.channels == 1);
    assert(f.frame.timestamp_ms == UINT32_MAX && f.payload == packet && f.payload_size == sizeof(packet));
    wire.flags = TIRTC_AUDIOSAMPLE_16K16B1C;
    f.cb->on_audio(&conn_a, &wire, (void *)packet);
    assert(f.frames == 2 && f.frame.format.codec == XIAOTAI_LINK_CODEC_NONE);
    wire.media = TIRTC_AUDIO_OPUS;
    f.cb->on_audio(&conn_a, &wire, (void *)packet);
    assert(f.frame.format.codec == XIAOTAI_LINK_CODEC_OPUS && f.frame.format.rate_hz == 16000);
    wire.media = TIRTC_VIDEO_H264;
    wire.flags = TIRTC_FRAME_FLAG_KEY_FRAME;
    f.cb->on_video(&conn_a, &wire, (void *)packet);
    assert(f.frames == 4 && f.frame.kind == XIAOTAI_LINK_VIDEO && f.frame.key_frame && f.frame.format.codec == XIAOTAI_LINK_CODEC_H264);
    f.cb->on_message(&conn_a, &wire, (void *)packet);
    xiaotai_rtc_diag_t diag;
    xiaotai_rtc_diag(&diag);
    assert(diag.messages == 1 && f.frames == 4);

    f.result = sizeof(packet);
    assert(xiaotai_rtc_send(a, &f.frame, packet, sizeof(packet), &detail) == XIAOTAI_OK && !detail && f.downcall == 4);
    assert(f.wire.length == sizeof(packet) && f.wire.media == TIRTC_VIDEO_H264 && f.wire.flags == TIRTC_FRAME_FLAG_KEY_FRAME);
    f.frame = (xiaotai_link_frame_t){.kind = XIAOTAI_LINK_AUDIO, .format = {XIAOTAI_LINK_CODEC_G711A, 8000, 1}, .stream = 5};
    assert(xiaotai_rtc_send(a, &f.frame, packet, sizeof(packet), &detail) == XIAOTAI_OK && !detail && f.downcall == 3);
    assert(f.wire.media == TIRTC_AUDIO_ALAW && f.wire.flags == TIRTC_AUDIOSAMPLE_8K16B1C && !f.wire.reserved);
    assert(xiaotai_rtc_command(a, 0xdead0001U, packet, sizeof(packet)) == XIAOTAI_OK);
    f.result = TIRTC_E_BUSY;
    assert(xiaotai_rtc_send(a, &f.frame, packet, sizeof(packet), &detail) == XIAOTAI_ERR_BUSY && detail == f.result);
    assert(xiaotai_rtc_command(a, 0xdead0001U, packet, sizeof(packet)) == XIAOTAI_ERR_BUSY);
    f.result = TIRTC_E_INVALID_HANDLE;
    assert(xiaotai_rtc_send(a, &f.frame, packet, sizeof(packet), NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_rtc_backlog(a) == 123 && !xiaotai_rtc_backlog(0));
    f.frame.format.channels = 2;
    assert(!xiaotai_rtc_format_supported(f.frame.kind, &f.frame.format));
    assert(xiaotai_rtc_send(a, &f.frame, packet, sizeof(packet), &detail) == XIAOTAI_FAIL && detail == TIRTC_E_INVALID_PARAMETER);
    static const struct { int raw; xiaotai_err_t expected; } subscription_results[] = {
        {0, XIAOTAI_OK}, {8, XIAOTAI_OK}, {TIRTC_E_INVALID_HANDLE, XIAOTAI_ERR_INVALID_STATE},
        {TIRTC_E_BUSY, XIAOTAI_ERR_BUSY}, {-32000, XIAOTAI_FAIL},
    };
    for (size_t i = 0; i < sizeof(subscription_results) / sizeof(subscription_results[0]); ++i) {
        f.result = subscription_results[i].raw;
        const xiaotai_err_t expected = subscription_results[i].expected;
        assert(xiaotai_rtc_subscribe(a, XIAOTAI_LINK_AUDIO, 5, true) == expected && f.downcall == 5);
        assert(xiaotai_rtc_subscribe(a, XIAOTAI_LINK_AUDIO, 5, false) == expected && f.downcall == 6);
        assert(xiaotai_rtc_subscribe(a, XIAOTAI_LINK_VIDEO, 5, true) == expected && f.downcall == 7);
        assert(xiaotai_rtc_subscribe(a, XIAOTAI_LINK_VIDEO, 5, false) == expected && f.downcall == 8);
    }
    f.result = 8;
    assert(xiaotai_rtc_request_key_frame(a, 5) == XIAOTAI_OK && f.downcall == 9);
    assert(xiaotai_rtc_set_video_bitrate(a, 5, 100, 200, 300) == XIAOTAI_FAIL && f.downcall == 10);
    f.result = TIRTC_E_BUSY;
    assert(xiaotai_rtc_request_key_frame(a, 5) == XIAOTAI_ERR_BUSY && f.downcall == 9);
    f.result = 0;
    assert(xiaotai_rtc_request_key_frame(a, 5) == XIAOTAI_OK && f.downcall == 9);
    assert(xiaotai_rtc_set_video_bitrate(a, 5, 100, 200, 300) == XIAOTAI_OK && f.downcall == 10);
    assert(f.minimum == 100 && f.maximum == 300 && f.initial == 200);
    f.cb->on_request_key_frame(&conn_a, 7);
    assert(f.key_requests == 1 && f.frame.stream == 7);
    f.cb->on_update_bitrate(&conn_a, 6, 75000);
    assert(f.rate_updates == 1 && f.frame.stream == 6 && f.initial == 75000);
    char body[12]; size_t size;
    assert(xiaotai_rtc_service_request("/fixture", NULL, &detail, body, sizeof(body), &size) == XIAOTAI_OK);
    assert(!detail && size == 10 && !strcmp(body, "{\"code\":0}"));
    assert(xiaotai_rtc_service_request("/fixture", NULL, &detail, body, 10, &size) == XIAOTAI_OK && !size && !*body);
    f.result = 410;
    assert(xiaotai_rtc_service_request("/fixture", NULL, &detail, body, sizeof(body), &size) == XIAOTAI_FAIL);
    assert(detail == 410 && !size && !*body);
    f.result = TIRTC_E_INVALID_HANDLE;
    assert(xiaotai_rtc_disconnect(a, &detail) == XIAOTAI_ERR_INVALID_STATE && detail == f.result);
    assert(f.disconnects == 1 && !f.close_pending && !f.closed);
    f.result = 0;
    assert(xiaotai_rtc_disconnect(a, &detail) == XIAOTAI_OK && !detail);
    assert(f.disconnects == 2 && f.close_pending && !f.closed);
    finish_disconnect();
    assert(!f.close_pending && f.closed == 1 && f.conn == a);
#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER)
    assert(f.closed_generation == 71);
#else
    assert(!f.closed_generation);
#endif
    /* Even if confirmed inside the SDK call, the adapter forwards only the
     * actual close upcall, with no second notification on return. Use the
     * same address for the next connection, as the SDK allocator can. */
    conn_a.tag = NULL;
    f.cb->on_conn_accepted(&conn_a);
    f.close_inline = true;
    assert(xiaotai_rtc_disconnect(a, &detail) == XIAOTAI_OK && !detail);
    assert(f.disconnects == 3 && !f.close_pending && f.closed == 2 && f.conn == a && !f.closed_generation);
    /* The transport still gets a non-null handle returned with a failed
     * result so it can dispose of it. That object is not tagged as opened. */
    assert(xiaotai_rtc_connect(XIAOTAI_LINK_WHIP, "fixture-peer", "fixture-token", 73, NULL) == XIAOTAI_OK);
    const unsigned prior_results = f.connection_results;
    f.connected(TIRTC_E_SERVER_ERROR, &conn_a, f.tag);
    assert(!conn_a.tag && f.connection_results == prior_results + 1U && f.conn == a && f.request == 73 &&
           f.reason == XIAOTAI_LINK_END_CONNECT_FAILED && f.detail == TIRTC_E_SERVER_ERROR);
#if defined(XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER)
    const unsigned prior_tags = f.peer_tag_calls;
    assert(xiaotai_rtc_connect(XIAOTAI_LINK_P2P, "fixture-peer", "fixture-token", 81, NULL) == XIAOTAI_OK);
    f.connected(0, &conn_a, f.tag);
    assert(!f.peer_tag_written && !conn_a.tag && f.peer_tag_calls == prior_tags);
    f.outbound_peer_tracking = true;
    f.connected(TIRTC_E_SERVER_ERROR, &conn_a, f.tag);
    assert(!f.peer_tag_written && !conn_a.tag && f.peer_tag_calls == prior_tags);

    f.reject_peer_tag = true;
    f.connected(0, &conn_a, f.tag);
    assert(!f.peer_tag_written && !conn_a.tag && f.peer_tag_calls == prior_tags + 1U);
    f.cb->on_disconnected(&conn_a);
    assert(!f.closed_generation);

    f.reject_peer_tag = false;
    f.connected(0, &conn_a, f.tag);
    assert(f.peer_tag_written && conn_a.tag == f.tag && f.peer_tag_calls == prior_tags + 2U);
    f.cb->on_disconnected(&conn_a);
    assert(f.closed_generation == 81);
    conn_a.tag = NULL;

    f.inbound_peer_generation = 82;
    f.cb->on_conn_accepted(&conn_a);
    assert(f.peer_tag_written && (uintptr_t)conn_a.tag == 82 && f.peer_tag_calls == prior_tags + 3U);
    f.cb->on_disconnected(&conn_a);
    assert(f.closed_generation == 82);
    conn_a.tag = NULL;
#endif
    shutdown();
}

#ifdef XIAOTAI_TEST_S3_JPEG
static void jpeg_backend(uint8_t stream)
{
    const xiaotai_rtc_start_t config = defaults();
    const uintptr_t conn = (uintptr_t)&conn_a;
    const xiaotai_link_frame_t frame = {
        .kind = XIAOTAI_LINK_VIDEO, .format = {XIAOTAI_LINK_CODEC_JPEG, 0, 0},
        .stream = stream, .key_frame = true, .timestamp_ms = 123,
    };
    int detail;
    assert(xiaotai_rtc_start(&config, NULL) == XIAOTAI_OK);
    assert(xiaotai_rtc_connect(XIAOTAI_LINK_WHIP, "fixture-peer", "fixture-token", 71, &detail) == XIAOTAI_OK);
    conn_a.tag = f.tag;
    f.connected(0, &conn_a, f.tag);
    assert(f.connection_results == 1 && f.conn == conn && f.request == 71);

    expected_jpeg_stream = stream;
    f.result = sizeof(jpeg);
    assert(xiaotai_rtc_send(conn, &frame, jpeg, sizeof(jpeg), &detail) == XIAOTAI_OK && !detail);
    assert(f.sends == 1 && f.downcall == 4);
    f.result = TIRTC_E_BUSY;
    assert(xiaotai_rtc_send(conn, &frame, jpeg, sizeof(jpeg), &detail) == XIAOTAI_ERR_BUSY);
    assert(f.sends == 2 && detail == TIRTC_E_BUSY);
    f.result = 0;
    assert(xiaotai_rtc_disconnect(conn, &detail) == XIAOTAI_OK && !detail);
    finish_disconnect();
    assert(f.disconnects == 1 && f.closed == 1 && f.conn == conn);
    shutdown();
}
#endif

#if FIXTURE_TGTRP
static void trace_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    f.trace_log(format, args);
    va_end(args);
}
#endif

static void logs(void)
{
    const xiaotai_rtc_start_t config = defaults();
    assert(xiaotai_rtc_start(&config, NULL) == XIAOTAI_OK && f.log && f.mode == 3);
    static const char service[] = {'/', 'v', '1', '/', 's', 't', 'a', 'r', 't', ':', ' ', '6', '0', '0', '6'};
    static const char wrong[] = "/v1/start: 40000fake";
    static const char unknown[] = "fixture payload, not a diagnostic";
    f.log(service, sizeof(service));
    f.log(wrong, sizeof(wrong) - 1);
    f.log(unknown, sizeof(unknown) - 1);
    f.log(NULL, 42);
    xiaotai_rtc_diag_t diag;
    xiaotai_rtc_diag(&diag);
    assert(diag.log_bytes == sizeof(service) + sizeof(wrong) + sizeof(unknown) - 2U);
    assert(diag.log_facts == 1 && diag.service_status == 6006);
#if FIXTURE_TGTRP
    assert(f.trace_log && f.log_level == 3);
    trace_log("(%s:%d) peer_connection_kcp_init ikcp_nodelay(%d %d %d %d) sndwnd=%d mtu=%d");
    trace_log("(%s:%d) media transport selected without tgtrp_enable_media_transport");
    trace_log("(%s:%d) tgtrp stat changes %s"); /* Disabled, no arguments must be read. */
    xiaotai_rtc_diag(&diag);
    assert(diag.log_facts == 3 && !diag.statistics_skipped);
    xiaotai_rtc_set_statistics(true);
    assert(f.log_level == TGTRP_LOG_FLAG_STAT && f.mode == 3);
    trace_log("(%s:%d) tgtrp stat changes %s"); /* Unknown layout must not read arguments. */
    trace_log("(%s:%d) tgtrp stat ondata-ex conn=%p ch=%p dc=%p dci=%u tr=%u media=%u "
        "stream=%u ft=%d size=%d payload=%u off=%u peer=%u dbg=%u diff=%d rtt=%u one=%u sndbuf=%u "
        "fid=%u send_span=%u recv_span=%u deliv=%u deliv_rtt=%u lrtt=%u prtt=%u nack=%u retx=%u "
        "pend_wait=%u pend_q=%u pacer_q_max=%u pacer_wait_max=%u send_samples=%u send_loss=%u recv_samples=%u recv_loss=%u",
        "fixture", 7, (void *)0, (void *)0, (void *)0,
        100U, 101U, 2U, 103U, 104, 105, 106U, 107U, 108U, 109U, 110, 111U, 112U, 113U, 114U,
        115U, 116U, 117U, 118U, 119U, 120U, 121U, 122U, 123U, 124U, 125U, 126U, 127U, 128U, 129U, 130U);
    xiaotai_rtc_diag(&diag);
    assert(f.traces == 1 && diag.statistics == 1 && diag.statistics_skipped == 1);
    assert(f.sample.media == 2 && f.sample.rtt_ms == 120 && f.sample.nack == 121 && f.sample.retransmitted == 122);
    assert(f.sample.send_loss == 128 && f.sample.receive_loss == 130);
    xiaotai_rtc_set_statistics(false);
    assert(f.log_level == 0 && f.mode == 3);
#else
    xiaotai_rtc_set_statistics(true);
    xiaotai_rtc_set_statistics(false);
    assert(!f.traces && !f.log_level && !diag.statistics && !diag.statistics_skipped);
#endif
    shutdown();
}

int main(int argc, char **argv)
{
    _Static_assert(sizeof(TIRTCFRAMEINFO) == 12 && offsetof(TIRTCFRAMEINFO, ts) == 4 &&
                   offsetof(TIRTCFRAMEINFO, length) == 8, "packed frame ABI");
    if (argc == 1) options();
    else if (!strcmp(argv[1], "backend")) backend();
#ifdef XIAOTAI_TEST_S3_JPEG
    else if (!strcmp(argv[1], "h5_jpeg")) jpeg_backend(11);
    else if (!strcmp(argv[1], "ai_jpeg")) jpeg_backend(0);
#endif
    else if (!strcmp(argv[1], "log")) logs();
    else assert(false);
    return 0;
}
#endif
