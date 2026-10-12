#!/usr/bin/env python3
"""Execute the production clock gate with deterministic SNTP timing."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / "components/platform_client/src/platform_client.c").read_text(encoding="utf-8")
assert 'ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org")' in source
provision = source[source.index("esp_err_t platform_client_provision("):
                   source.index("esp_err_t platform_client_start(")]
assert "snprintf(s_verification_code" not in provision
wait = source[source.index("static esp_err_t wait_for_auth_grant("):
              source.index("esp_err_t platform_client_provision(")]
assert wait.index("if ((bits & PROVISION_READY_BIT)") < wait.index("snprintf(s_verification_code")
assert wait.index("snprintf(s_verification_code") < wait.index("play_verification_prompt(report")
function = source[source.index("static bool s_clock_synchronized;"):
                  source.index("typedef struct {\n    char code[17];")]
assert "platform_client_sync_clock(void);" in (
    project / "components/platform_client/include/platform_client.h").read_text(encoding="utf-8")
app = (project / "main/app_main.c").read_text(encoding="utf-8")
startup = app[app.index("static void starter_start_task(void *argument)"):
              app.index("static void init_nvs(void)")]
gate = startup.index("err = platform_client_sync_clock();")
assert startup.index("while (!wifi_manager_connected())") < gate
for consumer in ("if (!credentials_valid)", "provision_and_save(",
                 "starter_runtime_start(", "starter_tirtc_start("):
    assert gate < startup.index(consumer), consumer
for consumer in (provision, source[source.index("esp_err_t platform_client_start("):
                                  source.index("static void release_request_resources(")]):
    assert consumer.index("platform_client_sync_clock()") < consumer.index("discover_services(")
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
typedef int esp_err_t;
typedef struct { unsigned num_of_servers; const char *servers[2]; } esp_sntp_config_t;
typedef struct { unsigned value; } ip_addr_t;
#define IPADDR_STRLEN_MAX 48
#define ESP_NETIF_DNS_MAX 3
#define ESP_NETIF_DNS_FALLBACK 2
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_TIMEOUT -3
#define ESP_ERR_INVALID_RESPONSE -4
#define SNTP_RECV_TIMEOUT 15000U
#define SNTP_STARTUP_DELAY 1
#define SNTP_MONITOR_SERVER_REACHABILITY 1
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY 5000U
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_SNTP_SERVER_LIST(...) {__VA_ARGS__}
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n, list) ((esp_sntp_config_t){n, list})
static const char *TAG = "test";
static void log_stub(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI log_stub
#define ESP_LOGE log_stub
#define ESP_LOGW log_stub
static unsigned now_ms, answer_ms, wait_calls, init_calls, budget;
static unsigned snapshot_calls, getter_calls;
static int init_result, wait_result, snapshot_result;
static bool valid, initialized, in_tcpip;
static ip_addr_t dns_addresses[ESP_NETIF_DNS_MAX], peer_addresses[2];
static uint8_t reachability[2];
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
static time_t fake_time(time_t *unused) { (void)unused; return valid ? 1800000000 : 0; }
#define time fake_time
static int64_t esp_timer_get_time(void) { return (int64_t)now_ms * 1000; }
static esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) {
    assert(config->num_of_servers == 2); ++init_calls;
    if (initialized) return ESP_ERR_INVALID_STATE;
    if (init_result == ESP_OK) initialized = true;
    return init_result;
}
static bool esp_sntp_enabled(void) { assert(in_tcpip); return initialized; }
static const ip_addr_t *dns_getserver(unsigned index) {
    assert(in_tcpip && index < ESP_NETIF_DNS_MAX); ++getter_calls; return &dns_addresses[index];
}
static const ip_addr_t *esp_sntp_getserver(unsigned index) {
    assert(in_tcpip && index < 2); ++getter_calls; return &peer_addresses[index];
}
static uint8_t esp_sntp_getreachability(unsigned index) {
    assert(in_tcpip && index < 2); ++getter_calls; return reachability[index];
}
static esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void *), void *context) {
    assert(!in_tcpip); ++snapshot_calls;
    if (snapshot_result != ESP_OK) return snapshot_result;
    in_tcpip = true;
    esp_err_t result = callback(context);
    in_tcpip = false;
    return result;
}
static char *ipaddr_ntoa_r(const ip_addr_t *address, char *output, int size) {
    assert(!in_tcpip);
    snprintf(output, size, "0.0.0.%u", address->value);
    return output;
}
static size_t heap_caps_get_free_size(unsigned caps) {
    assert(!in_tcpip && caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return 65536;
}
static size_t heap_caps_get_largest_free_block(unsigned caps) {
    assert(!in_tcpip && caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return 32768;
}
static esp_err_t esp_netif_sntp_sync_wait(unsigned ticks) {
    assert(initialized);
    ++wait_calls; budget = ticks;
    assert(ticks == 2 * SNTP_RECV_TIMEOUT + SNTP_RECV_TIMEOUT + 5000);
    if (wait_result != ESP_ERR_TIMEOUT) {
        valid = answer_ms != 0; return wait_result;
    }
    if (answer_ms && answer_ms <= now_ms + ticks) {
        if (answer_ms > now_ms) now_ms = answer_ms;
        valid = true; return ESP_OK;
    }
    now_ms += ticks; return ESP_ERR_TIMEOUT;
}
''' + function + r'''
static void reset(unsigned answer) {
    now_ms = wait_calls = init_calls = budget = 0;
    answer_ms = answer; init_result = ESP_OK; wait_result = ESP_ERR_TIMEOUT; valid = false;
    s_clock_synchronized = false;
    s_clock_initialized = initialized = in_tcpip = false;
    snapshot_calls = getter_calls = 0; snapshot_result = ESP_OK;
}
int main(void) {
    const unsigned successful[] = {60, 15060, 27000, 45000, 49999, 50000};
    for (unsigned i = 0; i < sizeof(successful) / sizeof(successful[0]); ++i) {
        reset(successful[i]); assert(platform_client_sync_clock() == ESP_OK);
        assert(now_ms == successful[i] && wait_calls == 1 && init_calls == 1);
        assert(s_clock_synchronized);
        assert(snapshot_calls == 0);
        /* Binding and platform auth reuse this boot's completed sync. */
        assert(platform_client_sync_clock() == ESP_OK);
        assert(wait_calls == 1 && init_calls == 1);
    }
    reset(50001); assert(platform_client_sync_clock() == ESP_ERR_TIMEOUT && now_ms == 50000);
    assert(!s_clock_synchronized);
    reset(0); assert(platform_client_sync_clock() == ESP_ERR_TIMEOUT && wait_calls == 1);
    assert(initialized && s_clock_initialized && snapshot_calls == 1 && getter_calls == ESP_NETIF_DNS_MAX + 4);
    /* A plausible retained clock cannot bypass the first sync of a new boot. */
    reset(0); valid = true;
    assert(platform_client_sync_clock() == ESP_ERR_TIMEOUT && wait_calls == 1 && !s_clock_synchronized);
    reset(60); valid = true; assert(platform_client_sync_clock() == ESP_OK && wait_calls == 1);
    /* Existing SNTP after a timeout can complete on a later bounded attempt. */
    reset(0); assert(platform_client_sync_clock() == ESP_ERR_TIMEOUT);
    answer_ms = now_ms + 15060;
    assert(platform_client_sync_clock() == ESP_OK && wait_calls == 2 && s_clock_synchronized);
    assert(init_calls == 1 && now_ms == 65060);
    /* Three wait windows share one service, including a reply during the gap. */
    reset(110001);
    assert(platform_client_sync_clock() == ESP_ERR_TIMEOUT);
    now_ms += 5000;
    assert(platform_client_sync_clock() == ESP_ERR_TIMEOUT);
    now_ms += 10000;
    assert(platform_client_sync_clock() == ESP_OK && now_ms == 115000);
    assert(wait_calls == 3 && init_calls == 1 && snapshot_calls == 2);
    /* Losing valid time invalidates the cached gate result. */
    valid = false; answer_ms = 0;
    assert(platform_client_sync_clock() == ESP_ERR_TIMEOUT && !s_clock_synchronized);
    assert(init_calls == 1);
    reset(0); init_result = ESP_FAIL; assert(platform_client_sync_clock() == ESP_FAIL && wait_calls == 0);
    assert(!s_clock_initialized);
    init_result = ESP_OK; answer_ms = 10;
    assert(platform_client_sync_clock() == ESP_OK && init_calls == 2 && wait_calls == 1);
    /* Unknown pre-existing owners must not silently satisfy our wait contract. */
    reset(0); initialized = true;
    assert(platform_client_sync_clock() == ESP_ERR_INVALID_STATE && wait_calls == 0);
    assert(!s_clock_initialized && !s_clock_synchronized);
    reset(0); wait_result = ESP_OK; assert(platform_client_sync_clock() == ESP_ERR_INVALID_RESPONSE);
    assert(!s_clock_synchronized);
    reset(0); wait_result = ESP_FAIL; assert(platform_client_sync_clock() == ESP_FAIL);
    /* Diagnostic failures cannot turn the original timeout into success. */
    reset(0); snapshot_result = ESP_FAIL;
    assert(platform_client_sync_clock() == ESP_ERR_TIMEOUT && !s_clock_synchronized);
    assert(snapshot_calls == 1 && getter_calls == 0);
    /* Snapshots copy lwIP-owned state instead of exporting live pointers. */
    clock_network_snapshot_t snapshot = {0};
    reset(0); initialized = true;
    dns_addresses[0].value = 1; dns_addresses[1].value = 2; dns_addresses[2].value = 5;
    peer_addresses[0].value = 3; peer_addresses[1].value = 0;
    reachability[0] = 0x05; reachability[1] = 0;
    assert(esp_netif_tcpip_exec(clock_network_snapshot, &snapshot) == ESP_OK);
    peer_addresses[0].value = 4;
    assert(snapshot.enabled && snapshot.dns[0].value == 1 && snapshot.dns[1].value == 2);
    assert(snapshot.dns[2].value == 5);
    assert(snapshot.peer[0].value == 3 && snapshot.peer[1].value == 0);
    assert(snapshot.reach[0] == 0x05 && snapshot.reach[1] == 0);
    puts("PASS: first-boot sync, single-owner init, late replies, timeout reuse, safe diagnostics, failures");
}
'''
with tempfile.TemporaryDirectory(prefix="xiaotai-clock-") as folder:
    path = Path(folder)
    (path / "test.c").write_text(harness, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
