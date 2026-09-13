#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DEVICE_UUID_MAX_LEN 37
#define DEVICE_UUID_MIN_LEN 4
#define DEVICE_UUID_EDIT_MAX_LEN 12

typedef struct {
    char uuid[DEVICE_UUID_MAX_LEN];
    uint8_t cpu_usage_percent;
    bool boot_pressed;
} device_state_t;

typedef void (*device_boot_button_cb_t)(bool pressed, void *ctx);

esp_err_t device_init(device_boot_button_cb_t cb, void *ctx);
esp_err_t device_set_uuid(const char *uuid);
void device_get_state(device_state_t *state);