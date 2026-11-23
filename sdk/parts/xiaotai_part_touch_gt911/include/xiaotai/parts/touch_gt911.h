// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: the GT911 capacitive touch controller on I2C, as a touch
 * driver for the display part (xiaotai/parts/display.h).
 *
 * start() pulses the reset pin; on boards where that reset is shared with the
 * panel, the display part starts this driver before the panel (touch_first)
 * and the reset is never pulsed again while the panel is started. */
#include "xiaotai/parts/display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [board] Touches no hardware; the configuration is copied. */
xiaotai_err_t xiaotai_touch_gt911_create(const xiaotai_touch_i2c_config_t *config, xiaotai_touch_t *touch);

#ifdef __cplusplus
}
#endif
