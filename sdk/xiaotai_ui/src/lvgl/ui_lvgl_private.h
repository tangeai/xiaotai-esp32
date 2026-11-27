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
/* Returns once the panel reads none of the port's buffers: after a refresh,
 * before any other call on the display part and before its buffers change
 * hands. A port that sends strips from a task of its own may still be sending
 * the last one when the refresh returns. */
void ui_lvgl_port_settle(lv_display_t *root);
/* LVGL was shut down after its last display closed: whatever the port had
 * registered with it (a draw unit) is gone. */
void ui_lvgl_port_reset(void);
/* What the port's own drawing did so far, one line; "" when it has nothing to
 * say. Callable from any task. */
void ui_lvgl_port_report(char *words, size_t capacity);
/* Turns the port's hardware drawing on or off (it starts on where the chip
 * and the display suit it). XIAOTAI_ERR_NOT_SUPPORTED when there is none. */
xiaotai_err_t ui_lvgl_port_accelerate(bool on);
/* Sends a block of finished pixels (RGB565, packed, in the panel's byte order)
 * straight to the panel and waits until the memory is free again. */
bool ui_lvgl_port_push(ui_display_t *display, const lv_area_t *area, uint8_t *pixels);
bool ui_lvgl_capture_flush(ui_display_t *display, const lv_area_t *area,
                          const uint8_t *pixels, size_t stride);
