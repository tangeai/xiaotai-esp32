// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: panels on a parallel RGB bus (HSYNC, VSYNC, DE, PCLK and up
 * to 16 data lines) that need no command interface, as a panel driver for the
 * display part (xiaotai/parts/display.h).
 *
 * The panel owns its frame buffers and scans them out (XIAOTAI_RENDER_SCANOUT,
 * scanout kind XIAOTAI_SCANOUT_RGB): start() reports them in the handles,
 * present() of one of them switches the scan-out to it, and present() of any
 * other source copies into the buffer being shown. Frame buffers are
 * XIAOTAI_MEM_BULK (external RAM where the board has it) and exist while the
 * panel is started. The pixel format is RGB565; data line i carries bit i of
 * the pixel as the renderer stores it.
 *
 * Calling tasks: create is [board]; the driver functions are called by the
 * display part only. */
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/parts/display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_PANEL_RGB_DATA_LINES 16

typedef struct {
    uint32_t pixel_clock_hz;
    uint16_t width, height;
    uint16_t hsync_pulse, hsync_back_porch, hsync_front_porch;
    uint16_t vsync_pulse, vsync_back_porch, vsync_front_porch;
    /* Signal polarities in the panel's datasheet terms. */
    bool hsync_idle_low, vsync_idle_low, de_idle_high;
    bool pclk_active_falling;   /* data is clocked out on the falling PCLK edge */
    bool pclk_idle_high;
    int hsync_pin, vsync_pin, de_pin, pclk_pin;
    int disp_pin;               /* < 0: no display-enable line */
    bool disp_active_low;
    uint8_t data_lines;         /* 16 (RGB565) */
    int data_pins[XIAOTAI_PANEL_RGB_DATA_LINES];
    uint8_t buffers;            /* 1 or 2 frame buffers */
    /* DMA burst in bytes (0: the platform default). */
    uint16_t dma_burst_bytes;
    /* zeroed: no reset line of its own; active: in reset. */
    xiaotai_line_t reset;
    uint16_t reset_hold_ms, reset_settle_ms;
} xiaotai_panel_rgb_config_t;

/* [board] Touches no hardware. A start() that fails releases what it took. */
xiaotai_err_t xiaotai_panel_rgb_create(const xiaotai_panel_rgb_config_t *config, xiaotai_panel_t *panel);

#ifdef __cplusplus
}
#endif
