// SPDX-License-Identifier: MIT
#pragma once
/* A sprite pack made in the test: every part is a plain rectangle, laid out
 * as a different figure from the one the firmware ships (smaller head, animal
 * ears and a tail, optionally arms and a head that turns). The tests run on
 * it and on the shipped pack alike, which is the proof that the character
 * takes its figure from the pack and from nowhere else. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "chr_internal.h"
#include "chr_sprites.h"

/* FIXTURE_FLAT: one picture of the whole figure with eyes and mouths as patches, and one picture for
 * waving, which is stored with a palette. FIXTURE_CLIP: a dance of four pictures (one to enter, two that loop, one to leave), each
 * changed on a beat; and a cheer of two pictures that take turns, the second stored as a patch over
 * the first (FIXTURE_PATCH_SLOT). */
enum { FIXTURE_EARS = 1, FIXTURE_TAIL = 2, FIXTURE_ARMS = 4, FIXTURE_YAW = 8, FIXTURE_NO_BODY = 16, FIXTURE_FLAT = 32,
       FIXTURE_CLIP = 64 };
enum { FIXTURE_CLIP_WORDS = 32, FIXTURE_PATCH_SLOT = SL_POSE + CHR_POSES - 4 };

typedef struct {
    uint8_t *bytes;
    size_t size, capacity;
    uint16_t slots[SL_COUNT];
    uint8_t table[SL_COUNT * 20];
    uint16_t count;
    uint8_t *body;
    size_t body_size, body_capacity;
} fixture_t;

/* The checksum of a pack's head, again after a test has changed something in it. */
static void fixture_seal(uint8_t *pack)
{
    uint32_t head, crc = 0;
    memcpy(&head, pack + 20, 4);
    memcpy(pack + 24, &crc, 4);
    crc = chr_crc32(0, pack, head);
    memcpy(pack + 24, &crc, 4);
}

static void fixture_put(fixture_t *f, unsigned slot, int x, int y, int width, int height, uint16_t colour)
{
    /* One sprite: the row offsets, then per row runs of at most 64 opaque pixels. */
    const size_t row = (size_t)((width + 63) / 64) + 2U * (size_t)width, need = 4U * ((size_t)height + 1U) + row * (size_t)height;
    if (f->body_size + need + 8U > f->body_capacity) {
        f->body_capacity = 2U * (f->body_capacity + need) + 64U;
        f->body = realloc(f->body, f->body_capacity);
    }
    while (f->body_size % 4U) f->body[f->body_size++] = 0;
    uint8_t *at = f->body + f->body_size;
    for (int r = 0; r <= height; ++r) {
        const uint32_t offset = (uint32_t)(4U * ((size_t)height + 1U) + row * (size_t)r);
        memcpy(at + 4 * r, &offset, 4);
    }
    uint8_t *p = at + 4 * (height + 1);
    for (int r = 0; r < height; ++r)
        for (int left = width; left > 0; left -= 64) {
            const int run = left > 64 ? 64 : left;
            *p++ = (uint8_t)(0x80 + run - 1);
            for (int i = 0; i < run; ++i) { memcpy(p, &colour, 2); p += 2; }
        }
    uint8_t *entry = f->table + 20U * f->count;
    const int16_t sx = (int16_t)x, sy = (int16_t)y;
    const uint16_t w = (uint16_t)width, h = (uint16_t)height;
    const uint32_t start = (uint32_t)f->body_size, length = (uint32_t)need;   /* start is made absolute at the end */
    memcpy(entry, &sx, 2); memcpy(entry + 2, &sy, 2); memcpy(entry + 4, &w, 2); memcpy(entry + 6, &h, 2);
    memcpy(entry + 8, &start, 4); memcpy(entry + 12, &length, 4);
    f->slots[slot] = f->count++;
    f->body_size += need;
}

/* The same rectangle stored with a palette: a byte a pixel, the colour in place FIXTURE_INK of 256. */
enum { FIXTURE_INK = 7 };
static void fixture_put_indexed(fixture_t *f, unsigned slot, int x, int y, int width, int height, uint16_t colour)
{
    const size_t row = (size_t)((width + 63) / 64) + (size_t)width, head = 4U * ((size_t)height + 1U) + 512U;
    const size_t need = head + row * (size_t)height;
    if (f->body_size + need + 8U > f->body_capacity) {
        f->body_capacity = 2U * (f->body_capacity + need) + 64U;
        f->body = realloc(f->body, f->body_capacity);
    }
    while (f->body_size % 4U) f->body[f->body_size++] = 0;
    uint8_t *at = f->body + f->body_size;
    for (int r = 0; r <= height; ++r) {
        const uint32_t offset = (uint32_t)(head + row * (size_t)r);
        memcpy(at + 4 * r, &offset, 4);
    }
    for (unsigned i = 0; i < 256U; ++i) {
        const uint16_t entry = i == FIXTURE_INK ? colour : (uint16_t)(0x1234U + i);
        memcpy(at + 4 * (height + 1) + 2U * i, &entry, 2);
    }
    uint8_t *p = at + head;
    for (int r = 0; r < height; ++r)
        for (int left = width; left > 0; left -= 64) {
            const int run = left > 64 ? 64 : left;
            *p++ = (uint8_t)(0x80 + run - 1);
            memset(p, FIXTURE_INK, (size_t)run);
            p += run;
        }
    uint8_t *entry = f->table + 20U * f->count;
    const int16_t sx = (int16_t)x, sy = (int16_t)y;
    const uint16_t w = (uint16_t)width, h = (uint16_t)height;
    const uint32_t start = (uint32_t)f->body_size, length = (uint32_t)need | 1U << 24;
    memcpy(entry, &sx, 2); memcpy(entry + 2, &sy, 2); memcpy(entry + 4, &w, 2); memcpy(entry + 6, &h, 2);
    memcpy(entry + 8, &start, 4); memcpy(entry + 12, &length, 4);
    f->slots[slot] = f->count++;
    f->body_size += need;
}

/* A patch over another sprite: in every row the columns [from, to) take the place of that sprite's
 * pixels. The upper half of its rows fill their span; the lower half fill only the first half of it
 * and leave the rest empty, which must show what is behind the figure. */
static void fixture_put_spans(fixture_t *f, unsigned slot, int x, int y, int width, int height, uint16_t colour, int from, int to)
{
    const size_t row = 4U + 4U + 2U * (size_t)(to - from), need = 4U * ((size_t)height + 1U) + row * (size_t)height;
    if (f->body_size + need + 8U > f->body_capacity) {
        f->body_capacity = 2U * (f->body_capacity + need) + 64U;
        f->body = realloc(f->body, f->body_capacity);
    }
    while (f->body_size % 4U) f->body[f->body_size++] = 0;
    uint8_t *at = f->body + f->body_size, *p = at + 4 * (height + 1);
    for (int r = 0; r < height; ++r) {
        const uint32_t offset = (uint32_t)(p - at);
        const uint16_t span[2] = {(uint16_t)from, (uint16_t)to};
        const int filled = r < height / 2 ? to - from : (to - from) / 2;
        memcpy(at + 4 * r, &offset, 4);
        memcpy(p, span, 4);
        p += 4;
        *p++ = (uint8_t)(from - 1);                 /* skip the columns before the span */
        for (int left = filled; left > 0; left -= 64) {
            const int run = left > 64 ? 64 : left;
            *p++ = (uint8_t)(0x80 + run - 1);
            for (int i = 0; i < run; ++i) { memcpy(p, &colour, 2); p += 2; }
        }
    }
    const uint32_t end = (uint32_t)(p - at);
    memcpy(at + 4 * height, &end, 4);
    uint8_t *entry = f->table + 20U * f->count;
    const int16_t sx = (int16_t)x, sy = (int16_t)y;
    const uint16_t w = (uint16_t)width, h = (uint16_t)height;
    const uint32_t start = (uint32_t)f->body_size, length = end | 2U << 24;
    memcpy(entry, &sx, 2); memcpy(entry + 2, &sy, 2); memcpy(entry + 4, &w, 2); memcpy(entry + 6, &h, 2);
    memcpy(entry + 8, &start, 4); memcpy(entry + 12, &length, 4);
    f->slots[slot] = f->count++;
    f->body_size += end;
}

/* The fixture figure. `out_fig` receives its layout. The caller frees the result. */
static uint8_t *fixture_pack(unsigned parts, size_t *size, chr_figure_t *out_fig)
{
    fixture_t f = {0};
    memset(f.slots, 0xFF, sizeof(f.slots));
    const chr_figure_t fig = {
        .ears_top = (parts & FIXTURE_EARS) ? -500 : -423, .hair_top = -423, .brow_y = -336, .eye_y = -303, .cheek_y = -272,
        .mouth_y = -258, .neck_y = -236, .ear_base_y = (parts & FIXTURE_EARS) ? -345 : -423, .eye_lx = 30, .eye_rx = -38,
        .face_x = -5, .head_half = 95, .tail_x0 = 120, .tail_x1 = 289, .tail_top = -177, .brow_lx = 28, .brow_rx = -36,
        .cheek_dx = 20, .blush_rx = 17, .blush_ry = 8, .ear_lx0 = 8, .ear_lx1 = 92, .ear_rx0 = -104, .ear_rx1 = -20,
        .shoulder_half = 118, .yaw_shift = 20,
        .arm_lx = (parts & FIXTURE_ARMS) ? 150 : 0, .arm_rx = (parts & FIXTURE_ARMS) ? -150 : 0,
        .arm_top = (parts & FIXTURE_ARMS) ? -230 : 0, .hand_lx = (parts & FIXTURE_ARMS) ? 290 : 0,
        .hand_ly = (parts & FIXTURE_ARMS) ? -150 : 0, .hand_rx = (parts & FIXTURE_ARMS) ? -290 : 0,
        .hand_ry = (parts & FIXTURE_ARMS) ? -150 : 0, .hand_half = (parts & FIXTURE_ARMS) ? 30 : 0,
    };
    if (parts & FIXTURE_TAIL) fixture_put(&f, SL_TAIL, fig.tail_x0, fig.tail_top, fig.tail_x1 - fig.tail_x0, -fig.tail_top - 5, 0xFD20);
    for (int i = 0; i < 3 && (parts & FIXTURE_EARS); ++i) {
        fixture_put(&f, SL_EAR_L + (unsigned)i, fig.ear_lx0, fig.ears_top, fig.ear_lx1 - fig.ear_lx0, fig.ear_base_y - fig.ears_top, 0xFD20);
        fixture_put(&f, SL_EAR_R + (unsigned)i, fig.ear_rx0, fig.ears_top, fig.ear_rx1 - fig.ear_rx0, fig.ear_base_y - fig.ears_top, 0xFD20);
    }
    if (parts & FIXTURE_FLAT) fixture_put(&f, SL_BODY, -fig.shoulder_half, fig.hair_top, 2 * fig.shoulder_half, -fig.hair_top, 0xFFFF);
    else if (!(parts & FIXTURE_NO_BODY)) fixture_put(&f, SL_BODY, -fig.shoulder_half, fig.neck_y, 2 * fig.shoulder_half, -fig.neck_y, 0xFFFF);
    uint16_t clip[FIXTURE_CLIP_WORDS] = {0};
    if (parts & FIXTURE_FLAT)
        fixture_put_indexed(&f, SL_POSE + CHR_GESTURE_WAVE - 1U, -fig.shoulder_half - 40, fig.hair_top - 30, 2 * fig.shoulder_half + 80,
                            -fig.hair_top + 30, 0xE71C);
    if (parts & FIXTURE_CLIP) {
        /* Four pictures of different widths; the last three sit in pose slots no gesture uses. */
        static const uint16_t ms[4] = {100, 300, 300, 100};
        const uint16_t head[14] = {2, 6, CHR_GESTURE_DANCE, 0, 1, 2, 1, CHR_STYLE_BEAT, CHR_GESTURE_CHEER, 4, 0, 2, 0, CHR_STYLE_SOFT};
        memcpy(clip, head, sizeof(head));
        for (unsigned i = 0; i < 4; ++i) {
            const unsigned slot = i ? SL_POSE + CHR_POSES - i : SL_POSE + CHR_GESTURE_DANCE - 1U;
            fixture_put(&f, slot, -fig.shoulder_half - 10 * (int)i, fig.hair_top, 2 * fig.shoulder_half + 20 * (int)i, -fig.hair_top,
                        (uint16_t)(0x8410 + 0x0841 * i));
            clip[14 + 3 * i] = f.slots[slot];
            clip[15 + 3 * i] = ms[i];
            clip[16 + 3 * i] = CHR_NO_SPRITE;
        }
        const unsigned cheer = SL_POSE + CHR_GESTURE_CHEER - 1U;
        fixture_put(&f, cheer, -fig.shoulder_half, fig.hair_top, 2 * fig.shoulder_half, -fig.hair_top, 0x07E0);
        fixture_put_spans(&f, FIXTURE_PATCH_SLOT, -fig.shoulder_half + 20, fig.hair_top + 40, 100, 60, 0xF800, 10, 90);
        const uint16_t turns[6] = {f.slots[cheer], 150, CHR_NO_SPRITE, f.slots[FIXTURE_PATCH_SLOT], 150, f.slots[cheer]};
        memcpy(clip + 26, turns, sizeof(turns));
    }
    for (int i = 0; i < CHR_ARM_STATES && (parts & FIXTURE_ARMS); ++i) {
        /* Hanging: a tall box beside the body. Raised: a wide one that reaches the hand. */
        const int reach = 60 + 40 * i, tall = 230 - 50 * i;
        fixture_put(&f, SL_ARM_L + (unsigned)i, fig.shoulder_half, fig.arm_top, reach, tall, 0xE71C);
        fixture_put(&f, SL_ARM_R + (unsigned)i, -fig.shoulder_half - reach, fig.arm_top, reach, tall, 0xE71C);
    }
    for (int yaw = 0; yaw < ((parts & FIXTURE_YAW) ? CHR_YAWS : 1); ++yaw) {
        const unsigned block = SL_HEAD + (unsigned)yaw * H_SLOTS;
        const int dx = yaw == 1 ? -fig.yaw_shift : yaw == 2 ? fig.yaw_shift : 0;
        if (!(parts & FIXTURE_FLAT)) {
            fixture_put(&f, block + H_HAIR_BACK, fig.face_x - fig.head_half + dx, fig.hair_top, 2 * fig.head_half, 197, 0xFCA0);
            fixture_put(&f, block + H_FACE, fig.face_x - 80 + dx, fig.hair_top + 23, 160, 164, 0xFF5A);
            fixture_put(&f, block + H_HAIR_FRONT, fig.face_x - fig.head_half + dx, fig.hair_top, 2 * fig.head_half, 100, 0xFCA0);
            fixture_put(&f, block + H_BROW_L, fig.brow_lx - 14 + dx, fig.brow_y - 4, 28, 8, 0x8200);
            fixture_put(&f, block + H_BROW_R, fig.brow_rx - 14 + dx, fig.brow_y - 4, 28, 8, 0x8200);
        }
        for (unsigned m = 0; m < 15; ++m)
            fixture_put(&f, block + H_MOUTH + m, fig.face_x - 15 + dx, fig.mouth_y - 4 - (int)(m / 3U) * 2, 30, 8 + (int)(m / 3U) * 4, 0xA000);
        for (unsigned e = 0; e < 9; ++e) {
            const int open = e < 3 ? 30 : e < 6 ? 20 : 8;
            fixture_put(&f, block + H_EYE_L + e, fig.eye_lx - 18 + dx, fig.eye_y - open / 2, 36, open, 0xC000);
            fixture_put(&f, block + H_EYE_R + e, fig.eye_rx - 18 + dx, fig.eye_y - open / 2, 36, open, 0xC000);
        }
    }
    /* Header, layout, slots, table, sprites. */
    const size_t layout_bytes = (2U * CHR_FIGURE_VALUES + 3U) & ~(size_t)3U, slot_bytes = (2U * SL_COUNT + 3U) & ~(size_t)3U;
    const size_t clip_bytes = (parts & FIXTURE_CLIP) ? sizeof(clip) : 0U;
    const size_t head = 28U + layout_bytes + slot_bytes + clip_bytes + 20U * f.count, total = head + f.body_size;
    uint8_t *pack = calloc(1, total);
    const uint16_t header[6] = {4, f.count, 520, CHR_FIGURE_VALUES, SL_COUNT, (uint16_t)(clip_bytes / 2U)};
    const uint32_t total32 = (uint32_t)total, head32 = (uint32_t)head;
    memcpy(pack, "XTCH", 4); memcpy(pack + 4, header, sizeof(header)); memcpy(pack + 16, &total32, 4); memcpy(pack + 20, &head32, 4);
    memcpy(pack + 28, &fig, sizeof(fig));
    memcpy(pack + 28 + layout_bytes, f.slots, sizeof(f.slots));
    for (unsigned i = 0; i < f.count; ++i) {
        uint32_t start, length;
        memcpy(&start, f.table + 20U * i + 8, 4);
        memcpy(&length, f.table + 20U * i + 12, 4);
        const uint32_t crc = chr_crc32(0, f.body + start, length & 0xFFFFFFU);
        start += (uint32_t)head;
        memcpy(f.table + 20U * i + 8, &start, 4);
        memcpy(f.table + 20U * i + 16, &crc, 4);
    }
    memcpy(pack + 28 + layout_bytes + slot_bytes, clip, clip_bytes);
    memcpy(pack + 28 + layout_bytes + slot_bytes + clip_bytes, f.table, 20U * f.count);
    fixture_seal(pack);
    memcpy(pack + head, f.body, f.body_size);
    free(f.body);
    *size = total;
    if (out_fig) *out_fig = fig;
    return pack;
}
