// SPDX-License-Identifier: MIT
#include "lcd_layout.h"
#include "xiaotai/theme_lcd.h"

lcd_box_t lcd_scale(const lcd_layout_t *layout, lcd_box_t logical)
{
    const int32_t w = layout->width, h = layout->height;
    return (lcd_box_t){logical.x * w / (int32_t)XIAOTAI_THEME_LCD_LOGICAL_WIDTH,
                       logical.y * h / (int32_t)XIAOTAI_THEME_LCD_LOGICAL_HEIGHT,
                       logical.width * w / (int32_t)XIAOTAI_THEME_LCD_LOGICAL_WIDTH,
                       logical.height * h / (int32_t)XIAOTAI_THEME_LCD_LOGICAL_HEIGHT};
}

lcd_box_t lcd_face_box(const lcd_layout_t *layout, int32_t width, int32_t height, bool preview)
{
    int32_t w = width * layout->height / (int32_t)XIAOTAI_THEME_LCD_LOGICAL_HEIGHT;
    int32_t h = height * layout->height / (int32_t)XIAOTAI_THEME_LCD_LOGICAL_HEIGHT;
    if (w > layout->width) {
        h = h * layout->width / w;
        w = layout->width;
    }
    const int32_t y = preview ? 48 * layout->height / (int32_t)XIAOTAI_THEME_LCD_LOGICAL_HEIGHT : (layout->height - h) / 2;
    return (lcd_box_t){(layout->width - w) / 2, y, w, h};
}

void lcd_place(lv_obj_t *obj, lcd_box_t box)
{
    lv_obj_set_pos(obj, box.x, box.y);
    lv_obj_set_size(obj, box.width, box.height);
}
