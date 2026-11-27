// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/ui_theme.h"

/* Antialiased shapes in the pixels of the display: edges in 256 steps, places
 * in fractions of a pixel, colours in 24 bits until a row is finished and then
 * dithered to the display's 16. Nothing is scaled, nothing is stored as a
 * picture. A row is composed at a time, so a frame may be drawn in strips. */

typedef struct { int16_t x0, y0, x1, y1; } ink_box_t;   /* inclusive; empty while x1 < x0 */

enum { INK_SLICES = 4, INK_SOLID = 256 };

/* One column of a shape: where its upright slices begin and end, in sixteenths of a row. */
typedef struct {
    int16_t top[INK_SLICES], bottom[INK_SLICES];
    int16_t first, last;        /* rows any slice touches */
    int16_t full0, full1;       /* rows every slice covers */
    int16_t lid;                /* where the shadow of the lid begins, in sixteenths of a row */
} ink_column_t;

typedef struct {
    ink_column_t *column;
    int16_t x0, count;
    ink_box_t box;
    bool whole;                 /* a row of it is one piece: no need to look at the middle */
    int16_t lid;                /* the lowest of the columns' */
} ink_shape_t;

typedef enum { INK_BAR, INK_HEART, INK_SPARK, INK_DROP } ink_glyph_t;

/* Pixels; y grows downwards. Words only, so that two forms compare as memory. */
typedef struct {
    int32_t glyph;              /* ink_glyph_t */
    float cx, cy, rx, ry;
    float round;                /* radius of the corners */
    float soft;                 /* 0: straight sides between round corners, 1: one smooth curve */
    float shear;                /* how much lower the right end is */
    float bend;                 /* above 0: the ends hang, below 0: they rise */
    float wave;                 /* a wavy line */
    float flat;                 /* 0 .. 1: the upper half is that much flatter */
    int32_t shade;              /* ink_lid will be asked for */
    /* Lids: no higher than `upper`, no lower than `lower`. `tilt` is for the right end, `arch` for the middle. */
    int32_t lids;
    float upper, upper_tilt, upper_arch;
    float lower, lower_tilt, lower_arch;
    float ease;                 /* how far from a corner between a lid and the shape it is rounded */
    /* Closing: the shape shrinks towards the row `line`, to `open` of its height and no less than `least`. */
    float line, open, least, sag;
} ink_form_t;

/* Builds the columns; `store` has room for `room` of them. */
void ink_form(ink_shape_t *shape, const ink_form_t *form, ink_column_t *store, size_t room, int16_t width, int16_t height);

typedef struct {
    const xiaotai_ui_surface_t *surface;
    int32_t left, top;          /* canvas position of the surface's first pixel */
    ink_box_t clip;             /* canvas pixels this paint covers */
    uint32_t background;
    uint16_t paper;             /* background as the display has it */
    uint32_t *line;             /* one row, from clip.x0 */
    uint16_t *mask;             /* coverage of one shape on this row, from clip.x0 */
    const ink_shape_t *masked;  /* whose it is */
    int16_t from, to;           /* where it covers anything */
    int16_t full0, full1;       /* where it covers everything */
    int16_t y;
} ink_t;

void ink_row(ink_t *ink, int16_t y);            /* begins a row */
void ink_row_end(ink_t *ink);                   /* writes it */

/* Each adds to the current row. `inside` clips to a shape; NULL does not. */
void ink_shape(ink_t *ink, const ink_shape_t *shape, uint32_t top, uint32_t bottom, unsigned alpha, const ink_shape_t *inside);
/* The shadow a lid throws on what it covers: from the shape's upper edge down. */
void ink_lid(ink_t *ink, const ink_shape_t *shape, uint32_t rgb, float depth, unsigned alpha);
void ink_disc(ink_t *ink, float cx, float cy, float rx, float ry, uint32_t rgb, unsigned alpha, const ink_shape_t *inside);
/* Full in the middle, nothing at the rim. */
void ink_soft(ink_t *ink, float cx, float cy, float rx, float ry, uint32_t rgb, unsigned alpha, const ink_shape_t *inside);
void ink_stroke(ink_t *ink, float ax, float ay, float bx, float by, float radius, uint32_t rgb, unsigned alpha,
                const ink_shape_t *inside);

/* An iris: colours over the squared distance from its middle, from its upper to its lower rim. */
enum { INK_RAMP = 48, INK_TIERS = 12 };
void ink_iris(ink_t *ink, float cx, float cy, float rx, float ry, const uint32_t ramp[INK_TIERS][INK_RAMP], unsigned alpha,
              const ink_shape_t *inside);

uint32_t ink_mix(uint32_t from, uint32_t to, unsigned amount);      /* amount 0 .. 256 */
static inline bool ink_empty(const ink_box_t *b) { return b->x1 < b->x0 || b->y1 < b->y0; }
ink_box_t ink_join(ink_box_t a, ink_box_t b);
