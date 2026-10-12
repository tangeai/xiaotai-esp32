#!/usr/bin/env python3
"""Compile the real identity helper for both targets with deterministic MAC APIs."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / "main/app_main.c").read_text()
start = source.index("static esp_err_t station_identity(")
end = source.index("\nstatic esp_err_t play_verification_prompt", start)
helper = source[start:end]
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define WIFI_IF_STA 0
#define ESP_MAC_WIFI_STA 0
static int failure, calls;
static int read_test_mac(uint8_t *mac) {
    ++calls;
    if (failure) return -7;
    const uint8_t value[] = {2, 4, 6, 8, 10, 12};
    memcpy(mac, value, sizeof(value));
    return ESP_OK;
}
#if CONFIG_IDF_TARGET_ESP32P4
static int esp_wifi_get_mac(int iface, uint8_t *mac) {
    assert(iface == WIFI_IF_STA); return read_test_mac(mac);
}
#else
static int esp_read_mac(uint8_t *mac, int type) {
    assert(type == ESP_MAC_WIFI_STA); return read_test_mac(mac);
}
#endif
''' + helper + r'''
int main(void) {
    char mac[18] = "unchanged", client[65] = "unchanged";
    failure = 1;
    assert(station_identity(mac, client) == -7);
    assert(strcmp(mac, "unchanged") == 0);
    assert(strcmp(client, "unchanged") == 0);
    failure = 0;
    assert(station_identity(mac, client) == ESP_OK);
    assert(strcmp(mac, "02:04:06:08:0A:0C") == 0);
    assert(strcmp(client, CONFIG_IDF_TARGET "-020406080a0c") == 0);
    assert(calls == 2);
    const char *unchanged_clients[] = {NULL, "", "custom-device", "FFFFFFFFFFFF",
                                      "020406080a0c", CONFIG_IDF_TARGET "-020406080a0c"};
    for (size_t i = 0; i < sizeof(unchanged_clients) / sizeof(*unchanged_clients); ++i) {
        assert(!station_binding_identity(mac, unchanged_clients[i]));
        assert(strcmp(mac, "02:04:06:08:0A:0C") == 0);
    }
    assert(station_binding_identity(mac, "020406080A0C"));
    assert(strcmp(mac, "020406080A0C") == 0);
    assert(!station_binding_identity(mac, "020406080A0C"));
    assert(!station_binding_identity(NULL, "020406080A0C"));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="xiaotai-identity-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(test)
    for target in ("esp32s3", "esp32p4"):
        binary = path / target
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f'-DCONFIG_IDF_TARGET="{target}"',
            f"-DCONFIG_IDF_TARGET_ESP32P4={int(target == 'esp32p4')}",
            str(path / "test.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
print("PASS: S3/P4 hardware identity; exact legacy binding preserved; fresh/custom IDs unchanged")
