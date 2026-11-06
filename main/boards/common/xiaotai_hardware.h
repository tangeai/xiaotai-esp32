#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct xiaotai_hardware {
    i2c_master_bus_handle_t i2c;
    esp_codec_dev_handle_t microphone;
    esp_codec_dev_handle_t speaker;
    struct esp_lcd_panel_io_t *panel_io;
    struct esp_lcd_panel_t *panel;
    struct esp_lcd_touch_s *touch;
} xiaotai_hardware_t;

enum {
    XIAOTAI_OUTPUT_LCD_CS = 1,
    XIAOTAI_OUTPUT_SPEAKER = 2,
    XIAOTAI_OUTPUT_CAMERA_PWDN = 4,
};

/* Borrowed handles: only Board creates/destroys physical resources. */
esp_err_t xiaotai_board_audio_prepare(void);
esp_err_t xiaotai_board_audio_open(xiaotai_hardware_t *hardware);
esp_err_t xiaotai_board_display_open(xiaotai_hardware_t *hardware);
esp_err_t xiaotai_board_output_set(uint8_t mask, bool high);
esp_err_t xiaotai_board_backlight_set(bool awake);
#ifdef __cplusplus
}
#endif
