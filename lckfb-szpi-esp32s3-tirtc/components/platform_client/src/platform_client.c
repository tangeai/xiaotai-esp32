/*
 * ThingConnect 平台传输 adapter。
 *
 * 正常上线：服务发现 -> SNTP -> HMAC 设备登录 -> MQTT token -> 永久 MQTT。
 * 首次绑定：设备上报 -> 显示验证码 -> 临时 MQTT auth_grant -> QoS1 ACK。
 * 业务 HTTP：调用者复制请求到固定队列，由 request_task 串行执行和回调。
 *
 * 正常在线阶段的 MQTT token 只保存在本模块内存中。首次绑定会把设备密钥
 * 放入 provision result 交给组合根持久化；两者都不会写入日志。
 */
#include "platform_client.h"
#include "platform_http_trace.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdatomic.h>
#include <errno.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/apps/sntp_opts.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mqtt_client.h"

#define PLATFORM_HTTP_BODY_MAX 8192
#define PLATFORM_REQUEST_QUEUE_DEPTH 4
#define PLATFORM_REQUEST_PATH_MAX 128
#define PLATFORM_REQUEST_BODY_MAX 2048
#define PLATFORM_SIGNAL_MAX 4096
#define PLATFORM_DEFAULT_HTTP_TIMEOUT_MS 15000U
#define PLATFORM_TTS_HTTP_TIMEOUT_MS 20000U
/* 服务端最坏情况：中文前缀 + 6 个最长数字 + 6 段静音 = 157824 bytes。 */
#define PLATFORM_TTS_PCM_MAX_BYTES (160U * 1024U)
#define PLATFORM_TTS_DOWNLOAD_ATTEMPTS 2U
/* 必须与组合根的联调 profile 一致，避免空配置时静默切回 HTTPS。 */
#define PLATFORM_DEFAULT_DISCOVERY CONFIG_XIAOTAI_DISCOVERY_URL
#define PLATFORM_DEFAULT_PROVISION_TIMEOUT_SECONDS 190U
#define EXPERIENCE_PLATFORM_URL CONFIG_XIAOTAI_PORTAL_URL
#define PROVISION_DONE_BIT BIT0
#define PROVISION_ERROR_BIT BIT1
#define PROVISION_READY_BIT BIT2

/* 服务发现结果；生成的起步工程会裁剪未使用的服务字段。 */
typedef struct {
    char device[256];
    char ai[256];
    char mqtt[256];
    char tirtc[256];
    char call[256];
    char voip[256];
} platform_services_t;

typedef struct {
    uint32_t started_ms;
    uint32_t prep_ms;
    uint32_t conn_ms;
    uint32_t head_ms;
    uint32_t first_ms;
    uint32_t done_ms;
    unsigned connections;
    bool reused;
    bool retained;
    bool redirected;
    int nodelay;
    platform_http_trace_t io;
} http_timing_t;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
    bool content_type_pcm;
    http_timing_t *timing;
} http_output_t;

/* Owned by the existing PSRAM request-loop stack, never by MQTT/runtime.
 * Only reuse a plain-HTTP connection inside a queued burst, not while idle. */
typedef struct {
    esp_http_client_handle_t client;
    char base[256];
    uint32_t epoch;
} http_session_t;

/* 请求按值进入固定队列，避免调用者栈内字符串在异步执行前失效。 */
typedef struct {
    platform_service_t service;
    uint32_t epoch;
    char path[PLATFORM_REQUEST_PATH_MAX];
    bool post;
    bool metadata_only;
    char body[PLATFORM_REQUEST_BODY_MAX];
    unsigned timeout_ms;
    platform_response_callback_t callback;
    void *user_data;
    uint32_t queued_at_ms; /* Timing only; stays in the existing PSRAM slot. */
} platform_request_t;

/* 首次绑定临时 MQTT 的全部可变状态，生命周期限制在 provision 调用内。 */
typedef struct {
    EventGroupHandle_t events;
    esp_mqtt_client_handle_t mqtt;
    char temp_client_id[65];
    char device_id[65];
    char device_secret[257];
    char message[PLATFORM_SIGNAL_MAX];
    size_t message_size;
    int message_id;
    int subscribe_message_id;
    int ack_message_id;
    platform_verification_prompt_cancel_t prompt_cancel_callback;
    void *prompt_user_data;
} provision_mqtt_t;

static const char *TAG = "platform_client";

/* 正常在线阶段的进程级状态；模块只支持一个设备实例。 */
/* Task-context payloads only. Keep atomics, handles and locks in internal RAM. */
static EXT_RAM_BSS_ATTR platform_services_t s_services;
static char s_device_id[65];
static char s_device_secret[257];
static char s_client_id[129];
static char s_mac_address[24];
static EXT_RAM_BSS_ATTR char s_mqtt_token[1024];
static platform_request_t *s_request_pool;
static char *s_request_response;
static QueueHandle_t s_request_ready_queue;
static QueueHandle_t s_request_free_queue;
static esp_mqtt_client_handle_t s_mqtt;
/* External users serialize handle lifetime, not just pointer visibility.
 * MQTT callbacks use event->client and never acquire this lock: stop joins them. */
static StaticSemaphore_t s_mqtt_lifecycle_storage;
static SemaphoreHandle_t s_mqtt_lifecycle;
static atomic_bool s_ready;
/* 0=online, 1=reconcile after lost signal, 2=confirmed unbind. */
static atomic_uint s_rebind_request;
static atomic_bool s_rebind_quiesced;
static atomic_uint s_epoch;
static uint32_t s_response_epoch;
static atomic_bool s_binding_retry;
static volatile bool s_request_worker_ready;
static atomic_bool s_mqtt_connected;
static platform_online_callback_t s_online_callback;
static void *s_online_user_data;
static volatile bool s_provisioning;
static bool s_services_ready;
static char s_verification_code[17];
static platform_signal_callback_t s_signal_callback;
static void *s_signal_user_data;
static EXT_RAM_BSS_ATTR char s_mqtt_message[PLATFORM_SIGNAL_MAX];
static size_t s_mqtt_message_size;
static int s_mqtt_message_id = -1;

/* ===== 有界 HTTP 与 JSON 基础函数 ===== */

static char *http_response_alloc(void)
{
    return heap_caps_malloc(PLATFORM_HTTP_BODY_MAX,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* esp_http_client 可能分片回调；按容量拼接，溢出时整次请求失败。 */
static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_output_t *output = event->user_data;
    http_timing_t *timing = output == NULL ? NULL : output->timing;
    if (timing != NULL) {
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000) - timing->started_ms;
        if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
            timing->conn_ms = elapsed;
            timing->connections++;
            /* IDF writes headers and a small JSON body separately. Disable
             * Nagle on this HTTP socket only, before either write occurs. */
            int saved_errno = errno;
            int fd = esp_http_client_get_socket(event->client);
            int enabled = 1;
            timing->nodelay = fd >= 0 &&
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0
                    ? 1 : 0;
            if (timing->nodelay == 0) ESP_LOGW(TAG, "HTTP TCP_NODELAY unavailable");
            errno = saved_errno;
        } else if (event->event_id == HTTP_EVENT_HEADERS_SENT) {
            timing->head_ms = elapsed;
        } else if (event->event_id == HTTP_EVENT_ON_HEADER) {
            if (timing->first_ms == UINT32_MAX) timing->first_ms = elapsed;
            /* Auto redirects do not emit HTTP_EVENT_REDIRECT to this handler. */
            if (event->header_key != NULL &&
                strcasecmp(event->header_key, "Location") == 0) timing->redirected = true;
        } else if (event->event_id == HTTP_EVENT_ON_DATA) {
            if (timing->first_ms == UINT32_MAX) timing->first_ms = elapsed;
        } else if (event->event_id == HTTP_EVENT_ON_FINISH) {
            timing->done_ms = elapsed;
        }
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL ||
        event->data_len <= 0) {
        return ESP_OK;
    }
    if (output == NULL || output->overflow ||
        output->length + (size_t)event->data_len >= output->capacity) {
        if (output != NULL) {
            output->overflow = true;
        }
        return ESP_OK;
    }
    memcpy(output->data + output->length, event->data, (size_t)event->data_len);
    output->length += (size_t)event->data_len;
    output->data[output->length] = '\0';
    return ESP_OK;
}

static void http_session_close(http_session_t *session)
{
    if (session->client != NULL) {
        /* The previous perform's stack output has already gone out of scope. */
        (void)esp_http_client_set_user_data(session->client, NULL);
        (void)esp_http_client_cleanup(session->client);
    }
    memset(session, 0, sizeof(*session));
}

static bool http_session_socket_idle(esp_http_client_handle_t client)
{
    int fd = esp_http_client_get_socket(client);
    if (fd < 0 || fd >= FD_SETSIZE) return false;
    fd_set read_fds, error_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&error_fds);
    FD_SET(fd, &read_fds);
    FD_SET(fd, &error_fds);
    struct timeval timeout = {0};
    /* FIN, unread data or socket errors invalidate reuse. Never consume bytes
     * here or replay a failed POST: room creation is not idempotent. */
    return select(fd + 1, &read_fds, NULL, &error_fds, &timeout) == 0;
}

static esp_err_t http_request_perform(const char *url,
                              const char *json_body,
                              const char *bearer,
                              const char *const header_names[],
                              const char *const header_values[],
                              size_t header_count,
                              char *response,
                              size_t response_size,
                              unsigned timeout_ms,
                              int *status,
                              http_session_t *session,
                              const char *base,
                              http_timing_t *timing)
{
    /* response 由调用者提供，函数返回时总是以 NUL 结尾或报告容量错误。 */
    http_output_t output = {
        .data = response,
        .capacity = response_size,
        .timing = timing,
    };
    response[0] = '\0';
    if (status != NULL) *status = 0;
    if (timing != NULL) {
        *timing = (http_timing_t){
            .conn_ms = UINT32_MAX, .head_ms = UINT32_MAX,
            .first_ms = UINT32_MAX, .done_ms = UINT32_MAX,
            .nodelay = -1,
        };
        timing->started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    bool can_reuse = session != NULL && base != NULL && timing != NULL &&
                     header_count == 0 && strncmp(base, "http://", 7) == 0 &&
                     strlen(base) < sizeof(session->base);
    if (session != NULL && session->client != NULL &&
        (!can_reuse || strcmp(session->base, base) != 0 ||
         !http_session_socket_idle(session->client))) {
        http_session_close(session);
    }
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &output,
        .timeout_ms = (int)(timeout_ms == 0
                                ? PLATFORM_DEFAULT_HTTP_TIMEOUT_MS
                                : timeout_ms),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = session == NULL ? NULL : session->client;
    esp_err_t err;
    if (client != NULL) {
        timing->reused = true;
        err = esp_http_client_set_url(client, url);
        if (err != ESP_OK) goto done;
        err = esp_http_client_set_user_data(client, &output);
        if (err != ESP_OK) goto done;
        err = esp_http_client_set_timeout_ms(client, config.timeout_ms);
        if (err != ESP_OK) goto done;
        err = esp_http_client_reset_redirect_counter(client);
        if (err != ESP_OK) goto done;
    } else {
        client = esp_http_client_init(&config);
    }
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_http_client_set_method(client, json_body != NULL ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (err != ESP_OK) goto done;
    err = esp_http_client_set_post_field(client, json_body, json_body == NULL ? 0 : strlen(json_body));
    /* IDF returns NOT_FOUND when clearing an already absent Content-Type. */
    if (json_body == NULL && err == ESP_ERR_NOT_FOUND) err = ESP_OK;
    if (err != ESP_OK) goto done;
    if (json_body != NULL) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (err != ESP_OK) goto done;
    }
    char authorization[1100];
    if (bearer != NULL && bearer[0] != '\0') {
        int count = snprintf(authorization, sizeof(authorization), "Bearer %s", bearer);
        if (count <= 0 || (size_t)count >= sizeof(authorization)) {
            err = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        err = esp_http_client_set_header(client, "Authorization", authorization);
    } else {
        err = esp_http_client_set_header(client, "Authorization", NULL);
        if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
    }
    if (err != ESP_OK) goto done;
    for (size_t i = 0; i < header_count; ++i) {
        err = esp_http_client_set_header(client, header_names[i], header_values[i]);
        if (err != ESP_OK) goto done;
    }

    if (timing != NULL) {
        timing->prep_ms = (uint32_t)(esp_timer_get_time() / 1000) - timing->started_ms;
        timing->started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    if (timing != NULL) platform_http_trace_begin(&timing->io);
    err = esp_http_client_perform(client);
    if (timing != NULL) platform_http_trace_end(&timing->io);
    if (status != NULL) {
        *status = esp_http_client_get_status_code(client);
    }
    if (err == ESP_OK && output.overflow) {
        err = ESP_ERR_INVALID_SIZE;
    }
done:
    /* post_field/user_data are borrowed pointers. Detach before their request
     * slot and stack are reused, even when the network operation failed. */
    (void)esp_http_client_set_user_data(client, NULL);
    (void)esp_http_client_set_post_field(client, NULL, 0);
    int response_status = esp_http_client_get_status_code(client);
    bool retain = can_reuse && err == ESP_OK && !timing->redirected &&
                  response_status >= 200 && response_status < 300 &&
                  esp_http_client_get_socket(client) >= 0;
    if (retain) {
        session->client = client;
        (void)snprintf(session->base, sizeof(session->base), "%s", base);
        timing->retained = true;
    } else {
        (void)esp_http_client_cleanup(client);
        if (session != NULL) memset(session, 0, sizeof(*session));
    }
    return err;
}

/* Log only known endpoint names, never a query, room/device ID or signed URL. */
static const char *http_api_name(const char *url_or_path)
{
    if (url_or_path == NULL) return "unknown";
    const char *scheme = strstr(url_or_path, "://");
    const char *path = scheme == NULL ? url_or_path : strchr(scheme + 3, '/');
    if (path == NULL) return "unknown";
    size_t length = strcspn(path, "?#");
    static const char *const names[] = {
        "/services", "/v1/device/token", "/v1/device/report", "/v1/device/tts",
        "/v1/ai/token", "/v1/call/device/contacts", "/v1/call/device/info",
        "/v1/call/request", "/v1/call/room", "/v1/call/cancel",
        "/v1/call/hangup", "/v1/call/reject", "/v1/voip/device/profile",
        "/v1/voip/device/call", "/v1/wxvoip/reject",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strlen(names[i]) == length && strncmp(path, names[i], length) == 0) return names[i];
    }
    return "other";
}

static void http_log_result(const char *path, bool post, uint32_t request_ms,
                            uint32_t queue_ms, uint32_t total_ms, esp_err_t err,
                            int status, const http_timing_t *timing)
{
    const platform_http_trace_t *io = &timing->io;
    /* Redirect/auth exchanges cannot be represented by one response interval.
     * net_ms includes TCP/TLS/local scheduling, wait_ms network + server time;
     * neither field claims to measure pure server execution. */
    bool single = io->enabled && !timing->redirected && io->connect_calls <= 1 &&
                  io->first_rx_ms != UINT32_MAX && io->write_ms != UINT32_MAX &&
                  io->first_rx_ms >= io->write_ms;
    long wait_ms = single ? (long)(io->first_rx_ms - io->write_ms) : -1;
    long net_ms = io->enabled && io->connect_ms >= io->dns_ms
                      ? (long)(io->connect_ms - io->dns_ms) : -1;
    if (strcmp(path, "/v1/ai/token") != 0 && !timing->reused && !timing->retained &&
        queue_ms < 500U && total_ms < 500U && err == ESP_OK && status >= 200 && status < 300) return;
    ESP_LOGI(TAG,
             "CONN http done: api=%s method=%s req_ms=%lu q_ms=%lu http_ms=%lu rc=%d status=%d reuse=%d keep=%d new_conn=%u prep=%lu conn=%ld head=%ld first=%ld done=%ld redir=%d dns_ms=%ld dns_n=%lu dns_rc=%d net_ms=%ld tx_done=%ld rx_first=%ld wait_ms=%ld tx=%lu rx=%lu nd=%d",
             path, post ? "POST" : "GET", (unsigned long)request_ms,
             (unsigned long)queue_ms, (unsigned long)total_ms, (int)err, status,
             (int)timing->reused, (int)timing->retained, timing->connections,
             (unsigned long)timing->prep_ms, (long)(int32_t)timing->conn_ms,
             (long)(int32_t)timing->head_ms, (long)(int32_t)timing->first_ms,
             (long)(int32_t)timing->done_ms, (int)timing->redirected,
             io->enabled ? (long)io->dns_ms : -1, (unsigned long)io->dns_calls, io->dns_rc,
             net_ms, io->enabled ? (long)(int32_t)io->write_ms : -1,
             io->enabled ? (long)(int32_t)io->first_rx_ms : -1, wait_ms,
             (unsigned long)io->tx_bytes, (unsigned long)io->rx_bytes, timing->nodelay);
}

/* Discovery, signed login and binding remain one-shot with no shared handle. */
static esp_err_t http_request(const char *url, const char *json_body, const char *bearer,
                             const char *const header_names[], const char *const header_values[],
                             size_t header_count, char *response, size_t response_size,
                             unsigned timeout_ms, int *status)
{
    http_timing_t timing;
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);
    int response_status = 0;
    esp_err_t err = http_request_perform(url, json_body, bearer, header_names, header_values,
                                header_count, response, response_size, timeout_ms, &response_status,
                                NULL, NULL, &timing);
    if (status != NULL) *status = response_status;
    http_log_result(http_api_name(url), json_body != NULL, start, 0,
                    (uint32_t)(esp_timer_get_time() / 1000) - start, err, response_status, &timing);
    return err;
}

/* 原始 PCM 可以包含 NUL，不能复用以 NUL 结尾的 JSON 收集器。 */
static esp_err_t http_binary_event(esp_http_client_event_t *event)
{
    http_output_t *output = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && output != NULL &&
        event->header_key != NULL && event->header_value != NULL &&
        strcasecmp(event->header_key, "Content-Type") == 0) {
        output->content_type_pcm =
            strncasecmp(event->header_value, "audio/pcm", 9) == 0;
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL ||
        event->data_len <= 0) {
        return ESP_OK;
    }
    if (output == NULL || output->overflow ||
        output->length + (size_t)event->data_len > output->capacity) {
        if (output != NULL) {
            output->overflow = true;
        }
        return ESP_OK;
    }
    memcpy(output->data + output->length, event->data, (size_t)event->data_len);
    output->length += (size_t)event->data_len;
    return ESP_OK;
}

static esp_err_t http_binary_get(const char *url,
                                 const char *bearer,
                                 uint8_t *response,
                                 size_t response_size,
                                 size_t *response_length,
                                 int *status)
{
    if (url == NULL || bearer == NULL || bearer[0] == '\0' || response == NULL ||
        response_size == 0U || response_length == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    http_output_t output = {
        .data = (char *)response,
        .capacity = response_size,
    };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_binary_event,
        .user_data = &output,
        .timeout_ms = PLATFORM_TTS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char authorization[1100] = {0};
    int count = snprintf(authorization, sizeof(authorization), "Bearer %s", bearer);
    if (count <= 0 || (size_t)count >= sizeof(authorization)) {
        mbedtls_platform_zeroize(authorization, sizeof(authorization));
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_err_t err = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    bool content_type_ok = *status != 200 || output.content_type_pcm;
    mbedtls_platform_zeroize(authorization, sizeof(authorization));
    esp_http_client_cleanup(client);
    *response_length = output.length;
    if (err == ESP_OK && output.overflow) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && !content_type_ok) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

static bool json_copy_string(const cJSON *object,
                             const char *name,
                             char *destination,
                             size_t destination_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        item->valuestring[0] == '\0' || strlen(item->valuestring) >= destination_size) {
        return false;
    }
    (void)snprintf(destination, destination_size, "%s", item->valuestring);
    return true;
}

static esp_err_t discover_services(const char *url)
{
    /* 服务地址属于运行时发现结果，不在固件中分别硬编码。 */
    char *response = http_response_alloc();
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t err = http_request(url, NULL, NULL, NULL, NULL, 0,
                                 response, PLATFORM_HTTP_BODY_MAX,
                                 PLATFORM_DEFAULT_HTTP_TIMEOUT_MS, &status);
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "service discovery failed: %s HTTP=%d",
                 esp_err_to_name(err), status);
        heap_caps_free(response);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    cJSON *root = cJSON_Parse(response);
    heap_caps_free(response);
    bool ok = root != NULL &&
              json_copy_string(root, "device-srv", s_services.device,
                               sizeof(s_services.device)) &&
              json_copy_string(root, "ai-srv", s_services.ai,
                               sizeof(s_services.ai)) &&
              json_copy_string(root, "mqtt-srv", s_services.mqtt,
                               sizeof(s_services.mqtt)) &&
              json_copy_string(root, "call-srv", s_services.call,
                               sizeof(s_services.call)) &&
              json_copy_string(root, "voip-srv", s_services.voip,
                               sizeof(s_services.voip));
    if (root != NULL) {
        (void)json_copy_string(root, "tirtc-srv", s_services.tirtc,
                               sizeof(s_services.tirtc));
    }
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGE(TAG, "service discovery response is incomplete");
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_services_ready = true;
    ESP_LOGI(TAG, "service discovery complete (credentials and token hidden)");
    return ESP_OK;
}

static esp_err_t hmac_signature_with_key(const char *key,
                                         const char *text,
                                         char *base64,
                                         size_t base64_size)
{
    unsigned char digest[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (key == NULL || key[0] == '\0' || info == NULL ||
        mbedtls_md_hmac(info,
                        (const unsigned char *)key,
                        strlen(key),
                        (const unsigned char *)text,
                        strlen(text),
                        digest) != 0) {
        return ESP_FAIL;
    }
    size_t encoded = 0;
    if (mbedtls_base64_encode((unsigned char *)base64,
                              base64_size,
                              &encoded,
                              digest,
                              sizeof(digest)) != 0 || encoded >= base64_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    base64[encoded] = '\0';
    return ESP_OK;
}

static esp_err_t hmac_signature(const char *text, char *base64, size_t base64_size)
{
    return hmac_signature_with_key(s_device_secret, text, base64, base64_size);
}

static esp_err_t obtain_mqtt_token(void)
{
    /* 时间戳、随机 nonce 和 HMAC 防止设备登录请求被直接重放。 */
    char timestamp[24];
    (void)snprintf(timestamp, sizeof(timestamp), "%lld", (long long)time(NULL));
    char nonce[17];
    (void)snprintf(nonce, sizeof(nonce), "%08lx%08lx",
                   (unsigned long)esp_random(), (unsigned long)esp_random());
    char signed_text[384];
    int text_length = snprintf(signed_text, sizeof(signed_text), "%s%s%s",
                               s_device_id, timestamp, nonce);
    char signature[64];
    if (text_length <= 0 || (size_t)text_length >= sizeof(signed_text) ||
        hmac_signature(signed_text, signature, sizeof(signature)) != ESP_OK) {
        return ESP_FAIL;
    }

    char url[384];
    int url_length = snprintf(url, sizeof(url), "%s/v1/device/token", s_services.device);
    if (url_length <= 0 || (size_t)url_length >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const char *names[] = {
        "X-Device-Id", "X-Timestamp", "X-Nonce", "X-Mac", "X-Signature",
    };
    const char *values[] = {
        s_device_id, timestamp, nonce, s_mac_address, signature,
    };
    char *response = http_response_alloc();
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t err = http_request(url, "", NULL, names, values, 5,
                                 response, PLATFORM_HTTP_BODY_MAX,
                                 PLATFORM_DEFAULT_HTTP_TIMEOUT_MS, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设备 token HTTP 请求失败: %s",
                 esp_err_to_name(err));
        heap_caps_free(response);
        return err;
    }
    cJSON *root = cJSON_Parse(response);
    heap_caps_free(response);
    const cJSON *code = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *data = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "data");
    bool ok = status == 200 && cJSON_IsNumber(code) && code->valueint == 200 &&
              cJSON_IsObject(data) &&
              json_copy_string(data, "mqtt_token", s_mqtt_token,
                               sizeof(s_mqtt_token));
    int response_code = cJSON_IsNumber(code) ? code->valueint : -1;
    char response_message[256] = "服务器未返回错误说明";
    if (root != NULL) {
        (void)json_copy_string(root, "msg", response_message,
                               sizeof(response_message));
    }
    cJSON_Delete(root);
    if (!ok) {
        if (response_code == 6006) {
            ESP_LOGW(TAG, "设备已被服务端解绑，需要重新进行验证码绑定");
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGE(TAG, "设备 token 响应失败 HTTP=%d code=%d msg=%s",
                 status, response_code, response_message);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "device token obtained (value hidden)");
    return ESP_OK;
}

static const char *service_base(platform_service_t service)
{
    switch (service) {
    case PLATFORM_SERVICE_DEVICE: return s_services.device;
    case PLATFORM_SERVICE_AI: return s_services.ai;
    case PLATFORM_SERVICE_CALL: return s_services.call;
    case PLATFORM_SERVICE_VOIP: return s_services.voip;
    default: return NULL;
    }
}

static void process_request(const platform_request_t *request,
                            char *response,
                            size_t response_size,
                            http_session_t *session)
{
    /* 请求任务是业务 HTTP 的唯一执行者，callback 也在该任务中同步调用。 */
    bool trace_token = request->service == PLATFORM_SERVICE_AI &&
                       strcmp(request->path, "/v1/ai/token") == 0;
    uint32_t dequeued_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t queue_ms = dequeued_ms - request->queued_at_ms;
    if (trace_token) {
        ESP_LOGI(TAG, "CONN token_http begin: api=/v1/ai/token method=%s req_ms=%lu q_ms=%lu",
                 request->post ? "POST" : "GET",
                 (unsigned long)request->queued_at_ms, (unsigned long)queue_ms);
    }
    s_response_epoch = request->epoch;
    if (session->epoch != request->epoch) http_session_close(session);
    if (!platform_client_ready() || request->epoch != platform_client_epoch()) {
        http_session_close(session);
        if (trace_token) {
            ESP_LOGW(TAG, "CONN token_http skipped: req_ms=%lu stale=1",
                     (unsigned long)request->queued_at_ms);
        }
        if (request->callback != NULL) request->callback(NULL, request->user_data);
        return;
    }
    const char *base = service_base(request->service);
    char url[512];
    int url_length = base == NULL ? -1 :
        snprintf(url, sizeof(url), "%s%s", base, request->path);
    int status = 0;
    http_timing_t timing = {
        .conn_ms = UINT32_MAX, .head_ms = UINT32_MAX,
        .first_ms = UINT32_MAX, .done_ms = UINT32_MAX,
        .nodelay = -1,
    };
    uint32_t http_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    esp_err_t err = url_length <= 0 || (size_t)url_length >= sizeof(url)
                        ? ESP_ERR_INVALID_SIZE
                        : http_request_perform(url,
                                       request->post ? request->body : NULL,
                                       s_mqtt_token,
                                       NULL,
                                       NULL,
                                       0,
                                       response,
                                       response_size,
                                       request->timeout_ms,
                                       &status,
                                       session,
                                       base,
                                       &timing);
    session->epoch = request->epoch;
    /* Runtime may submit TiRTC immediately from a completed AI/call response.
     * Release HTTP first; only metadata bursts can keep it for queued work.
     * No idle socket, new worker, or TLS heap is retained by this optimization. */
    if (err != ESP_OK || !request->metadata_only ||
        !platform_client_ready() || request->epoch != platform_client_epoch() ||
        uxQueueMessagesWaiting(s_request_ready_queue) == 0) {
        http_session_close(session);
        timing.retained = false;
    }
    uint32_t http_ms = (uint32_t)(esp_timer_get_time() / 1000) - http_started_ms;
    const char *api = http_api_name(request->path);
    http_log_result(api, request->post, request->queued_at_ms, queue_ms, http_ms, err, status, &timing);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "platform request %s failed: %s HTTP=%d",
                 api, esp_err_to_name(err), status);
        if (request->callback != NULL) {
            request->callback(NULL, request->user_data);
        }
        return;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "platform request %s returned HTTP=%d",
                 api, status);
    }
    if (request->callback != NULL) {
        /* A request already in flight may finish after unbind. Do not deliver
         * the previous owner's contacts or tokens into the new binding. */
        request->callback(platform_client_ready() && request->epoch == platform_client_epoch()
                              ? response : NULL, request->user_data);
    }
}

static void publish_heartbeat(unsigned sequence)
{
    if (s_mqtt_lifecycle == NULL || xSemaphoreTake(s_mqtt_lifecycle, 0) != pdTRUE) return;
    if (!platform_client_ready() || !s_mqtt_connected || s_mqtt == NULL) {
        xSemaphoreGive(s_mqtt_lifecycle);
        return;
    }
    char topic[128];
    char body[128];
    (void)snprintf(topic, sizeof(topic), "device/sn_%s/up", s_device_id);
    int length = snprintf(body, sizeof(body),
                          "{\"type\":\"heartbeat\",\"seq\":%u,\"ts\":%lld}",
                          sequence, (long long)time(NULL));
    if (length > 0 && (size_t)length < sizeof(body)) {
        (void)esp_mqtt_client_publish(s_mqtt, topic, body, length, 0, 0);
    }
    xSemaphoreGive(s_mqtt_lifecycle);
}

static void request_loop(void)
{
    /* 启动任务完成 TiRTC TLS 后直接转为请求循环，避免两个 24 KiB 栈重叠。 */
    unsigned heartbeat_sequence = 0;
    http_session_t http_session = {0};
    int64_t next_heartbeat_ms = esp_timer_get_time() / 1000 + 30000;
    for (;;) {
        if (atomic_load(&s_rebind_quiesced)) {
            http_session_close(&http_session);
            return;
        }
        if (!platform_client_ready() || uxQueueMessagesWaiting(s_request_ready_queue) == 0) {
            http_session_close(&http_session);
        }
        uint8_t slot = 0;
        if (xQueueReceive(s_request_ready_queue,
                          &slot,
                          pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (slot < PLATFORM_REQUEST_QUEUE_DEPTH) {
                platform_request_t *request = &s_request_pool[slot];
                process_request(request,
                                s_request_response,
                                PLATFORM_HTTP_BODY_MAX,
                                &http_session);
                memset(request, 0, sizeof(*request));
                if (xQueueSend(s_request_free_queue, &slot, 0) != pdTRUE) {
                    ESP_LOGE(TAG, "platform request slot %u could not be returned",
                             (unsigned)slot);
                }
            }
        }
        int64_t current_ms = esp_timer_get_time() / 1000;
        if (current_ms >= next_heartbeat_ms) {
            publish_heartbeat(++heartbeat_sequence);
            next_heartbeat_ms = current_ms + 30000;
        }
    }
}

static void dispatch_mqtt_signal(const char *json, size_t length)
{
    if (s_signal_callback != NULL) {
        s_signal_callback(json, length, s_signal_user_data);
    }
}

static void notify_platform_online(void)
{
    /* 回调只用于投递上层状态事件，不能在 MQTT 事件上下文做网络 I/O。 */
    if (platform_client_ready() && s_mqtt_connected && s_request_worker_ready && s_online_callback != NULL) {
        s_online_callback(s_online_user_data);
    }
}

static void mqtt_event(void *handler_args,
                       esp_event_base_t base,
                       int32_t event_id,
                       void *event_data)
{
    /* MQTT payload 可能分片；只在完整且未超限时向上层分发。 */
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        char command_topic[128];
        char notify_topic[128];
        (void)snprintf(command_topic, sizeof(command_topic),
                       "device/sn_%s/cmd", s_device_id);
        (void)snprintf(notify_topic, sizeof(notify_topic),
                       "device/sn_%s/notify", s_device_id);
        (void)esp_mqtt_client_subscribe(event->client, command_topic, 1);
        (void)esp_mqtt_client_subscribe(event->client, notify_topic, 1);
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected and device topics subscribed");
        notify_platform_online();
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected; client will reconnect");
    } else if (event_id == MQTT_EVENT_DATA) {
        if (event->current_data_offset == 0) {
            s_mqtt_message_size = 0;
            s_mqtt_message_id = event->msg_id;
            if (event->topic != NULL && event->topic_len > 0) {
                char topic[160];
                size_t topic_length = (size_t)event->topic_len < sizeof(topic) - 1U
                                          ? (size_t)event->topic_len
                                          : sizeof(topic) - 1U;
                memcpy(topic, event->topic, topic_length);
                topic[topic_length] = '\0';
                if (strstr(topic, "/cmd") != NULL) {
                    char ack_topic[128];
                    (void)snprintf(ack_topic, sizeof(ack_topic),
                                   "device/sn_%s/ack", s_device_id);
                    (void)esp_mqtt_client_publish(event->client,
                                                  ack_topic,
                                                  "{\"ack\":true}",
                                                  12,
                                                  1,
                                                  0);
                }
            }
        }
        bool valid_fragment = event->msg_id == s_mqtt_message_id &&
                              event->data_len >= 0 &&
                              s_mqtt_message_size + (size_t)event->data_len <
                                  sizeof(s_mqtt_message);
        if (!valid_fragment) {
            s_mqtt_message_id = -1;
            s_mqtt_message_size = 0;
            ESP_LOGW(TAG, "dropping oversized or fragmented MQTT signal");
            return;
        }
        memcpy(s_mqtt_message + s_mqtt_message_size,
               event->data,
               (size_t)event->data_len);
        s_mqtt_message_size += (size_t)event->data_len;
        if (s_mqtt_message_size == (size_t)event->total_data_len) {
            s_mqtt_message[s_mqtt_message_size] = '\0';
            dispatch_mqtt_signal(s_mqtt_message, s_mqtt_message_size);
            s_mqtt_message_id = -1;
            s_mqtt_message_size = 0;
        }
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGW(TAG, "MQTT transport error");
    }
}

static esp_err_t start_mqtt(bool binding_owner)
{
    /* First creation runs in the single startup task, before s_ready/worker. */
    if (s_mqtt_lifecycle == NULL)
        s_mqtt_lifecycle = xSemaphoreCreateMutexStatic(&s_mqtt_lifecycle_storage);
    if (xSemaphoreTake(s_mqtt_lifecycle, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    /* Recheck after locking: a queued realtime-resume must not recreate the
     * old client after the binding owner has stopped it. */
    if (!binding_owner && !platform_client_ready()) {
        xSemaphoreGive(s_mqtt_lifecycle);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mqtt != NULL) { xSemaphoreGive(s_mqtt_lifecycle); return ESP_OK; }
    esp_mqtt_client_config_t config = {
        .broker.address.uri = s_services.mqtt,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = s_device_id,
        .credentials.client_id = s_client_id,
        .credentials.authentication.password = s_mqtt_token,
        .session.keepalive = 60,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .network.reconnect_timeout_ms = 3000,
    };
    s_mqtt = esp_mqtt_client_init(&config);
    if (s_mqtt == NULL) {
        xSemaphoreGive(s_mqtt_lifecycle);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_mqtt,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_event,
                                                   NULL);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(s_mqtt);
    }
    if (err != ESP_OK) {
        (void)esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
    xSemaphoreGive(s_mqtt_lifecycle);
    return err;
}

static esp_err_t sync_clock(void)
{
    /*
     * 默认配置立即启动 lwIP SNTP，后续由其后台定时刷新。已有可信时间时
     * 不阻塞平台注册；冷启动时等待首个结果，避免使用错误时间计算 HMAC。
     */
    time_t current = time(NULL);
    bool time_was_valid = current > 1700000000;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2,
        ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (time_was_valid) {
        ESP_LOGI(TAG,
                 "SNTP refresh started asynchronously "
                 "(primary=ntp.aliyun.com fallback=pool.ntp.org)");
        return ESP_OK;
    }
    /* lwIP switches peers only after SNTP_RECV_TIMEOUT (15 s in IDF 5.5.4).
     * Cover both peers plus one resolution/start window; the former 10 s
     * deadline abandoned first binding before the fallback could answer.
     * This blocks only the startup worker, not LVGL or audio capture. */
    uint32_t timeout_ms = (config.num_of_servers + 1U) * SNTP_RECV_TIMEOUT;
#if SNTP_STARTUP_DELAY && defined(CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY)
    timeout_ms += CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY;
#endif
    const int64_t started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "waiting for network clock: timeout_ms=%lu peers=%u",
             (unsigned long)timeout_ms, (unsigned)config.num_of_servers);
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    const unsigned long elapsed_ms = (unsigned long)((esp_timer_get_time() - started_us) / 1000);
    if (time(NULL) > 1700000000) {
        ESP_LOGI(TAG, "network clock synchronized: elapsed_ms=%lu", elapsed_ms);
        return ESP_OK;
    }
    if (err == ESP_OK) err = ESP_ERR_INVALID_RESPONSE;
    ESP_LOGE(TAG, "network clock unavailable: elapsed_ms=%lu error=%s",
             elapsed_ms, esp_err_to_name(err));
    return err;
}

typedef struct {
    char code[17];
    char temp_token[1024];
    char temp_client_id[65];
} provision_report_t;

/* ===== 首次验证码绑定 / 服务端解绑后重绑 ===== */

static esp_err_t report_for_provision(const platform_provision_config_t *config,
                                      provision_report_t *report)
{
    /* existing_device_* 同时存在时给上报加签；首次绑定只上报 MAC。 */
    cJSON *body_root = cJSON_CreateObject();
    if (body_root == NULL ||
        !cJSON_AddStringToObject(body_root, "mac", config->mac_address)) {
        cJSON_Delete(body_root);
        return ESP_ERR_NO_MEM;
    }
    char *body = cJSON_PrintUnformatted(body_root);
    cJSON_Delete(body_root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const char *header_names[4] = {0};
    const char *header_values[4] = {0};
    size_t header_count = 0;
    char timestamp[24] = {0};
    char nonce[17] = {0};
    char signature[64] = {0};
    bool signed_report = config->existing_device_id != NULL &&
                         config->existing_device_id[0] != '\0' &&
                         config->existing_device_secret != NULL &&
                         config->existing_device_secret[0] != '\0';
    if (signed_report) {
        (void)snprintf(timestamp, sizeof(timestamp), "%lld", (long long)time(NULL));
        (void)snprintf(nonce, sizeof(nonce), "%08lx%08lx",
                       (unsigned long)esp_random(), (unsigned long)esp_random());
        char signed_text[384];
        int signed_length = snprintf(signed_text,
                                     sizeof(signed_text),
                                     "%s%s%s",
                                     config->existing_device_id,
                                     timestamp,
                                     nonce);
        if (signed_length <= 0 || (size_t)signed_length >= sizeof(signed_text) ||
            hmac_signature_with_key(config->existing_device_secret,
                                    signed_text,
                                    signature,
                                    sizeof(signature)) != ESP_OK) {
            free(body);
            return ESP_FAIL;
        }
        header_names[0] = "X-Device-Id";
        header_values[0] = config->existing_device_id;
        header_names[1] = "X-Timestamp";
        header_values[1] = timestamp;
        header_names[2] = "X-Nonce";
        header_values[2] = nonce;
        header_names[3] = "X-Signature";
        header_values[3] = signature;
        header_count = 4;
    }

    char url[384];
    int url_length = snprintf(url, sizeof(url), "%s/v1/device/report",
                              s_services.device);
    char *response = http_response_alloc();
    if (response == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t err = url_length <= 0 || (size_t)url_length >= sizeof(url)
                        ? ESP_ERR_INVALID_SIZE
                        : http_request(url,
                                       body,
                                       NULL,
                                       header_names,
                                       header_values,
                                       header_count,
                                       response,
                                       PLATFORM_HTTP_BODY_MAX,
                                       PLATFORM_DEFAULT_HTTP_TIMEOUT_MS,
                                       &status);
    free(body);
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "device report failed: %s HTTP=%d",
                 esp_err_to_name(err), status);
        heap_caps_free(response);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    cJSON *root = cJSON_Parse(response);
    heap_caps_free(response);
    const cJSON *response_code = root == NULL
                                     ? NULL
                                     : cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *data = root == NULL
                            ? NULL
                            : cJSON_GetObjectItemCaseSensitive(root, "data");
    int code = cJSON_IsNumber(response_code) ? response_code->valueint : -1;
    bool ok = code == 200 && cJSON_IsObject(data) &&
              json_copy_string(data, "code", report->code, sizeof(report->code)) &&
              json_copy_string(data,
                               "temp_token",
                               report->temp_token,
                               sizeof(report->temp_token)) &&
              json_copy_string(data,
                               "temp_client_id",
                               report->temp_client_id,
                               sizeof(report->temp_client_id));
    cJSON_Delete(root);
    if (!ok) {
        if (code == 40901) {
            ESP_LOGW(TAG, "the previous verification code is still valid; retry later");
        } else {
            ESP_LOGE(TAG, "device report response rejected code=%d", code);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "temporary MQTT credentials obtained (values hidden)");
    return ESP_OK;
}

static bool verification_code_valid(const char *code)
{
    if (code == NULL || strlen(code) != 6U) {
        return false;
    }
    for (size_t i = 0; i < 6U; ++i) {
        if (code[i] < '0' || code[i] > '9') {
            return false;
        }
    }
    return true;
}

static esp_err_t play_verification_prompt(
    const provision_report_t *report,
    const platform_provision_config_t *config,
    EventGroupHandle_t events)
{
    if (config->prompt_callback == NULL) {
        return ESP_OK;
    }
    if (!verification_code_valid(report->code)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t *pcm = heap_caps_malloc(PLATFORM_TTS_PCM_MAX_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char url[384] = {0};
    int url_length = snprintf(url,
                              sizeof(url),
                              "%s/v1/device/tts?code=%s",
                              s_services.device,
                              report->code);
    esp_err_t err = url_length <= 0 || (size_t)url_length >= sizeof(url)
                        ? ESP_ERR_INVALID_SIZE
                        : ESP_FAIL;
    size_t pcm_bytes = 0;
    for (unsigned attempt = 0;
         url_length > 0 && (size_t)url_length < sizeof(url) &&
         attempt < PLATFORM_TTS_DOWNLOAD_ATTEMPTS;
         ++attempt) {
        if (xEventGroupGetBits(events) & (PROVISION_DONE_BIT | PROVISION_ERROR_BIT)) {
            err = ESP_OK;
            break;
        }
        int status = 0;
        pcm_bytes = 0;
        err = http_binary_get(url,
                              report->temp_token,
                              pcm,
                              PLATFORM_TTS_PCM_MAX_BYTES,
                              &pcm_bytes,
                              &status);
        if (err == ESP_OK && status == 200 && pcm_bytes > 0U &&
            (pcm_bytes % sizeof(int16_t)) == 0U) {
            break;
        }
        if (err == ESP_OK) {
            err = status == 200 ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
        }
        ESP_LOGW(TAG,
                 "verification prompt download attempt %u failed: %s HTTP=%d bytes=%u",
                 attempt + 1U,
                 esp_err_to_name(err),
                 status,
                 (unsigned)pcm_bytes);
        if (attempt + 1U < PLATFORM_TTS_DOWNLOAD_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    if (err == ESP_OK &&
        !(xEventGroupGetBits(events) & (PROVISION_DONE_BIT | PROVISION_ERROR_BIT))) {
        ESP_LOGI(TAG,
                 "verification prompt downloaded bytes=%u format=pcm_s16le_8k_mono",
                 (unsigned)pcm_bytes);
        err = config->prompt_callback((const int16_t *)pcm,
                                      pcm_bytes / sizeof(int16_t),
                                      config->prompt_user_data);
    }
    mbedtls_platform_zeroize(pcm, PLATFORM_TTS_PCM_MAX_BYTES);
    heap_caps_free(pcm);
    mbedtls_platform_zeroize(url, sizeof(url));
    return err;
}

static void provision_finish_with_error(provision_mqtt_t *context)
{
    if (context != NULL && context->events != NULL) {
        if (context->prompt_cancel_callback != NULL) {
            context->prompt_cancel_callback(context->prompt_user_data);
        }
        xEventGroupSetBits(context->events, PROVISION_ERROR_BIT);
    }
}

static void provision_handle_message(provision_mqtt_t *context,
                                     const char *json,
                                     size_t length)
{
    /* 只处理 auth_grant；凭证完整校验后才发送 QoS1 ACK。 */
    cJSON *root = cJSON_ParseWithLength(json, length);
    const cJSON *type = root == NULL
                            ? NULL
                            : cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "auth_grant") != 0) {
        cJSON_Delete(root);
        return;
    }

    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (cJSON_IsObject(payload)) {
        const cJSON *device_id = cJSON_GetObjectItemCaseSensitive(payload, "device_id");
        const cJSON *device_key = cJSON_GetObjectItemCaseSensitive(payload, "device_key");
        bool has_id = cJSON_IsString(device_id) && device_id->valuestring != NULL &&
                      device_id->valuestring[0] != '\0';
        bool has_key = cJSON_IsString(device_key) && device_key->valuestring != NULL &&
                       device_key->valuestring[0] != '\0';
        if (has_id != has_key ||
            (has_id && (strlen(device_id->valuestring) >= sizeof(context->device_id) ||
                        strlen(device_key->valuestring) >=
                            sizeof(context->device_secret)))) {
            ESP_LOGE(TAG, "auth_grant contains invalid device credentials");
            cJSON_Delete(root);
            provision_finish_with_error(context);
            return;
        }
        if (has_id) {
            (void)snprintf(context->device_id,
                           sizeof(context->device_id),
                           "%s",
                           device_id->valuestring);
            (void)snprintf(context->device_secret,
                           sizeof(context->device_secret),
                           "%s",
                           device_key->valuestring);
        }
    }
    cJSON_Delete(root);

    if (context->device_id[0] == '\0' || context->device_secret[0] == '\0') {
        ESP_LOGE(TAG, "auth_grant did not provide credentials");
        provision_finish_with_error(context);
        return;
    }
    char ack_topic[128];
    ESP_LOGI(TAG, "binding grant received; delivering ACK");
    (void)snprintf(ack_topic,
                   sizeof(ack_topic),
                   "device/%s/ack",
                   context->temp_client_id);
    context->ack_message_id = esp_mqtt_client_publish(context->mqtt,
                                                       ack_topic,
                                                       "{\"ack\":true}",
                                                       12,
                                                       1,
                                                       0);
    if (context->ack_message_id < 0) {
        ESP_LOGE(TAG, "cannot publish auth_grant ACK");
        provision_finish_with_error(context);
    }
}

static void provision_mqtt_event(void *handler_args,
                                 esp_event_base_t base,
                                 int32_t event_id,
                                 void *event_data)
{
    /* context 位于阻塞等待函数的栈上，MQTT 停止并销毁后才会离开作用域。 */
    (void)base;
    provision_mqtt_t *context = handler_args;
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        char topic[128];
        (void)snprintf(topic,
                       sizeof(topic),
                       "device/%s/cmd",
                       context->temp_client_id);
        context->subscribe_message_id =
            esp_mqtt_client_subscribe(context->mqtt, topic, 1);
        if (context->subscribe_message_id < 0) {
            ESP_LOGE(TAG, "cannot subscribe temporary binding topic");
            provision_finish_with_error(context);
        } else {
            ESP_LOGI(TAG, "temporary MQTT connected; binding subscription pending");
        }
    } else if (event_id == MQTT_EVENT_SUBSCRIBED &&
               event->msg_id == context->subscribe_message_id) {
        ESP_LOGI(TAG, "temporary MQTT subscribed; waiting for H5 binding");
        xEventGroupSetBits(context->events, PROVISION_READY_BIT);
    } else if (event_id == MQTT_EVENT_DATA) {
        if (event->current_data_offset == 0) {
            context->message_size = 0;
            context->message_id = event->msg_id;
        }
        bool valid = event->msg_id == context->message_id && event->data_len >= 0 &&
                     context->message_size + (size_t)event->data_len <
                         sizeof(context->message);
        if (!valid) {
            ESP_LOGW(TAG, "dropping oversized temporary MQTT message");
            context->message_id = -1;
            context->message_size = 0;
            return;
        }
        memcpy(context->message + context->message_size,
               event->data,
               (size_t)event->data_len);
        context->message_size += (size_t)event->data_len;
        if (context->message_size == (size_t)event->total_data_len) {
            context->message[context->message_size] = '\0';
            provision_handle_message(context,
                                     context->message,
                                     context->message_size);
            context->message_id = -1;
            context->message_size = 0;
        }
    } else if (event_id == MQTT_EVENT_PUBLISHED &&
               event->msg_id == context->ack_message_id) {
        ESP_LOGI(TAG, "binding ACK delivered");
        /* Wake the synchronous prompt owner, not just its later event wait.
         * Cancellation is bounded/lock-free; credential persistence stays in
         * the cache-safe startup task, after MQTT has stopped. */
        if (context->prompt_cancel_callback != NULL) {
            context->prompt_cancel_callback(context->prompt_user_data);
        }
        xEventGroupSetBits(context->events, PROVISION_DONE_BIT);
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGW(TAG,
                 "temporary MQTT transport error; waiting for automatic reconnect");
    }
}

static esp_err_t wait_for_auth_grant(const provision_report_t *report,
                                     const platform_provision_config_t *config,
                                     platform_provision_result_t *result)
{
    /* 临时 MQTT 与正常设备 MQTT 完全分离，完成或超时后始终销毁。 */
    provision_mqtt_t context = {
        .message_id = -1,
        .subscribe_message_id = -1,
        .ack_message_id = -1,
        .prompt_cancel_callback = config->prompt_cancel_callback,
        .prompt_user_data = config->prompt_user_data,
    };
    (void)snprintf(context.temp_client_id,
                   sizeof(context.temp_client_id),
                   "%s",
                   report->temp_client_id);
    if (config->existing_device_id != NULL &&
        config->existing_device_secret != NULL) {
        (void)snprintf(context.device_id,
                       sizeof(context.device_id),
                       "%s",
                       config->existing_device_id);
        (void)snprintf(context.device_secret,
                       sizeof(context.device_secret),
                       "%s",
                       config->existing_device_secret);
    }
    context.events = xEventGroupCreate();
    if (context.events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = s_services.mqtt,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = report->temp_client_id,
        .credentials.client_id = report->temp_client_id,
        .credentials.authentication.password = report->temp_token,
        .session.keepalive = 60,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .network.reconnect_timeout_ms = 3000,
    };
    context.mqtt = esp_mqtt_client_init(&mqtt_config);
    if (context.mqtt == NULL) {
        vEventGroupDelete(context.events);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_mqtt_client_register_event(context.mqtt,
                                                    ESP_EVENT_ANY_ID,
                                                    provision_mqtt_event,
                                                    &context);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(context.mqtt);
    }
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(context.mqtt);
        vEventGroupDelete(context.events);
        return err;
    }

    unsigned timeout_seconds = config->timeout_seconds == 0
                                   ? PLATFORM_DEFAULT_PROVISION_TIMEOUT_SECONDS
                                   : config->timeout_seconds;
    TickType_t wait_started = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_seconds * 1000U);
    EventBits_t bits = xEventGroupWaitBits(context.events,
                                           PROVISION_READY_BIT |
                                               PROVISION_DONE_BIT |
                                               PROVISION_ERROR_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           timeout_ticks);
    if ((bits & PROVISION_READY_BIT) != 0 &&
        (bits & (PROVISION_DONE_BIT | PROVISION_ERROR_BIT)) == 0) {
        /* A Report response alone is not a usable binding session. Publish
         * the code only after SUBACK, before the optional spoken prompt. */
        (void)snprintf(s_verification_code, sizeof(s_verification_code), "%s", report->code);
        ESP_LOGI(TAG, "binding code ready: mqtt_ms=%lu",
                 (unsigned long)((xTaskGetTickCount() - wait_started) * portTICK_PERIOD_MS));
        esp_err_t prompt_err = play_verification_prompt(report, config, context.events);
        if (prompt_err != ESP_OK) {
            /* 屏幕验证码仍然可用，语音失败不应破坏绑定凭证状态机。 */
            ESP_LOGW(TAG,
                     "verification prompt unavailable: %s",
                     esp_err_to_name(prompt_err));
        }
        TickType_t elapsed = xTaskGetTickCount() - wait_started;
        TickType_t remaining = elapsed < timeout_ticks
                                   ? timeout_ticks - elapsed
                                   : 0;
        bits |= xEventGroupWaitBits(context.events,
                                    PROVISION_DONE_BIT | PROVISION_ERROR_BIT,
                                    pdFALSE,
                                    pdFALSE,
                                    remaining);
    }
    (void)esp_mqtt_client_stop(context.mqtt);
    (void)esp_mqtt_client_destroy(context.mqtt);
    vEventGroupDelete(context.events);
    if ((bits & PROVISION_DONE_BIT) == 0) {
        if ((bits & PROVISION_ERROR_BIT) != 0) {
            ESP_LOGE(TAG, "verification binding failed");
            mbedtls_platform_zeroize(&context, sizeof(context));
            return ESP_FAIL;
        }
        ESP_LOGE(TAG,
                 "verification binding timed out after %u seconds",
                 timeout_seconds);
        mbedtls_platform_zeroize(&context, sizeof(context));
        return ESP_ERR_TIMEOUT;
    }
    (void)snprintf(result->device_id,
                   sizeof(result->device_id),
                   "%s",
                   context.device_id);
    (void)snprintf(result->device_secret,
                   sizeof(result->device_secret),
                   "%s",
                   context.device_secret);
    mbedtls_platform_zeroize(&context, sizeof(context));
    return ESP_OK;
}

esp_err_t platform_client_provision(const platform_provision_config_t *config,
                                    platform_provision_result_t *result)
{
    /* 这是同步编排入口；调用者负责把 result 安全持久化。 */
    if (config == NULL || result == NULL || config->mac_address == NULL ||
        config->mac_address[0] == '\0' ||
        strlen(config->mac_address) >= sizeof(s_mac_address) ||
        ((config->existing_device_id == NULL) !=
         (config->existing_device_secret == NULL)) ||
        (config->existing_device_id != NULL &&
         (strlen(config->existing_device_id) >= sizeof(result->device_id) ||
          strlen(config->existing_device_secret) >= sizeof(result->device_secret)))) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    const int64_t provision_started = esp_timer_get_time();
    esp_err_t err = sync_clock();
    if (err != ESP_OK) {
        return err;
    }
    const int64_t clock_ready = esp_timer_get_time();
    if (!s_services_ready) {
        const char *discovery = config->discovery_url != NULL &&
                                config->discovery_url[0] != '\0'
                                    ? config->discovery_url
                                    : PLATFORM_DEFAULT_DISCOVERY;
        err = discover_services(discovery);
        if (err != ESP_OK) {
            return err;
        }
    }
    const int64_t services_ready = esp_timer_get_time();
    provision_report_t report = {0};
    err = report_for_provision(config, &report);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "binding setup: clock_ms=%lu discovery_ms=%lu report_ms=%lu",
             (unsigned long)((clock_ready - provision_started) / 1000),
             (unsigned long)((services_ready - clock_ready) / 1000),
             (unsigned long)((esp_timer_get_time() - services_ready) / 1000));
    s_provisioning = true;
    ESP_LOGW(TAG, "registration/login: %s", EXPERIENCE_PLATFORM_URL);
    ESP_LOGW(TAG, "open device binding and enter the displayed code");
    err = wait_for_auth_grant(&report, config, result);
    s_provisioning = false;
    mbedtls_platform_zeroize(s_verification_code,
                             sizeof(s_verification_code));
    mbedtls_platform_zeroize(&report, sizeof(report));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "verification binding completed; credentials ready for NVS");
    }
    return err;
}

esp_err_t platform_client_start(const platform_client_config_t *config)
{
    /* 正常在线入口只完成发现、鉴权和 MQTT；HTTP worker 延迟到 TiRTC 之后。 */
    if (s_ready) {
        return ESP_OK;
    }
    if (config == NULL || config->device_id == NULL || config->device_secret == NULL ||
        config->mac_address == NULL ||
        config->device_id[0] == '\0' || config->device_secret[0] == '\0' ||
        strlen(config->device_id) >= sizeof(s_device_id) ||
        strlen(config->device_secret) >= sizeof(s_device_secret) ||
        strlen(config->mac_address) >= sizeof(s_mac_address)) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(s_device_id, sizeof(s_device_id), "%s", config->device_id);
    (void)snprintf(s_device_secret, sizeof(s_device_secret), "%s", config->device_secret);
    int client_length = snprintf(s_client_id, sizeof(s_client_id),
                                 "sn_%s", config->device_id);
    if (client_length <= 0 || (size_t)client_length >= sizeof(s_client_id)) {
        return ESP_ERR_INVALID_SIZE;
    }
    (void)snprintf(s_mac_address, sizeof(s_mac_address), "%s", config->mac_address);

    esp_err_t err = sync_clock();
    if (err != ESP_OK) {
        return err;
    }
    const char *discovery = config->discovery_url != NULL &&
                            config->discovery_url[0] != '\0'
                                ? config->discovery_url
                                : PLATFORM_DEFAULT_DISCOVERY;
    if (!s_services_ready) {
        err = discover_services(discovery);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = obtain_mqtt_token();
    if (err != ESP_OK) {
        return err;
    }
    err = start_mqtt(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

static void release_request_resources(void)
{
    if (s_request_ready_queue != NULL) {
        vQueueDelete(s_request_ready_queue);
        s_request_ready_queue = NULL;
    }
    if (s_request_free_queue != NULL) {
        vQueueDelete(s_request_free_queue);
        s_request_free_queue = NULL;
    }
    heap_caps_free(s_request_response);
    s_request_response = NULL;
    heap_caps_free(s_request_pool);
    s_request_pool = NULL;
    s_request_worker_ready = false;
}

static esp_err_t prepare_request_resources(void)
{
    if (s_request_worker_ready) {
        return ESP_OK;
    }
    s_request_pool = heap_caps_calloc(PLATFORM_REQUEST_QUEUE_DEPTH,
                                      sizeof(*s_request_pool),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_request_response = heap_caps_malloc(PLATFORM_HTTP_BODY_MAX,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_request_ready_queue = xQueueCreate(PLATFORM_REQUEST_QUEUE_DEPTH,
                                         sizeof(uint8_t));
    s_request_free_queue = xQueueCreate(PLATFORM_REQUEST_QUEUE_DEPTH,
                                        sizeof(uint8_t));
    if (s_request_pool == NULL || s_request_response == NULL ||
        !esp_ptr_external_ram(s_request_pool) ||
        !esp_ptr_external_ram(s_request_response) ||
        s_request_ready_queue == NULL || s_request_free_queue == NULL) {
        release_request_resources();
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t slot = 0; slot < PLATFORM_REQUEST_QUEUE_DEPTH; ++slot) {
        if (xQueueSend(s_request_free_queue, &slot, 0) != pdTRUE) {
            release_request_resources();
            return ESP_FAIL;
        }
    }
    s_request_worker_ready = true;
    ESP_LOGI(TAG,
             "platform HTTP pool ready in PSRAM: slots=%u slot-bytes=%u scratch=%u",
             (unsigned)PLATFORM_REQUEST_QUEUE_DEPTH,
             (unsigned)sizeof(*s_request_pool),
             (unsigned)PLATFORM_HTTP_BODY_MAX);
    /* MQTT 往往先于 TiRTC/HTTP worker 建连；此处补发首次上线通知。 */
    notify_platform_online();
    return ESP_OK;
}

esp_err_t platform_client_run_request_loop(void)
{
    if (atomic_load(&s_rebind_quiesced)) return ESP_ERR_NOT_FOUND;
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = prepare_request_resources();
    if (err != ESP_OK) {
        return err;
    }
    request_loop();
    return ESP_ERR_NOT_FOUND;
}

bool platform_client_ready(void)
{
    return s_ready && atomic_load(&s_rebind_request) == 0;
}

uint32_t platform_client_epoch(void) { return atomic_load(&s_epoch); }
uint32_t platform_client_response_epoch(void) { return s_response_epoch; }
void platform_client_retry_binding(void) { atomic_store(&s_binding_retry, true); }
bool platform_client_take_binding_retry(void) { return atomic_exchange(&s_binding_retry, false); }

bool platform_client_request_rebind(bool known_unbound)
{
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong(&s_rebind_request, &expected, known_unbound ? 2U : 1U)) {
        if (known_unbound) atomic_store(&s_rebind_request, 2U);
        return false;
    }
    atomic_fetch_add(&s_epoch, 1U);
    return true;
}

bool platform_client_known_unbound(void) { return atomic_load(&s_rebind_request) == 2U; }
bool platform_client_reconciling(void) { return atomic_load(&s_rebind_request) != 0; }

void platform_client_rebind_quiesced(void)
{
    atomic_store(&s_rebind_quiesced, true);
    /* A sentinel wakes an idle queue without allocating another task/stack.
     * A full queue is already awake and checks quiesced before its next item. */
    const uint8_t wake = UINT8_MAX;
    if (s_request_ready_queue != NULL) (void)xQueueSend(s_request_ready_queue, &wake, 0);
}

static esp_err_t stop_mqtt(void);

esp_err_t platform_client_prepare_rebind(void)
{
    /* Called only by the HTTP owner, after its current request has returned.
     * No NVS is erased: device identity survives an ownership change. */
    if (!atomic_load(&s_rebind_quiesced)) return ESP_ERR_INVALID_STATE;
    s_ready = false;
    esp_err_t err = stop_mqtt();
    if (err != ESP_OK) return err;
    mbedtls_platform_zeroize(s_mqtt_token, sizeof(s_mqtt_token));
    uint8_t slot;
    while (s_request_ready_queue != NULL && xQueueReceive(s_request_ready_queue, &slot, 0) == pdTRUE) {
        if (slot >= PLATFORM_REQUEST_QUEUE_DEPTH) continue;
        platform_request_t *request = &s_request_pool[slot];
        s_response_epoch = request->epoch;
        if (request->callback != NULL) request->callback(NULL, request->user_data);
        memset(request, 0, sizeof(*request));
        if (xQueueSend(s_request_free_queue, &slot, 0) != pdTRUE)
            ESP_LOGE(TAG, "rebind request slot return failed: %u", (unsigned)slot);
    }
    ESP_LOGI(TAG, "binding transition: old MQTT/HTTP credentials released epoch=%lu",
             (unsigned long)platform_client_epoch());
    return ESP_OK;
}

void platform_client_complete_rebind(void)
{
    if (!s_ready) return;
    atomic_store(&s_rebind_quiesced, false);
    atomic_store(&s_rebind_request, 0);
    notify_platform_online();
}

bool platform_client_mqtt_connected(void)
{
    return s_mqtt_connected;
}

esp_err_t platform_client_suspend_mqtt_for_realtime(void)
{
    if (!platform_client_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return stop_mqtt();
}

static esp_err_t stop_mqtt(void)
{
    ESP_LOGI(TAG, "CONN mqtt_stop begin: online=%d", (int)s_mqtt_connected);
    uint32_t started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_mqtt_lifecycle == NULL ||
        xSemaphoreTake(s_mqtt_lifecycle, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "CONN mqtt_stop failed: stage=lock wait_ms=%lu",
                 (unsigned long)((uint32_t)(esp_timer_get_time() / 1000) - started_ms));
        return ESP_ERR_TIMEOUT;
    }
    uint32_t locked_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_mqtt == NULL) {
        s_mqtt_connected = false;
        xSemaphoreGive(s_mqtt_lifecycle);
        ESP_LOGI(TAG, "CONN mqtt_stop skipped: absent=1 lock_ms=%lu",
                 (unsigned long)(locked_ms - started_ms));
        return ESP_OK;
    }
    esp_mqtt_client_handle_t mqtt = s_mqtt;
    esp_err_t err = esp_mqtt_client_stop(mqtt);
    uint32_t stopped_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mqtt_lifecycle);
        ESP_LOGE(TAG, "CONN cannot stop MQTT client: %s lock_ms=%lu stop_ms=%lu",
                 esp_err_to_name(err), (unsigned long)(locked_ms - started_ms),
                 (unsigned long)(stopped_ms - locked_ms));
        return err;
    }
    s_mqtt_connected = false;
    err = esp_mqtt_client_destroy(mqtt);
    uint32_t destroyed_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mqtt_lifecycle);
        ESP_LOGE(TAG,
                 "CONN cannot release stopped MQTT client: %s lock_ms=%lu stop_ms=%lu destroy_ms=%lu",
                 esp_err_to_name(err), (unsigned long)(locked_ms - started_ms),
                 (unsigned long)(stopped_ms - locked_ms),
                 (unsigned long)(destroyed_ms - stopped_ms));
        return err;
    }
    s_mqtt = NULL;
    xSemaphoreGive(s_mqtt_lifecycle);
    ESP_LOGI(TAG, "CONN MQTT client released: lock_ms=%lu stop_ms=%lu destroy_ms=%lu total_ms=%lu",
             (unsigned long)(locked_ms - started_ms),
             (unsigned long)(stopped_ms - locked_ms),
             (unsigned long)(destroyed_ms - stopped_ms),
             (unsigned long)(destroyed_ms - started_ms));
    return ESP_OK;
}

esp_err_t platform_client_resume_mqtt_after_realtime(void)
{
    if (!platform_client_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = start_mqtt(false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT resume submitted after TiRTC external connect");
    } else {
        ESP_LOGE(TAG, "MQTT resume failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool platform_client_provisioning(void)
{
    return s_provisioning;
}

const char *platform_client_verification_code(void)
{
    return s_verification_code;
}

esp_err_t platform_client_request(platform_service_t service,
                                  const char *path,
                                  const char *json_body,
                                  platform_response_callback_t callback,
                                  void *user_data)
{
    return platform_client_request_timeout(service,
                                           path,
                                           json_body,
                                           PLATFORM_DEFAULT_HTTP_TIMEOUT_MS,
                                           callback,
                                           user_data);
}

static esp_err_t enqueue_request(platform_service_t service,
                                          const char *path,
                                          const char *json_body,
                                          unsigned timeout_ms,
                                          platform_response_callback_t callback,
                                          void *user_data,
                                          bool metadata_only)
{
    uint32_t epoch = platform_client_epoch();
    if (!platform_client_ready() || !s_request_worker_ready || s_request_ready_queue == NULL ||
        s_request_free_queue == NULL || s_request_pool == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (service_base(service) == NULL || path == NULL || path[0] != '/' ||
        strlen(path) >= PLATFORM_REQUEST_PATH_MAX ||
        (json_body != NULL && strlen(json_body) >= PLATFORM_REQUEST_BODY_MAX) ||
        timeout_ms == 0 || timeout_ms > 60000U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t slot = 0;
    if (xQueueReceive(s_request_free_queue, &slot, 0) != pdTRUE ||
        slot >= PLATFORM_REQUEST_QUEUE_DEPTH) {
        return ESP_ERR_TIMEOUT;
    }
    /* path/body 复制进 PSRAM slot；内部 FreeRTOS 队列只传一个字节索引。 */
    platform_request_t *request = &s_request_pool[slot];
    *request = (platform_request_t) {
        .service = service,
        .epoch = epoch,
        .post = json_body != NULL,
        .metadata_only = metadata_only,
        .timeout_ms = timeout_ms,
        .callback = callback,
        .user_data = user_data,
    };
    (void)snprintf(request->path, sizeof(request->path), "%s", path);
    if (json_body != NULL) {
        (void)snprintf(request->body, sizeof(request->body), "%s", json_body);
    }
    request->queued_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (xQueueSend(s_request_ready_queue, &slot, 0) == pdTRUE) {
        return ESP_OK;
    }
    memset(request, 0, sizeof(*request));
    (void)xQueueSend(s_request_free_queue, &slot, 0);
    return ESP_ERR_TIMEOUT;
}

esp_err_t platform_client_request_timeout(platform_service_t service, const char *path,
                                          const char *json_body, unsigned timeout_ms,
                                          platform_response_callback_t callback, void *user_data)
{
    return enqueue_request(service, path, json_body, timeout_ms, callback, user_data, false);
}

esp_err_t platform_client_request_metadata(platform_service_t service, const char *path,
                                           const char *json_body, unsigned timeout_ms,
                                           platform_response_callback_t callback, void *user_data)
{
    return enqueue_request(service, path, json_body,
                           timeout_ms == 0 ? PLATFORM_DEFAULT_HTTP_TIMEOUT_MS : timeout_ms,
                           callback, user_data, true);
}

void platform_client_set_signal_handler(platform_signal_callback_t callback,
                                        void *user_data)
{
    s_signal_callback = callback;
    s_signal_user_data = user_data;
}

void platform_client_set_online_handler(platform_online_callback_t callback,
                                        void *user_data)
{
    s_online_callback = callback;
    s_online_user_data = user_data;
}
