// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/theme_mono128.h"

typedef struct {
    uint8_t *frame;
    xiaotai_pixel_t pixel;
} mono_canvas_t;

typedef enum {
    MONO_CLEAR,
    MONO_SET,
    MONO_INVERT,
} mono_ink_t;

void mono_rect(const mono_canvas_t *canvas, int x, int y, int width,
               int height, mono_ink_t ink);
const uint8_t *mono_glyph5(char character);
void mono_text5(const mono_canvas_t *canvas, int x, int y, const char *text, unsigned scale);
void mono_center5(const mono_canvas_t *canvas, int y, const char *text, unsigned scale);
unsigned mono_utf8_width(const char *text);
unsigned mono_window(const mono_canvas_t *canvas, int x, int y, const char *text,
                     unsigned width, uint32_t tick);
/* Draw one newline-delimited line, returning the next line and scroll need. */
const char *mono_center_line(const mono_canvas_t *canvas, int y, const char *text,
                             uint32_t tick, bool *scrolling);
void mono_center(const mono_canvas_t *canvas, int y, const char *text);
bool mono_image(const mono_canvas_t *canvas, int y, int height, const xiaotai_ui_image_t *image);
