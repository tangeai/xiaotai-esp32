// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/net_driver.h"

typedef enum { NET_DNS_MAIN, NET_DNS_BACKUP, NET_DNS_FALLBACK, NET_DNS_COUNT } net_dns_slot_t;
typedef enum { NET_DNS_UNREAD, NET_DNS_EMPTY, NET_DNS_PRESENT, NET_DNS_FAILED } net_dns_state_t;
typedef enum { NET_DNS_DONE, NET_DNS_READ, NET_DNS_WRITE, NET_DNS_ERROR } net_dns_operation_t;

typedef struct {
    net_dns_operation_t operation;
    net_dns_slot_t slot;
} net_dns_plan_t;

net_dns_plan_t net_dns_plan(xiaotai_net_dns_fallback_t rule, bool has_address,
                            const net_dns_state_t slots[NET_DNS_COUNT]);

typedef struct {
    int type, static_count, dynamic_count, cache_count;
} net_wifi_tx_buffers_t;

net_wifi_tx_buffers_t net_wifi_tx_on_demand(net_wifi_tx_buffers_t buffers, bool enable);

/* All access to this state is serialized by the platform. The state translates
 * ESP notifications into requests without making a late event current again. */
typedef struct {
    uint32_t join, scan;
    uint32_t ipv4;
    uint8_t ipv6[16];
    bool associated, leaving, ipv6_valid, scan_canceling, scan_result;
    bool signal_valid;
    int8_t signal_dbm;
} net_wifi_flow_t;

bool net_wifi_flow_joined(net_wifi_flow_t *flow, const uint8_t *ssid, size_t length,
                          xiaotai_net_driver_event_t *event);
bool net_wifi_flow_address(net_wifi_flow_t *flow, uint32_t ipv4, const uint8_t *ipv6,
                           xiaotai_net_driver_event_t *event);
bool net_wifi_flow_lost(net_wifi_flow_t *flow, xiaotai_net_loss_t loss, int code,
                        xiaotai_net_driver_event_t *event);
bool net_wifi_flow_scan_done(net_wifi_flow_t *flow, xiaotai_err_t error, xiaotai_net_driver_event_t *event);
