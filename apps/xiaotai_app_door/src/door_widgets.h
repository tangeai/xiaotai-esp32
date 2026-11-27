// SPDX-License-Identifier: MIT
#pragma once
/* The plain shapes both template files draw with. Every function takes a
 * missing parent or object: a page short of memory draws less. */
#include <string.h>
#include "lvgl.h"

static inline lv_obj_t *door_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, lv_opa_t opacity)
{
    if (!parent) return NULL;
    lv_obj_t *object = lv_obj_create(parent);
    if (!object) return NULL;
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static inline lv_obj_t *door_label(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font, uint32_t color, int align)
{
    if (!parent) return NULL;
    lv_obj_t *object = lv_label_create(parent);
    if (!object) return NULL;
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, w);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, align, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

static inline void door_text(lv_obj_t *object, const char *value)
{
    if (!value) value = "";
    if (object && strcmp(lv_label_get_text(object), value)) lv_label_set_text(object, value);
}

static inline void door_show(lv_obj_t *object, bool visible)
{
    if (!object) return;
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
