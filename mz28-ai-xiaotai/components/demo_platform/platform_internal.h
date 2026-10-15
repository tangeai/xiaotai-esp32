#pragma once
#include <stdatomic.h>
#include "demo_platform.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "message_buffer.h"
typedef struct {
    const char *name;
    const char *value;
} http_header_t;
typedef struct {
    int status;
    int code;
    unsigned retry_after;
} http_result_t;
esp_err_t platform_http(const char *url, esp_http_client_method_t method, const char *body,
                        const char *token, const http_header_t *headers, size_t header_count,
                        message_buffer_t *response, size_t limit, http_result_t *result);
esp_err_t platform_http_pcm(const char *url, const char *token,
                            const platform_binding_prompt_t *prompt,
                            const atomic_bool *cancelled, size_t *pcm_bytes,
                            int *status);
bool json_string_copy(const cJSON *object, const char *key, char *out, size_t capacity);
esp_err_t platform_network_start(const char *ssid, const char *password);
bool platform_network_ready(void);
esp_err_t platform_time_sync(void);
esp_err_t platform_identity_load(platform_identity_t *identity);
esp_err_t platform_identity_save(const platform_identity_t *identity);
