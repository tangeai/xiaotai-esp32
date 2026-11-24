// SPDX-License-Identifier: MIT
#pragma once
#include "esp_lcd_io_spi.h"

esp_err_t xiaotai_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus, const esp_lcd_panel_io_spi_config_t *config,
                                      esp_lcd_panel_io_handle_t *out);
