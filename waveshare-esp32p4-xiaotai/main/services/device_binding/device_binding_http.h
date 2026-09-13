#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define DEVICE_BINDING_HTTP_CODE_MAX_LEN       8
#define DEVICE_BINDING_HTTP_TOKEN_MAX_LEN      1536
#define DEVICE_BINDING_HTTP_CLIENT_ID_MAX_LEN  64

typedef enum {
    DEVICE_BINDING_HTTP_REPORT_UNBOUND = 0,
    DEVICE_BINDING_HTTP_REPORT_RETRY_AFTER,
} device_binding_http_report_type_t;

typedef struct {
    device_binding_http_report_type_t type;
    int service_code;
    uint32_t retry_after_sec;
    char code[DEVICE_BINDING_HTTP_CODE_MAX_LEN];
    char temp_token[DEVICE_BINDING_HTTP_TOKEN_MAX_LEN];
    char temp_client_id[DEVICE_BINDING_HTTP_CLIENT_ID_MAX_LEN];
} device_binding_http_report_result_t;

esp_err_t device_binding_http_report(const char *api_base,
                                     const char *mac,
                                     const char *device_id,
                                     const char *device_key,
                                     device_binding_http_report_result_t *result);
