#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_err.h"

esp_err_t starter_board_init(i2c_master_bus_handle_t bus);
esp_err_t starter_board_backlight(bool enabled);
esp_err_t starter_board_power_key(uint8_t *press);
esp_err_t starter_board_lcd_reset(void);
esp_err_t starter_board_camera_power(bool enabled);
esp_err_t starter_board_audio_observe(i2s_chan_handle_t rx);
/* Single persistent reader; ES7210 wire order: MIC1/MIC3-reference/MIC2/MIC4-unused. */
esp_err_t starter_board_capture(int16_t *out, size_t bytes);
typedef struct {
    uint32_t captured, overflow, invalid_dma_size;
    uint32_t peak[4], nonzero[4];
} starter_board_audio_stats_t;
starter_board_audio_stats_t starter_board_audio_stats(void);
esp_err_t starter_board_debug_power(void);
