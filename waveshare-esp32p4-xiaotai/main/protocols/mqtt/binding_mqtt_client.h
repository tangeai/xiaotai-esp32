#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define BINDING_MQTT_DEVICE_ID_MAX_LEN 128
#define BINDING_MQTT_DEVICE_KEY_MAX_LEN 128

typedef void (*binding_mqtt_ready_cb_t)(void *ctx);
typedef bool (*binding_mqtt_cancel_cb_t)(void *ctx);

typedef struct {
    const char *broker_uri;
    const char *mac;
    const char *temp_client_id;
    const char *temp_token;
    uint32_t wait_timeout_ms;
    uint32_t ready_timeout_ms;
    binding_mqtt_ready_cb_t ready_cb;
    void *ready_ctx;
    binding_mqtt_cancel_cb_t should_cancel;
    void *cancel_ctx;
} binding_mqtt_client_config_t;

typedef struct {
    bool has_credentials;
    char device_id[BINDING_MQTT_DEVICE_ID_MAX_LEN];
    char device_key[BINDING_MQTT_DEVICE_KEY_MAX_LEN];
} binding_mqtt_auth_grant_t;

esp_err_t binding_mqtt_client_wait_auth_grant(const binding_mqtt_client_config_t *config,
                                              binding_mqtt_auth_grant_t *grant);
