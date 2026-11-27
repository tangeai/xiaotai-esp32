// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/eventlog.h"
#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
#include "diag_mini.h"
#endif

enum { TRACE_DEPTH = 8 };
typedef struct { uint32_t generation, word, bytes, sequence; int result; unsigned direction; } command_row_t;
static command_row_t s_trace[TRACE_DEPTH];
static uint32_t s_sequence;
static unsigned s_next, s_count;

void xt_diag_link(const char *label, uint32_t generation, int detail, bool warning)
{
    xiaotai_eventlog_emit(&(xiaotai_eventlog_event_t){.source = "link", .label = label, .generation = generation,
        .value = detail, .level = warning ? XIAOTAI_EVENTLOG_WARN : XIAOTAI_EVENTLOG_DEBUG,
        .kind = XIAOTAI_EVENTLOG_LINK});
}

void xt_diag_command_locked(unsigned direction, uint32_t generation, uint32_t word, uint32_t bytes, int result)
{
    if (++s_sequence == 0) ++s_sequence;
    s_trace[s_next] = (command_row_t){generation, word, bytes, s_sequence, result, direction};
    s_next = (s_next + 1U) % TRACE_DEPTH;
    if (s_count < TRACE_DEPTH) ++s_count;
}

void xt_diag_reset(void)
{
#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
    xt_diag_mini_enable(false);
#endif
    s_sequence = s_next = s_count = 0;
}

static const char *wait_name(xiaotai_transport_wait_t wait)
{
    switch (wait) {
    case XIAOTAI_TRANSPORT_WAIT_NONE: return "none";
    case XIAOTAI_TRANSPORT_WAIT_IDENTITY: return "identity";
    case XIAOTAI_TRANSPORT_WAIT_NETWORK: return "network";
    case XIAOTAI_TRANSPORT_WAIT_PLATFORM: return "platform";
    case XIAOTAI_TRANSPORT_WAIT_ENDPOINT: return "endpoint";
    case XIAOTAI_TRANSPORT_WAIT_HOLD: return "hold";
    case XIAOTAI_TRANSPORT_WAIT_MAINTENANCE: return "maintenance";
    case XIAOTAI_TRANSPORT_WAIT_UNBOUND: return "unbound";
    case XIAOTAI_TRANSPORT_WAIT_RETRY: return "retry";
    case XIAOTAI_TRANSPORT_WAIT_START_FAILED: return "start_failed";
    case XIAOTAI_TRANSPORT_WAIT_FAULT: return "fault";
    case XIAOTAI_TRANSPORT_WAIT_PROVISIONING: return "provisioning";
    default: return "other";
    }
}

static void print_trace(const xiaotai_reply_t *reply)
{
    command_row_t trace[TRACE_DEPTH];
    xiaotai_critical_enter(&xt.lock);
    const unsigned size = s_count;
    for (unsigned i = 0; i < size; ++i)
        trace[i] = s_trace[(s_next + TRACE_DEPTH - size + i) % TRACE_DEPTH];
    xiaotai_critical_exit(&xt.lock);
    char line[160];
    snprintf(line, sizeof(line), "+LINK-TRACE: count=%u", size);
    reply->write(reply->ctx, line);
    for (unsigned i = 0; i < size; ++i) {
        static const char *const directions[] = {"rx", "tx", "dispatch"};
        const command_row_t *r = &trace[i];
        snprintf(line, sizeof(line), "+LINK-CMD: dir=%s gen=%" PRIu32 " seq=%" PRIu32
                 " word=0x%08" PRIx32 " bytes=%" PRIu32 " result=%d", directions[r->direction],
                 r->generation, r->sequence, r->word, r->bytes, r->result);
        reply->write(reply->ctx, line);
    }
}

static void print_status(const xiaotai_reply_t *reply)
{
    xiaotai_transport_status_t s;
    xiaotai_transport_status(&s);
    xiaotai_rtc_diag_t d = {0};
    xiaotai_rtc_diag(&d);
    char line[XIAOTAI_AT_OUT_LINE_MAX + 1U];
    snprintf(line, sizeof(line), "+LINK: started=%u connected=%u kind=%u gen=0 send_buffer=%" PRIu32
        " error=%d wait=%s release_stalls=%" PRIu32 " unsolicited_stops=%" PRIu32 " early_dropped=%" PRIu32
        " send_call_max_us=%" PRIu32 " command_pool_bytes=%" PRIu32 " command_pool_peak=%" PRIu32
        " command_largest=%" PRIu32 " engine_bytes=%" PRIu32 " worker_stack_free=%" PRIu32
        " messages=%" PRIu32 " statistics=%" PRIu32 " statistics_skipped=%" PRIu32 " alerts=%" PRIu32,
        (unsigned)(s.state == XIAOTAI_TRANSPORT_ONLINE), (unsigned)s.links_open, (unsigned)s.state,
        s.send_buffer_bytes, s.last_fault_detail ? s.last_fault_detail : s.last_start_detail, wait_name(s.wait),
        s.release_stalls, s.unsolicited_stops, s.early_dropped, s.send_call_max_us, s.command_pool_bytes,
        s.command_pool_peak, s.command_largest, s.engine_bytes, s.worker_stack_free, d.messages,
        d.statistics, d.statistics_skipped, s.alerts);
    reply->write(reply->ctx, line);
    for (uint8_t i = 0; i < s.links_max && xt.slots; ++i) {
        uint32_t generation;
        xiaotai_critical_enter(&xt.lock);
        generation = atomic_load(&xt.slots[i].generation);
        if (!generation) generation = xt.slots[i].ended_generation;
        xiaotai_critical_exit(&xt.lock);
        xiaotai_link_stats_t v;
        if (!xiaotai_link_stats(generation, &v)) continue;
        snprintf(line, sizeof(line), "+LINK-SLOT: slot=%u kind=%u gen=%" PRIu32 " state=%u commands_rx=%" PRIu32
                 " commands_tx=%" PRIu32 " commands_dropped=%" PRIu32 " commands_early=%" PRIu32
                 " command_largest=%" PRIu32 " command_pool_peak=%" PRIu32 " audio_tx=%" PRIu32 " audio_rx=%" PRIu32
                 " video_tx=%" PRIu32 " video_rx=%" PRIu32,
                 i, (unsigned)v.kind, generation, (unsigned)v.state, v.commands_received, v.commands_sent,
                 v.commands_dropped, v.commands_early, v.command_largest, v.command_pool_peak,
                 v.audio.sent, v.audio.received, v.video.sent, v.video.received);
        reply->write(reply->ctx, line);
    }
    snprintf(line, sizeof(line), "+LINK-MEDIA: tx=%" PRIu32 " rx=%" PRIu32 " tx_dropped=%" PRIu32
             " rx_dropped=%" PRIu32 " stale=%" PRIu32, atomic_load(&xt.counters.media_tx),
             atomic_load(&xt.counters.media_rx), atomic_load(&xt.counters.media_tx_dropped),
             atomic_load(&xt.counters.media_rx_dropped), s.stale_calls);
    reply->write(reply->ctx, line);
#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
    xt_diag_mini_report(reply);
#endif
}

static void report(const xiaotai_reply_t *reply)
{
    print_status(reply);
    print_trace(reply);
}

static xiaotai_err_t status_at(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    print_status(reply);
    return XIAOTAI_OK;
}

static xiaotai_err_t trace_at(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    print_trace(reply);
    return XIAOTAI_OK;
}

/* The RTC stack's line of AT+LOGLEVEL, after the console engine's own. */
static xiaotai_err_t log_level_at(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    bool printed = false;
    const unsigned level = xiaotai_rtc_log_level(&printed);
    xiaotai_at_printf(reply, "+LOGLEVEL: target=tirtc level=%u max=%u printed=%u", level,
                      (unsigned)XIAOTAI_RTC_LOG_LEVEL_MAX, (unsigned)printed);
    return XIAOTAI_AT_PASS;
}

static xiaotai_err_t log_level_set_at(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    uint32_t level;
    if (!xiaotai_at_parse_u32(argv[0], 0, XIAOTAI_RTC_LOG_LEVEL_MAX, &level))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "LEVEL");
    return xiaotai_rtc_set_log_level((uint8_t)level);
}

xiaotai_err_t xt_diag_init(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.word = NULL, .run = status_at}, {.word = "TRACE", .run = trace_at},
    };
    static const xiaotai_at_verb_t log_verbs[] = {
        {.run = log_level_at},
        {.word = "TIRTC", .min_args = 1, .max_args = 1, .run = log_level_set_at, .usage = "<0..17>"},
    };
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+LINK", verbs, sizeof(verbs) / sizeof(verbs[0]));
    if (err == XIAOTAI_OK && xiaotai_rtc_caps()->stack_log)
        err = xiaotai_at_verbs_register("AT+LOGLEVEL", log_verbs, sizeof(log_verbs) / sizeof(log_verbs[0]));
    const xiaotai_err_t result = err == XIAOTAI_OK ? xiaotai_diag_register("link", report) : err;
#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
    if (result == XIAOTAI_OK) xt_diag_mini_enable(true);
#endif
    return result;
}
