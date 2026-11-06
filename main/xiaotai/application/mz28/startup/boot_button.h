#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef void (*boot_button_pressed_cb_t)(void);

esp_err_t boot_button_start(gpio_num_t gpio,
                            boot_button_pressed_cb_t single_click_cb,
                            boot_button_pressed_cb_t double_click_cb,
                            boot_button_pressed_cb_t quadruple_click_cb,
                            boot_button_pressed_cb_t long_press_cb,
                            uint32_t long_press_ms,
                            uint32_t multi_click_ms);
