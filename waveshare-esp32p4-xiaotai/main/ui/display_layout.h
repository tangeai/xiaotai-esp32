#pragma once

#include "lvgl.h"

/*
 * P4 renders a native 480x320 landscape UI. Legacy helpers are retained only
 * for inherited widget-local coordinate contracts; page composition can use
 * the native helpers directly.
 */
#define DISPLAY_NATIVE_WIDTH         480
#define DISPLAY_NATIVE_HEIGHT        320
#define DISPLAY_LEGACY_DESIGN_WIDTH  320
#define DISPLAY_LEGACY_DESIGN_HEIGHT 240

#define DISPLAY_DESIGN_WIDTH  DISPLAY_LEGACY_DESIGN_WIDTH
#define DISPLAY_DESIGN_HEIGHT DISPLAY_LEGACY_DESIGN_HEIGHT

lv_coord_t display_scale_x(lv_coord_t value);
lv_coord_t display_scale_y(lv_coord_t value);
lv_coord_t display_scale_square(lv_coord_t value);
void display_obj_set_design_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y);
void display_obj_set_design_size(lv_obj_t *obj, lv_coord_t width, lv_coord_t height);

lv_coord_t display_native_scale_x(lv_coord_t value);
lv_coord_t display_native_scale_y(lv_coord_t value);
lv_coord_t display_native_scale_square(lv_coord_t value);
void display_obj_set_native_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y);
void display_obj_set_native_size(lv_obj_t *obj, lv_coord_t width, lv_coord_t height);
