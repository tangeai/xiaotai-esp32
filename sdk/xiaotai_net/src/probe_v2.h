// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_NET_PROBE_V2_H
#define XIAOTAI_NET_PROBE_V2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    NET_PROBE_V2_HEADER = 16,
    NET_PROBE_V2_MAX_PACKETS = 500,
    NET_PROBE_V2_MAX_PAYLOAD = 512,
    NET_PROBE_V2_MAX_US = 12000000,
    NET_PROBE_V2_WINDOW = 64,
    NET_PROBE_V2_INTERVAL_BINS = 5,
};

typedef struct {
    uint32_t flow, seed;
    uint16_t packets, payload_bytes;
} net_probe_v2_plan_t;

/* received includes known-sequence errors; valid can increase on a corrected
 * duplicate. Timing includes in-flow duplicates, late packets and known errors.
 * A clipped burst is a lower bound, not an exact count. */
typedef struct {
    uint32_t received, valid, missing, longest_missing;
    uint32_t reordered, duplicate, late, corrupt, bad_length, malformed, foreign, out_of_range, clock_errors;
    uint32_t burst_5ms, interval[NET_PROBE_V2_INTERVAL_BINS];
    bool burst_clipped, complete;
} net_probe_v2_stats_t;

typedef struct {
    net_probe_v2_plan_t plan;
    net_probe_v2_stats_t stats;
    uint64_t seen, good;
    uint32_t arrivals[NET_PROBE_V2_WINDOW], last_us;
    uint16_t base, highest, missing_run;
    uint8_t arrival_first, arrival_count;
    bool active, have_arrival;
} net_probe_v2_rx_t;

bool net_probe_v2_plan_valid(const net_probe_v2_plan_t *plan);

/* XTP2: magic followed by big-endian flow, zero-based sequence, and sender-relative
 * microseconds. Payload is seeded xorshift32. It is a test pattern, not authentication. */
size_t net_probe_v2_packet(const net_probe_v2_plan_t *plan, uint32_t seq, uint32_t sent_us,
                           uint8_t *data, size_t capacity);

/* A socket owner must first authorize the exact peer and enforce the run limits.
 * arrival_us is relative to this receiver's start, never derived from sent_us. */
bool net_probe_v2_rx_begin(net_probe_v2_rx_t *rx, const net_probe_v2_plan_t *plan);
void net_probe_v2_rx_packet(net_probe_v2_rx_t *rx, uint32_t arrival_us,
                            const uint8_t *data, size_t size, bool truncated);

/* Missing means absent by window eviction, not physical link loss: an expired
 * sequence is late and can no longer change finalized data. Only a complete
 * receive window finalizes the tail; the sender's completion is checked separately. */
const net_probe_v2_stats_t *net_probe_v2_rx_finish(net_probe_v2_rx_t *rx, bool complete_window);

#endif
