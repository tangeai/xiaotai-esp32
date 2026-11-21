// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/parts/panel_ssd1306.h"

/* Controller operations run synchronously. The caller owns each handle
 * returned on success; a failed deletion keeps ownership with the caller. */
xiaotai_err_t xiaotai_ssd1306_probe(const xiaotai_panel_ssd1306_config_t *config, int timeout_ms);
xiaotai_err_t xiaotai_ssd1306_new_io(const xiaotai_panel_ssd1306_config_t *config, int timeout_ms, void **io);
xiaotai_err_t xiaotai_ssd1306_new_panel(void *io, uint8_t height, void **panel);
xiaotai_err_t xiaotai_ssd1306_configure(void *panel, bool mirror_x, bool mirror_y, bool invert);
xiaotai_err_t xiaotai_ssd1306_draw(void *panel, uint8_t width, uint8_t height, const void *pixels);
xiaotai_err_t xiaotai_ssd1306_switch(void *panel, bool on);
void xiaotai_ssd1306_native_handles(void *panel, void *io, xiaotai_display_handles_t *handles);
xiaotai_err_t xiaotai_ssd1306_del_panel(void *panel);
xiaotai_err_t xiaotai_ssd1306_del_io(void *io);
