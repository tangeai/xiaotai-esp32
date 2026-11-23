// SPDX-License-Identifier: MIT
#include "ui_test.h"
#include "xiaotai/theme_lcd320.h"

int main(void)
{
    CHECK(xiaotai_theme_lcd320.backend == XIAOTAI_UI_BACKEND_LVGL);
    CHECK(xiaotai_theme_lcd320.stack_bytes == XIAOTAI_THEME_LCD_STACK_BYTES);
    xiaotai_display_desc_t desc = {.width = 320, .height = 240, .pixel = XIAOTAI_PIXEL_RGB565};
    CHECK(xiaotai_theme_lcd320.match(&desc) == 100);
    desc.flags = XIAOTAI_DISPLAY_SLOW_REFRESH;
    CHECK(xiaotai_theme_lcd320.match(&desc) == 0);
    desc.flags = 0;
    desc.pixel = XIAOTAI_PIXEL_RGB888;
    CHECK(xiaotai_theme_lcd320.match(&desc) == 0);
    desc.pixel = XIAOTAI_PIXEL_RGB565;
    desc.width = 480;
    desc.height = 320;
    CHECK(xiaotai_theme_lcd320.match(&desc) == 0);
    return 0;
}
