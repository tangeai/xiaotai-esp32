// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DOOR_BACKDROP_DOOR,   /* door station: the front door at night */
    DOOR_BACKDROP_ROOM,   /* indoor station: a room, window at dusk */
} door_backdrop_t;

/* 16-bit values of working memory door_backdrop_draw() needs for a width. */
#define DOOR_BACKDROP_WORK(width) (4U * (size_t)(width))

/* Draws the picture as native RGB565, width * height values, row by row.
 * Pure: the same arguments give the same pixels. False for a size it does not
 * draw (wider than 1280) or a missing buffer. */
bool door_backdrop_draw(uint16_t *pixels, uint16_t width, uint16_t height, door_backdrop_t kind, uint16_t *work);
/* The same rows [from, from + rows) of it alone, into pixels. */
bool door_backdrop_rows(uint16_t *pixels, uint16_t width, uint16_t height, door_backdrop_t kind, uint16_t *work,
                        uint16_t from, uint16_t rows);
