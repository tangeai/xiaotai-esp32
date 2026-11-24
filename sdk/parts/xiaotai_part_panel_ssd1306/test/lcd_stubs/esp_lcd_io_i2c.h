// SPDX-License-Identifier: MIT
#pragma once

#include "driver/i2c_master.h"

typedef struct esp_lcd_panel_io_t esp_lcd_panel_io_t;
typedef esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef struct esp_lcd_panel_io_event_data_t esp_lcd_panel_io_event_data_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t,
                                                     esp_lcd_panel_io_event_data_t *, void *);
typedef struct {
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
} esp_lcd_panel_io_callbacks_t;
typedef struct {
    uint32_t dev_addr, scl_speed_hz;
    size_t control_phase_bytes;
    uint8_t dc_bit_offset;
    int lcd_cmd_bits, lcd_param_bits;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
    struct { unsigned disable_control_phase : 1, dc_low_on_data : 1; } flags;
} esp_lcd_panel_io_i2c_config_t;
