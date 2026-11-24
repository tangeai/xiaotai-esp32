// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: FT5x06-family capacitive touch controllers (FT5x06, FT6336)
 * on I2C, as a touch driver for the display part (xiaotai/parts/display.h). */
#include "xiaotai/parts/display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [board] Touches no hardware; the configuration is copied. start() resets the
 * controller when a reset pin is given. */
xiaotai_err_t xiaotai_touch_ft5x06_create(const xiaotai_touch_i2c_config_t *config, xiaotai_touch_t *touch);

#ifdef __cplusplus
}
#endif
