// SPDX-License-Identifier: MIT
#pragma once

#include "lcd_face_life.h"
#include "xiaotai/ui_theme.h"

/* The face is designed on a canvas of this size and drawn in the pixels of
 * the display: lcd_face_size() says how many there are. */
enum { LCD_FACE_WIDTH = 240, LCD_FACE_HEIGHT = 124, LCD_FACE_REGIONS = 6, LCD_FACE_FRAME_MS = 20 };

typedef struct { int16_t x, y, width, height; } lcd_face_rect_t;
typedef struct lcd_face lcd_face_t;

size_t lcd_face_resolve(const char *name);
/* How an expression looks and carries itself: its place in the catalogue, or
 * one of the states a page shows (ringing, dialing, offline, error). */
enum { LCD_FACE_RINGING = 23, LCD_FACE_DIALING, LCD_FACE_OFFLINE, LCD_FACE_ERROR, LCD_FACE_MANNERS };
size_t lcd_face_manner(const char *name);

xiaotai_err_t lcd_face_create(uint32_t background_rgb, lcd_face_t **out);
void lcd_face_destroy(lcd_face_t *face);
/* The canvas in pixels; LCD_FACE_WIDTH x LCD_FACE_HEIGHT until set. What is
 * drawn keeps its proportions and fits. */
xiaotai_err_t lcd_face_size(lcd_face_t *face, uint16_t width, uint16_t height);
void lcd_face_attach(lcd_face_t *face, uint64_t now_ms);
void lcd_face_detach(lcd_face_t *face);
void lcd_face_set_background(lcd_face_t *face, uint32_t background_rgb);
void lcd_face_set(lcd_face_t *face, const char *name, uint8_t phase, uint8_t level, uint64_t now_ms);

/* Milliseconds between frames; LCD_FACE_FRAME_MS unless set. */
void lcd_face_pace(lcd_face_t *face, uint16_t frame_ms);
/* What it senses now, on the canvas as designed; kept for the next frames. */
void lcd_face_sense(lcd_face_t *face, const lcd_face_senses_t *senses);
/* Whether the touch that just ended, or goes on, strokes it. */
bool lcd_face_stroked(const lcd_face_t *face);

/* One frame, while visible and when one is due. Visibility pauses drawing,
 * not time. The regions, in pixels of the canvas, are what differs from the
 * frame before: only parts that moved by an eighth of a pixel or more. */
size_t lcd_face_step(lcd_face_t *face, uint64_t now_ms, bool running, lcd_face_rect_t regions[LCD_FACE_REGIONS]);
uint32_t lcd_face_delay(const lcd_face_t *face, uint64_t now_ms);
bool lcd_face_paint(void *face, const xiaotai_ui_surface_t *target, int16_t x, int16_t y);
bool lcd_face_canvas(lcd_face_t *face, const xiaotai_ui_surface_t *target);
/* What the frames so far took, in microseconds: working them out and drawing them. */
uint32_t lcd_face_cost(const lcd_face_t *face);
void lcd_face_report(const lcd_face_t *face, char *line, size_t capacity);
