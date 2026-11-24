// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"

typedef struct {
    bool init_sccb;
    i2c_master_bus_handle_t i2c_handle;
    uint32_t freq;
} csi_test_sccb_t;
typedef struct {
    csi_test_sccb_t sccb_config;
    int reset_pin, pwdn_pin;
} esp_video_init_csi_config_t;
typedef struct {
    const esp_video_init_csi_config_t *csi;
} esp_video_init_config_t;

int esp_video_init(const esp_video_init_config_t *config);
