#pragma once

#include "esp_err.h"

#define DEVICE_BINDING_MAC_MAX_LEN 13

typedef struct {
    char mac[DEVICE_BINDING_MAC_MAX_LEN];
} device_binding_identity_t;

esp_err_t device_identity_get(device_binding_identity_t *identity);
