// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/port/log.h"

#define STATS_LIFECYCLE(F) \
    F(clock_attempts) F(discoveries) F(services_invalid) F(logins) F(login_failures) F(token_renewals) \
    F(signaling_connects) F(signaling_drops) F(signaling_rebuilds) F(subscribe_retries) F(heartbeats)
#define STATS_REQUEST(F) \
    F(requests) F(replies) F(request_failures) F(requests_stale) F(requests_refused) F(requests_busy) \
    F(transfers) F(transfers_cancelled) F(reply_post_retries) F(slot_clamps)
#define STATS_SIGNAL(F) \
    F(signals) F(signals_delivered) F(signals_acknowledged) F(signal_post_retries) \
    F(signals_malformed) F(signals_oversized) F(signals_fragment_errors) F(signals_no_slot) \
    F(signals_stale) F(signals_unauthorized) F(unbound_reports)
#define STATS_PROFILE(F) \
    F(profile_reports) F(profile_failures) F(profile_deferrals) F(profile_parts_dropped) \
    F(event_post_retries) F(worker_stack_free)

#define STATS_PREFIX "+PLATFORM-STATS:"
#define BYTE_COUNT(field) + (sizeof(#field) - 1U + 2U + sizeof("4294967295") - 1U)
#define COUNTER_SIZE(field) + sizeof(((xiaotai_platform_stats_t *)0)->field)
#define ROW_BOUND(group) (sizeof(STATS_PREFIX) group(BYTE_COUNT))
#define MAX_OF(a, b) ((a) > (b) ? (a) : (b))
enum {
    DIAGNOSTICS_LINE_CAPACITY = MAX_OF(MAX_OF(ROW_BOUND(STATS_LIFECYCLE), ROW_BOUND(STATS_REQUEST)),
                                       MAX_OF(ROW_BOUND(STATS_SIGNAL), ROW_BOUND(STATS_PROFILE)))
};
_Static_assert(DIAGNOSTICS_LINE_CAPACITY <= XIAOTAI_AT_OUT_LINE_MAX + 1U, "Platform diagnostics row exceeds AT limit");
_Static_assert((0 STATS_LIFECYCLE(COUNTER_SIZE) STATS_REQUEST(COUNTER_SIZE)
                 STATS_SIGNAL(COUNTER_SIZE) STATS_PROFILE(COUNTER_SIZE)) == sizeof(xiaotai_platform_stats_t),
               "Platform diagnostics must include every counter");
#undef MAX_OF
#undef ROW_BOUND
#undef BYTE_COUNT
#undef COUNTER_SIZE

bool platform_diagnostic_status(const xiaotai_platform_status_t *s, char *out, size_t capacity)
{
    const int n = snprintf(out, capacity, "+PLATFORM: phase=%u online=%u clock=%u discovered=%u authorized=%u"
        " signaling=%u verifying=%u held=%u gated=%u profile=%u gen=%" PRIu32 " revision=%" PRIu32 " error=%d exchanges=%u",
        (unsigned)s->phase, (unsigned)(s->phase == XIAOTAI_PLATFORM_ONLINE), (unsigned)s->clock_synced,
        (unsigned)s->discovered, (unsigned)s->authorized, (unsigned)s->signaling, (unsigned)s->verifying,
        (unsigned)s->held, (unsigned)s->gated, (unsigned)s->profile_ready, s->generation, s->notify_revision,
        (int)s->last_error, (unsigned)s->exchanges);
    return n > 0 && (size_t)n < capacity;
}

bool platform_diagnostic_stats_row(const xiaotai_platform_stats_t *stats, unsigned row, char *out, size_t capacity)
{
#define FORMAT(field) " " #field "=%" PRIu32
#define VALUE(field) , stats->field
#define ROW(group) snprintf(out, capacity, STATS_PREFIX group(FORMAT) group(VALUE))
    int n;
    switch (row) {
    case 0: n = ROW(STATS_LIFECYCLE); break;
    case 1: n = ROW(STATS_REQUEST); break;
    case 2: n = ROW(STATS_SIGNAL); break;
    case 3: n = ROW(STATS_PROFILE); break;
    default: return false;
    }
    return n > 0 && (size_t)n < capacity;
#undef ROW
#undef FORMAT
#undef VALUE
}

static xiaotai_err_t report(const xiaotai_reply_t *reply)
{
    xiaotai_platform_status_t status = {0};
    xiaotai_platform_stats_t stats = {0};
    if (platform.objects) {
        platform_lock();
        status = platform.status;
        stats = platform.stats;
        if (!atomic_load(&platform.running)) stats.worker_stack_free = 0;
        platform_unlock();
    }
    char line[DIAGNOSTICS_LINE_CAPACITY];
    if (!platform_diagnostic_status(&status, line, sizeof(line)))
        return xiaotai_at_fail(reply, XIAOTAI_FAIL, "OUTPUT");
    reply->write(reply->ctx, line);
    for (unsigned row = 0; row < PLATFORM_DIAGNOSTIC_STATS_ROWS; ++row) {
        if (!platform_diagnostic_stats_row(&stats, row, line, sizeof(line)))
            return xiaotai_at_fail(reply, XIAOTAI_FAIL, "OUTPUT");
        reply->write(reply->ctx, line);
    }
    return XIAOTAI_OK;
}

static void diagnose(const xiaotai_reply_t *reply) { (void)report(reply); }

static xiaotai_err_t at_platform(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return report(reply);
}

xiaotai_err_t platform_diagnostics_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {{.run = at_platform}};
    const xiaotai_err_t err = xiaotai_at_verbs_register("AT+PLATFORM", verbs, 1);
    return err == XIAOTAI_OK ? xiaotai_diag_register("platform", diagnose) : err;
}

#undef STATS_LIFECYCLE
#undef STATS_REQUEST
#undef STATS_SIGNAL
#undef STATS_PROFILE
#undef STATS_PREFIX

static const char *safe_route(const char *path)
{
    /* A caller can put an opaque ID in any segment. Print only the public
     * static route prefix, never a slice of the caller's raw path. */
    static const char *const routes[] = {
        "/v1/ai/token", "/v1/call/cancel", "/v1/call/device", "/v1/call/group", "/v1/call/hangup",
        "/v1/call/reject", "/v1/call/request", "/v1/call/room", "/v1/device/profile", "/v1/device/report",
        "/v1/device/token", "/v1/device/tts", "/v1/recognition/config", "/v1/recognition/requests",
        "/v1/voip/device", "/v1/wxvoip/reject",
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(*routes); ++i) {
        const size_t length = strlen(routes[i]);
        if (!strncmp(path, routes[i], length) && (!path[length] || path[length] == '/' || path[length] == '?'))
            return routes[i];
    }
    return "-";
}

bool platform_http_trace_format(const platform_slot_t *slot, const xiaotai_http_response_t *response,
                               xiaotai_err_t err, char *out, size_t capacity)
{
    if (response->timing.total_ms < 3000U && err == XIAOTAI_OK && response->status >= 200 && response->status < 300) return false;
    static const char *const method[] = {"GET", "POST", "PUT", "DELETE"};
    const xiaotai_http_timing_t *t = &response->timing;
    const int size = snprintf(out, capacity, "%s %s %s status=%d error=%d resolve=%u connect=%u send=%u wait=%u recv=%u total=%u",
        slot->service, method[slot->method], safe_route(slot->path), response->status, (int)err,
        (unsigned)t->resolve_ms, (unsigned)t->connect_ms, (unsigned)t->send_ms,
        (unsigned)t->wait_ms, (unsigned)t->receive_ms, (unsigned)t->total_ms);
    return size > 0 && (size_t)size < capacity;
}

void platform_http_trace(const platform_slot_t *slot, const xiaotai_http_response_t *response, xiaotai_err_t err)
{
    char line[224];
    if (platform_http_trace_format(slot, response, err, line, sizeof(line))) XIAOTAI_LOGI("platform", "%s", line);
}
