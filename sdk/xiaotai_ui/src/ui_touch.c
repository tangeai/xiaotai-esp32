// SPDX-License-Identifier: MIT
#include "ui_touch.h"

#include <math.h>
#include <string.h>

/* Units and units per second unless said otherwise. */
#define SLOP 6.0f                  /* a contact has moved */
#define CUTOFF_HZ 4.0f             /* of a finger at rest */
#define CUTOFF_PER_SPEED 0.02f     /* more for each unit per second */
#define SPEED_CUTOFF_HZ 10.0f
#define AHEAD_S 0.008f             /* prediction, and what it may amount to */
#define AHEAD_MAX 5.0f
#define AHEAD_SPEED 120.0f         /* a slower finger is not predicted */
#define JUMP 84.0f                 /* about a third of the shorter side ... */
#define JUMP_US 25000              /* ... within one sample */
#define JUMP_AGREES 40.0f
#define LIFT_US 15000              /* an empty sample this long after the first one ends a drag */
#define GAP_US 60000               /* a drag whose samples stopped this long ago had ended */
#define VELOCITY_US 100000
#define REST_US 50000              /* nothing heard for this long before the release: at rest */

static float clampf(float v, float low, float high) { return v < low ? low : v > high ? high : v; }
static float distance(float ax, float ay, float bx, float by) { return hypotf(ax - bx, ay - by); }

static float alpha(float cutoff_hz, float dt)
{
    const float tau = 1.0f / (6.2831853f * cutoff_hz);
    return dt / (dt + tau);
}

void ui_touch_init(ui_touch_t *t, uint16_t width, uint16_t height)
{
    *t = (ui_touch_t){.width = width, .height = height};
    const uint16_t side = width < height ? width : height;
    t->unit = side ? (float)side / 240.0f : 1.0f;
}

void ui_touch_reset(ui_touch_t *t) { t->down = t->lifting = t->suspect = false; }

static void remember(ui_touch_t *t, int64_t at_us)
{
    t->head = (uint8_t)((t->head + 1U) % UI_TOUCH_HISTORY);
    if (t->used < UI_TOUCH_HISTORY) ++t->used;
    t->history[t->head].at_us = at_us;
    t->history[t->head].x = t->raw_x;
    t->history[t->head].y = t->raw_y;
}

/* The slope of the last positions, by least squares. */
static void velocity(const ui_touch_t *t, int64_t now_us, float *vx, float *vy)
{
    *vx = *vy = 0.0f;
    if (!t->used || now_us - t->history[t->head].at_us > REST_US) return;
    float n = 0, st = 0, sx = 0, sy = 0, stt = 0, stx = 0, sty = 0;
    for (uint8_t i = 0; i < t->used; ++i) {
        const uint8_t at = (uint8_t)((t->head + UI_TOUCH_HISTORY - i) % UI_TOUCH_HISTORY);
        const int64_t age = now_us - t->history[at].at_us;
        if (age > VELOCITY_US) break;
        const float s = -(float)age / 1000000.0f;
        n += 1.0f; st += s; sx += t->history[at].x; sy += t->history[at].y;
        stt += s * s; stx += s * t->history[at].x; sty += s * t->history[at].y;
    }
    const float spread = n * stt - st * st;
    if (n < 3.0f || spread < 1e-6f) return;
    *vx = (n * stx - st * sx) / spread;
    *vy = (n * sty - st * sy) / spread;
}

static ui_contact_t report(ui_touch_t *t, int64_t at_us, bool down)
{
    if (down) {
        float x = t->x, y = t->y;
        const float speed = hypotf(t->vx, t->vy) / t->unit;
        if (t->moved && speed > AHEAD_SPEED) {
            const float far = clampf(speed * AHEAD_S, 0.0f, AHEAD_MAX) / speed;
            x += t->vx * far;
            y += t->vy * far;
        }
        t->out_x = (int32_t)lroundf(clampf(x, 0.0f, (float)(t->width - 1)));
        t->out_y = (int32_t)lroundf(clampf(y, 0.0f, (float)(t->height - 1)));
    }
    ui_contact_t out = {.at_us = at_us, .x = t->out_x, .y = t->out_y, .down = down};
    if (!down && t->moved) velocity(t, at_us, &out.vx, &out.vy);
    return out;
}

static ui_contact_t begin(ui_touch_t *t, const ui_sample_t *s)
{
    t->down = true;
    t->moved = t->lifting = t->suspect = false;
    t->at_us = s->at_us;
    t->x = t->raw_x = t->press_x = (float)s->x;
    t->y = t->raw_y = t->press_y = (float)s->y;
    t->vx = t->vy = 0.0f;
    t->used = 0;
    remember(t, s->at_us);
    return report(t, s->at_us, true);
}

static ui_contact_t end(ui_touch_t *t, int64_t at_us)
{
    const ui_contact_t out = report(t, at_us, false);
    ui_touch_reset(t);
    return out;
}

static size_t lifted(ui_touch_t *t, const ui_sample_t *s, ui_contact_t out[2])
{
    if (!t->down) return 0;
    t->suspect = false;
    if (!t->moved || s->failed) {
        out[0] = end(t, s->at_us);
        return 1;
    }
    if (!t->lifting) {
        t->lifting = true;
        t->lift_us = s->at_us;
        return 0;
    }
    if (s->at_us - t->lift_us < LIFT_US) return 0;
    /* It left with the first empty sample: that is when it had its speed. */
    out[0] = end(t, t->lift_us);
    out[0].at_us = s->at_us;
    return 1;
}

size_t ui_touch_feed(ui_touch_t *t, const ui_sample_t *s, ui_contact_t out[2])
{
    if (!s->down || s->failed) return lifted(t, s, out);
    if (!t->down) {
        out[0] = begin(t, s);
        return 1;
    }
    if (s->at_us - t->at_us > GAP_US && t->lifting) {
        out[0] = end(t, t->lift_us);
        out[0].at_us = s->at_us;
        out[1] = begin(t, s);
        return 2;
    }
    const float x = (float)s->x, y = (float)s->y, dt = (float)(s->at_us - t->at_us) / 1000000.0f;
    if (dt <= 0.0005f) return 0;   /* the same report read twice */
    if (t->suspect) {
        t->suspect = false;
        if (distance(x, y, t->jump_x, t->jump_y) < JUMP_AGREES * t->unit) {
            out[0] = end(t, t->at_us);
            out[0].vx = out[0].vy = 0.0f;
            out[0].at_us = s->at_us;
            out[1] = begin(t, s);
            return 2;
        }
    } else if (s->at_us - t->at_us < JUMP_US && distance(x, y, t->raw_x, t->raw_y) > JUMP * t->unit) {
        t->suspect = true;
        t->jump_x = x;
        t->jump_y = y;
        return 0;
    }
    t->lifting = false;
    t->at_us = s->at_us;
    const float smooth = alpha(SPEED_CUTOFF_HZ, dt);
    t->vx += smooth * ((x - t->raw_x) / dt - t->vx);
    t->vy += smooth * ((y - t->raw_y) / dt - t->vy);
    t->raw_x = x;
    t->raw_y = y;
    const float follow = alpha(CUTOFF_HZ + CUTOFF_PER_SPEED * hypotf(t->vx, t->vy) / t->unit, dt);
    t->x += follow * (x - t->x);
    t->y += follow * (y - t->y);
    if (distance(x, y, t->press_x, t->press_y) > SLOP * t->unit) t->moved = true;
    remember(t, s->at_us);
    out[0] = report(t, s->at_us, true);
    return 1;
}
