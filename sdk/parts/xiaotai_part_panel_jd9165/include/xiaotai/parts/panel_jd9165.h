// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: JD9165-class panels on MIPI DSI in video (DPI) mode, as a
 * panel driver for the display part (xiaotai/parts/display.h).
 *
 * The panel owns its frame buffers and scans them out (XIAOTAI_RENDER_SCANOUT):
 * start() reports them in the handles, present() of one of them switches the
 * scan-out to it, and present() of any other source copies into the buffer
 * being shown. Frame buffers are XIAOTAI_MEM_BULK and exist while the panel is
 * started.
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
    uint8_t lanes;              /* DSI data lanes */
    uint32_t lane_mbps;         /* bit rate per lane */
    /* Supply of the DSI PHY from an on-chip regulator: channel and millivolts;
     * channel < 0: the PHY is powered by the board. */
    int phy_ldo_channel;
    uint16_t phy_ldo_mv;
    uint32_t pixel_clock_hz;
    uint16_t width, height;
    uint16_t hsync_back_porch, hsync_pulse, hsync_front_porch;
    uint16_t vsync_back_porch, vsync_pulse, vsync_front_porch;
    uint8_t buffers;            /* 1 or 2 frame buffers */
    /* zeroed: the reset is shared with another chip that owns it (the display
     * part then starts that chip first, see touch_first); active: in reset. */
    xiaotai_line_t reset;
    uint16_t reset_hold_ms, reset_settle_ms;
} xiaotai_panel_jd9165_config_t;

/* [board] Touches no hardware. The pixel format is RGB565. A start() that fails
 * releases panel, panel IO, DSI bus and PHY supply in that order. */
xiaotai_err_t xiaotai_panel_jd9165_create(const xiaotai_panel_jd9165_config_t *config, xiaotai_panel_t *panel);

/* [board] Makes the DSI PHY regulator a separately sequenced display power
 * line. Set the panel's own phy_ldo_channel to -1 in that case. The display
 * owner serializes operations on the line; creation leaves the PHY off. */
xiaotai_err_t xiaotai_panel_jd9165_phy_line(int channel, uint16_t millivolts, xiaotai_line_t *line);

#ifdef __cplusplus
}
#endif
