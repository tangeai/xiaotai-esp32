// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_NET_TRACE_INTERNAL_H
#define XIAOTAI_NET_TRACE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/net.h"
#include "xiaotai/net_config.h"

/* The address is in network byte order, as it appeared on the wire. */
bool net_diag_private_ipv4(const uint8_t addr[4]);
uint32_t net_diag_be32(const uint8_t bytes[4]);

#define NET_PROBE_PORT 49161U
#define NET_PROBE_LIMIT_MS 12000U
#define NET_PROBE_SEQUENCES 500U
#define NET_PROBE_PHASES 5U
#define NET_PROBE_HEADER 8U

typedef struct {
    uint32_t unique;
    uint32_t duplicate;
    uint32_t invalid;
    uint32_t max_seq;
    uint32_t phases[NET_PROBE_PHASES];
} net_probe_result_t;

typedef struct {
    uint8_t seen[(NET_PROBE_SEQUENCES + 7U) / 8U];
    uint64_t start_us;
    net_probe_result_t result;
} net_probe_rx_t;

void net_probe_rx_begin(net_probe_rx_t *rx, uint64_t now_us);
uint32_t net_probe_rx_remaining_ms(const net_probe_rx_t *rx, uint64_t now_us);
void net_probe_rx_packet(net_probe_rx_t *rx, uint64_t now_us, const uint8_t from[4],
                         const uint8_t *data, size_t size);

typedef void (*net_probe_ready_fn_t)(void);
typedef void (*net_probe_done_fn_t)(xiaotai_err_t error, const net_probe_result_t *result);
/* No server/sender: a native bench receiver bound to the link's IPv4 address. */
xiaotai_err_t net_probe_platform_start(uint32_t ipv4, net_probe_ready_fn_t ready,
                                      net_probe_done_fn_t done);

#if XIAOTAI_NET_PATTERN_PROBE
#include "probe_v2.h"
static inline bool net_probe_v2_private_peer(const uint8_t ip[4])
{
    return net_diag_private_ipv4(ip) && ip[0] != 127 && ip[0] != 169 && ip[3] != 255;
}

typedef void (*net_probe_v2_ready_fn_t)(uint32_t flow);
typedef void (*net_probe_v2_done_fn_t)(xiaotai_err_t error, uint32_t flow,
                                       const net_probe_v2_stats_t *stats, uint32_t peer_rejects);

/* Both addresses are network-order IPv4 bytes stored in a uint32_t. */
xiaotai_err_t net_probe_v2_platform_start(uint32_t ipv4, uint32_t peer_ipv4, uint16_t peer_port,
                                         const net_probe_v2_plan_t *plan, net_probe_v2_ready_fn_t ready,
                                         net_probe_v2_done_fn_t done);
#endif

/* Called by the network engine init once AT verb/stream/notify is linked. */
xiaotai_err_t net_diag_at_register(void);

#if XIAOTAI_NET_PATH_TRACE
#define NET_TRACE_LINE_BYTES 284U /* prefix, 24 unsigned 32-bit values and commas */

/* Control is serialized on the framework owner. The console can read only a
 * frozen capture and must drop its pin before another capture can start. */
xiaotai_err_t net_trace_start(void);
xiaotai_err_t net_trace_stop(void);
xiaotai_err_t net_trace_export_begin(void);
bool net_trace_export_next(char line[NET_TRACE_LINE_BYTES]);
void net_trace_export_end(void);
void net_trace_frame(const void *ethernet, size_t size);
void net_trace_socket(const uint8_t from[4], size_t received);
#endif

#endif
