#!/usr/bin/env python3
"""Source contract for a deterministic SoftAP captive-portal startup."""

from pathlib import Path
import re
import sys


PROJECT = Path(__file__).resolve().parents[1]
WIFI_SOURCE = PROJECT / "components/wifi_manager/src/wifi_manager.c"
DNS_SOURCE = PROJECT / "components/wifi_manager/src/captive_dns.c"


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        return ""
    depth = 1
    cursor = match.end()
    while cursor < len(source) and depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    return source[match.end():cursor - 1] if depth == 0 else ""


def main() -> int:
    wifi = WIFI_SOURCE.read_text(encoding="utf-8")
    dns = DNS_SOURCE.read_text(encoding="utf-8")
    start = function_body(dns, "captive_dns_start")
    task = function_body(dns, "dns_task")

    socket_at = start.find("socket(")
    bind_at = start.find("bind(")
    task_at = start.find("xTaskCreate(")
    required = {
        "DNS socket is created synchronously": socket_at >= 0,
        "UDP/53 is bound synchronously": bind_at >= 0,
        "socket and bind finish before task creation": (
            socket_at >= 0 and bind_at > socket_at and task_at > bind_at
        ),
        "DNS worker only serves an already-bound socket": (
            "socket(" not in task and "bind(" not in task
        ),
        "HTTP handler registration errors are checked": (
            "httpd_register_uri_handler" in wifi
            and "cannot register provisioning HTTP handlers" in wifi
        ),
        "SoftAP DHCP advertises its local DNS": "ESP_NETIF_DOMAIN_NAME_SERVER" in wifi,
        "HTTP setup page is not misadvertised as a CAPPORT API": (
            "ESP_NETIF_CAPTIVEPORTAL_URI" not in wifi
        ),
        "DNS is bound only to the AP address": ".sin_addr.s_addr = ip_info.ip.addr" in start,
        "DNS worker supports bounded cooperative stop": (
            "SO_RCVTIMEO" in start and "SO_SNDTIMEO" in start
            and "while (!atomic_load(&s_dns_stop))" in task
            and "close(socket_fd)" in task
            and "atomic_store(&s_dns_running, false)" in task
            and "vTaskDelete(NULL)" in task
            and "vTaskDelete(s_dns_task)" not in dns
        ),
        "Portal revokes writes before joining HTTP and DNS": (
            function_body(wifi, "stop_provisioning").find("atomic_store(&s_provisioning, false)")
            < function_body(wifi, "stop_provisioning").find("httpd_stop(s_http_server)")
            and "captive_dns_stop()" in function_body(wifi, "stop_provisioning")
        ),
        "Portal rejects STA requests and expired body reads": (
            "config.open_fn = portal_open" in wifi
            and "local_ipv4 == ap_ip.ip.addr" in wifi
            and "esp_timer_get_time() >= deadline" in wifi
        ),
    }
    missing = [name for name, present in required.items() if not present]
    if missing:
        print("FAIL: captive portal startup contract is incomplete")
        for name in missing:
            print(f"missing: {name}")
        return 1

    print("PASS: captive DNS binds synchronously before client probes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
