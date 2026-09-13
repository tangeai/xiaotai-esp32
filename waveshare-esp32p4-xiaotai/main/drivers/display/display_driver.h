#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#define DISPLAY_DRIVER_WIDTH  ((lv_coord_t)display_driver_width())
#define DISPLAY_DRIVER_HEIGHT ((lv_coord_t)display_driver_height())

typedef struct {
    lv_disp_t *display;
    lv_indev_t *touch_indev;
} display_driver_handles_t;

typedef enum {
    DISPLAY_DRIVER_ORIENTATION_PORTRAIT = 0,
    DISPLAY_DRIVER_ORIENTATION_LANDSCAPE,
} display_driver_orientation_t;

esp_err_t display_driver_init(display_driver_handles_t *handles);
bool display_driver_is_initialized(void);
uint16_t display_driver_width(void);
uint16_t display_driver_height(void);
display_driver_orientation_t display_driver_get_orientation(void);

/**
 * Change the panel orientation from the LVGL task.
 *
 * Touch remains in the panel's physical coordinate space and LVGL applies the
 * matching pointer transform. Callers must relayout the visible page after a
 * successful change.
 */
esp_err_t display_driver_set_orientation(display_driver_orientation_t orientation);

/**
 * Push one RGB565 region from DMA-capable PSRAM to the LCD and wait for
 * completion.
 *
 * This is intended for frame-sized media updates from the LVGL task. Normal
 * controls and pages continue to use LVGL. The source must live in aligned,
 * DMA-capable PSRAM and already use the LCD byte order.
 */
esp_err_t display_driver_blit_rgb565(uint16_t x,
                                    uint16_t y,
                                    uint16_t width,
                                    uint16_t height,
                                    const uint16_t *pixels,
                                    uint32_t *elapsed_us);
