#!/usr/bin/env python3
"""Run real Wi-Fi owner/retry functions with deterministic clock/driver stubs."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / "components/wifi_manager/src/wifi_manager.c").read_text(encoding="utf-8")
functions = source[source.index("static esp_err_t configure_station_dns("):
                   source.index("esp_err_t wifi_manager_start(")]
got_ip_owner = functions[functions.index("event_id == IP_EVENT_STA_GOT_IP"):]
assert got_ip_owner.index("configure_station_dns(got_ip->esp_netif)") < got_ip_owner.index("s_connected = true")
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int esp_event_base_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define ESP_ERR_WIFI_NOT_CONNECT -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define WIFI_MANAGER_CONTROL 1
#define WIFI_EVENT 2
#define IP_EVENT 3
#define WIFI_EVENT_STA_START 4
#define WIFI_EVENT_STA_DISCONNECTED 5
#define IP_EVENT_STA_GOT_IP 6
#define WIFI_EVENT_STA_CONNECTED 7
#define WIFI_EVENT_SCAN_DONE 8
#define WIFI_PROVISION_AFTER_FAILURES 5U
enum { WIFI_CONTROL_DISCONNECT, WIFI_CONTROL_RETRY, WIFI_CONTROL_SCAN };
#define PORTAL_SCAN_TIMEOUT_US 5000000LL
#define PORTAL_SCAN_ERROR 5
static bool s_scan_active;
static int64_t s_scan_started_us;
static void portal_scan_begin(void) {}
static void portal_scan_done(void *event) { (void)event; }
static void portal_scan_cancel(int state) { assert(state == PORTAL_SCAN_ERROR); s_scan_active = false; }
static char s_station_credentials[98];
static int remember_network_job(void *data, size_t size) { (void)data; (void)size; return 0; }
static int nvs_worker_submit_latest(int (*job)(void *, size_t), void *data, size_t size) {
    assert(job == remember_network_job && data == s_station_credentials && size == 98); return 0;
}
typedef struct { unsigned id; } esp_netif_t;
static esp_netif_t station = {1};
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct {
    union { esp_ip4_addr_t ip4; } u_addr;
    unsigned type;
} esp_ip_addr_t;
typedef struct { esp_ip_addr_t ip; } esp_netif_dns_info_t;
typedef enum { ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP,
               ESP_NETIF_DNS_FALLBACK, ESP_NETIF_DNS_MAX } esp_netif_dns_type_t;
#define ESP_IPADDR_TYPE_V4 0U
#define ESP_IP_IS_ANY(ip) ((ip).u_addr.ip4.addr == 0)
static esp_netif_dns_info_t dns_servers[ESP_NETIF_DNS_MAX];
static int dns_read_fail_slot = -1, dns_write_error, dns_writes, dns_reads;
static const char *esp_err_to_name(int err) { (void)err; return "error"; }
static int esp_netif_get_dns_info(esp_netif_t *netif, esp_netif_dns_type_t slot,
                                  esp_netif_dns_info_t *dns) {
    assert(netif == &station && slot < ESP_NETIF_DNS_MAX);
    ++dns_reads;
    if ((int)slot == dns_read_fail_slot) return -21;
    *dns = dns_servers[slot];
    return ESP_OK;
}
static int esp_netif_set_dns_info(esp_netif_t *netif, esp_netif_dns_type_t slot,
                                  esp_netif_dns_info_t *dns) {
    assert(netif == &station && slot == ESP_NETIF_DNS_FALLBACK);
    ++dns_writes;
    if (dns_write_error) return dns_write_error;
    dns_servers[slot] = *dns;
    return ESP_OK;
}
static void esp_netif_set_ip4_addr(esp_ip4_addr_t *ip, unsigned a, unsigned b,
                                  unsigned c, unsigned d) {
    ip->addr = (a << 24) | (b << 16) | (c << 8) | d;
}
typedef struct { esp_netif_t *esp_netif; struct { int ip; } ip_info; } ip_event_got_ip_t;
typedef struct { unsigned reason; } wifi_event_sta_disconnected_t;
static bool s_started, s_connected, s_connecting, s_connection_failed;
static bool s_has_saved_credentials, s_provisioning;
static uint32_t s_retry_count;
static int64_t s_retry_due_us, now;
static int64_t s_connect_started_us, s_associated_us;
static int s_retry_timer;
static bool timer_running;
static char s_provisioning_status[128];
static atomic_bool s_manual_disconnect, s_control_pending;
static atomic_int s_control_error;
static int connect_calls, disconnect_calls, provisioning_calls;
static int post_error, connect_error, disconnect_error, portal_error, queued;
static int stop_error, stop_calls;
static int esp_wifi_connect(void) { ++connect_calls; return connect_error; }
static int esp_wifi_disconnect(void) { ++disconnect_calls; return disconnect_error; }
static int start_provisioning(void) {
    if (s_provisioning) return ESP_OK;
    ++provisioning_calls;
    if (!portal_error) s_provisioning = true;
    return portal_error;
}
static int stop_provisioning(void) {
    ++stop_calls; s_provisioning = false; return stop_error;
}
static void signal_connection_changed(void) {}
static int64_t esp_timer_get_time(void) { return now; }
static int esp_timer_stop(int timer) {
    (void)timer; timer_running = false; return ESP_OK;
}
static int esp_timer_start_periodic(int timer, uint64_t period) {
    (void)timer; assert(period == 1000000);
    if (timer_running) return ESP_ERR_INVALID_STATE;
    timer_running = true; return ESP_OK;
}
static int esp_event_post(int base, int id, void *data, size_t size, unsigned ticks) {
    assert(base == WIFI_MANAGER_CONTROL && data == NULL && size == 0 && ticks == 0);
    if (post_error) return post_error;
    queued = id; return ESP_OK;
}
''' + functions + r'''
static void drain(void) { wifi_event(NULL, WIFI_MANAGER_CONTROL, queued, NULL); }
static void disconnected(void) { wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL); }
static void got_ip(void) {
    ip_event_got_ip_t ip = {.esp_netif = &station};
    wifi_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
}
static void boot(void) {
    retry_stop(); s_retry_count = 0; s_connected = s_connecting = s_provisioning = false;
    s_connection_failed = false;
    atomic_store(&s_manual_disconnect, false);
    atomic_store(&s_control_pending, false);
    atomic_store(&s_control_error, ESP_OK);
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
}
static void tick(void) { assert(timer_running); retry_timer_callback(NULL); drain(); }
int main(void) {
    /* No DNS lease: fill only fallback; repeated got-IP must be idempotent. */
    assert(configure_station_dns(NULL) == ESP_ERR_INVALID_ARG);
    assert(dns_reads == 0 && dns_writes == 0);
    assert(configure_station_dns(&station) == ESP_OK);
    assert(dns_servers[0].ip.u_addr.ip4.addr == 0 && dns_servers[1].ip.u_addr.ip4.addr == 0);
    assert(dns_servers[2].ip.u_addr.ip4.addr == 0xdf050505U && dns_writes == 1);
    assert(configure_station_dns(&station) == ESP_OK && dns_writes == 1);
    /* Preserve every nonempty slot, including private or backup-only DNS. */
    for (unsigned slot = 0; slot < ESP_NETIF_DNS_MAX; ++slot) {
        memset(dns_servers, 0, sizeof(dns_servers));
        dns_servers[slot].ip.u_addr.ip4.addr = 0xc0a88901U;
        assert(configure_station_dns(&station) == ESP_OK && dns_writes == 1);
        assert(dns_servers[slot].ip.u_addr.ip4.addr == 0xc0a88901U);
    }
    /* Read/write errors remain errors, not a fabricated working resolver. */
    memset(dns_servers, 0, sizeof(dns_servers));
    for (int slot = 0; slot < ESP_NETIF_DNS_MAX; ++slot) {
        dns_read_fail_slot = slot;
        assert(configure_station_dns(&station) == -21 && dns_writes == 1);
    }
    dns_read_fail_slot = -1; dns_write_error = -22;
    assert(configure_station_dns(&station) == -22 && dns_writes == 2);
    assert(dns_servers[2].ip.u_addr.ip4.addr == 0);
    dns_write_error = 0; dns_reads = dns_writes = 0;
    (void)esp_err_to_name(0); /* Logging is compiled out in this driver harness. */
    assert(wifi_manager_disconnect() == ESP_ERR_INVALID_STATE);
    s_started = s_connected = s_has_saved_credentials = true;
    assert(wifi_manager_disconnect() == ESP_OK);
    assert(wifi_manager_control_pending());
    assert(wifi_manager_disconnect() == ESP_ERR_INVALID_STATE);
    drain(); disconnected();
    assert(!s_connected && wifi_manager_manually_disconnected() && s_provisioning);
    assert(connect_calls == 0 && provisioning_calls == 1 && !timer_running);
    got_ip(); /* Stale DHCP cannot undo the user's manual provisioning intent. */
    assert(!s_connected && disconnect_calls == 2 && stop_calls == 0);
    assert(dns_reads == 0 && dns_writes == 0);
    assert(wifi_manager_disconnect() == ESP_OK); drain(); disconnected();
    assert(connect_calls == 0 && provisioning_calls == 1);
    now += 1000000000; wifi_control_event(WIFI_CONTROL_RETRY);
    assert(connect_calls == 0);

    /* Reboot uses retained credentials. Duplicate events do not open concurrent
     * attempts. Automatic absence stays recoverable beyond the old retry limit. */
    boot(); assert(s_connecting && connect_calls == 1);
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    assert(connect_calls == 1);
    const unsigned delays[] = {1, 2, 4, 8, 15, 30};
    for (unsigned i = 0; i < 1000; ++i) {
        disconnected(); assert(timer_running);
        assert(s_retry_due_us == now + (int64_t)delays[i < 6 ? i : 5] * 1000000);
        int count = connect_calls;
        wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
        assert(connect_calls == count); /* A repeated start cannot bypass backoff. */
        tick(); assert(connect_calls == count);
        now = s_retry_due_us; tick();
        assert(connect_calls == count + 1 && s_connecting && !timer_running);
        wifi_control_event(WIFI_CONTROL_RETRY);
        assert(connect_calls == count + 1);
    }
    assert(s_provisioning && s_connection_failed && s_retry_count == 1000);
    got_ip(); assert(s_connected && s_retry_count == 0 && !timer_running && !s_connection_failed);
    assert(dns_writes == 1 && dns_servers[2].ip.u_addr.ip4.addr == 0xdf050505U);
    assert(!s_provisioning && stop_calls == 1 && wifi_manager_control_error() == ESP_OK);
    stop_error = -20; got_ip();
    assert(s_connected && !s_provisioning && wifi_manager_control_error() == -20);
    stop_error = 0; got_ip();
    disconnected(); assert(s_retry_due_us == now + 1000000);
    /* A full queue cannot lose the retry; a stale tick after success is inert. */
    int count = connect_calls;
    post_error = -10; now = s_retry_due_us; retry_timer_callback(NULL);
    assert(timer_running && connect_calls == count);
    post_error = 0; tick(); assert(connect_calls == count + 1);
    got_ip(); wifi_control_event(WIFI_CONTROL_RETRY); assert(connect_calls == count + 1);

    /* Manual provisioning cancels even an already scheduled automatic retry. */
    disconnected(); assert(timer_running);
    assert(wifi_manager_disconnect() == ESP_OK); drain();
    assert(!timer_running && s_retry_due_us == 0);
    wifi_control_event(WIFI_CONTROL_RETRY); disconnected();
    assert(connect_calls == count + 1 && wifi_manager_manually_disconnected());

    boot(); got_ip(); disconnect_error = -9;
    assert(wifi_manager_disconnect() == ESP_OK); drain();
    assert(s_connected && !wifi_manager_manually_disconnected());
    assert(wifi_manager_control_error() == -9);
    disconnect_error = 0; post_error = -10;
    assert(wifi_manager_disconnect() == -10 && !wifi_manager_control_pending());
    assert(s_connected && !wifi_manager_manually_disconnected());
    post_error = 0; portal_error = -11;
    assert(wifi_manager_disconnect() == ESP_OK); drain();
    assert(!s_connected && wifi_manager_manually_disconnected() && !s_provisioning);
    assert(wifi_manager_control_error() == -11);
    portal_error = 0;
    assert(wifi_manager_disconnect() == ESP_OK); drain();
    assert(s_provisioning && wifi_manager_control_error() == ESP_OK);

    connect_error = -12; boot(); assert(timer_running && !s_connecting);
    count = connect_calls; now = s_retry_due_us; tick();
    assert(connect_calls == count + 1 && timer_running && s_retry_count == 2);
    connect_error = 0;
    now = s_retry_due_us; tick(); got_ip();
    assert(s_connected && !timer_running);
    disconnected(); s_retry_count = UINT32_MAX;
    retry_stop(); retry_schedule();
    assert(s_retry_count == UINT32_MAX && s_retry_due_us == now + 30000000);
    s_has_saved_credentials = false; boot(); count = connect_calls;
    disconnected(); wifi_control_event(WIFI_CONTROL_RETRY);
    assert(s_provisioning && connect_calls == count && !timer_running);
    puts("PASS: missing/preserved DNS; manual portal; 1000 retries; capped backoff; network return; stale/duplicate events; driver/queue/portal failures; credentials retained");
}
'''
with tempfile.TemporaryDirectory(prefix="xiaotai-wifi-") as folder:
    path = Path(folder)
    (path / "test.c").write_text(test)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-variable", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
ui = (project / "components/starter_product/src/starter_product_s3_ui.inc").read_text(encoding="utf-8")
assert '"断开当前连接", 8, 188, 304, 48' in ui
assert "wifi_manager_reconnect" not in ui
assert "network_reconnect" not in ui
assert ui.index("else if (wifi_manager_provisioning())") < ui.index("else if (wifi_manager_manually_disconnected())")
assert "wifi_manager_forget_credentials" not in functions
assert "wifi_manager_load_credentials" not in functions
assert "s3_ui.network_disconnect = NULL" in ui
portal = source[source.index("static esp_err_t start_provisioning("):
                source.index("static void retry_timer_callback(")]
assert "ESP_ERROR_CHECK" not in portal
assert portal.index("captive_dns_start") < portal.index("s_provisioning = true")
