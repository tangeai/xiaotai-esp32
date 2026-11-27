// SPDX-License-Identifier: MIT
#include "lcd_internal.h"
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

static const lcd_touch_t touch_design = {
    .row = 44, .two_lines = 48, .form_row = 48, .foot = 38, .tab = 30, .dialog_row = 40, .header = 40, .circle = 48,
    .home_phone = 46, .home_phone_y = 136, .pager_w = 36, .pager_h = 52, .dialog_w = 272, .dialog_max = 224,
};
static const lcd_touch_t touch_small = {
    .row = 52, .two_lines = 58, .form_row = 56, .foot = 46, .tab = 38, .dialog_row = 50, .header = 52, .circle = 60,
    .home_phone = 56, .home_phone_y = 178, .pager_w = 44, .pager_h = 84, .dialog_w = 300, .dialog_max = 232,
};

const lcd_touch_t *lcd_touch(const lcd_state_t *s)
{
    const unsigned shorter = s->layout.width < s->layout.height ? s->layout.width : s->layout.height;
    return shorter <= XIAOTAI_THEME_LCD_LOGICAL_HEIGHT ? &touch_small : &touch_design;
}

void lcd_place(lv_obj_t *obj, lcd_box_t box)
{
    lv_obj_set_pos(obj, box.x, box.y);
    lv_obj_set_size(obj, box.width, box.height);
}
