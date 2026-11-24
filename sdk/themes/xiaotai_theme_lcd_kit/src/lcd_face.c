// SPDX-License-Identifier: MIT
#include "lcd_face.h"

#include "lvgl.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/theme_lcd.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lcd_face_model_data.inc"

enum { FACE_BOUNDS = 96, FACE_PALETTES = 20, FACE_CUES = FACE_POSE_VALUES - FACE_TEARS };
typedef struct { int16_t x0, y0, x1, y1; } face_bounds_t;
typedef struct { uint32_t rgb; uint16_t words[17]; } face_palette_t;

_Static_assert(FACE_POSE_VALUES == 55, "shared face pose format");
_Static_assert(sizeof(face_poses) / sizeof(*face_poses) == XIAOTAI_THEME_LCD_EMOTION_COUNT, "face catalog");

struct lcd_face {
    /* The display task owns animation and its published frame. Diagnostics
     * are the only calls allowed to read this from the application owner. */
    int32_t from[FACE_POSE_VALUES], target[FACE_POSE_VALUES], painted[FACE_POSE_VALUES];
    int64_t cue_at[FACE_CUES], painted_cue_at[FACE_CUES];
    int64_t transition_at, motion_at, blink_at, gaze_at, gaze_next, last_frame_at, painted_at;
    int32_t gaze_from_x, gaze_from_y, gaze_to_x, gaze_to_y, activity;
    face_bounds_t bounds[FACE_BOUNDS], damage[LCD_FACE_REGIONS];
    face_palette_t palettes[FACE_PALETTES];
    atomic_uint late, slow, direct, pixels, fallback;
    uint32_t random;
    uint16_t background, painted_background, transition_ms, pose, motion_pose;
    uint8_t phase, bound_count, damage_mask, palette_count, activity_target, variant;
    bool attached, visible, valid, painted_valid, clear_all;
};

typedef struct {
    lcd_face_t *face;
    /* An absent surface means the geometry-only pass that publishes damage. */
    const xiaotai_ui_surface_t *surface;
    face_bounds_t clip;
    int32_t target_x, target_y;
    const uint16_t *palette;
    uint16_t overflow[17];
    uint32_t colour;
} face_raster_t;

/* Counters have one writer. Avoid atomic read-modify-write on the BULK
 * allocation while keeping diagnostics race-free on the application task. */
static void count_add(atomic_uint *counter, unsigned delta)
{
    atomic_store_explicit(counter, atomic_load_explicit(counter, memory_order_relaxed) + delta, memory_order_relaxed);
}

const char *xiaotai_theme_lcd_emotion_name(size_t index)
{
    return index < XIAOTAI_THEME_LCD_EMOTION_COUNT ? face_poses[index].key : NULL;
}

bool lcd_face_has_alternate(size_t emotion)
{
    return emotion < XIAOTAI_THEME_LCD_EMOTION_COUNT && face_alternate_poses[emotion].key != NULL;
}

static uint32_t face_random(lcd_face_t *f)
{
    uint32_t x = f->random;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return f->random = x;
}
static int32_t face_ease(int64_t elapsed, uint32_t duration)
{
    if (elapsed <= 0) return 0;
    if (elapsed >= duration) return 1024;
    return (int32_t)lv_bezier3((uint32_t)(elapsed * 1024 / duration), 0, 0, 1024, 1024);
}
static int32_t face_lerp(int32_t a, int32_t b, int32_t t)
{
    return a + (b - a) * t / 1024;
}
static int32_t face_wave(int64_t elapsed, uint32_t period)
{
    return lv_trigo_sin((int16_t)(elapsed % period * 360 / period));
}
static int32_t face_pulse(int64_t age, uint32_t rise, uint32_t hold, uint32_t fall)
{
    if (age <= 0) return 0;
    if (age < rise) return face_ease(age, rise);
    age -= rise;
    if (age < hold) return 1024;
    return 1024 - face_ease(age - hold, fall);
}
static int64_t face_cue_age(const lcd_face_t *f, unsigned channel, int64_t now)
{
    int64_t age = now - f->cue_at[channel - FACE_TEARS];
    return age > 0 ? age : 0;
}
static void face_motion(const lcd_face_t *f, int64_t now, int32_t *offset)
{
    memset(offset, 0, FACE_MOTION_VALUES * sizeof(*offset));
    const face_track_t *track = &face_sequences[f->motion_pose].variant[f->variant];
    int64_t age = now - f->motion_at;
    if (age <= 0 || age >= track->end_ms) return;
    const face_keyframe_t *previous = NULL;
    for (unsigned step = 0; step <= FACE_MOTION_STEPS; ++step) {
        const face_keyframe_t *next = step < FACE_MOTION_STEPS ? &track->frame[step] : NULL;
        uint16_t from_ms = previous ? previous->at_ms : 0;
        uint16_t to_ms = next ? next->at_ms : track->end_ms;
        if (age <= to_ms) {
            int32_t t = face_ease(age - from_ms, to_ms - from_ms);
            for (unsigned i = 0; i < FACE_MOTION_VALUES; ++i)
                offset[i] = face_lerp(previous ? previous->value[i] * 256 : 0,
                                         next ? next->value[i] * 256 : 0, t);
            return;
        }
        previous = next;
    }
}
static void face_base(const lcd_face_t *f, int64_t now, int32_t *pose)
{
    int32_t motion[FACE_MOTION_VALUES];
    face_motion(f, now, motion);
    for (size_t i = 0; i < FACE_POSE_VALUES; ++i) {
        int32_t offset = 0;
        switch (i) {
        case FACE_GAZE_X: offset = motion[MOTION_LOOK_X]; break;
        case FACE_GAZE_Y: offset = motion[MOTION_LOOK_Y]; break;
        case 3: case 10: offset = motion[MOTION_EYE_HEIGHT]; break;
        case 6: case 13: offset = motion[MOTION_EYE_BEND]; break;
        case 16: offset = motion[MOTION_MOUTH_WIDTH]; break;
        case 17: offset = motion[MOTION_MOUTH_HEIGHT]; break;
        case 15: offset = motion[MOTION_MOUTH_Y]; break;
        case FACE_MOUTH_TILT: offset = motion[MOTION_MOUTH_TILT]; break;
        case FACE_LEAN: offset = motion[MOTION_LEAN]; break;
        default: break;
        }
        int32_t target = f->target[i] + offset;
        if ((i == 3 || i == 10) && target < 20 * 256) target = 20 * 256;
        if (i == 17 && target < 10 * 256) target = 10 * 256;
        /* Blend toward the animated endpoint. Retargeting captures this exact
         * pose, so changing phase never adds the old motion offset twice. */
        int delay = i >= 14 && i < 21 ? 70 : 0;
        int32_t t = face_ease(now - f->transition_at - delay, f->transition_ms);
        pose[i] = face_lerp(f->from[i], target, t);
    }
}

static const face_pose_t *face_pose_for_variant(size_t index, unsigned variant)
{
    return variant && lcd_face_has_alternate(index) ? &face_alternate_poses[index] : &face_poses[index];
}

void lcd_face_set(lcd_face_t *f, const char *name, uint8_t phase, uint8_t level, uint64_t now_ms)
{
    if (!f || !f->attached) return;
    const int64_t now = (int64_t)now_ms;
    const uint16_t index = (uint16_t)lcd_face_resolve(name);
    const uint16_t display_index = phase == XIAOTAI_TALK_LISTENING ? (uint16_t)lcd_face_resolve("listening") :
                                   phase == XIAOTAI_TALK_THINKING ? (uint16_t)lcd_face_resolve("thinking") : index;
    bool phase_changed = f->phase != phase;
    if (phase_changed) f->gaze_next = now;
    f->activity_target = level > 3U ? 3U : level;
    if (f->valid && f->pose == index && !phase_changed) return;
    bool expression_changed = !f->valid || f->motion_pose != display_index;
    /* Capture the old set/clip before drawing a new random choice. */
    if (f->valid) face_base(f, now, f->from);
    if (expression_changed) {
        unsigned count = lcd_face_has_alternate(display_index) ? FACE_VARIANTS : 1U;
        f->variant = count > 1U ? (uint8_t)(face_random(f) % count) : 0;
        f->motion_at = now;
    }
    const face_pose_t *mood = face_pose_for_variant(index, f->variant);
    const face_pose_t *display = face_pose_for_variant(display_index, f->variant);
    bool attending = phase == XIAOTAI_TALK_LISTENING || phase == XIAOTAI_TALK_THINKING;
    for (size_t i = 0; i < FACE_POSE_VALUES; ++i) {
        f->target[i] = mood->value[i] * 16;
        if (attending) {
            /* A clear phase face with a trace of the mood; no unrelated props. */
            bool silhouette = i < FACE_TEARS || (i >= FACE_RED && i <= FACE_BLUE);
            f->target[i] = silhouette ? face_lerp(display->value[i] * 16,
                                                          f->target[i], 256) : display->value[i] * 16;
        }
    }
    if (attending) {
        f->target[FACE_GAZE_X] = display->value[FACE_GAZE_X] * 16;
        f->target[FACE_GAZE_Y] = display->value[FACE_GAZE_Y] * 16;
        f->target[FACE_LEAN] = display->value[FACE_LEAN] * 16;
    }
    if (phase == XIAOTAI_TALK_THINKING) f->target[FACE_THOUGHT] = 65536;
    if (phase == XIAOTAI_TALK_LISTENING) f->target[FACE_LISTEN] = 65536;
    f->target[FACE_ATTENTION] = phase == XIAOTAI_TALK_LISTENING ? 65536 : 0;
    f->target[FACE_SPEECH] = phase == XIAOTAI_TALK_SPEAKING ? 65536 : 0;
    for (unsigned i = FACE_TEARS; i < FACE_POSE_VALUES; ++i) {
        /* Entry cues follow the new expression, even if the old cue is fading.
         * Activity updates and phases that keep this face do not replay entry. */
        if (!f->valid || (f->target[i] != 0 &&
                              (expression_changed || f->from[i] == 0)))
            f->cue_at[i - FACE_TEARS] = now;
    }
    if (!f->valid) memcpy(f->from, f->target, sizeof(f->from));
    f->phase = phase;
    f->transition_at = now;
    f->transition_ms = display->transition_ms;
    f->pose = index;
    f->motion_pose = display_index;
    f->valid = true;
}

static int64_t face_render_cue_age(const face_raster_t *r, unsigned channel, int64_t now)
{
    const int64_t age = now - r->face->painted_cue_at[channel - FACE_TEARS];
    return age > 0 ? age : 0;
}

static uint16_t pack_rgb(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

static uint16_t swap16(uint16_t pix)
{
    return (uint16_t)(pix << 8 | pix >> 8);
}

static void face_ink(face_raster_t *r, uint32_t rgb)
{
    if (r->palette && r->colour == rgb) return;
    lcd_face_t *f = r->face;
    r->colour = rgb;
    for (unsigned i = 0; i < f->palette_count; ++i) {
        if (f->palettes[i].rgb == rgb) {
            r->palette = f->palettes[i].words;
            return;
        }
    }
    uint16_t *words = r->overflow;
    if (!r->surface && f->palette_count < FACE_PALETTES) {
        face_palette_t *entry = &f->palettes[f->palette_count++];
        entry->rgb = rgb;
        words = entry->words;
    }
    const uint16_t ink = pack_rgb(rgb);
    for (unsigned i = 0; i <= 16; ++i)
        words[i] = lv_color_16_16_mix(ink, f->painted_background, (lv_opa_t)(i * 255U / 16U));
    r->palette = words;
}

/* Assign each original contour to one of the two eye or central damage
 * rectangles; merging the old and new contours prevents trails. */
static void face_damage(lcd_face_t *f, const face_bounds_t *b)
{
    const int centre = (b->x0 + b->x1) / 2;
    const unsigned region = centre < 90 ? 0U : centre > 130 ? 1U : 2U;
    face_bounds_t *d = &f->damage[region];
    if (!(f->damage_mask & (1U << region))) {
        *d = *b;
        f->damage_mask |= (uint8_t)(1U << region);
        return;
    }
    if (b->x0 < d->x0) d->x0 = b->x0;
    if (b->y0 < d->y0) d->y0 = b->y0;
    if (b->x1 > d->x1) d->x1 = b->x1;
    if (b->y1 > d->y1) d->y1 = b->y1;
}

static uint8_t *face_pixel_at(const face_raster_t *r, int x, int y)
{
    return r->surface->pixels + (size_t)(y - r->target_y) * r->surface->stride + 2U * (size_t)(x - r->target_x);
}

static void face_clear(face_raster_t *r)
{
    lcd_face_t *f = r->face;
    if (r->surface) {
        const uint16_t bg = r->surface->swap_bytes ? swap16(f->painted_background) : f->painted_background;
        for (int y = r->clip.y0; y <= r->clip.y1; ++y) {
            uint8_t *p = face_pixel_at(r, r->clip.x0, y);
            for (int x = r->clip.x0; x <= r->clip.x1; ++x, p += 2) memcpy(p, &bg, sizeof(bg));
        }
        return;
    }
    f->damage_mask = 0;
    if (f->clear_all) {
        const face_bounds_t all = {0, 0, LCD_FACE_WIDTH - 1, LCD_FACE_HEIGHT - 1};
        face_damage(f, &all);
    } else {
        for (unsigned i = 0; i < f->bound_count; ++i) face_damage(f, &f->bounds[i]);
    }
    f->clear_all = false;
    f->bound_count = 0;
}

static void face_record_bounds(face_raster_t *r, const face_bounds_t *bounds)
{
    if (r->surface) return;
    lcd_face_t *f = r->face;
    face_damage(f, bounds);
    if (f->bound_count < FACE_BOUNDS) f->bounds[f->bound_count++] = *bounds;
    else f->clear_all = true;
}

static void face_pixel(face_raster_t *r, int x, int y, unsigned coverage, bool overlay)
{
    if (!r->surface || !coverage || x < r->clip.x0 || x > r->clip.x1 || y < r->clip.y0 || y > r->clip.y1) return;
    if (coverage > 16U) coverage = 16U;
    uint8_t *p = face_pixel_at(r, x, y);
    uint16_t current;
    memcpy(&current, p, sizeof(current));
    if (r->surface->swap_bytes) current = swap16(current);
    uint16_t value = r->palette[coverage];
    if (overlay) {
        /* Ornaments are source-over; silhouette coverage uses bright union. */
        value = lv_color_16_16_mix(r->palette[16], current, (lv_opa_t)(coverage * 255U / 16U));
    } else if (value <= current) {
        return;
    }
    if (r->surface->swap_bytes) value = swap16(value);
    memcpy(p, &value, sizeof(value));
}

#include "lcd_face_raster.inc"

xiaotai_err_t lcd_face_create(uint32_t background_rgb, lcd_face_t **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    lcd_face_t *f = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*f));
    *out = f;
    if (!f) return XIAOTAI_ERR_NO_MEM;
    f->background = pack_rgb(background_rgb);
    atomic_init(&f->late, 0);
    atomic_init(&f->slow, 0);
    atomic_init(&f->direct, 0);
    atomic_init(&f->pixels, 0);
    atomic_init(&f->fallback, 0);
    return XIAOTAI_OK;
}

void lcd_face_destroy(lcd_face_t *f)
{
    xiaotai_mem_free(f);
}

void lcd_face_attach(lcd_face_t *f, uint64_t now_ms)
{
    if (!f) return;
    const int64_t now = (int64_t)now_ms;
    f->attached = true;
    f->visible = f->valid = f->painted_valid = false;
    f->bound_count = 0;
    f->clear_all = false;
    f->random = (uint32_t)now | 1U;
    f->gaze_from_x = f->gaze_to_x = f->gaze_from_y = f->gaze_to_y = 0;
    f->gaze_at = now;
    f->gaze_next = now + 6000;
    f->blink_at = now + 2600;
    f->activity = 0;
    lcd_face_set(f, "neutral", XIAOTAI_TALK_IDLE, 0, now_ms);
}

void lcd_face_detach(lcd_face_t *f)
{
    if (!f) return;
    f->attached = f->visible = f->painted_valid = false;
}

void lcd_face_set_background(lcd_face_t *f, uint32_t background_rgb)
{
    if (!f) return;
    const uint16_t background = pack_rgb(background_rgb);
    if (f->background == background) return;
    f->background = background;
    f->clear_all = true;
}

size_t lcd_face_step(lcd_face_t *f, uint64_t now_ms, bool running, lcd_face_rect_t regions[LCD_FACE_REGIONS])
{
    if (!f || !f->attached || !regions) return 0;
    if (!running) {
        f->visible = false;
        return 0;
    }
    const int64_t now = (int64_t)now_ms;
    if (f->visible && now - f->last_frame_at < LCD_FACE_FRAME_MS) return 0;
    const int64_t started = xiaotai_time_us();
    const int64_t gap = f->visible ? now - f->last_frame_at : LCD_FACE_FRAME_MS;
    if (f->visible && gap > 80) count_add(&f->late, 1);
    const int32_t dt = gap > 160 ? 160 : (int32_t)gap;
    f->visible = true;
    f->last_frame_at = now;
    int32_t *pose = f->painted;
    face_base(f, now, pose);
    pose[19] += pose[FACE_MOUTH_TILT];
    f->activity = face_lerp(f->activity, f->activity_target * 256, (int32_t)dt * 1024 / 160);
    int32_t gaze_t = face_ease(now - f->gaze_at, 560);
    int32_t gx = face_lerp(f->gaze_from_x, f->gaze_to_x, gaze_t);
    int32_t gy = face_lerp(f->gaze_from_y, f->gaze_to_y, gaze_t);
    if (now >= f->gaze_next) {
        f->gaze_from_x = gx; f->gaze_from_y = gy;
        f->gaze_to_x = ((int)(face_random(f) % 5U) - 2) * 256;
        f->gaze_to_y = ((int)(face_random(f) % 3U) - 1) * 256;
        if (f->phase == XIAOTAI_TALK_LISTENING || f->phase == XIAOTAI_TALK_SPEAKING)
            f->gaze_to_x = f->gaze_to_y = 0;
        else if (f->phase == XIAOTAI_TALK_THINKING) { f->gaze_to_x = 2 * 256; f->gaze_to_y = -256; }
        f->gaze_at = now;
        f->gaze_next = now + 6000 + face_random(f) % 4000U;
    }
    int32_t blink = 1024;
    int64_t blink_ms = now - f->blink_at;
    if (blink_ms >= 0 && blink_ms < 240) {
        if (blink_ms < 80) blink = 1024 - face_ease(blink_ms, 80);
        else if (blink_ms < 120) blink = 0;
        else blink = face_ease(blink_ms - 120, 120);
    } else if (blink_ms >= 240) f->blink_at = now + 3000 + face_random(f) % 3500U;
    gx += pose[FACE_GAZE_X]; gy += pose[FACE_GAZE_Y];
    /* Keep all looks inside the same canvas, including the side sound waves. */
    if (gx > 3 * 256) gx = 3 * 256;
    if (gx < -3 * 256) gx = -3 * 256;
    if (gy > 2 * 256) gy = 2 * 256;
    if (gy < -2 * 256) gy = -2 * 256;
    int32_t tilt = pose[FACE_LEAN];
    int32_t style = pose[FACE_STYLE_B] / 256;
    int32_t wink = face_pulse(face_cue_age(f, FACE_WINK, now) % 6800 - 420, 120, 160, 240);
    wink = wink * (pose[FACE_WINK] / 256) / 256;
    if (wink > 0) blink = 1024;
    int32_t breath = face_wave(now, 4800) * 104 / 32768;
    for (unsigned i = 0; i < 3; ++i) {
        int32_t *part = pose + i * 7;
        part[0] += i == 2 ? gx / 3 : gx;
        part[1] += gy + breath + (part[0] - 110 * 256) * tilt / (64 * 256);
        if (i < 2) {
            part[3] += f->activity * pose[FACE_ATTENTION] / (3 * 65536);
            /* Blend blink eligibility, not just blink time: morphing through a
             * folded eye or resting lid must not switch eye height in one frame. */
            int32_t fold = pose[i == 0 ? FACE_FOLD_LEFT : FACE_FOLD_RIGHT];
            int32_t openness = (part[3] - (24 * 256 + style * 8)) * 2;
            if (openness < 0) openness = 0;
            if (openness > 1024) openness = 1024;
            int32_t closure = (1024 - blink) * openness / 1024;
            closure = closure * (1024 - fold / 64) / 1024;
            part[3] = face_lerp(part[3], 10 * 256 + style * 10, closure);
            part[6] = face_lerp(part[6], 0, closure);
            int32_t left_wink = wink * (pose[FACE_WINK_LEFT] / 256) / 256;
            int32_t eye_wink = i == 0 ? left_wink : wink - left_wink;
            part[3] = face_lerp(part[3], 14 * 256 + style * 10, eye_wink);
            part[6] = face_lerp(part[6], 3 * 256 + style * 6, eye_wink);
        }
    }
    int32_t hum = face_pulse(face_cue_age(f, FACE_MUSIC, now) % 7200 - 1000, 700, 300, 1200);
    pose[15] -= hum * (pose[FACE_MUSIC] / 256) / (8 * 256) * (256 + style * 3) / 256;
    /* A state accent, not fabricated lip sync from the microphone meter. */
    pose[17] += (face_wave(now, 1000) + 32768) * 192 / 65536 * pose[FACE_SPEECH] / 65536;
    memcpy(f->painted_cue_at, f->cue_at, sizeof(f->painted_cue_at));
    f->painted_at = now;
    f->painted_background = f->background;
    f->palette_count = 0;
    face_raster_t layout = {.face = f};
    face_render(&layout, pose, now);
    f->painted_valid = true;
    size_t count = 0;
    for (unsigned i = 0; i < LCD_FACE_REGIONS; ++i) {
        if (!(f->damage_mask & (1U << i))) continue;
        const face_bounds_t *d = &f->damage[i];
        regions[count++] = (lcd_face_rect_t){d->x0, d->y0, (int16_t)(d->x1 - d->x0 + 1), (int16_t)(d->y1 - d->y0 + 1)};
    }
    if (xiaotai_time_us() - started > 8000) count_add(&f->slow, 1);
    return count;
}

uint32_t lcd_face_delay(const lcd_face_t *f, uint64_t now_ms)
{
    if (!f || !f->attached || !f->visible) return 0;
    const int64_t age = (int64_t)now_ms - f->last_frame_at;
    return age <= 0 ? LCD_FACE_FRAME_MS : age < LCD_FACE_FRAME_MS ? LCD_FACE_FRAME_MS - (uint32_t)age : 1;
}

static uint32_t raster(lcd_face_t *f, const xiaotai_ui_surface_t *target, int16_t x, int16_t y)
{
    if (!f || !f->painted_valid || !target || !target->pixels || target->pixel != XIAOTAI_PIXEL_RGB565 ||
        !target->width || !target->height || target->stride < (uint32_t)target->width * 2U ||
        (size_t)(target->height - 1U) > (SIZE_MAX - (size_t)target->width * 2U) / target->stride) return 0;

    const int32_t x0 = (int32_t)target->x - x, y0 = (int32_t)target->y - y;
    const int32_t x1 = x0 + target->width - 1, y1 = y0 + target->height - 1;
    const int32_t left = x0 < 0 ? 0 : x0, top = y0 < 0 ? 0 : y0;
    const int32_t right = x1 >= LCD_FACE_WIDTH ? LCD_FACE_WIDTH - 1 : x1;
    const int32_t bottom = y1 >= LCD_FACE_HEIGHT ? LCD_FACE_HEIGHT - 1 : y1;
    if (right < left || bottom < top) return 0;

    face_raster_t surface = {.face = f, .surface = target, .target_x = x0, .target_y = y0,
        .clip = {(int16_t)left, (int16_t)top, (int16_t)right, (int16_t)bottom}};
    face_render(&surface, f->painted, f->painted_at);
    return (uint32_t)(right - left + 1) * (uint32_t)(bottom - top + 1);
}

bool lcd_face_paint(void *face, const xiaotai_ui_surface_t *target, int16_t x, int16_t y)
{
    lcd_face_t *f = face;
    const uint32_t count = raster(f, target, x, y);
    if (!count) return false;
    count_add(&f->direct, 1);
    count_add(&f->pixels, count);
    return true;
}

bool lcd_face_canvas(lcd_face_t *f, const xiaotai_ui_surface_t *target)
{
    if (!raster(f, target, 0, 0)) return false;
    count_add(&f->fallback, 1);
    return true;
}

void lcd_face_report(const lcd_face_t *f, char *line, size_t capacity)
{
    if (!line || !capacity) return;
    (void)snprintf(line, capacity, "face_late=%u face_slow=%u direct=%u direct_pixels=%u fallback=%u",
        f ? atomic_load_explicit(&f->late, memory_order_relaxed) : 0U,
        f ? atomic_load_explicit(&f->slow, memory_order_relaxed) : 0U,
        f ? atomic_load_explicit(&f->direct, memory_order_relaxed) : 0U,
        f ? atomic_load_explicit(&f->pixels, memory_order_relaxed) : 0U,
        f ? atomic_load_explicit(&f->fallback, memory_order_relaxed) : 0U);
}
