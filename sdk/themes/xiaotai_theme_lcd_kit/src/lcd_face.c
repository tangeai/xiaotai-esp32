// SPDX-License-Identifier: MIT
/* The face: the numbers of an expression (lcd_face_look.c), each on its way to
 * where it should be, with the life of the moment on them (lcd_face_life.c),
 * become shapes in the pixels of the display (lcd_face_ink.c). A frame is laid
 * out once, in lcd_face_step(); drawing it, whole or in strips, only reads. */
#include "lcd_face.h"

#include "lvgl.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/theme_lcd.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "lcd_face_ink.h"
#include "lcd_face_look.h"

#define PI2 6.2831853f

/* Parts that move on their own: each is damaged for itself. */
enum {
    PART_CHEEK_L, PART_CHEEK_R, PART_EYE_L, PART_EYE_R, PART_BROW_L, PART_BROW_R, PART_MOUTH, PART_GLASSES,
    PART_TEARS_L, PART_TEARS_R, PART_SWEAT, PART_HEART, PART_KISS, PART_NOTES, PART_ZZZ, PART_QUESTION, PART_EXCLAIM,
    PART_SPARKLE, PART_VEIN, PART_THOUGHT, PART_COUNT
};
/* Shapes with columns of their own, which stay while the shape does not change. */
enum {
    SHAPE_EYE_L, SHAPE_EYE_R, SHAPE_LOVE_L, SHAPE_LOVE_R, SHAPE_BROW_L, SHAPE_BROW_R, SHAPE_MOUTH, SHAPE_HOLLOW, SHAPE_TONGUE,
    SHAPE_RIM_L, SHAPE_RIM_R, SHAPE_LENS_L, SHAPE_LENS_R, SHAPE_PROP, SHAPES = SHAPE_PROP + 12
};
/* The widest each gets, in units. */
static const uint8_t shape_room[SHAPES] = {
    98, 98, 44, 44, 48, 48, 80, 76, 20, 80, 80, 76, 76, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26
};
enum { ITEMS = 80 };
enum { CUE_FIRST = LOOK_TEARS, CUES = LOOK_COUNT - LOOK_TEARS };

typedef enum { ITEM_SHAPE, ITEM_LID, ITEM_IRIS, ITEM_DISC, ITEM_SOFT, ITEM_STROKE } item_kind_t;

typedef struct {
    uint8_t kind, part;
    int8_t shape, inside;       /* into shapes; -1: none */
    uint16_t alpha;
    ink_box_t box;
    uint32_t top, bottom;
    float v[5];
} item_t;

struct lcd_face {
    /* The display task owns all of this. The counters alone may be read from
     * another task. */
    float now[LOOK_COUNT], speed[LOOK_COUNT];
    int64_t cue_at[CUES];
    int64_t last_frame_at, asked_at;
    const look_t *look;
    lcd_face_life_t life;
    lcd_face_senses_t senses;
    lcd_face_frame_t frame;
    uint32_t background;
    uint16_t width, height, frame_ms;
    uint16_t asked, shown;              /* indices into lcd_face_looks */
    uint8_t asked_phase, asked_level;
    bool attached, visible, laid_out, clear_all;
    /* this frame */
    float scale, ox, oy;                /* pixels of a unit, and of its origin */
    lcd_face_drift_t drift;             /* as drawn */
    item_t items[ITEMS];
    ink_shape_t shapes[SHAPES];
    ink_form_t forms[SHAPES];
    uint16_t first_column[SHAPES + 1];
    uint32_t iris[INK_TIERS][INK_RAMP];
    uint32_t iris_key;
    uint16_t item_count, prop_count;
    ink_column_t *columns;
    ink_box_t part_box[PART_COUNT], part_before[PART_COUNT];
    uint32_t part_key[PART_COUNT], part_key_before[PART_COUNT];
    ink_box_t drawn;
    /* one row of it */
    uint32_t *line;
    uint16_t *mask;
    atomic_uint frames, late, slow, direct, pixels, fallback, step_us, paint_us;
};

/* One writer each; a plain store keeps the external memory free of atomic
 * read-modify-write. */
static void count_add(atomic_uint *counter, unsigned delta)
{
    atomic_store_explicit(counter, atomic_load_explicit(counter, memory_order_relaxed) + delta, memory_order_relaxed);
}

const char *xiaotai_theme_lcd_emotion_name(size_t index)
{
    return index < XIAOTAI_THEME_LCD_EMOTION_COUNT ? lcd_face_looks[index].name : NULL;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float ramp(float t, float a, float b)
{
    const float x = clampf((t - a) / (b - a), 0, 1);
    return x * x * (3.0f - 2.0f * x);
}
/* Up, held, down; nothing outside. */
static float pulse(float t, float at, float rise, float hold, float fall)
{
    return ramp(t, at, at + rise) * (1.0f - ramp(t, at + rise + hold, at + rise + hold + fall));
}
static float wave(float ms, float period) { return sinf(PI2 * fmodf(ms, period) / period); }
static unsigned alpha_of(float v) { return (unsigned)(clampf(v, 0, 1) * INK_SOLID + 0.5f); }

/* ------------------------------------------------------------------ layout */

/* Everything is kept to eighths of a pixel: what did not move by one does not
 * change, and is not drawn again. */
static float nearest(float v) { return (float)(int)(v < 0.0f ? v - 0.5f : v + 0.5f); }
static float snap(float v) { return nearest(v * 8.0f) * 0.125f; }
static float fine(float part) { return nearest(part * 32.0f) * (1.0f / 32.0f); }
static float px(const lcd_face_t *f, float units) { return snap(units * f->scale); }
static float at_x(const lcd_face_t *f, float x) { return snap(f->ox + x * f->scale); }
static float at_y(const lcd_face_t *f, float y) { return snap(f->oy + y * f->scale); }

static uint32_t stir(uint32_t key, uint32_t value) { return (key ^ value) * 16777619U; }
static uint32_t stir_f(uint32_t key, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return stir(key, bits ^ bits >> 15);
}

static ink_box_t box_of(const lcd_face_t *f, float x0, float y0, float x1, float y1)
{
    const float w = (float)f->width - 1.0f, h = (float)f->height - 1.0f;
    ink_box_t b = {0, 0, -1, -1};
    if (x1 < 0.0f || y1 < 0.0f || x0 > w || y0 > h) return b;
    b.x0 = (int16_t)clampf(x0 - 1.0f, 0, w);
    b.y0 = (int16_t)clampf(y0 - 1.0f, 0, h);
    b.x1 = (int16_t)clampf(x1 + 1.0f, 0, w);
    b.y1 = (int16_t)clampf(y1 + 1.0f, 0, h);
    return b;
}

static void add(lcd_face_t *f, const item_t *given)
{
    item_t *item = &f->items[f->item_count];
    if (ink_empty(&given->box) || f->item_count == ITEMS) return;
    *item = *given;
    item->alpha = (uint16_t)((given->alpha + 2U) & ~3U);
    if (!item->alpha) return;
    ++f->item_count;
    f->part_box[item->part] = ink_join(f->part_box[item->part], item->box);
    uint32_t key = stir(f->part_key[item->part], (uint32_t)item->kind | (uint32_t)item->alpha << 8 | (uint32_t)(uint8_t)item->inside << 24);
    key = stir(stir(key, item->top), item->bottom);
    for (unsigned i = 0; i < sizeof(item->v) / sizeof(item->v[0]); ++i) key = stir_f(key, item->v[i]);
    f->part_key[item->part] = key;
}

static int shape(lcd_face_t *f, unsigned slot, unsigned part, const ink_form_t *form)
{
    ink_shape_t *s = &f->shapes[slot];
    if (!s->column || memcmp(&f->forms[slot], form, sizeof(*form))) {
        f->forms[slot] = *form;
        ink_form(s, form, &f->columns[f->first_column[slot]], (size_t)(f->first_column[slot + 1] - f->first_column[slot]),
                 (int16_t)f->width, (int16_t)f->height);
    }
    if (!s->count) return -1;
    uint32_t key = f->part_key[part], bits[sizeof(*form) / sizeof(uint32_t)];
    memcpy(bits, form, sizeof(bits));
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) key = stir(key, bits[i] ^ bits[i] >> 15);
    f->part_key[part] = key;
    return (int)slot;
}

static void fill(lcd_face_t *f, unsigned part, int which, uint32_t top, uint32_t bottom, unsigned alpha, int inside)
{
    if (which < 0) return;
    add(f, &(item_t){.kind = ITEM_SHAPE, .part = (uint8_t)part, .shape = (int8_t)which, .inside = (int8_t)inside,
        .alpha = (uint16_t)alpha, .box = f->shapes[which].box, .top = top, .bottom = bottom});
}

static void round_item(lcd_face_t *f, unsigned part, item_kind_t kind, float x, float y, float rx, float ry, uint32_t rgb,
                       unsigned alpha, int inside)
{
    item_t item = {.kind = (uint8_t)kind, .part = (uint8_t)part, .shape = -1, .inside = (int8_t)inside, .alpha = (uint16_t)alpha,
        .box = box_of(f, x - rx, y - ry, x + rx, y + ry), .top = rgb, .v = {x, y, rx, ry}};
    if (inside >= 0) {
        const ink_box_t *in = &f->shapes[inside].box;
        if (item.box.x0 < in->x0) item.box.x0 = in->x0;
        if (item.box.y0 < in->y0) item.box.y0 = in->y0;
        if (item.box.x1 > in->x1) item.box.x1 = in->x1;
        if (item.box.y1 > in->y1) item.box.y1 = in->y1;
    }
    add(f, &item);
}

static void disc(lcd_face_t *f, unsigned part, float x, float y, float rx, float ry, uint32_t rgb, unsigned alpha, int inside)
{
    round_item(f, part, ITEM_DISC, x, y, rx, ry, rgb, alpha, inside);
}

/* Units, on the canvas. */
static void dot(lcd_face_t *f, unsigned part, float x, float y, float r, uint32_t rgb, unsigned alpha)
{
    disc(f, part, at_x(f, x), at_y(f, y), px(f, r), px(f, r), rgb, alpha, -1);
}

static void line(lcd_face_t *f, unsigned part, float ax, float ay, float bx, float by, float width, uint32_t rgb, unsigned alpha, int inside)
{
    const float x0 = at_x(f, ax), y0 = at_y(f, ay), x1 = at_x(f, bx), y1 = at_y(f, by);
    const float r = fmaxf(width * f->scale * 0.5f, 0.6f);
    add(f, &(item_t){.kind = ITEM_STROKE, .part = (uint8_t)part, .shape = -1, .inside = (int8_t)inside, .alpha = (uint16_t)alpha,
        .box = box_of(f, fminf(x0, x1) - r, fminf(y0, y1) - r, fmaxf(x0, x1) + r, fmaxf(y0, y1) + r), .top = rgb, .v = {x0, y0, x1, y1, r}});
}

static void path(lcd_face_t *f, unsigned part, const int8_t (*points)[2], unsigned count, float x, float y, float size, float width,
                 uint32_t rgb, unsigned alpha)
{
    for (unsigned i = 1; i < count; ++i)
        line(f, part, x + points[i - 1][0] * size, y + points[i - 1][1] * size, x + points[i][0] * size, y + points[i][1] * size,
             width, rgb, alpha, -1);
}

static int glyph(lcd_face_t *f, unsigned part, ink_glyph_t kind, float x, float y, float rx, float ry)
{
    if (f->prop_count == SHAPES - SHAPE_PROP) return -1;
    return shape(f, (unsigned)(SHAPE_PROP + f->prop_count++), part, &(ink_form_t){.glyph = kind, .cx = at_x(f, x), .cy = at_y(f, y), .rx = px(f, rx), .ry = px(f, ry), .open = 1});
}

static void heart(lcd_face_t *f, unsigned part, float x, float y, float size, unsigned alpha)
{
    const int s = glyph(f, part, INK_HEART, x, y, size, size * 0.94f);
    fill(f, part, s, 0xFF9DBC, 0xF0507F, alpha, -1);
    if (s >= 0) disc(f, part, at_x(f, x - size * 0.42f), at_y(f, y - size * 0.40f), px(f, size * 0.20f), px(f, size * 0.14f), 0xFFE3EC, alpha * 3U / 4U, s);
}

/* ------------------------------------------------------------------- parts */

typedef struct { float x, y, w, h; } spot_t;       /* where an eye came to be: middle and size, units */

static float age_of(const lcd_face_t *f, look_channel_t cue, int64_t now)
{
    const int64_t age = now - f->cue_at[cue - CUE_FIRST];
    return age > 0 ? (float)age : 0.0f;
}

static uint32_t tint_of(const lcd_face_t *f)
{
    const float shade = 1.0f - 0.45f * f->frame.shade;
    return (uint32_t)clampf(f->now[LOOK_RED] * shade, 0, 255) << 16 | (uint32_t)clampf(f->now[LOOK_GREEN] * shade, 0, 255) << 8 |
           (uint32_t)clampf(f->now[LOOK_BLUE] * shade, 0, 255);
}

/* From the middle of an iris to its rim. The lower half catches light; while
 * the face listens a ring lights up, as bright as it is loud. */
static void iris_colours(lcd_face_t *f, float pupil, float ring)
{
    const uint32_t key = (uint32_t)(pupil * 100.0f) << 8 | (uint32_t)(ring * 40.0f) | 0x80000000U;
    if (f->iris_key == key) return;
    f->iris_key = key;
    for (unsigned i = 0; i < INK_RAMP; ++i) {
        const float r = sqrtf((float)i / (INK_RAMP - 1));
        const float t = clampf((r - pupil) / (1.0f - pupil), 0, 1);
        const unsigned out = alpha_of(ramp(r, pupil - 0.05f, pupil + 0.05f));
        const unsigned rim = alpha_of(ramp(r, 0.80f, 0.97f));
        uint32_t upper = ink_mix(0x1A4257, 0x15384C, alpha_of(t)), lower = ink_mix(0x46B4DC, 0x2389B4, alpha_of(t));
        upper = ink_mix(ink_mix(0x08131C, upper, out), 0x0A202D, rim);
        lower = ink_mix(ink_mix(0x0B1C28, lower, out), 0x123E54, rim);
        const float d = (r - 0.80f) / 0.15f;
        const unsigned lit = alpha_of(ring * clampf(1.0f - d * d, 0, 1));
        upper = ink_mix(upper, 0x6FDCFF, lit);
        lower = ink_mix(lower, 0xA6ECFF, lit);
        for (unsigned tier = 0; tier < INK_TIERS; ++tier) f->iris[tier][i] = ink_mix(upper, lower, tier * 256U / (INK_TIERS - 1));
    }
}

static void lay_eye(lcd_face_t *f, unsigned side, int64_t now, spot_t *spot)
{
    const lcd_face_frame_t *l = &f->frame;
    const float *v = f->now;
    const unsigned part = side ? PART_EYE_R : PART_EYE_L;
    const float s = side ? 1.0f : -1.0f;
    const float gx = clampf(l->gaze_x + v[LOOK_GAZE_X], -15, 15), gy = clampf(l->gaze_y + v[LOOK_GAZE_Y], -9, 9);
    const float turn = gx / 15.0f, nod = gy / 9.0f;
    const float grow = 1.0f + v[side ? LOOK_GROW_R : LOOK_GROW_L];
    float open = l->open[side];
    const float tall = open > 1.0f ? open : 1.0f;
    const float w = v[LOOK_EYE_W] * grow * (1.0f + 0.07f * s * turn) * (1.0f + 0.50f * l->squash);
    const float h = fmaxf(v[LOOK_EYE_H] * grow * tall * (1.0f + 0.09f * s * turn) * (1.0f - 0.85f * l->squash) * (1.0f - 0.06f * nod), 5.0f);
    const float tilt = v[LOOK_LEAN] + l->tilt;
    float x = LCD_FACE_WIDTH * 0.5f + s * v[LOOK_EYE_GAP] + gx * 0.78f + l->sway;
    const float y = v[LOOK_EYE_Y] + gy * 0.70f + l->rise + (x - LCD_FACE_WIDTH * 0.5f) * tilt / 64.0f;
    x = clampf(x, w * 0.5f + 2.0f, LCD_FACE_WIDTH - w * 0.5f - 2.0f);
    *spot = (spot_t){x, y, w, h};

    /* A wink is a smile with one eye; what is shut already does not blink. */
    const float shows = clampf(v[LOOK_IRIS] * 2.0f, 0, 1);
    float sag = 3.0f;
    if (side && v[LOOK_WINK] > 0.02f) {
        const float wink = v[LOOK_WINK] * pulse(fmodf((float)(now - f->asked_at), 4200.0f), 380, 120, 760, 190);
        open = fminf(open, 1.0f - wink);
        sag = mixf(sag, -11.0f, wink);
    }
    open = 1.0f - (1.0f - clampf(open, 0, 1)) * fmaxf(shows, v[LOOK_HEARTS]);
    const float top = y - h * 0.5f, bottom = y + h * 0.5f;
    const float upper = v[side ? LOOK_UPPER_R : LOOK_UPPER_L], lower = v[side ? LOOK_LOWER_R : LOOK_LOWER_L];
    const bool lids = upper > 0.004f || lower > 0.004f || fabsf(v[LOOK_UPPER_TILT]) > 0.1f;
    const ink_form_t form = {
        .cx = at_x(f, x), .cy = at_y(f, y), .rx = px(f, w * 0.5f), .ry = px(f, h * 0.5f),
        .round = px(f, fminf(w, h) * 0.5f * v[LOOK_EYE_ROUND]), .soft = fine(clampf(v[LOOK_EYE_SOFT], 0, 1)),
        .shear = px(f, -s * v[LOOK_EYE_SHEAR]), .bend = px(f, v[LOOK_EYE_BEND]),
        .shade = 1, .lids = lids,
        .upper = at_y(f, top + upper * h), .upper_tilt = px(f, -s * v[LOOK_UPPER_TILT]), .upper_arch = px(f, v[LOOK_UPPER_ARCH]),
        .lower = at_y(f, bottom - lower * h), .lower_tilt = 0, .lower_arch = px(f, v[LOOK_LOWER_ARCH]), .ease = px(f, 6.0f),
        .line = at_y(f, top + h * 0.68f), .open = fine(open), .least = px(f, 2.7f), .sag = px(f, sag),
    };
    const float visible = 1.0f - v[LOOK_GLASSES];
    const unsigned solid = alpha_of(visible);
    if (!solid) return;
    const uint32_t tint = tint_of(f);
    const int eye = shape(f, SHAPE_EYE_L + side, part, &form);
    if (eye < 0) return;
    /* The white, with a little roundness. */
    fill(f, part, eye, ink_mix(tint, 0xFFFFFF, 70), ink_mix(tint, 0x8FA9BC, 92), solid, -1);

    /* Iris, hearts in love, and what shines on them. */
    const float round_iris = shows * (1.0f - v[LOOK_HEARTS]) * ramp(open, 0.08f, 0.30f);
    const float room = clampf(v[LOOK_EYE_H] / 46.0f, 0.2f, 1.1f);
    const float size = v[LOOK_EYE_H] * 0.355f * clampf(v[LOOK_IRIS], 0, 1.3f) * l->pupil * (0.55f + 0.45f * room) * grow;
    const float ix = x + gx * 0.52f - s * v[LOOK_CROSS] * 9.0f, iy = y + gy * 0.44f + upper * h * 0.30f - lower * h * 0.22f;
    if (round_iris > 0.02f && size > 1.5f) {
        iris_colours(f, clampf(v[LOOK_PUPIL], 0.2f, 0.8f), l->listen);
        const float rx = px(f, size * 0.96f), ry = px(f, size), cx = at_x(f, ix), cy = at_y(f, iy);
        item_t iris = {.kind = ITEM_IRIS, .part = (uint8_t)part, .shape = -1, .inside = (int8_t)eye, .alpha = (uint16_t)alpha_of(round_iris * visible),
            .box = box_of(f, cx - rx, cy - ry, cx + rx, cy + ry), .top = f->iris_key, .v = {cx, cy, rx, ry}};
        add(f, &iris);
        const unsigned shine = alpha_of(round_iris * visible * 0.96f);
        const float glance = -gx * 0.05f;
        disc(f, part, at_x(f, ix - size * 0.34f + glance), at_y(f, iy - size * 0.36f), px(f, size * 0.30f), px(f, size * 0.27f), 0xFFFFFF, shine, eye);
        disc(f, part, at_x(f, ix + size * 0.36f + glance), at_y(f, iy + size * 0.38f), px(f, size * 0.13f), px(f, size * 0.13f), 0xEAFBFF, shine * 3U / 4U, eye);
    }
    if (v[LOOK_HEARTS] > 0.02f) {
        const float beat = 1.0f + 0.10f * pulse(fmodf((float)(now - f->asked_at), 900.0f), 0, 90, 30, 260);
        const float r = h * 0.33f * beat * clampf(v[LOOK_HEARTS], 0, 1);
        const int love = shape(f, SHAPE_LOVE_L + side, part, &(ink_form_t){.glyph = INK_HEART, .cx = at_x(f, ix), .cy = at_y(f, iy + 1.5f), .rx = px(f, r),
            .ry = px(f, r * 0.94f), .open = 1});
        fill(f, part, love, 0xFF8FB2, 0xE83E74, alpha_of(v[LOOK_HEARTS] * visible), eye);
        disc(f, part, at_x(f, ix - r * 0.42f), at_y(f, iy - r * 0.32f), px(f, r * 0.20f), px(f, r * 0.15f), 0xFFE6EE, alpha_of(0.9f * v[LOOK_HEARTS] * visible), eye);
    }
    if (v[LOOK_WET] > 0.02f && shows > 0.1f) {
        /* Brimming: a bright rim along the lower lid that trembles. */
        const float tremble = 0.6f * wave((float)now, 900.0f);
        disc(f, part, at_x(f, x + tremble), at_y(f, bottom + 1.0f), px(f, w * 0.40f), px(f, h * 0.20f), 0xEAF8FF, alpha_of(0.75f * v[LOOK_WET] * visible), eye);
        disc(f, part, at_x(f, ix + size * 0.12f), at_y(f, iy + size * 0.55f), px(f, size * 0.42f), px(f, size * 0.22f), 0xD6F2FF, alpha_of(0.55f * v[LOOK_WET] * visible), eye);
    }
    /* The lid's shadow gives the eye its depth. */
    add(f, &(item_t){.kind = ITEM_LID, .part = (uint8_t)part, .shape = (int8_t)eye, .inside = -1, .alpha = (uint16_t)alpha_of((0.30f + 0.9f * fminf(upper, 0.35f)) * visible * shows),
        .box = f->shapes[eye].box, .top = 0x1A2A38, .v = {px(f, 7.0f + 10.0f * fminf(upper, 0.4f))}});
}

static void lay_brow(lcd_face_t *f, unsigned side, const spot_t *eye)
{
    const float *v = f->now;
    const unsigned part = side ? PART_BROW_R : PART_BROW_L;
    const float s = side ? 1.0f : -1.0f;
    const float shown = clampf(v[LOOK_BROW], 0, 1);
    if (shown < 0.02f) return;
    const float raise = v[side ? LOOK_BROW_RAISE_R : LOOK_BROW_RAISE_L] + f->frame.brow;
    const float y = v[LOOK_BROW_Y] - raise + (eye->y - v[LOOK_EYE_Y]) * 1.05f;
    const int brow = shape(f, SHAPE_BROW_L + side, part, &(ink_form_t){.cx = at_x(f, eye->x + s * 1.0f), .cy = at_y(f, y), .rx = px(f, eye->w * 0.27f), .ry = px(f, 2.7f),
        .round = px(f, 2.7f), .shear = px(f, -s * v[LOOK_BROW_TILT] * 0.5f), .bend = px(f, v[LOOK_BROW_BEND]), .open = 1});
    const uint32_t tint = tint_of(f);
    fill(f, part, brow, ink_mix(tint, 0xFFFFFF, 40), ink_mix(tint, 0x8FA9BC, 70), alpha_of(0.92f * shown), -1);
}

static void lay_mouth(lcd_face_t *f)
{
    const lcd_face_frame_t *l = &f->frame;
    const float *v = f->now;
    const unsigned part = PART_MOUTH;
    const float gx = clampf(l->gaze_x + v[LOOK_GAZE_X], -15, 15), gy = clampf(l->gaze_y + v[LOOK_GAZE_Y], -9, 9);
    const float say = l->mouth_open;
    /* A wide mouth opens downwards and draws in; a round one only grows. */
    const float wide = clampf((v[LOOK_MOUTH_W] - 18.0f) / 14.0f, 0, 1);
    const float w = fmaxf(v[LOOK_MOUTH_W] * l->mouth_wide * (1.0f + say * mixf(0.10f, -0.16f, wide)), 8.0f);
    const float h = v[LOOK_MOUTH_H] + say * mixf(9.0f, 21.0f, wide) * clampf(1.45f - v[LOOK_MOUTH_H] / 32.0f, 0.3f, 1.0f);
    const float x = LCD_FACE_WIDTH * 0.5f + v[LOOK_MOUTH_X] + gx * 0.45f + l->sway;
    const float y = v[LOOK_MOUTH_Y] + gy * 0.45f + l->rise + 3.2f * say;
    const uint32_t tint = tint_of(f);
    const float pucker = clampf(v[LOOK_MOUTH_PUCKER], 0, 1);
    /* Lips that come together for a kiss. */
    if (pucker > 0.02f) {
        const float push = 1.0f + 0.10f * pulse(fmodf(age_of(f, LOOK_KISS, f->last_frame_at), 2600.0f), 300, 140, 120, 260);
        const unsigned a = alpha_of(pucker);
        disc(f, part, at_x(f, x), at_y(f, y - 4.2f * push), px(f, 8.0f * push), px(f, 5.4f * push), 0xFF9DB8, a, -1);
        disc(f, part, at_x(f, x), at_y(f, y + 4.6f * push), px(f, 8.8f * push), px(f, 6.0f * push), 0xF77C9E, a, -1);
        line(f, part, x - 4.5f, y + 0.2f, x + 4.5f, y + 0.2f, 1.8f, 0x8E2F4C, a, -1);
        disc(f, part, at_x(f, x - 2.6f), at_y(f, y - 5.8f * push), px(f, 2.2f), px(f, 1.2f), 0xFFE6EE, a * 3U / 4U, -1);
    }
    const unsigned solid = alpha_of(1.0f - pucker);
    if (!solid) return;
    const float flat = clampf(v[LOOK_MOUTH_FLAT], 0, 1) * (1.0f - 0.5f * say);
    const float bend = v[LOOK_MOUTH_BEND] * (1.0f - 0.35f * say);
    ink_form_t lips = {.cx = at_x(f, x), .cy = at_y(f, y), .rx = px(f, w * 0.5f), .ry = px(f, h * 0.5f),
        .round = px(f, fminf(w, h) * 0.5f), .soft = fine(clampf((h - 9.0f) / 10.0f, 0, 1)), .shear = px(f, v[LOOK_MOUTH_SHEAR] * 0.5f),
        .bend = px(f, bend), .wave = px(f, v[LOOK_MOUTH_WAVE]), .flat = fine(flat), .open = 1};
    /* A tongue out of the mouth lies under nothing: it is laid over the lower lip. */
    const float stick = clampf(-v[LOOK_TONGUE], 0, 1);
    const int outer = shape(f, SHAPE_MOUTH, part, &lips);
    const uint32_t lip_top = ink_mix(tint, 0xFFE4EA, 90), lip_bottom = ink_mix(tint, 0xF0AFC0, 110);
    fill(f, part, outer, lip_top, lip_bottom, solid, -1);
    const float inside = ramp(v[LOOK_MOUTH_OPEN] + say, 0.02f, 0.30f);
    const float lip = 2.5f;
    const float ih = h * (1.0f - flat * 0.5f) - 2.0f * lip;
    if (inside > 0.02f && ih > 1.0f && w > 3.0f * lip) {
        ink_form_t cave = lips;
        cave.rx = px(f, w * 0.5f - lip);
        cave.ry = px(f, h * 0.5f - lip);
        cave.cy = at_y(f, y + flat * lip * 0.35f);
        cave.round = px(f, fmaxf(fminf(w, h) * 0.5f - lip, 0.5f));
        cave.bend = px(f, bend * 0.9f);
        const unsigned dark = alpha_of(inside * ramp(ih, 1.0f, 5.0f) * (1.0f - pucker));
        const int hollow = shape(f, SHAPE_HOLLOW, part, &cave);
        if (hollow >= 0) {
            fill(f, part, hollow, 0x2C0F1C, 0x57202F, dark, -1);
            const float tongue = fmaxf(clampf(v[LOOK_TONGUE], 0, 1), 0.7f * ramp(say, 0.15f, 0.5f));
            if (tongue > 0.02f) {
                const float low = y + h * 0.5f - lip - bend * 0.35f;
                const float tr = h * (0.36f + 0.06f * tongue);
                disc(f, part, at_x(f, x), at_y(f, low + tr * (0.46f - 0.20f * tongue)), px(f, w * 0.31f), px(f, tr), 0xE8657F, dark, hollow);
                round_item(f, part, ITEM_SOFT, at_x(f, x - w * 0.06f), at_y(f, low - tr * 0.10f), px(f, w * 0.17f), px(f, tr * 0.5f), 0xFF9FB0, dark * 3U / 4U, hollow);
            }
            if (v[LOOK_TEETH] > 0.02f) {
                const float ty = y - h * 0.5f * (1.0f - flat) + bend * -0.5f + lip * 0.6f;
                line(f, part, x - w * 0.36f, ty, x + w * 0.36f, ty, 6.4f, 0xFDFDFF, dark * alpha_of(v[LOOK_TEETH]) / INK_SOLID, hollow);
            }
        }
    }
    if (stick > 0.02f) {
        const float tx = x + v[LOOK_TONGUE_X], ty = y + h * 0.5f - 1.5f - bend * (0.5f - (v[LOOK_TONGUE_X] / (w * 0.5f)) * (v[LOOK_TONGUE_X] / (w * 0.5f)));
        const float lick = 1.0f + 0.12f * wave((float)f->last_frame_at, 700.0f);
        const float long_ = 13.0f * stick * lick;
        const int tongue = shape(f, SHAPE_TONGUE, part, &(ink_form_t){.cx = at_x(f, tx), .cy = at_y(f, ty + long_ * 0.30f), .rx = px(f, 6.6f), .ry = px(f, long_ * 0.5f),
            .round = px(f, fminf(6.6f, long_ * 0.5f)), .soft = 0.4f, .open = 1});
        fill(f, part, tongue, 0xE8657F, 0xFF93A8, alpha_of(stick), -1);
        line(f, part, tx, ty - 1.0f, tx, ty + long_ * 0.38f, 1.3f, 0xC2435F, alpha_of(stick * 0.8f), tongue);
    }
}

static void lay_cheeks(lcd_face_t *f, const spot_t eyes[2])
{
    const float *v = f->now;
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned part = i ? PART_CHEEK_R : PART_CHEEK_L;
        const float s = i ? 1.0f : -1.0f, x = eyes[i].x + s * 9.0f, y = eyes[i].y + 30.0f - (eyes[i].y - v[LOOK_EYE_Y]) * 0.25f;
        const float a = clampf(v[LOOK_CHEEK], 0, 1);
        if (a > 0.02f) round_item(f, part, ITEM_SOFT, at_x(f, x), at_y(f, y), px(f, 19.0f), px(f, 11.0f), 0xFF6F93, alpha_of(0.62f * a), -1);
        const unsigned hatch = alpha_of(v[LOOK_HATCH] * 0.75f);
        for (int j = -1; hatch && j <= 1; ++j)
            line(f, part, x + (float)j * 6.0f - 1.6f, y + 3.4f, x + (float)j * 6.0f + 1.6f, y - 3.4f, 2.0f, 0xFF8FA9, hatch, -1);
    }
}

static void lay_glasses(lcd_face_t *f, const spot_t eyes[2], int64_t now)
{
    const float shown = clampf(f->now[LOOK_GLASSES], 0, 1);
    if (shown < 0.02f) return;
    const unsigned part = PART_GLASSES, a = alpha_of(ramp(shown, 0.0f, 0.5f));
    const float drop = (1.0f - ramp(age_of(f, LOOK_GLASSES, now), 60, 520)) * -34.0f * shown - (1.0f - shown) * 18.0f;
    line(f, part, eyes[0].x + 24.0f, eyes[0].y - 9.0f + drop, eyes[1].x - 24.0f, eyes[1].y - 9.0f + drop, 5.0f, 0x9DC3E2, a, -1);
    for (unsigned i = 0; i < 2; ++i) {
        const float s = i ? 1.0f : -1.0f;
        ink_form_t frame = {.cx = at_x(f, eyes[i].x), .cy = at_y(f, eyes[i].y + drop), .rx = px(f, 36.0f), .ry = px(f, 21.5f), .round = px(f, 15.0f),
            .soft = 0.25f, .shear = px(f, -s * 1.5f), .open = 1};
        const int rim = shape(f, SHAPE_RIM_L + i, part, &frame);
        fill(f, part, rim, 0xC6E2F8, 0x7FA6C8, a, -1);
        frame.rx = px(f, 33.6f);
        frame.ry = px(f, 19.0f);
        frame.round = px(f, 12.8f);
        const int lens = shape(f, SHAPE_LENS_L + i, part, &frame);
        fill(f, part, lens, 0x33475A, 0x0C151D, a, -1);
        if (lens < 0) continue;
        /* A glint crosses the lenses now and then. */
        const float sweep = fmodf((float)(now - f->asked_at), 3600.0f), at = mixf(-64.0f, 64.0f, ramp(sweep, 700, 1500));
        for (int j = 0; j < 2; ++j)
            line(f, part, eyes[i].x + at + (float)j * 11.0f - 9.0f, eyes[i].y + drop + 26.0f, eyes[i].x + at + (float)j * 11.0f + 9.0f,
                 eyes[i].y + drop - 26.0f, j ? 3.0f : 6.0f, 0xBFE0FA, alpha_of(0.42f * shown), lens);
        line(f, part, eyes[i].x - 22.0f, eyes[i].y + drop - 11.0f, eyes[i].x - 12.0f, eyes[i].y + drop - 13.0f, 2.6f, 0xDCEEFF, alpha_of(0.5f * shown), lens);
    }
}

static void lay_props(lcd_face_t *f, const spot_t eyes[2], int64_t now)
{
    const float *v = f->now;
    const spot_t *right = &eyes[1];
    const float rise = right->y - v[LOOK_EYE_Y];
    float a;
    if ((a = clampf(v[LOOK_TEARS], 0, 1)) > 0.02f) {
        for (unsigned i = 0; i < 2; ++i) for (unsigned k = 0; k < 2; ++k) {
            const unsigned part = i ? PART_TEARS_R : PART_TEARS_L;
            const float t = fmodf(age_of(f, LOOK_TEARS, now) + (float)k * 620.0f + (float)i * 310.0f, 1240.0f) / 1240.0f;
            const float x = eyes[i].x + (i ? 1.0f : -1.0f) * (19.0f + 3.0f * t), y = eyes[i].y + 11.0f + 44.0f * t * t;
            const float size = 1.0f - 0.25f * t;
            const int drop = glyph(f, part, INK_DROP, x, y, 4.6f * size, 7.6f * size);
            fill(f, part, drop, 0xD9F3FF, 0x63BFEF, alpha_of(a * 0.95f * ramp(t, 0.0f, 0.12f) * (1.0f - ramp(t, 0.80f, 1.0f))), -1);
            if (drop >= 0) disc(f, part, at_x(f, x - 1.4f * size), at_y(f, y + 2.0f * size), px(f, 1.0f), px(f, 1.6f), 0xFFFFFF, alpha_of(a * 0.8f * (1.0f - ramp(t, 0.7f, 0.95f))), drop);
        }
    }
    if ((a = clampf(v[LOOK_SWEAT], 0, 1)) > 0.02f) {
        const float t = fmodf(age_of(f, LOOK_SWEAT, now), 2600.0f);
        const float x = right->x + right->w * 0.5f + 5.0f, y = right->y - 22.0f + 13.0f * ramp(t, 250, 1900);
        const int drop = glyph(f, PART_SWEAT, INK_DROP, x, y, 4.8f, 8.2f);
        const unsigned alpha = alpha_of(a * ramp(t, 60, 380) * (1.0f - ramp(t, 1900, 2350)));
        fill(f, PART_SWEAT, drop, 0xDDF5FF, 0x58B7EA, alpha, -1);
        if (drop >= 0) disc(f, PART_SWEAT, at_x(f, x - 1.6f), at_y(f, y + 2.4f), px(f, 1.1f), px(f, 1.8f), 0xFFFFFF, alpha * 3U / 4U, drop);
    }
    if ((a = clampf(v[LOOK_HEART], 0, 1)) > 0.02f) {
        for (unsigned k = 0; k < 3; ++k) {
            const float t = fmodf(age_of(f, LOOK_HEART, now) + (float)k * 900.0f, 2700.0f) / 2700.0f;
            heart(f, PART_HEART, 205.0f + (float)k * 9.0f + 4.0f * sinf(t * 7.0f + (float)k), 34.0f - 27.0f * t, (5.0f + (float)(k % 2U) * 2.4f) * (0.55f + 0.45f * ramp(t, 0, 0.25f)),
                  alpha_of(a * ramp(t, 0.0f, 0.15f) * (1.0f - ramp(t, 0.68f, 0.98f))));
        }
    }
    if ((a = clampf(v[LOOK_KISS], 0, 1)) > 0.02f) {
        const float t = fmodf(age_of(f, LOOK_KISS, now), 2600.0f);
        const float go = ramp(t, 420, 2100);
        heart(f, PART_KISS, 136.0f + 58.0f * go, 95.0f - 40.0f * go + 4.0f * sinf(go * 9.0f), 4.0f + 6.5f * go,
              alpha_of(a * ramp(t, 400, 560) * (1.0f - ramp(t, 1750, 2250))));
    }
    if ((a = clampf(v[LOOK_NOTES], 0, 1)) > 0.02f) {
        for (unsigned k = 0; k < 2; ++k) {
            const float t = fmodf(age_of(f, LOOK_NOTES, now) + (float)k * 1500.0f, 3000.0f) / 3000.0f;
            const float x = 206.0f + (float)k * 16.0f + 4.0f * sinf(t * 8.0f), y = 38.0f - 24.0f * t + rise * 0.3f, size = 0.85f + 0.25f * (float)k;
            const unsigned alpha = alpha_of(a * ramp(t, 0.0f, 0.15f) * (1.0f - ramp(t, 0.72f, 0.98f)));
            const uint32_t rgb = k ? 0xF7D7F1 : 0xFBEBC4;
            disc(f, PART_NOTES, at_x(f, x - 3.0f * size), at_y(f, y + 6.0f * size), px(f, 3.9f * size), px(f, 3.0f * size), rgb, alpha, -1);
            line(f, PART_NOTES, x, y + 5.0f * size, x, y - 7.0f * size, 2.0f, rgb, alpha, -1);
            line(f, PART_NOTES, x, y - 7.0f * size, x + 5.5f * size, y - 4.0f * size, 2.4f, rgb, alpha, -1);
        }
    }
    if ((a = clampf(v[LOOK_ZZZ], 0, 1)) > 0.02f) {
        static const int8_t z[][2] = {{0, 0}, {6, 0}, {0, 7}, {6, 7}};
        for (unsigned k = 0; k < 3; ++k) {
            const float t = fmodf(age_of(f, LOOK_ZZZ, now) + (float)k * 1300.0f, 3900.0f) / 3900.0f;
            path(f, PART_ZZZ, z, 4, 196.0f + 24.0f * t + 3.0f * sinf(t * 6.0f), 33.0f - 30.0f * t, 0.62f + 0.80f * t, 2.3f, 0xD2CEF2,
                 alpha_of(a * ramp(t, 0.0f, 0.14f) * (1.0f - ramp(t, 0.74f, 1.0f))));
        }
    }
    if ((a = clampf(v[LOOK_QUESTION], 0, 1)) > 0.02f) {
        const float t = age_of(f, LOOK_QUESTION, now), pop = ramp(t, 60, 260) + 0.25f * pulse(t, 180, 80, 10, 160);
        const float x = 219.0f + 1.2f * wave(t, 2200.0f), y = 21.0f + 1.4f * wave(t + 500.0f, 1700.0f) + rise * 0.4f, r = 6.6f * pop;
        /* Three quarters of a circle, then down. */
        float lx = x - r, ly = y - 0.5f * r;
        for (unsigned k = 1; k <= 9; ++k) {
            const float turn = 3.64f + (float)k * 0.47f, nx = x + r * cosf(turn), ny = y + r * sinf(turn);
            line(f, PART_QUESTION, lx, ly, nx, ny, 3.5f, 0xB6E2FA, alpha_of(a), -1);
            lx = nx;
            ly = ny;
        }
        line(f, PART_QUESTION, lx, ly, x, y + 11.0f * pop, 3.5f, 0xB6E2FA, alpha_of(a), -1);
        dot(f, PART_QUESTION, x, y + 17.5f * pop, 2.3f, 0xB6E2FA, alpha_of(a));
    }
    if ((a = clampf(v[LOOK_EXCLAIM], 0, 1)) > 0.02f) {
        const float t = age_of(f, LOOK_EXCLAIM, now), pop = ramp(t, 40, 200) + 0.35f * pulse(t, 140, 70, 10, 180);
        const float x = 221.0f + 1.5f * wave(t, 130.0f) * (1.0f - ramp(t, 250, 700)), y = 20.0f + rise * 0.4f;
        line(f, PART_EXCLAIM, x + 1.2f, y - 9.0f * pop, x, y + 4.0f * pop, 5.0f, 0xFFE9A8, alpha_of(a), -1);
        dot(f, PART_EXCLAIM, x - 0.4f, y + 12.0f * pop, 2.7f, 0xFFE9A8, alpha_of(a));
    }
    if ((a = clampf(v[LOOK_SPARKLE], 0, 1)) > 0.02f) {
        static const float spots[][3] = {{217, 22, 10}, {230, 40, 6}, {22, 24, 6}};
        for (unsigned k = 0; k < 3; ++k) {
            const float t = fmodf(age_of(f, LOOK_SPARKLE, now) + (float)k * 700.0f, 2100.0f) / 2100.0f;
            const float twinkle = pulse(t, 0.02f, 0.18f, 0.12f, 0.30f);
            const float r = spots[k][2] * (0.35f + 0.65f * twinkle);
            const int up = glyph(f, PART_SPARKLE, INK_SPARK, spots[k][0], spots[k][1], r, r * 1.25f);
            fill(f, PART_SPARKLE, up, 0xFFF6D6, 0xF6DE97, alpha_of(a * twinkle), -1);
        }
    }
    if ((a = clampf(v[LOOK_VEIN], 0, 1)) > 0.02f) {
        /* Four short bows around a point, throbbing. */
        const float t = fmodf(age_of(f, LOOK_VEIN, now), 900.0f);
        const float size = 1.0f + 0.16f * pulse(t, 0, 80, 40, 240), x = 216.0f, y = 21.0f + rise * 0.4f;
        for (unsigned k = 0; k < 4; ++k) {
            const float sx = k & 1U ? 1.0f : -1.0f, sy = k & 2U ? 1.0f : -1.0f, in = 2.6f * size, out = 9.5f * size;
            line(f, PART_VEIN, x + sx * in, y + sy * out, x + sx * in * 1.45f, y + sy * in * 1.45f, 3.0f, 0xFF8492, alpha_of(a), -1);
            line(f, PART_VEIN, x + sx * in * 1.45f, y + sy * in * 1.45f, x + sx * out, y + sy * in, 3.0f, 0xFF8492, alpha_of(a), -1);
        }
    }
    if ((a = clampf(v[LOOK_THOUGHT], 0, 1)) > 0.02f) {
        const float t = age_of(f, LOOK_THOUGHT, now);
        for (unsigned k = 0; k < 3; ++k) {
            const float p = pulse(fmodf(t + 1500.0f - (float)k * 220.0f, 1500.0f), 0, 260, 80, 420);
            dot(f, PART_THOUGHT, 214.0f + (float)k * 9.5f, 14.0f - 3.5f * p, 2.6f + 1.1f * p, tint_of(f), alpha_of(a * (0.40f + 0.60f * p)));
        }
    }
}

/* ------------------------------------------------------------------- frame */

static void lay_out(lcd_face_t *f, int64_t now)
{
    memcpy(f->part_before, f->part_box, sizeof(f->part_before));
    memcpy(f->part_key_before, f->part_key, sizeof(f->part_key_before));
    for (unsigned i = 0; i < PART_COUNT; ++i) {
        f->part_box[i] = (ink_box_t){0, 0, -1, -1};
        f->part_key[i] = 2166136261U;
    }
    f->item_count = f->prop_count = 0;
    spot_t eyes[2];
    /* Eyes first decide where the rest goes; what lies under them is drawn first by its place in the list. */
    lay_eye(f, 0, now, &eyes[0]);
    lay_eye(f, 1, now, &eyes[1]);
    lay_cheeks(f, eyes);
    lay_brow(f, 0, &eyes[0]);
    lay_brow(f, 1, &eyes[1]);
    lay_mouth(f);
    lay_glasses(f, eyes, now);
    lay_props(f, eyes, now);
}

static size_t damage(lcd_face_t *f, lcd_face_rect_t regions[LCD_FACE_REGIONS])
{
    ink_box_t boxes[PART_COUNT];
    size_t count = 0;
    if (f->clear_all) {
        boxes[count++] = (ink_box_t){0, 0, (int16_t)(f->width - 1), (int16_t)(f->height - 1)};
    } else {
        for (unsigned i = 0; i < PART_COUNT; ++i) {
            if (f->part_key[i] == f->part_key_before[i]) continue;
            const ink_box_t b = ink_join(f->part_box[i], f->part_before[i]);
            if (!ink_empty(&b)) boxes[count++] = b;
        }
    }
    f->clear_all = false;
    /* Boxes that nearly make one are one; then as few as the caller takes. */
    for (;;) {
        size_t best_a = 0, best_b = 0;
        int32_t best = INT32_MAX;
        for (size_t a = 0; a < count; ++a) for (size_t b = a + 1; b < count; ++b) {
            const ink_box_t u = ink_join(boxes[a], boxes[b]);
#define AREA(x) ((int32_t)((x).x1 - (x).x0 + 1) * ((x).y1 - (x).y0 + 1))
            const int32_t more = AREA(u) - AREA(boxes[a]) - AREA(boxes[b]);
#undef AREA
            if (more < best) {
                best = more;
                best_a = a;
                best_b = b;
            }
        }
        if (count < 2 || (count <= LCD_FACE_REGIONS && best > 160)) break;
        boxes[best_a] = ink_join(boxes[best_a], boxes[best_b]);
        boxes[best_b] = boxes[--count];
    }
    for (size_t i = 0; i < count; ++i)
        regions[i] = (lcd_face_rect_t){boxes[i].x0, boxes[i].y0, (int16_t)(boxes[i].x1 - boxes[i].x0 + 1), (int16_t)(boxes[i].y1 - boxes[i].y0 + 1)};
    return count;
}

/* Every number of the expression moves towards its new value, and may go a
 * little beyond it before it rests. */
static void move(lcd_face_t *f, float dt_ms)
{
    const unsigned steps = dt_ms > 45.0f ? 4U : dt_ms > 22.0f ? 2U : 1U;
    const float h = dt_ms / 1000.0f / (float)steps;
    for (unsigned c = 0; c < LOOK_COUNT; ++c) {
        const float to = f->look->value[c];
        if (f->now[c] == to && f->speed[c] == 0.0f) continue;
        const float w = PI2 * lcd_face_look_pace((look_channel_t)c), damping = c >= LOOK_RED ? 1.0f : 0.72f;
        for (unsigned k = 0; k < steps; ++k) {
            f->speed[c] += (w * w * (to - f->now[c]) - 2.0f * damping * w * f->speed[c]) * h;
            f->now[c] += f->speed[c] * h;
        }
        if (fabsf(f->now[c] - to) < 0.002f && fabsf(f->speed[c]) < 0.02f) {
            f->now[c] = to;
            f->speed[c] = 0.0f;
        }
    }
}

static void show(lcd_face_t *f, uint16_t which, int64_t now)
{
    const look_t *next = &lcd_face_looks[which < LCD_FACE_MANNERS ? which : 0];
    f->shown = which;
    if (f->look == next) return;
    for (unsigned i = 0; i < CUES; ++i)
        if (next->value[CUE_FIRST + i] > 0.0f && f->now[CUE_FIRST + i] < 0.05f) f->cue_at[i] = now;
    f->look = next;
    f->asked_at = now;
}

/* What the phase of a conversation makes of the expression asked for. */
static uint16_t look_for(const lcd_face_t *f)
{
    if (f->asked_phase == XIAOTAI_TALK_LISTENING) return LOOK_LISTENING;
    if (f->asked_phase == XIAOTAI_TALK_THINKING) return LOOK_THINKING;
    return f->asked;
}

xiaotai_err_t lcd_face_size(lcd_face_t *f, uint16_t width, uint16_t height)
{
    if (!f || !width || !height) return XIAOTAI_ERR_INVALID_ARG;
    if (f->columns && f->width == width && f->height == height) return XIAOTAI_OK;
    const float by_width = (float)width / LCD_FACE_WIDTH, by_height = (float)height / LCD_FACE_HEIGHT;
    const float scale = by_width < by_height ? by_width : by_height;
    uint16_t first[SHAPES + 1];
    first[0] = 0;
    for (unsigned i = 0; i < SHAPES; ++i) first[i + 1] = (uint16_t)(first[i] + (uint16_t)((float)shape_room[i] * scale) + 3U);
    const size_t room = first[SHAPES];
    ink_column_t *columns = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, room * sizeof(*columns));
    /* One row is worked on pixel by pixel: it is kept where that is quick. */
    uint32_t *row = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, width * sizeof(*row));
    if (!row) row = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, width * sizeof(*row));
    uint16_t *mask = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, width * sizeof(*mask));
    if (!mask) mask = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, width * sizeof(*mask));
    if (!columns || !row || !mask) {
        xiaotai_mem_free(columns);
        xiaotai_mem_free(row);
        xiaotai_mem_free(mask);
        return XIAOTAI_ERR_NO_MEM;
    }
    xiaotai_mem_free(f->columns);
    xiaotai_mem_free(f->line);
    xiaotai_mem_free(f->mask);
    f->columns = columns;
    f->line = row;
    f->mask = mask;
    memcpy(f->first_column, first, sizeof(first));
    memset(f->shapes, 0, sizeof(f->shapes));
    f->width = width;
    f->height = height;
    f->scale = scale;
    f->ox = ((float)width - LCD_FACE_WIDTH * scale) * 0.5f;
    f->oy = ((float)height - LCD_FACE_HEIGHT * scale) * 0.5f;
    f->laid_out = false;
    f->clear_all = true;
    return XIAOTAI_OK;
}

xiaotai_err_t lcd_face_create(uint32_t background_rgb, lcd_face_t **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    lcd_face_t *f = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*f));
    if (!f) return XIAOTAI_ERR_NO_MEM;
    f->background = background_rgb;
    f->frame_ms = LCD_FACE_FRAME_MS;
    f->look = &lcd_face_looks[LOOK_NEUTRAL];
    for (unsigned i = 0; i < PART_COUNT; ++i) f->part_box[i] = (ink_box_t){0, 0, -1, -1};
    atomic_uint *const counters[] = {&f->frames, &f->late, &f->slow, &f->direct, &f->pixels, &f->fallback, &f->step_us, &f->paint_us};
    for (size_t i = 0; i < sizeof(counters) / sizeof(counters[0]); ++i) atomic_init(counters[i], 0);
    const xiaotai_err_t err = lcd_face_size(f, LCD_FACE_WIDTH, LCD_FACE_HEIGHT);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(f);
        return err;
    }
    *out = f;
    return XIAOTAI_OK;
}

void lcd_face_destroy(lcd_face_t *f)
{
    if (!f) return;
    xiaotai_mem_free(f->columns);
    xiaotai_mem_free(f->line);
    xiaotai_mem_free(f->mask);
    xiaotai_mem_free(f);
}

void lcd_face_attach(lcd_face_t *f, uint64_t now_ms)
{
    if (!f) return;
    const int64_t now = (int64_t)now_ms;
    f->attached = true;
    f->visible = f->laid_out = false;
    f->clear_all = true;
    f->senses = (lcd_face_senses_t){0};
    f->frame = (lcd_face_frame_t){.open = {1.0f, 1.0f}, .mouth_wide = 1.0f, .pupil = 1.0f};
    lcd_face_life_start(&f->life, now);
    f->look = &lcd_face_looks[LOOK_NEUTRAL];
    memcpy(f->now, f->look->value, sizeof(f->now));
    memset(f->speed, 0, sizeof(f->speed));
    f->asked_at = now;
    f->asked = f->shown = LOOK_NEUTRAL;
    lcd_face_set(f, "neutral", XIAOTAI_TALK_IDLE, 0, now_ms);
}

void lcd_face_detach(lcd_face_t *f)
{
    if (!f) return;
    f->attached = f->visible = f->laid_out = false;
}

void lcd_face_set_background(lcd_face_t *f, uint32_t background_rgb)
{
    if (!f || f->background == background_rgb) return;
    f->background = background_rgb;
    f->clear_all = true;
}

void lcd_face_set(lcd_face_t *f, const char *name, uint8_t phase, uint8_t level, uint64_t now_ms)
{
    if (!f || !f->attached) return;
    f->asked = (uint16_t)lcd_face_manner(name);
    f->asked_phase = phase;
    f->asked_level = level;
    lcd_face_life_mood(&f->life, (int)f->asked, phase, level, (int64_t)now_ms);
}

void lcd_face_pace(lcd_face_t *f, uint16_t frame_ms)
{
    if (f && frame_ms) f->frame_ms = frame_ms;
}

void lcd_face_sense(lcd_face_t *f, const lcd_face_senses_t *senses)
{
    if (f && senses) f->senses = *senses;
}

bool lcd_face_stroked(const lcd_face_t *f)
{
    return f && lcd_face_life_stroked(&f->life);
}

size_t lcd_face_step(lcd_face_t *f, uint64_t now_ms, bool running, lcd_face_rect_t regions[LCD_FACE_REGIONS])
{
    if (!f || !f->attached || !regions) return 0;
    if (!running) {
        f->visible = false;
        return 0;
    }
    const int64_t now = (int64_t)now_ms;
    const int64_t gap = f->visible ? now - f->last_frame_at : f->frame_ms;
    /* The clock counts whole milliseconds: one due now may be one early. */
    if (f->visible && gap + 1 < f->frame_ms) return 0;
    const int64_t started = xiaotai_time_us();
    if (f->visible && gap > 2 * f->frame_ms) count_add(&f->late, 1);
    f->visible = true;
    f->last_frame_at = now;
    const int mood = lcd_face_life_step(&f->life, now, &f->senses, &f->frame);
    const lcd_face_drift_t slow = f->frame.slow;
    const float far = fmaxf(fmaxf(fabsf(slow.rise - f->drift.rise), fabsf(slow.sway - f->drift.sway)), fabsf(slow.tilt - f->drift.tilt));
    if (!f->laid_out || far * f->scale >= 0.1875f) f->drift = slow;
    f->frame.rise += f->drift.rise;
    f->frame.sway += f->drift.sway;
    f->frame.tilt += f->drift.tilt;
    f->frame.squash += f->drift.squash;
    /* A reaction shows its own expression, then what was asked for returns. */
    const uint16_t wanted = mood != LCD_FACE_MOOD_NONE ? (uint16_t)mood : look_for(f);
    if (wanted != f->shown) show(f, wanted, now);
    move(f, gap > 120 ? 120.0f : (float)gap);
    lay_out(f, now);
    f->laid_out = true;
    const size_t count = damage(f, regions);
    count_add(&f->frames, 1);
    const int64_t took = xiaotai_time_us() - started;
    count_add(&f->step_us, (unsigned)took);
    if (took > 8000) count_add(&f->slow, 1);
    return count;
}

uint32_t lcd_face_delay(const lcd_face_t *f, uint64_t now_ms)
{
    if (!f || !f->attached || !f->visible) return 0;
    const int64_t age = (int64_t)now_ms - f->last_frame_at;
    return age <= 0 ? f->frame_ms : age < f->frame_ms ? f->frame_ms - (uint32_t)age : 1;
}

static void draw_item(lcd_face_t *f, ink_t *ink, const item_t *i)
{
    const ink_shape_t *inside = i->inside >= 0 ? &f->shapes[i->inside] : NULL;
    switch ((item_kind_t)i->kind) {
    case ITEM_SHAPE: ink_shape(ink, &f->shapes[i->shape], i->top, i->bottom, i->alpha, inside); break;
    case ITEM_LID: ink_lid(ink, &f->shapes[i->shape], i->top, i->v[0], i->alpha); break;
    case ITEM_IRIS: ink_iris(ink, i->v[0], i->v[1], i->v[2], i->v[3], f->iris, i->alpha, inside); break;
    case ITEM_DISC: ink_disc(ink, i->v[0], i->v[1], i->v[2], i->v[3], i->top, i->alpha, inside); break;
    case ITEM_SOFT: ink_soft(ink, i->v[0], i->v[1], i->v[2], i->v[3], i->top, i->alpha, inside); break;
    case ITEM_STROKE: ink_stroke(ink, i->v[0], i->v[1], i->v[2], i->v[3], i->v[4], i->top, i->alpha, inside); break;
    default: break;
    }
}

static uint32_t raster(lcd_face_t *f, const xiaotai_ui_surface_t *target, int16_t x, int16_t y)
{
    if (!f || !f->laid_out || !target || !target->pixels || target->pixel != XIAOTAI_PIXEL_RGB565 ||
        !target->width || !target->height || target->stride < (uint32_t)target->width * 2U ||
        (size_t)(target->height - 1U) > (SIZE_MAX - (size_t)target->width * 2U) / target->stride) return 0;
    const int32_t x0 = (int32_t)target->x - x, y0 = (int32_t)target->y - y;
    const int32_t x1 = x0 + target->width - 1, y1 = y0 + target->height - 1;
    const int32_t left = x0 < 0 ? 0 : x0, top = y0 < 0 ? 0 : y0;
    const int32_t right = x1 >= f->width ? f->width - 1 : x1, bottom = y1 >= f->height ? f->height - 1 : y1;
    if (right < left || bottom < top) return 0;
    ink_t ink = {.surface = target, .left = x0, .top = y0, .clip = {(int16_t)left, (int16_t)top, (int16_t)right, (int16_t)bottom},
        .background = f->background, .paper = lv_color_to_u16(lv_color_hex(f->background)), .line = f->line, .mask = f->mask};
    /* Which items lie in this part at all. */
    uint8_t here[ITEMS];
    unsigned count = 0;
    for (unsigned i = 0; i < f->item_count; ++i) {
        const ink_box_t *b = &f->items[i].box;
        if (b->x1 >= left && b->x0 <= right && b->y1 >= top && b->y0 <= bottom) here[count++] = (uint8_t)i;
    }
    for (int32_t row = top; row <= bottom; ++row) {
        ink_row(&ink, (int16_t)row);
        for (unsigned k = 0; k < count; ++k) {
            const item_t *item = &f->items[here[k]];
            if (row >= item->box.y0 && row <= item->box.y1) draw_item(f, &ink, item);
        }
        ink_row_end(&ink);
    }
    return (uint32_t)(right - left + 1) * (uint32_t)(bottom - top + 1);
}

bool lcd_face_paint(void *face, const xiaotai_ui_surface_t *target, int16_t x, int16_t y)
{
    lcd_face_t *f = face;
    const int64_t started = xiaotai_time_us();
    const uint32_t count = raster(f, target, x, y);
    if (!count) return false;
    count_add(&f->direct, 1);
    count_add(&f->pixels, count);
    count_add(&f->paint_us, (unsigned)(xiaotai_time_us() - started));
    return true;
}

bool lcd_face_canvas(lcd_face_t *f, const xiaotai_ui_surface_t *target)
{
    if (!raster(f, target, 0, 0)) return false;
    count_add(&f->fallback, 1);
    return true;
}

uint32_t lcd_face_cost(const lcd_face_t *f)
{
    return atomic_load_explicit(&f->step_us, memory_order_relaxed) + atomic_load_explicit(&f->paint_us, memory_order_relaxed);
}

void lcd_face_report(const lcd_face_t *f, char *line, size_t capacity)
{
    if (!line || !capacity) return;
#define COUNTER(name) (f ? atomic_load_explicit(&f->name, memory_order_relaxed) : 0U)
    (void)snprintf(line, capacity, "face_frames=%u face_late=%u face_slow=%u face_step_us=%u face_us=%u direct=%u direct_pixels=%u fallback=%u",
        COUNTER(frames), COUNTER(late), COUNTER(slow), COUNTER(step_us), COUNTER(paint_us), COUNTER(direct), COUNTER(pixels), COUNTER(fallback));
#undef COUNTER
}
