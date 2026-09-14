#!/usr/bin/env python3
"""Host checks of actual history/Hosted scan functions; no device operation.

Requires a C11 host compiler (CC, default cc). Does not validate C6 radio timing,
phone rendering, NVS Flash behaviour, or the P4 task stack high-water mark.
"""
import os
import re
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    # Match the definition, not a forward declaration with the same signature.
    match = re.search(re.escape(signature) + r"[^;{}]*\{", source)
    if match is None:
        raise ValueError("Function definition not found: " + signature)
    start = match.start()
    return source[start:source.index("\n}", match.end()) + 2]


def main():
    history = (ROOT / "components/wifi_manager/src/wifi_history.c").read_text(encoding="utf-8")
    manager = (ROOT / "components/wifi_manager/src/wifi_manager.c").read_text(encoding="utf-8")
    wrapper = (ROOT / "components/espressif__esp_hosted/host/drivers/rpc/wrap/rpc_wrap.c").read_text(encoding="utf-8")
    decoder = (ROOT / "components/espressif__esp_hosted/host/drivers/rpc/core/rpc_rsp.c").read_text(encoding="utf-8")
    page = (ROOT / "components/wifi_manager/web/setup.html").read_text(encoding="utf-8")
    assert 'EMBED_TXTFILES "web/setup.html"' in (ROOT / "components/wifi_manager/CMakeLists.txt").read_text()
    assert "esp_wifi_scan_start(&config, false)" in manager
    assert "if (s_scan_active) return;" in function(manager, "static void wifi_control_event(")
    assert "esp_wifi_clear_ap_list()" in function(manager, "static void portal_scan_finish(")
    assert "s_history_pending, true" in function(manager, "static void wifi_event(")
    assert "s_history_pending" not in function(manager, "static esp_err_t wifi_config_post(")
    api = function(manager, "static esp_err_t portal_networks_get(")
    assert "esp_wifi_scan_" not in api and "portal_socket_allowed" in api
    assert '"password"' not in function(manager, "static bool portal_append_network(")
    assert "name.textContent = network.ssid" in page and "innerHTML" not in page
    assert "localStorage" not in page and "use_saved:true" in page
    assert "n_ap_records" in decoder and "if (!p_a->number) break;" in decoder

    body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_RESPONSE 4
#define NVS_STORE_BLOB 1
#define NVS_STORE_ERASE_ALL 2
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define WIFI_HISTORY_CAPACITY 4
#define WIFI_HISTORY_NAMESPACE "wifi_cfg"
#define WIFI_HISTORY_KEY "known"
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef struct { char ssid[33]; char password[65]; } wifi_manager_credentials_t;
typedef struct { uint8_t version, count; wifi_manager_credentials_t entries[4]; } wifi_history_t;
typedef struct { int kind; const char *key; const void *value; size_t size; } nvs_store_op_t;
static wifi_history_t disk;
static wifi_manager_credentials_t s_active;
static void *s_mutex = (void *)1;
static bool s_allow_remember = true, alloc_fail;
static int writes, load_error, write_error, lock_ok = 1;
static int xSemaphoreTake(void *m, int t) {(void)m; (void)t; return lock_ok;}
static void xSemaphoreGive(void *m) {(void)m;}
static void *heap_caps_malloc(size_t n, int caps) {assert(caps & MALLOC_CAP_SPIRAM); return alloc_fail ? NULL : malloc(n);}
#define heap_caps_free free
static int wifi_history_load(wifi_history_t *h) {*h = disk; return load_error;}
static int nvs_store_execute(const char *ns, const nvs_store_op_t *op, int count, int timeout) {
    assert(!strcmp(ns, "wifi_cfg") && count == 1 && timeout == 5000);
    if (write_error) return write_error;
    if (op->kind == NVS_STORE_ERASE_ALL) memset(&disk, 0, sizeof(disk));
    else { assert(op->size == 394); disk = *(const wifi_history_t *)op->value; }
    ++writes; return ESP_OK;
}
'''
    body += function(history, "esp_err_t wifi_history_remember_active(")
    body += function(history, "esp_err_t wifi_history_forget_all(")
    body += r'''
#define SUCCESS 0
#define FAILURE (-1)
#define ESP_LOGV(...) ((void)0)
typedef struct { uint32_t sample; } wifi_ap_record_t;
typedef struct { int resp_event_status; union { struct { uint16_t number; wifi_ap_record_t *out_list; } wifi_scan_ap_list; } u; } ctrl_cmd_t;
static ctrl_cmd_t req, response;
typedef struct { void *(*_h_memset)(void *, int, size_t); void *(*_h_memcpy)(void *, const void *, size_t); } host_funcs_t;
static host_funcs_t funcs = {memset, memcpy};
static struct { host_funcs_t *funcs; } g_h = {&funcs};
static ctrl_cmd_t *RPC_DEFAULT_REQ(void) {memset(&req, 0, sizeof(req)); return &req;}
static ctrl_cmd_t *wifi_scan_get_ap_records(ctrl_cmd_t *r) {assert(r == &req); return &response;}
static int rpc_rsp_callback(ctrl_cmd_t *r) {return r->resp_event_status;}
'''
    body += function(wrapper, "int rpc_wifi_scan_get_ap_records(")
    body += r'''
static void remember(const char *ssid) {
    memset(&s_active, 0, sizeof(s_active));
    snprintf(s_active.ssid, sizeof(s_active.ssid), "%s", ssid);
    strcpy(s_active.password, "fixture-password");
    assert(wifi_history_remember_active() == ESP_OK);
}
int main(void) {
    _Static_assert(sizeof(wifi_history_t) == 394, "history bytes");
    disk.version = 1;
    remember("A"); assert(disk.count == 1 && writes == 1);
    remember("A"); assert(writes == 1);
    remember("B"); remember("C"); remember("D"); remember("E");
    assert(disk.count == 4 && !strcmp(disk.entries[0].ssid, "E") && !strcmp(disk.entries[3].ssid, "B"));
    remember("C"); assert(!strcmp(disk.entries[0].ssid, "C") && !strcmp(disk.entries[1].ssid, "E"));
    strcpy(s_active.password, "changed-password");
    assert(wifi_history_remember_active() == ESP_OK && !strcmp(disk.entries[0].password, "changed-password"));
    int before = writes; load_error = ESP_ERR_INVALID_RESPONSE;
    assert(wifi_history_remember_active() == load_error && writes == before); load_error = 0;
    alloc_fail = true; assert(wifi_history_remember_active() == ESP_ERR_NO_MEM); alloc_fail = false;
    lock_ok = 0; assert(wifi_history_remember_active() == ESP_ERR_TIMEOUT); lock_ok = 1;
    assert(wifi_history_forget_all() == ESP_OK && disk.count == 0 && !s_allow_remember);
    before = writes; remember("late-IP"); assert(disk.count == 0 && writes == before);
    wifi_ap_record_t input[2] = {{42}, {99}}, output[3] = {{0}, {0}, {12345}};
    response.u.wifi_scan_ap_list.out_list = input;
    response.u.wifi_scan_ap_list.number = 2; uint16_t count = 2;
    assert(rpc_wifi_scan_get_ap_records(&count, output) == SUCCESS && count == 2 && output[1].sample == 99 && output[2].sample == 12345);
    response.u.wifi_scan_ap_list.number = 1; count = 2;
    assert(rpc_wifi_scan_get_ap_records(&count, output) == SUCCESS && count == 1 && output[1].sample == 0);
    response.u.wifi_scan_ap_list.number = 0; count = 2;
    assert(rpc_wifi_scan_get_ap_records(&count, output) == SUCCESS && count == 0);
    response.u.wifi_scan_ap_list.number = 3; count = 2;
    assert(rpc_wifi_scan_get_ap_records(&count, output) == ESP_ERR_INVALID_RESPONSE && count == 0 && output[2].sample == 12345);
    response.resp_event_status = SUCCESS; response.u.wifi_scan_ap_list.number = 1;
    response.u.wifi_scan_ap_list.out_list = NULL; count = 2;
    assert(rpc_wifi_scan_get_ap_records(&count, output) == ESP_ERR_INVALID_RESPONSE && count == 0);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="p4-portal-") as directory:
        path = Path(directory)
        source = path / "test.c"
        source.write_text(body, encoding="utf-8")
        binary = path / ("test.exe" if os.name == "nt" else "test")
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run([*compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: successful-history LRU, no reconnect writes, forget ordering, scan count/bounds, portal contracts")


if __name__ == "__main__":
    main()
