// SPDX-License-Identifier: MIT
#include "wifi_policy.h"

net_dns_plan_t net_dns_plan(xiaotai_net_dns_fallback_t rule, bool has_address,
                            const net_dns_state_t slots[NET_DNS_COUNT])
{
    if (!has_address || rule == XIAOTAI_NET_DNS_FALLBACK_NEVER)
        return (net_dns_plan_t){NET_DNS_DONE, NET_DNS_FALLBACK};
    if (rule != XIAOTAI_NET_DNS_FALLBACK_ALL_EMPTY && rule != XIAOTAI_NET_DNS_FALLBACK_SLOT_EMPTY)
        return (net_dns_plan_t){NET_DNS_ERROR, NET_DNS_FALLBACK};

    const net_dns_slot_t first = rule == XIAOTAI_NET_DNS_FALLBACK_ALL_EMPTY ? NET_DNS_MAIN : NET_DNS_FALLBACK;
    bool present = false;
    for (net_dns_slot_t i = first; i < NET_DNS_COUNT; ++i) {
        switch (slots[i]) {
        case NET_DNS_UNREAD: return (net_dns_plan_t){NET_DNS_READ, i};
        case NET_DNS_FAILED: return (net_dns_plan_t){NET_DNS_ERROR, i};
        case NET_DNS_PRESENT: present = true; break;
        case NET_DNS_EMPTY: break;
        }
    }
    return (net_dns_plan_t){present ? NET_DNS_DONE : NET_DNS_WRITE, NET_DNS_FALLBACK};
}
