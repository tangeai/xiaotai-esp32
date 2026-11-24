// SPDX-License-Identifier: MIT
#pragma once
/* Colour theme for landscape RGB565 displays larger than 320x240, drawn with
 * LVGL. It is the colour theme kit (xiaotai/theme_lcd.h, component
 * xiaotai_theme_lcd_kit; the manifest depends on theme_lcd_kit for it, which
 * also brings the LVGL configuration, and not on theme_lcd320, so no 320x240
 * theme is built beside it): pages are laid out on the same
 * logical 320x240 canvas and scaled to the display (horizontally by
 * width / 320, vertically by height / 240). It adds a full-screen background behind HOME and CLOCK,
 * the clock on HOME and a video region for views with XIAOTAI_VIEW_VIDEO.
 * Custom templates receive their container in native display pixels and are
 * not scaled.
 *
 * Match: 100 for RGB565 480x320; 60 for any other landscape RGB565 display of
 * at least 480x320; 0 otherwise, and 0 for a display with
 * XIAOTAI_DISPLAY_SLOW_REFRESH (animated face, video region, clock).
 *
 * Emotions: those of the kit (xiaotai/theme_lcd.h). */
#include "xiaotai/port/err.h"
#include "xiaotai/ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_THEME_LCD480_NAME "lcd480"

extern const xiaotai_theme_t xiaotai_theme_lcd480;

/* Lifecycle unit "theme_lcd480" (theme.json): registers the theme with the UI
 * system. */
xiaotai_err_t xiaotai_engine_theme_lcd480_init(void);

#ifdef __cplusplus
}
#endif
