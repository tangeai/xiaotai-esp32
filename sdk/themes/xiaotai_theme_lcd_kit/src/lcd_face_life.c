// SPDX-License-Identifier: MIT
#include "lcd_face_life.h"

#include <math.h>
#include <string.h>
#include "lcd_face.h"
#include "xiaotai/view.h"

/* Canvas pixels and milliseconds throughout. */
#define GAZE_X 13.0f
#define GAZE_Y 8.0f
#define CALM_MS 5000            /* quiet before the face starts doing things of its own */
#define POKE_MS 2600            /* pokes closer together than this add up */
#define HOLD_MS 650             /* a finger resting this long is petting */
#define SLOW_HZ 0.5f            /* what sways slower is drift */
#define PI2 6.2831853f

typedef enum {
    ACT_NONE, ACT_LOOK_AROUND, ACT_CURIOUS, ACT_YAWN, ACT_HOP, ACT_PEEK, ACT_WINK, ACT_HUM, ACT_SNEEZE, ACT_COUNT,
    /* reactions, never chosen by chance */
    ACT_GIGGLE = ACT_COUNT, ACT_OUCH, ACT_CROSS, ACT_DIZZY, ACT_BLISS, ACT_SHY, ACT_WAKE,
} act_t;

/* How each expression carries itself, by the name of its pose. `pop` is the
 * jump it enters with; the rest goes on for as long as it shows. */
typedef struct {
    const char *name;
    float pop;                  /* squash on entry: above 0 a stretch */
    float bounce, bounce_hz;    /* rise, pixels */
    float shake, shake_hz;      /* sway, pixels */
    float rock, rock_hz;        /* tilt */
    float linger;               /* what is left of the movement after the first seconds */
    bool rests;                 /* acts of its own may interrupt it */
} manner_t;

static const manner_t manners[] = {
    {"neutral", 0.10f, 0, 0, 0, 0, 0, 0, 0, true},
    {"happy", 0.35f, 2.6f, 2.2f, 0, 0, 1.2f, 1.1f, 0.25f, true},
    {"laughing", 0.45f, 2.4f, 5.5f, 0, 0, 1.6f, 2.7f, 0.45f, false},
    {"funny", 0.35f, 1.2f, 1.6f, 0, 0, 3.2f, 0.8f, 0.5f, true},
    {"sad", -0.10f, 0.8f, 0.25f, 0, 0, 0, 0, 1.0f, true},
    {"angry", 0.25f, 0, 0, 1.1f, 9.0f, 0, 0, 0.35f, false},
    {"crying", -0.10f, 0.9f, 3.2f, 0.7f, 4.6f, 0, 0, 0.6f, false},
    {"loving", 0.30f, 1.6f, 1.2f, 0, 0, 1.5f, 0.6f, 0.5f, true},
    {"embarrassed", 0.15f, 0.8f, 0.6f, 0, 0, 2.0f, 0.5f, 0.6f, true},
    {"surprised", 0.75f, 0, 0, 0, 0, 0, 0, 0, true},
    {"shocked", 0.95f, 0, 0, 0.8f, 12.0f, 0, 0, 0.15f, true},
    {"thinking", 0.10f, 0.8f, 0.35f, 0, 0, 2.4f, 0.22f, 1.0f, false},
    {"winking", 0.35f, 1.2f, 1.2f, 0, 0, 2.2f, 0.6f, 0.2f, true},
    {"cool", 0.25f, 0.6f, 0.5f, 0, 0, 1.4f, 0.4f, 0.6f, true},
    {"relaxed", 0.10f, 1.6f, 0.45f, 2.4f, 0.45f, 2.2f, 0.45f, 1.0f, true},
    {"delicious", 0.40f, 1.5f, 2.4f, 0, 0, 1.6f, 1.2f, 0.4f, true},
    {"kissy", 0.40f, 1.1f, 0.9f, 0, 0, 2.0f, 0.5f, 0.4f, true},
    {"confident", 0.30f, 0.6f, 0.4f, 0, 0, 1.6f, 0.3f, 0.6f, true},
    {"sleepy", 0, 2.6f, 0.16f, 0, 0, 2.2f, 0.11f, 1.0f, false},
    {"silly", 0.45f, 1.4f, 2.0f, 1.2f, 1.6f, 3.4f, 0.9f, 0.5f, true},
    {"confused", 0.20f, 0.6f, 0.5f, 0, 0, 4.0f, 0.38f, 0.8f, true},
    {"listening", 0.25f, 0.5f, 0.35f, 0, 0, 1.2f, 0.18f, 1.0f, false},
    {"ambient", 0, 1.4f, 0.22f, 2.0f, 0.13f, 1.8f, 0.16f, 1.0f, true},
    [LCD_FACE_RINGING] = {"ringing", 0.60f, 2.2f, 3.0f, 0, 0, 4.4f, 3.0f, 0.75f, false},
    [LCD_FACE_DIALING] = {"dialing", 0.25f, 0.8f, 0.6f, 0, 0, 2.2f, 0.4f, 1.0f, false},
    [LCD_FACE_OFFLINE] = {"offline", 0, 0.8f, 0.25f, 0, 0, 2.2f, 0.15f, 1.0f, true},
    [LCD_FACE_ERROR] = {"error", -0.10f, 0.4f, 0.3f, 2.4f, 2.4f, 0, 0, 0.06f, false},
};
_Static_assert(sizeof(manners) / sizeof(manners[0]) == LCD_FACE_MANNERS, "every manner");

static int index_of(const char *name) { return (int)lcd_face_resolve(name); }

static const manner_t *manner_of(int mood)
{
    /* The table follows the order of the poses; the first one is the fallback. */
    const size_t count = sizeof(manners) / sizeof(manners[0]);
    return &manners[mood >= 0 && (size_t)mood < count ? (size_t)mood : 0U];
}

static uint32_t rnd(lcd_face_life_t *f)
{
    uint32_t x = f->random;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return f->random = x;
}
static float chance(lcd_face_life_t *f) { return (float)(rnd(f) % 1000U) / 1000.0f; }
static float between(lcd_face_life_t *f, float a, float b) { return a + (b - a) * chance(f); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float toward(float from, float to, float dt, float tau) { return from + (to - from) * (1.0f - expf(-dt / tau)); }
static float ramp(float t, float a, float b)
{
    const float x = clampf((t - a) / (b - a), 0, 1);
    return x * x * (3.0f - 2.0f * x);
}
/* Up over `rise`, held, down over `fall`. */
static float pulse(float t, float at, float rise, float hold, float fall)
{
    return ramp(t, at, at + rise) * (1.0f - ramp(t, at + rise + hold, at + rise + hold + fall));
}

static void spring(float *value, float *speed, float to, float dt_s, float hz, float damping)
{
    const float w = PI2 * hz;
    /* Two half steps keep it stable at 25 frames a second. */
    for (int i = 0; i < 2; ++i) {
        const float h = dt_s * 0.5f;
        *speed += (-2.0f * damping * w * *speed - w * w * (*value - to)) * h;
        *value += *speed * h;
    }
}

void lcd_face_life_start(lcd_face_life_t *f, int64_t now)
{
    memset(f, 0, sizeof(*f));
    f->random = (uint32_t)now * 2654435761U | 1U;
    f->now = f->calm_since = now;
    f->mood = 0;
    f->lid[0] = f->lid[1] = f->lid_to[0] = f->lid_to[1] = 1.0f;
    f->blink_next = now + 1800;
    f->look_next = now + 900;
    f->act_next = now + 9000;
    f->override_mood = LCD_FACE_MOOD_NONE;
    f->loud_ref = 2500.0f;
    f->heard_ref = 900.0f;
    f->wide = f->pupil = 1.0f;
}

static void blink(lcd_face_life_t *f, int64_t now, uint8_t count, bool slow)
{
    f->blink_at = now;
    f->blink_left = count;
    f->blink_slow = slow;
}

static void act(lcd_face_life_t *f, act_t what, int64_t now)
{
    f->act = (uint8_t)what;
    f->act_at = now;
}

static void show(lcd_face_life_t *f, const char *name, int64_t until)
{
    f->override_mood = index_of(name);
    f->override_until = until;
}

static bool talking(const lcd_face_life_t *f) { return f->phase != XIAOTAI_TALK_IDLE; }

void lcd_face_life_mood(lcd_face_life_t *f, int mood, uint8_t phase, uint8_t level, int64_t now)
{
    const bool changed = mood != f->mood || phase != f->phase;
    f->level = level;
    if (!changed) return;
    const bool slept = !strcmp(manner_of(f->mood)->name, "sleepy");
    f->mood = mood;
    f->phase = phase;
    f->mood_at = f->calm_since = now;
    f->look_next = now;
    if (f->act && f->act < ACT_COUNT) f->act = ACT_NONE;
    f->act_next = now + (int64_t)between(f, 8000, 15000);
    /* The lids cover the change, and what enters arrives with a jump. */
    blink(f, now, 1, false);
    if (slept && strcmp(manner_of(mood)->name, "sleepy")) act(f, ACT_WAKE, now);
    f->squash_v -= manner_of(mood)->pop * 9.0f;
}

/* ------------------------------------------------------------------ looking */

static void choose_look(lcd_face_life_t *f, int64_t now)
{
    const float dice = chance(f);
    float dwell = between(f, 900, 3200);
    if (f->phase == XIAOTAI_TALK_LISTENING) {
        f->aim_x = between(f, -2.5f, 2.5f);
        f->aim_y = between(f, -2.0f, 1.0f);
        dwell = between(f, 1200, 2600);
    } else if (f->phase == XIAOTAI_TALK_THINKING) {
        /* Up there somewhere; now and then across. */
        f->aim_x = dice > 0.62f ? between(f, -GAZE_X * 1.5f, -GAZE_X) : between(f, -3, 3);
        f->aim_y = between(f, -3, 2);
        dwell = between(f, 800, 1700);
    } else if (f->phase == XIAOTAI_TALK_SPEAKING) {
        /* To the listener, with a glance away now and then. */
        const bool away = dice > 0.78f;
        f->aim_x = away ? between(f, -10, 10) : between(f, -2, 2);
        f->aim_y = away ? between(f, -5, 2) : between(f, -1, 1);
        dwell = away ? between(f, 350, 700) : between(f, 1400, 3000);
    } else if (dice < 0.50f) {
        f->aim_x = between(f, -4, 4);
        f->aim_y = between(f, -2.5f, 2.5f);
    } else if (dice < 0.86f) {
        f->aim_x = (chance(f) < 0.5f ? -1.0f : 1.0f) * between(f, 7, GAZE_X);
        f->aim_y = between(f, -5, 4);
        if (chance(f) < 0.4f) blink(f, now, 1, false);
    } else {
        f->aim_x = f->aim_y = 0;
        dwell += 900;
    }
    f->aim_x = clampf(f->aim_x * 0.8f, -GAZE_X, GAZE_X);
    f->aim_y = clampf(f->aim_y * 0.8f, -GAZE_Y, GAZE_Y);
    f->look_next = now + (int64_t)dwell;
}

static void look(lcd_face_life_t *f, int64_t now, float dt, const lcd_face_senses_t *in)
{
    f->follow = in->down;
    if (in->down) {
        f->aim_x = clampf((float)(in->x - 120) * 0.16f, -GAZE_X - 2, GAZE_X + 2);
        f->aim_y = clampf((float)(in->y - 62) * 0.13f, -GAZE_Y - 2, GAZE_Y + 2);
        f->look_next = now + 900;
    } else if (now >= f->look_next) {
        choose_look(f, now);
    }
    /* Eyes jump: most of the way within two frames. */
    const float tau = in->down ? 55.0f : f->phase == XIAOTAI_TALK_IDLE && !strcmp(manner_of(f->mood)->name, "ambient") ? 260.0f : 48.0f;
    f->gaze_x = toward(f->gaze_x, f->aim_x, dt, tau);
    f->gaze_y = toward(f->gaze_y, f->aim_y, dt, tau);
}

/* ----------------------------------------------------------------- blinking */

static float blink_lid(lcd_face_life_t *f, int64_t now)
{
    if (!f->blink_left) return 1.0f;
    const float stretch = f->blink_slow ? 2.6f : 1.0f;
    const float t = (float)(now - f->blink_at) / stretch;
    if (t < 0) return 1.0f;
    if (t < 70) return 1.0f - ramp(t, 0, 70);
    if (t < 105) return 0;
    if (t < 235) return ramp(t, 105, 235) * 1.05f;
    if (--f->blink_left) f->blink_at = now + 60;
    return 1.0f;
}

/* ------------------------------------------------------------------- finger */

static bool near_eye(const lcd_face_senses_t *in, int eye)
{
    const int cx = eye ? 180 : 60;
    return in->x > cx - 40 && in->x < cx + 40 && in->y > 26 && in->y < 90;
}

static void poke(lcd_face_life_t *f, int64_t now, const lcd_face_senses_t *in)
{
    f->pokes = now - f->poke_at < POKE_MS ? (uint8_t)(f->pokes + 1U) : 1U;
    f->poke_at = now;
    f->squash_v += 6.0f;
    if (f->pokes >= 6) {
        act(f, ACT_DIZZY, now);
        show(f, "confused", now + 2600);
        f->pokes = 0;
    } else if (f->pokes >= 4) {
        act(f, ACT_CROSS, now);
        show(f, "angry", now + 1500);
    } else if (near_eye(in, 0) || near_eye(in, 1)) {
        act(f, ACT_OUCH, now);
        f->lid_to[near_eye(in, 1) ? 1 : 0] = 0;
        show(f, f->pokes > 1 ? "funny" : "surprised", now + 900);
    } else if (in->y > 78) {
        act(f, ACT_GIGGLE, now);
        show(f, f->pokes > 1 ? "laughing" : "silly", now + 1300);
    } else {
        act(f, ACT_GIGGLE, now);
        show(f, f->pokes > 1 ? "laughing" : "happy", now + 1200);
    }
}

static void touch(lcd_face_life_t *f, int64_t now, const lcd_face_senses_t *in)
{
    const bool on_face = in->x > -10 && in->x < 250 && in->y > -16 && in->y < 140;
    if (in->down && !f->down) {
        f->down = true;
        f->petting = f->pet_done = false;
        f->down_at = f->touched_at = now;
        f->down_x = f->last_x = in->x;
        f->down_y = f->last_y = in->y;
        f->path = 0;
        if (on_face) f->squash_to = 0.10f;
    } else if (in->down) {
        f->path += fabsf((float)(in->x - f->last_x)) + fabsf((float)(in->y - f->last_y));
        f->last_x = in->x;
        f->last_y = in->y;
        f->touched_at = now;
        const bool stays = fabsf((float)(in->x - f->down_x)) < 60 && fabsf((float)(in->y - f->down_y)) < 60;
        if (!f->petting && on_face && stays && (now - f->down_at > HOLD_MS || f->path > 110)) {
            f->petting = true;
            f->pokes = 0;
            act(f, ACT_BLISS, now);
        }
        if (f->petting) show(f, "loving", now + 400);
    } else if (f->down) {
        f->down = false;
        f->squash_to = 0;
        if (f->petting) {
            f->pet_done = true;
            act(f, ACT_SHY, now);
            show(f, "embarrassed", now + 1400);
        } else if (f->path < 24 && now - f->down_at < 450) {
            const lcd_face_senses_t at = {.x = f->down_x, .y = f->down_y};
            if (f->down_x > -10 && f->down_x < 250 && f->down_y > -16 && f->down_y < 140) poke(f, now, &at);
        }
        f->petting = false;
    }
    if (in->down) f->calm_since = now;
}

bool lcd_face_life_stroked(const lcd_face_life_t *f)
{
    return f && (f->petting || (f->pet_done && f->now - f->touched_at < 120));
}

/* ------------------------------------------------------------------- acting */

static void start_act(lcd_face_life_t *f, int64_t now)
{
    /* Commoner ones are listed more often. */
    static const uint8_t pool[] = {ACT_LOOK_AROUND, ACT_LOOK_AROUND, ACT_CURIOUS, ACT_CURIOUS, ACT_HOP, ACT_WINK,
        ACT_HUM, ACT_PEEK, ACT_YAWN, ACT_SNEEZE};
    uint8_t what;
    do what = pool[rnd(f) % sizeof(pool)]; while (what == f->last_act);
    f->last_act = what;
    act(f, (act_t)what, now);
    f->act_next = now + (int64_t)between(f, 9000, 19000);
    switch (what) {
    case ACT_HOP: show(f, "happy", now + 1500); break;
    case ACT_WINK: show(f, "winking", now + 1700); break;
    case ACT_HUM: show(f, "relaxed", now + 4200); break;
    case ACT_CURIOUS: f->aim_x = chance(f) < 0.5f ? -11.0f : 11.0f; f->look_next = now + 1800; break;
    default: break;
    }
}

/* What the current act adds; clears it when it is over. */
static void play_act(lcd_face_life_t *f, int64_t now, lcd_face_frame_t *o, float lids[2])
{
    const float t = (float)(now - f->act_at);
    float over = 1400;
    switch (f->act) {
    case ACT_LOOK_AROUND:
        f->aim_x = -GAZE_X * pulse(t, 0, 90, 600, 90) + GAZE_X * pulse(t, 820, 110, 650, 160);
        f->aim_y = -2.0f * pulse(t, 820, 110, 650, 160);
        o->tilt += -2.0f * pulse(t, 0, 200, 500, 200) + 2.0f * pulse(t, 820, 200, 550, 200);
        f->look_next = now + 600;
        over = 1900;
        if (t > 800 && t < 845) blink(f, now, 1, false);
        break;
    case ACT_CURIOUS: {
        const float p = pulse(t, 0, 260, 1200, 320), side = f->aim_x >= 0 ? 1.0f : -1.0f;
        o->tilt += side * 5.0f * p;
        o->squash -= 0.06f * p;
        lids[side > 0 ? 0 : 1] *= 1.0f - 0.18f * p;
        over = 1900;
        break;
    }
    case ACT_YAWN: {
        const float p = pulse(t, 0, 500, 900, 400);
        lids[0] *= 1.0f - 0.92f * p;
        lids[1] *= 1.0f - 0.92f * p;
        o->mouth_open += 0.95f * p;
        o->rise -= 4.0f * p;
        o->squash -= 0.10f * p;
        o->sway += 1.6f * sinf(t * 0.030f) * pulse(t, 1750, 100, 250, 200);
        over = 2400;
        break;
    }
    case ACT_HOP:
        o->squash += 0.20f * pulse(t, 0, 110, 20, 70) - 0.16f * pulse(t, 190, 80, 80, 120) + 0.18f * pulse(t, 480, 60, 20, 160);
        o->rise -= 10.0f * pulse(t, 170, 150, 20, 150);
        over = 900;
        break;
    case ACT_PEEK: {
        const float hide = pulse(t, 0, 260, 620, 10);
        o->rise += 70.0f * hide + 36.0f * pulse(t, 890, 10, 700, 240) * (1.0f - hide);
        f->aim_x = t > 900 ? 10.0f * sinf((t - 900) * 0.006f) : 0;
        f->look_next = now + 400;
        over = 2000;
        break;
    }
    case ACT_WINK:
        o->tilt += 3.0f * pulse(t, 250, 180, 500, 250);
        over = 1400;
        break;
    case ACT_HUM:
        o->sway += 4.0f * sinf(t * 0.0048f) * pulse(t, 0, 400, 3200, 500);
        o->tilt += 3.0f * sinf(t * 0.0048f + 0.6f) * pulse(t, 0, 400, 3200, 500);
        over = 4200;
        break;
    case ACT_SNEEZE: {
        const float in = pulse(t, 0, 700, 60, 60), out = pulse(t, 800, 50, 60, 260);
        lids[0] *= 1.0f - 0.85f * in - 0.95f * out * (1.0f - in);
        lids[1] *= 1.0f - 0.85f * in - 0.95f * out * (1.0f - in);
        o->rise += -5.0f * in + 7.0f * out;
        o->squash += -0.10f * in + 0.24f * out;
        o->mouth_open += 0.35f * in + 0.5f * out;
        over = 1500;
        break;
    }
    case ACT_GIGGLE:
        o->rise += 2.6f * fabsf(sinf(t * 0.016f)) * -(1.0f - ramp(t, 700, 1200));
        o->tilt += 2.4f * sinf(t * 0.011f) * (1.0f - ramp(t, 700, 1200));
        over = 1250;
        break;
    case ACT_OUCH:
        o->sway += (f->lid_to[0] < 0.5f ? 4.0f : -4.0f) * pulse(t, 0, 60, 160, 300);
        if (t > 420) f->lid_to[0] = f->lid_to[1] = 1.0f;
        over = 900;
        break;
    case ACT_CROSS:
        o->sway += 3.2f * sinf(t * 0.045f) * (1.0f - ramp(t, 350, 900));
        over = 1500;
        break;
    case ACT_DIZZY: {
        const float p = 1.0f - ramp(t, 1900, 2500);
        f->aim_x = 13.0f * cosf(t * 0.012f) * p;
        f->aim_y = 7.0f * sinf(t * 0.012f) * p;
        f->look_next = now + 300;
        o->tilt += 4.0f * sinf(t * 0.006f) * p;
        over = 2600;
        break;
    }
    case ACT_BLISS:
        /* As long as the stroking goes on. */
        o->tilt += 2.2f * sinf(t * 0.0042f);
        o->rise += 1.2f * sinf(t * 0.0060f);
        o->squash += 0.05f;
        over = f->petting ? t + 1.0f : 0;
        break;
    case ACT_SHY:
        o->tilt += -2.6f * pulse(t, 0, 220, 700, 400);
        o->rise += 2.0f * pulse(t, 0, 220, 700, 400);
        over = 1400;
        break;
    case ACT_WAKE: {
        /* Starts up, shakes itself awake. */
        const float p = pulse(t, 0, 90, 260, 260);
        o->squash -= 0.22f * p;
        o->rise -= 5.0f * p;
        o->sway += 3.0f * sinf(t * 0.040f) * pulse(t, 420, 80, 240, 200);
        over = 1000;
        break;
    }
    default: return;
    }
    if (t < over) return;
    if (f->act == ACT_OUCH) f->lid_to[0] = f->lid_to[1] = 1.0f;
    f->act = ACT_NONE;
    f->calm_since = now;
}

/* -------------------------------------------------------------------- sound */

/* The opening of the mouth from the sound that is played, against the loudest
 * of late: quiet volumes open it as far as loud ones. */
static void sound(lcd_face_life_t *f, int64_t now, float dt, const lcd_face_senses_t *in, lcd_face_frame_t *o)
{
    float level = 0;
    if (in->hears && in->playout > 40U) {
        f->sound_at = now;
        f->loud_ref = f->loud_ref * (1.0f - 0.0006f * dt);
        if (in->playout > f->loud_ref) f->loud_ref = in->playout;
        if (f->loud_ref < 300.0f) f->loud_ref = 300.0f;
        level = clampf(((float)in->playout / f->loud_ref - 0.06f) * 1.25f, 0, 1);
        level = sqrtf(level);
    } else if (!in->hears && f->phase == XIAOTAI_TALK_SPEAKING) {
        /* Nothing measures the sound here: an even movement, as before. */
        level = 0.35f + 0.35f * sinf((float)(now % 100000) * 0.0063f);
    }
    f->mouth = toward(f->mouth, level, dt, level > f->mouth ? 32.0f : 85.0f);
    if (f->mouth < 0.01f) f->mouth = 0;
    /* Vowels differ in width too; a slow wander keeps it from pumping. */
    f->wide = toward(f->wide, 1.0f + 0.16f * sinf((float)(now % 100000) * 0.0041f) * f->mouth, dt, 90.0f);
    o->mouth_open += f->mouth;
    o->mouth_wide *= f->wide;
    /* The head nods along with the stresses. */
    o->rise -= 1.8f * f->mouth;

    float heard = 0;
    if (in->hears && f->phase == XIAOTAI_TALK_LISTENING) {
        /* A voice, not what the room sounds like all the time. */
        const float level = (float)in->capture;
        f->heard_floor = f->heard_floor > 0 ? toward(f->heard_floor, level, dt, level < f->heard_floor ? 260.0f : 9000.0f) : level;
        const float voice = level - f->heard_floor * 1.7f - 30.0f;
        f->heard_ref = f->heard_ref * (1.0f - 0.0004f * dt);
        if (voice > f->heard_ref) f->heard_ref = voice;
        if (f->heard_ref < 320.0f) f->heard_ref = 320.0f;
        heard = clampf(voice / f->heard_ref * 1.1f - 0.06f, 0, 1);
    } else if (f->phase == XIAOTAI_TALK_LISTENING) {
        heard = (float)f->level / 3.0f;
    }
    f->heard = toward(f->heard, heard, dt, heard > f->heard ? 45.0f : 180.0f);
    /* It shows that it listens with its whole face: the irises light up, the
     * eyes widen and the brows lift with the voice, and where the speaker
     * pauses it nods. */
    const bool attends = f->phase == XIAOTAI_TALK_LISTENING;
    const bool voice = attends && f->heard > 0.34f;
    if (voice && (!f->voice_at || now - f->voice_at > 700)) f->brow_v += 34.0f;
    if (voice) {
        f->voice_at = now;
        f->nod_due = true;
    } else if (attends && f->nod_due && now - f->voice_at > 620) {
        f->nod_due = false;
        f->rise_v += 130.0f;
        blink(f, now, 1, false);
    }
    if (!attends) f->voice_at = 0, f->nod_due = false;
    f->ring = toward(f->ring, attends ? 0.46f + 0.54f * f->heard : 0, dt, attends ? 90.0f : 220.0f);
    o->listen = f->ring;
    o->brow += 2.4f * f->heard;
    o->squash -= 0.06f * f->heard;
    o->rise -= 1.6f * f->heard;
}

/* -------------------------------------------------------------------- frame */

int lcd_face_life_step(lcd_face_life_t *f, int64_t now, const lcd_face_senses_t *in, lcd_face_frame_t *out)
{
    float dt = (float)(now - f->now);
    if (dt < 0) dt = 0;
    if (dt > 160) dt = 160;
    f->now = now;
    *out = (lcd_face_frame_t){.mouth_wide = 1.0f};
    touch(f, now, in);

    const bool overridden = f->override_mood != LCD_FACE_MOOD_NONE && now < f->override_until;
    if (!overridden) f->override_mood = LCD_FACE_MOOD_NONE;
    const int mood = overridden ? f->override_mood : f->mood;
    const manner_t *m = manner_of(mood);

    float lids[2] = {1.0f, 1.0f};
    if (!f->act && !in->down && !talking(f) && manner_of(f->mood)->rests && now - f->calm_since > CALM_MS && now >= f->act_next)
        start_act(f, now);
    play_act(f, now, out, lids);
    look(f, now, dt, in);

    /* Its own manner: strong when it comes, then what lingers. */
    const float age = (float)(now - (overridden ? f->act_at : f->mood_at)) / 1000.0f;
    const float energy = m->linger + (1.0f - m->linger) * (1.0f - ramp(age, 1.2f, 2.6f));
    const float s = (float)(now % 600000) / 1000.0f;
    out->slow = (lcd_face_drift_t){.rise = 1.2f * sinf(PI2 * s / 4.3f), .squash = 0.012f * sinf(PI2 * s / 4.3f + 1.2f)};
    if (m->bounce_hz > 0) *(m->bounce_hz < SLOW_HZ ? &out->slow.rise : &out->rise) += m->bounce * energy * sinf(PI2 * m->bounce_hz * s);
    if (m->shake_hz > 0) *(m->shake_hz < SLOW_HZ ? &out->slow.sway : &out->sway) += m->shake * energy * sinf(PI2 * m->shake_hz * s);
    if (m->rock_hz > 0) *(m->rock_hz < SLOW_HZ ? &out->slow.tilt : &out->tilt) += m->rock * energy * sinf(PI2 * m->rock_hz * s + 0.7f);

    if (now >= f->blink_next) {
        const float dice = chance(f);
        blink(f, now, dice > 0.84f ? 2 : 1, dice < 0.10f || !strcmp(m->name, "sleepy"));
        f->blink_next = now + (int64_t)(talking(f) ? between(f, 1700, 4200) : between(f, 2200, 5600));
    }
    const float closing = blink_lid(f, now);
    for (int i = 0; i < 2; ++i) {
        f->lid[i] = toward(f->lid[i], f->lid_to[i], dt, f->lid_to[i] < f->lid[i] ? 28.0f : 90.0f);
        out->open[i] = clampf(lids[i], 0.0f, 1.2f) * f->lid[i] * closing;
    }
    /* Closing lids widen the eyes a little. */
    out->squash += 0.05f * (1.0f - clampf(closing, 0, 1));
    if (!strcmp(m->name, "sleepy")) {
        /* Nods off, and starts up again. */
        const float nod = fmodf(s, 7.5f);
        const float p = pulse(nod, 2.6f, 2.2f, 0.5f, 0.18f);
        out->rise += 7.0f * p;
        out->open[0] *= 1.0f - 0.75f * p;
        out->open[1] *= 1.0f - 0.75f * p;
    }

    sound(f, now, dt, in, out);

    const float dts = dt / 1000.0f;
    spring(&f->squash, &f->squash_v, f->squash_to, dts, 3.4f, 0.34f);
    spring(&f->rise, &f->rise_v, f->rise_to, dts, 2.6f, 0.5f);
    spring(&f->brow, &f->brow_v, 0, dts, 2.4f, 0.55f);
    out->rise += f->rise;
    out->brow += f->brow;
    out->squash = clampf(out->squash + f->squash, -0.35f, 0.40f);
    out->gaze_x = f->gaze_x;
    out->gaze_y = f->gaze_y;
    out->rise = clampf(out->rise, -14.0f, 80.0f);
    out->sway = clampf(out->sway, -10.0f, 10.0f);
    out->tilt = clampf(out->tilt, -7.0f, 7.0f);
    out->mouth_open = clampf(out->mouth_open, 0, 1);
    f->pupil = toward(f->pupil, (1.0f + 0.07f * f->heard) * (in->down ? 1.08f : 1.0f), dt, 120.0f);
    out->pupil = f->pupil;
    const float shade = overridden ? 0 : !strcmp(m->name, "sleepy") ? 0.35f : !strcmp(m->name, "offline") ? 0.25f : 0;
    f->shade = toward(f->shade, shade, dt, 600.0f);
    out->shade = f->shade;
    return overridden ? f->override_mood : LCD_FACE_MOOD_NONE;
}
