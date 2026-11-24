// SPDX-License-Identifier: MIT
#include "probe_v2.h"

#include <string.h>

_Static_assert(sizeof(net_probe_v2_rx_t) <= 384U, "XTP2 checker exceeds its bounded receive state");

bool net_probe_v2_plan_valid(const net_probe_v2_plan_t *plan)
{
    return plan && plan->flow && plan->packets && plan->packets <= NET_PROBE_V2_MAX_PACKETS &&
           plan->payload_bytes && plan->payload_bytes <= NET_PROBE_V2_MAX_PAYLOAD;
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t seed_packet(uint32_t seed, uint32_t seq)
{
    const uint32_t state = seed ^ (seq * UINT32_C(0x9e3779b9)) ^ UINT32_C(0xa341316c);
    return state ? state : UINT32_C(0x6d2b79f5);
}

static uint8_t next_byte(uint32_t *state)
{
    uint32_t next = *state;
    next ^= next << 13;
    next ^= next >> 17;
    next ^= next << 5;
    *state = next;
    return (uint8_t)next;
}

size_t net_probe_v2_packet(const net_probe_v2_plan_t *plan, uint32_t seq, uint32_t sent_us,
                           uint8_t *data, size_t capacity)
{
    if (!net_probe_v2_plan_valid(plan) || seq >= plan->packets || sent_us > NET_PROBE_V2_MAX_US || !data) return 0;
    const size_t size = NET_PROBE_V2_HEADER + plan->payload_bytes;
    if (capacity < size) return 0;
    memcpy(data, "XTP2", 4);
    write_u32(data + 4, plan->flow);
    write_u32(data + 8, seq);
    write_u32(data + 12, sent_us);
    uint32_t state = seed_packet(plan->seed, seq);
    for (size_t i = NET_PROBE_V2_HEADER; i < size; ++i) data[i] = next_byte(&state);
    return size;
}

bool net_probe_v2_rx_begin(net_probe_v2_rx_t *rx, const net_probe_v2_plan_t *plan)
{
    if (!rx || !net_probe_v2_plan_valid(plan)) return false;
    *rx = (net_probe_v2_rx_t){.plan = *plan, .active = true};
    return true;
}

static void missing(net_probe_v2_rx_t *rx, unsigned count)
{
    rx->stats.missing += count;
    rx->missing_run = (uint16_t)(rx->missing_run + count);
    if (rx->stats.longest_missing < rx->missing_run) rx->stats.longest_missing = rx->missing_run;
}

static void age(net_probe_v2_rx_t *rx, unsigned count)
{
    const unsigned bits = count < NET_PROBE_V2_WINDOW ? count : NET_PROBE_V2_WINDOW;
    for (unsigned i = 0; i < bits; ++i) {
        if (rx->seen & (UINT64_C(1) << i)) rx->missing_run = 0;
        else missing(rx, 1);
    }
    if (count > bits) missing(rx, count - bits);
    rx->seen = bits == NET_PROBE_V2_WINDOW ? 0 : rx->seen >> bits;
    rx->good = bits == NET_PROBE_V2_WINDOW ? 0 : rx->good >> bits;
    rx->base = (uint16_t)(rx->base + count);
}

static unsigned interval_bin(uint32_t delta)
{
    if (delta < 1000) return 0;
    if (delta < 5000) return 1;
    if (delta < 15000) return 2;
    if (delta < 25000) return 3;
    return 4;
}

static void advance_time(net_probe_v2_rx_t *rx, uint32_t arrival)
{
    enum { BURST_US = 5000 };
    if (rx->have_arrival) ++rx->stats.interval[interval_bin(arrival - rx->last_us)];
    rx->have_arrival = true;
    rx->last_us = arrival;
    while (rx->arrival_count && arrival - rx->arrivals[rx->arrival_first] > BURST_US) {
        rx->arrival_first = (uint8_t)((rx->arrival_first + 1U) % NET_PROBE_V2_WINDOW);
        --rx->arrival_count;
    }
    const unsigned burst = rx->arrival_count + 1U;
    if (burst > rx->stats.burst_5ms) rx->stats.burst_5ms = burst;
    if (rx->arrival_count == NET_PROBE_V2_WINDOW) {
        rx->stats.burst_clipped = true;
        rx->arrival_first = (uint8_t)((rx->arrival_first + 1U) % NET_PROBE_V2_WINDOW);
        --rx->arrival_count;
    }
    rx->arrivals[(rx->arrival_first + rx->arrival_count) % NET_PROBE_V2_WINDOW] = arrival;
    ++rx->arrival_count;
}

void net_probe_v2_rx_packet(net_probe_v2_rx_t *rx, uint32_t arrival_us,
                            const uint8_t *data, size_t size, bool truncated)
{
    if (!rx || !rx->active) return;
    if (!data || size < NET_PROBE_V2_HEADER || memcmp(data, "XTP2", 4) != 0) {
        ++rx->stats.malformed;
        return;
    }
    if (read_u32(data + 4) != rx->plan.flow) {
        ++rx->stats.foreign;
        return;
    }
    const uint32_t seq = read_u32(data + 8);
    if (seq >= rx->plan.packets) {
        ++rx->stats.out_of_range;
        return;
    }
    if (read_u32(data + 12) > NET_PROBE_V2_MAX_US) {
        ++rx->stats.malformed;
        return;
    }
    if (arrival_us > NET_PROBE_V2_MAX_US || (rx->have_arrival && arrival_us < rx->last_us)) {
        ++rx->stats.clock_errors;
        return;
    }
    advance_time(rx, arrival_us);
    if (seq < rx->base) {
        ++rx->stats.late;
        return;
    }
    if (seq - rx->base >= NET_PROBE_V2_WINDOW) age(rx, seq - rx->base - NET_PROBE_V2_WINDOW + 1U);
    const uint64_t bit = UINT64_C(1) << (seq - rx->base);
    if (rx->seen & bit) {
        ++rx->stats.duplicate;
    } else {
        if (rx->stats.received && seq < rx->highest) ++rx->stats.reordered;
        if (seq > rx->highest) rx->highest = (uint16_t)seq;
        rx->seen |= bit;
        ++rx->stats.received;
    }

    if (truncated || size != (size_t)NET_PROBE_V2_HEADER + rx->plan.payload_bytes) {
        ++rx->stats.bad_length;
        return;
    }
    uint32_t state = seed_packet(rx->plan.seed, seq);
    for (size_t i = NET_PROBE_V2_HEADER; i < size; ++i) {
        if (data[i] != next_byte(&state)) {
            ++rx->stats.corrupt;
            return;
        }
    }
    if (!(rx->good & bit)) {
        rx->good |= bit;
        ++rx->stats.valid;
    }
}

const net_probe_v2_stats_t *net_probe_v2_rx_finish(net_probe_v2_rx_t *rx, bool complete_window)
{
    if (!rx) return NULL;
    if (rx->active) {
        if (complete_window) age(rx, (unsigned)(rx->plan.packets - rx->base));
        rx->stats.complete = complete_window;
        rx->active = false;
    }
    return &rx->stats;
}
