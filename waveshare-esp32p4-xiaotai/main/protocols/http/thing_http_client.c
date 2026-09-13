#include "thing_http_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "thing_http";

#define THING_HTTP_DEFAULT_TIMEOUT_MS 10000U
#define THING_HTTP_BUFFER_SIZE       2048
#define THING_HTTP_TX_BUFFER_SIZE    2048

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} thing_http_response_t;

static bool thing_http_is_https(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

bool thing_http_error_is_recoverable(esp_err_t ret)
{
    switch (ret) {
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_HTTP_WRITE_DATA:
    case ESP_ERR_HTTP_FETCH_HEADER:
    case ESP_ERR_HTTP_CONNECTING:
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
    case ESP_ERR_HTTP_READ_TIMEOUT:
    case ESP_ERR_HTTP_INCOMPLETE_DATA:
        return true;
    default:
        return false;
    }
}

esp_err_t thing_http_join_url(char *out, size_t out_size, const char *base_url, const char *path)
{
    if (out == NULL || out_size == 0 || base_url == NULL || base_url[0] == '\0' ||
        path == NULL || path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') {
        base_len--;
    }

    int written = snprintf(out, out_size, "%.*s%s", (int)base_len, base_url, path);
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t thing_http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->event_id != HTTP_EVENT_ON_DATA ||
        event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    thing_http_response_t *response = (thing_http_response_t *)event->user_data;
    if (response == NULL || response->data == NULL || response->cap == 0) {
        return ESP_OK;
    }
    if (response->len + (size_t)event->data_len + 1 > response->cap) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(response->data + response->len, event->data, (size_t)event->data_len);
    response->len += (size_t)event->data_len;
    response->data[response->len] = '\0';
    return ESP_OK;
}

static esp_http_client_method_t thing_http_method_from_string(const char *method)
{
    if (method != NULL && strcmp(method, "GET") == 0) {
        return HTTP_METHOD_GET;
    }
    if (method != NULL && strcmp(method, "PUT") == 0) {
        return HTTP_METHOD_PUT;
    }
    return HTTP_METHOD_POST;
}

esp_err_t thing_http_request_json(const thing_http_request_t *request,
                                  char *response_buf,
                                  size_t response_buf_size,
                                  int *status_code)
{
    if (request == NULL || request->url == NULL || request->url[0] == '\0' ||
        response_buf == NULL || response_buf_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!thing_http_is_https(request->url)) {
        ESP_LOGE(TAG, "plaintext HTTP request rejected");
        return ESP_ERR_INVALID_ARG;
    }

    thing_http_response_t response = {
        .data = response_buf,
        .cap = response_buf_size,
    };
    response_buf[0] = '\0';
    if (status_code != NULL) {
        *status_code = 0;
    }
    uint32_t timeout_ms = request->timeout_ms != 0 ? request->timeout_ms : THING_HTTP_DEFAULT_TIMEOUT_MS;

    esp_http_client_config_t config = {
        .url = request->url,
        .method = thing_http_method_from_string(request->method),
        .event_handler = thing_http_event_handler,
        .user_data = &response,
        .timeout_ms = (int)timeout_ms,
        .buffer_size = THING_HTTP_BUFFER_SIZE,
        .buffer_size_tx = THING_HTTP_TX_BUFFER_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };

    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "request begin: method=%s timeout=%ums internal_free=%u largest=%u",
             config.method == HTTP_METHOD_GET ? "GET" :
             config.method == HTTP_METHOD_PUT ? "PUT" : "POST",
             (unsigned)timeout_ms,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "request init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Connection", "close");
    for (size_t index = 0; index < request->header_count; ++index) {
        const thing_http_header_t *header = &request->headers[index];
        if (header->name != NULL && header->value != NULL) {
            esp_http_client_set_header(client, header->name, header->value);
        }
    }
    if (config.method == HTTP_METHOD_POST || config.method == HTTP_METHOD_PUT) {
        const char *body = request->body != NULL ? request->body : "";
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t ret = esp_http_client_perform(client);
    if (ret == ESP_OK && status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    int status = esp_http_client_get_status_code(client);
    int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGI(TAG,
             "request done: ret=%s status=%d elapsed=%lldms bytes=%u internal_free=%u largest=%u",
             esp_err_to_name(ret),
             status,
             (long long)elapsed_ms,
             (unsigned)response.len,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    esp_http_client_cleanup(client);
    return ret;
}
