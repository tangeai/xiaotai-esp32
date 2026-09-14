#!/usr/bin/env python3
"""Exercise real NVS/HTTP/restart functions with explicit failure boundaries.

The NVS fake models a whole-record update, not physical Flash power loss.
No private device commands, serial access, or real credentials are involved.
"""
from pathlib import Path
import subprocess
import tempfile

from test_captive_portal_contract import function_body

PROJECT = Path(__file__).resolve().parents[1]
source = (PROJECT / "components/wifi_manager/src/wifi_manager.c").read_text(encoding="utf-8")
header = (PROJECT / "components/wifi_manager/include/wifi_manager.h").read_text(encoding="utf-8")
credentials_type = header[header.index("#define WIFI_MANAGER_SSID_MAX"):header.index("/** 初始化")]
record_type = source[source.index('#define WIFI_NVS_NAMESPACE'):source.index('static const char *TAG')]
storage = source[source.index("static void set_error("):source.index("static bool portal_socket_allowed(")]
post = "static esp_err_t wifi_config_post(httpd_req_t *request) {\n" + function_body(source, "wifi_config_post") + "\n}\n"
worker = "static void signal_task(void *context) {\n" + function_body(source, "signal_task") + "\n}\n"

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stddef.h>
#include "cJSON.h"
typedef int esp_err_t;
typedef unsigned nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_RESPONSE -3
#define ESP_ERR_NVS_NOT_FOUND -4
#define ESP_ERR_NVS_INVALID_LENGTH -5
#define ESP_ERR_INVALID_SIZE -6
#define NVS_WORKER_WAIT_MS 5000
typedef esp_err_t (*nvs_worker_fn_t)(void *, size_t);
static bool on_nvs_worker;
static int nvs_worker_call(nvs_worker_fn_t fn, void *data, size_t size, unsigned timeout) {
    _Alignas(max_align_t) unsigned char copy[512] = {0};
    assert(size <= sizeof(copy) && timeout == NVS_WORKER_WAIT_MS);
    if (size) memcpy(copy, data, size);
    on_nvs_worker = true;
    int result = fn(copy, size);
    on_nvs_worker = false;
    if (size) memcpy(data, copy, size);
    return result;
}
#define NVS_READONLY 1
#define NVS_READWRITE 2
#define ESP_LOGE(...) ((void)0)
TYPES
static wifi_credentials_record_t stored;
static size_t stored_size;
static bool has_record, legacy_ssid, legacy_password;
static char old_ssid[33], old_password[65];
static int open_error, read_error, write_error, commit_error;
static unsigned writes, commits, legacy_reads;
static bool write_before_error;
static int nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(on_nvs_worker && !strcmp(name, WIFI_NVS_NAMESPACE) && (mode == 1 || mode == 2));
    if (open_error) return open_error;
    *handle = 1; return ESP_OK;
}
static int nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    assert(handle == 1 && !strcmp(key, WIFI_NVS_CREDENTIALS));
    if (read_error) return read_error;
    if (!has_record) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < stored_size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, &stored, stored_size); *size = stored_size; return ESP_OK;
}
static int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size) {
    assert(handle == 1 && !strcmp(key, WIFI_NVS_CREDENTIALS) && size == 99);
    ++writes;
    if (!write_error || write_before_error) {
        memcpy(&stored, value, size); stored_size = size; has_record = true;
    }
    return write_error;
}
static int nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size) {
    assert(handle == 1); ++legacy_reads;
    bool ssid = !strcmp(key, WIFI_NVS_SSID);
    assert(ssid || !strcmp(key, WIFI_NVS_PASSWORD));
    if (!(ssid ? legacy_ssid : legacy_password)) return ESP_ERR_NVS_NOT_FOUND;
    const char *value = ssid ? old_ssid : old_password;
    if (*size < strlen(value) + 1) return ESP_ERR_NVS_INVALID_LENGTH;
    strcpy(out, value); *size = strlen(value) + 1; return ESP_OK;
}
static int nvs_commit(nvs_handle_t handle) { assert(handle == 1); ++commits; return commit_error; }
static void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
static int nvs_erase_all(nvs_handle_t handle) {
    assert(handle == 1); has_record = legacy_ssid = legacy_password = false; return ESP_OK;
}
STORAGE

typedef struct { int content_len; } httpd_req_t;
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_408_REQ_TIMEOUT 408
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_SOCK_ERR_TIMEOUT -2
static atomic_bool s_provisioning = true, s_restart_requested;
static void *s_signal_task = (void *)1;
static const char *body = "{\"ssid\":\"new-ap\",\"password\":\"new-password\"}";
static size_t received_bytes;
static unsigned success_responses, notifications;
static int send_error;
static int http_status = 200;
static int portal_socket_allowed(int fd) { (void)fd; return atomic_load(&s_provisioning); }
static int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return 1; }
static int httpd_resp_send_err(httpd_req_t *r, int code, const char *text) {
    (void)r; (void)text; http_status = code; return send_error;
}
static void httpd_resp_set_status(httpd_req_t *r, const char *status) {
    (void)r; assert(!strcmp(status, "503 Service Unavailable")); http_status = 503;
}
static int64_t esp_timer_get_time(void) { return 0; }
static int httpd_req_recv(httpd_req_t *r, char *out, size_t length) {
    (void)r;
    if (length > 7) length = 7; /* Fragmented HTTP body. */
    memcpy(out, body + received_bytes, length); received_bytes += length; return (int)length;
}
static void httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; }
static int httpd_resp_sendstr(httpd_req_t *r, const char *text) {
    (void)r; (void)text;
    if (http_status == 200) {
        assert(atomic_load(&s_restart_requested) && notifications == 1);
        ++success_responses;
    }
    return send_error;
}
static void xTaskNotifyGive(void *task) { assert(task == s_signal_task && task); ++notifications; }
POST

typedef struct { int rssi; } wifi_ap_record_t;
static bool s_connected;
static uint32_t s_signal_epoch, s_signal_revision;
static int s_cached_rssi;
#define WIFI_SIGNAL_POLL_MS 30000U
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define taskENTER_CRITICAL(...) ((void)0)
#define taskEXIT_CRITICAL(...) ((void)0)
static jmp_buf worker_done;
static unsigned restarts, delay_ms, poll_ms;
static bool inject_restart;
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) {
    ap->rssi = -50; return ESP_OK;
}
static void vTaskDelay(unsigned ticks) { delay_ms = ticks; }
static void esp_restart(void) { ++restarts; longjmp(worker_done, 1); }
static unsigned ulTaskNotifyTake(int clear, unsigned ticks) {
    assert(clear == pdTRUE); poll_ms = ticks;
    if (inject_restart) { atomic_store(&s_restart_requested, true); return 1; }
    longjmp(worker_done, 2);
}
WORKER

static void reset(void) {
    memset(&stored, 0, sizeof(stored)); stored_size = 0; has_record = false;
    legacy_ssid = legacy_password = true;
    strcpy(old_ssid, "old-ap"); strcpy(old_password, "old-password");
    open_error = read_error = write_error = commit_error = send_error = 0;
    writes = commits = legacy_reads = success_responses = notifications = 0;
    write_before_error = false;
    atomic_store(&s_restart_requested, false); atomic_store(&s_provisioning, true);
    s_signal_task = (void *)1;
    restarts = delay_ms = poll_ms = 0; inject_restart = s_connected = false;
    s_signal_epoch = s_signal_revision = 0; s_cached_rssi = 1;
}
static void check_pair(const char *ssid, const char *password) {
    wifi_manager_credentials_t out;
    assert(wifi_manager_load_credentials(&out) == ESP_OK);
    assert(!strcmp(out.ssid, ssid) && !strcmp(out.password, password));
}
static void check_bad(int expected) {
    wifi_manager_credentials_t out, empty = {0}; memset(&out, 0xaa, sizeof(out));
    assert(wifi_manager_load_credentials(&out) == expected);
    assert(!memcmp(&out, &empty, sizeof(out)));
}
static int request(void) {
    received_bytes = 0; http_status = 200;
    httpd_req_t req = {.content_len = (int)strlen(body)};
    return wifi_config_post(&req);
}
static void run_worker(void) { if (setjmp(worker_done) == 0) signal_task(NULL); }
int main(void) {
    reset(); check_pair("old-ap", "old-password"); assert(writes == 0);
    assert(wifi_manager_load_credentials(NULL) == ESP_ERR_INVALID_ARG);
    legacy_password = false; check_bad(ESP_ERR_NVS_NOT_FOUND); legacy_password = true;
    old_password[0] = '\0'; check_pair("old-ap", ""); strcpy(old_password, "old-password");
    open_error = -10; check_bad(-10); assert(wifi_manager_save_credentials("new-ap", "new-password") == -10);
    open_error = 0;
    assert(wifi_manager_save_credentials("", "new-password") == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_save_credentials("new-ap", "short") == ESP_ERR_INVALID_ARG && writes == 0);

    /* A failed first migration keeps the entire legacy pair, not new SSID/old password. */
    write_error = -11;
    assert(wifi_manager_save_credentials("new-ap", "new-password") == -11 && commits == 0);
    check_pair("old-ap", "old-password");
    write_error = 0;
    assert(wifi_manager_save_credentials("new-ap", "new-password") == ESP_OK && commits == 1);
    legacy_reads = 0; check_pair("new-ap", "new-password"); assert(legacy_reads == 0);
    assert(!strcmp(old_ssid, "old-ap") && !strcmp(old_password, "old-password"));
    write_error = -12;
    assert(wifi_manager_save_credentials("next-ap", "next-password") == -12);
    check_pair("new-ap", "new-password");
    /* NVS errors after persistence may expose the new WHOLE pair. No rollback claim. */
    write_before_error = true;
    assert(wifi_manager_save_credentials("next-ap", "next-password") == -12);
    check_pair("next-ap", "next-password");
    write_error = 0; commit_error = -13;
    assert(wifi_manager_save_credentials("last-ap", "last-password") == -13);
    check_pair("last-ap", "last-password"); commit_error = 0;
    read_error = -14; check_bad(-14); read_error = 0;
    stored_size = 98; check_bad(ESP_ERR_INVALID_RESPONSE); stored_size = 100;
    check_bad(ESP_ERR_NVS_INVALID_LENGTH); stored_size = 99;
    stored.version = 2; check_bad(ESP_ERR_INVALID_RESPONSE); stored.version = 1;
    memset(stored.credentials.ssid, 'x', sizeof(stored.credentials.ssid)); check_bad(ESP_ERR_INVALID_RESPONSE);
    strcpy(stored.credentials.ssid, "valid");
    memset(stored.credentials.password, 'x', sizeof(stored.credentials.password)); check_bad(ESP_ERR_INVALID_RESPONSE);
    char max_ssid[33], max_password[65]; memset(max_ssid, 's', 32); max_ssid[32] = 0;
    memset(max_password, 'a', 64); max_password[64] = 0;
    assert(wifi_manager_save_credentials(max_ssid, max_password) == ESP_OK); check_pair(max_ssid, max_password);
    assert(wifi_manager_save_credentials("open-ap", "") == ESP_OK); check_pair("open-ap", "");
    assert(wifi_manager_forget_credentials() == ESP_OK); check_bad(ESP_ERR_NVS_NOT_FOUND);
    puts("PASS: Wi-Fi whole-record save, legacy reads/migration, write/commit failures, corrupt/version/length rejection, open/max credentials, forget");

    reset(); s_signal_task = NULL;
    assert(request() == ESP_OK && http_status == 503 && writes == 0 && success_responses == 0 && notifications == 0);
    reset(); write_error = -11;
    assert(request() == ESP_OK && http_status == 500 && success_responses == 0 && notifications == 0 && !s_restart_requested);
    reset(); commit_error = -13;
    assert(request() == ESP_OK && http_status == 500 && success_responses == 0 && notifications == 0 && !s_restart_requested);
    reset();
    assert(request() == ESP_OK && http_status == 200 && success_responses == 1 && writes == 1 && notifications == 1);
    assert(request() == ESP_OK && http_status == 503 && success_responses == 1 && writes == 1 && notifications == 1);
    run_worker(); assert(restarts == 1 && delay_ms == 1000 && poll_ms == 0);
    reset(); send_error = -15;
    assert(request() == -15); run_worker(); assert(restarts == 1); check_pair("new-ap", "new-password");
    reset(); atomic_store(&s_provisioning, false);
    assert(request() == ESP_OK && http_status == 403 && writes == 0 && notifications == 0);
    reset(); run_worker(); assert(restarts == 0 && delay_ms == 0 && poll_ms == 30000);
    reset(); s_connected = true; inject_restart = true;
    run_worker(); assert(s_cached_rssi == -50 && s_signal_revision == 1 && restarts == 1 && delay_ms == 1000);
    puts("PASS: HTTP rejects unavailable owner/pending restart/storage errors; accepted save survives response loss; existing worker restarts and RSSI polling remains intact");
}
'''
harness = harness.replace("TYPES", credentials_type + record_type).replace("STORAGE", storage)
harness = harness.replace("\nPOST\n", "\n" + post + "\n").replace("\nWORKER\n", "\n" + worker + "\n")
assert "xTaskCreate(" not in post and "restart_task(" not in source
cjson = PROJECT / "managed_components/espressif__cjson/cJSON"
if not (cjson / "cJSON.c").is_file():
    raise SystemExit("Missing managed cJSON: run idf.py reconfigure in the ESP-IDF environment first")
with tempfile.TemporaryDirectory(prefix="xiaotai-wifi-credentials-") as folder:
    folder = Path(folder)
    c = folder / "test.c"
    binary = folder / "test"
    c.write_text(harness, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", str(cjson),
                    str(c), str(cjson / "cJSON.c"), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
