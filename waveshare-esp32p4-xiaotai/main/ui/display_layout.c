#include "display_layout.h"

#include <stdint.h>

#include "display_driver.h"

static lv_coord_t display_scale_axis(lv_coord_t value, lv_coord_t runtime, lv_coord_t design)
{
    int32_t scaled = (int32_t)value * (int32_t)runtime;
    int32_t half = (int32_t)design / 2;

    scaled += scaled >= 0 ? half : -half;
    return (lv_coord_t)(scaled / (int32_t)design);
}

lv_coord_t display_scale_x(lv_coord_t value)
{
    return display_scale_axis(value, DISPLAY_DRIVER_WIDTH, DISPLAY_DESIGN_WIDTH);
}

lv_coord_t display_scale_y(lv_coord_t value)
{
    return display_scale_axis(value, DISPLAY_DRIVER_HEIGHT, DISPLAY_DESIGN_HEIGHT);
}

lv_coord_t display_scale_square(lv_coord_t value)
{
    lv_coord_t scaled_x = display_scale_x(value);
    lv_coord_t scaled_y = display_scale_y(value);

    return scaled_x < scaled_y ? scaled_x : scaled_y;
}

void display_obj_set_design_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y)
{
    lv_obj_set_pos(obj, display_scale_x(x), display_scale_y(y));
}

void display_obj_set_design_size(lv_obj_t *obj, lv_coord_t width, lv_coord_t height)
{
    lv_obj_set_size(obj, display_scale_x(width), display_scale_y(height));
}

lv_coord_t display_native_scale_x(lv_coord_t value)
{
    return display_scale_axis(value, DISPLAY_DRIVER_WIDTH, DISPLAY_NATIVE_WIDTH);
}

lv_coord_t display_native_scale_y(lv_coord_t value)
{
    return display_scale_axis(value, DISPLAY_DRIVER_HEIGHT, DISPLAY_NATIVE_HEIGHT);
}

lv_coord_t display_native_scale_square(lv_coord_t value)
{
    lv_coord_t scaled_x = display_native_scale_x(value);
    lv_coord_t scaled_y = display_native_scale_y(value);

    return scaled_x < scaled_y ? scaled_x : scaled_y;
}

void display_obj_set_native_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y)
{
    lv_obj_set_pos(obj, display_native_scale_x(x), display_native_scale_y(y));
}

void display_obj_set_native_size(lv_obj_t *obj, lv_coord_t width, lv_coord_t height)
{
    lv_obj_set_size(obj, display_native_scale_x(width), display_native_scale_y(height));
}
