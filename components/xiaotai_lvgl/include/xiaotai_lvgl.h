#pragma once
#include "lvgl.h"
#include "esp_err.h"
#include "src/draw/lv_draw_private.h"
#include "src/display/lv_display_private.h"
#include "src/misc/lv_area_private.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef bool (*xiaotai_image_draw_t)(lv_draw_task_t *task,
                                    const lv_draw_image_dsc_t *image, const lv_area_t *area);
/* Synchronous hook on LVGL's owner task; false delegates the same task to LVGL. */
esp_err_t xiaotai_lvgl_image_hook(xiaotai_image_draw_t draw);
typedef struct {
    uint16_t *buf;
    const lv_area_t *buf_area;
    const lv_area_t *clip_area;
    uint32_t stride;
} xiaotai_rgb565_surface_t;
bool xiaotai_lvgl_rgb565_surface(lv_draw_task_t *task, xiaotai_rgb565_surface_t *surface);
#ifdef __cplusplus
}
#endif
