// SPDX-License-Identifier: MIT
#pragma once
/* The character's rasterizer: anti-aliased filled shapes blended into an
 * RGB565 frame. Every shape answers one question, "which pixels of this row
 * are inside", with at most two spans; the rasterizer samples four sub-rows,
 * accumulates the coverage and blends. Opaque interiors are written as runs,
 * so the cost follows the pixels touched and not the shape's complexity. No
 * drawing library is involved: the frame is whatever the caller hands over,
 * clipped to the rectangle it may write. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The part of the stage a call may write. `pixels` addresses (x0, y0). */
typedef struct {
    uint8_t *pixels;
    uint32_t stride;
    int16_t x0, y0, x1, y1;   /* inclusive, stage coordinates */
    bool swap;                /* RGB565 words are stored byte-swapped */
    int16_t *scratch;         /* x1 - x0 + 4 entries, all zero between calls */
    /* Past those the scratch may hold an unpacked sprite row of up to `lines` pixels:
     * 3 * (lines + 1) more entries. A sprite that grows wider or narrower is drawn only when
     * its rows fit. */
    uint16_t lines;
} chr_target_t;

/* The scratch entries a target needs that is `width` wide and unpacks rows of up to `lines` pixels. */
static inline size_t chr_target_scratch(uint16_t width, uint16_t lines)
{
    return (size_t)width + 4U + (lines ? 3U * ((size_t)lines + 1U) : 0U);
}

typedef enum {
    CHR_ELLIPSE = 0,   /* a: cx cy rx ry */
    CHR_EYE,           /* a: cx cy hw hh radius lid slant arc: a rounded box cut by its lids */
    CHR_BLOB,          /* a: cx top bottom w_top w_bottom dome base shear: the body silhouette */
    CHR_STADIUM,       /* a: ax ay bx by radius: a thick line with round ends */
    CHR_BAND,          /* a: cx hw at bt ab bb: between two parabolas, round ends (the mouth) */
    CHR_POLYGON,       /* a: up to four points x y, convex */
    CHR_GLOW,          /* a: cx cy rx ry: alpha falls off from the centre to the edge */
} chr_kind_t;

typedef struct {
    uint8_t kind;             /* chr_kind_t */
    uint8_t alpha;            /* 0..32 */
    uint8_t points;           /* CHR_POLYGON */
    uint32_t top, bottom;     /* RGB888 at the top and the bottom row of the bounds */
    float a[8];
    int16_t x0, y0, x1, y1;   /* inclusive bounds, set by chr_shape_bounds */
} chr_shape_t;

/* Fills the bounds from the geometry; false when the shape is empty. */
bool chr_shape_bounds(chr_shape_t *shape);
/* Draws one shape; returns the pixels written. */
uint32_t chr_shape_fill(const chr_target_t *target, const chr_shape_t *shape);
/* The spans of one row, for tests: returns how many (0..2). */
int chr_shape_spans(const chr_shape_t *shape, float y, float spans[4]);

/* ----------------------------------------------------------------- sprites */

/* A sprite pack (tools/character/bake.py): pictures cut from the puppet at
 * display size, stored as rows of runs (transparent, opaque, with alpha), the
 * slots that say which picture is which part (chr_sprites.h), the layout
 * (where the parts of this figure are) and the clips (gestures that are played
 * with several pictures). The pack is drawn from where the caller keeps it;
 * nothing is decoded ahead.
 *
 * Either all of it is in memory, or only its head is (everything but the
 * sprites) and a shelf keeps the sprites that are in use: see chr_shelf.h. */
typedef enum { CHR_SHELF_READY = 0, CHR_SHELF_COMING, CHR_SHELF_FAILED } chr_shelf_state_t;

typedef struct {
    /* [painters, any core] The bytes of a sprite, NULL while it is not in memory. */
    const uint8_t *(*find)(void *ctx, unsigned id);
    /* [step] Asks for sprites (CHR_NO_SPRITE among them is nothing) without disturbing the kept
     * ones, which are being drawn. READY: all of them are in memory. FAILED: one cannot be had. */
    chr_shelf_state_t (*want)(void *ctx, const uint16_t *ids, unsigned count, const uint16_t *kept, unsigned kept_count);
    void *ctx;
} chr_shelf_t;

typedef struct chr_pack {
    const uint8_t *data;
    uint32_t size, head;
    uint16_t count, figure_height, layout_count, slot_count, clip_words;
    uint32_t layout_at, slots_at, clips_at, table_at;
    const chr_shelf_t *shelf;
} chr_pack_t;

/* A clip: the pictures a gesture is played with, one after the other, each for its own time. The
 * frames from `first` on: `enter` of them are shown once, the next `loop` again and again for as
 * long as the gesture lasts (none: the last picture of the entry stays), the last `leave` once on
 * the way out. `style`: how she gets into the gesture and from one of its pictures to the next
 * (the character's business; the pack only carries the number). */
typedef struct { uint16_t first, enter, loop, leave, style; } chr_clip_t;
/* ms 0: this picture stays. `under`: the picture this one is a patch over (it differs from that
 * one in a hand or an eye and is stored as the difference); CHR_NO_SPRITE: a picture of its own. */
typedef struct { uint16_t sprite, ms, under; } chr_frame_t;

typedef struct { int16_t x, y; uint16_t width, height; } chr_sprite_t;
enum { CHR_NO_SPRITE = 0xFFFF };

/* False when the bytes are not a pack this code reads. */
bool chr_pack_open(chr_pack_t *pack, const void *data, size_t size);
/* The same for a pack of which only the head is in memory; its sprites come from the shelf. */
bool chr_pack_open_head(chr_pack_t *pack, const void *head, size_t size, const chr_shelf_t *shelf);
/* How long the head is, from the first CHR_PACK_FIXED bytes of a pack; 0 when it is not one. */
enum { CHR_PACK_FIXED = 28, CHR_PACK_HEAD_MAX = 65536 };
uint32_t chr_pack_head(const void *fixed, uint32_t *total);
/* Where the bytes of a sprite lie in the pack. */
bool chr_pack_place(const chr_pack_t *pack, unsigned id, uint32_t *start, uint32_t *length);
/* Whether these are the bytes of that sprite: their checksum, and rows that stay inside them. */
bool chr_pack_intact(const chr_pack_t *pack, unsigned id, const uint8_t *bytes);
/* zlib's CRC-32, continued from `crc` (0 to begin). */
uint32_t chr_crc32(uint32_t crc, const void *data, size_t size);
/* Where sprite `id` sits relative to the anchor of the figure (bottom centre). */
bool chr_pack_sprite(const chr_pack_t *pack, unsigned id, chr_sprite_t *out);
/* The sprite in `slot`; CHR_NO_SPRITE when this figure has no such part. */
unsigned chr_pack_slot(const chr_pack_t *pack, unsigned slot);
/* Layout value `index`, pixels from the anchor; 0 past what the pack holds. */
int chr_pack_layout(const chr_pack_t *pack, unsigned index);
/* The clip of a gesture (its number); false when the pack plays it with one picture or not at all. */
bool chr_pack_clip(const chr_pack_t *pack, unsigned gesture, chr_clip_t *out);
/* Frame `index` of the pack's frames (a clip's `first` and on). */
bool chr_pack_frame(const chr_pack_t *pack, unsigned index, chr_frame_t *out);

/* One sprite on the stage. Rows may slide sideways (a tilt about a pivot row,
 * and a sway that grows towards one end), which is how hair, ears and the
 * tail move without redrawing them; small sprites may also turn (columns
 * slide up and down), which is how a brow slants.
 *
 * A picture of a whole figure moves as one piece instead: it grows or shrinks
 * about a pin (her feet), each direction by its own amount, which is how she
 * breathes, squashes and stretches without any part of the picture bending.
 * Pictures that share the pin and the amounts stay on top of each other (her
 * eyes and mouth over the picture of all of her). Each stage pixel takes the
 * nearest pixel of the picture, so a picture that only grows taller or shorter
 * costs what a picture at rest costs. A picture that grows does not also tilt,
 * sway or turn. */
typedef struct {
    uint16_t id;
    int16_t x, y;             /* stage position of the top-left corner */
    uint8_t alpha;            /* 0..32 */
    uint8_t grey;             /* 0..32: washed out */
    float tilt, tilt_y;       /* each row above tilt_y moves right by tilt pixels more */
    float sway, sway_from, sway_to; /* 0 at row sway_from, `sway` pixels at row sway_to, squared between */
    float turn, turn_x;       /* each column right of turn_x sinks by turn pixels more */
    uint16_t patch;           /* 1 + a sprite of spans that takes the place of parts of this one's rows; 0: none */
    float grow_x, grow_y;     /* 0.05: a twentieth wider, taller; negative: narrower, shorter */
    float pin_x, pin_y;       /* the stage point that stays where it is while the picture grows */
    int16_t x0, y0, x1, y1;   /* inclusive bounds on the stage, set by chr_blit_bounds */
} chr_blit_t;

bool chr_blit_bounds(const chr_pack_t *pack, chr_blit_t *blit);
uint32_t chr_blit_draw(const chr_target_t *target, const chr_pack_t *pack, const chr_blit_t *blit);

#ifdef __cplusplus
}
#endif
