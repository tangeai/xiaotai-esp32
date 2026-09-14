#!/usr/bin/env python3
"""Exercise real portal/scan/owner code with a fake radio, clock and NVS snapshot.

No device, real network, firmware build or credentials are needed. This checks
ownership and wire JSON, not physical radio timing or browser rendering.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
wifi = (ROOT / "components/wifi_manager/src/wifi_manager.c").read_text(encoding="utf-8")
portal = (ROOT / "components/wifi_manager/src/wifi_portal.inc").read_text(encoding="utf-8")
owner = wifi[wifi.index("static void retry_timer_callback("):wifi.index("static esp_err_t wifi_control_post(")]

test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_WIFI_STATE -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_WIFI_NOT_CONNECT -4
#define ESP_ERR_NO_MEM -5
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define EXT_RAM_BSS_ATTR
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define WIFI_PROVISION_AFTER_FAILURES 5U
#define WIFI_MANAGER_CONTROL 123
#define WIFI_SCAN_TYPE_ACTIVE 0
enum { WIFI_CONTROL_DISCONNECT, WIFI_CONTROL_RETRY, WIFI_CONTROL_SCAN };
enum { WIFI_AUTH_OPEN, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK,
       WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_WPA3_PSK,
       WIFI_AUTH_WPA2_ENTERPRISE };
typedef int wifi_auth_mode_t;
typedef struct { uint8_t ssid[33]; int8_t rssi; wifi_auth_mode_t authmode; } wifi_ap_record_t;
typedef struct { unsigned status; } wifi_event_sta_scan_done_t;
typedef struct { bool show_hidden; int scan_type; struct { struct { unsigned min, max; } active; } scan_time; } wifi_scan_config_t;
typedef struct { size_t content_len; } httpd_req_t;
typedef struct { char ssid[33], password[65]; } wifi_manager_credentials_t;
typedef struct { uint8_t version, count; wifi_manager_credentials_t entries[5]; } wifi_history_t;
static wifi_history_t fixture_history;
static int history_error;
static bool close_during_history;
static atomic_bool s_provisioning = true, s_manual_disconnect, s_control_pending;
static atomic_int s_control_error;
static bool s_connected, s_connecting, s_has_saved_credentials, s_connection_failed;
static uint32_t s_retry_count;
static int64_t now = 1000, s_retry_due_us, s_connect_started_us, s_associated_us;
static void *s_retry_timer = (void *)1;
static char s_provisioning_status[128];
static unsigned connects, disconnects, starts, stops, clears, queries, posts;
static int queued, scan_error, get_error, post_error, timer_error;
static bool timer_running;
static wifi_ap_record_t radio[32];
static uint16_t radio_count;
static char response[16384];
static size_t response_size;
static int http_status, chunk_error;
static bool socket_allowed = true;
static int64_t esp_timer_get_time(void) { return now; }
static int esp_timer_start_periodic(void *timer, unsigned period) {
    assert(timer && period == 1000000);
    if (timer_error) return timer_error;
    if (timer_running) return ESP_ERR_INVALID_STATE;
    timer_running = true; return 0;
}
static int esp_timer_stop(void *timer) { assert(timer); timer_running = false; return 0; }
static int esp_wifi_scan_start(const wifi_scan_config_t *config, bool block) {
    assert(!block && config->scan_type == WIFI_SCAN_TYPE_ACTIVE && !config->show_hidden);
    assert(config->scan_time.active.max == 120);
    ++starts; return scan_error;
}
static int esp_wifi_scan_stop(void) { ++stops; return 0; }
static int esp_wifi_clear_ap_list(void) { ++clears; return 0; }
static int esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records) {
    ++queries; assert(*count == 24);
    if (get_error) return get_error;
    if (*count > radio_count) *count = radio_count;
    memcpy(records, radio, *count * sizeof(*records)); return 0;
}
static int esp_event_post(int base, int event, void *data, size_t size, unsigned wait) {
    assert(base == WIFI_MANAGER_CONTROL && !data && !size && !wait);
    ++posts; if (post_error) return post_error;
    queued = event; return 0;
}
static int esp_wifi_connect(void) { ++connects; return 0; }
static int esp_wifi_disconnect(void) { ++disconnects; return 0; }
static int start_provisioning(void) { atomic_store(&s_provisioning, true); return 0; }
static void signal_connection_changed(void) {}
static int wifi_history_load(wifi_history_t *history) {
    memset(history, 0, sizeof(*history));
    if (close_during_history) atomic_store(&s_provisioning, false);
    if (!history_error) *history = fixture_history;
    return history_error;
}
static bool portal_socket_allowed(int fd) {
    assert(fd == 1); return socket_allowed && atomic_load(&s_provisioning);
}
static int httpd_req_to_sockfd(httpd_req_t *request) { (void)request; return 1; }
static int httpd_resp_send_err(httpd_req_t *r, int code, const char *text) {
    (void)r; http_status = code; snprintf(response, sizeof(response), "%s", text); return 0;
}
static void httpd_resp_set_type(httpd_req_t *r, const char *type) {
    (void)r; assert(strstr(type, "application/json"));
}
static void httpd_resp_set_hdr(httpd_req_t *r, const char *key, const char *value) {
    (void)r; assert(!strcmp(key, "Cache-Control") && !strcmp(value, "no-store"));
}
static int httpd_resp_send_chunk(httpd_req_t *r, const char *text, size_t bytes) {
    (void)r; if (chunk_error) return chunk_error;
    assert(response_size + bytes < sizeof(response));
    if (bytes) memcpy(response + response_size, text, bytes);
    response_size += bytes; response[response_size] = 0; return 0;
}
static int httpd_resp_sendstr_chunk(httpd_req_t *r, const char *text) {
    return httpd_resp_send_chunk(r, text, strlen(text));
}
static int httpd_resp_sendstr(httpd_req_t *r, const char *text) { return httpd_resp_sendstr_chunk(r, text); }
PORTAL
OWNER
static void reset_response(void) { response_size = 0; response[0] = 0; http_status = 200; }
static int scan_request(void) {
    reset_response(); httpd_req_t r = {0}; return portal_scan_post(&r);
}
static void begin(void) {
    now += 6000000; assert(scan_request() == 0 && http_status == 200);
    wifi_control_event(queued);
}
static void done(unsigned status) {
    wifi_event_sta_scan_done_t event = {.status = status}; portal_scan_done(&event);
}
static cJSON *networks(void) {
    reset_response(); httpd_req_t r = {0}; assert(portal_networks_get(&r) == 0);
    assert(http_status == 200 && !strstr(response, "fixture-secret"));
    cJSON *json = cJSON_Parse(response); assert(json); return json;
}
int main(void) {
    assert(!strcmp(portal_scan_status(PORTAL_SCAN_IDLE), "idle"));
    assert(scan_request() == 0 && posts == 1 && queued == WIFI_CONTROL_SCAN);
    assert(scan_request() == 0 && posts == 1); /* Repeated requests coalesce. */
    wifi_control_event(queued); assert(starts == 1 && s_scan_active && timer_running);
    cJSON *json = networks(); assert(!strcmp(cJSON_GetObjectItem(json, "state")->valuestring, "scanning")); cJSON_Delete(json);
    s_has_saved_credentials = true; connect_saved_network(); assert(!connects);
    /* A manual disconnect during a scan must preserve the existing watchdog. */
    wifi_control_event(WIFI_CONTROL_DISCONNECT);
    assert(timer_running && s_manual_disconnect && s_scan_active);
    now += 4999999; wifi_control_event(WIFI_CONTROL_RETRY); assert(s_scan_active);
    now += 1; wifi_control_event(WIFI_CONTROL_RETRY);
    assert(!s_scan_active && !timer_running && stops == 1 && clears == 1 && !connects);
    assert(atomic_load(&s_scan_state) == PORTAL_SCAN_ERROR);
    unsigned before_cancel_drain = starts;
    begin(); assert(starts == before_cancel_drain && s_scan_cancel_pending);
    done(0); assert(queries == 0); /* Canceled scan cannot publish late results. */
    assert(!s_scan_cancel_pending);
    atomic_store(&s_manual_disconnect, false);
    s_connecting = true; begin(); assert(!s_scan_active && starts == 1);
    assert(atomic_load(&s_scan_state) == PORTAL_SCAN_BUSY);
    s_connecting = false; s_connected = true; begin(); assert(starts == 1);
    s_connected = false;
    scan_error = ESP_ERR_WIFI_STATE; begin(); assert(!s_scan_active && !timer_running);
    assert(atomic_load(&s_scan_state) == PORTAL_SCAN_BUSY); scan_error = 0;
    timer_error = -44; begin(); assert(!s_scan_active && stops == 2 && clears == 2); timer_error = 0; done(1);
    unsigned attempts = starts;
    assert(scan_request() == 0); wifi_control_event(queued); assert(starts == attempts); /* Cooldown. */
    post_error = -45; scan_request(); assert(http_status == 500 && s_scan_state == PORTAL_SCAN_ERROR); post_error = 0;

    fixture_history.version = 1; fixture_history.count = 2;
    strcpy(fixture_history.entries[0].ssid, "home"); strcpy(fixture_history.entries[0].password, "fixture-secret");
    strcpy(fixture_history.entries[1].ssid, "offline"); strcpy(fixture_history.entries[1].password, "fixture-secret-2");
    strcpy((char *)radio[0].ssid, "home"); radio[0].rssi = -40; radio[0].authmode = WIFI_AUTH_WPA2_PSK;
    radio[1] = radio[0]; radio[1].rssi = -60;
    radio[2] = radio[0]; radio[2].authmode = WIFI_AUTH_OPEN;
    strcpy((char *)radio[3].ssid, "\xe4\xb8\xad\xe6\x96\x87\"\\"); radio[3].rssi = -70;
    memset(radio[4].ssid, 'A', 32); radio[4].authmode = WIFI_AUTH_WEP;
    radio[5].ssid[0] = 0; radio_count = 6;
    begin(); assert(s_scan_active);
    s_retry_due_us = now + 1000; now += 2000;
    done(0); assert(!s_scan_active && !timer_running && connects == 1 && s_connecting);
    assert(queries == 1 && s_scan_state == PORTAL_SCAN_READY);
    json = networks();
    cJSON *list = cJSON_GetObjectItem(json, "networks"); assert(cJSON_GetArraySize(list) == 5);
    cJSON *item = cJSON_GetArrayItem(list, 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(item, "saved")));
    assert(cJSON_GetObjectItem(item, "rssi")->valueint == -40);
    assert(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(list, 1), "saved"))); /* Open lookalike. */
    item = cJSON_GetArrayItem(list, 2); char decoded[33];
    assert(portal_decode_ssid(cJSON_GetObjectItem(item, "ssid_hex")->valuestring, decoded));
    assert(!strcmp(decoded, (char *)radio[3].ssid));
    item = cJSON_GetArrayItem(list, 3);
    assert(strlen(cJSON_GetObjectItem(item, "ssid")->valuestring) == 32);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(item, "supported")));
    item = cJSON_GetArrayItem(list, 4);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(item, "available")) && cJSON_IsNull(cJSON_GetObjectItem(item, "rssi")));
    cJSON_Delete(json);
    history_error = -46; json = networks();
    assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "history_error")));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(json, "networks")) == 4); cJSON_Delete(json); history_error = 0;
    s_connecting = false; begin(); get_error = -47; done(0);
    assert(!s_scan_count && s_scan_state == PORTAL_SCAN_ERROR && clears == 3); get_error = 0;
    begin(); done(1); assert(clears == 4 && !s_scan_active);
    begin(); radio_count = 32; done(0); assert(s_scan_count == 24);
    begin(); portal_scan_cancel(PORTAL_SCAN_IDLE); done(0); assert(!s_scan_active && !timer_running);

    socket_allowed = false; scan_request(); assert(http_status == 403);
    reset_response(); httpd_req_t r = {0}; assert(portal_networks_get(&r) == 0 && http_status == 403);
    socket_allowed = true; r.content_len = 1; reset_response();
    assert(portal_scan_post(&r) == 0 && http_status == 400);
    r.content_len = 0; close_during_history = true; reset_response();
    assert(portal_networks_get(&r) == 0 && http_status == 403); close_during_history = false;
    atomic_store(&s_provisioning, true); chunk_error = -48; reset_response();
    assert(portal_networks_get(&r) == -48);
    puts("PASS: asynchronous scan ownership, connection priority, manual-disconnect watchdog, timeout/late completion, retry continuity, driver/queue failures");
    puts("PASS: bounded/deduplicated JSON, exact/escaped SSID, saved/open isolation, history read failure, AP-only boundary, send failure, no password disclosure");
}
'''
test = test.replace("\nPORTAL\n", "\n" + portal + "\n").replace("\nOWNER\n", "\n" + owner + "\n")
cjson = ROOT / "managed_components/espressif__cjson/cJSON"
with tempfile.TemporaryDirectory(prefix="xiaotai-portal-") as folder:
    folder = Path(folder)
    path = folder / "test.c"
    path.write_text(test, encoding="utf-8")
    binary = folder / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-fsanitize=address,undefined", "-g", "-I", str(cjson),
                    str(path), str(cjson / "cJSON.c"), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
