// SPDX-License-Identifier: MIT
#pragma once

#include "lvgl.h"

typedef struct { uint16_t width, height; } lcd_layout_t;
typedef struct { int32_t x, y, width, height; } lcd_box_t;

lcd_box_t lcd_scale(const lcd_layout_t *layout, lcd_box_t logical);
lcd_box_t lcd_face_box(const lcd_layout_t *layout, int32_t width, int32_t height, bool preview);
void lcd_place(lv_obj_t *obj, lcd_box_t physical);
