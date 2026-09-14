/*
 * Wi-Fi STA 与 SoftAP 配网 adapter。
 *
 * 启动时优先读取 NVS 并连接 STA；没有配置或连续连接失败时开启 APSTA，提供
 * 一个最小配置页。网页只保存配置并重启，连接状态仍由同一事件处理路径建立。
 * 实时媒体要求关闭 Wi-Fi power save，避免省电唤醒引入接收延迟。
 */
#include "wifi_manager.h"
#include "captive_dns.h"
#include "wifi_history.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_store.h"

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID "ssid"
#define WIFI_NVS_PASSWORD "password"
#define WIFI_NVS_CREDENTIALS "credentials"
#define WIFI_CREDENTIALS_VERSION 1U
#define WIFI_PROVISION_AFTER_FAILURES 5U
#define WIFI_SETUP_URL "http://192.168.6.1"

typedef struct {
    uint8_t version;
    wifi_manager_credentials_t credentials;
} wifi_credentials_record_t;
_Static_assert(sizeof(wifi_credentials_record_t) == 99, "Wi-Fi NVS record layout changed");

static const char *TAG = "wifi_manager";
static bool s_started;
static volatile bool s_connected;
ESP_EVENT_DEFINE_BASE(WIFI_MANAGER_CONTROL);
enum { WIFI_CONTROL_DISCONNECT, WIFI_CONTROL_RETRY, WIFI_CONTROL_SCAN };
static atomic_bool s_manual_disconnect;
static atomic_bool s_control_pending;
static atomic_int s_control_error;
/* RSSI is informational. Hosted queries may block on C6 RPC, so only this
 * background worker reads the hardware; UI consumers read a cached snapshot. */
#define WIFI_SIGNAL_POLL_MS 30000U
static TaskHandle_t s_signal_task;
/* This internal-stack worker also owns the one-shot provisioning reboot.
 * No task/stack allocation is allowed after promising to apply saved settings. */
static atomic_bool s_restart_requested;
static atomic_bool s_history_pending;
static portMUX_TYPE s_signal_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_signal_epoch;
static uint32_t s_signal_revision;
static int s_cached_rssi = 1; /* positive sentinel: no sample */

static void signal_connection_changed(void)
{
    taskENTER_CRITICAL(&s_signal_lock);
    ++s_signal_epoch;
    ++s_signal_revision;
    s_cached_rssi = 1;
    taskEXIT_CRITICAL(&s_signal_lock);
    if (s_signal_task != NULL) xTaskNotifyGive(s_signal_task);
}

static void signal_task(void *context)
{
    (void)context;
    for (;;) {
        if (atomic_load(&s_restart_requested)) {
            /* Let HTTPD finish its response without blocking the event loop. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        if (atomic_exchange(&s_history_pending, false)) {
            esp_err_t err = wifi_history_remember_active();
            if (err != ESP_OK)
                ESP_LOGW(TAG, "Wi-Fi history save failed: %s", esp_err_to_name(err));
        }
        taskENTER_CRITICAL(&s_signal_lock);
        uint32_t epoch = s_signal_epoch;
        taskEXIT_CRITICAL(&s_signal_lock);
        wifi_ap_record_t access_point = {0};
        if (s_connected && esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
            taskENTER_CRITICAL(&s_signal_lock);
            if (s_connected && epoch == s_signal_epoch && s_cached_rssi != access_point.rssi) {
                s_cached_rssi = access_point.rssi;
                ++s_signal_revision;
            }
            taskEXIT_CRITICAL(&s_signal_lock);
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WIFI_SIGNAL_POLL_MS));
    }
}
static atomic_bool s_provisioning;
static volatile bool s_connection_failed;
static bool s_has_saved_credentials;
/* Retry state is owned only by the default event loop, not the timer or UI. */
static uint32_t s_retry_count;
static int64_t s_retry_due_us;
static int64_t s_connect_started_us;
static int64_t s_associated_us;
static esp_timer_handle_t s_retry_timer;
static bool s_connecting;
static char s_provisioning_ssid[33];
static char s_provisioning_status[96];
static httpd_handle_t s_http_server;
static esp_netif_t *s_ap_netif;

/* 页面内嵌在固件中，避免模板依赖额外文件系统分区。 */
extern const char s_setup_page[] asm("_binary_setup_html_start");

#define WIFI_PORTAL_SCAN_MAX 24U
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secured;
    bool supported;
} portal_network_t;
typedef enum { SCAN_IDLE, SCAN_QUEUED, SCAN_RUNNING, SCAN_READY, SCAN_ERROR } portal_scan_state_t;
typedef struct {
    portal_scan_state_t state;
    esp_err_t error;
    uint16_t count;
    bool truncated;
    portal_network_t networks[WIFI_PORTAL_SCAN_MAX];
} portal_scan_t;
static EXT_RAM_BSS_ATTR portal_scan_t s_scan;
static portMUX_TYPE s_scan_lock = portMUX_INITIALIZER_UNLOCKED;
/* Driver operations and scan lifetime belong to the default event loop. HTTP
 * only queues intent and copies metadata; it never blocks on a Hosted scan. */
static bool s_scan_active;
static bool s_scan_draining;
static int64_t s_scan_deadline_us;
static int64_t s_scan_request_us;
static void portal_scan_finish(esp_err_t result);

static void portal_scan_status(portal_scan_state_t state, esp_err_t error)
{
    taskENTER_CRITICAL(&s_scan_lock);
    s_scan.state = state;
    s_scan.error = error;
    taskEXIT_CRITICAL(&s_scan_lock);
}

static void portal_scan_start(void)
{
    if (!atomic_load(&s_provisioning) || s_scan_active || s_scan_draining ||
        atomic_load(&s_restart_requested)) {
        portal_scan_status(SCAN_ERROR, ESP_ERR_INVALID_STATE);
        return;
    }
    if (s_connected || s_connecting) {
        portal_scan_status(SCAN_ERROR, ESP_ERR_WIFI_STATE);
        return;
    }
    const wifi_scan_config_t config = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err != ESP_OK) {
        portal_scan_status(SCAN_ERROR, err);
        return;
    }
    s_scan_active = true;
    s_scan_deadline_us = esp_timer_get_time() + 10000000;
    portal_scan_status(SCAN_RUNNING, ESP_OK);
    err = esp_timer_start_periodic(s_retry_timer, 1000000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        (void)esp_wifi_scan_stop();
        s_scan_draining = true;
        portal_scan_finish(err);
    }
}

static bool portal_auth_supported(wifi_auth_mode_t auth)
{
    return auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WPA2_PSK ||
           auth == WIFI_AUTH_WPA_WPA2_PSK || auth == WIFI_AUTH_WPA3_PSK ||
           auth == WIFI_AUTH_WPA2_WPA3_PSK;
}

static void portal_scan_finish(esp_err_t result)
{
    wifi_ap_record_t *records = NULL;
    uint16_t count = WIFI_PORTAL_SCAN_MAX;
    uint16_t total = 0;
    if (result == ESP_OK) {
        records = heap_caps_calloc(count, sizeof(*records), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (records == NULL) result = ESP_ERR_NO_MEM;
        if (result == ESP_OK) result = esp_wifi_scan_get_ap_num(&total);
        if (result == ESP_OK) result = esp_wifi_scan_get_ap_records(&count, records);
    }
    /* The driver owns a result list after SCAN_DONE, including failed/cancelled
     * scans. Always release it; do not leave C6 scan memory accumulated. */
    esp_err_t clear_err = esp_wifi_clear_ap_list();
    if (result == ESP_OK) result = clear_err;
    taskENTER_CRITICAL(&s_scan_lock);
    if (result == ESP_OK) {
        s_scan.count = 0;
        s_scan.truncated = total > WIFI_PORTAL_SCAN_MAX;
        for (size_t i = 0; i < count; ++i) {
            records[i].ssid[32] = '\0';
            if (records[i].ssid[0] == '\0' ||
                strcmp((char *)records[i].ssid, s_provisioning_ssid) == 0) continue;
            size_t j = 0;
            while (j < s_scan.count && strcmp(s_scan.networks[j].ssid, (char *)records[i].ssid) != 0)
                ++j;
            if (j < s_scan.count && s_scan.networks[j].rssi >= records[i].rssi) continue;
            if (j == s_scan.count) ++s_scan.count;
            memcpy(s_scan.networks[j].ssid, records[i].ssid, 33);
            s_scan.networks[j].rssi = records[i].rssi;
            s_scan.networks[j].secured = records[i].authmode != WIFI_AUTH_OPEN;
            s_scan.networks[j].supported = portal_auth_supported(records[i].authmode);
        }
    }
    s_scan.state = result == ESP_OK ? SCAN_READY : SCAN_ERROR;
    s_scan.error = result;
    taskEXIT_CRITICAL(&s_scan_lock);
    heap_caps_free(records);
    s_scan_active = false;
    if (s_retry_due_us == 0) (void)esp_timer_stop(s_retry_timer);
    if (result != ESP_OK) ESP_LOGW(TAG, "portal scan failed: %s", esp_err_to_name(result));
}

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

bool wifi_manager_credentials_valid(const char *ssid,
                                    const char *password,
                                    char *error,
                                    size_t error_size)
{
    if (ssid == NULL || password == NULL) {
        set_error(error, error_size, "SSID/password is null");
        return false;
    }
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > WIFI_MANAGER_SSID_MAX) {
        set_error(error, error_size, "SSID length must be 1..32 bytes");
        return false;
    }
    if (password_length != 0 && (password_length < 8 ||
                                 password_length > WIFI_MANAGER_PASSWORD_MAX)) {
        set_error(error, error_size, "password length must be 0 or 8..64 bytes");
        return false;
    }
    set_error(error, error_size, "");
    return true;
}

esp_err_t wifi_manager_load_credentials(wifi_manager_credentials_t *credentials)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(credentials, 0, sizeof(*credentials));
    wifi_credentials_record_t record = {0};
    size_t record_size = sizeof(record);
    esp_err_t err = nvs_store_read(WIFI_NVS_NAMESPACE, WIFI_NVS_CREDENTIALS,
        NVS_STORE_GET_BLOB, &record, &record_size, 5000);
    if (err == ESP_OK) {
        if (record_size != sizeof(record) || record.version != WIFI_CREDENTIALS_VERSION) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            *credentials = record.credentials;
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Read old firmware's pair only when no new record exists. Never hide
         * a corrupt/newer record by silently reviving stale legacy settings. */
        size_t ssid_size = sizeof(credentials->ssid);
        size_t password_size = sizeof(credentials->password);
        err = nvs_store_read(WIFI_NVS_NAMESPACE, WIFI_NVS_SSID,
            NVS_STORE_GET_STRING, credentials->ssid, &ssid_size, 5000);
        if (err == ESP_OK) {
            err = nvs_store_read(WIFI_NVS_NAMESPACE, WIFI_NVS_PASSWORD,
                NVS_STORE_GET_STRING, credentials->password, &password_size, 5000);
        }
    }
    if (err == ESP_OK &&
        (memchr(credentials->ssid, '\0', sizeof(credentials->ssid)) == NULL ||
         memchr(credentials->password, '\0', sizeof(credentials->password)) == NULL ||
         !wifi_manager_credentials_valid(credentials->ssid, credentials->password, NULL, 0))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        memset(credentials, 0, sizeof(*credentials));
    }
    return err;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    char validation_error[80];
    if (!wifi_manager_credentials_valid(ssid,
                                        password,
                                        validation_error,
                                        sizeof(validation_error))) {
        ESP_LOGE(TAG, "invalid Wi-Fi credentials: %s", validation_error);
        return ESP_ERR_INVALID_ARG;
    }

    wifi_credentials_record_t record = {.version = WIFI_CREDENTIALS_VERSION};
    memcpy(record.credentials.ssid, ssid, strlen(ssid) + 1);
    memcpy(record.credentials.password, password, strlen(password) + 1);
    /* One versioned blob still owns the SSID/password pair. The portal must
     * confirm actual persistence before accepting configuration or restarting. */
    const nvs_store_op_t op = {NVS_STORE_BLOB, WIFI_NVS_CREDENTIALS, &record, sizeof(record)};
    return nvs_store_execute(WIFI_NVS_NAMESPACE, &op, 1, 5000);
}

esp_err_t wifi_manager_forget_credentials(void)
{
    return wifi_history_forget_all();
}

static bool portal_socket_allowed(int socket_fd)
{
    if (!atomic_load(&s_provisioning) || s_ap_netif == NULL) return false;
    struct sockaddr_storage local = {0};
    socklen_t size = sizeof(local);
    esp_netif_ip_info_t ap_ip = {0};
    if (getsockname(socket_fd, (struct sockaddr *)&local, &size) != 0 ||
        esp_netif_get_ip_info(s_ap_netif, &ap_ip) != ESP_OK) return false;
    uint32_t local_ipv4 = 0;
    if (local.ss_family == AF_INET && size >= sizeof(struct sockaddr_in)) {
        local_ipv4 = ((const struct sockaddr_in *)&local)->sin_addr.s_addr;
#if CONFIG_LWIP_IPV6
    } else if (local.ss_family == AF_INET6 && size >= sizeof(struct sockaddr_in6)) {
        /* HTTPD uses a dual-stack listener. IPv4 clients then have mapped IPv6
         * local addresses; keep the same AP-only boundary after normalization. */
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&local;
        if (!IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) return false;
        memcpy(&local_ipv4, &v6->sin6_addr.s6_addr[12], sizeof(local_ipv4));
#endif
    } else {
        return false;
    }
    return ap_ip.ip.addr != 0 && local_ipv4 == ap_ip.ip.addr;
}

static esp_err_t portal_open(httpd_handle_t server, int socket_fd)
{
    (void)server;
    /* HTTPD listens on all interfaces; never accept provisioning via STA. */
    return portal_socket_allowed(socket_fd) ? ESP_OK : ESP_FAIL;
}

static esp_err_t setup_page_get(httpd_req_t *request)
{
    if (!portal_socket_allowed(httpd_req_to_sockfd(request)))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "provisioning closed");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(request, s_setup_page, HTTPD_RESP_USE_STRLEN);
}

static bool portal_write_allowed(httpd_req_t *request)
{
    if (!portal_socket_allowed(httpd_req_to_sockfd(request))) return false;
    size_t length = httpd_req_get_hdr_value_len(request, "Origin");
    if (length == 0) return true;
    char origin[64];
    return length < sizeof(origin) &&
           httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) == ESP_OK &&
           strcmp(origin, WIFI_SETUP_URL) == 0;
}

static esp_err_t portal_scan_post(httpd_req_t *request)
{
    if (!portal_write_allowed(request))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "provisioning closed or invalid origin");
    if (request->content_len != 0)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "empty request required");
    int64_t now = esp_timer_get_time();
    bool enqueue = false;
    taskENTER_CRITICAL(&s_scan_lock);
    if (s_scan.state != SCAN_QUEUED && s_scan.state != SCAN_RUNNING &&
        (s_scan_request_us == 0 || now - s_scan_request_us >= 5000000)) {
        s_scan.state = SCAN_QUEUED;
        s_scan.error = ESP_OK;
        s_scan_request_us = now;
        enqueue = true;
    }
    taskEXIT_CRITICAL(&s_scan_lock);
    if (enqueue) {
        esp_err_t err = esp_event_post(WIFI_MANAGER_CONTROL, WIFI_CONTROL_SCAN, NULL, 0, 0);
        if (err != ESP_OK) {
            portal_scan_status(SCAN_ERROR, err);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "scan queue unavailable");
        }
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

/* Serialize one entry at a time: cJSON provides escaping, while the large
 * response buffer is explicitly PSRAM and temporary cJSON nodes stay bounded. */
static bool portal_append_network(char *output, size_t capacity, size_t *used,
                                  const char *ssid, int rssi, bool available,
                                  bool secured, bool saved, bool supported, bool comma)
{
    cJSON *entry = cJSON_CreateObject();
    bool ok = entry != NULL && cJSON_AddStringToObject(entry, "ssid", ssid) &&
        cJSON_AddNumberToObject(entry, "rssi", rssi) &&
        cJSON_AddBoolToObject(entry, "available", available) &&
        cJSON_AddBoolToObject(entry, "secured", secured) &&
        cJSON_AddBoolToObject(entry, "saved", saved) &&
        cJSON_AddBoolToObject(entry, "supported", supported);
    if (ok && capacity - *used > 2) {
        if (comma) output[(*used)++] = ',';
        ok = cJSON_PrintPreallocated(entry, output + *used, (int)(capacity - *used), false);
        if (ok) *used += strlen(output + *used);
    } else {
        ok = false;
    }
    cJSON_Delete(entry);
    return ok;
}

static esp_err_t portal_networks_get(httpd_req_t *request)
{
    if (!portal_socket_allowed(httpd_req_to_sockfd(request)))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "provisioning closed");
    /* HTTPD already uses a PSRAM stack. Never execute an on-demand radio RPC
     * here: a stalled C6 must not hold up the page or its password submission. */
    portal_scan_t snapshot;
    taskENTER_CRITICAL(&s_scan_lock);
    snapshot = s_scan;
    taskEXIT_CRITICAL(&s_scan_lock);
    wifi_history_t history;
    esp_err_t history_err = wifi_history_load(&history);
    if (history_err != ESP_OK) memset(&history, 0, sizeof(history));
    static const char *states[] = {"idle", "queued", "scanning", "ready", "error"};
    const size_t capacity = 12288;
    char *output = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (output == NULL) {
        memset(&history, 0, sizeof(history));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no response memory");
    }
    size_t used = (size_t)snprintf(output, capacity,
        "{\"state\":\"%s\",\"busy\":%s,\"truncated\":%s,\"history_unavailable\":%s,"
        "\"scan_error\":%d,\"history_error\":%d,\"networks\":[",
        states[snapshot.state], snapshot.error == ESP_ERR_WIFI_STATE ? "true" : "false",
        snapshot.truncated ? "true" : "false", history_err == ESP_OK ? "false" : "true",
        (int)snapshot.error, (int)history_err);
    bool ok = used < capacity;
    bool comma = false;
    bool seen[WIFI_HISTORY_CAPACITY] = {0};
    for (size_t i = 0; ok && i < snapshot.count; ++i) {
        const portal_network_t *network = &snapshot.networks[i];
        bool saved = false;
        for (size_t j = 0; j < history.count; ++j) {
            if (strcmp(network->ssid, history.entries[j].ssid) == 0) {
                seen[j] = true;
                // Do not reuse protected credentials for a same-name open AP.
                saved = network->secured == (history.entries[j].password[0] != '\0');
            }
        }
        ok = portal_append_network(output, capacity, &used, network->ssid, network->rssi,
            true, network->secured, saved, network->supported, comma);
        comma = true;
    }
    for (size_t i = 0; ok && i < history.count; ++i) {
        if (seen[i]) continue;
        ok = portal_append_network(output, capacity, &used, history.entries[i].ssid, 0,
            false, history.entries[i].password[0] != '\0', true, true, comma);
        comma = true;
    }
    memset(&history, 0, sizeof(history));
    esp_err_t result;
    if (ok && capacity - used >= 3) {
        memcpy(output + used, "]}", 3);
        httpd_resp_set_type(request, "application/json; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        result = httpd_resp_send(request, output, used + 2);
    } else {
        result = httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot encode networks");
    }
    heap_caps_free(output);
    return result;
}

static esp_err_t captive_portal_404(httpd_req_t *request,
                                    httpd_err_code_t error)
{
    (void)error;
    if (!portal_socket_allowed(httpd_req_to_sockfd(request)))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "provisioning closed");
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", WIFI_SETUP_URL);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_sendstr(
        request,
        "<!doctype html><meta charset=utf-8>"
        "<meta http-equiv=refresh content='0;url=http://192.168.6.1'>"
        "正在打开 TiRTC 配网页…");
}

static esp_err_t wifi_config_post(httpd_req_t *request)
{
    if (!portal_write_allowed(request))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "provisioning closed");
    /* 请求体有硬上限且分段读取，防止配置 HTTP 任务被无界占用。 */
    if (request->content_len <= 0 || request->content_len > 512) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid request size");
    }
    char body[513];
    size_t total = 0;
    unsigned receive_timeouts = 0;
    const int64_t deadline = esp_timer_get_time() + 3000000;
    while (total < (size_t)request->content_len) {
        if (esp_timer_get_time() >= deadline || !atomic_load(&s_provisioning))
            return httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT, "provisioning request expired");
        int received = httpd_req_recv(request,
                                      body + total,
                                      (size_t)request->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++receive_timeouts >= 3U) {
                return httpd_resp_send_err(request,
                                           HTTPD_408_REQ_TIMEOUT,
                                           "request body timed out");
            }
            continue;
        }
        if (received <= 0) {
            return httpd_resp_send_err(request,
                                       HTTPD_400_BAD_REQUEST,
                                       "cannot read request");
        }
        receive_timeouts = 0;
        total += (size_t)received;
    }
    body[total] = '\0';

    wifi_history_t history = {0};
    cJSON *root = cJSON_ParseWithLength(body, total);
    const cJSON *ssid = root == NULL ? NULL :
        cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = root == NULL ? NULL :
        cJSON_GetObjectItemCaseSensitive(root, "password");
    bool use_saved = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "use_saved"));
    if (!cJSON_IsString(ssid) || (!use_saved && !cJSON_IsString(password)) ||
        (use_saved && password != NULL)) {
        memset(&history, 0, sizeof(history));
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "ssid/password required");
    }

    const char *selected_password = use_saved ? NULL : password->valuestring;
    if (use_saved) {
        esp_err_t history_err = wifi_history_load(&history);
        if (history_err != ESP_OK) {
            memset(&history, 0, sizeof(history));
            cJSON_Delete(root);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "saved networks unavailable");
        }
        for (size_t i = 0; i < history.count; ++i) {
            if (strcmp(history.entries[i].ssid, ssid->valuestring) == 0) {
                selected_password = history.entries[i].password;
                break;
            }
        }
        if (selected_password == NULL) {
            memset(&history, 0, sizeof(history));
            cJSON_Delete(root);
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "saved network not found; enter password");
        }
    }
    char validation_error[80];
    if (!wifi_manager_credentials_valid(ssid->valuestring,
                                        selected_password,
                                        validation_error,
                                        sizeof(validation_error))) {
        memset(&history, 0, sizeof(history));
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, validation_error);
    }
    /* Recheck after body reception. A request already accepted here may finish
     * committing while stop joins HTTPD; no later request may begin a write. */
    if (!portal_socket_allowed(httpd_req_to_sockfd(request))) {
        memset(&history, 0, sizeof(history));
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "provisioning closed");
    }
    /* HTTPD serializes these requests. The worker lives for the entire Wi-Fi
     * lifetime; reject before writing if it cannot own the restart. */
    if (s_signal_task == NULL || atomic_load(&s_restart_requested)) {
        memset(&history, 0, sizeof(history));
        cJSON_Delete(root);
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "configuration unavailable or restart pending");
    }
    esp_err_t err = wifi_manager_save_credentials(ssid->valuestring, selected_password);
    memset(&history, 0, sizeof(history));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi config save failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    }

    atomic_store(&s_restart_requested, true);
    xTaskNotifyGive(s_signal_task);
    /* Apply an accepted save even if the browser disconnects during delivery. */
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "配置已保存，设备即将重启，请查看设备联网状态。");
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Portal buffers live only while AP provisioning is active. NVS writes and
     * esp_restart() are delegated to their internal-stack owners. */
    config.stack_size = 8192;
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;
    config.open_fn = portal_open;
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 1;
    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        s_http_server = NULL;
        ESP_LOGE(TAG, "cannot start provisioning HTTP server: %s",
                 esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t page = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = setup_page_get,
    };
    const httpd_uri_t api = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = wifi_config_post,
    };
    const httpd_uri_t scan = { .uri = "/api/scan", .method = HTTP_POST, .handler = portal_scan_post };
    const httpd_uri_t networks = { .uri = "/api/networks", .method = HTTP_GET, .handler = portal_networks_get };
    err = httpd_register_uri_handler(s_http_server, &page);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &api);
    }
    if (err == ESP_OK) err = httpd_register_uri_handler(s_http_server, &scan);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_http_server, &networks);
    if (err == ESP_OK) {
        err = httpd_register_err_handler(s_http_server,
                                         HTTPD_404_NOT_FOUND,
                                         captive_portal_404);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot register provisioning HTTP handlers: %s",
                 esp_err_to_name(err));
        (void)httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    return err;
}

static esp_err_t stop_provisioning(void)
{
    /* Revoke new writes before joining workers. Keep failed handles available
     * for diagnosis/retry, and report teardown errors instead of false success. */
    atomic_store(&s_provisioning, false);
    esp_err_t err = captive_dns_stop();
    if (s_http_server != NULL) {
        esp_err_t http_err = httpd_stop(s_http_server);
        if (http_err == ESP_OK) s_http_server = NULL;
        if (err == ESP_OK) err = http_err;
    }
    if (s_scan_active) {
        esp_err_t scan_err = esp_wifi_scan_stop();
        s_scan_draining = true;
        portal_scan_finish(scan_err == ESP_OK ? ESP_ERR_INVALID_STATE : scan_err);
        if (err == ESP_OK) err = scan_err;
    }
    taskENTER_CRITICAL(&s_scan_lock);
    memset(&s_scan, 0, sizeof(s_scan));
    s_scan_request_us = 0;
    taskEXIT_CRITICAL(&s_scan_lock);
    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = mode_err;
    if (err != ESP_OK)
        ESP_LOGE(TAG, "captive portal teardown failed: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t start_provisioning(void)
{
    if (s_provisioning) {
        return ESP_OK;
    }
    /* Clean a partial previous start/stop before reusing handles. */
    esp_err_t cleanup_err = stop_provisioning();
    if (cleanup_err != ESP_OK) return cleanup_err;
    uint8_t mac[6] = {0};
    /* Use the initialized Wi-Fi interface: P4's MAC belongs to its C6 slave. */
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (mac_err != ESP_OK) return mac_err;
    (void)snprintf(s_provisioning_ssid,
                   sizeof(s_provisioning_ssid),
                   "XiaoTai-%02X%02X",
                   mac[4],
                   mac[5]);

    /* AP 名称后缀来自 STA MAC，方便同时调试多块开发板。 */
    wifi_config_t ap = {0};
    size_t ap_ssid_length = strlen(s_provisioning_ssid);
    memcpy(ap.ap.ssid, s_provisioning_ssid, ap_ssid_length);
    ap.ap.ssid_len = ap_ssid_length;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t portal_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (portal_err == ESP_OK) portal_err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (portal_err == ESP_OK) portal_err = start_http_server();
    if (portal_err == ESP_OK) {
        portal_err = captive_dns_start(s_ap_netif);
    }
    if (portal_err != ESP_OK) {
        ESP_LOGE(TAG, "cannot start captive portal: %s",
                 esp_err_to_name(portal_err));
        (void)stop_provisioning();
        return portal_err;
    }
    s_provisioning = true;
    ESP_LOGW(TAG,
             "provisioning active: connect open SSID=%s; portal=%s",
             s_provisioning_ssid,
             WIFI_SETUP_URL);
    return ESP_OK;
}

static void retry_timer_callback(void *argument)
{
    (void)argument;
    /* No driver calls or blocking in esp_timer. Keep ticking while pending:
     * a temporarily full event queue must not permanently lose reconnection. */
    (void)esp_event_post(WIFI_MANAGER_CONTROL, WIFI_CONTROL_RETRY, NULL, 0, 0);
}

static void retry_stop(void)
{
    s_retry_due_us = 0;
    if (!s_scan_active) (void)esp_timer_stop(s_retry_timer);
}

static void retry_schedule(void)
{
    if (atomic_load(&s_manual_disconnect) || s_connected ||
        !s_has_saved_credentials || s_retry_due_us != 0) return;
    static const uint8_t delay_seconds[] = {1, 2, 4, 8, 15, 30};
    if (s_retry_count < UINT32_MAX) ++s_retry_count;
    unsigned index = s_retry_count < sizeof(delay_seconds) ? s_retry_count - 1U :
        sizeof(delay_seconds) - 1U;
    unsigned delay = delay_seconds[index];
    s_retry_due_us = esp_timer_get_time() + (int64_t)delay * 1000000;
    esp_err_t err = esp_timer_start_periodic(s_retry_timer, 1000000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        s_retry_due_us = 0;
        ESP_LOGE(TAG, "Wi-Fi retry timer failed: %s", esp_err_to_name(err));
    }
    if (s_retry_count <= 6U || s_retry_count % 10U == 0U)
        ESP_LOGW(TAG, "Wi-Fi retry pending: attempt=%lu delay=%us",
                 (unsigned long)s_retry_count, delay);
    if (s_retry_count >= WIFI_PROVISION_AFTER_FAILURES) {
        s_connection_failed = true;
        (void)snprintf(s_provisioning_status, sizeof(s_provisioning_status),
            "正在持续重连已保存的网络，也可在此更换网络");
        /* Portal availability must never disable retries of saved credentials. */
        (void)start_provisioning();
    }
}

static void connect_saved_network(void)
{
    if (atomic_load(&s_manual_disconnect) || !s_has_saved_credentials ||
        s_connected || s_connecting || s_scan_active || s_retry_due_us != 0) return;
    s_connect_started_us = esp_timer_get_time();
    s_associated_us = 0;
    esp_err_t err = esp_wifi_connect();
    s_connecting = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connect rejected: %s", esp_err_to_name(err));
        retry_schedule();
    }
}

static void wifi_control_event(int32_t event_id)
{
    if (event_id == WIFI_CONTROL_SCAN) {
        portal_scan_start();
        return;
    }
    if (event_id == WIFI_CONTROL_RETRY) {
        if (s_scan_active && esp_timer_get_time() >= s_scan_deadline_us) {
            esp_err_t err = esp_wifi_scan_stop();
            // Wait for the stopped scan's DONE before allowing another scan.
            s_scan_draining = true;
            portal_scan_finish(err == ESP_OK ? ESP_ERR_TIMEOUT : err);
        }
        if (s_scan_active) return;
        if (s_retry_due_us == 0 || esp_timer_get_time() < s_retry_due_us) return;
        retry_stop();
        connect_saved_network();
        return;
    }
    /* Manual disconnect means "change network". It keeps stored credentials
     * but stays in the portal until the existing save-and-reboot flow or reboot. */
    bool was_manual = atomic_load(&s_manual_disconnect);
    esp_err_t err = ESP_OK;
    if (event_id == WIFI_CONTROL_DISCONNECT) {
        atomic_store(&s_manual_disconnect, true);
        err = esp_wifi_disconnect();
        if (err == ESP_ERR_WIFI_NOT_CONNECT) err = ESP_OK;
        if (err == ESP_OK) {
            s_connected = false;
            s_connecting = false;
            s_connection_failed = false;
            retry_stop();
            signal_connection_changed();
            (void)snprintf(s_provisioning_status, sizeof(s_provisioning_status),
                "已断开原网络，请在手机上配置 Wi-Fi");
            err = start_provisioning();
            ESP_LOGI(TAG, "Wi-Fi manual provisioning; credentials retained");
        } else {
            atomic_store(&s_manual_disconnect, was_manual);
        }
    } else {
        err = ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_control_error, err);
    atomic_store(&s_control_pending, false);
    if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi control failed: %s", esp_err_to_name(err));
}

static esp_err_t wifi_control_post(int32_t event_id)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_control_pending, &expected, true))
        return ESP_ERR_INVALID_STATE;
    atomic_store(&s_control_error, ESP_OK);
    esp_err_t err = esp_event_post(WIFI_MANAGER_CONTROL, event_id, NULL, 0, 0);
    if (err != ESP_OK) {
        atomic_store(&s_control_error, err);
        atomic_store(&s_control_pending, false);
    }
    return err;
}

esp_err_t wifi_manager_disconnect(void) { return wifi_control_post(WIFI_CONTROL_DISCONNECT); }
bool wifi_manager_manually_disconnected(void) { return atomic_load(&s_manual_disconnect); }
bool wifi_manager_control_pending(void) { return atomic_load(&s_control_pending); }
esp_err_t wifi_manager_control_error(void) { return atomic_load(&s_control_error); }

#if CONFIG_IDF_TARGET_ESP32S3
/* Native S3 policy only: P4 shares this file but configures a Hosted radio.
 * Apply before STA_START can connect. HT20 favors realtime delivery in crowded
 * 2.4 GHz environments over HT40 peak throughput; keep legacy AP compatibility.
 * Do not override the AP country/channel or rate-dependent PHY power limits. */
static esp_err_t configure_s3_realtime_wifi(void)
{
    const char *stage = "power-save";
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) {
        stage = "protocol";
        err = esp_wifi_set_protocol(WIFI_IF_STA,
                                    WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    }
    if (err == ESP_OK) {
        stage = "bandwidth";
        err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Wi-Fi perf setup failed: stage=%s err=%s", stage, esp_err_to_name(err));
    return err;
}

static void log_s3_realtime_wifi(void)
{
    wifi_ps_type_t power_save;
    wifi_bandwidth_t bandwidth;
    uint8_t protocol;
    int8_t tx_power;
    wifi_ap_record_t ap = {0};
    esp_err_t err = esp_wifi_get_ps(&power_save);
    if (err == ESP_OK) err = esp_wifi_get_bandwidth(WIFI_IF_STA, &bandwidth);
    if (err == ESP_OK) err = esp_wifi_get_protocol(WIFI_IF_STA, &protocol);
    if (err == ESP_OK) err = esp_wifi_get_max_tx_power(&tx_power);
    if (err == ESP_OK) err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi perf readback failed: %s", esp_err_to_name(err));
        return;
    }
    /* One snapshot per IP event, not per packet. TX is a cap in 0.25 dBm units,
     * not a claim about actual RF power or negotiated throughput. */
    ESP_LOGI(TAG, "Wi-Fi perf: ps=%u bw_limit=%uMHz proto=0x%02x tx_max_qdbm=%d ch=%u rssi=%d",
             (unsigned)power_save, bandwidth == WIFI_BW_HT20 ? 20U : 40U,
             (unsigned)protocol, (int)tx_power, (unsigned)ap.primary, (int)ap.rssi);
}
#elif CONFIG_IDF_TARGET_ESP32P4
/* P4 owns the policy; these APIs are synchronous RPCs to the C6 radio.
 * Disable modem sleep before STA_START can trigger the first connection.
 * C6 supports 11ax at HE20; forcing HT40 would require dropping 11ax. */
static esp_err_t configure_p4_realtime_wifi(void)
{
    const uint8_t protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                             WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
    wifi_ps_type_t power_save = WIFI_PS_MIN_MODEM;
    const char *stage = "power-save";
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) {
        stage = "protocol";
        err = esp_wifi_set_protocol(WIFI_IF_STA, protocol);
    }
    if (err == ESP_OK) {
        stage = "bandwidth";
        err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    }
    if (err == ESP_OK) {
        stage = "power-save-readback";
        err = esp_wifi_get_ps(&power_save);
        if (err == ESP_OK && power_save != WIFI_PS_NONE) err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "P4 Wi-Fi perf setup failed: stage=%s ps=%u err=%s",
                 stage, (unsigned)power_save, esp_err_to_name(err));
        return err;
    }
    /* One bounded readback before start; no extra diagnostic RPCs in the UI or
     * event callback. Report requested capabilities, not negotiated link speed.
     * Country/channel and rate-dependent PHY power caps remain driver-owned. */
    ESP_LOGI(TAG, "P4 Wi-Fi perf: ps=%u bw_limit=20MHz proto_cfg=0x%02x sdio_max_khz=%u lines=%u",
             (unsigned)power_save, (unsigned)protocol,
             (unsigned)CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ,
             (unsigned)CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH);
    return ESP_OK;
}
#endif

static void wifi_event(void *argument,
                       esp_event_base_t event_base,
                       int32_t event_id,
                       void *event_data)
{
    /* STA attempts and provisioning intent share one owner, but are independent. */
    (void)argument;
    (void)event_data;
    if (event_base == WIFI_MANAGER_CONTROL) {
        wifi_control_event(event_id);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        const wifi_event_sta_scan_done_t *done = event_data;
        if (s_scan_active) {
            portal_scan_finish(done != NULL && done->status == 0 ? ESP_OK : ESP_FAIL);
        } else if (s_scan_draining) {
            esp_err_t err = esp_wifi_clear_ap_list();
            if (err != ESP_OK) ESP_LOGW(TAG, "scan drain failed: %s", esp_err_to_name(err));
        }
        s_scan_draining = false;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (atomic_load(&s_manual_disconnect)) return;
        if (s_has_saved_credentials) {
            connect_saved_network();
        } else {
            s_has_saved_credentials = false;
            s_connection_failed = false;
            (void)snprintf(s_provisioning_status,
                           sizeof(s_provisioning_status),
                           "尚未配置 Wi-Fi，请在手机页面选择家庭网络");
            atomic_store(&s_control_error, start_provisioning());
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        s_associated_us = esp_timer_get_time();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *lost = event_data;
        if (!atomic_load(&s_manual_disconnect) && lost != NULL)
            ESP_LOGW(TAG, "Wi-Fi link failed: reason=%u elapsed_ms=%lu",
                     (unsigned)lost->reason,
                     (unsigned long)((esp_timer_get_time() - s_connect_started_us) / 1000));
        s_connected = false;
        s_connecting = false;
        signal_connection_changed();
        if (atomic_load(&s_manual_disconnect)) return;
        if (s_has_saved_credentials) {
            retry_schedule();
        } else if (!s_provisioning) {
            atomic_store(&s_control_error, start_provisioning());
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (atomic_load(&s_manual_disconnect)) {
            /* A queued DHCP event cannot undo a newer offline user intent. */
            (void)esp_wifi_disconnect();
            return;
        }
        const ip_event_got_ip_t *got_ip = event_data;
        const int64_t ip_ready_us = esp_timer_get_time();
        s_connected = true;
        s_connecting = false;
        retry_stop();
        // Only the credentials used by this boot earned a successful-history entry.
        atomic_store(&s_history_pending, true);
        signal_connection_changed();
        s_connection_failed = false;
        s_retry_count = 0;
        atomic_store(&s_control_error, stop_provisioning());
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&got_ip->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi setup: link_ms=%ld dhcp_ms=%ld portal_stop_ms=%lu",
                 s_associated_us ? (long)((s_associated_us - s_connect_started_us) / 1000) : -1L,
                 s_associated_us ? (long)((ip_ready_us - s_associated_us) / 1000) : -1L,
                 (unsigned long)((esp_timer_get_time() - ip_ready_us) / 1000));
#if CONFIG_IDF_TARGET_ESP32S3
        log_s3_realtime_wifi();
#endif
    }
}

esp_err_t wifi_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    (void)esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* 产品配网页明确约定 192.168.6.1，不能依赖 IDF 的 192.168.4.1 默认值。 */
    esp_netif_ip_info_t ap_ip = {0};
    esp_netif_set_ip4_addr(&ap_ip.ip, 192, 168, 6, 1);
    esp_netif_set_ip4_addr(&ap_ip.gw, 192, 168, 6, 1);
    esp_netif_set_ip4_addr(&ap_ip.netmask, 255, 255, 255, 0);
    (void)esp_netif_dhcps_stop(s_ap_netif);
    err = esp_netif_set_ip_info(s_ap_netif, &ap_ip);
    if (err != ESP_OK) {
        return err;
    }
    esp_netif_dns_info_t ap_dns = {0};
    ap_dns.ip.type = ESP_IPADDR_TYPE_V4;
    ap_dns.ip.u_addr.ip4.addr = ap_ip.ip.addr;
    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &ap_dns);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t offer_dns = 1U;
    err = esp_netif_dhcps_option(s_ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns,
                                 sizeof(offer_dns));
    if (err != ESP_OK) {
        return err;
    }
    /*
     * Do not advertise the local HTML page through DHCP option 114. RFC 8910
     * clients treat that URI as an RFC 8908 HTTPS/JSON API; an HTTP page can
     * suppress their legacy captive-network probe instead of opening it.
     * Wildcard DNS plus the HTTP 303 fallback below remain the discovery path.
     */
    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        return err;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_MANAGER_CONTROL, ESP_EVENT_ANY_ID, wifi_event, NULL));

    /* 启动前先决定 STA 还是 APSTA；STA_START 事件负责真正 connect。 */
    wifi_manager_credentials_t credentials;
    esp_err_t credentials_err = wifi_manager_load_credentials(&credentials);
    if (credentials_err == ESP_OK) {
        s_has_saved_credentials = true;
        wifi_config_t station = {0};
        memcpy(station.sta.ssid, credentials.ssid, strlen(credentials.ssid));
        memcpy(station.sta.password, credentials.password, strlen(credentials.password));
        station.sta.threshold.authmode = credentials.password[0] == '\0'
                                             ? WIFI_AUTH_OPEN
                                             : WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
        ESP_LOGI(TAG, "connecting to configured SSID=%s", credentials.ssid);
    } else {
        if (credentials_err != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGE(TAG, "Wi-Fi config load failed: %s", esp_err_to_name(credentials_err));
        s_has_saved_credentials = false;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }
#if CONFIG_IDF_TARGET_ESP32S3
    err = configure_s3_realtime_wifi();
    if (err != ESP_OK) return err;
#elif CONFIG_IDF_TARGET_ESP32P4
    err = configure_p4_realtime_wifi();
    if (err != ESP_OK) return err;
#endif
    wifi_history_init(credentials_err == ESP_OK ? &credentials : NULL);
    memset(&credentials, 0, sizeof(credentials));
    if (xTaskCreate(signal_task, "wifi_signal", 3072, NULL, 1, &s_signal_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t retry_timer = {
        .callback = retry_timer_callback,
        .name = "wifi_retry",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&retry_timer, &s_retry_timer);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
#if !CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_IDF_TARGET_ESP32P4
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot disable Wi-Fi power save: %s",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Wi-Fi power save disabled for real-time media");
#endif
    s_started = true;
    return ESP_OK;
}

bool wifi_manager_connected(void)
{
    return s_connected;
}

uint32_t wifi_manager_signal_revision(void)
{
    taskENTER_CRITICAL(&s_signal_lock);
    uint32_t revision = s_signal_revision;
    taskEXIT_CRITICAL(&s_signal_lock);
    return revision;
}

bool wifi_manager_signal_dbm(int8_t *rssi)
{
    if (rssi == NULL || !s_connected) {
        return false;
    }
    taskENTER_CRITICAL(&s_signal_lock);
    int cached = s_cached_rssi;
    taskEXIT_CRITICAL(&s_signal_lock);
    if (cached > 0) return false;
    *rssi = (int8_t)cached;
    return s_connected;
}

bool wifi_manager_provisioning(void)
{
    return s_provisioning;
}

bool wifi_manager_connection_failed(void)
{
    return s_connection_failed;
}

const char *wifi_manager_provisioning_ssid(void)
{
    return s_provisioning ? s_provisioning_ssid : "";
}

const char *wifi_manager_provisioning_password(void)
{
    return "";
}

const char *wifi_manager_provisioning_url(void)
{
    return WIFI_SETUP_URL;
}

const char *wifi_manager_provisioning_status(void)
{
    return s_provisioning ? s_provisioning_status : "";
}
