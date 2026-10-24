// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

// 78/xiaozhi-esp32 v2.5.2, main/boards/xmini/c3-v3/config.h.
#define XMINI_I2C_SDA 0
#define XMINI_I2C_SCL 1
#define XMINI_I2S_MCLK 8
#define XMINI_I2S_BCLK 7
#define XMINI_I2S_WS 5
#define XMINI_I2S_DIN 6
#define XMINI_I2S_DOUT 4
#define XMINI_PA_GPIO 10
#define XMINI_BOOT_GPIO 9
#define XMINI_OLED_WIDTH 128
#define XMINI_OLED_HEIGHT 64
#define XMINI_FRAMEBUFFER_BYTES 1024

esp_err_t xmini_board_init(void);
i2c_master_bus_handle_t xmini_board_i2c(void);
bool xmini_board_button_down(void);
esp_err_t xmini_board_display(const uint8_t frame[XMINI_FRAMEBUFFER_BYTES]);
