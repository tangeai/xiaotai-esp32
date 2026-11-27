// SPDX-License-Identifier: MIT
#pragma once
/* Colour theme for 320x240 landscape displays (RGB565), drawn with LVGL on a
 * logical 320x240 canvas. It implements every generic view kind and shows
 * custom templates in its content area. It is the colour theme kit
 * (xiaotai/theme_lcd.h, component xiaotai_theme_lcd_kit, which this
 * component depends on) without additions; the kit carries the drawing code
 * and the LVGL configuration, this component only the registration. Larger
 * colour themes depend on the kit, not on this component, so a product with
 * a larger display does not build this theme.
 *
 * Match: 100 for RGB565 320x240; 0 for anything else (larger colour displays
 * belong to xiaotai_theme_lcd480, mono displays to xiaotai_theme_mono128),
 * and 0 for a display with XIAOTAI_DISPLAY_SLOW_REFRESH: the face animates
 * and the clock redraws every second, which such a panel cannot follow. The
 * manifest's display rule is the same at build time: RGB565, 320x240.
 *
 * Emotions: those of the kit (xiaotai/theme_lcd.h, included below, with
 * XIAOTAI_THEME_LCD_EMOTION_COUNT and xiaotai_theme_lcd_emotion_name()). */
#include <stddef.h>
#include "xiaotai/port/err.h"
#include "xiaotai/theme_lcd.h"
#include "xiaotai/ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_THEME_LCD320_NAME "lcd320"

/* The theme. Registered by xiaotai_engine_theme_lcd320_init(); exported so a
 * product or a test can register or inspect it directly. */
extern const xiaotai_theme_t xiaotai_theme_lcd320;

/* Lifecycle unit "theme_lcd320" (theme.json): registers the theme with the UI
 * system. */
xiaotai_err_t xiaotai_engine_theme_lcd320_init(void);

#ifdef __cplusplus
}
#endif
