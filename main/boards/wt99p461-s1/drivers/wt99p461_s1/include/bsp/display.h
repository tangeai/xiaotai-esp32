#pragma once

#include "esp_err.h"

esp_err_t bsp_display_brightness_set(int percent);
esp_err_t bsp_display_backlight_on(void);
esp_err_t bsp_display_backlight_off(void);
