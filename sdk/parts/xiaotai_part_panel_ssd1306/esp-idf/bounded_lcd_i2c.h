// SPDX-License-Identifier: MIT
#pragma once
#include "esp_lcd_io_i2c.h"

/* The backing bus must use synchronous transfers (as xiaotai_part_bus does). */
esp_err_t xiaotai_ssd1306_new_bounded_lcd_i2c(i2c_master_bus_handle_t bus,
    const esp_lcd_panel_io_i2c_config_t *config, int timeout_ms, esp_lcd_panel_io_handle_t *output);
