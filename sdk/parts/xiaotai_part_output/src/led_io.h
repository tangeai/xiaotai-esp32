// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/parts/output.h"

#define XT_LED_BYTES_MAX (3U * XIAOTAI_LED_STRIP_PIXELS_MAX)
#define XT_LED_WAIT_MS 10

typedef struct xiaotai_led_io xiaotai_led_io_t;

xiaotai_err_t xiaotai_led_io_create(int pin, xiaotai_led_io_t **io);
/* Owns any asynchronous hardware copy. Caller can reuse data when it returns,
 * including on timeout; a second write may be refused while a frame drains. */
xiaotai_err_t xiaotai_led_io_send(xiaotai_led_io_t *io, const uint8_t *data, size_t bytes);
