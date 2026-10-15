#include "wifi_provisioning.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "captive_dns.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"

#define TAG "wifi_provision"
#define WIFI_SETUP_URL "http://192.168.6.1"
#define WIFI_RETRY_LIMIT 3U
#define WIFI_SCAN_RESULT_LIMIT 16U
#define WIFI_HTTP_BODY_MAX 512U
#define WIFI_HTTP_BODY_TIMEOUT_US 5000000LL

enum {
    WIFI_SCAN_IDLE,
    WIFI_SCAN_QUEUED,
    WIFI_SCAN_RUNNING,
    WIFI_SCAN_READY,
    WIFI_SCAN_BUSY,
    WIFI_SCAN_ERROR,
};

ESP_EVENT_DEFINE_BASE(WIFI_PROVISIONING_CONTROL);

enum {
    WIFI_PROVISIONING_CONTROL_SCAN = 1,
};

static bool s_started;
static atomic_bool s_station_connecting;
static atomic_bool s_credentials_queued;
static uint8_t s_retry_count;
static wifi_config_t s_station_config;
static wifi_provisioning_credentials_t s_pending_credentials;
static StaticQueue_t s_credentials_queue_storage;
static uint8_t s_credentials_queue_buffer[sizeof(wifi_provisioning_credentials_t)];
static QueueHandle_t s_credentials_queue;

static esp_netif_t *s_ap_netif;
static httpd_handle_t s_http_server;
static char s_ap_ssid[33];
static atomic_int s_scan_state;
static bool s_scan_active;
static portMUX_TYPE s_scan_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_ap_record_t s_scan_records[WIFI_SCAN_RESULT_LIMIT];
static uint16_t s_scan_count;

extern const char s_setup_page[] asm("_binary_setup_html_start");

static bool credentials_valid(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return false;
    }
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    return ssid_length > 0U && ssid_length <= 32U &&
           (password_length == 0U ||
            (password_length >= 8U && password_length <= 64U));
}

static bool decode_ssid_hex(const char *hex, char ssid[33])
{
    if (hex == NULL) {
        return false;
    }
    size_t length = strlen(hex);
    if (length == 0U || length > 64U || (length & 1U) != 0U) {
        return false;
    }
    for (size_t i = 0; i < length / 2U; ++i) {
        unsigned value = 0U;
        for (unsigned nibble = 0; nibble < 2U; ++nibble) {
            char c = hex[i * 2U + nibble];
            unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0')
                             : c >= 'a' && c <= 'f'
                                 ? (unsigned)(c - 'a' + 10)
                             : c >= 'A' && c <= 'F'
                                 ? (unsigned)(c - 'A' + 10)
                                 : 16U;
            if (digit >= 16U) {
                return false;
            }
            value = value * 16U + digit;
        }
        if (value == 0U) {
            return false;
        }
        ssid[i] = (char)value;
    }
    ssid[length / 2U] = '\0';
    return true;
}

static bool auth_supported(wifi_auth_mode_t auth)
{
    return auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WPA2_PSK ||
           auth == WIFI_AUTH_WPA_WPA2_PSK || auth == WIFI_AUTH_WPA3_PSK ||
           auth == WIFI_AUTH_WPA2_WPA3_PSK;
}

static bool portal_socket_allowed(int socket_fd)
{
    if (!s_started || s_ap_netif == NULL) {
        return false;
    }
    struct sockaddr_storage local = {0};
    socklen_t size = sizeof(local);
    esp_netif_ip_info_t ap_ip = {0};
    if (getsockname(socket_fd, (struct sockaddr *)&local, &size) != 0 ||
        esp_netif_get_ip_info(s_ap_netif, &ap_ip) != ESP_OK) {
        return false;
    }
    uint32_t local_ipv4 = 0U;
    if (local.ss_family == AF_INET && size >= sizeof(struct sockaddr_in)) {
        local_ipv4 = ((const struct sockaddr_in *)&local)->sin_addr.s_addr;
#if CONFIG_LWIP_IPV6
    } else if (local.ss_family == AF_INET6 &&
               size >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *v6 =
            (const struct sockaddr_in6 *)&local;
        if (!IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
            return false;
        }
        memcpy(&local_ipv4, &v6->sin6_addr.s6_addr[12], sizeof(local_ipv4));
#endif
    } else {
        return false;
    }
    return ap_ip.ip.addr != 0U && local_ipv4 == ap_ip.ip.addr;
}

static esp_err_t portal_open(httpd_handle_t server, int socket_fd)
{
    (void)server;
    return portal_socket_allowed(socket_fd) ? ESP_OK : ESP_FAIL;
}

static void connect_station(void)
{
    s_retry_count = 0U;
    s_station_connecting = true;
    if (esp_wifi_connect() != ESP_OK) {
        s_station_connecting = false;
        ESP_LOGE(TAG, "Wi-Fi credential test could not start");
    }
}

static void scan_done(const wifi_event_sta_scan_done_t *event)
{
    if (!s_scan_active) {
        ESP_LOGW(TAG, "ignored Wi-Fi scan completion without active scan");
        return;
    }
    uint16_t count = WIFI_SCAN_RESULT_LIMIT;
    esp_err_t err = event != NULL && event->status == 0
                        ? esp_wifi_scan_get_ap_records(&count, s_scan_records)
                        : ESP_FAIL;
    if (err != ESP_OK) {
        count = 0U;
        (void)esp_wifi_clear_ap_list();
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    }
    taskENTER_CRITICAL(&s_scan_lock);
    s_scan_count = count;
    taskEXIT_CRITICAL(&s_scan_lock);
    s_scan_active = false;
    atomic_store_explicit(&s_scan_state,
                          err == ESP_OK ? WIFI_SCAN_READY : WIFI_SCAN_ERROR,
                          memory_order_release);
    ESP_LOGI(TAG, "Wi-Fi scan complete: count=%u", (unsigned)count);
}

static void start_scan_on_event_loop(void)
{
    if (atomic_load_explicit(&s_scan_state, memory_order_acquire) !=
        WIFI_SCAN_QUEUED) {
        return;
    }
    if (!s_started || s_station_connecting || s_scan_active) {
        atomic_store_explicit(&s_scan_state, WIFI_SCAN_BUSY,
                              memory_order_release);
        ESP_LOGW(TAG,
                 "Wi-Fi scan deferred: started=%d connecting=%d active=%d",
                 s_started, s_station_connecting, s_scan_active);
        return;
    }

    wifi_scan_config_t config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 0, .max = 120},
    };
    taskENTER_CRITICAL(&s_scan_lock);
    s_scan_count = 0U;
    taskEXIT_CRITICAL(&s_scan_lock);
    esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err == ESP_OK) {
        s_scan_active = true;
        atomic_store_explicit(&s_scan_state, WIFI_SCAN_RUNNING,
                              memory_order_release);
        ESP_LOGI(TAG, "Wi-Fi scan started");
        return;
    }
    atomic_store_explicit(&s_scan_state,
                          err == ESP_ERR_WIFI_STATE ? WIFI_SCAN_BUSY
                                                    : WIFI_SCAN_ERROR,
                          memory_order_release);
    ESP_LOGW(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
}

static void provisioning_control_handler(void *arg, esp_event_base_t base,
                                         int32_t id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base == WIFI_PROVISIONING_CONTROL &&
        id == WIFI_PROVISIONING_CONTROL_SCAN) {
        start_scan_on_event_loop();
    }
}

static esp_err_t queue_scan(void)
{
    int state = atomic_load_explicit(&s_scan_state, memory_order_acquire);
    if (state == WIFI_SCAN_RUNNING || state == WIFI_SCAN_QUEUED) {
        return ESP_OK;
    }
    if (s_station_connecting) {
        atomic_store_explicit(&s_scan_state, WIFI_SCAN_BUSY,
                              memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &s_scan_state, &state, WIFI_SCAN_QUEUED, memory_order_acq_rel,
            memory_order_acquire)) {
        return ESP_OK;
    }
    esp_err_t err = esp_event_post(WIFI_PROVISIONING_CONTROL,
                                   WIFI_PROVISIONING_CONTROL_SCAN, NULL, 0U,
                                   pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        atomic_store_explicit(&s_scan_state, WIFI_SCAN_ERROR,
                              memory_order_release);
        ESP_LOGW(TAG, "Wi-Fi scan queue failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *event_data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "received Wi-Fi credentials associated; waiting for IP");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_station_connecting && s_retry_count++ < WIFI_RETRY_LIMIT) {
            s_station_connecting = esp_wifi_connect() == ESP_OK;
        } else if (s_station_connecting) {
            s_station_connecting = false;
            ESP_LOGW(TAG,
                     "received Wi-Fi credentials failed verification; SoftAP remains active");
        }
        break;
    case WIFI_EVENT_SCAN_DONE:
        scan_done(event_data);
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id,
                             void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;
    if (id != IP_EVENT_STA_GOT_IP || !s_station_connecting) {
        return;
    }
    if (!s_credentials_queued &&
        xQueueOverwrite(s_credentials_queue, &s_pending_credentials) ==
            pdPASS) {
        s_credentials_queued = true;
        ESP_LOGI(TAG,
                 "Wi-Fi verified; credentials queued for persistence and restart");
    } else if (!s_credentials_queued) {
        ESP_LOGE(TAG, "verified credential queue unavailable");
    }
    /* Keep HTTP submissions excluded until the verified snapshot is queued. */
    s_station_connecting = false;
}

static esp_err_t setup_page_get(httpd_req_t *request)
{
    if (!portal_socket_allowed(httpd_req_to_sockfd(request))) {
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                                   "provisioning closed");
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_setup_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_portal_404(httpd_req_t *request,
                                    httpd_err_code_t error)
{
    (void)error;
    if (!portal_socket_allowed(httpd_req_to_sockfd(request))) {
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                                   "provisioning closed");
    }
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", WIFI_SETUP_URL);
    return httpd_resp_sendstr(request, "Open http://192.168.6.1");
}

static esp_err_t wifi_config_post(httpd_req_t *request)
{
    if (!portal_socket_allowed(httpd_req_to_sockfd(request))) {
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                                   "provisioning closed");
    }
    if (request->content_len <= 0 ||
        request->content_len > WIFI_HTTP_BODY_MAX) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid request size");
    }

    char body[WIFI_HTTP_BODY_MAX + 1U];
    size_t total = 0U;
    const int64_t deadline = esp_timer_get_time() + WIFI_HTTP_BODY_TIMEOUT_US;
    while (total < (size_t)request->content_len) {
        if (esp_timer_get_time() >= deadline) {
            memset(body, 0, sizeof(body));
            return httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT,
                                       "Wi-Fi request body timed out");
        }
        int received = httpd_req_recv(request, body + total,
                                      (size_t)request->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                       "cannot read request");
        }
        total += (size_t)received;
    }
    body[total] = '\0';

    cJSON *root = cJSON_ParseWithLength(body, total);
    memset(body, 0, sizeof(body));
    cJSON *ssid_json = root == NULL
                           ? NULL
                           : cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password_json =
        root == NULL
            ? NULL
            : cJSON_GetObjectItemCaseSensitive(root, "password");
    cJSON *ssid_hex = root == NULL
                          ? NULL
                          : cJSON_GetObjectItemCaseSensitive(root, "ssid_hex");
    char decoded_ssid[33] = {0};
    const char *ssid = cJSON_IsString(ssid_json) ? ssid_json->valuestring : NULL;
    if (cJSON_IsString(ssid_hex)) {
        if (!decode_ssid_hex(ssid_hex->valuestring, decoded_ssid)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                       "invalid SSID identity");
        }
        ssid = decoded_ssid;
    }
    const char *password =
        cJSON_IsString(password_json) ? password_json->valuestring : "";
    if (!credentials_valid(ssid, password)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid SSID or password");
    }
    if (s_station_connecting || s_credentials_queued) {
        cJSON_Delete(root);
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request,
                                  "Wi-Fi verification already running");
    }

    size_t ssid_length = strlen(ssid);
    memset(&s_station_config, 0, sizeof(s_station_config));
    memset(&s_pending_credentials, 0, sizeof(s_pending_credentials));
    memcpy(s_station_config.sta.ssid, ssid, ssid_length);
    memcpy(s_station_config.sta.password, password, strlen(password));
    memcpy(s_pending_credentials.ssid, ssid, strlen(ssid) + 1U);
    memcpy(s_pending_credentials.password, password, strlen(password) + 1U);
    s_station_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cJSON_Delete(root);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_station_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi station configuration failed: %s",
                 esp_err_to_name(err));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "cannot apply Wi-Fi settings");
    }
    s_credentials_queued = false;
    connect_station();
    if (!s_station_connecting) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "cannot start Wi-Fi verification");
    }
    ESP_LOGI(TAG, "testing received Wi-Fi credentials: ssid_len=%u",
             (unsigned)ssid_length);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(
        request,
        "正在验证网络，连接成功后设备会自动保存并重启。");
}

static const char *scan_state_name(int state)
{
    switch (state) {
    case WIFI_SCAN_QUEUED:
    case WIFI_SCAN_RUNNING:
        return "scanning";
    case WIFI_SCAN_READY:
        return "ready";
    case WIFI_SCAN_BUSY:
        return "busy";
    case WIFI_SCAN_ERROR:
        return "error";
    default:
        return "idle";
    }
}

static esp_err_t send_network(httpd_req_t *request,
                              const wifi_ap_record_t *record, bool *first)
{
    char ssid[33];
    memcpy(ssid, record->ssid, 32U);
    ssid[32] = '\0';
    size_t length = strlen(ssid);
    char hex[65];
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        unsigned byte = (unsigned char)ssid[i];
        hex[i * 2U] = digits[byte >> 4U];
        hex[i * 2U + 1U] = digits[byte & 15U];
    }
    hex[length * 2U] = '\0';
    bool secure = record->authmode != WIFI_AUTH_OPEN;
    bool supported = auth_supported(record->authmode);
    char json[512];
    cJSON fields[] = {
        {.type = cJSON_String, .string = "ssid", .valuestring = ssid},
        {.type = cJSON_String, .string = "ssid_hex", .valuestring = hex},
        {.type = cJSON_Number,
         .string = "rssi",
         .valuedouble = record->rssi},
        {.type = secure ? cJSON_True : cJSON_False, .string = "secure"},
        {.type = cJSON_False, .string = "saved"},
        {.type = cJSON_True, .string = "available"},
        {.type = supported ? cJSON_True : cJSON_False,
         .string = "supported"},
    };
    for (size_t i = 0; i + 1U < sizeof(fields) / sizeof(fields[0]); ++i) {
        fields[i].next = &fields[i + 1U];
    }
    cJSON object = {.type = cJSON_Object, .child = fields};
    if (!cJSON_PrintPreallocated(&object, json, sizeof(json), false)) {
        return ESP_ERR_NO_MEM;
    }
    if (!*first && httpd_resp_send_chunk(request, ",", 1U) != ESP_OK) {
        return ESP_FAIL;
    }
    *first = false;
    return httpd_resp_sendstr_chunk(request, json);
}

static esp_err_t networks_get(httpd_req_t *request)
{
    if (!portal_socket_allowed(httpd_req_to_sockfd(request))) {
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                                   "provisioning closed");
    }
    int state = atomic_load_explicit(&s_scan_state, memory_order_acquire);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char header[96];
    snprintf(header, sizeof(header),
             "{\"state\":\"%s\",\"history_error\":false,\"networks\":[",
             scan_state_name(state));
    esp_err_t err = httpd_resp_sendstr_chunk(request, header);
    bool first = true;
    uint16_t count = 0U;
    if (state == WIFI_SCAN_READY) {
        taskENTER_CRITICAL(&s_scan_lock);
        count = s_scan_count;
        taskEXIT_CRITICAL(&s_scan_lock);
    }
    for (uint16_t i = 0; i < count && err == ESP_OK; ++i) {
        wifi_ap_record_t record;
        taskENTER_CRITICAL(&s_scan_lock);
        record = s_scan_records[i];
        taskEXIT_CRITICAL(&s_scan_lock);
        if (record.ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (uint16_t j = 0; j < i; ++j) {
            taskENTER_CRITICAL(&s_scan_lock);
            wifi_ap_record_t previous = s_scan_records[j];
            taskEXIT_CRITICAL(&s_scan_lock);
            if (strncmp((const char *)record.ssid,
                        (const char *)previous.ssid, 33U) == 0 &&
                (record.authmode == WIFI_AUTH_OPEN) ==
                    (previous.authmode == WIFI_AUTH_OPEN)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            err = send_network(request, &record, &first);
        }
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(request, "]}");
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return err;
}

static esp_err_t scan_post(httpd_req_t *request)
{
    if (!portal_socket_allowed(httpd_req_to_sockfd(request))) {
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                                   "provisioning closed");
    }
    if (request->content_len != 0) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "scan body must be empty");
    }
    esp_err_t err = queue_scan();
    ESP_LOGI(TAG, "Wi-Fi scan requested: result=%s state=%s",
             esp_err_to_name(err),
             scan_state_name(atomic_load_explicit(&s_scan_state,
                                                  memory_order_acquire)));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "cannot queue Wi-Fi scan");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "{\"accepted\":true}");
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 7168U;
    config.max_uri_handlers = 4U;
    config.lru_purge_enable = true;
    config.open_fn = portal_open;
    config.recv_wait_timeout = 2U;
    config.send_wait_timeout = 2U;
    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        s_http_server = NULL;
        return err;
    }
    const httpd_uri_t page = {
        .uri = "/", .method = HTTP_GET, .handler = setup_page_get};
    const httpd_uri_t config_uri = {
        .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_config_post};
    const httpd_uri_t networks = {.uri = "/api/wifi/networks",
                                  .method = HTTP_GET,
                                  .handler = networks_get};
    const httpd_uri_t scan = {.uri = "/api/wifi/scan",
                              .method = HTTP_POST,
                              .handler = scan_post};
    err = httpd_register_uri_handler(s_http_server, &page);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &config_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &networks);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &scan);
    }
    if (err == ESP_OK) {
        err = httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND,
                                         captive_portal_404);
    }
    if (err != ESP_OK) {
        (void)httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    return err;
}

static esp_err_t configure_ap_netif(void)
{
    esp_netif_ip_info_t ap_ip = {0};
    esp_netif_set_ip4_addr(&ap_ip.ip, 192, 168, 6, 1);
    esp_netif_set_ip4_addr(&ap_ip.gw, 192, 168, 6, 1);
    esp_netif_set_ip4_addr(&ap_ip.netmask, 255, 255, 255, 0);
    (void)esp_netif_dhcps_stop(s_ap_netif);
    esp_err_t err = esp_netif_set_ip_info(s_ap_netif, &ap_ip);
    if (err != ESP_OK) {
        return err;
    }
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ap_ip.ip.addr;
    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t offer_dns = 1U;
    err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns,
                                 sizeof(offer_dns));
    if (err != ESP_OK) {
        return err;
    }
    err = esp_netif_dhcps_start(s_ap_netif);
    return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? ESP_OK : err;
}

esp_err_t wifi_provisioning_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_credentials_queue = xQueueCreateStatic(
        1U, sizeof(wifi_provisioning_credentials_t),
        s_credentials_queue_buffer, &s_credentials_queue_storage);
    if (s_credentials_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = configure_ap_netif();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     ip_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(WIFI_PROVISIONING_CONTROL,
                                     WIFI_PROVISIONING_CONTROL_SCAN,
                                     provisioning_control_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&wifi_init)) != ESP_OK ||
        (err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) {
        return err;
    }
    uint8_t mac[6] = {0};
    if ((err = esp_wifi_get_mac(WIFI_IF_STA, mac)) != ESP_OK) {
        return err;
    }
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "XiaoTai-%02X%02X", mac[4],
             mac[5]);
    wifi_config_t ap_config = {0};
    size_t ssid_length = strlen(s_ap_ssid);
    memcpy(ap_config.ap.ssid, s_ap_ssid, ssid_length);
    ap_config.ap.ssid_len = ssid_length;
    ap_config.ap.channel = 1U;
    ap_config.ap.max_connection = 4U;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    if ((err = esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_AP, &ap_config)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        return err;
    }
    s_started = true;
    if ((err = start_http_server()) != ESP_OK ||
        (err = captive_dns_start(s_ap_netif)) != ESP_OK) {
        s_started = false;
        ESP_LOGE(TAG, "SoftAP portal start failed: %s", esp_err_to_name(err));
        return err;
    }
    atomic_store(&s_scan_state, WIFI_SCAN_IDLE);
    s_scan_active = false;
    err = queue_scan();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial Wi-Fi scan queue failed: %s",
                 esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "provisioning active: connect open SSID=%s portal=%s",
             s_ap_ssid, WIFI_SETUP_URL);
    return ESP_OK;
}

bool wifi_provisioning_take_credentials(
    wifi_provisioning_credentials_t *credentials)
{
    return credentials != NULL && s_credentials_queue != NULL &&
           xQueueReceive(s_credentials_queue, credentials, 0) == pdTRUE;
}

bool wifi_provisioning_active(void)
{
    return s_started;
}
