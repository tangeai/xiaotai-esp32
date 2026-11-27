// SPDX-License-Identifier: MIT
#pragma once

#include "esp_lcd_io_i2c.h"

struct esp_lcd_panel_io_t {
    esp_err_t (*del)(esp_lcd_panel_io_t *);
    esp_err_t (*rx_param)(esp_lcd_panel_io_t *, int, void *, size_t);
    esp_err_t (*tx_param)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*tx_color)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*register_event_callbacks)(esp_lcd_panel_io_handle_t, const esp_lcd_panel_io_callbacks_t *, void *);
};
