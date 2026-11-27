// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/parts/panel_jd9165.h"

/* The native panel owns its scan-out buffers. The portable driver controls
 * the resource order; each acquisition publishes only on success. */
typedef struct {
    xiaotai_err_t (*phy_open)(const xiaotai_panel_jd9165_config_t *config, void **phy);
    xiaotai_err_t (*phy_close)(void *phy);
    xiaotai_err_t (*bus_open)(const xiaotai_panel_jd9165_config_t *config, void **bus);
    xiaotai_err_t (*bus_close)(void *bus);
    xiaotai_err_t (*io_open)(void *bus, void **io);
    xiaotai_err_t (*io_close)(void *io);
    xiaotai_err_t (*panel_open)(const xiaotai_panel_jd9165_config_t *config, void *bus, void *io, void **panel);
    xiaotai_err_t (*panel_close)(void *panel);
    xiaotai_err_t (*panel_enable)(void *panel, bool soft_reset);
    xiaotai_err_t (*framebuffers)(void *panel, uint8_t buffers, void *out[2]);
    xiaotai_err_t (*present)(void *panel, const xiaotai_rect_t *area, const void *pixels);
    void (*native_handles)(void *panel, void *io, xiaotai_display_handles_t *handles);
} jd9165_io_t;

extern const jd9165_io_t xiaotai_jd9165_io;
