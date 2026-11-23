// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* [any] Host LVGL backend only: the picture a display shows now, as packed
 * RGB888 rows of width * height pixels. It is kept up to date on every flush,
 * also while a session runs, so preview tools can save any scene. The buffer
 * belongs to the backend and changes with the next frame. false: no frame
 * yet, or no such display. */
bool xiaotai_ui_host_preview(uint8_t display, uint16_t *width, uint16_t *height, const uint8_t **rgb888);

#ifdef __cplusplus
}
#endif
