// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_captive_portal_contract.py
// Replaces legacy: tests/regression/test_station_identity.py
// Compile the production adapter against a deterministic ESP event and driver fixture.
#include "esp_netif.h"
#include "esp_wifi.h"
#include "net_platform.h"
#include "wifi_policy.h"
#include "xiaotai/port/os.h"
#include <assert.h>
#include <string.h>

extern const xiaotai_net_driver_t *net_driver_for(xiaotai_network_kind_t kind);
extern esp_event_base_t XIAOTAI_WIFI_FENCE;
esp_event_base_t WIFI_EVENT = "test_wifi", IP_EVENT = "test_ip";
struct esp_netif {esp_netif_ip_info_t ip; bool live, dhcp;};
static esp_netif_t sta, ap;
static esp_ip6_addr_t sta_ipv6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
static int sta_ipv6_count;
static struct {esp_event_base_t base; esp_event_handler_t handler; void *arg;} handlers[3];
static wifi_init_config_t initialized;
static wifi_mode_t mode;
static wifi_config_t station_cfg, ap_cfg;
static wifi_ps_type_t power_save;
static wifi_bandwidth_t width;
static uint8_t protocol = 0x17;
static uint32_t dns_values[3], pending_fence;
static unsigned dns_reads[3], dns_writes, storage, cleans, deleted, connects, stopped;
static unsigned disconnects, fence_posts;
static esp_err_t disconnect_result = ESP_ERR_WIFI_NOT_CONNECT, fence_result = ESP_OK;
static esp_err_t connect_result = ESP_OK, station_config_result = ESP_OK;
static esp_err_t dns_write_result = ESP_OK;
static int failed_dns = -1;
static bool radio, fail_alloc, fail_bandwidth, fail_radio_start, fail_ap_config, handed_dns, sta_netif_up;
static bool fail_ap_mode_after_change, fail_dhcp_stop;

static void fire(esp_event_base_t base, int32_t id, void *data)
{
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i)
        if (handlers[i].base == base) handlers[i].handler(handlers[i].arg, base, id, data);
}

esp_err_t esp_event_loop_create_default(void) {return ESP_OK;}
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id, esp_event_handler_t fn,
                                             void *ctx, esp_event_handler_instance_t *handle)
{
    (void)id;
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        if (handlers[i].base) continue;
        handlers[i].base = base; handlers[i].handler = fn; handlers[i].arg = ctx;
        *handle = &handlers[i];
        return ESP_OK;
    }
    return ESP_FAIL;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id, esp_event_handler_instance_t handle)
{
    (void)id;
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        if (&handlers[i] == handle && handlers[i].base == base) {
            handlers[i].base = NULL;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data, size_t size, uint32_t wait)
{
    assert(base == XIAOTAI_WIFI_FENCE && id == 0 && size == sizeof(pending_fence) && wait == 0);
    ++fence_posts;
    if (fence_result != ESP_OK) return fence_result;
    memcpy(&pending_fence, data, size);
    return ESP_OK;
}
esp_err_t esp_netif_init(void) {return ESP_OK;}
esp_netif_t *esp_netif_new(const esp_netif_config_t *cfg)
{
    if (cfg->ap && fail_alloc) return NULL;
    esp_netif_t *out = cfg->ap ? &ap : &sta;
    out->live = true;
    return out;
}
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *n) {assert(n == &sta && n->live); return ESP_OK;}
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *n) {assert(n == &ap && n->live); return ESP_OK;}
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void) {return ESP_OK;}
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void) {return ESP_OK;}
void esp_netif_destroy_default_wifi(void *ptr) {((esp_netif_t *)ptr)->live = false; ++deleted;}
esp_err_t esp_netif_get_dns_info(esp_netif_t *n, esp_netif_dns_type_t slot, esp_netif_dns_info_t *info)
{
    assert(n == &sta); ++dns_reads[slot];
    info->ip.u_addr.ip4.addr = dns_values[slot];
    return (int)slot == failed_dns ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_netif_set_dns_info(esp_netif_t *n, esp_netif_dns_type_t slot, esp_netif_dns_info_t *info)
{
    if (n == &ap) {
        assert(slot == ESP_NETIF_DNS_MAIN && info->ip.u_addr.ip4.addr == ap.ip.ip.addr);
    } else {
        assert(n == &sta && slot == ESP_NETIF_DNS_FALLBACK && !dns_values[slot]);
        ++dns_writes;
        if (dns_write_result != ESP_OK) return dns_write_result;
        dns_values[slot] = info->ip.u_addr.ip4.addr;
    }
    return ESP_OK;
}
esp_err_t esp_netif_str_to_ip4(const char *s, esp_ip4_addr_t *out)
{assert(!strcmp(s, "223.5.5.5")); out->addr = 0x050505df; return ESP_OK;}
void esp_netif_set_ip4_addr(esp_ip4_addr_t *out, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{out->addr = (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);}
esp_err_t esp_netif_set_ip_info(esp_netif_t *n, const esp_netif_ip_info_t *ip)
{assert(n == &ap && !n->dhcp); n->ip = *ip; return ESP_OK;}
bool esp_netif_is_netif_up(esp_netif_t *n) {assert(n == &sta); return sta_netif_up;}
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip) {*ip = n->ip; return ESP_OK;}
esp_err_t esp_netif_dhcps_stop(esp_netif_t *n)
{
    assert(n == &ap);
    if (!n->dhcp) return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED;
    if (fail_dhcp_stop) { fail_dhcp_stop = false; return ESP_FAIL; }
    n->dhcp = false;
    return ESP_OK;
}
esp_err_t esp_netif_dhcps_start(esp_netif_t *n)
{assert(n == &ap && mode == WIFI_MODE_APSTA && handed_dns); n->dhcp = true; return ESP_OK;}
esp_err_t esp_netif_dhcps_option(esp_netif_t *n, int op, int opt, void *value, uint32_t size)
{
    assert(n == &ap && !n->dhcp && op == ESP_NETIF_OP_SET && opt == ESP_NETIF_DOMAIN_NAME_SERVER);
    assert(size == 1 && *(uint8_t *)value == 1);
    handed_dns = true;
    return ESP_OK;
}
esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t *n) {assert(n == &sta); return ESP_OK;}
int esp_netif_get_all_ip6(esp_netif_t *n, esp_ip6_addr_t *out)
{
    assert(n == &sta && (size_t)sta_ipv6_count <= sizeof(sta_ipv6) / sizeof(sta_ipv6[0]));
    if (!sta_netif_up) return 0;
    memcpy(out, sta_ipv6, (size_t)sta_ipv6_count * sizeof(*out));
    return sta_ipv6_count;
}
int esp_netif_ip6_get_addr_type(esp_ip6_addr_t *addr)
{return (addr->addr[0] & 0xe0) == 0x20 ? ESP_IP6_ADDR_IS_GLOBAL : 0;}
esp_err_t esp_wifi_init(const wifi_init_config_t *init) {initialized = *init; return ESP_OK;}
esp_err_t esp_wifi_start(void)
{radio = true; fire(WIFI_EVENT, WIFI_EVENT_STA_START, NULL); return fail_radio_start ? ESP_FAIL : ESP_OK;}
esp_err_t esp_wifi_stop(void) {radio = false; ++stopped; fire(WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL); return ESP_OK;}
esp_err_t esp_wifi_deinit(void) {return ESP_OK;}
esp_err_t esp_wifi_set_storage(int storage_mode) {assert(storage_mode == WIFI_STORAGE_RAM); ++storage; return ESP_OK;}
esp_err_t esp_wifi_set_mode(wifi_mode_t next)
{
    wifi_mode_t prev = mode; mode = next;
    if (radio && (prev == WIFI_MODE_APSTA) != (next == WIFI_MODE_APSTA))
        fire(WIFI_EVENT, next == WIFI_MODE_APSTA ? WIFI_EVENT_AP_START : WIFI_EVENT_AP_STOP, NULL);
    if (next == WIFI_MODE_APSTA && fail_ap_mode_after_change) {
        fail_ap_mode_after_change = false;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
esp_err_t esp_wifi_get_mode(wifi_mode_t *out) {*out = mode; return ESP_OK;}
esp_err_t esp_wifi_get_protocol(wifi_interface_t i, uint8_t *out) {assert(i == WIFI_IF_STA); *out = protocol; return ESP_OK;}
esp_err_t esp_wifi_set_protocol(wifi_interface_t i, uint8_t mask) {assert(i == WIFI_IF_STA && mask == protocol); return ESP_OK;}
esp_err_t esp_wifi_set_bandwidth(wifi_interface_t i, wifi_bandwidth_t b)
{assert(i == WIFI_IF_STA); width = b; return fail_bandwidth ? ESP_FAIL : ESP_OK;}
esp_err_t esp_wifi_get_bandwidth(wifi_interface_t i, wifi_bandwidth_t *out) {assert(i == WIFI_IF_STA); *out = width; return ESP_OK;}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t power) {power_save = power; return ESP_OK;}
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *out) {*out = power_save; return ESP_OK;}
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *config)
{
    if (i == WIFI_IF_STA) {
        if (station_config_result != ESP_OK) return station_config_result;
        station_cfg = *config;
    }
    else {assert(mode == WIFI_MODE_APSTA); if (fail_ap_config) return ESP_FAIL; ap_cfg = *config;}
    return ESP_OK;
}
esp_err_t esp_wifi_connect(void) {++connects; return connect_result;}
esp_err_t esp_wifi_disconnect(void) {++disconnects; return disconnect_result;}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *cfg, bool block)
{assert(!cfg->show_hidden && !block && cfg->scan_time.active.max == 120); return ESP_OK;}
esp_err_t esp_wifi_scan_stop(void) {return ESP_OK;}
esp_err_t esp_wifi_clear_ap_list(void) {++cleans; return ESP_OK;}
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *found) {*found = 2; return ESP_OK;}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *out)
{
    assert(*count == 1);
    out[0] = (wifi_ap_record_t){.ssid = "visible", .authmode = WIFI_AUTH_WPA2_WPA3_PSK, .primary = 7, .rssi = -60};
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) {out->rssi = -43; return ESP_OK;}
esp_err_t esp_wifi_get_mac(wifi_interface_t i, uint8_t *out) {assert(i == WIFI_IF_STA); memset(out, 0x41, 6); return ESP_OK;}
esp_err_t esp_wifi_get_channel(uint8_t *out, wifi_second_chan_t *secondary)
{*out = 3; *secondary = WIFI_SECOND_CHAN_NONE; return ESP_OK;}

#ifdef NW11_WORKER_TEST
#include "wifi_worker_fake.h"

void wifi_fixture_disconnect_result(esp_err_t result) {disconnect_result = result;}
void wifi_fixture_connect_result(esp_err_t result) {connect_result = result;}
void wifi_fixture_station_config_result(esp_err_t result) {station_config_result = result;}
void wifi_fixture_radio(bool up)
{
    radio = up;
    fire(WIFI_EVENT, up ? WIFI_EVENT_STA_START : WIFI_EVENT_STA_STOP, NULL);
}
unsigned wifi_fixture_joins(void) {return connects;}
unsigned wifi_fixture_leaves(void) {return disconnects;}

void wifi_fixture_associated(void)
{
    wifi_event_sta_connected_t event = {.ssid = "network", .ssid_len = 7};
    sta_netif_up = true;
    fire(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &event);
}

void wifi_fixture_ipv4(uint32_t ipv4)
{
    ip_event_got_ip_t event = {.esp_netif = &sta, .ip_info.ip.addr = ipv4};
    sta.ip.ip.addr = ipv4;
    fire(IP_EVENT, ipv4 ? IP_EVENT_STA_GOT_IP : IP_EVENT_STA_LOST_IP, &event);
}

void wifi_fixture_physically_disconnected(void)
{
    wifi_event_sta_disconnected_t event = {.reason = WIFI_REASON_BEACON_TIMEOUT, .rssi = -66};
    sta_netif_up = false;
    sta.ip.ip.addr = 0;
    fire(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
}
#else
static xiaotai_net_driver_event_t events[64];
static size_t event_count;

static void emitted(void *ctx, const xiaotai_net_driver_event_t *event)
{
    assert(ctx == &sta && event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = *event;
}

static void diag(void *ctx, const char *line)
{
    assert(strstr(line, (const char *)ctx));
    assert(!strstr(line, "private"));
}

bool __wrap_xiaotai_mem_has_external(void) { return true; }

typedef struct {
    const xiaotai_net_driver_t *driver;
    const xiaotai_network_t *part;
    const xiaotai_net_driver_start_t *start;
    const xiaotai_net_credentials_t *credentials;
    const xiaotai_net_ap_config_t *setup;
    bool stop_ap;
    xiaotai_signal_t complete;
} external_stack_case_t;

static void try_external_stack(void *arg)
{
    external_stack_case_t *tc = arg;
    const xiaotai_net_driver_t *d = tc->driver;
    if (tc->start) {
        assert(d->start(d->self, tc->part, tc->start) == XIAOTAI_ERR_INVALID_STATE);
    }
    if (tc->credentials) {
        assert(d->join(d->self, 7, tc->credentials, XIAOTAI_NET_JOIN_REQUIRE_WPA2) == XIAOTAI_ERR_INVALID_STATE);
    }
    if (tc->setup) {
        assert(d->ap_start(d->self, tc->setup) == XIAOTAI_ERR_INVALID_STATE);
    }
    if (tc->stop_ap) {
        assert(d->ap_stop(d->self) == XIAOTAI_ERR_INVALID_STATE);
    }
    xiaotai_signal_give(&tc->complete);
}

static void check_external_stack(external_stack_case_t *tc)
{
    const xiaotai_task_config_t config = {
        .name = "wifi_bulk_probe", .entry = try_external_stack, .arg = tc,
        .stack_bytes = 4096, .stack = XIAOTAI_MEM_BULK, .priority = 1, .core = XIAOTAI_CORE_ANY,
    };
    xiaotai_task_t task;
    assert(xiaotai_signal_init(&tc->complete) == XIAOTAI_OK);
    assert(xiaotai_task_create(&config, &task) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&tc->complete, 5000));
    xiaotai_signal_deinit(&tc->complete);
}

int main(void)
{
    net_platform_register_drivers();
    const xiaotai_net_driver_t *d = net_driver_for(XIAOTAI_NETWORK_WIFI_COPROCESSOR);
    assert(d && net_driver_for(XIAOTAI_NETWORK_WIFI)->join == d->join);
    const xiaotai_network_wifi_t config = {.tx_buffers_on_demand = true};
    const xiaotai_network_t part = {.desc.kind = XIAOTAI_NETWORK_WIFI_COPROCESSOR, .driver_config = &config};
    xiaotai_net_driver_start_t start = {.fallback_dns = "223.5.5.5", .emit = emitted, .emit_ctx = &sta,
                                       .fallback_dns_rule = XIAOTAI_NET_DNS_FALLBACK_ALL_EMPTY};
    check_external_stack(&(external_stack_case_t){.driver = d, .part = &part, .start = &start});
    assert(!sta.live && !ap.live && !radio && !initialized.nvs_enable);
    fail_alloc = true;
    assert(d->start(d->self, &part, &start) == ESP_ERR_NO_MEM && deleted == 1 && !sta.live && !ap.live);
    fail_alloc = false;
    fail_bandwidth = true;
    assert(d->start(d->self, &part, &start) == ESP_FAIL && deleted == 3 && !radio && !event_count);
    fail_bandwidth = false;
    start.driver_storage = true;
    fail_radio_start = true;
    assert(d->start(d->self, &part, &start) == ESP_FAIL && deleted == 5 && stopped == 1 && !radio && !event_count);
    fail_radio_start = false;
    assert(d->start(d->self, &part, &start) == ESP_OK && !event_count);
    assert(initialized.nvs_enable == 1 && storage == 1 && width == WIFI_BW20 && power_save == WIFI_PS_NONE);
    assert(initialized.tx_buf_type == 1 && !initialized.static_tx_buf_num &&
           initialized.dynamic_tx_buf_num == 7 && !initialized.cache_tx_buf_num);

    const xiaotai_net_credentials_t credentials = {.ssid = "network", .password = "private"};
    const xiaotai_net_ap_config_t setup = {.ssid = "setup", .channel = 1, .max_clients = 4, .ipv4 = 0x0106a8c0};
    check_external_stack(&(external_stack_case_t){.driver = d, .credentials = &credentials, .setup = &setup});
    assert(!connects && station_cfg.sta.ssid[0] == 0 && mode == WIFI_MODE_STA && !ap.dhcp);
    assert(d->join(d->self, 7, &credentials, XIAOTAI_NET_JOIN_REQUIRE_WPA2) == ESP_OK);
    assert(connects == 1 && station_cfg.sta.threshold.authmode == WIFI_AUTH_WPA2_PSK &&
           !memcmp(station_cfg.sta.password, "private", 7));
    ip_event_got_ip_t ip = {.esp_netif = &sta, .ip_info.ip.addr = 123, .ip_info.gw.addr = 42};
    sta_netif_up = true;
    sta.ip.ip.addr = ip.ip_info.ip.addr;
    sta.ip.gw.addr = 100;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(!event_count);
    wifi_event_sta_connected_t connected = {.ssid = "network", .ssid_len = 7, .channel = 11};
    fire(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &connected);
    failed_dns = ESP_NETIF_DNS_BACKUP;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(event_count == 2 && events[0].kind == XIAOTAI_NET_DRIVER_JOINED && events[0].channel == 11 &&
           events[1].kind == XIAOTAI_NET_DRIVER_ADDRESS && events[1].ipv4 == 123 &&
           events[1].gateway_ipv4 == 100 && events[1].token == 7);
    assert(dns_reads[0] == 1 && dns_reads[1] == 1 && !dns_reads[2] && !dns_writes);
    d->report(d->self, &(xiaotai_reply_t){.write = diag, .ctx = "dns_errors=1 dns_last=-1"});
    failed_dns = -1;
    dns_write_result = -22;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(dns_writes == 1 && !dns_values[2]);
    d->report(d->self, &(xiaotai_reply_t){.write = diag, .ctx = "dns_errors=2 dns_last=-22"});
    dns_write_result = ESP_OK;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(dns_writes == 2 && dns_values[2] == 0x050505df);
    ip_event_got_ip6_t v6 = {.esp_netif = &sta, .ip6_info.ip.addr = {0x20, 0, 0, 0}};
    sta_ipv6[0] = v6.ip6_info.ip;
    sta_ipv6_count = 1;
    fire(IP_EVENT, IP_EVENT_GOT_IP6, &v6);
    assert(events[event_count - 1].ipv6_valid && events[event_count - 1].ipv4 == 123 &&
           events[event_count - 1].gateway_ipv4 == 100);
    fail_ap_config = true;
    assert(d->ap_start(d->self, &setup) == ESP_FAIL && mode == WIFI_MODE_STA && !ap.dhcp);
    fail_ap_config = false;
    /* A timed-out hosted mode change may already have changed the radio. */
    fail_ap_mode_after_change = true;
    assert(d->ap_start(d->self, &setup) == ESP_ERR_TIMEOUT && mode == WIFI_MODE_STA && !ap.dhcp);
    assert(d->ap_start(d->self, &setup) == ESP_OK && ap.dhcp && handed_dns);
    assert(ap.ip.ip.addr == setup.ipv4 && ap.ip.gw.addr == setup.ipv4 && ap.ip.netmask.addr == 0xffffff &&
           ap_cfg.ap.channel == 1 && ap_cfg.ap.authmode == WIFI_AUTH_OPEN && connects == 1);
    check_external_stack(&(external_stack_case_t){.driver = d, .stop_ap = true});
    assert(mode == WIFI_MODE_APSTA && ap.dhcp);
    fail_dhcp_stop = true;
    assert(d->ap_stop(d->self) == ESP_FAIL && mode == WIFI_MODE_STA && ap.dhcp);
    assert(d->ap_stop(d->self) == ESP_OK && mode == WIFI_MODE_STA && !ap.dhcp);
    int8_t dbm = 0;
    assert(d->signal(d->self, &dbm) == ESP_OK && dbm == -43);
    uint8_t mac[6];
    assert(d->mac(d->self, mac) == ESP_OK && mac[5] == 0x41);
    d->report(d->self, &(xiaotai_reply_t){.write = diag, .ctx = "dns_errors=2 dns_last=-22"});
    assert(d->leave(d->self) == ESP_OK && pending_fence == 7 && event_count == 6);
    const wifi_event_sta_disconnected_t gone = {.reason = WIFI_REASON_BEACON_TIMEOUT, .rssi = -66};
    fire(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, (void *)&gone);
    assert(events[6].token == 7 && events[6].loss == XIAOTAI_NET_LOSS_REQUESTED && events[6].signal_dbm == -66);
    fire(XIAOTAI_WIFI_FENCE, 0, &pending_fence);
    assert(event_count == 7 && d->leave(d->self) == ESP_ERR_INVALID_STATE);

    assert(d->scan(d->self, 101) == ESP_OK && d->scan_cancel(d->self) == ESP_OK);
    assert(d->scan(d->self, 102) == XIAOTAI_ERR_BUSY);
    wifi_event_sta_scan_done_t scan_done = {0};
    fire(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &scan_done);
    assert(event_count == 7 && cleans == 1);
    assert(d->scan(d->self, 102) == ESP_OK);
    fire(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &scan_done);
    assert(events[7].kind == XIAOTAI_NET_DRIVER_SCAN_DONE && events[7].token == 102);
    xiaotai_net_scan_record_t entry;
    size_t count;
    bool truncated;
    assert(d->scan_read(d->self, &entry, 1, &count, &truncated) == ESP_OK && count == 1 && truncated &&
           entry.security == XIAOTAI_NET_SECURITY_WPA2_WPA3 && !strcmp(entry.ssid, "visible"));
    assert(d->scan_read(d->self, &entry, 1, &count, &truncated) == ESP_ERR_INVALID_STATE);
    assert(d->join(d->self, 8, &credentials, 0) == ESP_OK && station_cfg.sta.threshold.authmode == WIFI_AUTH_OPEN);
    fire(WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    assert(events[8].kind == XIAOTAI_NET_DRIVER_STOPPED && events[9].kind == XIAOTAI_NET_DRIVER_LOST && events[9].token == 8);
    fire(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    assert(events[10].kind == XIAOTAI_NET_DRIVER_STARTED && stopped == 1);

    assert(d->ap_start(d->self, &setup) == ESP_OK && mode == WIFI_MODE_APSTA && ap.dhcp);
    assert(d->join(d->self, 9, &credentials, 0) == ESP_OK);
    sta_ipv6_count = 0;
    fire(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &connected);
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    size_t seen = event_count;
    assert(events[seen - 1].kind == XIAOTAI_NET_DRIVER_ADDRESS &&
           events[seen - 1].token == 9 && events[seen - 1].ipv4 == 123);
    unsigned calls = disconnects;
    fire(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL); /* Stale loss: the current address still exists. */
    assert(event_count == seen);
    sta.ip.ip.addr = 0;
    ip_event_got_ip_t ap_ip = {.esp_netif = &ap};
    fire(IP_EVENT, IP_EVENT_STA_LOST_IP, &ap_ip);
    assert(event_count == seen);
    fire(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip);
    assert(event_count == seen + 1 && disconnects == calls &&
           events[seen].kind == XIAOTAI_NET_DRIVER_ADDRESS && events[seen].token == 9 &&
           events[seen].ipv4 == 0 && !events[seen].ipv6_valid);
    ++seen;
    fire(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    disconnect_result = ESP_FAIL;
    assert(d->leave(d->self) == ESP_FAIL && disconnects == calls + 1);
    assert(d->join(d->self, 10, &credentials, 0) == ESP_ERR_INVALID_STATE && event_count == seen);
    disconnect_result = ESP_ERR_INVALID_STATE;
    assert(d->leave(d->self) == ESP_FAIL && disconnects == calls + 2);
    /* Neither family can make the old token online while waiting for disassociation. */
    const unsigned reads_at_leave = dns_reads[0] + dns_reads[1] + dns_reads[2];
    sta.ip.ip.addr = 123;
    sta_ipv6[0] = v6.ip6_info.ip;
    sta_ipv6_count = 1;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    fire(IP_EVENT, IP_EVENT_GOT_IP6, &v6);
    assert(dns_reads[0] + dns_reads[1] + dns_reads[2] == reads_at_leave && dns_writes == 2);
    disconnect_result = ESP_OK;
    assert(d->leave(d->self) == ESP_OK && disconnects == calls + 3 && event_count == seen);
    assert(d->join(d->self, 10, &credentials, 0) == ESP_ERR_INVALID_STATE);

    unsigned posts = fence_posts;
    disconnect_result = ESP_ERR_WIFI_NOT_CONNECT;
    fence_result = ESP_ERR_TIMEOUT;
    assert(d->leave(d->self) == ESP_ERR_TIMEOUT && disconnects == calls + 4 && fence_posts == posts + 1);
    assert(d->join(d->self, 10, &credentials, 0) == ESP_ERR_INVALID_STATE && event_count == seen);
    fence_result = ESP_OK;
    assert(d->leave(d->self) == ESP_OK && pending_fence == 9 && fence_posts == posts + 2);
    assert(d->leave(d->self) == ESP_OK && disconnects == calls + 4 &&
           fence_posts == posts + 2 && event_count == seen);
    fire(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, (void *)&gone);
    assert(event_count == seen + 1 && events[seen].kind == XIAOTAI_NET_DRIVER_LOST &&
           events[seen].token == 9 && events[seen].loss == XIAOTAI_NET_LOSS_ADDRESS &&
           events[seen].code == WIFI_REASON_BEACON_TIMEOUT);
    assert(mode == WIFI_MODE_APSTA && ap.dhcp && stopped == 1);

    /* A queued old marker is token-bound; a queued old IP must match live STA state. */
    assert(d->join(d->self, 10, &credentials, 0) == ESP_OK);
    fire(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &connected);
    seen = event_count;
    fire(XIAOTAI_WIFI_FENCE, 0, &pending_fence);
    ip_event_got_ip6_t v6_first = {.esp_netif = &sta, .ip6_info.ip.addr = {0x20, 0, 0, 1}};
    ip_event_got_ip6_t v6_second = {.esp_netif = &sta, .ip6_info.ip.addr = {0x20, 0, 0, 2}};
    ip_event_got_ip6_t v6_local = {.esp_netif = &sta, .ip6_info.ip.addr = {0x80fe, 0, 0, 1}};
    sta_ipv6[0] = v6_first.ip6_info.ip;
    sta_ipv6[1] = v6_second.ip6_info.ip;
    sta_ipv6[2] = v6_local.ip6_info.ip;
    sta_ipv6_count = 3;
    sta_netif_up = false; /* esp_netif_get_ip_info can still return the cached 123. */
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    fire(IP_EVENT, IP_EVENT_GOT_IP6, &v6_first);
    sta_netif_up = true;
    sta.ip.ip.addr = 124;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    fire(IP_EVENT, IP_EVENT_GOT_IP6, &v6); /* Stale global address from another association. */
    fire(IP_EVENT, IP_EVENT_GOT_IP6, &v6_local);
    assert(event_count == seen);
    ip.ip_info.ip.addr = 124;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    fire(IP_EVENT, IP_EVENT_GOT_IP6, &v6_first);
    fire(IP_EVENT, IP_EVENT_GOT_IP6, &v6_second);
    assert(event_count == seen + 3 && events[seen + 1].token == 10 &&
           events[seen + 1].ipv4 == 124 && events[seen + 1].ipv6_valid &&
           !memcmp(events[seen + 1].ipv6, v6_first.ip6_info.ip.addr, sizeof(events[seen + 1].ipv6)) &&
           events[seen + 2].token == 10 && events[seen + 2].ipv4 == 124 && events[seen + 2].ipv6_valid &&
           !memcmp(events[seen + 2].ipv6, v6_second.ip6_info.ip.addr, sizeof(events[seen + 2].ipv6)));
    seen = event_count;
    calls = disconnects;
    sta.ip.ip.addr = 0;
    fire(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    fire(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    assert(event_count == seen + 1 && disconnects == calls &&
           events[seen].kind == XIAOTAI_NET_DRIVER_ADDRESS && events[seen].token == 10 &&
           events[seen].ipv4 == 0 && events[seen].ipv6_valid &&
           !memcmp(events[seen].ipv6, v6_second.ip6_info.ip.addr, sizeof(events[seen].ipv6)));
    sta.ip.ip.addr = 124;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(event_count == seen + 2 && events[seen + 1].ipv4 == 124 && events[seen + 1].ipv6_valid);
    disconnect_result = ESP_ERR_WIFI_NOT_STARTED;
    assert(d->leave(d->self) == ESP_OK && pending_fence == 10 && event_count == seen + 2);
    fire(XIAOTAI_WIFI_FENCE, 0, &pending_fence);
    fire(XIAOTAI_WIFI_FENCE, 0, &pending_fence);
    assert(event_count == seen + 3 && events[seen + 2].kind == XIAOTAI_NET_DRIVER_LOST &&
           events[seen + 2].token == 10 && events[seen + 2].loss == XIAOTAI_NET_LOSS_REQUESTED);
    assert(d->leave(d->self) == ESP_ERR_INVALID_STATE && mode == WIFI_MODE_APSTA && ap.dhcp);

    assert(d->join(d->self, 11, &credentials, 0) == ESP_OK);
    sta_ipv6_count = 0;
    fire(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &connected);
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    seen = event_count;
    disconnect_result = ESP_OK;
    assert(d->leave(d->self) == ESP_OK);
    sta.ip.ip.addr = 0;
    fire(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    fire(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, (void *)&gone);
    assert(event_count == seen + 2 && events[seen].kind == XIAOTAI_NET_DRIVER_ADDRESS &&
           !events[seen].ipv4 && !events[seen].ipv6_valid &&
           events[seen + 1].kind == XIAOTAI_NET_DRIVER_LOST && events[seen + 1].token == 11 &&
           events[seen + 1].loss == XIAOTAI_NET_LOSS_REQUESTED);

    /* An IPv4 loss must not leave an expired IPv6 address in the snapshot.
     * Keep another live global if present; a link-local is not a substitute. */
    assert(d->join(d->self, 12, &credentials, 0) == ESP_OK);
    fire(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &connected);
    sta.ip.ip.addr = 124;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    sta_ipv6[0] = v6_first.ip6_info.ip;
    sta_ipv6[1] = v6_second.ip6_info.ip;
    sta_ipv6_count = 2;
    fire(IP_EVENT, IP_EVENT_GOT_IP6, &v6_first);
    seen = event_count;
    sta_ipv6[0] = v6_second.ip6_info.ip;
    sta_ipv6_count = 1; /* v6_first has expired and this IDF has no LOST_IP6 event. */
    sta.ip.ip.addr = 0;
    fire(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    assert(event_count == seen + 1 && events[seen].kind == XIAOTAI_NET_DRIVER_ADDRESS &&
           events[seen].token == 12 && !events[seen].ipv4 && events[seen].ipv6_valid &&
           !memcmp(events[seen].ipv6, v6_second.ip6_info.ip.addr, sizeof(events[seen].ipv6)));
    sta.ip.ip.addr = 124;
    fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    seen = event_count;
    sta_ipv6[0] = v6_local.ip6_info.ip;
    sta.ip.ip.addr = 0;
    fire(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    assert(event_count == seen + 1 && events[seen].kind == XIAOTAI_NET_DRIVER_ADDRESS &&
           events[seen].token == 12 && !events[seen].ipv4 && !events[seen].ipv6_valid);
    fire(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, (void *)&gone);
    assert(event_count == seen + 2 && events[seen + 1].kind == XIAOTAI_NET_DRIVER_LOST &&
           events[seen + 1].token == 12 && events[seen + 1].loss == XIAOTAI_NET_LOSS_ADDRESS);
    assert(d->ap_stop(d->self) == ESP_OK);
}
#endif
