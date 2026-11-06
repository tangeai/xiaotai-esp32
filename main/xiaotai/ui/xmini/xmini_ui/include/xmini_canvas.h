// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#define XMINI_CANVAS_BYTES 1024

void xmini_canvas_text(uint8_t *frame, int x, int y, const char *text, unsigned scale);
void xmini_canvas_center(uint8_t *frame, int y, const char *text, unsigned scale);
void xmini_canvas_header(uint8_t *frame, const char *text);
unsigned xmini_canvas_utf8_width(const char *text);
void xmini_canvas_utf8(uint8_t *frame, int x, int y, const char *text);
void xmini_canvas_utf8_center(uint8_t *frame, int y, const char *text);
void xmini_canvas_utf8_window(uint8_t *frame, int x, int y, const char *text, unsigned width, uint32_t tick);
