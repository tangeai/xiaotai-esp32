// SPDX-License-Identifier: MIT
#pragma once
#include "camera_dvp_io.h"

typedef struct {
    xiaotai_camera_dvp_config_t config;
    xt_dvp_size_t size;
    unsigned opens, closes, takes, returns, flips, mirror;
} xt_dvp_fake_status_t;

typedef struct {
    uint16_t sensor_id; /* 0 uses the configuration's PID */
    bool sensor_missing, external_dma, fail_mirror, fail_flip;
    bool partial_open;
    xiaotai_err_t open_error, close_error;
} xt_dvp_fake_script_t;

void xt_dvp_fake_script(const xt_dvp_fake_script_t *script);
xt_dvp_fake_status_t xt_dvp_fake_status(void);
/* A frame enters the fixed-length driver pool; sizes or formats may be
 * injected independently of what open() requested. */
void xt_dvp_fake_frame(uint8_t first, size_t size, size_t width, size_t height, uint32_t format, int64_t stamp);
