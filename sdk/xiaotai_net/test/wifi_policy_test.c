// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_wifi_credentials.c::wifi_credentials
#include "wifi_policy.h"
#include <assert.h>
#include <string.h>

static void check(xiaotai_net_dns_fallback_t rule, bool address, const net_dns_state_t state[],
                  net_dns_operation_t expected, net_dns_slot_t slot)
{
    net_dns_plan_t plan = net_dns_plan(rule, address, state);
    assert(plan.operation == expected);
    if (expected != NET_DNS_DONE) assert(plan.slot == slot);
}

int main(void)
{
    net_dns_state_t state[NET_DNS_COUNT] = {0};
    xiaotai_net_dns_fallback_t all = XIAOTAI_NET_DNS_FALLBACK_ALL_EMPTY;
    xiaotai_net_dns_fallback_t last = XIAOTAI_NET_DNS_FALLBACK_SLOT_EMPTY;
    xiaotai_net_dns_fallback_t never = XIAOTAI_NET_DNS_FALLBACK_NEVER;
    check(never, true, state, NET_DNS_DONE, NET_DNS_FALLBACK);
    check(all, false, state, NET_DNS_DONE, NET_DNS_FALLBACK);
    check(last, false, state, NET_DNS_DONE, NET_DNS_FALLBACK);
    check(never, false, state, NET_DNS_DONE, NET_DNS_FALLBACK);
    check(all, true, state, NET_DNS_READ, NET_DNS_MAIN);
    state[NET_DNS_MAIN] = NET_DNS_EMPTY;
    check(all, true, state, NET_DNS_READ, NET_DNS_BACKUP);
    state[NET_DNS_BACKUP] = NET_DNS_EMPTY;
    check(all, true, state, NET_DNS_READ, NET_DNS_FALLBACK);
    state[NET_DNS_FALLBACK] = NET_DNS_EMPTY;
    check(all, true, state, NET_DNS_WRITE, NET_DNS_FALLBACK);
    for (net_dns_slot_t i = NET_DNS_MAIN; i < NET_DNS_COUNT; ++i) {
        state[i] = NET_DNS_PRESENT;
        check(all, true, state, NET_DNS_DONE, i);
        state[i] = NET_DNS_FAILED;
        check(all, true, state, NET_DNS_ERROR, i);
        state[i] = NET_DNS_EMPTY;
    }
    state[NET_DNS_MAIN] = NET_DNS_PRESENT;
    state[NET_DNS_BACKUP] = NET_DNS_FAILED;
    state[NET_DNS_FALLBACK] = NET_DNS_UNREAD;
    check(last, true, state, NET_DNS_READ, NET_DNS_FALLBACK);
    state[NET_DNS_FALLBACK] = NET_DNS_EMPTY;
    check(last, true, state, NET_DNS_WRITE, NET_DNS_FALLBACK);
    state[NET_DNS_FALLBACK] = NET_DNS_PRESENT;
    check(last, true, state, NET_DNS_DONE, NET_DNS_FALLBACK);
    state[NET_DNS_FALLBACK] = NET_DNS_FAILED;
    check(last, true, state, NET_DNS_ERROR, NET_DNS_FALLBACK);

    const net_wifi_tx_buffers_t static_tx = {0, 7, 9, 32}, dynamic_tx = {1, 7, 9, 32};
    net_wifi_tx_buffers_t tx = net_wifi_tx_on_demand(static_tx, true);
    assert(tx.type == 1 && tx.static_count == 0 && tx.dynamic_count == 7 && tx.cache_count == 0);
    tx = net_wifi_tx_on_demand(dynamic_tx, true);
    assert(tx.type == 1 && tx.static_count == 7 && tx.dynamic_count == 9 && tx.cache_count == 0);
    tx = net_wifi_tx_on_demand(static_tx, false);
    assert(tx.type == 0 && tx.static_count == 7 && tx.dynamic_count == 9 && tx.cache_count == 32);
    tx = net_wifi_tx_on_demand(dynamic_tx, false);
    assert(tx.type == 1 && tx.static_count == 7 && tx.dynamic_count == 9 && tx.cache_count == 32);

    net_wifi_flow_t flow = {.join = 17};
    xiaotai_net_driver_event_t event;
    const uint8_t ssid[] = "literal-ssid";
    assert(!net_wifi_flow_address(&flow, 123, NULL, &event));
    assert(!net_wifi_flow_joined(&flow, ssid, sizeof(ssid), &event)); /* NUL is part of the reported length. */
    assert(!flow.associated && !net_wifi_flow_address(&flow, 123, NULL, &event));
    assert(net_wifi_flow_joined(&flow, ssid, sizeof(ssid) - 1, &event));
    assert(event.kind == XIAOTAI_NET_DRIVER_JOINED && event.token == 17 && !strcmp(event.ssid, "literal-ssid"));
    assert(!net_wifi_flow_joined(&flow, ssid, sizeof(ssid) - 1, &event));
    assert(net_wifi_flow_address(&flow, 123, NULL, &event));
    assert(event.kind == XIAOTAI_NET_DRIVER_ADDRESS && event.token == 17 && event.ipv4 == 123 && !event.ipv6_valid);
    const uint8_t ipv6[16] = {0x20, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    assert(net_wifi_flow_address(&flow, 0, ipv6, &event));
    assert(event.token == 17 && event.ipv4 == 123 && event.ipv6_valid && !memcmp(event.ipv6, ipv6, sizeof(ipv6)));
    assert(net_wifi_flow_address(&flow, 124, NULL, &event));
    assert(event.ipv4 == 124 && event.ipv6_valid && !memcmp(event.ipv6, ipv6, sizeof(ipv6)));
    flow.leaving = true;
    flow.signal_valid = true;
    flow.signal_dbm = -51;
    assert(!net_wifi_flow_address(&flow, 125, NULL, &event));
    assert(net_wifi_flow_lost(&flow, XIAOTAI_NET_LOSS_SIGNAL, 200, &event));
    assert(event.kind == XIAOTAI_NET_DRIVER_LOST && event.token == 17 &&
           event.loss == XIAOTAI_NET_LOSS_REQUESTED && event.code == 200 &&
           event.signal_valid && event.signal_dbm == -51);
    assert(!net_wifi_flow_lost(&flow, XIAOTAI_NET_LOSS_SIGNAL, 200, &event));
    flow.join = 18;
    assert(net_wifi_flow_joined(&flow, ssid, sizeof(ssid) - 1, &event));
    assert(net_wifi_flow_address(&flow, 127, NULL, &event) && !event.ipv6_valid);
    assert(net_wifi_flow_lost(&flow, XIAOTAI_NET_LOSS_CREDENTIALS, 202, &event));
    assert(event.token == 18 && event.loss == XIAOTAI_NET_LOSS_CREDENTIALS && !event.signal_valid);
    uint8_t full_ssid[XIAOTAI_NET_SSID_MAX];
    memset(full_ssid, 0x84, sizeof(full_ssid));
    flow.join = 19;
    assert(net_wifi_flow_joined(&flow, full_ssid, sizeof(full_ssid), &event));
    assert(!memcmp(event.ssid, full_ssid, sizeof(full_ssid)) && !event.ssid[sizeof(full_ssid)]);

    flow.scan_canceling = true;
    assert(!net_wifi_flow_scan_done(&flow, XIAOTAI_OK, &event));
    assert(!flow.scan_canceling && !flow.scan_result);
    flow.scan = 500;
    assert(net_wifi_flow_scan_done(&flow, XIAOTAI_FAIL, &event));
    assert(event.kind == XIAOTAI_NET_DRIVER_SCAN_DONE && event.token == 500 && event.error == XIAOTAI_FAIL);
    assert(!flow.scan_result);
    assert(!net_wifi_flow_scan_done(&flow, XIAOTAI_OK, &event));
    flow.scan = 501;
    assert(net_wifi_flow_scan_done(&flow, XIAOTAI_OK, &event));
    assert(event.token == 501 && flow.scan_result);
}
