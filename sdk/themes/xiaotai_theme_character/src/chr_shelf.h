// SPDX-License-Identifier: MIT
#pragma once
/* The pictures of a pack that lies on a slow medium (flash, behind the firmware's image).
 *
 * The head of the pack, her base picture and the small patches over it stay in memory. The pictures
 * of the whole figure in a pose are several megabytes together and only one or two of them show at
 * a time: they are read when they are wanted, by a task of their own, into a few places of fixed
 * size, and checked there. Nothing else ever waits for the medium, and nothing is allocated after
 * the start. A picture that is not there yet, or cannot be had, is simply not gone to: she stays in
 * the picture she is in (chr_mind.c).
 *
 * The pack is read and copied, never mapped: on a chip that runs from external RAM a write to flash
 * has to stop the caches and the other core for as long as any of it is mapped, and not otherwise. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chr_raster.h"

/* The partition the firmware runs from, as bytes. Both are called by the shelf's task alone, whose
 * stack is in internal RAM. */
typedef struct {
    uint32_t (*open)(void *ctx);                                    /* its size; 0 when there is none */
    bool (*read)(void *ctx, uint32_t at, void *to, size_t bytes);
    void *ctx;
    uint32_t size;                                                  /* what open returned */
} chr_medium_t;

typedef struct {
    uint32_t at, bytes, resident, places, place_bytes;  /* the pack; what of it stays in memory; the places for poses */
    uint32_t open_ms, loads, load_max_ms, failed, stack_free;
} chr_shelves_info_t;

typedef struct chr_shelves chr_shelves_t;

/* [ui] Looks behind the image for a pack, reads what stays in memory and starts the task that reads
 * the rest. Waits for the first of it, `wait_ms` at most. NULL: no pack that could be used. */
chr_shelves_t *chr_shelves_open(const chr_medium_t *medium, uint32_t wait_ms);
const chr_pack_t *chr_shelves_pack(const chr_shelves_t *s);
void chr_shelves_info(const chr_shelves_t *s, chr_shelves_info_t *out);
/* [ui] Nothing may be drawn from it any more. */
void chr_shelves_close(chr_shelves_t *s);

/* Where an application image ends, from its own headers; 0 when the medium does not begin with one. */
uint32_t chr_image_end(const chr_medium_t *medium);
