// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/ui_theme.h"

enum { LCD_FACE_WIDTH = 220, LCD_FACE_HEIGHT = 108, LCD_FACE_REGIONS = 3, LCD_FACE_FRAME_MS = 40 };

typedef struct { int16_t x, y, width, height; } lcd_face_rect_t;
typedef struct lcd_face lcd_face_t;

size_t lcd_face_resolve(const char *name);
bool lcd_face_has_alternate(size_t emotion);

xiaotai_err_t lcd_face_create(uint32_t background_rgb, lcd_face_t **out);
void lcd_face_destroy(lcd_face_t *face);
void lcd_face_attach(lcd_face_t *face, uint64_t now_ms);
void lcd_face_detach(lcd_face_t *face);
void lcd_face_set_background(lcd_face_t *face, uint32_t background_rgb);
void lcd_face_set(lcd_face_t *face, const char *name, uint8_t phase, uint8_t level, uint64_t now_ms);

/* One published frame at 25 fps while visible. Visibility pauses rendering,
 * not absolute motion/cue time. Resume uses one 40 ms activity step and no
 * late-frame penalty. Damage includes the previous and the current shapes. */
size_t lcd_face_step(lcd_face_t *face, uint64_t now_ms, bool running, lcd_face_rect_t regions[LCD_FACE_REGIONS]);
uint32_t lcd_face_delay(const lcd_face_t *face, uint64_t now_ms);
bool lcd_face_paint(void *face, const xiaotai_ui_surface_t *target, int16_t x, int16_t y);
bool lcd_face_canvas(lcd_face_t *face, const xiaotai_ui_surface_t *target);
void lcd_face_report(const lcd_face_t *face, char *line, size_t capacity);
