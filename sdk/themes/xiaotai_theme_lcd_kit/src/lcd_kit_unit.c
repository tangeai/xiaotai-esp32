// SPDX-License-Identifier: MIT
/* Lifecycle unit "theme_lcd_kit". The kit is a library for the colour themes:
 * it registers no theme (only themes with a display rule do, so the UI system
 * never matches a display against the kit) and keeps no state of its own
 * (every page, style and face buffer belongs to the state a theme's start
 * creates). Init must not allocate: it runs on every colour product,
 * including before the UI system has a display. */
#include "xiaotai/theme_lcd.h"

xiaotai_err_t xiaotai_engine_theme_lcd_kit_init(void)
{
    return XIAOTAI_OK;
}
