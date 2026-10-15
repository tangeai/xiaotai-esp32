#include "platform_internal.h"
#include "message_buffer.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mqtt_client.h"
#include "nvs.h"

#define TAG "platform"
#define JSON_CAPACITY 4096
#define PLATFORM_TASK_STACK_BYTES 8192
#define BIND_PROMPT_REPEAT_INTERVAL_MS 10000U
#define TIRTC_SDK_DEFAULT_ENDPOINT_HTTPS_ALIAS "https://ep-tirtc.tange365.com"
#define MQTT_READY BIT0
#define GRANT_RECEIVED BIT1
#define ACK_RECEIVED BIT2
#define UNBOUND BIT3
#define AUTH_FAILED BIT4
#define RETRY BIT5
typedef struct {
    platform_operation_t operation;
    uint32_t generation;
    unsigned epoch;
    char body[768];
} request_t;
static platform_identity_t s_identity, s_granted;
static char s_mac[18], s_client_id[32];
static char s_device_url[256], s_voip_url[256], s_rtc_url[256], s_mqtt_url[256];
static char s_discovery_url[256], s_token[1024], s_mqtt_id[80];
/* HTTP belongs to platform_task; MQTT reassembly belongs to mqtt_event. */
static message_buffer_t s_response, s_incoming;
static char s_topic[128];
static size_t s_received, s_total;
static bool s_temp;
static atomic_bool s_online, s_retry;
static atomic_bool s_prompt_cancelled;
static atomic_bool s_realtime_media;
static atomic_bool s_mqtt_suspended;
static atomic_uint s_epoch;
static portMUX_TYPE s_identity_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_mqtt_client_handle_t s_mqtt;
static EventGroupHandle_t s_events;
static StaticEventGroup_t s_event_storage;
static QueueHandle_t s_requests;
static StaticQueue_t s_request_storage;
static uint8_t s_request_buffer[2 * sizeof(request_t)];
static TaskHandle_t s_task;
/* Retained for the one platform worker, including a fatal identity-storage failure. */
_Alignas(portBYTE_ALIGNMENT) static StackType_t s_task_stack[PLATFORM_TASK_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_task_storage;
static platform_observer_t s_observer;
static platform_binding_prompt_t s_binding_prompt;
static int s_sub_cmd, s_sub_notify;
static atomic_int s_last_ack, s_published;
static bool s_cmd_ready, s_notify_ready;
static int64_t s_token_expiry;

static bool emit(platform_event_kind_t kind, platform_operation_t op, uint32_t gen, esp_err_t error,
                 const char *json)
{
    platform_event_t event = {
        .kind = kind, .operation = op, .generation = gen, .error = error, .json = json};
    bool ok = s_observer(&event);
    if (!ok)
        ESP_LOGE(TAG, "event delivery failed: kind=%d generation=%lu", kind, (unsigned long)gen);
    return ok;
}
static bool has_credentials(void)
{
    return s_identity.device_id[0] && s_identity.device_key[0];
}
void platform_identity_snapshot(platform_identity_t *identity)
{
    portENTER_CRITICAL(&s_identity_lock);
    *identity = s_identity;
    portEXIT_CRITICAL(&s_identity_lock);
}
static void identity_update(const platform_identity_t *identity)
{
    portENTER_CRITICAL(&s_identity_lock);
    s_identity = *identity;
    portEXIT_CRITICAL(&s_identity_lock);
}
static void offline(void)
{
    if (atomic_exchange(&s_online, false))
        atomic_fetch_add(&s_epoch, 1);
}
const char *platform_client_id(void)
{
    return s_client_id;
}
const char *platform_rtc_endpoint(void)
{
    return s_rtc_url;
}
bool platform_is_online(void)
{
    return atomic_load(&s_online) && platform_network_ready();
}
void platform_set_realtime_media(bool active)
{
    atomic_store_explicit(&s_realtime_media, active, memory_order_release);
}
void platform_retry(void)
{
    atomic_store(&s_retry, true);
}

esp_err_t platform_set_binding_prompt(const platform_binding_prompt_t *prompt)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (prompt == NULL) {
        memset(&s_binding_prompt, 0, sizeof(s_binding_prompt));
        return ESP_OK;
    }
    bool local = prompt->play_code != NULL;
    bool streaming = prompt->begin != NULL && prompt->write_pcm8 != NULL &&
                     prompt->finish != NULL;
    if ((!local && !streaming) || prompt->cancel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_binding_prompt = *prompt;
    return ESP_OK;
}

static int64_t jwt_expiry(const char *token)
{
    const char *start = strchr(token, '.');
    if (!start)
        return 0;
    ++start;
    const char *end = strchr(start, '.');
    if (!end || end - start > 1400)
        return 0;
    char encoded[1408], decoded[1056];
    size_t n = end - start, size = 0;
    memcpy(encoded, start, n);
    for (size_t i = 0; i < n; ++i) {
        if (encoded[i] == '-')
            encoded[i] = '+';
        if (encoded[i] == '_')
            encoded[i] = '/';
    }
    while (n % 4)
        encoded[n++] = '=';
    if (mbedtls_base64_decode((unsigned char *)decoded, sizeof(decoded) - 1, &size,
                              (unsigned char *)encoded, n) != 0)
        return 0;
    decoded[size] = 0;
    cJSON *root = cJSON_Parse(decoded);
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "exp");
    int64_t value = cJSON_IsNumber(exp) ? (int64_t)exp->valuedouble : 0;
    cJSON_Delete(root);
    memset(decoded, 0, sizeof(decoded));
    return value;
}
static esp_err_t signature(char *timestamp, char *nonce, char *signature)
{
    snprintf(timestamp, 24, "%lld", (long long)time(NULL));
    snprintf(nonce, 17, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    char input[128];
    int n = snprintf(input, sizeof(input), "%s%s%s", s_identity.device_id, timestamp, nonce);
    unsigned char digest[32];
    size_t written = 0;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (n < 0 || n >= sizeof(input) || !info ||
        mbedtls_md_hmac(info, (const unsigned char *)s_identity.device_key,
                        strlen(s_identity.device_key), (const unsigned char *)input, n,
                        digest) != 0)
        return ESP_FAIL;
    int rc =
        mbedtls_base64_encode((unsigned char *)signature, 64, &written, digest, sizeof(digest));
    memset(digest, 0, sizeof(digest));
    if (rc != 0 || written >= 64)
        return ESP_FAIL;
    signature[written] = 0;
    return ESP_OK;
}
static esp_err_t discover(void)
{
    http_result_t result;
    esp_err_t err = platform_http(s_discovery_url, HTTP_METHOD_GET, NULL, NULL, NULL, 0,
                                  &s_response, JSON_CAPACITY, &result);
    if (err != ESP_OK)
        return err;
    cJSON *root = cJSON_Parse(s_response.data);
    bool ok = json_string_copy(root, "device-srv", s_device_url, sizeof(s_device_url)) &&
              json_string_copy(root, "voip-srv", s_voip_url, sizeof(s_voip_url)) &&
              json_string_copy(root, "mqtt-srv", s_mqtt_url, sizeof(s_mqtt_url)) &&
              json_string_copy(root, "tirtc-srv", s_rtc_url, sizeof(s_rtc_url));
    cJSON_Delete(root);
    /* Keep the control plane encrypted. The Mini SDK data-plane endpoint is
     * HTTP-only; the official discovery service advertises its HTTPS alias. */
    bool rtc_endpoint_ok = strncmp(s_rtc_url, "http://", 7) == 0 ||
                           strcmp(s_rtc_url, TIRTC_SDK_DEFAULT_ENDPOINT_HTTPS_ALIAS) == 0;
    ok = ok && strncmp(s_device_url, "https://", 8) == 0 &&
         strncmp(s_voip_url, "https://", 8) == 0 && strncmp(s_mqtt_url, "mqtts://", 8) == 0 &&
         rtc_endpoint_ok;
    if (!ok)
        ESP_LOGE(TAG, "discovery missing service or insecure endpoint; inspect deployment");
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
static esp_err_t authenticate(bool report, http_result_t *result)
{
    char url[320], timestamp[24], nonce[17], sig[64], body[64];
    http_header_t headers[5];
    size_t count = 0;
    if (has_credentials()) {
        esp_err_t err = signature(timestamp, nonce, sig);
        if (err != ESP_OK)
            return err;
        headers[count++] = (http_header_t){"X-Device-Id", s_identity.device_id};
        headers[count++] = (http_header_t){"X-Timestamp", timestamp};
        headers[count++] = (http_header_t){"X-Nonce", nonce};
        headers[count++] = (http_header_t){"X-Signature", sig};
        if (!report)
            headers[count++] = (http_header_t){"X-Mac", s_mac};
    }
    snprintf(url, sizeof(url), "%s/v1/device/%s", s_device_url, report ? "report" : "token");
    snprintf(body, sizeof(body), "{\"mac\":\"%s\"}", s_mac);
    esp_err_t err = platform_http(url, HTTP_METHOD_POST, report ? body : "", NULL, headers, count,
                                  &s_response, JSON_CAPACITY, result);
    memset(sig, 0, sizeof(sig));
    if (err != ESP_OK || result->code != 200)
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}
static int publish_ack(void)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "device/%s/ack", s_mqtt_id);
    xEventGroupClearBits(s_events, ACK_RECEIVED);
    int id = esp_mqtt_client_enqueue(s_mqtt, topic, "{\"ack\":true}", 12, 1, 0, true);
    atomic_store(&s_last_ack, id);
    if (id < 0)
        ESP_LOGE(TAG, "MQTT ACK enqueue failed");
    return id;
}
static bool wait_ack(int id, unsigned timeout_ms)
{
    int64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    while (id >= 0 && atomic_load(&s_published) != id) {
        int64_t remaining = deadline - esp_timer_get_time() / 1000;
        if (remaining <= 0)
            return false;
        xEventGroupWaitBits(s_events, ACK_RECEIVED, pdTRUE, pdTRUE, pdMS_TO_TICKS(remaining));
    }
    return id >= 0;
}
static void complete_message(void)
{
    cJSON *root = cJSON_Parse(s_incoming.data);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    if (s_temp && strcmp(type->valuestring, "auth_grant") == 0) {
        if (!(xEventGroupGetBits(s_events) & GRANT_RECEIVED)) {
            s_granted = s_identity;
            bool ok = !payload || cJSON_IsNull(payload) ||
                      (cJSON_IsObject(payload) && cJSON_GetArraySize(payload) == 0);
            if (!ok)
                ok = json_string_copy(payload, "device_id", s_granted.device_id,
                                      sizeof(s_granted.device_id)) &&
                     json_string_copy(payload, "device_key", s_granted.device_key,
                                      sizeof(s_granted.device_key));
            if (ok && s_granted.device_id[0] && s_granted.device_key[0]) {
                atomic_store_explicit(&s_prompt_cancelled, true,
                                      memory_order_release);
                if (s_binding_prompt.cancel != NULL) {
                    s_binding_prompt.cancel(s_binding_prompt.user_data);
                }
                xEventGroupSetBits(s_events, GRANT_RECEIVED);
            } else {
                ESP_LOGE(TAG, "auth_grant invalid; identity retained, no ACK");
            }
        }
    } else if (!s_temp && !(xEventGroupGetBits(s_events) & UNBOUND)) {
        bool command = strstr(s_topic, "/cmd") != NULL;
        if (strcmp(type->valuestring, "unbind") == 0 && command) {
            offline();
            publish_ack();
            xEventGroupSetBits(s_events, UNBOUND);
        } else if (emit(PLATFORM_MESSAGE, 0, 0, ESP_OK, s_incoming.data) && command) {
            publish_ack();
        }
    }
    cJSON_Delete(root);
}
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) {
        message_buffer_release(&s_incoming);
        s_received = s_total = 0;
        s_cmd_ready = s_notify_ready = false;
        char topic[128];
        snprintf(topic, sizeof(topic), "device/%s/cmd", s_mqtt_id);
        s_sub_cmd = esp_mqtt_client_subscribe(e->client, topic, 1);
        s_sub_notify = -1;
        if (!s_temp) {
            snprintf(topic, sizeof(topic), "device/%s/notify", s_mqtt_id);
            s_sub_notify = esp_mqtt_client_subscribe(e->client, topic, 1);
        }
    } else if (id == MQTT_EVENT_SUBSCRIBED) {
        if (e->data_len && e->data && (unsigned char)e->data[0] == 0x80) {
            xEventGroupSetBits(s_events, AUTH_FAILED);
            return;
        }
        if (e->msg_id == s_sub_cmd)
            s_cmd_ready = true;
        if (e->msg_id == s_sub_notify)
            s_notify_ready = true;
        if (s_cmd_ready && (s_temp || s_notify_ready))
            xEventGroupSetBits(s_events, MQTT_READY);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        message_buffer_release(&s_incoming);
        xEventGroupClearBits(s_events, MQTT_READY);
        offline();
        s_received = s_total = 0;
        if (!s_temp && !atomic_load_explicit(&s_mqtt_suspended,
                                             memory_order_acquire))
            emit(PLATFORM_OFFLINE, 0, 0, ESP_FAIL, NULL);
    } else if (id == MQTT_EVENT_ERROR) {
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
            xEventGroupSetBits(s_events, AUTH_FAILED);
        ESP_LOGW(TAG, "MQTT error; inspect TLS/time/token and broker");
    } else if (id == MQTT_EVENT_PUBLISHED) {
        /* Latch PUBACK even when it arrives before enqueue() returns its ID. */
        atomic_store(&s_published, e->msg_id);
        xEventGroupSetBits(s_events, ACK_RECEIVED);
    } else if (id == MQTT_EVENT_DATA) {
        if (e->current_data_offset == 0) {
            message_buffer_release(&s_incoming);
            s_received = s_total = 0;
            if (e->total_data_len <= 0 || e->total_data_len >= JSON_CAPACITY ||
                !e->topic || e->topic_len <= 0 || e->topic_len >= sizeof(s_topic)) {
                ESP_LOGE(TAG, "MQTT message exceeds bounded input; not ACKed");
                return;
            }
            memcpy(s_topic, e->topic, e->topic_len);
            s_topic[e->topic_len] = 0;
            char expected[128];
            snprintf(expected, sizeof(expected), "device/%s/cmd", s_mqtt_id);
            bool valid = strcmp(s_topic, expected) == 0;
            if (!s_temp) {
                snprintf(expected, sizeof(expected), "device/%s/notify", s_mqtt_id);
                valid = valid || strcmp(s_topic, expected) == 0;
            }
            if (!valid)
                return;
            if (!message_buffer_begin(&s_incoming, (size_t)e->total_data_len + 1)) {
                ESP_LOGE(TAG, "MQTT message allocation failed: bytes=%d; not ACKed",
                         e->total_data_len + 1);
                return;
            }
            s_total = e->total_data_len;
        }
        if (!s_total || e->data_len < 0 || e->current_data_offset != s_received ||
            (size_t)e->data_len > s_total - s_received ||
            !message_buffer_append(&s_incoming, s_received, e->data, (size_t)e->data_len)) {
            message_buffer_release(&s_incoming);
            s_received = s_total = 0;
            return;
        }
        s_received += e->data_len;
        if (s_received == s_total) {
            complete_message();
            message_buffer_release(&s_incoming);
            s_total = s_received = 0;
        }
    }
}
static void mqtt_stop(void)
{
    offline();
    if (s_mqtt) {
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
    /* stop/destroy joined the callback owner, so release an incomplete fragment. */
    message_buffer_release(&s_incoming);
    s_received = s_total = 0;
    xEventGroupClearBits(s_events, MQTT_READY | ACK_RECEIVED | AUTH_FAILED);
}
static esp_err_t mqtt_start(bool temporary, const char *client, const char *user)
{
    ESP_LOGI(TAG, "MQTT start: temporary=%d free=%u largest=%u", temporary,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_temp = temporary;
    strlcpy(s_mqtt_id, client, sizeof(s_mqtt_id));
    xEventGroupClearBits(s_events,
                         MQTT_READY | ACK_RECEIVED | GRANT_RECEIVED | AUTH_FAILED | UNBOUND);
    atomic_store(&s_last_ack, -1);
    atomic_store(&s_published, -1);
    esp_mqtt_client_config_t config = {
        .broker.address.uri = s_mqtt_url,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = s_mqtt_id,
        .credentials.username = user,
        .credentials.authentication.password = s_token,
        .session.keepalive = 30,
        .buffer.size = 1024,
        .buffer.out_size = 1024,
        .task.stack_size = 6144,
        .network.reconnect_timeout_ms = 5000,
        .outbox.limit = 4096,
    };
    s_mqtt = esp_mqtt_client_init(&config);
    if (!s_mqtt)
        return ESP_ERR_NO_MEM;
    esp_err_t err = esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (err == ESP_OK)
        err = esp_mqtt_client_start(s_mqtt);
    if (err != ESP_OK)
        mqtt_stop();
    return err;
}

static void announce_binding_code(const char *code)
{
    if (s_binding_prompt.play_code == NULL && s_binding_prompt.begin == NULL) {
        return;
    }
    atomic_store_explicit(&s_prompt_cancelled, false, memory_order_release);
    if (s_binding_prompt.play_code != NULL) {
        esp_err_t local_err =
            s_binding_prompt.play_code(code, s_binding_prompt.user_data);
        bool cancelled = atomic_load_explicit(&s_prompt_cancelled,
                                               memory_order_acquire);
        if (local_err == ESP_OK && !cancelled) {
            ESP_LOGI(TAG, "binding code announced: source=local-opus");
            return;
        }
        if (cancelled) {
            ESP_LOGI(TAG, "binding prompt cancelled after authorization");
            return;
        }
        ESP_LOGW(TAG, "local binding prompt failed: %s; trying platform TTS",
                 esp_err_to_name(local_err));
        if (s_binding_prompt.begin == NULL ||
            s_binding_prompt.write_pcm8 == NULL ||
            s_binding_prompt.finish == NULL) {
            return;
        }
    }
    esp_err_t err = s_binding_prompt.begin(s_binding_prompt.user_data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "binding prompt start failed: %s", esp_err_to_name(err));
        return;
    }
    char url[352];
    int length = snprintf(url, sizeof(url), "%s/v1/device/tts?code=%s",
                          s_device_url, code);
    size_t pcm_bytes = 0;
    int status = 0;
    if (length <= 0 || length >= sizeof(url)) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        err = platform_http_pcm(url, s_token, &s_binding_prompt,
                                &s_prompt_cancelled, &pcm_bytes, &status);
    }
    bool cancelled = atomic_load_explicit(&s_prompt_cancelled,
                                           memory_order_acquire);
    if (err == ESP_OK && !cancelled) {
        err = s_binding_prompt.finish(s_binding_prompt.user_data);
    }
    if (err != ESP_OK || cancelled) {
        s_binding_prompt.cancel(s_binding_prompt.user_data);
    }
    if (cancelled) {
        ESP_LOGI(TAG, "binding prompt cancelled after authorization");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "binding prompt unavailable: %s HTTP=%d bytes=%u; binding remains active",
                 esp_err_to_name(err), status, (unsigned)pcm_bytes);
    } else {
        ESP_LOGI(TAG, "binding code announced: pcm_bytes=%u",
                 (unsigned)pcm_bytes);
    }
}

static esp_err_t provision(void)
{
    http_result_t result = {0};
    esp_err_t err = authenticate(true, &result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "report failed: HTTP=%d code=%d retry_after=%us; use retry after cooldown",
                 result.status, result.code, result.retry_after);
        return err;
    }
    char code[16], temp_id[80];
    cJSON *root = cJSON_Parse(s_response.data);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    bool ok = json_string_copy(data, "code", code, sizeof(code)) &&
              json_string_copy(data, "temp_client_id", temp_id, sizeof(temp_id)) &&
              json_string_copy(data, "temp_token", s_token, sizeof(s_token));
    cJSON_Delete(root);
    memset(s_response.data, 0, s_response.capacity);
    if (!ok)
        return ESP_ERR_INVALID_RESPONSE;
    err = mqtt_start(true, temp_id, temp_id);
    if (err != ESP_OK)
        return err;
    EventBits_t bits = xEventGroupWaitBits(s_events, MQTT_READY | AUTH_FAILED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    if (!(bits & MQTT_READY)) {
        mqtt_stop();
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG,
             "BIND CODE: %s (repeats every %us until authorization; temporary MQTT subscribed)",
             code, BIND_PROMPT_REPEAT_INTERVAL_MS / 1000U);
    int64_t expiry = jwt_expiry(s_token);
    int64_t seconds = expiry > time(NULL) ? expiry - time(NULL) : 190;
    if (seconds > 600)
        seconds = 600;
    int64_t deadline_ms = esp_timer_get_time() / 1000 + seconds * 1000;
    unsigned announcement = 0;
    do {
        bits = xEventGroupGetBits(s_events);
        if (bits & (GRANT_RECEIVED | AUTH_FAILED))
            break;

        ESP_LOGI(TAG, "binding announcement: sequence=%u code=%s", ++announcement, code);
        announce_binding_code(code);

        bits = xEventGroupGetBits(s_events);
        if (bits & (GRANT_RECEIVED | AUTH_FAILED))
            break;
        int64_t remaining_ms = deadline_ms - esp_timer_get_time() / 1000;
        if (remaining_ms <= 0)
            break;
        uint32_t wait_ms = remaining_ms < BIND_PROMPT_REPEAT_INTERVAL_MS
                               ? (uint32_t)remaining_ms
                               : BIND_PROMPT_REPEAT_INTERVAL_MS;
        bits = xEventGroupWaitBits(s_events, GRANT_RECEIVED | AUTH_FAILED,
                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(wait_ms));
    } while (!(bits & (GRANT_RECEIVED | AUTH_FAILED)) &&
             esp_timer_get_time() / 1000 < deadline_ms);
    if (!(bits & GRANT_RECEIVED)) {
        mqtt_stop();
        return ESP_ERR_TIMEOUT;
    }
    /* Flash writes stay on this internal-stack worker, outside MQTT callbacks.
     * Commit the complete identity atomically before acknowledging delivery. */
    err = platform_identity_save(&s_granted);
    if (err == ESP_OK) {
        identity_update(&s_granted);
        if (!wait_ack(publish_ack(), 5000))
            err = ESP_ERR_TIMEOUT;
    }
    mqtt_stop();
    memset(&s_granted, 0, sizeof(s_granted));
    if (err == ESP_OK)
        ESP_LOGI(TAG, "binding persisted and grant ACK PUBACK confirmed");
    return err;
}
static esp_err_t login(void)
{
    http_result_t result = {0};
    if (!has_credentials()) {
        esp_err_t err = provision();
        if (err != ESP_OK)
            return err;
    }
    esp_err_t err = authenticate(false, &result);
    if (result.status == 410 && result.code == 6006) {
        emit(PLATFORM_UNBOUND, 0, 0, ESP_OK, NULL);
        err = provision();
        if (err == ESP_OK)
            err = authenticate(false, &result);
    }
    if (err != ESP_OK)
        return err;
    cJSON *root = cJSON_Parse(s_response.data);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    bool ok = json_string_copy(data, "mqtt_token", s_token, sizeof(s_token));
    cJSON_Delete(root);
    memset(s_response.data, 0, s_response.capacity);
    if (!ok)
        return ESP_ERR_INVALID_RESPONSE;
    s_token_expiry = jwt_expiry(s_token);
    char client[80];
    snprintf(client, sizeof(client), "sn_%s", s_identity.device_id);
    return mqtt_start(false, client, s_identity.device_id);
}
esp_err_t platform_request(platform_operation_t operation, uint32_t gen, const char *body)
{
    if (!platform_is_online() || !s_requests)
        return ESP_ERR_INVALID_STATE;
    if (operation < PLATFORM_PROFILE || operation > PLATFORM_CALL ||
        (body && strlen(body) >= sizeof(((request_t *)0)->body)))
        return ESP_ERR_INVALID_SIZE;
    request_t r = {.operation = operation, .generation = gen, .epoch = atomic_load(&s_epoch)};
    if (body)
        strcpy(r.body, body);
    return xQueueSend(s_requests, &r, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}
static void process_request(request_t *request)
{
    const char *path = request->operation == PLATFORM_PROFILE    ? "/v1/device/profile"
                       : request->operation == PLATFORM_CONTACTS ? "/v1/voip/device/contacts"
                                                                 : "/v1/voip/device/call";
    const char *base = request->operation == PLATFORM_PROFILE ? s_device_url : s_voip_url;
    char url[352];
    snprintf(url, sizeof(url), "%s%s", base, path);
    http_result_t result = {0};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!platform_is_online() || request->epoch != atomic_load(&s_epoch)) {
        emit(PLATFORM_RESPONSE, request->operation, request->generation, err, NULL);
        return;
    }
    if (platform_is_online() && request->epoch == atomic_load(&s_epoch)) {
        err = platform_http(
            url, request->operation == PLATFORM_CONTACTS ? HTTP_METHOD_GET : HTTP_METHOD_POST,
            request->operation == PLATFORM_CONTACTS ? NULL : request->body, s_token, NULL, 0,
            &s_response, JSON_CAPACITY, &result);
        if (err == ESP_OK && result.code != 0 && result.code != 200)
            err = ESP_FAIL;
        if (result.status == 401)
            xEventGroupSetBits(s_events, AUTH_FAILED);
        if (result.code == 6006 && result.status == 410) {
            offline();
            atomic_store(&s_last_ack, -1);
            xEventGroupSetBits(s_events, UNBOUND);
        }
    }
    if (!platform_is_online() || request->epoch != atomic_load(&s_epoch)) {
        err = ESP_ERR_INVALID_STATE;
        if (s_response.data)
            s_response.data[0] = 0;
    }
    emit(PLATFORM_RESPONSE, request->operation, request->generation, err, s_response.data);
    message_buffer_release(&s_response);
}
static void platform_task(void *arg)
{
    (void)arg;
    esp_err_t err = platform_identity_load(&s_granted);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        memset(&s_granted, 0, sizeof(s_granted));
    else if (err != ESP_OK) {
        ESP_LOGE(TAG, "identity storage invalid: %s; refusing to replace identity",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    identity_update(&s_granted);
    memset(&s_granted, 0, sizeof(s_granted));
    bool discovered = false;
    bool ready_announced = false;
    int64_t last_heartbeat = 0, last_refresh = 0;
    unsigned sequence = 0;
    atomic_store(&s_retry, true);
    request_t request;
    for (;;) {
        if (!platform_network_ready()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        EventBits_t bits = xEventGroupGetBits(s_events);
        bool realtime_media = atomic_load_explicit(&s_realtime_media,
                                                   memory_order_acquire);
        bool mqtt_suspended = atomic_load_explicit(&s_mqtt_suspended,
                                                   memory_order_acquire);
        if (realtime_media && !mqtt_suspended) {
            atomic_store_explicit(&s_mqtt_suspended, true,
                                  memory_order_release);
            ready_announced = false;
            ESP_LOGI(TAG,
                     "realtime media entered: pause MQTT/TLS free=%u largest=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                       MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (s_mqtt != NULL && !s_temp) {
                mqtt_stop();
            }
            ESP_LOGI(TAG,
                     "realtime media ready: MQTT/TLS paused free=%u largest=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                       MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        } else if (!realtime_media && mqtt_suspended) {
            atomic_store_explicit(&s_mqtt_suspended, false,
                                  memory_order_release);
            atomic_store_explicit(&s_retry, true, memory_order_release);
            ESP_LOGI(TAG,
                     "realtime media exited: resume business channel free=%u largest=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                       MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (bits & UNBOUND) {
            int ack = atomic_load(&s_last_ack);
            if (ack >= 0 && !wait_ack(ack, 3000))
                ESP_LOGW(TAG, "unbind ACK PUBACK not confirmed");
            mqtt_stop();
            xEventGroupClearBits(s_events, UNBOUND);
            emit(PLATFORM_UNBOUND, 0, 0, ESP_OK, NULL);
            ready_announced = false;
            atomic_store(&s_retry, true);
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (s_mqtt &&
            ((bits & AUTH_FAILED) || (s_token_expiry && time(NULL) >= s_token_expiry - 60))) {
            mqtt_stop();
            ready_announced = false;
            emit(PLATFORM_OFFLINE, 0, 0, ESP_FAIL, NULL);
            if (now - last_refresh > 5000)
                atomic_store(&s_retry, true);
            last_refresh = now;
        }
        if (!s_mqtt &&
            !atomic_load_explicit(&s_mqtt_suspended, memory_order_acquire) &&
            atomic_exchange(&s_retry, false)) {
            err = platform_time_sync();
            if (err == ESP_OK && !discovered) {
                err = discover();
                discovered = err == ESP_OK;
            }
            if (err == ESP_OK)
                err = login();
            message_buffer_release(&s_response);
            if (err != ESP_OK)
                ESP_LOGE(TAG, "startup paused: %s; correct cause then enter retry",
                         esp_err_to_name(err));
        }
        bits = xEventGroupGetBits(s_events);
        if (s_mqtt && !s_temp && (bits & MQTT_READY) && !(bits & (UNBOUND | AUTH_FAILED))) {
            atomic_store(&s_online, true);
            if (!ready_announced)
                ready_announced = emit(PLATFORM_ONLINE, 0, 0, ESP_OK, NULL);
            if (now - last_heartbeat >= 30000) {
                char topic[128], body[128];
                snprintf(topic, sizeof(topic), "device/%s/up", s_mqtt_id);
                snprintf(body, sizeof(body), "{\"type\":\"heartbeat\",\"seq\":%u,\"ts\":%lld}",
                         ++sequence, (long long)time(NULL));
                if (esp_mqtt_client_enqueue(s_mqtt, topic, body, 0, 0, 0, true) < 0)
                    ESP_LOGW(TAG, "heartbeat enqueue failed");
                last_heartbeat = now;
            }
        } else
            ready_announced = false;
        if (xQueueReceive(s_requests, &request, pdMS_TO_TICKS(100)) == pdTRUE)
            process_request(&request);
    }
}
esp_err_t platform_start(const char *ssid, const char *password, const char *url,
                         platform_observer_t observer)
{
    if (s_task || !ssid || !password || !url || !observer || !ssid[0] || strlen(ssid) > 32 ||
        strlen(password) > 64 || strlen(url) >= sizeof(s_discovery_url))
        return ESP_ERR_INVALID_ARG;
    if (strncmp(url, "https://", 8) != 0)
        return ESP_ERR_INVALID_ARG;
    s_observer = observer;
    strcpy(s_discovery_url, url);
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK)
        return err;
    snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    snprintf(s_client_id, sizeof(s_client_id), "c3-%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    s_events = xEventGroupCreateStatic(&s_event_storage);
    s_requests = xQueueCreateStatic(2, sizeof(request_t), s_request_buffer, &s_request_storage);
    err = platform_network_start(ssid, password);
    if (err != ESP_OK)
        return err;
    s_task = xTaskCreateStatic(platform_task, "platform", sizeof(s_task_stack), NULL, 4,
                               s_task_stack, &s_task_storage);
    return s_task ? ESP_OK : ESP_ERR_NO_MEM;
}
