// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: SSD1306-class monochrome panels on I2C, as a panel driver
 * for the display part (xiaotai/parts/display.h).
 *
 * The panel takes whole frames in XIAOTAI_PIXEL_MONO1_VLSB (the controller's
 * page layout): width * height / 8 bytes, 1024 for 128x64. Every bus transfer
 * has a bounded timeout. A frame can require multiple transfers (column
 * range, page range, pixel data); the limit applies to each transfer.
 *
 * Calling tasks: create is [board]; the driver functions are called by the
 * display part only. */
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/parts/display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t address;            /* 7-bit */
    uint32_t hz;
    uint16_t transfer_timeout_ms;   /* per transfer; 0: 100 */
    uint8_t width, height;      /* 128x64, 128x32 */
    bool mirror_x, mirror_y;
    bool invert;
} xiaotai_panel_ssd1306_config_t;

/* [board] Copies the config into an object that lives until power-off; touches
 * no hardware. start() probes the address (XIAOTAI_ERR_NOT_FOUND
 * when it does not acknowledge), initializes the controller, clears the frame
 * and turns the output on. present() accepts only the whole frame
 * (XIAOTAI_ERR_INVALID_ARG otherwise). set_on() switches the panel output. */
xiaotai_err_t xiaotai_panel_ssd1306_create(const xiaotai_panel_ssd1306_config_t *config, xiaotai_panel_t *panel);

#ifdef __cplusplus
}
#endif
