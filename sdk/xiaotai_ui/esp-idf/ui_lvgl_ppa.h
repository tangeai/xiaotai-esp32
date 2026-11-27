// SPDX-License-Identifier: MIT
#pragma once
/* The chip's pixel accelerator (PPA) in front of LVGL's software renderer,
 * for displays that are drawn in place in their own frame buffers. Those are
 * RGB panels today, so this is compiled where the chip has an accelerator and
 * the build an RGB panel; everywhere else the calls are empty and the
 * software renderer does everything, as before. */
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "soc/soc_caps.h"
#include "xiaotai/port/err.h"
#include "xiaotai/ui_config.h"

#if SOC_PPA_SUPPORTED && SOC_LCD_RGB_SUPPORTED && XIAOTAI_UI_RGB_SCANOUT
#define UI_LVGL_PPA 1
#else
#define UI_LVGL_PPA 0
#endif

#if UI_LVGL_PPA
/* [LVGL lock held] From now on the accelerator serves drawing into these frame
 * buffers: the copy that brings the hidden buffer up to date, opaque fills,
 * and opaque RGB565 pictures; what it cannot do stays with the software
 * renderer. XIAOTAI_ERR_NOT_SUPPORTED when the buffers do not suit it or its
 * self-check failed: the display then simply works without it. */
xiaotai_err_t ui_lvgl_ppa_attach(lv_draw_buf_t *buffers, size_t count);
/* [LVGL lock held] Before the buffers go away. */
void ui_lvgl_ppa_detach(lv_draw_buf_t *buffers, size_t count);
/* [LVGL lock held] LVGL was shut down: the draw unit went with it. */
void ui_lvgl_ppa_reset(void);
/* [LVGL lock held] Off: every job goes to the software renderer, the counters stand still.
 * XIAOTAI_ERR_NOT_SUPPORTED when nothing is attached. */
xiaotai_err_t ui_lvgl_ppa_enable(bool on);
/* [any] One line of counters since start; "" when nothing is attached. */
void ui_lvgl_ppa_report(char *words, size_t capacity);
#else
static inline xiaotai_err_t ui_lvgl_ppa_attach(lv_draw_buf_t *buffers, size_t count)
{
    (void)buffers;
    (void)count;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
static inline void ui_lvgl_ppa_detach(lv_draw_buf_t *buffers, size_t count)
{
    (void)buffers;
    (void)count;
}
static inline void ui_lvgl_ppa_reset(void) {}
static inline xiaotai_err_t ui_lvgl_ppa_enable(bool on)
{
    (void)on;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
static inline void ui_lvgl_ppa_report(char *words, size_t capacity)
{
    if (words && capacity) words[0] = '\0';
}
#endif
