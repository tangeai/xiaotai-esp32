// SPDX-License-Identifier: MIT
#pragma once

#include "ui_internal.h"

/* Called with the LVGL backend lock held. */
xiaotai_err_t ui_lvgl_draw_start(void);
void ui_lvgl_draw_reset(void);
xiaotai_err_t ui_lvgl_draw_init(ui_display_t *display);
void ui_lvgl_draw_drop(ui_display_t *display);
void ui_lvgl_draw_capture(ui_display_t *display, bool active);
