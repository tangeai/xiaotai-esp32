// SPDX-License-Identifier: MIT
#include "trace_internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"
#if XIAOTAI_NET_PATTERN_PROBE
#include "../../xiaotai_at/src/internal.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); abort(); } } while (0)

static const xiaotai_at_verb_t *s_verbs;
static size_t s_count;
static bool s_online;
static bool s_console;
static bool s_handover;
static xiaotai_at_stream_t s_stream;
static char s_notice[XIAOTAI_AT_OUT_LINE_MAX + 1];
static bool s_probe_fail;
static unsigned s_probe_starts;
static const uint8_t lan[4] = {192, 168, 1, 2};
static const uint8_t wan[4] = {172, 32, 1, 2};

xiaotai_err_t xiaotai_at_verbs_register(const char *command, const xiaotai_at_verb_t *verbs, size_t count)
{
    CHECK(strcmp(command, "AT+NET") == 0);
    s_verbs = verbs;
    s_count = count;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_at_stream(const xiaotai_reply_t *reply, const xiaotai_at_stream_t *stream)
{
    CHECK(reply && stream && stream->next);
    if (!s_console) return XIAOTAI_ERR_NOT_SUPPORTED;
    CHECK(!s_handover);
    s_stream = *stream;
    s_handover = true;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_at_fail(const xiaotai_reply_t *reply, xiaotai_err_t error, const char *reason)
{
    CHECK(reply && reason);
    return error;
}

xiaotai_err_t xiaotai_at_notifyf(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    const int length = vsnprintf(s_notice, sizeof(s_notice), format, va);
    CHECK(length > 0 && (size_t)length < sizeof(s_notice));
    va_end(va);
    return XIAOTAI_OK;
}

void xiaotai_net_status(xiaotai_net_status_t *status)
{
    *status = (xiaotai_net_status_t){.state = s_online ? XIAOTAI_NET_UP : XIAOTAI_NET_WAITING};
    if (s_online) memcpy(&status->ipv4, lan, sizeof(lan));
}

xiaotai_err_t net_probe_platform_start(uint32_t ipv4, net_probe_ready_fn_t ready, net_probe_done_fn_t done)
{
    CHECK(memcmp(&ipv4, lan, sizeof(lan)) == 0);
    s_probe_starts++;
    ready();
    CHECK(strcmp(s_notice, "+NET-PROBE: port=49161 limit_ms=12000") == 0);
    const net_probe_result_t result = {.unique = 3, .duplicate = 1, .invalid = 2,
                                      .max_seq = 410, .phases = {1, 1, 0, 0, 1}};
    done(s_probe_fail ? XIAOTAI_FAIL : XIAOTAI_OK, &result);
    return XIAOTAI_OK;
}

#if XIAOTAI_NET_PATTERN_PROBE
static unsigned s_v2_starts;

void xiaotai_at_reply_output_fault(const xiaotai_reply_t *reply, xiaotai_at_output_fault_t fault)
{
    (void)reply;
    (void)fault;
    abort();
}

xiaotai_err_t net_probe_v2_platform_start(uint32_t ipv4, uint32_t peer, uint16_t port,
                                         const net_probe_v2_plan_t *plan, net_probe_v2_ready_fn_t ready,
                                         net_probe_v2_done_fn_t done)
{
    const uint8_t expected_peer[4] = {10, 1, 2, 3};
    CHECK(memcmp(&ipv4, lan, 4) == 0 && memcmp(&peer, expected_peer, 4) == 0);
    CHECK(port == 49200 && plan->flow == UINT32_MAX && !plan->seed &&
          plan->packets == NET_PROBE_V2_MAX_PACKETS && plan->payload_bytes == NET_PROBE_V2_MAX_PAYLOAD);
    s_v2_starts++;
    ready(plan->flow);
    CHECK(strcmp(s_notice, "+NET-PROBE2: ready flow=4294967295 port=49161 limit_ms=12000") == 0);
    const net_probe_v2_stats_t stats = {
        .received = 4, .valid = 3, .missing = 497, .longest_missing = 496,
        .reordered = 1, .duplicate = 1, .burst_5ms = 2, .interval = {1, 0, 0, 1, 0},
        .complete = true,
    };
    done(XIAOTAI_OK, plan->flow, &stats, 1);
    return XIAOTAI_OK;
}
#endif

typedef struct { char lines[8][300]; unsigned count; } sink_t;

static void write_line(void *ctx, const char *text)
{
    sink_t *sink = ctx;
    CHECK(sink && sink->count < 8 && strlen(text) < sizeof(sink->lines[0]));
    strcpy(sink->lines[sink->count++], text);
}

static xiaotai_err_t run_words(const char *verb, int argc, const char *const *argv, sink_t *sink, bool consume)
{
    const xiaotai_reply_t reply = {write_line, sink};
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_verbs[i].word, verb) != 0) continue;
        CHECK(!s_handover);
        CHECK(argc >= s_verbs[i].min_args && argc <= s_verbs[i].max_args);
        const xiaotai_err_t error = s_verbs[i].run(argc, argv, &reply);
        if (consume && s_handover) {
            if (error == XIAOTAI_OK) while (s_stream.next(s_stream.ctx, &reply)) { }
            if (s_stream.release) s_stream.release(s_stream.ctx);
            s_handover = false;
        }
        return error;
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

static xiaotai_err_t run(const char *verb, const char *argument, sink_t *sink, bool consume)
{
    return run_words(verb, argument ? 1 : 0, &argument, sink, consume);
}

static void test_pattern_probe(void)
{
    const char *words[] = {"10.1.2.3", "49200", "4294967295", "0", "500", "512"};
#if XIAOTAI_NET_PATTERN_PROBE
    CHECK(s_count == 2);
    s_online = true;
    s_console = false;
    CHECK(run_words("PROBE2", 6, words, NULL, true) == XIAOTAI_ERR_NOT_ALLOWED && !s_v2_starts);
    s_console = true;
    s_online = false;
    CHECK(run_words("PROBE2", 6, words, NULL, true) == XIAOTAI_ERR_INVALID_STATE && !s_v2_starts);
    s_online = true;
    static const struct { size_t index; const char *input; } invalid[] = {
        {0, "8.8.8.8"}, {0, "127.0.0.1"}, {0, "169.254.1.2"}, {0, "192.168.1.2"},
        {0, "10.01.2.3"}, {0, "10.1.256.3"}, {0, "10.1.2.3.host"}, {0, "10.1.2.255"},
        {1, "0"}, {1, "1023"}, {1, "65536"}, {2, "0"}, {2, "4294967296"}, {3, "-1"},
        {4, "0"}, {4, "501"}, {5, "0"}, {5, "513"},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        const size_t n = invalid[i].index;
        const char *saved = words[n];
        words[n] = invalid[i].input;
        CHECK(run_words("PROBE2", 6, words, NULL, true) == XIAOTAI_ERR_INVALID_ARG && !s_v2_starts);
        words[n] = saved;
    }
    CHECK(run_words("PROBE2", 6, words, NULL, true) == XIAOTAI_OK && s_v2_starts == 1);
    CHECK(strcmp(s_notice, "+NET-PROBE2: done flow=4294967295 rc=0 complete=1"
        " rx=4 good=3 loss=497 ooo=1 dup=1 bad=0 len=0 late=0 max_gap=496 burst=2 overflow=0"
        " malformed=0 foreign=0 range=0 clock=0 peer=1 bins=1/0/0/1/0") == 0);
#else
    CHECK(run_words("PROBE2", 6, words, NULL, true) == XIAOTAI_ERR_NOT_FOUND);
#endif
}

static void put32(uint8_t *data, uint32_t number)
{
    for (unsigned i = 0; i != 4; ++i) data[i] = (uint8_t)(number >> (24 - i * 8));
}

static void test_probe(void)
{
    const uint8_t private_ranges[][4] = {{10}, {127}, {192, 168}, {172, 16}, {172, 31}, {169, 254}};
    const uint8_t other_ranges[][4] = {{9}, {126}, {192, 169}, {172, 15}, {172, 32}, {169, 253}, {100, 64}, {0}};
    for (size_t i = 0; i < sizeof(private_ranges) / 4; ++i) CHECK(net_diag_private_ipv4(private_ranges[i]));
    for (size_t i = 0; i < sizeof(other_ranges) / 4; ++i) CHECK(!net_diag_private_ipv4(other_ranges[i]));
    CHECK(!net_diag_private_ipv4(NULL));

    net_probe_rx_t rx;
    net_probe_rx_begin(&rx, 5000000);
    CHECK(net_probe_rx_remaining_ms(&rx, 5000000) == 12000);
    CHECK(net_probe_rx_remaining_ms(&rx, 16999999) == 1);
    CHECK(net_probe_rx_remaining_ms(&rx, 17000000) == 0);
    uint8_t header[12] = "XTP1";
    static const uint32_t sequences[] = {499, 0, 1, 100, 200, 300, 400, 100};
    for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]); ++i) {
        put32(header + 4, sequences[i]);
        net_probe_rx_packet(&rx, 5010000, lan, header, sizeof(header));
    }
    put32(header + 4, 2);
    net_probe_rx_packet(&rx, 5010000, wan, header, sizeof(header));
    net_probe_rx_packet(&rx, 5010000, lan, header, 7);
    net_probe_rx_packet(&rx, 17000000, lan, header, sizeof(header));
    put32(header + 4, 500);
    net_probe_rx_packet(&rx, 5010000, lan, header, sizeof(header));
    header[0] = 'Y';
    net_probe_rx_packet(&rx, 5010000, lan, header, sizeof(header));
    CHECK(rx.result.unique == 7 && rx.result.duplicate == 1 && rx.result.invalid == 3);
    CHECK(rx.result.max_seq == 499);
    const uint32_t phases[] = {2, 1, 1, 1, 2};
    CHECK(memcmp(rx.result.phases, phases, sizeof(phases)) == 0);

    CHECK(run("PROBE", NULL, NULL, true) == XIAOTAI_ERR_NOT_ALLOWED && s_probe_starts == 0);
    s_console = true;
    CHECK(run("PROBE", NULL, NULL, true) == XIAOTAI_ERR_INVALID_STATE && s_probe_starts == 0);
    s_online = true;
    CHECK(run("PROBE", NULL, NULL, true) == XIAOTAI_OK && s_probe_starts == 1);
    CHECK(strcmp(s_notice, "+NET-PROBE: unique=3 duplicate=1 invalid=2 max_seq=410 phases=1/1/0/0/1") == 0);
    s_probe_fail = true;
    CHECK(run("PROBE", NULL, NULL, true) == XIAOTAI_OK && s_probe_starts == 2);
    CHECK(strcmp(s_notice, "+NET-PROBE: error=RECEIVE_FAILED") == 0);
}

#if XIAOTAI_NET_PATH_TRACE
static bool s_activity;
static bool s_quiesce;
static bool s_timer_fail;
static bool s_event_fail;
static bool s_alloc_fail;
static unsigned s_allocs;
static unsigned s_clock_reads;
static unsigned s_event_count;
static uint64_t s_now = 20000000;
static uint64_t s_due;
static int s_events[12];
static xiaotai_timer_fn_t s_timer_cb;
static void *s_timer_ctx;

void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    CHECK(kind == XIAOTAI_MEM_BULK && count == 3 && size == 23 * sizeof(uint32_t));
    s_allocs++;
    return s_alloc_fail ? NULL : __real_xiaotai_mem_calloc(kind, count, size);
}

int64_t __wrap_xiaotai_time_us(void)
{
    s_clock_reads++;
    return (int64_t)s_now;
}

uint32_t xiaotai_activity_current(const char **name) { CHECK(!name); return s_activity; }
bool xiaotai_quiesced(void) { return s_quiesce; }

xiaotai_err_t xiaotai_event_emit(const char *name, const xiaotai_args_t *args, const void *data, size_t size)
{
    CHECK(strcmp(name, XIAOTAI_EVENT_NET_TRACE) == 0 && !data && size == 0);
    if (s_event_fail) return XIAOTAI_ERR_NO_MEM;
    CHECK(s_event_count < sizeof(s_events) / sizeof(s_events[0]));
    s_events[s_event_count++] = args->value;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_timer_start(uint32_t delay, xiaotai_timer_fn_t cb, void *ctx, uint32_t *id)
{
    if (s_timer_fail) return XIAOTAI_ERR_NO_MEM;
    CHECK(!s_timer_cb && cb && id);
    s_timer_cb = cb;
    s_timer_ctx = ctx;
    s_due = s_now + (uint64_t)delay * 1000;
    *id = 1;
    return XIAOTAI_OK;
}

void xiaotai_timer_cancel(uint32_t id) { CHECK(id == 1 && s_timer_cb); s_timer_cb = NULL; }

static void expire(void)
{
    CHECK(s_timer_cb && s_now >= s_due);
    xiaotai_timer_fn_t cb = s_timer_cb;
    s_timer_cb = NULL;
    cb(s_timer_ctx);
}

static void frame(uint8_t *p, const uint8_t addr[4], uint8_t ihl, bool is_probe, uint32_t sequence)
{
    memset(p, 0, 64);
    p[12] = 8;
    p[14] = 0x40 | ihl;
    p[17] = ihl * 4 + (is_probe ? 16 : 8);
    p[20] = 0x40; /* DF alone is not a fragment. */
    p[23] = 17;
    memcpy(p + 26, addr, 4);
    uint8_t *udp = p + 14 + ihl * 4;
    udp[5] = is_probe ? 16 : 8;
    if (is_probe) {
        udp[2] = 49162 >> 8;
        udp[3] = 49162 & 255;
        memcpy(udp + 8, "XTLN", 4);
        put32(udp + 12, sequence);
    }
}

static void test_trace(void)
{
    uint8_t packet[64];
    frame(packet, lan, 5, true, UINT32_C(0xfffffffe));
    xiaotai_net_trace_media(true);
    xiaotai_net_trace_transport(NULL);
    net_trace_frame(packet, sizeof(packet));
    net_trace_socket(wan, 7);
    CHECK(s_clock_reads == 0 && !xiaotai_net_trace_active() && !xiaotai_net_trace_transport_active());
    CHECK(s_count == 2 && s_verbs[1].max_args == 1);

    s_console = false;
    CHECK(run("TRACE", "START", NULL, true) == XIAOTAI_ERR_NOT_ALLOWED);
    CHECK(!xiaotai_net_trace_active());
    s_console = true;
    CHECK(run("TRACE", "WRONG", NULL, true) == XIAOTAI_ERR_INVALID_ARG);
    s_activity = true;
    CHECK(run("TRACE", "START", NULL, true) == XIAOTAI_ERR_BUSY);
    CHECK(run("TRACE", NULL, NULL, true) == XIAOTAI_ERR_BUSY);
    s_activity = false;
    s_quiesce = true;
    CHECK(net_trace_start() == XIAOTAI_ERR_BUSY && net_trace_export_begin() == XIAOTAI_ERR_BUSY);
    s_quiesce = false;
    s_alloc_fail = true;
    CHECK(net_trace_start() == XIAOTAI_ERR_NO_MEM && !s_timer_cb && !xiaotai_net_trace_active());
    s_alloc_fail = false;
    s_timer_fail = true;
    CHECK(net_trace_start() == XIAOTAI_ERR_NO_MEM && !xiaotai_net_trace_active());
    s_timer_fail = false;
    s_event_fail = true;
    CHECK(net_trace_start() == XIAOTAI_ERR_NO_MEM && !s_timer_cb && !xiaotai_net_trace_active());
    s_event_fail = false;

    CHECK(run("TRACE", "START", NULL, true) == XIAOTAI_OK);
    CHECK(xiaotai_net_trace_active());
    CHECK(xiaotai_net_trace_transport_active() == !!XIAOTAI_NET_PATH_TRACE_TRANSPORT);
    CHECK(s_events[0] == 1 && s_event_count == 1 && s_due == 23000000);
    CHECK(net_trace_start() == XIAOTAI_ERR_BUSY && net_trace_export_begin() == XIAOTAI_ERR_BUSY);
    s_console = false;
    CHECK(run("TRACE", "STOP", NULL, true) == XIAOTAI_ERR_NOT_ALLOWED && xiaotai_net_trace_active());
    s_console = true;

    net_trace_frame(packet, sizeof(packet));
    net_trace_socket(wan, 100);
    xiaotai_net_trace_media(false);
    xiaotai_net_trace_transport(&(xiaotai_net_trace_transport_t){1, 17, 1, 4, 6, 8});
    s_now = 20090000;
    xiaotai_net_trace_media(true);
    s_now = 20100000;
    xiaotai_net_trace_media(true);
    xiaotai_net_trace_transport(&(xiaotai_net_trace_transport_t){2, 13, 2, 3, 7, 6});
    frame(packet, wan, 5, false, 0);
    net_trace_frame(packet, 15); /* truncated */
    packet[20] = 0x20;
    net_trace_frame(packet, sizeof(packet)); /* MF */
    packet[20] = 0;
    packet[21] = 1;
    net_trace_frame(packet, sizeof(packet)); /* nonzero fragment offset */
    packet[21] = 0;
    packet[23] = 6;
    net_trace_frame(packet, sizeof(packet)); /* TCP */
    packet[23] = 17;
    packet[39] = 9;
    net_trace_frame(packet, sizeof(packet)); /* exceeds declared IP length */
    packet[39] = 8;
    net_trace_frame(packet, sizeof(packet)); /* padded Ethernet frame still counts 28 IP bytes */
    s_now = 20300000;
    frame(packet, wan, 6, true, 1);
    net_trace_frame(packet, sizeof(packet)); /* extra IPv4 options, wrapping XTLN forward */
    s_now = 20350000;
    frame(packet, lan, 5, true, 0);
    net_trace_frame(packet, sizeof(packet)); /* older packet */
    s_now = 20400000;
    frame(packet, lan, 5, true, 1);
    net_trace_frame(packet, sizeof(packet)); /* same highest */
    packet[42] = 'Z';
    net_trace_frame(packet, sizeof(packet)); /* ordinary LAN, wrong magic */
    packet[42] = 'X';
    packet[37]--;
    net_trace_frame(packet, sizeof(packet)); /* ordinary LAN, wrong destination */

    s_now = 21100000;
    frame(packet, lan, 5, true, 2);
    net_trace_frame(packet, sizeof(packet));
    frame(packet, wan, 5, false, 0);
    net_trace_frame(packet, sizeof(packet));
    net_trace_socket(wan, 101);
    net_trace_socket(lan, 200);
    net_trace_socket(wan, 0);
    xiaotai_net_trace_media(false);
    s_activity = true; /* an active capture does not revoke or prevent an activity */
    s_now = 23000000;
    CHECK(!xiaotai_net_trace_active());
    xiaotai_net_trace_media(false);
    expire();
    CHECK(!xiaotai_net_trace_transport_active() && s_event_count == 2 && s_events[1] == 0);
    CHECK(run("TRACE", NULL, NULL, true) == XIAOTAI_ERR_BUSY);
    s_activity = false;

    s_console = false;
    CHECK(run("TRACE", NULL, NULL, true) == XIAOTAI_ERR_NOT_ALLOWED);
    s_console = true;
    sink_t sink = {0};
    CHECK(run("TRACE", NULL, &sink, false) == XIAOTAI_OK && s_handover);
    CHECK(net_trace_export_begin() == XIAOTAI_ERR_BUSY && net_trace_start() == XIAOTAI_ERR_BUSY);
    const xiaotai_reply_t reply = {write_line, &sink};
    while (s_stream.next(s_stream.ctx, &reply)) { }
    s_stream.release(s_stream.ctx);
    s_handover = false;
    CHECK(sink.count == 6);
#if XIAOTAI_NET_PATH_TRACE_TRANSPORT
    CHECK(strcmp(sink.lines[0], "+NET-TRACE: BEGIN,start_ms=20000,bins=3,bytes=276,transport=1") == 0);
    CHECK(strcmp(sink.lines[2], "+NET-TRACE: 0,2,5,68,1,0,100,1,2,1,1,200,0,0,10,17,2,4,7,8,4,2,2,1") == 0);
#else
    CHECK(strcmp(sink.lines[0], "+NET-TRACE: BEGIN,start_ms=20000,bins=3,bytes=276,transport=0") == 0);
    CHECK(strcmp(sink.lines[2], "+NET-TRACE: 0,2,5,68,1,0,100,1,2,0,0,200,0,0,10,0,0,0,0,0,4,2,2,1") == 0);
#endif
    CHECK(strcmp(sink.lines[1], "+NET-TRACE: columns=second,net_wan,net_lan,net_wan_bytes,socket_wan,socket_lan,socket_wan_bytes,audio,video,tp_audio,tp_video,net_gap_ms,socket_gap_ms,audio_gap_ms,video_gap_ms,rtt_ms,nack,retx,send_loss,receive_loss,probe,probe_gaps,probe_duplicate,probe_last") == 0);
    CHECK(strcmp(sink.lines[3], "+NET-TRACE: 1,1,1,28,1,1,101,1,0,0,0,800,1100,1100,0,0,0,0,0,0,1,0,0,2") == 0);
    CHECK(strcmp(sink.lines[4], "+NET-TRACE: 2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0") == 0);
    CHECK(strcmp(sink.lines[5], "+NET-TRACE: END") == 0);

    CHECK(run("TRACE", "START", NULL, true) == XIAOTAI_OK);
    s_activity = true;
    s_event_fail = true;
    CHECK(run("TRACE", "STOP", NULL, true) == XIAOTAI_OK && !xiaotai_net_trace_active());
    CHECK(s_event_count == 3 && s_events[2] == 1 && s_due == 23010000);
    CHECK(net_trace_stop() == XIAOTAI_OK); /* no duplicate stop */
    s_activity = false;
    CHECK(net_trace_start() == XIAOTAI_ERR_BUSY); /* pending stop cannot reorder events */
    s_event_fail = false;
    s_now = 23010000;
    expire();
    CHECK(s_event_count == 4 && s_events[3] == 0);
    CHECK(s_allocs == 4); /* failed allocation, two failed starts, one reusable recording */
}
#else
static void test_trace(void)
{
    CHECK(s_count == 1 + XIAOTAI_NET_PATTERN_PROBE);
    xiaotai_net_trace_media(true);
    xiaotai_net_trace_transport(&(xiaotai_net_trace_transport_t){.rtt_ms = 11});
    CHECK(!xiaotai_net_trace_active() && !xiaotai_net_trace_transport_active());
    CHECK(run("TRACE", "START", NULL, true) == XIAOTAI_ERR_NOT_FOUND);
}
#endif

int main(void)
{
    CHECK(net_diag_at_register() == XIAOTAI_OK);
    test_probe();
    test_pattern_probe();
    test_trace();
    return 0;
}
