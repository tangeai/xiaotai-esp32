#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_AUTH_MQTT_TOKEN_MAX_LEN 1536

typedef struct {
    char mqtt_token[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN];
} device_auth_token_t;

esp_err_t device_auth_http_get_mqtt_token(const char *api_base,
                                          const char *device_id,
                                          const char *device_key,
                                          const char *mac,
                                          device_auth_token_t *token);

#ifdef __cplusplus
}
#endif
