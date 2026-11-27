// SPDX-License-Identifier: MIT
#include "../src/trace_internal.h"

xiaotai_err_t net_probe_platform_start(uint32_t ipv4, net_probe_ready_fn_t ready,
                                      net_probe_done_fn_t done)
{
    (void)ipv4;
    (void)ready;
    (void)done;
    /* Protocol behavior is tested without opening a host listener. */
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

#if XIAOTAI_NET_PATTERN_PROBE
xiaotai_err_t net_probe_v2_platform_start(uint32_t ipv4, uint32_t peer_ipv4, uint16_t peer_port,
                                         const net_probe_v2_plan_t *plan, net_probe_v2_ready_fn_t ready,
                                         net_probe_v2_done_fn_t done)
{
    (void)ipv4;
    (void)peer_ipv4;
    (void)peer_port;
    (void)plan;
    (void)ready;
    (void)done;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
#endif
