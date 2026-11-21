// SPDX-License-Identifier: MIT
#pragma once

#include "mono_canvas.h"

enum {
    MONO_WIDTH = XIAOTAI_THEME_MONO128_WIDTH,
    MONO_HEIGHT = XIAOTAI_THEME_MONO128_HEIGHT,
    MONO_ROWS = XIAOTAI_THEME_MONO128_ROWS,
    MONO_LIST_Y = 17,
    MONO_ROW_HEIGHT = 16,
    MONO_ROW_X = 4,
    MONO_TEXT_WIDTH = 118,
    MONO_HINT_Y = 51,
    MONO_TOAST_Y = 46,
};

typedef struct {
    uint8_t x, y, width, height, layer, element, index;
} mono_control_t;

typedef struct {
#if !XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    uint8_t frame[XIAOTAI_THEME_MONO128_FRAME_BYTES];
#endif
    mono_canvas_t canvas;
    const xiaotai_ui_host_t *host;
    mono_control_t controls[MONO_ROWS + 3];
    uint32_t tick_at;
    uint8_t control_count;
    bool scrolling, animated;
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    uint8_t frame[];
#endif
} mono_state_t;

typedef struct {
    uint8_t y; /* 0 when body already occupies the hint area */
    bool primary_only;
} mono_hint_t;

void mono_header(const mono_canvas_t *canvas, const char *title, const char *right);
void mono_remember_control(mono_state_t *state, uint8_t layer, uint8_t element,
                           uint8_t index, uint8_t x, uint8_t y, uint8_t width, uint8_t height);
mono_hint_t mono_render_page(mono_state_t *state, const xiaotai_ui_layer_t *layer, uint8_t layer_id,
                             const xiaotai_ui_clock_t *clock, uint32_t tick, bool toast);
