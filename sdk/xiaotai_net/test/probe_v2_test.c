// SPDX-License-Identifier: MIT
#include "probe_v2.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static net_probe_v2_rx_t rx;
static uint8_t wire[NET_PROBE_V2_HEADER + NET_PROBE_V2_MAX_PAYLOAD + 1];

static void begin(unsigned count)
{
    const net_probe_v2_plan_t plan = {UINT32_C(0x01020304), UINT32_C(0x89abcdef), (uint16_t)count, 12};
    assert(net_probe_v2_rx_begin(&rx, &plan));
}

static size_t make(uint32_t seq, uint32_t sent_us)
{
    const size_t size = net_probe_v2_packet(&rx.plan, seq, sent_us, wire, sizeof(wire));
    assert(size == (size_t)NET_PROBE_V2_HEADER + rx.plan.payload_bytes);
    return size;
}

static void good(uint32_t seq, uint32_t at)
{
    const size_t size = make(seq, seq * 20000U);
    net_probe_v2_rx_packet(&rx, at, wire, size, false);
}

static void test_wire(void)
{
    static const uint8_t sample[] = {
        'X', 'T', 'P', '2', 1, 2, 3, 4, 0, 0, 0, 7, 0, 0x3c, 0x12, 0x78,
        0xd3, 0x56, 0xcb, 0x2b, 0xca, 0x23, 0x39, 0x45, 0x9e, 0xb1, 0x7c, 0x5f,
    };
    static const uint8_t seed_zero[] = {0x9a, 0x2a, 0xb7, 0xe7, 0x49, 0xa9, 0x57, 0x95, 3, 0x13, 0xf7, 0x48};
    static const uint8_t state_zero[] = {0x1f, 0x19, 0x28, 0x9d, 0xcc, 0xe7, 0x8c, 0x17, 0x42, 0xce, 0xef, 0xbe};
    begin(8);
    assert(make(7, UINT32_C(0x003c1278)) == sizeof(sample));
    assert(!memcmp(wire, sample, sizeof(sample)));
    rx.plan.seed = 0;
    make(0, 0);
    assert(!memcmp(wire + NET_PROBE_V2_HEADER, seed_zero, sizeof(seed_zero)));
    rx.plan.seed = UINT32_C(0xa341316c);
    make(0, NET_PROBE_V2_MAX_US);
    assert(!memcmp(wire + NET_PROBE_V2_HEADER, state_zero, sizeof(state_zero)));
    assert(!net_probe_v2_packet(&rx.plan, 8, 0, wire, sizeof(wire)));
    assert(!net_probe_v2_packet(&rx.plan, 0, NET_PROBE_V2_MAX_US + 1U, wire, sizeof(wire)));
    assert(!net_probe_v2_packet(&rx.plan, 0, 0, wire, sizeof(sample) - 1U));
    const net_probe_v2_plan_t bad[] = {{0, 1, 1, 1}, {1, 1, 0, 1}, {1, 1, 501, 1}, {1, 1, 1, 0}, {1, 1, 1, 513}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!net_probe_v2_rx_begin(&rx, &bad[i]));
        assert(!net_probe_v2_packet(&bad[i], 0, 0, wire, sizeof(wire)));
    }
    assert(rx.plan.packets == 8);
    const net_probe_v2_plan_t maximum = {1, 0, NET_PROBE_V2_MAX_PACKETS, NET_PROBE_V2_MAX_PAYLOAD};
    assert(net_probe_v2_rx_begin(&rx, &maximum));
    const size_t size = make(NET_PROBE_V2_MAX_PACKETS - 1U, NET_PROBE_V2_MAX_US);
    net_probe_v2_rx_packet(&rx, NET_PROBE_V2_MAX_US, wire, size, false);
    const net_probe_v2_stats_t *stats = net_probe_v2_rx_finish(&rx, true);
    assert(stats->valid == 1 && stats->missing == 499);
}

static void test_quality(void)
{
    begin(5);
    good(0, 0);
    good(2, 2000);
    good(1, 3000);
    good(1, 4000);
    const size_t size = make(3, 60000);
    wire[size - 1] ^= 1;
    net_probe_v2_rx_packet(&rx, 5000, wire, size, false);
    const net_probe_v2_stats_t *stats = net_probe_v2_rx_finish(&rx, true);
    assert(stats->complete && stats->received == 4 && stats->valid == 3 && stats->missing == 1);
    assert(stats->longest_missing == 1 && stats->reordered == 1 && stats->duplicate == 1 && stats->corrupt == 1);
    assert(stats->burst_5ms == 5 && stats->interval[1] == 4 && !stats->burst_clipped);

    begin(6);
    good(0, 0);
    make(1, 100);
    wire[NET_PROBE_V2_HEADER] ^= 1;
    net_probe_v2_rx_packet(&rx, 20, wire, size, false);
    net_probe_v2_rx_packet(&rx, 20, wire, size, false);
    good(1, 30);
    good(1, 30);
    make(2, 100);
    net_probe_v2_rx_packet(&rx, 40, wire, size, true);
    net_probe_v2_rx_packet(&rx, 50, wire, size - 1U, false);
    net_probe_v2_rx_packet(&rx, 60, wire, size + 1U, false);
    good(2, 70);
    stats = net_probe_v2_rx_finish(&rx, true);
    assert(stats->received == 3 && stats->valid == 3 && stats->missing == 3 && stats->longest_missing == 3);
    assert(stats->duplicate == 6 && stats->corrupt == 2 && stats->bad_length == 3);
}

static void test_window(void)
{
    begin(68);
    good(0, 0);
    good(63, 1);
    assert(!rx.stats.missing);
    good(64, 2);
    assert(!rx.stats.missing);
    good(65, 3);
    assert(rx.stats.missing == 1);
    good(0, 4);
    good(1, 5);
    good(2, 6);
    const net_probe_v2_stats_t *stats = net_probe_v2_rx_finish(&rx, true);
    assert(stats->received == 5 && stats->valid == 5 && stats->missing == 63 && stats->longest_missing == 60);
    assert(stats->late == 2 && stats->reordered == 1 && !stats->duplicate);

    begin(500);
    good(0, 0);
    good(499, 1);
    assert(rx.stats.missing == 435 && rx.stats.longest_missing == 435);
    stats = net_probe_v2_rx_finish(&rx, true);
    assert(stats->missing == 498 && stats->longest_missing == 498);
    assert(net_probe_v2_rx_finish(&rx, true) == stats && stats->missing == 498);

    begin(100);
    good(99, 0);
    stats = net_probe_v2_rx_finish(&rx, false);
    assert(!stats->complete && stats->missing == 36 && stats->longest_missing == 36);
    good(98, 1);
    assert(net_probe_v2_rx_finish(&rx, true) == stats && !stats->complete && stats->received == 1 && stats->missing == 36);
}

static void test_time_and_namespace(void)
{
    begin(8);
    good(1, 0);
    good(0, 4999); /* Sender time went backwards, receiver time did not. */
    good(2, 5000);
    assert(rx.stats.burst_5ms == 3 && rx.arrival_count == 3);
    good(3, 5001);
    assert(rx.stats.burst_5ms == 3 && rx.arrival_count == 3);
    good(4, 10001);
    good(5, 25001);
    good(6, 50001);
    const size_t size = make(7, 50001);
    net_probe_v2_rx_packet(&rx, 50000, wire, size, false);
    net_probe_v2_rx_packet(&rx, NET_PROBE_V2_MAX_US + 1U, wire, size, false);
    net_probe_v2_rx_packet(&rx, 50001, wire, NET_PROBE_V2_HEADER - 1U, false);
    wire[3] = '1';
    net_probe_v2_rx_packet(&rx, 50001, wire, size, false);
    make(7, 50001);
    wire[4] ^= 1;
    net_probe_v2_rx_packet(&rx, 50001, wire, size, false);
    make(7, 50001);
    wire[11] = 8;
    net_probe_v2_rx_packet(&rx, 50001, wire, size, false);
    make(7, 50001);
    wire[12] = 0xff;
    net_probe_v2_rx_packet(&rx, 50001, wire, size, false);
    const net_probe_v2_stats_t *stats = net_probe_v2_rx_finish(&rx, true);
    static const uint32_t histogram[] = {2, 1, 1, 1, 1};
    assert(stats->received == 7 && stats->valid == 7 && stats->missing == 1 && stats->reordered == 1);
    assert(stats->clock_errors == 2 && stats->malformed == 3 && stats->foreign == 1 && stats->out_of_range == 1);
    assert(!memcmp(stats->interval, histogram, sizeof(histogram)));

    begin(1);
    for (unsigned n = 0; n < NET_PROBE_V2_WINDOW; ++n) good(0, 0);
    assert(rx.stats.burst_5ms == 64 && !rx.stats.burst_clipped);
    good(0, 0);
    stats = net_probe_v2_rx_finish(&rx, true);
    assert(stats->burst_5ms == 65 && stats->burst_clipped && stats->received == 1 && stats->valid == 1);
    assert(stats->duplicate == 64 && stats->interval[0] == 64 && rx.arrival_count == 64);
}

int main(void)
{
    test_wire();
    test_quality();
    test_window();
    test_time_and_namespace();
    puts("XTP2 wire, fixed window, quality and receive timing passed");
    return 0;
}
