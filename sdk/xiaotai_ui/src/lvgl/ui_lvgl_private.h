// SPDX-License-Identifier: MIT
#pragma once

#include "lvgl.h"
#include "ui_internal.h"

/* A port opens/closes the LVGL display while the shared LVGL lock is held.
 * It flushes its native display; a diagnostic capture intercepts the flush
 * into logical RGB565LE before the port's conversion and panel I/O. */
xiaotai_err_t ui_lvgl_port_open(ui_display_t *display, lv_display_t **root);
void ui_lvgl_port_close(ui_display_t *display, lv_display_t *root);
/* The port can defer a refresh while an earlier buffer remains on the panel. */
bool ui_lvgl_port_ready(lv_display_t *root);
bool ui_lvgl_capture_flush(ui_display_t *display, const lv_area_t *area,
                          const uint8_t *pixels, size_t stride);
