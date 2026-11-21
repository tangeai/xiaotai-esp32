// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/parts/panel_ssd1306.h"

typedef enum {
    XT_OLED_PROBE, XT_OLED_NEW_IO, XT_OLED_NEW_PANEL, XT_OLED_CONFIGURE,
    XT_OLED_DRAW, XT_OLED_SWITCH, XT_OLED_DEL_PANEL, XT_OLED_DEL_IO, XT_OLED_OPS,
} xt_oled_operation_t;

typedef struct {
    xt_oled_operation_t log[32];
    size_t count;
    xiaotai_panel_ssd1306_config_t config;
    int probe_timeout_ms, io_timeout_ms;
    bool mirror_x, mirror_y, invert, on;
    size_t frame_bytes;
    uint8_t frame[1024];
} xt_oled_fake_t;

void xt_oled_fake_reset(void);
void xt_oled_fake_fail(xt_oled_operation_t op, xiaotai_err_t error);
const xt_oled_fake_t *xt_oled_fake_state(void);
