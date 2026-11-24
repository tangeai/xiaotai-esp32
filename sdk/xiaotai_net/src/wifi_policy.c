// SPDX-License-Identifier: MIT
#include "wifi_policy.h"
#include <string.h>

net_wifi_tx_buffers_t net_wifi_tx_on_demand(net_wifi_tx_buffers_t buffers, bool enable)
{
    if (enable) {
        /* The platform's buffer selector uses 0 for static, 1 for dynamic. */
        if (buffers.type == 0) {
            buffers.type = 1;
            buffers.dynamic_count = buffers.static_count;
            buffers.static_count = 0;
        }
        buffers.cache_count = 0;
    }
    return buffers;
}

bool net_wifi_flow_joined(net_wifi_flow_t *flow, const uint8_t *ssid, size_t length,
                          xiaotai_net_driver_event_t *event)
{
    /* The link event uses an SSID string; never turn raw bytes into a match for a shorter name. */
    if (!flow->join || flow->associated || flow->leaving || !ssid || !length ||
        length > XIAOTAI_NET_SSID_MAX || memchr(ssid, '\0', length)) return false;
    flow->associated = true;
    *event = (xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_JOINED, .token = flow->join};
    memcpy(event->ssid, ssid, length);
    return true;
}

bool net_wifi_flow_address(net_wifi_flow_t *flow, uint32_t ipv4, const uint8_t *ipv6,
                           xiaotai_net_driver_event_t *event)
{
    if (!flow->join || !flow->associated || flow->leaving || (!ipv4 && !ipv6)) return false;
    if (ipv4) flow->ipv4 = ipv4;
    if (ipv6) {
        flow->ipv6_valid = true;
        memcpy(flow->ipv6, ipv6, sizeof(flow->ipv6));
    }
    *event = (xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = flow->join,
                                         .ipv4 = flow->ipv4, .ipv6_valid = flow->ipv6_valid};
    if (flow->ipv6_valid) memcpy(event->ipv6, flow->ipv6, sizeof(event->ipv6));
    return true;
}

bool net_wifi_flow_lost(net_wifi_flow_t *flow, xiaotai_net_loss_t loss, int code,
                        xiaotai_net_driver_event_t *event)
{
    if (!flow->join) return false;
    *event = (xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_LOST, .token = flow->join,
        .loss = flow->leaving ? XIAOTAI_NET_LOSS_REQUESTED : loss, .code = code,
        .signal_valid = flow->signal_valid, .signal_dbm = flow->signal_dbm};
    flow->join = 0;
    flow->associated = flow->leaving = flow->signal_valid = flow->ipv6_valid = false;
    flow->ipv4 = 0;
    return true;
}

bool net_wifi_flow_scan_done(net_wifi_flow_t *flow, xiaotai_err_t error, xiaotai_net_driver_event_t *event)
{
    if (flow->scan_canceling) {
        flow->scan_canceling = false;
        return false;
    }
    if (!flow->scan) return false;
    *event = (xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = flow->scan,
                                         .error = error};
    flow->scan_result = error == XIAOTAI_OK;
    flow->scan = 0;
    return true;
}
