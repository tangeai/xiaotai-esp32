// SPDX-License-Identifier: MIT
#include "wifi_policy.h"
#include "net_platform.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include <stdio.h>
#include <string.h>

ESP_EVENT_DEFINE_BASE(XIAOTAI_WIFI_FENCE);

typedef enum {ADDRESS_TRACKING, ADDRESS_LOST, ADDRESS_LOST_WHILE_LEAVING} address_state_t;
typedef enum {FENCE_NONE, FENCE_NEEDS_POST, FENCE_POSTED} fence_state_t;

typedef struct {
    net_wifi_flow_t flow;
    xiaotai_critical_t lock;
    xiaotai_signal_t sta_changed, ap_changed;
    esp_netif_t *sta, *ap;
    esp_event_handler_instance_t wifi_events, ip_events, local_events;
    xiaotai_net_driver_start_t start;
    bool accepting, ready, sta_up, ap_up;
    address_state_t addresses;
    fence_state_t fence;
    uint32_t gateway_ipv4;
    uint32_t dns_errors;
    esp_err_t last_dns_error;
} wifi_t;

static wifi_t s_wifi;

static void lock(wifi_t *w) { xiaotai_critical_enter(&w->lock); }
static void unlock(wifi_t *w) { xiaotai_critical_exit(&w->lock); }
static void publish(wifi_t *w, const xiaotai_net_driver_event_t *e)
{
    w->start.emit(w->start.emit_ctx, e);
}

static void forget(void *buffer, size_t bytes)
{
    volatile uint8_t *byte = buffer;
    while (bytes--) *byte++ = 0;
}

static xiaotai_net_loss_t loss_kind(int code)
{
    switch (code) {
    case WIFI_REASON_NO_AP_FOUND: return XIAOTAI_NET_LOSS_NOT_FOUND;
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return XIAOTAI_NET_LOSS_SECURITY;
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return XIAOTAI_NET_LOSS_CREDENTIALS;
    case WIFI_REASON_BEACON_TIMEOUT: return XIAOTAI_NET_LOSS_SIGNAL;
    default: return XIAOTAI_NET_LOSS_UNKNOWN;
    }
}

static esp_err_t fallback_dns(wifi_t *w)
{
    static const esp_netif_dns_type_t types[] = {ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK};
    net_dns_state_t slots[NET_DNS_COUNT] = {0};
    const char *address = w->start.fallback_dns;
    bool enabled = address && *address;
    for (;;) {
        net_dns_plan_t plan = net_dns_plan(w->start.fallback_dns_rule, enabled, slots);
        esp_netif_dns_info_t dns = {0};
        switch (plan.operation) {
        case NET_DNS_DONE: return ESP_OK;
        case NET_DNS_ERROR: return ESP_ERR_INVALID_ARG;
        case NET_DNS_READ: {
            const esp_err_t err = esp_netif_get_dns_info(w->sta, types[plan.slot], &dns);
            if (err != ESP_OK) return err;
            slots[plan.slot] = ESP_IP_IS_ANY(dns.ip) ? NET_DNS_EMPTY : NET_DNS_PRESENT;
            break;
        }
        case NET_DNS_WRITE:
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            if (esp_netif_str_to_ip4(address, &dns.ip.u_addr.ip4) != ESP_OK || !dns.ip.u_addr.ip4.addr)
                return ESP_ERR_INVALID_ARG;
            return esp_netif_set_dns_info(w->sta, ESP_NETIF_DNS_FALLBACK, &dns);
        }
    }
}

static void address(wifi_t *w, uint32_t ipv4, uint32_t gateway, const uint8_t *ipv6)
{
    lock(w);
    const uint32_t token = w->flow.associated && !w->flow.leaving && w->addresses == ADDRESS_TRACKING
        ? w->flow.join : 0;
    const bool set_dns = ipv4 || w->flow.ipv4 == 0;
    unlock(w);
    if (!token) return;
    const esp_err_t result = set_dns ? fallback_dns(w) : ESP_OK;
    xiaotai_net_driver_event_t event;
    lock(w);
    if (result != ESP_OK) {
        ++w->dns_errors;
        w->last_dns_error = result;
    }
    bool emit = w->flow.join == token && w->addresses == ADDRESS_TRACKING &&
                net_wifi_flow_address(&w->flow, ipv4, ipv6, &event);
    if (emit) {
        if (ipv4) w->gateway_ipv4 = gateway;
        event.gateway_ipv4 = w->flow.ipv4 ? w->gateway_ipv4 : 0;
    }
    unlock(w);
    if (emit) publish(w, &event);
}

static void lost(wifi_t *w, int code, int8_t rssi, uint32_t required_token)
{
    xiaotai_net_driver_event_t event;
    lock(w);
    const bool matches = !required_token || w->flow.join == required_token;
    bool emit = matches && net_wifi_flow_lost(&w->flow, loss_kind(code), code, &event);
    if (emit && w->addresses == ADDRESS_LOST) event.loss = XIAOTAI_NET_LOSS_ADDRESS;
    if (emit && rssi < 0) {
        event.signal_valid = true;
        event.signal_dbm = rssi;
    }
    if (matches) {
        w->addresses = ADDRESS_TRACKING;
        w->fence = FENCE_NONE;
        w->gateway_ipv4 = 0;
    }
    unlock(w);
    if (emit) publish(w, &event);
}

static bool current_ip_info(esp_netif_t *sta, esp_netif_ip_info_t *current)
{
    /* get_ip_info() can return a cached address while the interface is down. */
    return esp_netif_is_netif_up(sta) && esp_netif_get_ip_info(sta, current) == ESP_OK;
}

static uint32_t current_ipv4(esp_netif_t *sta)
{
    esp_netif_ip_info_t current;
    return current_ip_info(sta, &current) ? current.ip.addr : 0;
}

#if CONFIG_LWIP_IPV6
static bool current_global_ipv6(esp_netif_t *sta, esp_ip6_addr_t *ip, bool allow_other)
{
    /* IDF 5 uses a non-const argument for classification. */
    const bool preferred_global = esp_netif_ip6_get_addr_type(ip) == ESP_IP6_ADDR_IS_GLOBAL;
    if (!allow_other && !preferred_global) return false;
    /* The IDF list is empty on a down interface and excludes invalid addresses.
     * Use the configured slot count: more than one global address can be valid. */
    esp_ip6_addr_t current[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    const int count = esp_netif_get_all_ip6(sta, current);
    int other = -1;
    for (int i = 0; i < count; ++i) {
        if (preferred_global && !memcmp(current[i].addr, ip->addr, sizeof(ip->addr))) return true;
        if (allow_other && other < 0 && esp_netif_ip6_get_addr_type(&current[i]) == ESP_IP6_ADDR_IS_GLOBAL)
            other = i;
    }
    if (other < 0) return false;
    *ip = current[other];
    return true;
}
#endif

static void ipv4_lost(wifi_t *w)
{
    if (current_ipv4(w->sta)) return; /* The address was restored after this event was posted. */
    xiaotai_net_driver_event_t event = {.kind = XIAOTAI_NET_DRIVER_ADDRESS};
    lock(w);
    if (w->flow.associated && w->flow.ipv4) event.token = w->flow.join;
#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t ipv6 = {0};
    if (w->flow.ipv6_valid) memcpy(ipv6.addr, w->flow.ipv6, sizeof(ipv6.addr));
#endif
    unlock(w);
    if (!event.token) return;

#if CONFIG_LWIP_IPV6
    /* Only the live STA's global address can keep the link up without IPv4. */
    event.ipv6_valid = current_global_ipv6(w->sta, &ipv6, true);
    if (event.ipv6_valid) memcpy(event.ipv6, ipv6.addr, sizeof(event.ipv6));
    if (current_ipv4(w->sta)) return;
#endif
    lock(w);
    const bool changed = event.token == w->flow.join && w->flow.associated && w->flow.ipv4;
    if (changed) {
        w->flow.ipv4 = 0;
        w->gateway_ipv4 = 0;
        w->flow.ipv6_valid = event.ipv6_valid;
        memcpy(w->flow.ipv6, event.ipv6, sizeof(w->flow.ipv6));
        if (!event.ipv6_valid) w->addresses = w->flow.leaving ? ADDRESS_LOST_WHILE_LEAVING : ADDRESS_LOST;
    }
    unlock(w);
    /* Losing an address is observable immediately; only actual disconnect or
     * the event-loop fence can end the association. Leave/retry belongs to the worker. */
    if (changed) publish(w, &event);
}

static void event_received(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_t *w = arg;
    lock(w);
    bool accepting = w->accepting;
    unlock(w);
    if (!accepting) return;
    if (base == XIAOTAI_WIFI_FENCE && data) {
        lost(w, 0, 0, *(uint32_t *)data);
        return;
    }
    if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP && data) {
            const ip_event_got_ip_t *ip = data;
            esp_netif_ip_info_t current;
            /* ESP IP events carry no join generation. Even after association,
             * a queued event cannot advertise an address absent from the live STA. */
            if (ip->esp_netif == w->sta && ip->ip_info.ip.addr && current_ip_info(w->sta, &current) &&
                ip->ip_info.ip.addr == current.ip.addr)
                address(w, current.ip.addr, current.gw.addr, NULL);
        }
#if CONFIG_LWIP_IPV6
        else if (id == IP_EVENT_GOT_IP6 && data) {
            const ip_event_got_ip6_t *ip = data;
            esp_ip6_addr_t candidate = ip->ip6_info.ip;
            if (ip->esp_netif == w->sta && current_global_ipv6(w->sta, &candidate, false))
                address(w, 0, 0, (const uint8_t *)candidate.addr);
        }
#endif
        else if (id == IP_EVENT_STA_LOST_IP) {
            const ip_event_got_ip_t *ip = data;
            if (!ip || ip->esp_netif == w->sta) ipv4_lost(w);
        }
        return;
    }
    if (base != WIFI_EVENT) return;
    xiaotai_net_driver_event_t event;
    bool emit = false;
    switch (id) {
    case WIFI_EVENT_STA_START:
    case WIFI_EVENT_STA_STOP: {
        const bool up = id == WIFI_EVENT_STA_START;
        xiaotai_net_driver_event_t scan_event;
        bool scan_done = false;
        lock(w);
        const bool changed = w->sta_up != up;
        w->sta_up = up;
        const bool ready = w->ready;
        if (!up) {
            scan_done = net_wifi_flow_scan_done(&w->flow, XIAOTAI_ERR_INVALID_STATE, &scan_event);
            w->flow.scan_result = false;
        }
        unlock(w);
        xiaotai_signal_give(&w->sta_changed);
        if (ready && changed) {
            event = (xiaotai_net_driver_event_t){.kind = up ? XIAOTAI_NET_DRIVER_STARTED : XIAOTAI_NET_DRIVER_STOPPED};
            /* The engine stops retrying before it handles the terminal request events. */
            publish(w, &event);
            if (!up) lost(w, 0, 0, 0);
            if (scan_done) publish(w, &scan_event);
        }
        return;
    }
    case WIFI_EVENT_AP_START:
    case WIFI_EVENT_AP_STOP:
        lock(w);
        w->ap_up = id == WIFI_EVENT_AP_START;
        unlock(w);
        xiaotai_signal_give(&w->ap_changed);
        return;
    case WIFI_EVENT_STA_CONNECTED:
        if (data) {
            const wifi_event_sta_connected_t *joined = data;
            lock(w);
            emit = net_wifi_flow_joined(&w->flow, joined->ssid, joined->ssid_len, &event);
            unlock(w);
            if (emit) {
                event.channel = joined->channel;
                publish(w, &event);
#if CONFIG_LWIP_IPV6
                (void)esp_netif_create_ip6_linklocal(w->sta);
#endif
            }
        }
        return;
    case WIFI_EVENT_STA_DISCONNECTED: {
        /* IDF does not give this event a join token, and its default handler
         * may already have brought the STA netif down. AP readback cannot
         * establish that it is safe to ignore the disconnect. */
        const wifi_event_sta_disconnected_t *reason = data;
        lost(w, reason ? reason->reason : 0, reason ? reason->rssi : 0, 0);
        return;
    }
    case WIFI_EVENT_SCAN_DONE: {
        const wifi_event_sta_scan_done_t *scan = data;
        const esp_err_t err = scan && scan->status == 0 ? ESP_OK : ESP_FAIL;
        lock(w);
        if (w->flow.scan_canceling || (w->flow.scan && err != ESP_OK)) {
            unlock(w);
            /* Keep the scan reserved until its result storage has been freed. */
            (void)esp_wifi_clear_ap_list();
            lock(w);
        }
        emit = net_wifi_flow_scan_done(&w->flow, err, &event);
        unlock(w);
        if (emit) publish(w, &event);
        return;
    }
    default: return;
    }
}

static bool wait_interface(wifi_t *w, bool ap, bool up)
{
    const int64_t end = xiaotai_time_us() + (int64_t)XIAOTAI_NET_DRIVER_CALL_MAX_MS * 1000;
    do {
        lock(w);
        const bool current = ap ? w->ap_up : w->sta_up;
        unlock(w);
        if (current == up) return true;
        const int64_t left = end - xiaotai_time_us();
        if (left <= 0) return false;
        (void)xiaotai_signal_take(ap ? &w->ap_changed : &w->sta_changed, (uint32_t)((left + 999) / 1000));
    } while (true);
}

static esp_err_t create_interface(esp_netif_t **out, bool ap)
{
    /* The create_default helpers assert and abort on OOM/attach failure.
     * Build the same default netifs using their recoverable primitives. */
    esp_netif_config_t sta_cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    esp_netif_config_t ap_cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    *out = esp_netif_new(ap ? &ap_cfg : &sta_cfg);
    if (!*out) return ESP_ERR_NO_MEM;
    esp_err_t err = ap ? esp_netif_attach_wifi_ap(*out) : esp_netif_attach_wifi_station(*out);
    if (err != ESP_OK) return err;
    return ap ? esp_wifi_set_default_wifi_ap_handlers() : esp_wifi_set_default_wifi_sta_handlers();
}

static esp_err_t start(void *self, const xiaotai_network_t *part, const xiaotai_net_driver_start_t *config)
{
    wifi_t *w = self;
    if (!part || !config || !config->emit) return XIAOTAI_ERR_INVALID_ARG;
    if (w->ready) return XIAOTAI_ERR_INVALID_STATE;
    /* Even with Wi-Fi's own NVS disabled, radio startup can load/store PHY calibration. */
    esp_err_t err = xiaotai_require_internal_stack("wifi.start");
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if ((err = xiaotai_signal_init(&w->sta_changed)) != XIAOTAI_OK) return err;
    if ((err = xiaotai_signal_init(&w->ap_changed)) != XIAOTAI_OK) {
        xiaotai_signal_deinit(&w->sta_changed);
        return err;
    }
    w->start = *config;
    bool initialized = false, start_attempted = false;
    if ((err = create_interface(&w->sta, false)) != ESP_OK) goto rollback;
    if ((err = create_interface(&w->ap, true)) != ESP_OK) goto rollback;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (!config->driver_storage) init.nvs_enable = 0;
    const xiaotai_network_wifi_t *facts = part->driver_config;
    const net_wifi_tx_buffers_t tx = net_wifi_tx_on_demand(
        (net_wifi_tx_buffers_t){init.tx_buf_type, init.static_tx_buf_num, init.dynamic_tx_buf_num, init.cache_tx_buf_num},
        part->desc.kind == XIAOTAI_NETWORK_WIFI_COPROCESSOR && facts && facts->tx_buffers_on_demand);
    init.tx_buf_type = tx.type;
    init.static_tx_buf_num = tx.static_count;
    init.dynamic_tx_buf_num = tx.dynamic_count;
    init.cache_tx_buf_num = tx.cache_count;
    if ((err = esp_wifi_init(&init)) != ESP_OK) goto rollback;
    initialized = true;
    if (!config->driver_storage && (err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) goto rollback;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) goto rollback;
    /* Read the supported default protocol from the driver's own radio, including
     * 11ax when the local/remote driver enables it; never infer it from host chip. */
    uint8_t protocols = 0;
    if ((err = esp_wifi_get_protocol(WIFI_IF_STA, &protocols)) != ESP_OK) goto rollback;
    if ((err = esp_wifi_set_protocol(WIFI_IF_STA, protocols)) != ESP_OK) goto rollback;
    if ((err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20)) != ESP_OK) goto rollback;
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) goto rollback;
    if ((err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_received,
                                                  w, &w->wifi_events)) != ESP_OK) goto rollback;
    if ((err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, event_received,
                                                  w, &w->ip_events)) != ESP_OK) goto rollback;
    if ((err = esp_event_handler_instance_register(XIAOTAI_WIFI_FENCE, 0, event_received,
                                                  w, &w->local_events)) != ESP_OK) goto rollback;
    lock(w);
    w->accepting = true;
    unlock(w);
    start_attempted = true;
    if ((err = esp_wifi_start()) != ESP_OK) goto rollback;
    if (!wait_interface(w, false, true)) {
        err = ESP_ERR_TIMEOUT;
        goto rollback;
    }
    wifi_ps_type_t power = WIFI_PS_MIN_MODEM;
    if ((err = esp_wifi_get_ps(&power)) != ESP_OK) goto rollback;
    if (power != WIFI_PS_NONE) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto rollback;
    }
    lock(w);
    w->ready = w->sta_up;
    unlock(w);
    if (w->ready) return ESP_OK;
    err = ESP_ERR_WIFI_NOT_STARTED;

rollback:
    lock(w);
    w->accepting = w->sta_up = w->ap_up = false;
    unlock(w);
    if (w->local_events) (void)esp_event_handler_instance_unregister(XIAOTAI_WIFI_FENCE, 0, w->local_events);
    if (w->ip_events) (void)esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, w->ip_events);
    if (w->wifi_events) (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, w->wifi_events);
    w->local_events = w->ip_events = w->wifi_events = NULL;
    if (start_attempted) (void)esp_wifi_stop();
    if (initialized) (void)esp_wifi_deinit();
    if (w->ap) esp_netif_destroy_default_wifi(w->ap);
    if (w->sta) esp_netif_destroy_default_wifi(w->sta);
    w->ap = w->sta = NULL;
    xiaotai_signal_deinit(&w->ap_changed);
    xiaotai_signal_deinit(&w->sta_changed);
    return err;
}

static esp_err_t join(void *self, uint32_t token, const xiaotai_net_credentials_t *creds, uint32_t flags)
{
    wifi_t *w = self;
    if (!creds || !token) return ESP_ERR_INVALID_ARG;
    wifi_config_t config = {0};
    size_t ssid = strnlen(creds->ssid, sizeof(creds->ssid));
    size_t password = strnlen(creds->password, sizeof(creds->password));
    if (!ssid || ssid > sizeof(config.sta.ssid) || password > sizeof(config.sta.password))
        return ESP_ERR_INVALID_ARG;
    lock(w);
    const bool allowed = w->ready && w->sta_up && !w->flow.join && !w->flow.scan;
    unlock(w);
    if (!allowed) return ESP_ERR_INVALID_STATE;
    esp_err_t err = xiaotai_require_internal_stack("wifi.join");
    if (err != ESP_OK) return err;
    memcpy(config.sta.ssid, creds->ssid, ssid);
    memcpy(config.sta.password, creds->password, password);
    config.sta.threshold.authmode = password && (flags & XIAOTAI_NET_JOIN_REQUIRE_WPA2)
        ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    forget(&config, sizeof(config));
    if (err != ESP_OK) return err;
    lock(w);
    const bool still_up = w->sta_up;
    if (still_up) w->flow.join = token;
    unlock(w);
    if (!still_up) return ESP_ERR_WIFI_NOT_STARTED;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        lock(w);
        if (w->flow.join == token) w->flow.join = 0;
        unlock(w);
    }
    return err;
}

static bool join_radio_unavailable(xiaotai_err_t err)
{
    return err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_STOP_STATE;
}

static esp_err_t leave(void *self)
{
    wifi_t *w = self;
    lock(w);
    const uint32_t token = w->flow.join;
    const fence_state_t fence = w->fence;
    const bool was_leaving = w->flow.leaving;
    if (token) w->flow.leaving = true;
    unlock(w);
    if (!token) return ESP_ERR_INVALID_STATE;
    if (fence == FENCE_POSTED) return ESP_OK;

    esp_err_t err = fence == FENCE_NONE ? esp_wifi_disconnect() : ESP_OK;
    if (fence == FENCE_NEEDS_POST || err == ESP_ERR_WIFI_NOT_CONNECT || err == ESP_ERR_WIFI_NOT_STARTED) {
        /* The default loop processes events already queued for this STA before
         * the marker. Retain proof of disassociation if posting fails. */
        err = esp_event_post(XIAOTAI_WIFI_FENCE, 0, &token, sizeof(token), 0);
        lock(w);
        if (w->flow.join == token) w->fence = err == ESP_OK ? FENCE_POSTED : FENCE_NEEDS_POST;
        unlock(w);
    } else if (err != ESP_OK) {
        lock(w);
        if (w->flow.join == token) {
            w->flow.leaving = was_leaving;
            if (!was_leaving && w->addresses == ADDRESS_LOST_WHILE_LEAVING) w->addresses = ADDRESS_LOST;
        }
        unlock(w);
    }
    /* INVALID_STATE belongs to the no-token case, not to a platform failure. */
    return err == ESP_ERR_INVALID_STATE ? ESP_FAIL : err;
}

static esp_err_t scan(void *self, uint32_t token)
{
    wifi_t *w = self;
    if (!token) return ESP_ERR_INVALID_ARG;
    lock(w);
    const bool allowed = w->ready && w->sta_up && (!w->flow.join || w->flow.associated) &&
                         !w->flow.leaving && !w->flow.scan && !w->flow.scan_canceling && !w->flow.scan_result;
    if (allowed) w->flow.scan = token;
    unlock(w);
    if (!allowed) return XIAOTAI_ERR_BUSY;
    const wifi_scan_config_t config = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                                       .scan_time.active = {.min = 0, .max = 120}};
    esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err != ESP_OK) {
        lock(w);
        if (w->flow.scan == token) w->flow.scan = 0;
        unlock(w);
    }
    return err;
}

static esp_err_t scan_cancel(void *self)
{
    wifi_t *w = self;
    lock(w);
    const uint32_t token = w->flow.scan;
    const bool had_result = w->flow.scan_result;
    w->flow.scan_result = false;
    if (token) {
        w->flow.scan = 0;
        w->flow.scan_canceling = true;
    }
    unlock(w);
    if (!token) return had_result ? esp_wifi_clear_ap_list() : ESP_OK;
    esp_err_t err = esp_wifi_scan_stop();
    if (err != ESP_OK) {
        lock(w);
        if (w->flow.scan_canceling) {
            w->flow.scan_canceling = false;
            w->flow.scan = token;
        }
        unlock(w);
    }
    return err;
}

static xiaotai_net_security_t security(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return XIAOTAI_NET_SECURITY_OPEN;
    case WIFI_AUTH_WEP: return XIAOTAI_NET_SECURITY_WEP;
    case WIFI_AUTH_WPA_PSK: return XIAOTAI_NET_SECURITY_WPA;
    case WIFI_AUTH_WPA2_PSK: return XIAOTAI_NET_SECURITY_WPA2;
    case WIFI_AUTH_WPA_WPA2_PSK: return XIAOTAI_NET_SECURITY_WPA_WPA2;
    case WIFI_AUTH_WPA3_PSK: return XIAOTAI_NET_SECURITY_WPA3;
    case WIFI_AUTH_WPA2_WPA3_PSK: return XIAOTAI_NET_SECURITY_WPA2_WPA3;
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return XIAOTAI_NET_SECURITY_ENTERPRISE;
    default: return XIAOTAI_NET_SECURITY_OTHER;
    }
}

static esp_err_t scan_read(void *self, xiaotai_net_scan_record_t *records, size_t capacity,
                           size_t *count, bool *truncated)
{
    wifi_t *w = self;
    if ((!records && capacity) || !count || !truncated) return ESP_ERR_INVALID_ARG;
    *count = 0;
    *truncated = false;
    lock(w);
    const bool result = w->flow.scan_result;
    w->flow.scan_result = false;
    unlock(w);
    if (!result) return ESP_ERR_INVALID_STATE;
    uint16_t found = 0;
    esp_err_t err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK || !found || !capacity) {
        *truncated = err == ESP_OK && found && !capacity;
        esp_err_t release = esp_wifi_clear_ap_list();
        return err == ESP_OK ? release : err;
    }
    uint16_t size = found < capacity ? found : (uint16_t)capacity;
    wifi_ap_record_t *list = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, size, sizeof(*list));
    if (!list) {
        (void)esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&size, list);
    if (err != ESP_OK) (void)esp_wifi_clear_ap_list();
    else {
        *truncated = size < found;
        for (uint16_t i = 0; i < size; ++i) {
            if (!list[i].ssid[0]) continue;
            xiaotai_net_scan_record_t *out = &records[(*count)++];
            *out = (xiaotai_net_scan_record_t){.signal_dbm = list[i].rssi, .channel = list[i].primary,
                                               .security = security(list[i].authmode)};
            memcpy(out->ssid, list[i].ssid, strnlen((const char *)list[i].ssid, XIAOTAI_NET_SSID_MAX));
        }
    }
    xiaotai_mem_free(list);
    return err;
}

static esp_err_t ap_stop(void *self)
{
    wifi_t *w = self;
    if (!w->ap) return ESP_ERR_INVALID_STATE;
    esp_err_t err = xiaotai_require_internal_stack("wifi.ap_stop");
    if (err != ESP_OK) return err;
    wifi_mode_t mode;
    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return err;
    if (mode == WIFI_MODE_APSTA) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) return err;
    }
    /* The mode may already have changed on a failed stop. A retry still
     * needs to observe AP_STOP and finish shutting down the DHCP server. */
    if (!wait_interface(w, true, false)) return ESP_ERR_TIMEOUT;
    err = esp_netif_dhcps_stop(w->ap);
    return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED ? ESP_OK : err;
}

static esp_err_t ap_start(void *self, const xiaotai_net_ap_config_t *request)
{
    wifi_t *w = self;
    if (!request || !request->ipv4 || !request->channel || !request->max_clients) return ESP_ERR_INVALID_ARG;
    if (!w->ready || !w->ap) return ESP_ERR_INVALID_STATE;
    wifi_config_t config = {0};
    const size_t ssid = strnlen(request->ssid, sizeof(request->ssid));
    const size_t password = strnlen(request->password, sizeof(request->password));
    if (!ssid || ssid > sizeof(config.ap.ssid) || password >= sizeof(config.ap.password) ||
        (password && password < 8)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = xiaotai_require_internal_stack("wifi.ap_start");
    if (err != ESP_OK) return err;
    wifi_mode_t mode;
    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return err;
    if (mode != WIFI_MODE_STA) return ESP_ERR_INVALID_STATE;
    err = esp_netif_dhcps_stop(w->ap);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ip.gw.addr = request->ipv4;
    esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
    if ((err = esp_netif_set_ip_info(w->ap, &ip)) != ESP_OK) return err;
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = request->ipv4;
    if ((err = esp_netif_set_dns_info(w->ap, ESP_NETIF_DNS_MAIN, &dns)) != ESP_OK) return err;
    uint8_t offer_dns = 1;
    if ((err = esp_netif_dhcps_option(w->ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                    &offer_dns, sizeof(offer_dns))) != ESP_OK) return err;
    /* Do not announce the HTTP setup page as the DHCP captive API. */
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        memcpy(config.ap.ssid, request->ssid, ssid);
        memcpy(config.ap.password, request->password, password);
        config.ap.ssid_len = (uint8_t)ssid;
        config.ap.channel = request->channel;
        config.ap.max_connection = request->max_clients;
        config.ap.authmode = password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
        forget(&config, sizeof(config));
    }
    if (err == ESP_OK && !wait_interface(w, true, true)) err = ESP_ERR_TIMEOUT;
    if (err == ESP_OK) {
        err = esp_netif_dhcps_start(w->ap);
        if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) err = ESP_OK;
    }
    if (err != ESP_OK) {
        /* A hosted mode change can take effect even when its reply fails. */
        const esp_err_t cleanup = ap_stop(w);
        if (cleanup != ESP_OK) return cleanup;
    }
    return err;
}

static esp_err_t station_signal(void *self, int8_t *dbm)
{
    wifi_t *w = self;
    if (!dbm) return ESP_ERR_INVALID_ARG;
    lock(w);
    const uint32_t valid = w->flow.associated ? w->flow.join : 0;
    unlock(w);
    if (!valid) return ESP_ERR_INVALID_STATE;
    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) return err;
    lock(w);
    const bool same = valid == w->flow.join;
    if (same) {
        w->flow.signal_valid = true;
        *dbm = w->flow.signal_dbm = ap.rssi;
    }
    unlock(w);
    return same ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t mac(void *self, uint8_t address_out[6])
{
    (void)self;
    return address_out ? esp_wifi_get_mac(WIFI_IF_STA, address_out) : ESP_ERR_INVALID_ARG;
}

static void report(void *self, const xiaotai_reply_t *out)
{
    wifi_t *w = self;
    if (!out || !out->write) return;
    wifi_ps_type_t ps;
    wifi_bandwidth_t bandwidth;
    uint8_t protocols = 0, channel = 0;
    wifi_second_chan_t secondary;
    int8_t dbm = 0;
    esp_err_t p = esp_wifi_get_ps(&ps);
    esp_err_t b = esp_wifi_get_bandwidth(WIFI_IF_STA, &bandwidth);
    esp_err_t m = esp_wifi_get_protocol(WIFI_IF_STA, &protocols);
    esp_err_t c = esp_wifi_get_channel(&channel, &secondary);
    esp_err_t r = station_signal(w, &dbm);
    lock(w);
    const uint32_t dns_errors = w->dns_errors;
    const int dns_error = w->last_dns_error;
    unlock(w);
    char line[200];
    (void)snprintf(line, sizeof(line), "wifi ps=%d bandwidth=%d protocol=%d channel=%d rssi=%d "
                   "errors=%d/%d/%d/%d/%d dns_errors=%lu dns_last=%d",
                   p == ESP_OK ? (int)ps : -1, b == ESP_OK ? (int)bandwidth : -1,
                   m == ESP_OK ? (int)protocols : -1, c == ESP_OK ? (int)channel : -1,
                   r == ESP_OK ? (int)dbm : 0, p, b, m, c, r, (unsigned long)dns_errors, dns_error);
    out->write(out->ctx, line);
}

#define WIFI_DRIVER(kind_, name_) {                                           \
    .kind = (kind_), .name = (name_),                                           \
    .capabilities = XIAOTAI_NET_DRIVER_CREDENTIALS | XIAOTAI_NET_DRIVER_SCAN |    \
                    XIAOTAI_NET_DRIVER_ACCESS_POINT | XIAOTAI_NET_DRIVER_SIGNAL,  \
    .start = start, .join = join, .join_radio_unavailable = join_radio_unavailable, \
    .leave = leave, .scan = scan,                                              \
    .scan_cancel = scan_cancel, .scan_read = scan_read,                         \
    .ap_start = ap_start, .ap_stop = ap_stop, .signal = station_signal,         \
    .mac = mac, .report = report, .self = &s_wifi,                             \
}

static const xiaotai_net_driver_t s_native = WIFI_DRIVER(XIAOTAI_NETWORK_WIFI, "wifi");
static const xiaotai_net_driver_t s_remote = WIFI_DRIVER(XIAOTAI_NETWORK_WIFI_COPROCESSOR, "wifi_remote");

void net_platform_register_drivers(void)
{
    xiaotai_critical_init(&s_wifi.lock);
    (void)xiaotai_net_driver_register(&s_native);
    (void)xiaotai_net_driver_register(&s_remote);
}
