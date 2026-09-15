#!/usr/bin/env python3
"""Check the actual fallback DNS policy with host C stubs.

Requires CC/cc. Does not test DHCP packets, DNS reachability or board behavior.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
assert re.search(
    r'set_property\(DIRECTORY\s+APPEND\s+PROPERTY\s+CMAKE_CONFIGURE_DEPENDS\s+'
    r'"\$\{CMAKE_CURRENT_LIST_DIR\}/main/Kconfig\.xiaotai"\s*\)', cmake
), "Incremental builds must regenerate configuration after DNS Kconfig changes"
assert 'config XIAOTAI_FALLBACK_DNS_IPV4' in (
    ROOT / "main/Kconfig.xiaotai"
).read_text(encoding="utf-8")
source = (ROOT / "components/wifi_manager/src/wifi_manager.c").read_text(encoding="utf-8")
match = re.search(r"^static esp_err_t wifi_apply_fallback_dns\([^;{}]*\)\s*\{", source, re.M)
assert match
function = source[match.start():source.index("\n}", match.end()) + 2]
event = source[source.index("event_id == IP_EVENT_STA_GOT_IP"):]
assert event.index("wifi_apply_fallback_dns(got_ip->esp_netif)") < event.index("s_connected = true")
assert event.index('"got-ip"') < event.index("wifi_apply_fallback_dns(got_ip->esp_netif)")
assert event.index('"after-portal-stop"') > event.index("stop_provisioning()")

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM };
enum { ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK };
enum { ESP_IPADDR_TYPE_V4, ESP_IPADDR_TYPE_V6 = 6 };
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { int type; union {
    esp_ip4_addr_t ip4; struct { uint32_t addr[4]; } ip6;
} u_addr; } esp_ip_addr_t;
typedef struct { esp_ip_addr_t ip; } esp_netif_dns_info_t;
typedef struct { int unused; } esp_netif_t;
#define ESP_IP_IS_ANY(a) ((a).type == ESP_IPADDR_TYPE_V4 ? !(a).u_addr.ip4.addr : \
    !((a).u_addr.ip6.addr[0] | (a).u_addr.ip6.addr[1] | (a).u_addr.ip6.addr[2] | (a).u_addr.ip6.addr[3]))
#define TAG "test"
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
static const char *configured = "223.5.5.5";
#define CONFIG_XIAOTAI_FALLBACK_DNS_IPV4 configured
static esp_netif_t station;
static esp_netif_dns_info_t slots[3];
static int read_error, write_error, reads, writes;
static esp_err_t esp_netif_get_dns_info(esp_netif_t *n, int slot, esp_netif_dns_info_t *out) {
    assert(n == &station && slot == ESP_NETIF_DNS_FALLBACK);
    reads++; if (read_error) return read_error;
    *out = slots[slot]; return ESP_OK;
}
static esp_err_t esp_netif_set_dns_info(esp_netif_t *n, int slot, esp_netif_dns_info_t *value) {
    assert(n == &station && slot == ESP_NETIF_DNS_FALLBACK);
    writes++; if (write_error) return write_error;
    slots[slot] = *value; return ESP_OK;
}
static esp_err_t esp_netif_str_to_ip4(const char *text, esp_ip4_addr_t *out) {
    if (!strcmp(text, "223.5.5.5")) out->addr = 0x050505df;
    else if (!strcmp(text, "0.0.0.0")) out->addr = 0;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
static void reset(void) {
    memset(slots, 0, sizeof(slots));
    read_error = write_error = reads = writes = 0;
    configured = "223.5.5.5";
}
'''
code += function
code += r'''
int main(void) {
    reset();
    assert(wifi_apply_fallback_dns(&station) == ESP_OK);
    assert(writes == 1 && slots[2].ip.u_addr.ip4.addr == 0x050505df);
    assert(!slots[0].ip.u_addr.ip4.addr && !slots[1].ip.u_addr.ip4.addr);
    assert(wifi_apply_fallback_dns(&station) == ESP_OK && writes == 1);

    reset(); slots[0].ip.u_addr.ip4.addr = 10; slots[1].ip.u_addr.ip4.addr = 20;
    assert(wifi_apply_fallback_dns(&station) == ESP_OK && writes == 1);
    assert(slots[0].ip.u_addr.ip4.addr == 10 && slots[1].ip.u_addr.ip4.addr == 20);

    reset(); slots[2].ip.u_addr.ip4.addr = 30;
    assert(wifi_apply_fallback_dns(&station) == ESP_OK && writes == 0);
    assert(slots[2].ip.u_addr.ip4.addr == 30);
    reset(); slots[2].ip.type = ESP_IPADDR_TYPE_V6; slots[2].ip.u_addr.ip6.addr[3] = 1;
    assert(wifi_apply_fallback_dns(&station) == ESP_OK && writes == 0);

    reset(); configured = "";
    assert(wifi_apply_fallback_dns(&station) == ESP_OK && !reads && !writes);
    reset(); configured = "invalid";
    assert(wifi_apply_fallback_dns(&station) == ESP_ERR_INVALID_ARG && !writes);
    configured = "0.0.0.0";
    assert(wifi_apply_fallback_dns(&station) == ESP_ERR_INVALID_ARG && !writes);
    reset();
    assert(wifi_apply_fallback_dns(NULL) == ESP_ERR_INVALID_ARG && !reads);
    read_error = ESP_ERR_TIMEOUT;
    assert(wifi_apply_fallback_dns(&station) == ESP_ERR_TIMEOUT && !writes);
    reset(); write_error = ESP_ERR_NO_MEM;
    assert(wifi_apply_fallback_dns(&station) == ESP_ERR_NO_MEM && writes == 1);
    assert(!slots[2].ip.u_addr.ip4.addr);
    puts("Wi-Fi DNS policy: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-wifi-dns-") as directory:
    c_file = Path(directory) / "dns.c"
    binary = Path(directory) / ("dns.exe" if os.name == "nt" else "dns")
    c_file.write_text(code, encoding="utf-8")
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_file), "-o", str(binary)
    ], check=True)
    subprocess.run([str(binary)], check=True)
