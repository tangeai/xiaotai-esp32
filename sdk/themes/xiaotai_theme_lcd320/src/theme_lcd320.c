// SPDX-License-Identifier: MIT
#include "xiaotai/theme_lcd320.h"

static int match(const xiaotai_display_desc_t *display)
{
    return display && display->width == XIAOTAI_THEME_LCD_LOGICAL_WIDTH &&
           display->height == XIAOTAI_THEME_LCD_LOGICAL_HEIGHT && display->pixel == XIAOTAI_PIXEL_RGB565 &&
           !(display->flags & XIAOTAI_DISPLAY_SLOW_REFRESH) ? 100 : 0;
}

static xiaotai_err_t start(const xiaotai_ui_host_t *host, void **state)
{
    static const xiaotai_theme_lcd_variant_t variant = {
        .features = XIAOTAI_THEME_LCD_EMOTION | XIAOTAI_THEME_LCD_DIRECT_EMOTION};
    return xiaotai_theme_lcd_start(host, &variant, state);
}

const xiaotai_theme_t xiaotai_theme_lcd320 = {
    .name = XIAOTAI_THEME_LCD320_NAME, .backend = XIAOTAI_UI_BACKEND_LVGL, .match = match, .start = start,
    .render = xiaotai_theme_lcd_render, .tick = xiaotai_theme_lcd_tick, .sleep = xiaotai_theme_lcd_sleep,
    .controls = xiaotai_theme_lcd_controls, .stop = xiaotai_theme_lcd_stop, .report = xiaotai_theme_lcd_report,
    .stack_bytes = XIAOTAI_THEME_LCD_STACK_BYTES,
};

xiaotai_err_t xiaotai_engine_theme_lcd320_init(void)
{
    return xiaotai_ui_theme_register(&xiaotai_theme_lcd320);
}
