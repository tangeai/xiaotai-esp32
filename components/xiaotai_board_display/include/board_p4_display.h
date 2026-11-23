#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lvgl_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t physical_width, physical_height;
    lv_display_rotation_t landscape_rotation, portrait_rotation;
    uint8_t buffer_count;
    const char *transfer;
} xiaotai_p4_display_spec_t;

const xiaotai_p4_display_spec_t *xiaotai_p4_display_spec(void);

// Stable layout bounds; safe to query without touching LVGL state.
static inline uint16_t xiaotai_p4_display_landscape_extent(bool horizontal)
{
    const xiaotai_p4_display_spec_t *spec = xiaotai_p4_display_spec();
    bool rotated = spec->landscape_rotation == LV_DISPLAY_ROTATION_90 ||
                   spec->landscape_rotation == LV_DISPLAY_ROTATION_270;
    return horizontal != rotated ? spec->physical_width : spec->physical_height;
}

// The board registers its panel and touch device in the LVGL port. The caller
// supplies task settings and borrows both objects for the life of the display.
esp_err_t xiaotai_p4_display_open(const lvgl_port_cfg_t *port,
                                 lv_display_t **display, lv_indev_t **touch);

// Caller owns the LVGL lock, validates the rectangle and syncs its RGB565
// source, which must not be a DSI scanout buffer. Success releases the source.
esp_err_t xiaotai_p4_display_present(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                                    const uint16_t *pixels);

#ifdef __cplusplus
}
#endif
