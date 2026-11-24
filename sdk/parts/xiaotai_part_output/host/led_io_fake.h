// SPDX-License-Identifier: MIT
#pragma once
#include "led_io.h"

void xiaotai_led_fake_fail(int pin, xiaotai_err_t err);
/* frame must have XT_LED_BYTES_MAX bytes. */
size_t xiaotai_led_fake_frame(int pin, uint8_t *frame, unsigned *calls);
