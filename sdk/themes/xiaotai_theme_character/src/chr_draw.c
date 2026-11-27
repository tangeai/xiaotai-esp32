// SPDX-License-Identifier: MIT
/* How the character is drawn: the pose picks the sprites (which eyes, which
 * mouth, which ears) and says how they move; marks are added as shapes. The
 * list is built back to front and kept until the next step, so painting any
 * part of the stage only walks it. */
#include "chr_internal.h"
#include "chr_sprites.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f

enum {
    BLUSH = 0xFF7F9F, ACCENT = 0x5EC8F2, HEART = 0xFF6F9D, ZED = 0xBFE9FF, NOTE = 0x9BE7FF, SPARK = 0xFFF3B0,
    DROP = 0x9BDCFF, MARK = 0xFFD76A, PUFF = 0xD8DEE6, AMBER = 0xFFC054, GOLD = 0xFFD98A, PINK = 0xFF9EC8, LILAC = 0xC9A8FF,
    /* Capsules run from light above to full below: hers are the colours of the tips of her hair, a call's are green. */
    SKY_LIGHT = 0xD6F3FF, SKY = 0x58B6F8, LEAF_LIGHT = 0xDCFFE8, LEAF = 0x58D88A,
};

typedef struct {
    chr_t *c;
    float ox, oy;          /* stage position of the figure's anchor, plus what moves this part */
    uint8_t group;
} pen_t;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
#define RUNG_LEAST_MS 60.0f        /* lids, and a mouth outside words, stay this long at least on each step */
static float ramp(float t, float from, float to)
{
    if (t <= from) return 0.0f;
    if (t >= to) return 1.0f;
    const float u = (t - from) / (to - from);
    return u * u * (3.0f - 2.0f * u);
}

static uint32_t hash(uint32_t h, int32_t v) { return (h ^ (uint32_t)v) * 16777619U; }

static void grow(chr_group_t *g, int x0, int y0, int x1, int y1)
{
    if (g->x1 < g->x0) { g->x0 = (int16_t)x0; g->y0 = (int16_t)y0; g->x1 = (int16_t)x1; g->y1 = (int16_t)y1; return; }
    if (x0 < g->x0) g->x0 = (int16_t)x0;
    if (y0 < g->y0) g->y0 = (int16_t)y0;
    if (x1 > g->x1) g->x1 = (int16_t)x1;
    if (y1 > g->y1) g->y1 = (int16_t)y1;
}

static void add(pen_t *p, chr_shape_t *s)
{
    chr_t *c = p->c;
    if (!s->alpha || c->item_count >= CHR_ITEMS_MAX || !chr_shape_bounds(s)) return;
    if (s->x1 < 0 || s->y1 < 0 || s->x0 >= (int)c->width || s->y0 >= (int)c->height) return;
    chr_group_t *g = &c->groups[p->group];
    uint32_t h = hash(g->signature, s->kind | s->alpha << 8);
    h = hash(hash(h, (int32_t)s->top), (int32_t)s->bottom);
    for (int i = 0; i < 8; ++i) h = hash(h, (int32_t)(s->a[i] * 2.0f));
    g->signature = h;
    grow(g, s->x0, s->y0, s->x1, s->y1);
    chr_item_t *item = &c->items[c->item_count++];
    item->sprite = 0;
    item->group = p->group;
    item->as.shape = *s;
}

/* The sprite in a slot at its baked place plus (dx, dy), moved as `motion` says and as opaque as it
 * says (fully, when it says nothing). A part this figure does not have draws nothing. */
static void picture(pen_t *p, unsigned id, float dx, float dy, const chr_blit_t *motion)
{
    chr_t *c = p->c;
    chr_sprite_t box;
    if (c->item_count >= CHR_ITEMS_MAX || id == CHR_NO_SPRITE || !chr_pack_sprite(&c->pack, id, &box)) return;
    chr_blit_t b = motion ? *motion : (chr_blit_t){0};
    b.id = (uint16_t)id;
    b.x = (int16_t)floorf(p->ox + (float)box.x + dx + 0.5f);
    b.y = (int16_t)floorf(p->oy + (float)box.y + dy + 0.5f);
    b.alpha = motion && motion->alpha ? motion->alpha : 32;
    b.grey = (uint8_t)(clampf(c->out[P_GREY], 0, 1) * 32.0f);
    if (!chr_blit_bounds(&c->pack, &b)) return;
    if (b.x1 < 0 || b.y1 < 0 || b.x0 >= (int)c->width || b.y0 >= (int)c->height) return;
    chr_group_t *g = &c->groups[p->group];
    uint32_t h = hash(g->signature, (int32_t)(id | (uint32_t)b.grey << 16));
    h = hash(hash(h, b.x), b.y);
    h = hash(hash(h, (int32_t)(b.tilt * 1024.0f)), (int32_t)(b.sway * 2.0f));
    h = hash(hash(h, (int32_t)(b.turn * 1024.0f)), (int32_t)b.tilt_y);
    h = hash(hash(h, (int32_t)(b.grow_x * 8192.0f)), (int32_t)(b.grow_y * 8192.0f));
    h = hash(hash(h, (int32_t)b.pin_x), (int32_t)b.pin_y | (int32_t)b.alpha << 16);
    h = hash(h, (int32_t)b.patch);
    g->signature = h;
    grow(g, b.x0, b.y0, b.x1, b.y1);
    chr_item_t *item = &c->items[c->item_count++];
    item->sprite = 1;
    item->group = p->group;
    item->as.blit = b;
}

static void sprite(pen_t *p, unsigned slot, float dx, float dy, const chr_blit_t *motion)
{
    picture(p, chr_pack_slot(&p->c->pack, slot), dx, dy, motion);
}

static void ellipse(pen_t *p, float x, float y, float rx, float ry, uint32_t rgb, int alpha)
{
    chr_shape_t s = {.kind = CHR_ELLIPSE, .alpha = (uint8_t)clampf((float)alpha, 0, 32), .top = rgb, .bottom = rgb,
        .a = {p->ox + x, p->oy + y, rx, ry}};
    add(p, &s);
}

static void disc(pen_t *p, float x, float y, float r, uint32_t rgb, int alpha) { ellipse(p, x, y, r, r, rgb, alpha); }

static void line(pen_t *p, float x0, float y0, float x1, float y1, float r, uint32_t rgb, int alpha)
{
    chr_shape_t s = {.kind = CHR_STADIUM, .alpha = (uint8_t)clampf((float)alpha, 0, 32), .top = rgb, .bottom = rgb,
        .a = {p->ox + x0, p->oy + y0, p->ox + x1, p->oy + y1, r}};
    add(p, &s);
}

/* An upright capsule around (x, y), `tall` pixels between the middles of its round ends. */
static void capsule(pen_t *p, float x, float y, float tall, float r, uint32_t top, uint32_t bottom, int alpha)
{
    chr_shape_t s = {.kind = CHR_STADIUM, .alpha = (uint8_t)clampf((float)alpha, 0, 32), .top = top, .bottom = bottom,
        .a = {p->ox + x, p->oy + y - 0.5f * tall, p->ox + x, p->oy + y + 0.5f * tall, r}};
    add(p, &s);
}

static void polygon(pen_t *p, const float *xy, int points, uint32_t rgb, int alpha)
{
    chr_shape_t s = {.kind = CHR_POLYGON, .alpha = (uint8_t)clampf((float)alpha, 0, 32), .points = (uint8_t)points,
        .top = rgb, .bottom = rgb};
    for (int i = 0; i < points; ++i) {
        s.a[2 * i] = p->ox + xy[2 * i];
        s.a[2 * i + 1] = p->oy + xy[2 * i + 1];
    }
    add(p, &s);
}

static void arc(pen_t *p, float x, float y, float r, float from, float to, float thick, uint32_t rgb, int alpha)
{
    enum { PARTS = 9 };
    float px = x + r * cosf(from), py = y + r * sinf(from);
    for (int i = 1; i <= PARTS; ++i) {
        const float a = from + (to - from) * (float)i / (float)PARTS;
        const float nx = x + r * cosf(a), ny = y + r * sinf(a);
        line(p, px, py, nx, ny, thick, rgb, alpha);
        px = nx; py = ny;
    }
}

static void ring(pen_t *p, float x, float y, float r, float thick, uint32_t rgb, int alpha);
static void light(pen_t *p, float x, float y, float r, uint32_t rgb, int alpha);

/* ------------------------------------------------------------------ figure */

/* The picture a value selects; it only changes once the value is clearly past a threshold. */
static uint8_t choose(float value, const float *thresholds, int count, uint8_t now)
{
    int pick = 0;
    while (pick < count && value >= thresholds[pick]) ++pick;
    if (pick > (int)now && value < thresholds[pick - 1] + 0.03f) --pick;
    else if (pick < (int)now && value >= thresholds[pick] - 0.03f) ++pick;
    return (uint8_t)pick;
}

static void select_pictures(chr_t *c)
{
    const float *o = c->out;
    static const float open_steps[3] = {0.2f, 0.5f, 0.82f}, thirds[2] = {-0.36f, 0.36f}, forms[2] = {-0.3f, 0.45f};
    static const float flops[2] = {-0.55f, 0.55f}, raised[3] = {0.17f, 0.5f, 0.83f}, turns[2] = {-0.4f, 0.4f};
    for (int i = 0; i < 2; ++i) {
        /* Pictures: 0 open, 1 half, 2 nearly shut, 3 shut in a smile, 4 shut. */
        if (o[P_ARC] > (c->eye[i] == 3 ? 0.4f : 0.5f)) { c->eye[i] = 3; continue; }
        const uint8_t was = c->eye[i] == 4 ? 0 : c->eye[i] == 3 ? 3 : (uint8_t)(3 - c->eye[i]);
        const uint8_t level = choose(o[i ? P_EOR : P_EOL], open_steps, 3, was);
        c->eye[i] = o[P_ARC] < -0.45f || level == 0 ? 4 : (uint8_t)(3 - level);
    }
    /* A flat figure looks at you: her eyes do not wander to either side. */
    c->gaze = c->features & CHR_FLAT ? 1 : choose(o[P_GX], thirds, 2, c->gaze);
    c->mouth_form = choose(o[P_MC], forms, 2, c->mouth_form);
    const float wanted = o[P_MO] * 4.0f;
#if CHR_MARKS
    const uint8_t mouth_was = c->mouth_open;
#endif
    c->mouth_ms = minf(c->mouth_ms + c->step_ms, 10000.0f);
    if (fabsf(wanted - (float)c->mouth_open) > 0.6f) {
        const uint8_t to = (uint8_t)clampf(floorf(wanted + 0.5f), 0, 4);
        /* A flat figure's mouth outside words goes shut, small, medium, wide one step at a time, a moment on each. */
        if (!(c->features & CHR_FLAT) || c->voice_step || c->mouth > 0.01f || c->word_shut) c->mouth_open = to;
        else if (c->mouth_ms >= RUNG_LEAST_MS || c->face_cut) {
            c->mouth_open = c->face_cut ? to : (uint8_t)(to > c->mouth_open ? c->mouth_open + 1U : c->mouth_open - 1U);
            c->mouth_ms = 0.0f;
        }
    }
    if (c->voice_step || !c->mouth_open) c->word_shut = false;
#if CHR_MARKS
    if (c->mouth_open != mouth_was) CHR_MARK("MO %u", (unsigned)c->mouth_open);
#endif
    /* 0 folded inwards, 1 upright, 2 flopped outwards. */
    c->ear[0] = choose(o[P_EAR_L], flops, 2, c->ear[0]);
    c->ear[1] = choose(o[P_EAR_R], flops, 2, c->ear[1]);
    c->arm[0] = choose(o[P_ARM_L], raised, 3, c->arm[0]);
    c->arm[1] = choose(o[P_ARM_R], raised, 3, c->arm[1]);
    /* 0 turned to the viewer's left, 1 facing, 2 to the right. */
    c->turn = c->features & CHR_HAS_YAW ? choose(o[P_YAW], turns, 2, c->turn) : 1;
}

static unsigned eye_sprite(unsigned base, uint8_t state, uint8_t gaze)
{
    return state < 2 ? base + state * 3U + gaze : base + 6U + (state - 2U);
}

/* The picture of an open eye with another look; CHR_NO_SPRITE for a plain one. Eyes that comfort, and eyes with
 * tears, are soft ones in a pack that has none of their own. */
static unsigned look_slot(const chr_t *c, uint8_t look, int right)
{
    static const uint16_t pairs[] = {[LOOK_SOFT] = SL_EYE_L_SOFT, [LOOK_BRIGHT] = SL_EYE_L_BRIGHT,
        [LOOK_COMFORT] = SL_EYE_L_COMFORT, [LOOK_SERIOUS] = SL_EYE_L_SERIOUS, [LOOK_GLAD] = SL_EYE_L_GLAD,
        [LOOK_DOWN] = SL_EYE_L_DOWN, [LOOK_TEARY] = SL_EYE_L_TEARY};
    if (look >= sizeof(pairs) / sizeof(pairs[0]) || !pairs[look]) return CHR_NO_SPRITE;
    unsigned slot = (unsigned)pairs[look] + (unsigned)right;
    if (chr_pack_slot(&c->pack, slot) == CHR_NO_SPRITE && (look == LOOK_COMFORT || look == LOOK_TEARY)) slot = SL_EYE_L_SOFT + (unsigned)right;
    return chr_pack_slot(&c->pack, slot) == CHR_NO_SPRITE ? CHR_NO_SPRITE : slot;
}

static void figure(pen_t *p)
{
    chr_t *c = p->c;
    const chr_figure_t *f = &c->fig;
    const float *o = c->out;
    const float ax = c->anchor_x, ay = c->anchor_y;
    select_pictures(c);
    float hx, hy;
    chr_head_offset(c, &hx, &hy);
    /* The body leans a little with the head; the head stands on its shoulders. */
    const float body_tilt = o[P_LEAN] * 0.2f / (float)-f->neck_y;
    hx += body_tilt * (float)-f->neck_y;
    const chr_blit_t head = {.tilt = o[P_LEAN] / (float)(f->neck_y - f->hair_top), .tilt_y = ay + hy + f->neck_y};
    const chr_blit_t body = {.tilt = body_tilt, .tilt_y = ay + o[P_BY]};
    /* The pictures of the head as it is turned, and how far the face moved sideways in them. */
    const unsigned block = SL_HEAD + (c->turn == 1 ? 0U : c->turn == 0 ? 1U : 2U) * H_SLOTS;
    const float turned = ((float)c->turn - 1.0f) * (float)f->yaw_shift;

    p->ox = ax; p->oy = ay;
    if (c->features & CHR_HAS_TAIL) {
        p->group = G_TAIL;
        /* The tail is a large picture and every new position redraws all of it, so it is moved ten
         * times a second when calm and twenty when she is excited, whatever the frame rate. */
        if ((c->tail_in -= c->step_ms) <= 0.0f) {
            c->tail_sway = sinf(c->wag_phase) * 20.0f * o[P_WAG] + c->swing * 0.4f;
            c->tail_in = o[P_WAG] < 0.6f ? 100.0f : 50.0f;
        }
        const chr_blit_t tail = {.sway = c->tail_sway, .sway_from = ay + o[P_BY] - 5, .sway_to = ay + o[P_BY] + f->tail_top};
        sprite(p, SL_TAIL, o[P_BX], o[P_BY], &tail);
    }
    for (int i = 0; i < 2 && (c->features & CHR_HAS_ARMS); ++i) {
        /* An arm is one of a few pictures; its sleeve swings a little after the body, in whole pixels
         * so that a still arm is not redrawn. */
        p->group = i ? G_ARM_R : G_ARM_L;
        chr_blit_t arm = body;
        arm.sway = floorf(c->swing * 0.3f + 0.5f);
        arm.sway_from = ay + o[P_BY] + f->arm_top;
        arm.sway_to = ay + o[P_BY] - 5;
        sprite(p, (i ? (unsigned)SL_ARM_R : (unsigned)SL_ARM_L) + c->arm[i], o[P_BX], o[P_BY], &arm);
    }
    for (int i = 0; i < 2 && (c->features & CHR_HAS_EARS); ++i) {
        p->group = i ? G_EAR_R : G_EAR_L;
        /* Left ear: outwards is to the right. The upright picture bends for small moves. */
        const float value = o[i ? P_EAR_R : P_EAR_L], out = i ? -1.0f : 1.0f;
        const uint8_t state = c->ear[i];
        const float bend = state == 1 ? value * 16.0f : (value - (state == 2 ? 1.0f : -1.0f)) * 12.0f;
        chr_blit_t ear = head;
        ear.sway = out * bend - c->swing * 0.25f;
        ear.sway_from = ay + hy + f->ear_base_y;
        ear.sway_to = ay + hy + f->ears_top;
        sprite(p, i ? (unsigned)SL_EAR_R + (2U - state) : (unsigned)SL_EAR_L + state, hx, hy, &ear);
    }
    p->group = G_HEAD;
    {
        chr_blit_t hair = head;
        hair.sway = c->swing * 0.6f;
        hair.sway_from = ay + hy + f->hair_top + 40;
        hair.sway_to = ay + hy + f->neck_y + 10;
        sprite(p, block + H_HAIR_BACK, hx, hy, &hair);
    }

    p->group = G_BODY;
    sprite(p, SL_BODY, o[P_BX], o[P_BY], &body);

    p->group = G_HEAD;
    sprite(p, block + H_FACE, hx, hy, &head);
    p->group = G_FACE;
    sprite(p, block + H_MOUTH + c->mouth_open * 3U + c->mouth_form, hx, hy, &head);
    sprite(p, eye_sprite(block + H_EYE_L, c->eye[0], c->gaze), hx, hy, &head);
    sprite(p, eye_sprite(block + H_EYE_R, c->eye[1], c->gaze), hx, hy, &head);
    {
        const int blush = (int)(15.0f * o[P_BLUSH]);
        const float lean = head.tilt * (float)(f->neck_y - f->cheek_y) + turned;
        ellipse(p, (float)(f->eye_lx + f->cheek_dx) + hx + lean, f->cheek_y + hy, f->blush_rx, f->blush_ry, BLUSH, blush);
        ellipse(p, (float)(f->eye_rx - f->cheek_dx) + hx + lean, f->cheek_y + hy, f->blush_rx, f->blush_ry, BLUSH, blush);
    }
    p->group = G_HEAD;
    {
        chr_blit_t hair = head;
        hair.sway = c->swing * 0.35f;
        hair.sway_from = ay + hy + f->brow_y - 10;
        hair.sway_to = ay + hy + f->neck_y - 8;
        sprite(p, block + H_HAIR_FRONT, hx, hy, &hair);
    }
    p->group = G_FACE;
    for (int i = 0; i < 2; ++i) {
        /* A brow slants about its middle: the inner end sinks when she is cross. */
        chr_blit_t brow = head;
        brow.turn = (i ? 1.0f : -1.0f) * o[P_SLANT] * 0.2f;
        brow.turn_x = ax + hx + turned + (float)(i ? f->brow_rx : f->brow_lx);
        sprite(p, block + (i ? (unsigned)H_BROW_R : (unsigned)H_BROW_L), hx, hy - o[P_BROW], &brow);
    }
}

/* ------------------------------------------------------------- a flat face */

/* A flat figure's face changes one part at a time: each of her eyes, her brows and her mouth goes from its picture to
 * the next by fading over FADE_MS (the patches lie exactly on the base picture, so a fade is a slow change and not two
 * faces). Lids are another matter: they shut and open one rung at a time (shut, half, open; open, glad, smiling), at
 * least RUNG_LEAST_MS on each, which is how she blinks; and a mouth that speaks moves at once. */
#define FADE_MS 160.0f

static bool has(const chr_t *c, unsigned slot) { return chr_pack_slot(&c->pack, slot) != CHR_NO_SPRITE; }

/* A part goes to another picture, at once or by fading. */
static void part_to(chr_t *c, unsigned part, unsigned sprite, bool cut)
{
    if (c->face_cut) cut = true;
    if (sprite == c->part_now[part]) {
        if (cut) c->part_ms[part] = FADE_MS;
        return;
    }
    if (!cut && c->part_ms[part] < FADE_MS) {
        /* A change in the middle of a fade: back to where it came from turns the fade round; anything else fades
         * from whichever of the two shows more by now. */
        if (sprite == c->part_was[part]) {
            c->part_was[part] = c->part_now[part];
            c->part_now[part] = (uint16_t)sprite;
            c->part_ms[part] = FADE_MS - c->part_ms[part];
            return;
        }
        if (c->part_ms[part] >= 0.5f * FADE_MS) c->part_was[part] = c->part_now[part];
        c->part_now[part] = (uint16_t)sprite;
        c->part_ms[part] = 0.0f;
        return;
    }
    c->part_was[part] = cut ? (uint16_t)CHR_NO_SPRITE : c->part_now[part];
    c->part_now[part] = (uint16_t)sprite;
    c->part_ms[part] = cut ? FADE_MS : 0.0f;
}

static void part_draw(pen_t *p, unsigned part, const chr_blit_t *motion)
{
    const chr_t *c = p->c;
    const float u = clampf(c->part_ms[part] / FADE_MS, 0.0f, 1.0f);
    chr_blit_t m = *motion;
    if (u < 1.0f && c->part_was[part] != CHR_NO_SPRITE) {
        m.alpha = (uint8_t)((float)motion->alpha * (1.0f - u) + 0.5f);
        if (m.alpha) picture(p, c->part_was[part], 0, 0, &m);
    }
    if (c->part_now[part] != CHR_NO_SPRITE) {
        m.alpha = (uint8_t)((float)motion->alpha * u + 0.5f);
        if (m.alpha) picture(p, c->part_now[part], 0, 0, &m);
    }
}

/* The rung an eye is asked to be on. A smile goes as far as glad eyes where the pack has them: shut, smiling eyes are
 * her pictures' business, of a laugh behind her hand. */
static uint8_t rung_wanted(const chr_t *c, int i)
{
    switch (c->eye[i]) {
    case 0: return RUNG_OPEN;
    case 3: return has(c, i ? SL_EYE_R_GLAD : SL_EYE_L_GLAD) ? (uint8_t)RUNG_GLAD : (uint8_t)RUNG_SMILE;
    case 4: return RUNG_SHUT;
    default: return RUNG_HALF;
    }
}

static unsigned eye_picture(const chr_t *c, int i)
{
    const unsigned base = SL_HEAD + (i ? (unsigned)H_EYE_R : (unsigned)H_EYE_L);
    unsigned slot = base + 1U;      /* open, looking ahead: in most packs the base picture's own */
    switch (c->rung[i]) {
    case RUNG_SHUT: slot = base + 8U; break;
    case RUNG_HALF: slot = base + 4U; break;
    case RUNG_GLAD: slot = i ? (unsigned)SL_EYE_R_GLAD : (unsigned)SL_EYE_L_GLAD; break;
    case RUNG_SMILE: slot = base + 7U; break;
    default:
        /* Lowered to one side, or another look; she never looks up (the whites under the eyes would show), nor opens
         * them wide. */
        if (c->ponder && has(c, i ? SL_EYE_R_PONDER : SL_EYE_L_PONDER)) slot = i ? (unsigned)SL_EYE_R_PONDER : (unsigned)SL_EYE_L_PONDER;
        else if (c->eyes_shown != LOOK_PLAIN && look_slot(c, c->eyes_shown, i) != CHR_NO_SPRITE) slot = look_slot(c, c->eyes_shown, i);
        break;
    }
    return chr_pack_slot(&c->pack, slot);
}

static unsigned mouth_picture(const chr_t *c)
{
    /* Wider than a word is a laugh (a smile), a round mouth (a pout) or the widest a word goes. */
    unsigned open = c->mouth_open, form = c->mouth_form;
    if (open >= 3U) open = form == 1U ? 2U : 4U;
    unsigned slot = SL_HEAD + H_MOUTH + open * 3U + form;
    if (!c->mouth_open) {
        static const uint16_t rests[] = {[LOOK_COMFORT] = SL_MOUTH_COMFORT, [LOOK_SERIOUS] = SL_MOUTH_SERIOUS,
                                         [LOOK_SMIRK] = SL_MOUTH_SMIRK, [LOOK_FROWN] = SL_MOUTH_FROWN,
                                         [LOOK_SMILE] = SL_HEAD + H_MOUTH + 2U};
        const unsigned rest = c->mouth_rest < sizeof(rests) / sizeof(rests[0]) ? rests[c->mouth_rest] : 0U;
        if (rest && (has(c, rest) || c->mouth_rest == LOOK_SMILE)) slot = rest;
    }
    return chr_pack_slot(&c->pack, slot);
}

static unsigned brows_picture(const chr_t *c)
{
    const float *o = c->out;
    unsigned slot = CHR_NO_SPRITE;
    if (c->brows_look == LOOK_COMFORT && has(c, SL_BROWS_COMFORT)) slot = SL_BROWS_COMFORT;
    else if (c->brows_look == LOOK_SERIOUS && has(c, SL_BROWS_SERIOUS)) slot = SL_BROWS_SERIOUS;
    else if (o[P_SLANT] > 0.45f) slot = SL_BROWS + 1U;
    else if (o[P_SLANT] < -0.45f) slot = SL_BROWS + 2U;
    else if (o[P_BROW] > 4.5f) slot = SL_BROWS;
    return slot == CHR_NO_SPRITE ? (unsigned)CHR_NO_SPRITE : chr_pack_slot(&c->pack, slot);
}

/* Where each part of the face is going, for the time that has passed. A mood that has just changed reaches her mouth
 * a little after her eyes and brows; words reach it at once. */
static void face_step(chr_t *c)
{
    const float dt = c->step_ms;
    for (int part = 0; part < PART_COUNT; ++part) c->part_ms[part] = minf(c->part_ms[part] + dt, FADE_MS);
    for (int i = 0; i < 2; ++i) {
        const uint8_t was = c->rung[i], wanted = rung_wanted(c, i);
        c->rung_ms[i] = minf(c->rung_ms[i] + dt, 10000.0f);
        /* Lids that are asked to go further while they wait on a rung go that far: a blink, quicker than three rungs,
         * still shuts them. */
        const int ahead = (int)wanted - (int)was, planned = (int)c->aim[i] - (int)was;
        if (ahead && (!planned || (ahead > 0) != (planned > 0) || abs(ahead) > abs(planned))) {
            if (!planned || (ahead > 0) == (planned > 0)) c->aim[i] = wanted;
        }
        if (c->face_cut) c->rung[i] = c->aim[i] = wanted;
        else if (c->aim[i] != was && c->rung_ms[i] >= RUNG_LEAST_MS) {
            /* Smiling shut and shut are a step apart. A pack without glad eyes goes from open to smiling. */
            const int aim = c->aim[i];
            int next = (was == RUNG_SMILE && aim == RUNG_SHUT) || (was == RUNG_SHUT && aim == RUNG_SMILE) ? aim : (int)was + (aim > was ? 1 : -1);
            if (next == RUNG_GLAD && !has(c, i ? SL_EYE_R_GLAD : SL_EYE_L_GLAD)) next = aim > was ? RUNG_SMILE : RUNG_OPEN;
            c->rung[i] = (uint8_t)next;
            if (next == aim) c->aim[i] = wanted == next ? (uint8_t)next : wanted;
        }
        if (c->rung[i] != was) c->rung_ms[i] = 0.0f;
        part_to(c, PART_EYE_L + (unsigned)i, eye_picture(c, i), c->rung[i] != was && c->rung[i] <= RUNG_OPEN && was <= RUNG_OPEN);
    }
    part_to(c, PART_BROWS, brows_picture(c), false);
    /* In a talk a word opens and shuts her mouth at once; a mood changes how it rests by fading, after her eyes. */
    const bool word = (c->phase != CHR_PHASE_IDLE || c->parting) && c->mouth_open != c->mouth_drawn;
    if (c->mouth_lag <= 0.0f || c->face_cut) c->mouth_rest = c->mouth_look;        /* (a word shuts it as it was) */
    if (c->mouth_lag > 0.0f && !word && !c->face_cut) c->mouth_lag -= dt;
    else {
        part_to(c, PART_MOUTH, mouth_picture(c), word);
        c->mouth_drawn = c->mouth_open;
    }
    c->face_cut = false;
}

/* One picture of all of a flat figure, as opaque as `motion` says: a pose picture (a sprite, or a
 * sprite that is a patch over the sprite `under`), which has its own face, or the base picture
 * (CHR_NO_SPRITE) with her eyes, mouth and brows as patches over it. */
static void flat_picture(pen_t *p, unsigned pose, unsigned under, const chr_blit_t *motion)
{
    chr_t *c = p->c;
    const chr_figure_t *f = &c->fig;
    const float *o = c->out;
    p->group = G_BODY;
    if (pose != CHR_NO_SPRITE && under != CHR_NO_SPRITE) {
        /* A picture that is a patch: the one under it, with the patch in place of what differs. */
        chr_blit_t patched = *motion;
        patched.patch = (uint16_t)(pose + 1U);
        picture(p, under, 0, 0, &patched);
        return;
    }
    if (pose != CHR_NO_SPRITE) {
        picture(p, pose, 0, 0, motion);
        return;
    }
    sprite(p, SL_BODY, 0, 0, motion);

    p->group = G_FACE;
    /* Whole-face pictures, in a pack that has nothing better: crying, and the puffed cheeks of someone cross who says
     * nothing. With eyes of tears and lowered ones she does both with her eyes and mouth (face_step()). */
    const bool puffed = o[P_SLANT] > 0.75f && o[P_MC] < -0.5f && c->mouth_open == 0;
    if (c->crying && !has(c, SL_EYE_L_TEARY)) sprite(p, SL_FACE + 1U, 0, 0, motion);
    else if (puffed && !has(c, SL_EYE_L_DOWN)) sprite(p, SL_FACE, 0, 0, motion);
    else {
        part_draw(p, PART_MOUTH, motion);
        part_draw(p, PART_EYE_L, motion);
        part_draw(p, PART_EYE_R, motion);
        part_draw(p, PART_BROWS, motion);
    }
    {
        /* Her cheeks are where the growth puts them. */
        const int blush = (int)(15.0f * o[P_BLUSH]) * motion->alpha / 32;
        const float y = (float)f->cheek_y * (1.0f + motion->grow_y), wide = 1.0f + motion->grow_x;
        ellipse(p, (float)(f->eye_lx + f->cheek_dx) * wide, y, f->blush_rx, f->blush_ry, BLUSH, blush);
        ellipse(p, (float)(f->eye_rx - f->cheek_dx) * wide, y, f->blush_rx, f->blush_ry, BLUSH, blush);
    }
}

/* A flat figure: a picture of all of her that is never taken apart or bent. She moves as one piece:
 * from side to side and up and down, and by growing about her feet. What would sink or lift a head
 * makes all of her a little shorter or taller, which is how she breathes; a squash also spreads her
 * and a stretch narrows her. All of it stays within a few pixels (chr_flat_x and its like): a large
 * picture that moves further rocks. Her height changes in whole pixels, so breathing redraws her a
 * few times a second and not on every frame. A change of picture is covered by shut eyes and a small
 * dip (chr_change_shape), never by showing two pictures at once. */
static void flat_figure(pen_t *p)
{
    chr_t *c = p->c;
    select_pictures(c);
    face_step(c);
    const float tall = (float)-c->fig.hair_top;
    float grow_y = tall > 0.0f ? floorf(chr_flat_tall(c) + 0.5f) / tall : 0.0f;
    const float past = (grow_y < 0.0f ? -grow_y : grow_y) - 0.012f;
    float grow_x = past > 0.0f ? (grow_y < 0.0f ? 0.5f : -0.5f) * past : 0.0f;
    float change_x, change_y, lift;
    chr_change_shape(c, &change_x, &change_y, &lift);
    grow_y += change_y;
    grow_x = floorf((grow_x + change_x) * 512.0f + 0.5f) / 512.0f;
    p->ox = c->anchor_x + floorf(chr_flat_x(c) + 0.5f);
    p->oy = c->anchor_y + floorf(chr_flat_y(c) - lift + 0.5f);
    c->moved_x = (int16_t)(p->ox - c->anchor_x);
    c->moved_feet = (int16_t)(p->oy - c->anchor_y);
    c->moved_top = (int16_t)floorf((float)c->moved_feet - grow_y * tall + 0.5f);
    const chr_blit_t motion = {.alpha = 32, .grow_x = grow_x, .grow_y = grow_y, .pin_x = p->ox, .pin_y = p->oy};
    flat_picture(p, c->pose ? c->picture : (unsigned)CHR_NO_SPRITE, c->pose ? c->picture_under : (unsigned)CHR_NO_SPRITE, &motion);
}

/* ----------------------------------------------------------------- effects */

_Static_assert((int)FX_KINDS == (int)CHR_FX_KINDS, "an effect for each run of the pack's slots for them");

/* The size (chr_emit()) at which a shape is as large as the picture of its kind was baked. */
static const float shape_size[FX_PUFF] = {[FX_HEART] = 17, [FX_ZED] = 17, [FX_NOTE] = 22, [FX_SPARK] = 14, [FX_DROP] = 14,
                                           [FX_QUESTION] = 34, [FX_EXCLAIM] = 35};

/* Pictures of a few frames that are played once over the particle's life; the frames of the others are variants. */
static bool plays(chr_fx_t kind)
{
    return kind == FX_SPARK || kind == FX_SPARK_SKY || kind == FX_GLINT || kind == FX_RING_GOLD || kind == FX_RING_SKY ||
           kind == FX_BURST;
}

chr_particle_t *chr_spark(chr_t *c, chr_fx_t kind, float x, float y, float delay, float life, float size)
{
    /* Kinds whose picture a pack may leave out (they would stay in memory) take a near one's. */
    static const struct { uint8_t kind, like; } borrowed[] = {
        {FX_SPARK_SKY, FX_SPARK}, {FX_HEART_SMALL, FX_HEART}, {FX_NOTE2, FX_NOTE}, {FX_ROSE, FX_PETAL},
    };
    for (size_t i = 0; i < sizeof(borrowed) / sizeof(borrowed[0]); ++i)
        if (kind == borrowed[i].kind && !chr_fx_drawn(c, kind) && chr_fx_drawn(c, (chr_fx_t)borrowed[i].like)) kind = (chr_fx_t)borrowed[i].like;
    chr_particle_t *slot = NULL;
    for (int i = 0; i < CHR_PARTICLES_MAX; ++i) {
        chr_particle_t *q = &c->particles[i];
        if (!q->live) { slot = q; break; }
        if (!slot || q->age / q->life > slot->age / slot->life) slot = q;
    }
    const unsigned frames = c->fx_frames[kind];
    *slot = (chr_particle_t){.kind = (uint8_t)kind, .live = 1, .x = x, .y = y, .age = -delay, .life = life > 1.0f ? life : 1.0f,
        .size = size, .animated = frames > 1U && plays(kind), .frame = frames > 1U ? (uint8_t)(chr_random(c) % frames) : 0,
        .phase = chr_uniform(c, 0, 6.28f), .fade_in = 80.0f, .fade_out = 300.0f};
    return slot;
}

void chr_emit(chr_t *c, chr_fx_t kind, float x, float y, float vx, float vy, float size, float life)
{
    const float scale = kind < FX_PUFF && chr_fx_drawn(c, kind) ? size / shape_size[kind] : size;
    chr_particle_t *q = chr_spark(c, kind, x, y, 0.0f, life, scale);
    q->vx = vx;
    q->vy = vy;
    q->sway = kind == FX_DROP ? 0.0f : 6.0f;
    q->fall = kind == FX_DROP ? 260.0f : 0.0f;
    q->drag = kind == FX_DROP ? 0.0f : 1.2f;
    /* In over its first eighth, out over its last two fifths. */
    q->fade_in = 0.12f * life;
    q->fade_out = 0.4f * life;
}

void chr_effects_step(chr_t *c, float dt)
{
    for (int i = 0; i < CHR_PARTICLES_MAX; ++i) {
        chr_particle_t *q = &c->particles[i];
        if (!q->live) continue;
        q->age += dt;
        if (q->age >= q->life) { q->live = 0; continue; }
        if (q->age <= 0.0f) continue;
        const float s = dt * 0.001f;
        q->x += q->vx * s;
        q->y += q->vy * s;
        q->vy += q->fall * s;
        q->vx *= 1.0f - clampf(s * q->drag, 0, 1);
    }
}

/* Particles are grouped by where they are: either side of her face, above or below her chin. */
static uint8_t particle_group(const chr_t *c, float x, float y)
{
    const bool left = x < (float)c->fig.face_x, low = y > (float)c->fig.neck_y;
    return (uint8_t)(low ? (left ? G_FX_DL : G_FX_DR) : (left ? G_FX_UL : G_FX_UR));
}

/* Nothing flies across her face: a particle fades as it comes within 20 pixels of it, and is gone on it (a ring around
 * it is not). 1: clear of her face. */
static float off_face(const chr_t *c, float x, float y)
{
    const chr_figure_t *f = &c->fig;
    const float out_x = fabsf(x - (float)f->face_x) - (float)f->head_half;
    const float out_y = maxf((float)f->brow_y - 30.0f - y, y - (float)f->neck_y - 10.0f);
    return ramp(maxf(out_x, out_y), 0.0f, 20.0f);
}

static void particle(pen_t *p, const chr_particle_t *q)
{
    chr_t *c = p->c;
    if (q->age < 0.0f) return;
    const float t = q->age / q->life, s = q->size;
    const float x = q->x + q->sway * sinf(q->phase + q->age * 0.0035f), y = q->y;
    const unsigned frames = c->fx_frames[q->kind];
    const bool around = !frames || q->kind == FX_AURA || q->kind == FX_RING_GOLD || q->kind == FX_RING_SKY;
    const float below = q->kind == FX_AURA ? 1.0f : ramp(y - chr_mark_top(c), 0.0f, 24.0f);
    const float fade = ramp(q->age, 0.0f, q->fade_in) * (1.0f - ramp(q->age, q->life - q->fade_out, q->life)) *
                       (around ? 1.0f : off_face(c, x, y)) * below;
    /* (The glow behind her is a large picture: it fades in eight steps, not on every frame.) */
    const int alpha = q->kind == FX_AURA ? 4 * (int)(8.0f * fade + 0.5f) : (int)(32.0f * fade);
    if (alpha <= 0) return;
    p->group = q->kind == FX_AURA ? (uint8_t)G_AURA : particle_group(c, x, y);
    if (frames) {
        unsigned frame = q->animated ? (unsigned)(t * (float)frames) : q->frame;
        if (frame >= frames) frame = frames - 1U;
        /* A picture grows about its middle; in steps, so that one that has stopped growing is not redrawn. */
        const float size = s * (1.0f - q->pop * (1.0f - ramp(q->age, 0.0f, 150.0f)));
        const float grow = floorf((size - 1.0f) * 64.0f + 0.5f) / 64.0f;
        const chr_blit_t motion = {.alpha = (uint8_t)(alpha > 32 ? 32 : alpha), .grow_x = grow, .grow_y = grow,
                                   .pin_x = floorf(p->ox + x + 0.5f), .pin_y = floorf(p->oy + y + 0.5f)};
        sprite(p, SL_FX + (unsigned)q->kind * CHR_FX_FRAMES + frame, x, y, &motion);
        return;
    }
    switch (q->kind) {
    case FX_HEART: {
        const float tip[6] = {x - 0.95f * s, y - 0.02f * s, x + 0.95f * s, y - 0.02f * s, x, y + 1.02f * s};
        disc(p, x - 0.47f * s, y - 0.26f * s, 0.53f * s, HEART, alpha);
        disc(p, x + 0.47f * s, y - 0.26f * s, 0.53f * s, HEART, alpha);
        polygon(p, tip, 3, HEART, alpha);
        break;
    }
    case FX_ZED: {
        const float g = s * (0.7f + 0.6f * t), r = g * 0.15f;
        line(p, x - g * 0.5f, y - g * 0.6f, x + g * 0.5f, y - g * 0.6f, r, ZED, alpha);
        line(p, x + g * 0.5f, y - g * 0.6f, x - g * 0.5f, y + g * 0.6f, r, ZED, alpha);
        line(p, x - g * 0.5f, y + g * 0.6f, x + g * 0.5f, y + g * 0.6f, r, ZED, alpha);
        break;
    }
    case FX_NOTE:
        ellipse(p, x, y, 0.42f * s, 0.32f * s, NOTE, alpha);
        line(p, x + 0.36f * s, y, x + 0.36f * s, y - 1.25f * s, 0.07f * s, NOTE, alpha);
        line(p, x + 0.36f * s, y - 1.25f * s, x + 0.9f * s, y - 0.98f * s, 0.11f * s, NOTE, alpha);
        break;
    case FX_SPARK: {
        const float g = s * (0.6f + 0.6f * sinf(t * PI));
        const float tall[8] = {x, y - g, x + 0.24f * g, y, x, y + g, x - 0.24f * g, y};
        const float wide[8] = {x - g, y, x, y - 0.24f * g, x + g, y, x, y + 0.24f * g};
        polygon(p, tall, 4, SPARK, alpha);
        polygon(p, wide, 4, SPARK, alpha);
        break;
    }
    case FX_DROP: {
        const float tip[6] = {x - 0.44f * s, y + 0.1f * s, x + 0.44f * s, y + 0.1f * s, x, y - 0.95f * s};
        disc(p, x, y + 0.2f * s, 0.48f * s, DROP, alpha);
        polygon(p, tip, 3, DROP, alpha);
        break;
    }
    case FX_QUESTION:
        arc(p, x, y - 0.5f * s, 0.34f * s, -PI * 1.05f, PI * 0.42f, 0.11f * s, MARK, alpha);
        line(p, x + 0.1f * s, y - 0.17f * s, x, y + 0.1f * s, 0.11f * s, MARK, alpha);
        disc(p, x, y + 0.48f * s, 0.14f * s, MARK, alpha);
        break;
    case FX_EXCLAIM:
        line(p, x, y - 0.85f * s, x, y + 0.05f * s, 0.14f * s, MARK, alpha);
        disc(p, x, y + 0.48f * s, 0.15f * s, MARK, alpha);
        break;
    case FX_PUFF:
        disc(p, x, y, s * (0.5f + 0.5f * t), PUFF, alpha * 2 / 3);
        disc(p, x + 0.6f * s, y + 0.2f * s, s * (0.35f + 0.4f * t), PUFF, alpha * 2 / 3);
        break;
    /* Drawn rather than kept as pictures, which would stay in memory: a ring that spreads and thins, a burst of rays,
     * a soft light with a bright middle, and the glow behind her (`size`: its radius in pixels). */
    case FX_RING_GOLD:
    case FX_RING_SKY: {
        const uint32_t rgb = q->kind == FX_RING_GOLD ? GOLD : SKY_LIGHT;
        const float r = s * (34.0f + 46.0f * ramp(t, 0.0f, 1.0f));
        ring(p, x, y, r, 7.0f * s, rgb, alpha / 4);
        ring(p, x, y, r, 2.2f * s * (1.0f - 0.5f * t), rgb, alpha);
        break;
    }
    case FX_BURST:
        for (int k = 0; k < 10; ++k) {
            const float a = (float)k * (2.0f * PI / 10.0f) + 0.3f, near = s * (10.0f + 44.0f * t), far = s * (24.0f + 60.0f * t);
            line(p, x + near * cosf(a), y + near * sinf(a), x + far * cosf(a), y + far * sinf(a), 1.8f * s, GOLD, alpha);
        }
        break;
    case FX_ORB_GOLD:
    case FX_ORB_PINK:
    case FX_ORB_SKY:
    case FX_ORB_LILAC: {
        static const uint32_t lights[4] = {GOLD, PINK, SKY_LIGHT, LILAC};
        light(p, x, y, 16.0f * s, lights[q->kind - FX_ORB_GOLD], alpha);
        break;
    }
    case FX_AURA: {
        const chr_shape_t outer = {.kind = CHR_GLOW, .alpha = (uint8_t)(alpha * 3 / 8), .top = SKY, .bottom = SKY, .a = {p->ox + x, p->oy + y, s, s}};
        const chr_shape_t inner = {.kind = CHR_GLOW, .alpha = (uint8_t)(alpha / 2), .top = GOLD, .bottom = GOLD,
                                   .a = {p->ox + x, p->oy + y - 0.15f * s, 0.55f * s, 0.55f * s}};
        chr_shape_t shape = outer;
        add(p, &shape);
        shape = inner;
        add(p, &shape);
        break;
    }
    default: break;
    }
}

/* A circle of short strokes. */
static void ring(pen_t *p, float x, float y, float r, float thick, uint32_t rgb, int alpha)
{
    enum { PARTS = 28 };
    if (alpha <= 0 || thick < 0.5f) return;
    for (int i = 0; i < PARTS; ++i) {
        const float a0 = (float)i * (2.0f * PI / PARTS), a1 = a0 + 2.0f * PI / PARTS;
        line(p, x + r * cosf(a0), y + r * sinf(a0), x + r * cosf(a1), y + r * sinf(a1), thick, rgb, alpha);
    }
}

/* A soft light: a glow that fades to its edge, and a bright middle. */
static void light(pen_t *p, float x, float y, float r, uint32_t rgb, int alpha)
{
    if (alpha <= 0) return;
    chr_shape_t glow = {.kind = CHR_GLOW, .alpha = (uint8_t)(alpha * 3 / 4), .top = rgb, .bottom = rgb, .a = {p->ox + x, p->oy + y, r, r}};
    add(p, &glow);
    disc(p, x, y, 0.28f * r, 0xFFFFF4, alpha);
}

/* A voice, on both sides of her head: capsules that stand up with it, lower and fainter outwards. It runs
 * inwards, to her. While there is none they are a row of dots, which keeps still. */
enum { VOICE_BARS = 5, VOICE_STEP_MS = 60 /* as often as a wave has to be drawn to look like one */ };
static void voice(pen_t *p, float y, float loud, float in, uint32_t light, uint32_t full)
{
    static const float share[VOICE_BARS] = {0.50f, 0.86f, 1.00f, 0.68f, 0.42f};
    static const float strength[VOICE_BARS] = {1.00f, 0.86f, 0.70f, 0.53f, 0.36f};
    const chr_t *c = p->c;
    const float clock = floorf((float)fmod(c->now, 3600000.0) / (float)VOICE_STEP_MS) * (float)VOICE_STEP_MS;
    const float level = clampf((loud - 0.15f) / 0.7f, 0, 1);
    const uint8_t group = p->group;
    for (int s = 0; s < 2; ++s) {
        p->group = s ? G_FX : G_FX_OTHER;
        for (int i = 0; i < VOICE_BARS; ++i) {
            const float wave = 0.5f + 0.5f * sinf(clock * 0.012f + (float)i * 1.15f);
            const float tall = 2.0f * floorf(0.5f * level * share[i] * (28.0f + 16.0f * wave) + 0.5f);
            const float out = (float)(c->fig.head_half + 40) + 13.0f * (float)i;
            capsule(p, (float)c->fig.face_x + (s ? out : -out), y, tall, 3.2f, light, full, (int)(in * 30.0f * strength[i]));
        }
    }
    p->group = group;
}

static void light_at(pen_t *p, float x, float y, float r, int alpha) { light(p, x, y, r, GOLD, alpha); }

/* Marks that belong to a state rather than to a moment. */
static void overlays(pen_t *p)
{
    chr_t *c = p->c;
    const float now = (float)fmod(c->now, 3600000.0);
    const chr_figure_t *f = &c->fig;
    const float side = chr_side_x(c);
    float hx, hy;
    chr_head_offset(c, &hx, &hy);
    if (c->phase == CHR_PHASE_LISTENING || (c->conditions & (CHR_RINGING | CHR_IN_CALL))) {
        /* Where her ears are: on top of the head, or at its sides. */
        const float ear_y = (float)(c->features & CHR_HAS_EARS ? f->hair_top + 13 : f->eye_y - 4);
        const bool call = (c->conditions & (CHR_RINGING | CHR_IN_CALL)) != 0;
        voice(p, ear_y, c->conditions & CHR_RINGING ? 0.5f + 0.5f * sinf(now * 0.009f) : c->listen,
              call ? 1.0f : ramp(c->phase_age, 100, 400), call ? LEAF_LIGHT : SKY_LIGHT, call ? LEAF : SKY);
    }
    if (c->phase == CHR_PHASE_THINKING) {
        const float in = ramp(c->phase_age, 100, 400);
        for (int i = 0; i < 3; ++i) {
            const float hop = sinf(now * 0.0075f - (float)i * 1.1f), lit = hop > 0 ? hop : 0;
            if (c->features & CHR_FLAT) {
                /* Three gold lights beside her head that light up one after the other, in steps of an eighth. */
                const float x = side + CHR_SIDE * (8.0f + 28.0f * (float)i) + hx, y = chr_mark_top(c) + 14.0f + hy;
                const float grow = -0.45f + 0.05f * (float)i;
                const chr_blit_t light = {.alpha = (uint8_t)(4.0f * floorf(in * (3.0f + 5.0f * lit) + 0.5f)), .grow_x = grow, .grow_y = grow,
                                          .pin_x = floorf(p->ox + x + 0.5f), .pin_y = floorf(p->oy + y + 0.5f)};
                if (light.alpha && chr_fx_drawn(c, FX_ORB_GOLD)) sprite(p, SL_FX + FX_ORB_GOLD * CHR_FX_FRAMES, x, y, &light);
                else if (light.alpha) {
                    const uint8_t group = p->group;
                    light_at(p, x, y, 9.0f, light.alpha);
                    p->group = group;
                }
                continue;
            }
            disc(p, side + CHR_SIDE * (6.0f + 25.0f * (float)i) + hx, chr_mark_top(c) + 20.0f - 10.0f * lit + hy, 6.0f + 1.6f * (float)i, AMBER,
                 (int)(in * (16.0f + 14.0f * lit)));
        }
    }
    if (c->phase == CHR_PHASE_IDLE && !c->parting && (c->conditions & CHR_CONNECTING) &&
        !(c->conditions & (CHR_ALARM | CHR_RINGING | CHR_IN_CALL))) {
        const int lit = (int)(now / 380.0f) % 4;
        for (int i = 0; i < 3; ++i)
            arc(p, side + CHR_SIDE * 28.0f, chr_mark_top(c) + 44.0f, 13.0f + 13.0f * (float)i, -PI * 0.78f, -PI * 0.22f, 2.8f, ACCENT, i < lit ? 30 : 7);
        disc(p, side + CHR_SIDE * 28.0f, chr_mark_top(c) + 44.0f, 4.0f, ACCENT, 30);
    }
    if ((c->conditions & CHR_MUTED) && !(c->features & CHR_HAS_EARS) && !c->pose &&
        !(c->conditions & (CHR_ALARM | CHR_RINGING | CHR_IN_CALL))) {
        /* The microphone is off: a small cross beside her mouth (a figure with ears lets them droop). */
        const float x = (float)f->face_x + 0.45f * (float)f->head_half + hx, y = (float)f->mouth_y + hy, r = 6.0f;
        line(p, x - r, y - r, x + r, y + r, 2.2f, MARK, 28);
        line(p, x + r, y - r, x - r, y + r, 2.2f, MARK, 28);
    }
    if (c->cue == CHR_CUE_VOLUME) {
        const float in = ramp(c->cue_age, 0, 150) * (1.0f - ramp(c->cue_age, c->cue_len - 300, c->cue_len));
        const int bars = c->cue_value <= 0 ? 0 : c->cue_value < 34 ? 1 : c->cue_value < 67 ? 2 : 3;
        /* Three steps beside her mouth, lit up to the volume; a cross where there is none. */
        const float x = (float)(f->face_x + f->head_half + 40) + hx, foot = (float)f->mouth_y + 10.0f + hy;
        for (int i = 0; i < 3; ++i) {
            const float tall = 8.0f + 9.0f * (float)i;
            capsule(p, x + 14.0f * (float)i, foot - 0.5f * tall, tall, 3.6f, SKY_LIGHT, SKY,
                    (int)(in * (i < bars ? 30.0f : bars ? 7.0f : 4.0f)));
        }
        if (!bars) {
            line(p, x + 2, foot - 24, x + 26, foot, 2.8f, MARK, (int)(in * 30.0f));
            line(p, x + 26, foot - 24, x + 2, foot, 2.8f, MARK, (int)(in * 30.0f));
        }
    }
    if (c->conditions & CHR_ALARM) {
        const float beat = 0.5f + 0.5f * sinf(now * 0.0125f);
        for (int i = 0; i < 2; ++i) {
            const float s = i ? 1.0f : -1.0f;
            for (int k = 0; k < 2; ++k)
                arc(p, (float)f->face_x + s * (float)(f->head_half + 33) + hx, f->brow_y + 6 + hy, 18.0f + 15.0f * (float)k + 4.0f * beat, i ? -0.7f : PI - 0.7f,
                    i ? 0.7f : PI + 0.7f, 2.8f, MARK, (int)(10.0f + 20.0f * beat));
        }
    }
}

void chr_build(chr_t *c)
{
    pen_t pen = {.c = c, .ox = c->anchor_x, .oy = c->anchor_y};
    c->item_count = 0;
    for (int g = 0; g < G_COUNT; ++g) c->groups[g] = (chr_group_t){.x0 = 0, .x1 = -1, .signature = 2166136261U};
    for (int i = 0; i < CHR_PARTICLES_MAX; ++i)
        if (c->particles[i].live && c->particles[i].kind == FX_AURA) particle(&pen, &c->particles[i]);
    if (c->features & CHR_FLAT) flat_figure(&pen);
    else figure(&pen);
    pen.ox = c->anchor_x;
    pen.oy = c->anchor_y;
    pen.group = G_FX;
    overlays(&pen);
    for (int i = 0; i < CHR_PARTICLES_MAX; ++i)
        if (c->particles[i].live && c->particles[i].kind != FX_AURA) particle(&pen, &c->particles[i]);
}

uint32_t chr_paint(const chr_t *c, const chr_target_t *target)
{
    if (!c || !target || !c->built) return 0;
    uint32_t written = 0;
    for (unsigned i = 0; i < c->item_count; ++i) {
        const chr_item_t *item = &c->items[i];
        written += item->sprite ? chr_blit_draw(target, &c->pack, &item->as.blit) : chr_shape_fill(target, &item->as.shape);
    }
    return written;
}
