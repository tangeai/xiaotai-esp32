#include "device_binding_http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "app_memory_policy.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static const char *TAG = "binding_http";

#define DEVICE_BINDING_HTTP_TIMEOUT_MS 10000
#define DEVICE_BINDING_HTTP_RETRY_COUNT 2
#define DEVICE_BINDING_HTTP_RETRY_DELAY_MS 1000U
#define DEVICE_BINDING_HTTP_RESPONSE_MAX_LEN 4096
#define DEVICE_BINDING_HTTP_URL_MAX_LEN 256
#define DEVICE_BINDING_HTTP_BODY_MAX_LEN 320
#define DEVICE_BINDING_HTTP_CODE_OK 200
#define DEVICE_BINDING_HTTP_CODE_VERIFY_PENDING 40901
#define DEVICE_BINDING_HTTP_CODE_RATE_LIMIT 429
#define DEVICE_BINDING_HTTP_DEFAULT_RETRY_AFTER_SEC 10U
#define DEVICE_BINDING_HTTP_NONCE_HEX_LEN 16
#define DEVICE_BINDING_HTTP_SIGNATURE_MAX 96

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    uint32_t retry_after_sec;
    int64_t start_us;
    int64_t connected_us;
    int64_t headers_sent_us;
    int64_t first_header_us;
    int64_t first_data_us;
    int64_t finish_us;
    int64_t disconnected_us;
    int header_events;
    int data_events;
    int disconnect_events;
    esp_err_t last_event_error;
} device_binding_http_response_t;

typedef struct {
    bool enabled;
    const char *device_id;
    const char *timestamp;
    const char *nonce;
    const char *signature;
} device_binding_http_auth_headers_t;

static bool device_binding_http_is_https(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

static bool device_binding_http_has_value(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static void device_binding_http_make_nonce(char *nonce, size_t nonce_size)
{
    if (nonce == NULL || nonce_size == 0U) {
        return;
    }

    nonce[0] = '\0';
    uint8_t raw[8];
    esp_fill_random(raw, sizeof(raw));
    for (size_t index = 0; index < sizeof(raw) && (index * 2U + 2U) < nonce_size; ++index) {
        snprintf(nonce + index * 2U, 3, "%02x", raw[index]);
    }
}

static esp_err_t device_binding_http_sign(const char *device_id,
                                          const char *device_key,
                                          const char *timestamp,
                                          const char *nonce,
                                          char *signature,
                                          size_t signature_size)
{
    char raw[256];
    uint8_t digest[32];
    size_t encoded_len = 0;

    if (!device_binding_http_has_value(device_id) ||
        !device_binding_http_has_value(device_key) ||
        !device_binding_http_has_value(timestamp) ||
        !device_binding_http_has_value(nonce) ||
        signature == NULL || signature_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(raw, sizeof(raw), "%s%s%s", device_id, timestamp, nonce);
    if (written <= 0 || written >= (int)sizeof(raw)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        return ESP_FAIL;
    }
    if (mbedtls_md_hmac(md_info,
                        (const uint8_t *)device_key,
                        strlen(device_key),
                        (const uint8_t *)raw,
                        strlen(raw),
                        digest) != 0) {
        return ESP_FAIL;
    }
    if (mbedtls_base64_encode((uint8_t *)signature,
                              signature_size - 1U,
                              &encoded_len,
                              digest,
                              sizeof(digest)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    signature[encoded_len] = '\0';
    return ESP_OK;
}

static void device_binding_http_join_url(char *url,
                                         size_t url_size,
                                         const char *api_base,
                                         const char *path)
{
    size_t base_len = strlen(api_base);

    while (base_len > 0U && api_base[base_len - 1U] == '/') {
        base_len--;
    }
    snprintf(url, url_size, "%.*s%s", (int)base_len, api_base, path);
}

static int64_t device_binding_http_elapsed_ms(const device_binding_http_response_t *response)
{
    if (response == NULL || response->start_us == 0) {
        return 0;
    }
    return (esp_timer_get_time() - response->start_us) / 1000;
}

static const char *device_binding_http_last_stage(const device_binding_http_response_t *response)
{
    if (response == NULL) {
        return "none";
    }
    if (response->finish_us != 0) {
        return "finish";
    }
    if (response->first_data_us != 0) {
        return "body";
    }
    if (response->first_header_us != 0) {
        return "header";
    }
    if (response->headers_sent_us != 0) {
        return "sent";
    }
    if (response->connected_us != 0) {
        return "connected";
    }
    return "init";
}

static esp_err_t device_binding_http_event_handler(esp_http_client_event_t *event)
{
    device_binding_http_response_t *response = NULL;

    if (event == NULL) {
        return ESP_OK;
    }

    response = (device_binding_http_response_t *)event->user_data;
    if (response == NULL) {
        return ESP_OK;
    }

    switch (event->event_id) {
    case HTTP_EVENT_ERROR:
        response->last_event_error = ESP_FAIL;
        ESP_LOGW(TAG, "binding report event error: elapsed=%lldms stage=%s",
                 (long long)device_binding_http_elapsed_ms(response),
                 device_binding_http_last_stage(response));
        return ESP_OK;
    case HTTP_EVENT_ON_CONNECTED:
        response->connected_us = esp_timer_get_time();
        return ESP_OK;
    case HTTP_EVENT_HEADERS_SENT:
        response->headers_sent_us = esp_timer_get_time();
        return ESP_OK;
    case HTTP_EVENT_ON_HEADER:
        response->header_events++;
        if (response->first_header_us == 0) {
            response->first_header_us = esp_timer_get_time();
        }
        if (event->header_key != NULL && event->header_value != NULL &&
            strcasecmp(event->header_key, "Retry-After") == 0) {
            unsigned long retry_after = strtoul(event->header_value, NULL, 10);
            if (retry_after > 0UL && retry_after <= 3600UL) {
                response->retry_after_sec = (uint32_t)retry_after;
            }
        }
        return ESP_OK;
    case HTTP_EVENT_ON_DATA:
        if (event->data == NULL || event->data_len <= 0 || response->data == NULL) {
            return ESP_OK;
        }
        response->data_events++;
        if (response->first_data_us == 0) {
            response->first_data_us = esp_timer_get_time();
        }
        if (response->len + (size_t)event->data_len + 1U > response->cap) {
            response->last_event_error = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
        memcpy(response->data + response->len, event->data, (size_t)event->data_len);
        response->len += (size_t)event->data_len;
        response->data[response->len] = '\0';
        return ESP_OK;
    case HTTP_EVENT_ON_FINISH:
        response->finish_us = esp_timer_get_time();
        return ESP_OK;
    case HTTP_EVENT_DISCONNECTED:
        response->disconnect_events++;
        response->disconnected_us = esp_timer_get_time();
        return ESP_OK;
    default:
        return ESP_OK;
    }
}

static bool device_binding_http_should_retry(esp_err_t ret,
                                             int status,
                                             const device_binding_http_response_t *response)
{
    return ret != ESP_OK &&
           response != NULL &&
           status == 0 &&
           response->len == 0 &&
           response->first_header_us == 0 &&
           response->last_event_error != ESP_ERR_NO_MEM;
}

static esp_err_t device_binding_http_post_json(const char *url,
                                               const char *body,
                                               const device_binding_http_auth_headers_t *auth,
                                               char *response_buf,
                                               size_t response_buf_size,
                                               int *status_code,
                                               uint32_t *retry_after_sec)
{
    esp_err_t ret = ESP_OK;
    int status = 0;

    if (url == NULL || body == NULL || response_buf == NULL || response_buf_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    response_buf[0] = '\0';
    if (status_code != NULL) {
        *status_code = 0;
    }
    if (retry_after_sec != NULL) {
        *retry_after_sec = 0;
    }

    for (uint8_t attempt = 1; attempt <= (DEVICE_BINDING_HTTP_RETRY_COUNT + 1U); ++attempt) {
        device_binding_http_response_t response = {
            .data = response_buf,
            .cap = response_buf_size,
            .start_us = esp_timer_get_time(),
        };
        response_buf[0] = '\0';
        if (status_code != NULL) {
            *status_code = 0;
        }
        if (retry_after_sec != NULL) {
            *retry_after_sec = 0;
        }

        esp_http_client_config_t http_config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .event_handler = device_binding_http_event_handler,
            .user_data = &response,
            .timeout_ms = DEVICE_BINDING_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_config);
        if (client == NULL) {
            return ESP_ERR_NO_MEM;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Connection", "close");
        if (auth != NULL && auth->enabled) {
            esp_http_client_set_header(client, "X-Device-Id", auth->device_id);
            esp_http_client_set_header(client, "X-Timestamp", auth->timestamp);
            esp_http_client_set_header(client, "X-Nonce", auth->nonce);
            esp_http_client_set_header(client, "X-Signature", auth->signature);
        }
        esp_http_client_set_post_field(client, body, (int)strlen(body));

        ESP_LOGD(TAG,
                 "binding report request begin: attempt=%u/%u timeout=%ums body_len=%u",
                 (unsigned)attempt,
                 (unsigned)(DEVICE_BINDING_HTTP_RETRY_COUNT + 1U),
                 (unsigned)DEVICE_BINDING_HTTP_TIMEOUT_MS,
                 (unsigned)strlen(body));
        ret = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        if (ret == ESP_OK && status_code != NULL) {
            *status_code = status;
        }
        if (retry_after_sec != NULL) {
            *retry_after_sec = response.retry_after_sec;
        }
        ESP_LOGD(TAG,
                 "binding report request done: ret=%s status=%d elapsed=%lldms stage=%s bytes=%u retry_after=%us hdr=%d data=%d disc=%d event_err=%s",
                 esp_err_to_name(ret),
                 status,
                 (long long)device_binding_http_elapsed_ms(&response),
                 device_binding_http_last_stage(&response),
                 (unsigned)response.len,
                 (unsigned)response.retry_after_sec,
                 response.header_events,
                 response.data_events,
                 response.disconnect_events,
                 esp_err_to_name(response.last_event_error));
        esp_http_client_cleanup(client);

        bool no_response_failure = device_binding_http_should_retry(ret, status, &response);
        if (attempt <= DEVICE_BINDING_HTTP_RETRY_COUNT && no_response_failure) {
            uint32_t delay_ms = DEVICE_BINDING_HTTP_RETRY_DELAY_MS * attempt;
            ESP_LOGW(TAG,
                     "binding report retry: ret=%s stage=%s wait_ms=%u",
                     esp_err_to_name(ret),
                     device_binding_http_last_stage(&response),
                     (unsigned)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }
        if (no_response_failure && ret == ESP_FAIL) {
            ret = ESP_ERR_HTTP_CONNECT;
        }
        break;
    }
    return ret;
}

static const cJSON *device_binding_pick_data_object(const cJSON *root)
{
    const cJSON *data = NULL;

    if (!cJSON_IsObject(root)) {
        return NULL;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsObject(data) ? data : root;
}

static esp_err_t device_binding_parse_report_response(const char *json,
                                                      device_binding_http_report_result_t *result)
{
    cJSON *root = NULL;
    const cJSON *code = NULL;
    const cJSON *data = NULL;
    const cJSON *verify_code = NULL;
    const cJSON *temp_token = NULL;
    const cJSON *temp_client_id = NULL;

    if (json == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    data = device_binding_pick_data_object(root);
    if (data == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(result, 0, sizeof(*result));
    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    result->service_code = cJSON_IsNumber(code) ? code->valueint : 0;

    verify_code = cJSON_GetObjectItemCaseSensitive(data, "code");
    temp_token = cJSON_GetObjectItemCaseSensitive(data, "temp_token");
    temp_client_id = cJSON_GetObjectItemCaseSensitive(data, "temp_client_id");
    if (!cJSON_IsString(verify_code) || verify_code->valuestring[0] == '\0' ||
        !cJSON_IsString(temp_token) || temp_token->valuestring[0] == '\0' ||
        !cJSON_IsString(temp_client_id) || temp_client_id->valuestring[0] == '\0') {
        if (result->service_code == DEVICE_BINDING_HTTP_CODE_VERIFY_PENDING ||
            result->service_code == DEVICE_BINDING_HTTP_CODE_RATE_LIMIT) {
            result->type = DEVICE_BINDING_HTTP_REPORT_RETRY_AFTER;
            cJSON_Delete(root);
            return ESP_OK;
        }
        if (result->service_code != 0 && result->service_code != DEVICE_BINDING_HTTP_CODE_OK) {
            ESP_LOGW(TAG, "binding report service error: code=%d", result->service_code);
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    result->type = DEVICE_BINDING_HTTP_REPORT_UNBOUND;
    strlcpy(result->code, verify_code->valuestring, sizeof(result->code));
    strlcpy(result->temp_token, temp_token->valuestring, sizeof(result->temp_token));
    strlcpy(result->temp_client_id, temp_client_id->valuestring, sizeof(result->temp_client_id));
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t device_binding_http_report(const char *api_base,
                                     const char *mac,
                                     const char *device_id,
                                     const char *device_key,
                                     device_binding_http_report_result_t *result)
{
    char url[DEVICE_BINDING_HTTP_URL_MAX_LEN] = {0};
    char body[DEVICE_BINDING_HTTP_BODY_MAX_LEN] = {0};
    char timestamp[24] = {0};
    char nonce[DEVICE_BINDING_HTTP_NONCE_HEX_LEN + 1] = {0};
    char signature[DEVICE_BINDING_HTTP_SIGNATURE_MAX] = {0};
    device_binding_http_auth_headers_t auth = {0};
    char *response = NULL;
    int status_code = 0;
    uint32_t retry_after_sec = 0;
    esp_err_t ret = ESP_OK;
    bool has_device_id = device_binding_http_has_value(device_id);
    bool has_device_key = device_binding_http_has_value(device_key);

    if (api_base == NULL || api_base[0] == '\0' ||
        mac == NULL || mac[0] == '\0' ||
        result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!device_binding_http_is_https(api_base)) {
        ESP_LOGE(TAG, "plaintext binding HTTP transport rejected");
        return ESP_ERR_INVALID_ARG;
    }
    if (has_device_id != has_device_key) {
        return ESP_ERR_INVALID_ARG;
    }

    device_binding_http_join_url(url, sizeof(url), api_base, "/v1/device/report");
    snprintf(body, sizeof(body), "{\"mac\":\"%s\"}", mac);
    if (has_device_id) {
        snprintf(timestamp, sizeof(timestamp), "%lld", (long long)time(NULL));
        device_binding_http_make_nonce(nonce, sizeof(nonce));
        ret = device_binding_http_sign(device_id,
                                       device_key,
                                       timestamp,
                                       nonce,
                                       signature,
                                       sizeof(signature));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "binding report signature failed: %s", esp_err_to_name(ret));
            return ret;
        }
        auth.enabled = true;
        auth.device_id = device_id;
        auth.timestamp = timestamp;
        auth.nonce = nonce;
        auth.signature = signature;
    }

    response = app_memory_calloc_psram(1, DEVICE_BINDING_HTTP_RESPONSE_MAX_LEN);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG,
             "binding report begin: signed=%d device_id_len=%u",
             auth.enabled ? 1 : 0,
             has_device_id ? (unsigned)strlen(device_id) : 0U);
    ret = device_binding_http_post_json(url,
                                        body,
                                        &auth,
                                        response,
                                        DEVICE_BINDING_HTTP_RESPONSE_MAX_LEN,
                                        &status_code,
                                        &retry_after_sec);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "binding report request failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = device_binding_parse_report_response(response, result);
    if (ret == ESP_OK && result->type == DEVICE_BINDING_HTTP_REPORT_RETRY_AFTER) {
        result->retry_after_sec = retry_after_sec != 0U ?
                                  retry_after_sec :
                                  DEVICE_BINDING_HTTP_DEFAULT_RETRY_AFTER_SEC;
        ESP_LOGD(TAG,
                 "binding report pending: service_code=%d retry_after=%us",
                 result->service_code,
                 (unsigned)result->retry_after_sec);
        ret = ESP_OK;
        goto cleanup;
    }
    if (status_code != 200 && ret != ESP_OK) {
        ESP_LOGW(TAG, "binding report HTTP status=%d body_len=%u",
                 status_code,
                 (unsigned)strlen(response));
        goto cleanup;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "binding report HTTP status=%d body_len=%u",
                 status_code,
                 (unsigned)strlen(response));
        ret = ESP_FAIL;
        goto cleanup;
    }
cleanup:
    free(response);
    return ret;
}
