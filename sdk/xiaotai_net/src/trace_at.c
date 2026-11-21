// SPDX-License-Identifier: MIT
#include "trace_internal.h"

#include <inttypes.h>
#include <string.h>
#include "xiaotai/at_command.h"

/* The framework's generic command runner can call these handlers without a
 * console. Only a console can acquire a reply stream. Check before mutating
 * the capture or opening the LAN receiver, even for commands with no output. */
static bool finish_local(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    (void)reply;
    return false;
}

static xiaotai_err_t require_console(const xiaotai_reply_t *reply)
{
    const xiaotai_at_stream_t stream = {.next = finish_local};
    const xiaotai_err_t error = xiaotai_at_stream(reply, &stream);
    return error == XIAOTAI_ERR_NOT_SUPPORTED ? XIAOTAI_ERR_NOT_ALLOWED : error;
}

static void probe_ready(void)
{
    (void)xiaotai_at_notifyf("+NET-PROBE: port=%u limit_ms=%u", NET_PROBE_PORT, NET_PROBE_LIMIT_MS);
}

static void probe_done(xiaotai_err_t error, const net_probe_result_t *result)
{
    if (error != XIAOTAI_OK) {
        (void)xiaotai_at_notifyf("+NET-PROBE: error=RECEIVE_FAILED");
        return;
    }
    (void)xiaotai_at_notifyf("+NET-PROBE: unique=%" PRIu32 " duplicate=%" PRIu32 " invalid=%" PRIu32
                             " max_seq=%" PRIu32 " phases=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32,
                             result->unique, result->duplicate, result->invalid, result->max_seq,
                             result->phases[0], result->phases[1], result->phases[2], result->phases[3], result->phases[4]);
}

static xiaotai_err_t probe_run(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_err_t error = require_console(reply);
    if (error != XIAOTAI_OK) return error;
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    if (net.state != XIAOTAI_NET_UP || !net.ipv4) {
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "NETWORK_DOWN");
    }
    return net_probe_platform_start(net.ipv4, probe_ready, probe_done);
}

#if XIAOTAI_NET_PATTERN_PROBE
#define PROBE_V2_USAGE "<peer-ipv4> <peer-port> <flow> <seed> <count> <bytes>"

static bool probe_v2_peer(const char *text, uint32_t *ip)
{
    uint8_t bytes[4];
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        const char *start = text;
        unsigned octet = 0;
        while (*text >= '0' && *text <= '9' && text - start < 3)
            octet = octet * 10U + (unsigned)(*text++ - '0');
        if (text == start || (text - start > 1 && *start == '0') || octet > 255U ||
            *text != (i == 3 ? '\0' : '.')) return false;
        bytes[i] = (uint8_t)octet;
        if (i < 3) ++text;
    }
    if (!net_probe_v2_private_peer(bytes)) return false;
    memcpy(ip, bytes, sizeof(bytes));
    return true;
}

static void probe_v2_ready(uint32_t flow)
{
    (void)xiaotai_at_notifyf("+NET-PROBE2: ready flow=%" PRIu32 " port=%u limit_ms=%u",
                             flow, NET_PROBE_PORT, NET_PROBE_LIMIT_MS);
}

static void probe_v2_done(xiaotai_err_t error, uint32_t flow, const net_probe_v2_stats_t *r, uint32_t peers)
{
    (void)xiaotai_at_notifyf("+NET-PROBE2: done flow=%" PRIu32 " rc=%d complete=%u"
        " rx=%" PRIu32 " good=%" PRIu32 " loss=%" PRIu32 " ooo=%" PRIu32 " dup=%" PRIu32
        " bad=%" PRIu32 " len=%" PRIu32 " late=%" PRIu32 " max_gap=%" PRIu32
        " burst=%" PRIu32 " overflow=%u malformed=%" PRIu32 " foreign=%" PRIu32
        " range=%" PRIu32 " clock=%" PRIu32 " peer=%" PRIu32
        " bins=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32,
        flow, (int)error, (unsigned)r->complete, r->received, r->valid, r->missing, r->reordered,
        r->duplicate, r->corrupt, r->bad_length, r->late, r->longest_missing, r->burst_5ms,
        (unsigned)r->burst_clipped, r->malformed, r->foreign, r->out_of_range, r->clock_errors,
        peers, r->interval[0], r->interval[1], r->interval[2], r->interval[3], r->interval[4]);
}

static xiaotai_err_t xiaotai_net_console_probe_v2_run(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    net_probe_v2_plan_t plan;
    uint32_t peer, port, count, bytes;
    if (!probe_v2_peer(argv[0], &peer) ||
        !xiaotai_at_parse_u32(argv[1], 1024, UINT16_MAX, &port) ||
        !xiaotai_at_parse_u32(argv[2], 1, UINT32_MAX, &plan.flow) ||
        !xiaotai_at_parse_u32(argv[3], 0, UINT32_MAX, &plan.seed) ||
        !xiaotai_at_parse_u32(argv[4], 1, NET_PROBE_V2_MAX_PACKETS, &count) ||
        !xiaotai_at_parse_u32(argv[5], 1, NET_PROBE_V2_MAX_PAYLOAD, &bytes))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "PROBE_PLAN");

    xiaotai_err_t error = require_console(reply);
    if (error != XIAOTAI_OK) return error;
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    if (net.state != XIAOTAI_NET_UP || !net.ipv4)
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "NETWORK_DOWN");
    if (net.ipv4 == peer) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "PROBE_PEER");

    plan.packets = (uint16_t)count;
    plan.payload_bytes = (uint16_t)bytes;
    return net_probe_v2_platform_start(net.ipv4, peer, (uint16_t)port, &plan, probe_v2_ready, probe_v2_done);
}
#endif

#if XIAOTAI_NET_PATH_TRACE
_Static_assert(NET_TRACE_LINE_BYTES <= XIAOTAI_AT_OUT_LINE_MAX, "trace export exceeds AT line limit");

static bool xiaotai_net_console_trace_next(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    char line[NET_TRACE_LINE_BYTES];
    if (!net_trace_export_next(line)) return false;
    reply->write(reply->ctx, line);
    return true;
}

static void xiaotai_net_console_trace_release(void *ctx)
{
    (void)ctx;
    net_trace_export_end();
}

static xiaotai_err_t xiaotai_net_console_trace_run(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    if (argc) {
        const bool start = strcmp(argv[0], "START") == 0;
        if (!start && strcmp(argv[0], "STOP") != 0) {
            return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "TRACE_ACTION");
        }
        const xiaotai_err_t error = require_console(reply);
        if (error != XIAOTAI_OK) return error;
        return start ? net_trace_start() : net_trace_stop();
    }

    /* Pin the frozen recording for the stream; a non-console caller or a
     * rejected hand-over cannot keep the pin or see any data. */
    xiaotai_err_t error = net_trace_export_begin();
    if (error != XIAOTAI_OK) return error;
    const xiaotai_at_stream_t stream = {
        .next = xiaotai_net_console_trace_next, .release = xiaotai_net_console_trace_release,
    };
    error = xiaotai_at_stream(reply, &stream);
    if (error != XIAOTAI_OK) net_trace_export_end();
    return error == XIAOTAI_ERR_NOT_SUPPORTED ? XIAOTAI_ERR_NOT_ALLOWED : error;
}
#endif

xiaotai_err_t net_diag_at_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.word = "PROBE", .run = probe_run},
#if XIAOTAI_NET_PATTERN_PROBE
        {.word = "PROBE2", .min_args = 6, .max_args = 6, .run = xiaotai_net_console_probe_v2_run,
         .usage = PROBE_V2_USAGE},
#endif
#if XIAOTAI_NET_PATH_TRACE
        {.word = "TRACE", .max_args = 1, .run = xiaotai_net_console_trace_run, .usage = "[START|STOP]"},
#endif
    };
    return xiaotai_at_verbs_register("AT+NET", verbs, sizeof(verbs) / sizeof(verbs[0]));
}
