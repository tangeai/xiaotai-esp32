#include "platform_internal.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
typedef struct {
    message_buffer_t *buffer;
    size_t limit, content_capacity;
    bool overflow;
    bool oom;
    unsigned retry_after;
} response_t;

typedef struct {
    const platform_binding_prompt_t *prompt;
    const atomic_bool *cancelled;
    size_t pcm_bytes;
    bool content_type_seen;
    bool content_type_pcm;
    bool has_carry;
    uint8_t carry;
    esp_err_t error;
} pcm_response_t;

static esp_err_t pcm_write(pcm_response_t *response, const uint8_t *data,
                           size_t length)
{
    int16_t samples[128];
    size_t sample_count = 0;
    size_t offset = 0;
    while (offset < length) {
        if (atomic_load_explicit(response->cancelled, memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (response->has_carry) {
            uint16_t value = (uint16_t)response->carry |
                             ((uint16_t)data[offset++] << 8);
            samples[sample_count++] = (int16_t)value;
            response->has_carry = false;
        }
        while (offset + 1 < length && sample_count < 128U) {
            uint16_t value = (uint16_t)data[offset] |
                             ((uint16_t)data[offset + 1] << 8);
            samples[sample_count++] = (int16_t)value;
            offset += 2;
        }
        if (sample_count > 0) {
            esp_err_t err = response->prompt->write_pcm8(
                samples, sample_count, response->prompt->user_data);
            if (err != ESP_OK) {
                return err;
            }
            response->pcm_bytes += sample_count * sizeof(samples[0]);
            sample_count = 0;
        }
        if (offset < length) {
            response->carry = data[offset++];
            response->has_carry = true;
        }
    }
    return ESP_OK;
}

static esp_err_t pcm_http_event(esp_http_client_event_t *event)
{
    pcm_response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != NULL &&
        event->header_value != NULL &&
        strcasecmp(event->header_key, "Content-Type") == 0) {
        response->content_type_seen = true;
        response->content_type_pcm =
            strncasecmp(event->header_value, "audio/pcm", 9) == 0;
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL ||
        event->data_len <= 0) {
        return ESP_OK;
    }
    if (!response->content_type_seen || !response->content_type_pcm) {
        response->error = ESP_ERR_INVALID_RESPONSE;
        return response->error;
    }
    response->error = pcm_write(response, event->data,
                                (size_t)event->data_len);
    return response->error;
}
static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_t *r = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        if (strcasecmp(event->header_key, "Retry-After") == 0) {
            char *end = NULL;
            unsigned long value = strtoul(event->header_value, &end, 10);
            if (end != event->header_value && *end == 0 && value <= 3600)
                r->retry_after = value;
        } else if (strcasecmp(event->header_key, "Content-Length") == 0) {
            char *end = NULL;
            unsigned long value = strtoul(event->header_value, &end, 10);
            if (end != event->header_value && *end == 0 && value < r->limit)
                r->content_capacity = (size_t)value + 1;
        }
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if (!r->buffer->data &&
            !message_buffer_begin(r->buffer, r->content_capacity > 1 ? r->content_capacity : r->limit)) {
            r->oom = true;
            return ESP_ERR_NO_MEM;
        }
        if (!message_buffer_append(r->buffer, r->buffer->used, event->data,
                                   (size_t)event->data_len)) {
            r->overflow = true;
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}
bool json_string_copy(const cJSON *object, const char *key, char *out, size_t capacity)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(v) || !v->valuestring[0] || strlen(v->valuestring) >= capacity)
        return false;
    memcpy(out, v->valuestring, strlen(v->valuestring) + 1);
    return true;
}
esp_err_t platform_http(const char *url, esp_http_client_method_t method, const char *body,
                        const char *token, const http_header_t *headers, size_t header_count,
                        message_buffer_t *response, size_t limit, http_result_t *result)
{
    if (!url || !response || limit < 2 || !result)
        return ESP_ERR_INVALID_ARG;
    message_buffer_release(response);
    memset(result, 0, sizeof(*result));
    result->code = -1;
    response_t r = {.buffer = response, .limit = limit};
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &r,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 1536,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    char authorization[1100];
    if (token && token[0]) {
        int n = snprintf(authorization, sizeof(authorization), "Bearer %s", token);
        if (n < 0 || n >= sizeof(authorization))
            err = ESP_ERR_INVALID_SIZE;
        else
            err = esp_http_client_set_header(client, "Authorization", authorization);
    }
    for (size_t i = 0; err == ESP_OK && i < header_count; ++i)
        err = esp_http_client_set_header(client, headers[i].name, headers[i].value);
    if (err == ESP_OK && body) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (err == ESP_OK)
            err = esp_http_client_set_post_field(client, body, strlen(body));
    }
    int64_t begin = esp_timer_get_time();
    ESP_LOGI("thing_http", "begin: method=%d free=%u largest=%u", method,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (err == ESP_OK)
        err = esp_http_client_perform(client);
    result->status = esp_http_client_get_status_code(client);
    result->retry_after = r.retry_after;
    if (r.oom)
        err = ESP_ERR_NO_MEM;
    else if (r.overflow)
        err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(client))
        err = ESP_ERR_INVALID_RESPONSE;
    esp_http_client_cleanup(client);
    memset(authorization, 0, sizeof(authorization));
    if (err == ESP_OK && !response->data && !message_buffer_begin(response, 2))
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        cJSON *root = memchr(response->data, 0, response->used) ? NULL : cJSON_ParseWithOpts(response->data, NULL, true);
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (cJSON_IsNumber(code))
            result->code = code->valueint;
        if (!cJSON_IsObject(root)) err = ESP_ERR_INVALID_RESPONSE;
        cJSON_Delete(root);
        if (result->status < 200 || result->status >= 300)
            err = ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI("thing_http", "done: http=%d code=%d bytes=%u response_alloc=%u elapsed_ms=%lld result=%s",
             result->status, result->code, (unsigned)response->used, (unsigned)response->capacity,
             (long long)((esp_timer_get_time() - begin) / 1000), esp_err_to_name(err));
    return err;
}

esp_err_t platform_http_pcm(const char *url, const char *token,
                            const platform_binding_prompt_t *prompt,
                            const atomic_bool *cancelled, size_t *pcm_bytes,
                            int *status)
{
    if (url == NULL || token == NULL || token[0] == '\0' || prompt == NULL ||
        prompt->write_pcm8 == NULL || cancelled == NULL || pcm_bytes == NULL ||
        status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pcm_bytes = 0;
    *status = 0;
    pcm_response_t response = {
        .prompt = prompt,
        .cancelled = cancelled,
        .error = ESP_OK,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = pcm_http_event,
        .user_data = &response,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char authorization[1100];
    int written = snprintf(authorization, sizeof(authorization), "Bearer %s",
                           token);
    esp_err_t err = written < 0 || written >= sizeof(authorization)
                        ? ESP_ERR_INVALID_SIZE
                        : esp_http_client_set_header(client, "Authorization",
                                                     authorization);
    int64_t begin = esp_timer_get_time();
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    *status = esp_http_client_get_status_code(client);
    if (response.error != ESP_OK) {
        err = response.error;
    } else if (atomic_load_explicit(cancelled, memory_order_acquire)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (err == ESP_OK &&
               !esp_http_client_is_complete_data_received(client)) {
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (err == ESP_OK &&
               (*status < 200 || *status >= 300 ||
                !response.content_type_pcm || response.has_carry ||
                response.pcm_bytes == 0U)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    *pcm_bytes = response.pcm_bytes;
    esp_http_client_cleanup(client);
    memset(authorization, 0, sizeof(authorization));
    ESP_LOGI("thing_http",
             "binding prompt: http=%d bytes=%u elapsed_ms=%lld result=%s",
             *status, (unsigned)*pcm_bytes,
             (long long)((esp_timer_get_time() - begin) / 1000),
             esp_err_to_name(err));
    return err;
}
