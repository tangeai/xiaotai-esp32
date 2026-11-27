// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/parts/panel_rgb.h"

/* The native panel owns its scan-out buffers. The portable driver controls
 * the order; each acquisition publishes only on success. */
typedef struct {
    xiaotai_err_t (*panel_open)(const xiaotai_panel_rgb_config_t *config, void **panel);
    xiaotai_err_t (*panel_close)(void *panel);
    /* Resets the controller timing engine and starts scanning out. */
    xiaotai_err_t (*panel_enable)(void *panel);
    xiaotai_err_t (*framebuffers)(void *panel, uint8_t buffers, void *out[2]);
    xiaotai_err_t (*present)(void *panel, const xiaotai_rect_t *area, const void *pixels);
    void (*native_handles)(void *panel, xiaotai_display_handles_t *handles);
} rgb_io_t;

extern const rgb_io_t xiaotai_rgb_io;
