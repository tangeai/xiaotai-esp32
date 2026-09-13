#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DEVICE_BINDING_CODE_MAX_LEN      8
#define DEVICE_BINDING_DEVICE_ID_MAX_LEN 128
#define DEVICE_BINDING_DEVICE_KEY_MAX_LEN 128
#define DEVICE_BINDING_MESSAGE_MAX_LEN   96

typedef enum {
    DEVICE_BINDING_STATE_DISABLED = 0,
    DEVICE_BINDING_STATE_IDLE,
    DEVICE_BINDING_STATE_REPORTING,
    DEVICE_BINDING_STATE_WAITING_USER,
    DEVICE_BINDING_STATE_BOUND,
    DEVICE_BINDING_STATE_ERROR,
} device_binding_state_t;

typedef esp_err_t (*device_binding_save_credentials_cb_t)(const char *device_id,
                                                          const char *device_key,
                                                          void *ctx);

typedef struct {
    char device_id[DEVICE_BINDING_DEVICE_ID_MAX_LEN];
    char device_key[DEVICE_BINDING_DEVICE_KEY_MAX_LEN];
} device_binding_credentials_t;

typedef esp_err_t (*device_binding_load_credentials_cb_t)(device_binding_credentials_t *credentials,
                                                          void *ctx);

typedef struct {
    bool enabled;
    const char *api_base;
    const char *mqtt_uri;
    uint32_t wait_timeout_ms;
    device_binding_load_credentials_cb_t load_credentials;
    device_binding_save_credentials_cb_t save_credentials;
    void *ctx;
} device_binding_config_t;

typedef struct {
    device_binding_state_t state;
    bool running;
    char mac[16];
    char code[DEVICE_BINDING_CODE_MAX_LEN];
    char device_id[DEVICE_BINDING_DEVICE_ID_MAX_LEN];
    esp_err_t last_error;
    char message[DEVICE_BINDING_MESSAGE_MAX_LEN];
} device_binding_snapshot_t;

esp_err_t device_binding_init(const device_binding_config_t *config);
esp_err_t device_binding_start_async(const char *reason);
void device_binding_reset_state(const char *reason);
void device_binding_get_snapshot(device_binding_snapshot_t *snapshot);
