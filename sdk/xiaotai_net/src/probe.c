// SPDX-License-Identifier: MIT
#include "trace_internal.h"

#include <string.h>

bool net_diag_private_ipv4(const uint8_t addr[4])
{
    return addr && (addr[0] == 10 || addr[0] == 127 ||
           (addr[0] == 192 && addr[1] == 168) ||
           (addr[0] == 172 && addr[1] >= 16 && addr[1] <= 31) ||
           (addr[0] == 169 && addr[1] == 254));
}

uint32_t net_diag_be32(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

void net_probe_rx_begin(net_probe_rx_t *rx, uint64_t now_us)
{
    *rx = (net_probe_rx_t){.start_us = now_us};
}

uint32_t net_probe_rx_remaining_ms(const net_probe_rx_t *rx, uint64_t now_us)
{
    const uint64_t duration_us = (uint64_t)NET_PROBE_LIMIT_MS * 1000U;
    const uint64_t elapsed_us = now_us - rx->start_us;
    return elapsed_us >= duration_us ? 0 : (uint32_t)((duration_us - elapsed_us + 999U) / 1000U);
}

void net_probe_rx_packet(net_probe_rx_t *rx, uint64_t now_us, const uint8_t from[4],
                         const uint8_t *data, size_t size)
{
    if (!net_probe_rx_remaining_ms(rx, now_us) || !net_diag_private_ipv4(from)) return;
    if (!data || size < NET_PROBE_HEADER || memcmp(data, "XTP1", 4) != 0) {
        rx->result.invalid++;
        return;
    }
    const uint32_t seq = net_diag_be32(data + 4);
    if (seq >= NET_PROBE_SEQUENCES) {
        rx->result.invalid++;
        return;
    }
    const uint8_t mask = (uint8_t)(1U << (seq % 8U));
    if (rx->seen[seq / 8U] & mask) {
        rx->result.duplicate++;
    } else {
        rx->seen[seq / 8U] |= mask;
        rx->result.unique++;
        rx->result.phases[seq / (NET_PROBE_SEQUENCES / NET_PROBE_PHASES)]++;
    }
    if (seq > rx->result.max_seq) rx->result.max_seq = seq;
}
