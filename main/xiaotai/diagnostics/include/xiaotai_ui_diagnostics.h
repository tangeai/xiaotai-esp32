#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool ready;
    bool awake;
    unsigned width, height, page, objects;
    uint32_t ticks;
    uint32_t capture_stack_min_free;
} xiaotai_ui_snapshot_t;

typedef struct {
    uint8_t *pixels;
    unsigned width, height;
    size_t size;
} xiaotai_ui_frame_t;

/* UI owns LVGL access. No borrowed LVGL objects escape these calls. */
esp_err_t xiaotai_ui_inspect(xiaotai_ui_snapshot_t *out);
/* Independent RGB565 little-endian render, NOT physical LCD readback. Caller
 * owns pixels and must free them. Queued to the UI owner, with a bounded wait;
 * late completion owns its own storage. Returns busy in real-time sessions. */
esp_err_t xiaotai_ui_capture(xiaotai_ui_frame_t *out);
