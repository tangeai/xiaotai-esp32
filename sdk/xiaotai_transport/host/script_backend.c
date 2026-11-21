// SPDX-License-Identifier: MIT
/* Scripted RTC backend and manual clock for host tests. No TiRTC or sockets:
 * callbacks execute in the injecting thread, just like callbacks from a
 * separate stack or from inside a downcall can reach the real core. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "xiaotai_transport_host.h"

#define CALLS_KEPT 256U

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static xiaotai_transport_host_call_t s_calls[CALLS_KEPT];
static size_t s_call_count;
static uint32_t s_counts[XIAOTAI_TRANSPORT_HOST_BITRATE + 1];
static int64_t s_call_last_at[XIAOTAI_TRANSPORT_HOST_BITRATE + 1];
static xiaotai_transport_host_start_t s_last_start;
static xiaotai_rtc_caps_t s_caps;
static struct {
    xiaotai_err_t result;
    int detail;
    uint32_t times;
} s_fail_start, s_fail_stop, s_fail_connect, s_fail_disconnect;
static uint32_t s_pending;
static struct {
    xiaotai_transport_host_call_kind_t kind;
    xiaotai_err_t result;
    int detail;
    uint32_t times;
} s_fail_media;
static size_t s_backlog;
static struct {
    uintptr_t conn;
    size_t bytes;
} s_link_backlogs[XIAOTAI_TRANSPORT_LINKS_MAX];
static xiaotai_link_frame_t s_last_frame;
static xiaotai_transport_host_media_hook_t s_media_hook;
static void *s_hook_ctx;
static xiaotai_transport_host_connect_hook_t s_connect_hook;
static void *s_connect_ctx;
static xiaotai_transport_host_command_hook_t s_command_hook;
static void *s_command_ctx;
static xiaotai_transport_host_service_fn s_service;
static void *s_service_ctx;
static bool s_auto_started, s_auto_stopped;
static bool s_initialized, s_stopped_reported;
static uint32_t s_violations;

static _Atomic bool s_manual;
static _Atomic int64_t s_manual_ms;

static const xiaotai_rtc_caps_t s_default_caps = {
    .version = "host-script",
    .https = true,
    .cancel_pending = false,
    .media_transport = true,
    .stack_log = true,
    .video = true,
    .statistics = false,
    .send_stack_bytes = XIAOTAI_RTC_SEND_STACK_UNMEASURED,
};

/* ------------------------------------------------------------------- clock */

int64_t xt_now_ms(void)
{
    if (atomic_load(&s_manual)) return atomic_load(&s_manual_ms);
    return xiaotai_time_us() / 1000;
}

uint32_t xt_clock_wait_ms(int64_t delta_ms)
{
    if (atomic_load(&s_manual)) return XIAOTAI_WAIT_FOREVER;   /* advancing kicks the worker */
    if (delta_ms <= 0) return 0;
    return delta_ms >= (int64_t)XT_EVALUATE_PERIOD_MS ? XT_EVALUATE_PERIOD_MS : (uint32_t)delta_ms;
}

void xiaotai_transport_host_clock_manual(int64_t start_ms)
{
    atomic_store(&s_manual_ms, start_ms);
    atomic_store(&s_manual, true);
    xt_worker_kick();
}

void xiaotai_transport_host_clock_advance(uint32_t ms)
{
    atomic_fetch_add(&s_manual_ms, (int64_t)ms);
    xt_worker_kick();
}

int64_t xiaotai_transport_host_clock(void)
{
    return xt_now_ms();
}

bool xiaotai_transport_host_settle(uint32_t timeout_ms)
{
    const int64_t until = xiaotai_time_us() / 1000 + timeout_ms;
    unsigned quiet = 0;
    while (xiaotai_time_us() / 1000 < until) {
        quiet = xt_worker_settled() ? quiet + 1U : 0U;
        if (quiet >= 3U) return true;
        xiaotai_delay_ms(1);
    }
    return false;
}

/* ------------------------------------------------------------------ script */

static void record(xiaotai_transport_host_call_kind_t kind, int64_t value, xiaotai_err_t result, uintptr_t conn)
{
    const int64_t at = xt_now_ms();
    if (s_call_count < CALLS_KEPT) {
        s_calls[s_call_count] =
            (xiaotai_transport_host_call_t){.kind = kind, .at_ms = at, .value = value, .result = result,
                                            .connection = conn};
    }
    ++s_call_count;
    ++s_counts[kind];
    s_call_last_at[kind] = at;
}

void xiaotai_transport_host_reset(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_calls, 0, sizeof(s_calls));
    s_call_count = 0;
    memset(s_counts, 0, sizeof(s_counts));
    memset(&s_last_start, 0, sizeof(s_last_start));
    s_caps = s_default_caps;
    memset(&s_fail_start, 0, sizeof(s_fail_start));
    memset(&s_fail_stop, 0, sizeof(s_fail_stop));
    memset(&s_fail_connect, 0, sizeof(s_fail_connect));
    memset(&s_fail_disconnect, 0, sizeof(s_fail_disconnect));
    s_pending = 0;
    s_backlog = 0;
    memset(s_link_backlogs, 0, sizeof(s_link_backlogs));
    memset(&s_fail_media, 0, sizeof(s_fail_media));
    memset(&s_last_frame, 0, sizeof(s_last_frame));
    s_media_hook = NULL;
    s_hook_ctx = NULL;
    s_connect_hook = NULL;
    s_connect_ctx = NULL;
    s_command_hook = NULL;
    s_command_ctx = NULL;
    s_service = NULL;
    s_service_ctx = NULL;
    s_auto_started = s_auto_stopped = false;
    s_initialized = s_stopped_reported = false;
    s_violations = 0;
    pthread_mutex_unlock(&s_lock);
    atomic_store(&s_manual, false);
    atomic_store(&s_manual_ms, 0);
}

void xiaotai_transport_host_set_caps(const xiaotai_transport_host_caps_t *caps)
{
    pthread_mutex_lock(&s_lock);
    s_caps = s_default_caps;
    if (caps) {
        s_caps.https = caps->https;
        s_caps.cancel_pending = caps->cancel_pending;
        s_caps.published_retirement_barrier = caps->published_retirement_barrier;
        s_caps.media_transport = caps->media_transport;
        s_caps.stack_log = caps->stack_log;
        s_caps.video = caps->video;
        s_caps.statistics = caps->statistics;
        s_caps.send_stack_bytes = caps->send_stack_bytes ? caps->send_stack_bytes : XIAOTAI_RTC_SEND_STACK_UNMEASURED;
    }
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_service(xiaotai_transport_host_service_fn fn, void *ctx)
{
    pthread_mutex_lock(&s_lock);
    s_service = fn;
    s_service_ctx = ctx;
    pthread_mutex_unlock(&s_lock);
}

xiaotai_err_t xiaotai_rtc_service_request(const char *path, const char *json, int *status, char *body,
                                         size_t capacity, size_t *size)
{
    pthread_mutex_lock(&s_lock);
    xiaotai_transport_host_service_fn fn = s_service;
    void *ctx = s_service_ctx;
    pthread_mutex_unlock(&s_lock);
    if (size) *size = 0;
    if (status) *status = XIAOTAI_ERR_NOT_SUPPORTED;
    if (body && capacity) body[0] = '\0';
    return fn ? fn(ctx, path, json, status, body, capacity, size) : XIAOTAI_ERR_NOT_SUPPORTED;
}

void xiaotai_transport_host_fail_start(xiaotai_err_t result, int detail, uint32_t times)
{
    pthread_mutex_lock(&s_lock);
    s_fail_start.result = result;
    s_fail_start.detail = detail;
    s_fail_start.times = times;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_fail_stop(xiaotai_err_t result, int detail, uint32_t times)
{
    pthread_mutex_lock(&s_lock);
    s_fail_stop.result = result;
    s_fail_stop.detail = detail;
    s_fail_stop.times = times;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_fail_connect(xiaotai_err_t result, int detail, uint32_t times)
{
    pthread_mutex_lock(&s_lock);
    s_fail_connect.result = result;
    s_fail_connect.detail = detail;
    s_fail_connect.times = times;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_fail_disconnect(xiaotai_err_t result, int detail, uint32_t times)
{
    pthread_mutex_lock(&s_lock);
    s_fail_disconnect.result = result;
    s_fail_disconnect.detail = detail;
    s_fail_disconnect.times = times;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_pending(uint32_t count)
{
    pthread_mutex_lock(&s_lock);
    s_pending = count;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_auto_started(bool in_call)
{
    pthread_mutex_lock(&s_lock);
    s_auto_started = in_call;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_auto_stopped(bool in_call)
{
    pthread_mutex_lock(&s_lock);
    s_auto_stopped = in_call;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_started(void)
{
    xiaotai_rtc_on_system(XIAOTAI_RTC_STARTED, 0);
}

void xiaotai_transport_host_stopped(int detail)
{
    pthread_mutex_lock(&s_lock);
    s_stopped_reported = true;
    pthread_mutex_unlock(&s_lock);
    xiaotai_rtc_on_system(XIAOTAI_RTC_STOPPED, detail);
}

void xiaotai_transport_host_alert(void)
{
    xiaotai_rtc_on_system(XIAOTAI_RTC_ALERT, 0);
}

void xiaotai_transport_host_accepted(uintptr_t conn) { xiaotai_rtc_on_accepted(conn); }

void xiaotai_transport_host_connected(uint32_t request, uintptr_t conn, xiaotai_link_end_t reason, int detail)
{
    xiaotai_rtc_on_connected(request, conn, reason, detail);
}

void xiaotai_transport_host_closed(uintptr_t conn) { xiaotai_rtc_on_closed(conn); }
void xiaotai_transport_host_closed_for(uintptr_t conn, uint32_t request) { xiaotai_rtc_on_closed_for(conn, request); }

void xiaotai_transport_host_error(uintptr_t conn, xiaotai_link_end_t reason, int detail)
{
    xiaotai_rtc_on_error(conn, reason, detail);
}

size_t xiaotai_transport_host_calls(xiaotai_transport_host_call_t *out, size_t capacity)
{
    pthread_mutex_lock(&s_lock);
    const size_t kept = s_call_count < CALLS_KEPT ? s_call_count : CALLS_KEPT;
    for (size_t i = 0; out && i < kept && i < capacity; ++i) out[i] = s_calls[i];
    const size_t count = s_call_count;
    pthread_mutex_unlock(&s_lock);
    return count;
}

uint32_t xiaotai_transport_host_count(xiaotai_transport_host_call_kind_t kind)
{
    if ((unsigned)kind > XIAOTAI_TRANSPORT_HOST_BITRATE) return 0;
    pthread_mutex_lock(&s_lock);
    const uint32_t count = s_counts[kind];
    pthread_mutex_unlock(&s_lock);
    return count;
}

int64_t xiaotai_transport_host_last_at(xiaotai_transport_host_call_kind_t kind)
{
    if ((unsigned)kind > XIAOTAI_TRANSPORT_HOST_BITRATE) return -1;
    pthread_mutex_lock(&s_lock);
    const int64_t at = s_counts[kind] ? s_call_last_at[kind] : -1;
    pthread_mutex_unlock(&s_lock);
    return at;
}

void xiaotai_transport_host_last_start(xiaotai_transport_host_start_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&s_lock);
    *out = s_last_start;
    pthread_mutex_unlock(&s_lock);
}

bool xiaotai_transport_host_initialized(void)
{
    pthread_mutex_lock(&s_lock);
    const bool initialized = s_initialized;
    pthread_mutex_unlock(&s_lock);
    return initialized;
}

uint32_t xiaotai_transport_host_violations(void)
{
    pthread_mutex_lock(&s_lock);
    const uint32_t violations = s_violations;
    pthread_mutex_unlock(&s_lock);
    return violations;
}

/* ----------------------------------------------------------------- backend */

const xiaotai_rtc_caps_t *xiaotai_rtc_caps(void)
{
    /* Written only while the engine is reset or before it starts. */
    if (!s_caps.version) s_caps = s_default_caps;
    return &s_caps;
}

void xiaotai_rtc_diag(xiaotai_rtc_diag_t *diag)
{
    if (diag) memset(diag, 0, sizeof(*diag));
}

static void copy_text(char *out, size_t capacity, const char *text)
{
    (void)snprintf(out, capacity, "%s", text ? text : "");
}

xiaotai_err_t xiaotai_rtc_start(const xiaotai_rtc_start_t *options, int *detail)
{
    pthread_mutex_lock(&s_lock);
    if (s_initialized) ++s_violations;
    xiaotai_err_t result = XIAOTAI_OK;
    int code = 0;
    if (s_fail_start.times) {
        --s_fail_start.times;
        result = s_fail_start.result;
        code = s_fail_start.detail;
    }
    xiaotai_transport_host_start_t *last = &s_last_start;
    copy_text(last->device_id, sizeof(last->device_id), options->device_id);
    copy_text(last->device_secret, sizeof(last->device_secret), options->device_secret);
    copy_text(last->client_id, sizeof(last->client_id), options->client_id);
    copy_text(last->endpoint, sizeof(last->endpoint), options->endpoint);
    last->send_buffer_bytes = options->send_buffer_bytes;
    last->links = options->links;
    last->network_kind = options->network_kind;
    last->poll_timeout_ms = options->poll_timeout_ms;
    last->stack_log_level = options->stack_log_level;
    last->connect_cache = options->connect_cache;
    last->media_transport = options->media_transport;
    last->media_transport_before_init = options->media_transport_before_init;
    last->option_strings_without_nul = options->option_strings_without_nul;
    last->endpoint_http_rewrite = options->endpoint_http_rewrite;
    last->endpoint_default_unset = options->endpoint_default_unset;
    if (result == XIAOTAI_OK) {
        s_initialized = true;
        s_stopped_reported = false;
    }
    record(XIAOTAI_TRANSPORT_HOST_START, 0, result, 0);
    const bool report = result == XIAOTAI_OK && s_auto_started;
    pthread_mutex_unlock(&s_lock);
    if (detail) *detail = code;
    if (report) xiaotai_rtc_on_system(XIAOTAI_RTC_STARTED, 0);
    return result;
}

xiaotai_err_t xiaotai_rtc_stop(int *detail)
{
    pthread_mutex_lock(&s_lock);
    xiaotai_err_t result = XIAOTAI_OK;
    int code = 0;
    if (!s_initialized) {
        ++s_violations;
        result = XIAOTAI_ERR_INVALID_STATE;
    } else if (s_fail_stop.times) {
        --s_fail_stop.times;
        result = s_fail_stop.result;
        code = s_fail_stop.detail;
    }
    record(XIAOTAI_TRANSPORT_HOST_STOP, 0, result, 0);
    const bool report = result == XIAOTAI_OK && s_auto_stopped;
    if (report) s_stopped_reported = true;
    pthread_mutex_unlock(&s_lock);
    if (detail) *detail = code;
    if (report) xiaotai_rtc_on_system(XIAOTAI_RTC_STOPPED, 0);
    return result;
}

void xiaotai_rtc_uninit(void)
{
    pthread_mutex_lock(&s_lock);
    if (!s_initialized || !s_stopped_reported) ++s_violations;
    s_initialized = false;
    s_stopped_reported = false;
    record(XIAOTAI_TRANSPORT_HOST_UNINIT, 0, XIAOTAI_OK, 0);
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_rtc_set_statistics(bool enabled)
{
    pthread_mutex_lock(&s_lock);
    record(XIAOTAI_TRANSPORT_HOST_STATISTICS, enabled ? 1 : 0, XIAOTAI_OK, 0);
    pthread_mutex_unlock(&s_lock);
}

xiaotai_err_t xiaotai_rtc_disconnect(xiaotai_rtc_conn_t conn, int *detail)
{
    pthread_mutex_lock(&s_lock);
    const bool fail = s_fail_disconnect.times != 0;
    if (fail) --s_fail_disconnect.times;
    const xiaotai_err_t result = fail ? s_fail_disconnect.result : XIAOTAI_OK;
    if (detail) *detail = fail ? s_fail_disconnect.detail : 0;
    record(XIAOTAI_TRANSPORT_HOST_DISCONNECT, (int64_t)conn, result, conn);
    pthread_mutex_unlock(&s_lock);
    return result;
}

xiaotai_err_t xiaotai_rtc_connect(xiaotai_link_kind_t kind, const char *peer, const char *token, uint32_t request,
                                  int *detail)
{
    pthread_mutex_lock(&s_lock);
    const bool valid = s_initialized && peer && *peer && request &&
                       (kind == XIAOTAI_LINK_P2P || kind == XIAOTAI_LINK_WHIP);
    const bool fail = s_fail_connect.times != 0;
    if (fail) --s_fail_connect.times;
    if (!valid) ++s_violations;
    const xiaotai_err_t result = valid ? (fail ? s_fail_connect.result : XIAOTAI_OK) : XIAOTAI_ERR_INVALID_STATE;
    if (detail) *detail = fail ? s_fail_connect.detail : 0;
    record(XIAOTAI_TRANSPORT_HOST_CONNECT, (int64_t)request, result, 0);
    const xiaotai_transport_host_connect_hook_t hook = s_connect_hook;
    void *ctx = s_connect_ctx;
    pthread_mutex_unlock(&s_lock);
    if (hook && result == XIAOTAI_OK) hook(ctx, kind, request, peer, token);
    return result;
}

xiaotai_err_t xiaotai_rtc_cancel(uint32_t request)
{
    pthread_mutex_lock(&s_lock);
    const xiaotai_err_t result = s_caps.cancel_pending ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED;
    record(XIAOTAI_TRANSPORT_HOST_CANCEL, (int64_t)request, result, 0);
    pthread_mutex_unlock(&s_lock);
    return result;
}

uint32_t xiaotai_rtc_pending(void)
{
    pthread_mutex_lock(&s_lock);
    const uint32_t result = s_caps.cancel_pending ? s_pending : 0;
    record(XIAOTAI_TRANSPORT_HOST_PENDING, (int64_t)result, XIAOTAI_OK, 0);
    pthread_mutex_unlock(&s_lock);
    return result;
}

bool xiaotai_rtc_format_supported(xiaotai_link_media_kind_t kind, const xiaotai_link_format_t *format)
{
    if (!format) return false;
    if (kind == XIAOTAI_LINK_VIDEO)
        return s_caps.video && !format->rate_hz && !format->channels &&
               (format->codec == XIAOTAI_LINK_CODEC_H264 || format->codec == XIAOTAI_LINK_CODEC_JPEG);
    return kind == XIAOTAI_LINK_AUDIO && format->rate_hz && format->channels &&
           (format->codec == XIAOTAI_LINK_CODEC_G711A || format->codec == XIAOTAI_LINK_CODEC_OPUS);
}

void xiaotai_transport_host_backlog_set(size_t bytes)
{
    pthread_mutex_lock(&s_lock);
    s_backlog = bytes;
    memset(s_link_backlogs, 0, sizeof(s_link_backlogs));
    pthread_mutex_unlock(&s_lock);
}

xiaotai_err_t xiaotai_transport_host_backlog_for(uintptr_t conn, size_t bytes)
{
    if (!conn) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    size_t idx = XIAOTAI_TRANSPORT_LINKS_MAX;
    for (size_t i = 0; i < XIAOTAI_TRANSPORT_LINKS_MAX; ++i) {
        if (s_link_backlogs[i].conn == conn) {
            idx = i;
            break;
        }
        if (!s_link_backlogs[i].conn && idx == XIAOTAI_TRANSPORT_LINKS_MAX) idx = i;
    }
    if (idx < XIAOTAI_TRANSPORT_LINKS_MAX) {
        s_link_backlogs[idx].conn = conn;
        s_link_backlogs[idx].bytes = bytes;
    }
    pthread_mutex_unlock(&s_lock);
    return idx < XIAOTAI_TRANSPORT_LINKS_MAX ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

void xiaotai_transport_host_media_fail(xiaotai_transport_host_call_kind_t kind, xiaotai_err_t result,
                                      int detail, uint32_t times)
{
    pthread_mutex_lock(&s_lock);
    s_fail_media.kind = kind;
    s_fail_media.result = result;
    s_fail_media.detail = detail;
    s_fail_media.times = times;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_media_hook(xiaotai_transport_host_media_hook_t fn, void *ctx)
{
    pthread_mutex_lock(&s_lock);
    s_media_hook = fn;
    s_hook_ctx = ctx;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_transport_host_connect_hook(xiaotai_transport_host_connect_hook_t fn, void *ctx)
{
    pthread_mutex_lock(&s_lock);
    s_connect_hook = fn;
    s_connect_ctx = ctx;
    pthread_mutex_unlock(&s_lock);
}

bool xiaotai_transport_host_last_frame(xiaotai_link_frame_t *frame)
{
    if (!frame) return false;
    pthread_mutex_lock(&s_lock);
    const bool sent = s_counts[XIAOTAI_TRANSPORT_HOST_SEND] != 0;
    if (sent) *frame = s_last_frame;
    pthread_mutex_unlock(&s_lock);
    return sent;
}

static xiaotai_err_t media_call(xiaotai_transport_host_call_kind_t kind, xiaotai_rtc_conn_t conn, int64_t value,
                                const xiaotai_link_frame_t *frame, int *detail)
{
    pthread_mutex_lock(&s_lock);
    const bool fail = s_fail_media.times && s_fail_media.kind == kind;
    if (fail) --s_fail_media.times;
    const xiaotai_err_t result = fail ? s_fail_media.result : XIAOTAI_OK;
    if (detail) *detail = fail ? s_fail_media.detail : 0;
    if (frame) s_last_frame = *frame;
    record(kind, value, result, conn);
    const xiaotai_transport_host_media_hook_t hook = s_media_hook;
    void *ctx = s_hook_ctx;
    pthread_mutex_unlock(&s_lock);
    if (hook) hook(ctx, kind, conn, value);
    return result;
}

xiaotai_err_t xiaotai_rtc_send(xiaotai_rtc_conn_t conn, const xiaotai_link_frame_t *frame, const uint8_t *data,
                               size_t size, int *detail)
{
    if (!conn || !frame || !data || !size) return XIAOTAI_ERR_INVALID_ARG;
    return media_call(XIAOTAI_TRANSPORT_HOST_SEND, conn, (int64_t)size, frame, detail);
}

size_t xiaotai_rtc_backlog(xiaotai_rtc_conn_t conn)
{
    pthread_mutex_lock(&s_lock);
    size_t bytes = s_backlog;
    for (size_t i = 0; conn && i < XIAOTAI_TRANSPORT_LINKS_MAX; ++i) {
        if (s_link_backlogs[i].conn == conn) {
            bytes = s_link_backlogs[i].bytes;
            break;
        }
    }
    record(XIAOTAI_TRANSPORT_HOST_BACKLOG, (int64_t)bytes, XIAOTAI_OK, conn);
    pthread_mutex_unlock(&s_lock);
    return bytes;
}

xiaotai_err_t xiaotai_rtc_command(xiaotai_rtc_conn_t conn, uint32_t word, const void *data, size_t size)
{
    if (size && !data) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t err = media_call(XIAOTAI_TRANSPORT_HOST_COMMAND, conn, word, NULL, NULL);
    if (err != XIAOTAI_OK) return err;
    pthread_mutex_lock(&s_lock);
    const xiaotai_transport_host_command_hook_t hook = s_command_hook;
    void *ctx = s_command_ctx;
    pthread_mutex_unlock(&s_lock);
    if (hook) hook(ctx, conn, word, data, size);
    return err;
}

void xiaotai_transport_host_command_hook(xiaotai_transport_host_command_hook_t fn, void *ctx)
{
    pthread_mutex_lock(&s_lock);
    s_command_hook = fn;
    s_command_ctx = ctx;
    pthread_mutex_unlock(&s_lock);
}

xiaotai_err_t xiaotai_rtc_subscribe(xiaotai_rtc_conn_t conn, xiaotai_link_media_kind_t kind, uint8_t stream, bool on)
{
    return media_call(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE, conn, ((int64_t)kind << 16) | ((int64_t)stream << 8) | on,
                      NULL, NULL);
}

xiaotai_err_t xiaotai_rtc_request_key_frame(xiaotai_rtc_conn_t conn, uint8_t stream)
{
    return media_call(XIAOTAI_TRANSPORT_HOST_KEY_FRAME, conn, stream, NULL, NULL);
}

xiaotai_err_t xiaotai_rtc_set_video_bitrate(xiaotai_rtc_conn_t conn, uint8_t stream, uint32_t min_bps,
                                            uint32_t start_bps, uint32_t max_bps)
{
    (void)stream;
    if (!min_bps || min_bps >= start_bps || start_bps >= max_bps) return XIAOTAI_ERR_INVALID_ARG;
    return media_call(XIAOTAI_TRANSPORT_HOST_BITRATE, conn, start_bps, NULL, NULL);
}

void xiaotai_transport_host_frame(uintptr_t conn, const xiaotai_link_frame_t *frame, const uint8_t *data, size_t size)
{
    xiaotai_rtc_on_frame(conn, frame, data, size);
}
void xiaotai_transport_host_command(uintptr_t conn, uint32_t request, uint32_t word, const uint8_t *data, size_t size)
{
    xiaotai_rtc_on_command(conn, request, word, data, size);
}
xiaotai_err_t xiaotai_transport_host_peer_subscribe(uintptr_t conn, uint32_t request,
                                                  xiaotai_link_media_kind_t kind, uint8_t stream, bool on)
{
    return xiaotai_rtc_on_subscribe(conn, request, kind, stream, on);
}
void xiaotai_transport_host_key_frame(uintptr_t conn, uint8_t stream)
{
    xiaotai_rtc_on_key_frame_request(conn, stream);
}
void xiaotai_transport_host_bitrate(uintptr_t conn, uint8_t stream, uint32_t bps)
{
    xiaotai_rtc_on_video_bitrate(conn, stream, bps);
}
