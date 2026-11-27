// SPDX-License-Identifier: MIT
/* What the character does: the pose each mood asks for, how conversation
 * phases and device conditions bend it, the one-shot reactions, touch, and
 * the small things she does on her own. Everything ends in a target pose;
 * springs carry the pose there. What can only change in one step (a picture,
 * the eyes of another mood) changes behind shut lids or at the bottom of a dip.
 *
 * What she speaks with besides her face depends on the figure the sprite pack
 * brings. With animal ears and a tail: ears prick up to listen, flop when she
 * is sad or offline and fold when she is being silly, the tail wags with her
 * spirits. With arms: she waves, cheers, offers a hand. With a head that
 * turns: she looks round, shakes her head. The pose always carries every
 * channel; a part the figure does not have is simply never drawn. */
#include "chr_internal.h"
#include "chr_sprites.h"

#include <math.h>
#include <string.h>

#if CHR_MARKS
uint32_t chr_mark_ms;
#endif

#define PI 3.14159265f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }
/* 0 before `from`, 1 after `to`, smooth between. */
static float ramp(float t, float from, float to)
{
    if (t <= from) return 0.0f;
    if (t >= to) return 1.0f;
    const float u = (t - from) / (to - from);
    return u * u * (3.0f - 2.0f * u);
}
/* Rises over `in`, holds, falls over the last `out` of [0, 1]. */
static float window(float u, float in, float out)
{
    return ramp(u, 0.0f, in) * (1.0f - ramp(u, 1.0f - out, 1.0f));
}

uint32_t chr_random(chr_t *c)
{
    uint32_t x = c->random;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return c->random = x;
}

float chr_uniform(chr_t *c, float from, float to)
{
    return from + (to - from) * (float)(chr_random(c) >> 8) / 16777216.0f;
}

/* What she is doing in a talk. While its last words are still heard she is speaking them, whatever
 * the page says by then. */
static uint8_t talk_phase(const chr_t *c) { return c->parting ? (uint8_t)CHR_PHASE_SPEAKING : c->phase; }

/* ------------------------------------------------------------------- moods */

/* What a mood keeps doing for as long as it lasts. */
enum { M_HEARTS = 1u << 0, M_ZED = 1u << 1, M_NOTES = 1u << 2, M_TEARS = 1u << 3, M_SWEAT = 1u << 4,
       M_SPARKS = 1u << 5, M_QUESTION = 1u << 6, M_EXCLAIM = 1u << 7, M_BOUNCE = 1u << 8, M_SWAY = 1u << 9,
       M_SHAKE = 1u << 10, M_PUFF = 1u << 11,
       M_SMILES = 1u << 12 /* now and then the eyes close in a smile */,
       M_OPEN = 1u << 17 /* the smile is an open one, also in a talk */,
       M_LAUGH = 1u << 18 /* a flat figure rests in an open smile */ };

/* The pictures of a flat figure's face that make a mood (LOOK_*): her eyes as the mood comes and once it has stayed
 * (MOOD_FULL_MS), her brows, the mouth she rests in, and how much she blushes. A mood without one is her own face with
 * the mood's blush. Other figures keep the mood's numbers. */
typedef struct { uint8_t eyes, full, brows, mouth; float blush; } face_t;

typedef struct {
    const char *name;
    float eol, eor, arc, slant, brow;     /* eyes and brows */
    float mc, mo, blush;                  /* mouth and cheeks */
    float hy, lean, gx, gy;               /* head */
    float ear_l, ear_r, wag;              /* ears and tail */
    uint32_t flags;
    face_t face;
} mood_t;

#define PLAIN_FACE {0, 0, 0, 0, 0}

static const mood_t moods[CHR_MOOD_COUNT] = {
    {"neutral", 1, 1, 0, 0, 0, .6f, 0, 0, 0, 0, 0, 0, 0, 0, .25f, 0, PLAIN_FACE},
    /* Soft eyes once she has been happy for a moment. */
    {"happy", 1, 1, 0, 0, 2, 1, .3f, .3f, -2, 0, 0, 0, 0, 0, .8f, M_SMILES, {LOOK_PLAIN, LOOK_SOFT, 0, 0, .25f}},
    {"laughing", 1, 1, 1, 0, 3, 1, .9f, .3f, 0, 0, 0, 0, .1f, .1f, 1.2f, M_BOUNCE | M_LAUGH, {LOOK_GLAD, LOOK_GLAD, 0, 0, .3f}},
    {"funny", 1, 0, 0, 0, 2, 1, .5f, .15f, 0, 9, 0, 0, -.3f, .3f, .8f, 0, PLAIN_FACE},
    /* Hurt, and a little cross: eyes cast down, brows drawn together, the corners of the mouth down or a pout. */
    {"sad", .7f, .7f, 0, -.9f, -2, -1, 0, 0, 6, 0, 0, .4f, 1, 1, 0, 0, {LOOK_DOWN, LOOK_DOWN, LOOK_COMFORT, LOOK_FROWN, .3f}},
    {"angry", .8f, .8f, 0, 1, -3, -1, 0, .2f, 0, 0, 0, 0, .7f, .7f, 0, M_SHAKE | M_PUFF, {LOOK_DOWN, LOOK_DOWN, LOOK_COMFORT, 0, .3f}},
    {"crying", .45f, .45f, 0, -1, -2, -1, .5f, .3f, 7, 0, 0, .3f, 1, 1, 0, M_TEARS, {LOOK_TEARY, LOOK_TEARY, LOOK_COMFORT, LOOK_FROWN, .3f}},
    {"loving", 1, 1, 1, 0, 2, 1, .25f, .8f, 0, 0, 0, 0, 0, 0, 1, M_HEARTS, {LOOK_SOFT, LOOK_GLAD, 0, 0, .5f}},
    {"embarrassed", .7f, .7f, 0, -.4f, 0, -.2f, 0, 1, 3, -5, .6f, .3f, .8f, .8f, .1f, M_SWEAT, {LOOK_DOWN, LOOK_DOWN, 0, LOOK_SMILE, .5f}},
    {"surprised", 1.2f, 1.2f, 0, 0, 7, -1, .7f, 0, -6, 0, 0, 0, -.2f, -.2f, 0, M_EXCLAIM, PLAIN_FACE},
    {"shocked", 1.3f, 1.3f, 0, 0, 9, -1, 1, 0, -9, 0, 0, 0, 1, 1, 0, M_EXCLAIM | M_SHAKE, PLAIN_FACE},
    {"thinking", .9f, .9f, 0, .15f, 2, 0, 0, 0, 0, 6, -.6f, .15f, -.2f, .4f, .15f, 0, PLAIN_FACE},
    {"winking", 1, 0, 0, 0, 2, 1, .25f, .2f, 0, 7, 0, 0, 0, -.3f, .8f, M_SPARKS, PLAIN_FACE},
    {"cool", .6f, .6f, 0, .25f, 0, .6f, 0, 0, 0, -6, .25f, 0, 0, 0, .2f, 0, PLAIN_FACE},
    {"relaxed", 1, 1, -1, 0, 0, .6f, 0, .1f, 3, 0, 0, 0, .35f, .35f, .3f, 0, PLAIN_FACE},
    {"delicious", 1, 1, 1, 0, 2, 1, .5f, .4f, 0, 0, 0, 0, 0, 0, 1.1f, M_SPARKS, {LOOK_GLAD, LOOK_GLAD, 0, 0, .4f}},
    {"kissy", 1, 1, -1, 0, 1, -1, .25f, .5f, 0, 7, 0, 0, 0, 0, .6f, M_HEARTS, PLAIN_FACE},
    /* Pleased with herself: soft eyes and a smile higher on one side. */
    {"confident", .82f, .82f, 0, .4f, 0, 1, .1f, 0, -3, 0, 0, 0, -.15f, -.15f, .5f, 0, {LOOK_SOFT, LOOK_SOFT, 0, LOOK_SMIRK, .1f}},
    {"sleepy", .3f, .3f, 0, 0, -1, 0, 0, 0, 9, 0, 0, .3f, .6f, .6f, 0, M_ZED, PLAIN_FACE},
    {"silly", 1, .5f, 0, 0, 3, 1, .7f, .15f, 0, -10, -.3f, 0, -1, .6f, 1, 0, PLAIN_FACE},
    {"confused", 1, .9f, 0, .1f, 5, -.1f, 0, 0, 0, 9, .5f, -.1f, .6f, -.3f, .1f, M_QUESTION, PLAIN_FACE},
    {"listening", 1.1f, 1.1f, 0, 0, 3, .3f, 0, 0, -2, 6, 0, 0, -.25f, -.25f, .3f, 0, PLAIN_FACE},
    {"ambient", .85f, .85f, 0, 0, 0, .45f, 0, 0, 0, 0, -.4f, -.35f, 0, 0, .35f, M_NOTES | M_SWAY, PLAIN_FACE},
    /* Of a conversation. */
    {"excited", 1, 1, 0, 0, 5, 1, .25f, .4f, 0, 0, 0, 0, 0, 0, 1, M_OPEN | M_LAUGH, {LOOK_BRIGHT, LOOK_BRIGHT, 0, 0, .3f}},
    /* Apart from her own face: soft eyes, her smile, a little blush. */
    {"gentle", 1, 1, 0, 0, 0, .7f, 0, .25f, 0, 0, 0, 0, .2f, .2f, .3f, 0, {LOOK_SOFT, LOOK_SOFT, 0, 0, .2f}},
    /* Soft eyes, and tears in them once she has comforted you for a moment. */
    {"comfort", 1, 1, 0, -.2f, 2, .55f, 0, .25f, 0, 0, 0, 0, .3f, .3f, .2f, 0, {LOOK_SOFT, LOOK_TEARY, LOOK_COMFORT, LOOK_COMFORT, .1f}},
    {"serious", 1, 1, 0, .15f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, .1f, 0, {LOOK_SERIOUS, LOOK_SERIOUS, LOOK_SERIOUS, LOOK_SERIOUS, 0}},
    /* Touched by something: tears in her eyes, and a smile. Another figure is loving. */
    {"moved", 1, 1, 1, 0, 2, 1, .25f, .8f, 0, 0, 0, 0, 0, 0, 1, M_HEARTS, {LOOK_TEARY, LOOK_TEARY, LOOK_COMFORT, LOOK_SMILE, .2f}},
};

static const struct { const char *alias, *name; } aliases[] = {
    {"calm", "neutral"}, {"curious", "confused"}, {"proud", "confident"}, {"speech", "listening"},
    /* States that a page names (xiaotai/view.h); what she makes of a lost network is a condition of hers. */
    {"ringing", "surprised"}, {"dialing", "listening"}, {"offline", "neutral"}, {"error", "sad"},
};

const char *chr_mood_name(size_t index) { return index < CHR_MOOD_COUNT ? moods[index].name : NULL; }

size_t chr_mood_find(const char *name)
{
    if (!name) return CHR_MOOD_NEUTRAL;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i)
        if (!strcmp(name, aliases[i].alias)) name = aliases[i].name;
    for (size_t i = 0; i < CHR_MOOD_COUNT; ++i)
        if (!strcmp(name, moods[i].name)) return i;
    return CHR_MOOD_NEUTRAL;
}

/* -------------------------------------------------------------------- cues */

static const struct { const char *name; uint16_t ms; } cues[CHR_CUE_COUNT] = {
    [CHR_CUE_GREET] = {"greet", 2400},     [CHR_CUE_WAKE] = {"wake", 900},
    [CHR_CUE_STARTLE] = {"startle", 800},  [CHR_CUE_BYE] = {"bye", 1600},
    [CHR_CUE_VOLUME] = {"volume", 1200},   [CHR_CUE_ERROR] = {"error", 1200},
    [CHR_CUE_NOD] = {"nod", 700},          [CHR_CUE_STRETCH] = {"stretch", 1900},
    [CHR_CUE_CHEER] = {"cheer", 1400},     [CHR_CUE_SHIVER] = {"shiver", 900},
    [CHR_CUE_POKE] = {"poke", 600},        [CHR_CUE_DIZZY] = {"dizzy", 2300},
    [CHR_CUE_POUT] = {"pout", 2700},       [CHR_CUE_FLICK] = {"flick", 900},
    [CHR_CUE_TAIL] = {"tail", 1500},       [CHR_CUE_SWAY] = {"sway", 900},
    [CHR_CUE_JUMP] = {"jump", 820},        [CHR_CUE_DUCK] = {"duck", 1100},
    [CHR_CUE_LOOK] = {"look", 950},        [CHR_CUE_HOP] = {"hop", 620},
    [CHR_CUE_HUM] = {"hum", 2700},         [CHR_CUE_YAWN] = {"yawn", 2100},
    [CHR_CUE_EARS] = {"ears", 1500},       [CHR_CUE_WAG] = {"wag", 1600},
    [CHR_CUE_TWITCH] = {"twitch", 700},   [CHR_CUE_OFFER] = {"offer", 2600},
    [CHR_CUE_HIGHFIVE] = {"highfive", 1100}, [CHR_CUE_WAVE] = {"wave", 2200},
    [CHR_CUE_GLANCE] = {"glance", 1500},   [CHR_CUE_NOD_OFF] = {"nodoff", 2600},
    [CHR_CUE_DANCE] = {"dance", 8700},
};

const char *chr_cue_name(chr_cue_t cue) { return cue > CHR_CUE_NONE && cue < CHR_CUE_COUNT ? cues[cue].name : NULL; }

chr_cue_t chr_cue_find(const char *name)
{
    for (int i = 1; name && i < CHR_CUE_COUNT; ++i)
        if (!strcmp(name, cues[i].name)) return (chr_cue_t)i;
    return CHR_CUE_NONE;
}

static void burst(chr_t *c, chr_fx_t kind, int count, float x, float y, float speed, float size)
{
    for (int i = 0; i < count; ++i) {
        const float angle = chr_uniform(c, -PI, 0.0f);
        chr_emit(c, kind, x + chr_uniform(c, -18, 18), y + chr_uniform(c, -12, 12), cosf(angle) * speed,
                 sinf(angle) * speed - 30.0f, size * chr_uniform(c, 0.8f, 1.2f), chr_uniform(c, 700, 1100));
    }
}

/* ----------------------------------------------------------------- effects */

/* What happens gets an effect of pictures around her, where the pack has them (one of a few seconds at most, bright
 * as it comes and gone cleanly); a figure without them keeps to the few marks it draws itself. Places are pixels of
 * the figure from her feet, sizes how much larger than the pictures were baked. Nothing is put on her face (chr_draw.c
 * leaves out what passes over it), and a glow is behind her. */
static bool fancy(const chr_t *c) { return chr_fx_drawn(c, FX_SPARK); }

static float either(chr_t *c) { return chr_random(c) & 1U ? 1.0f : -1.0f; }
/* Where the effects are put: the middle of her face, her eyes, the top of her hair, her chest. */
static float face_x(const chr_t *c) { return (float)c->fig.face_x; }
static float top_y(const chr_t *c) { return (float)c->fig.hair_top + 30.0f; }
static float chest_y(const chr_t *c) { return (float)c->fig.neck_y + 100.0f; }

/* Greeted, woken, good morning: a glow behind her, sparkles beside her head one after the other, petals falling past
 * her on both sides from above. */
static void fx_greet(chr_t *c)
{
    const float x = face_x(c), top = -c->anchor_y - 20.0f;
    /* The glow is behind her head and shoulders and reaches up to the top of her hair, whatever size its picture was
     * baked at. */
    chr_sprite_t picture;
    const float middle = (float)c->fig.eye_y + 60.0f;
    const float wide = chr_pack_sprite(&c->pack, chr_pack_slot(&c->pack, SL_FX + FX_AURA * CHR_FX_FRAMES), &picture) && picture.width
                           ? 2.0f * (middle - chr_mark_top(c)) / (float)picture.width : middle - chr_mark_top(c);
    chr_particle_t *glow = chr_spark(c, FX_AURA, x, middle, 0.0f, 2200.0f, wide);
    glow->fade_in = 400.0f;
    glow->fade_out = 600.0f;
    for (int k = 0; k < 6; ++k)
        chr_spark(c, k % 3 ? FX_SPARK : FX_SPARK_SKY, x + (k % 2 ? 1.0f : -1.0f) * chr_uniform(c, 150, 290), top_y(c) + chr_uniform(c, 0, 220),
                  250.0f + 180.0f * (float)k, 900.0f, chr_uniform(c, 0.8f, 1.33f));
    for (int k = 0; k < 6; ++k) {
        chr_particle_t *q = chr_spark(c, k % 3 ? FX_PETAL : FX_ROSE, x + (k % 2 ? 1.0f : -1.0f) * chr_uniform(c, 210, 340), top,
                                      100.0f + 300.0f * (float)k, 2800.0f, chr_uniform(c, 0.8f, 1.2f));
        q->vy = chr_uniform(c, 90, 150);
        q->vx = chr_uniform(c, -12, 12);
        q->sway = 14.0f;
        q->fade_out = 600.0f;
    }
}

/* Praised, loved, stroked: hearts rise beside her chest one after the other, pink lights float up, a few sparkles. */
static void fx_praise(chr_t *c)
{
    const float x = face_x(c), chest = chest_y(c);
    for (int k = 0; k < 10; ++k) {
        chr_particle_t *q = chr_spark(c, k % 3 ? FX_HEART : FX_HEART_SMALL, x + (k % 2 ? 1.0f : -1.0f) * chr_uniform(c, 120, 250),
                                      chest + chr_uniform(c, -40, 40), 250.0f + 130.0f * (float)k, 1800.0f, chr_uniform(c, 0.8f, 1.25f));
        q->vy = -chr_uniform(c, 80, 120);
        q->sway = 10.0f;
        q->pop = 0.3f;
        q->fade_out = 500.0f;
    }
    for (int k = 0; k < 10; ++k) {
        chr_particle_t *q = chr_spark(c, k % 2 ? FX_ORB_PINK : FX_ORB_LILAC, x + either(c) * chr_uniform(c, 140, 300),
                                      chest + chr_uniform(c, -200, 80), 200.0f + 120.0f * (float)k, 2000.0f, chr_uniform(c, 0.7f, 1.15f));
        q->vy = -30.0f;
        q->sway = 6.0f;
        q->fade_out = 600.0f;
    }
    for (int k = 0; k < 4; ++k)
        chr_spark(c, FX_SPARK_SKY, x + either(c) * chr_uniform(c, 160, 260), (float)c->fig.eye_y + chr_uniform(c, -60, 80),
                  500.0f + 250.0f * (float)k, 800.0f, chr_uniform(c, 0.75f, 1.0f));
}

/* Poked: a sky-blue ring spreads from the finger, twice, and glints fly off. */
static void fx_poke(chr_t *c, float x, float y)
{
    chr_spark(c, FX_RING_SKY, x, y, 0.0f, 500.0f, 1.6f);
    chr_spark(c, FX_RING_SKY, x, y, 120.0f, 500.0f, 1.1f);
    for (int k = 0; k < 6; ++k) {
        const float angle = chr_uniform(c, -PI, PI);
        chr_particle_t *q = chr_spark(c, FX_GLINT, x, y, 50.0f, 600.0f, 1.05f);
        q->vx = 170.0f * cosf(angle);
        q->vy = 170.0f * sinf(angle);
    }
}

/* A high five: a burst of gold light and a ring where the hands met, ribbons flying off and falling, gold sparkles. */
static void fx_clap(chr_t *c, float x, float y)
{
    chr_spark(c, FX_BURST, x, y, 0.0f, 550.0f, 1.6f);
    chr_spark(c, FX_RING_GOLD, x, y, 0.0f, 550.0f, 1.4f);
    chr_particle_t *q;
    for (int k = 0; k < 26; ++k) {
        /* Up and away from her face, to the side the hand is on. */
        const float away = x < face_x(c) ? 1.0f : 0.0f;
        const float angle = away ? chr_uniform(c, -PI * 0.95f, -PI * 0.55f) : chr_uniform(c, -PI * 0.45f, -PI * 0.05f);
        const float speed = chr_uniform(c, 220, 420);
        q = chr_spark(c, FX_RIBBON, x, y, 50.0f, 1900.0f, chr_uniform(c, 0.8f, 1.2f));
        q->vx = speed * cosf(angle);
        q->vy = speed * sinf(angle);
        q->fall = 520.0f;
        q->fade_out = 500.0f;
    }
    for (int k = 0; k < 5; ++k)
        chr_spark(c, FX_SPARK, x + chr_uniform(c, -120, 120), y + chr_uniform(c, -120, 60), 100.0f + 100.0f * (float)k, 800.0f,
                  chr_uniform(c, 0.8f, 1.13f));
}

/* Good news (back on the network, a cheer): ribbons and petals falling from above on both sides, gold sparkles. */
static void fx_cheer(chr_t *c)
{
    const float x = face_x(c), top = -c->anchor_y - 20.0f;
    for (int k = 0; k < 22; ++k) {
        chr_particle_t *q = chr_spark(c, k % 3 ? FX_RIBBON : FX_PETAL, x + (k % 2 ? 1.0f : -1.0f) * chr_uniform(c, 200, 360), top,
                                      70.0f * (float)k, 2000.0f + chr_uniform(c, 0, 400), chr_uniform(c, 0.8f, 1.15f));
        q->vy = chr_uniform(c, 140, 210);
        q->vx = chr_uniform(c, -15, 15);
        q->sway = 10.0f;
        q->fade_out = 500.0f;
    }
    for (int k = 0; k < 6; ++k)
        chr_spark(c, FX_SPARK, x + (k % 2 ? 1.0f : -1.0f) * chr_uniform(c, 150, 290), top_y(c) + chr_uniform(c, -40, 200),
                  150.0f + 150.0f * (float)k, 900.0f, chr_uniform(c, 0.8f, 1.2f));
}

/* Surprised: a gold "!" pops up beside her head, with a ring and a burst of light. */
static void fx_surprise(chr_t *c)
{
    const float x = face_x(c) + CHR_SIDE * 190.0f, y = chr_mark_top(c) + 100.0f;
    chr_particle_t *q = chr_spark(c, FX_EXCLAIM, x, y, 0.0f, 1200.0f, 1.0f);
    q->pop = 0.4f;
    chr_spark(c, FX_RING_GOLD, x, y + 4.0f, 0.0f, 550.0f, 1.2f);
    chr_spark(c, FX_BURST, x, y + 4.0f, 0.0f, 500.0f, 1.0f);
}

/* A beat of the dance: a light rises beside her (sky, pink, gold, lilac in turn) and a glint on the other side; every
 * fourth beat a note, at the start of a phrase sparkles. */
static void fx_beat(chr_t *c, unsigned beat)
{
    static const uint8_t lights[4] = {FX_ORB_SKY, FX_ORB_PINK, FX_ORB_GOLD, FX_ORB_LILAC};
    const float x = face_x(c), s = beat % 2U ? 1.0f : -1.0f;
    chr_particle_t *q = chr_spark(c, (chr_fx_t)lights[beat % 4U], x + s * chr_uniform(c, 150, 260), chest_y(c) + chr_uniform(c, -20, 80),
                                  0.0f, 1900.0f, chr_uniform(c, 0.85f, 1.3f));
    q->vy = -80.0f;
    q->sway = 10.0f;
    q->fade_out = 600.0f;
    chr_spark(c, FX_GLINT, x - s * chr_uniform(c, 170, 280), (float)c->fig.eye_y + chr_uniform(c, -80, 140), 0.0f, 600.0f, 1.0f);
    if (beat % 4U == 0U) {
        q = chr_spark(c, beat % 8U ? FX_NOTE2 : FX_NOTE, x + s * 210.0f, (float)c->fig.eye_y - 40.0f, 0.0f, 1900.0f, 1.0f);
        q->vy = -45.0f;
        q->vx = s * 10.0f;
        q->sway = 6.0f;
        q->fade_out = 600.0f;
    }
    if (beat % 8U == 0U)
        for (int k = 0; k < 5; ++k)
            chr_spark(c, k % 2 ? FX_SPARK : FX_SPARK_SKY, x + either(c) * chr_uniform(c, 160, 300), top_y(c) + chr_uniform(c, -20, 120),
                      100.0f * (float)k, 800.0f, chr_uniform(c, 0.8f, 1.2f));
}

/* What sends all of her off in a direction; a figure in one piece goes a small part of the way. */
static void push(chr_t *c, unsigned channel, float velocity)
{
    c->v[channel] += (c->features & CHR_FLAT) ? velocity * CHR_FLAT_PUSH : velocity;
}

void chr_cue(chr_t *c, chr_cue_t cue, int32_t value)
{
    if (!c || cue <= CHR_CUE_NONE || cue >= CHR_CUE_COUNT) return;
    /* In a talk only her face moves: what the network does is shown afterwards, or not at all. */
    if ((cue == CHR_CUE_CHEER || cue == CHR_CUE_SHIVER) && talk_phase(c) != CHR_PHASE_IDLE) return;
    /* A sulk is not interrupted by more poking, only by the device or a stroke. */
    if (c->cue == CHR_CUE_POUT && (cue == CHR_CUE_POKE || cue == CHR_CUE_DIZZY || cue >= CHR_CUE_HOP)) return;
    c->cue = (uint8_t)cue;
    c->cue_age = 0.0f;
    c->cue_len = (float)cues[cue].ms;
    c->cue_value = value;
    c->cue_dir = value < 0 ? -1.0f : 1.0f;
    c->idle_in = chr_uniform(c, 7000, 15000);
    const bool flat = (c->features & CHR_FLAT) != 0, shown = fancy(c) && talk_phase(c) == CHR_PHASE_IDLE;
    /* A flat figure dances two phrases of eight beats; a moment to herself is a few seconds with her hands clasped. */
    if (flat && cue == CHR_CUE_DANCE) c->cue_len = 10500.0f;
    if (flat && cue == CHR_CUE_GLANCE) c->cue_len = 3200.0f;
    switch (cue) {
    case CHR_CUE_GREET:
        /* Up from below; a figure in one piece only straightens up. */
        if (flat) push(c, P_BY, 240.0f);
        else { c->p[P_BY] = 260.0f; c->v[P_BY] = -500.0f; }
        c->v[P_EAR_L] -= 6; c->v[P_EAR_R] -= 6;
        if (shown) fx_greet(c);
        break;
    case CHR_CUE_WAKE:
        push(c, P_BY, -220.0f); c->v[P_HY] -= 70.0f; c->v[P_EAR_L] -= 7; c->v[P_EAR_R] -= 7;
        burst(c, FX_SPARK, 3, c->fig.face_x, c->fig.hair_top + 40, 90, 14);
        break;
    case CHR_CUE_STARTLE:
        c->v[P_HY] -= 120.0f; c->v[P_EAR_L] += 9; c->v[P_EAR_R] += 9;
        if (shown) fx_surprise(c);
        else if (talk_phase(c) == CHR_PHASE_IDLE) chr_emit(c, FX_EXCLAIM, chr_side_x(c) + CHR_SIDE * 14, c->fig.hair_top + 70, CHR_SIDE * 20, -70, 34, 800);
        break;
    case CHR_CUE_STRETCH:
        /* Woken: good morning. */
        if (shown) fx_greet(c);
        break;
    case CHR_CUE_ERROR:
    case CHR_CUE_SHIVER: chr_emit(c, FX_DROP, chr_side_x(c) + CHR_SIDE * (fancy(c) ? 10.0f : -26.0f), c->fig.brow_y - 14, CHR_SIDE * 12, 26, 17, 950); break;
    case CHR_CUE_CHEER:
        /* A flat figure keeps her feet on the ground: the effect is the cheer. */
        if (!flat) push(c, P_BY, -420.0f);
        c->v[P_HY] -= 60.0f;
        if (shown) fx_cheer(c);
        else burst(c, FX_SPARK, 4, c->fig.face_x, c->fig.hair_top + 40, 120, 15);
        break;
    case CHR_CUE_POKE:
        c->v[P_HY] += 150.0f; push(c, P_LEAN, -c->cue_dir * 160.0f);
        c->v[c->cue_dir < 0 ? P_EAR_R : P_EAR_L] += 8;
        if (shown) fx_poke(c, c->touch_x, c->touch_y);
        break;
    case CHR_CUE_DIZZY: burst(c, FX_SPARK, 3, c->fig.face_x, c->fig.hair_top + 40, 60, 13); break;
    case CHR_CUE_POUT:
        c->sulk_ms = (float)cues[cue].ms;
        chr_emit(c, FX_PUFF, (float)c->fig.face_x - c->cue_dir * (float)(c->fig.head_half + 6), c->fig.brow_y, -c->cue_dir * 50, -40, 20, 900);
        break;
    case CHR_CUE_FLICK: c->v[c->cue_dir < 0 ? P_EAR_R : P_EAR_L] -= 14; push(c, P_LEAN, c->cue_dir * 90.0f); break;
    case CHR_CUE_TAIL:
        push(c, P_BY, -380.0f); c->v[P_HY] -= 90.0f; c->v[P_EAR_L] += 10; c->v[P_EAR_R] += 10;
        chr_emit(c, FX_EXCLAIM, chr_side_x(c) + CHR_SIDE * 14, c->fig.hair_top + 70, CHR_SIDE * 20, -70, 34, 800);
        break;
    case CHR_CUE_JUMP: push(c, P_BY, -900.0f); c->v[P_HY] -= 80.0f; break;
    case CHR_CUE_HOP:
        if (c->features & CHR_FLAT) c->cue_len = 1100.0f;      /* a figure in one piece smiles instead */
        else { c->v[P_BY] -= 380.0f; c->v[P_HY] -= 40.0f; }
        break;
    case CHR_CUE_SWAY: push(c, P_LEAN, c->cue_dir * 420.0f); push(c, P_BX, c->cue_dir * 260.0f); break;
    case CHR_CUE_TWITCH: c->v[chr_random(c) & 1U ? P_EAR_L : P_EAR_R] += (chr_random(c) & 1U) ? 9.0f : -7.0f; break;
    case CHR_CUE_HIGHFIVE: {
        /* The clap: sparks where the hands met, and a hop (a flat figure raises her palm, which is all). */
        const bool right = c->cue_dir < 0 || !c->fig.hand_lx;
        if (shown) fx_clap(c, face_x(c) - 150.0f, (float)c->fig.eye_y + 60.0f);
        else burst(c, FX_SPARK, 5, right ? c->fig.hand_rx : c->fig.hand_lx, right ? c->fig.hand_ry : c->fig.hand_ly, 130, 14);
        if (!flat) push(c, P_BY, -300.0f);
        break;
    }
    default: break;
    }
    /* What a reaction does to how she feels: a clap and a greeting returned bring her closer, being
     * poked silly or into a sulk a little less so. */
    if (cue == CHR_CUE_HIGHFIVE) c->closeness = minf(c->closeness + 0.06f, 1.0f);
    else if (cue == CHR_CUE_DIZZY) c->closeness = maxf(c->closeness - 0.03f, 0.0f);
    else if (cue == CHR_CUE_POUT) c->closeness = maxf(c->closeness - 0.08f, 0.0f);
}

static void both_eyes(float *T, float open, float weight)
{
    T[P_EOL] = mixf(T[P_EOL], open, weight);
    T[P_EOR] = mixf(T[P_EOR], open, weight);
}

static void both_ears(float *T, float ear, float weight)
{
    T[P_EAR_L] = mixf(T[P_EAR_L], ear, weight);
    T[P_EAR_R] = mixf(T[P_EAR_R], ear, weight);
}

/* What the running reaction adds to the target. */
static void react(chr_t *c, float *T)
{
    const float t = c->cue_age / c->cue_len, age = c->cue_age, dir = c->cue_dir;
    const float in = window(t, 0.12f, 0.25f);
    const bool flat = (c->features & CHR_FLAT) != 0;
    switch (c->cue) {
    case CHR_CUE_GREET: {
        const float wave = ramp(age, 500, 800) * (1.0f - ramp(age, 1900, 2300));
        T[P_ARC] = mixf(T[P_ARC], 1.0f, wave); T[P_MC] = mixf(T[P_MC], 1.0f, in); T[P_MO] = mixf(T[P_MO], 0.6f, in);
        T[P_LEAN] += wave * 9.0f * sinf(age * 0.011f); T[P_WAG] = mixf(T[P_WAG], 1.4f, in);
        T[P_EAR_L] += wave * 0.3f * sinf(age * 0.022f); T[P_EAR_R] -= wave * 0.3f * sinf(age * 0.022f);
        T[P_BROW] += 3.0f * in;
        break;
    }
    case CHR_CUE_WAKE:
        /* Her eyes light up. */
        both_eyes(T, flat ? 1.1f : 1.25f, in); both_ears(T, -0.3f, in); T[P_BROW] += 5.0f * in;
        if (!flat) T[P_MO] = maxf(T[P_MO], 0.3f * in);
        T[P_WAG] += 0.6f * in;
        break;
    case CHR_CUE_STARTLE:
        both_eyes(T, 1.3f, in); T[P_ARC] = mixf(T[P_ARC], 0, in); T[P_BROW] += 8.0f * in;
        T[P_MC] = mixf(T[P_MC], -1.0f, in); T[P_MO] = mixf(T[P_MO], flat ? 1.0f : 0.55f, in);
        T[P_HY] -= 5.0f * in; T[P_WAG] = mixf(T[P_WAG], 0, in);
        break;
    case CHR_CUE_BYE: {
        const float wave = window(t, 0.2f, 0.3f);
        T[P_ARC] = mixf(T[P_ARC], 1.0f, wave); T[P_MC] = mixf(T[P_MC], 1.0f, wave);
        T[P_LEAN] += wave * 8.0f; T[P_HY] += 4.0f * wave; T[P_WAG] = mixf(T[P_WAG], 0.9f, wave);
        break;
    }
    case CHR_CUE_VOLUME: {
        const float loud = clampf((float)c->cue_value / 100.0f, 0, 1);
        /* In a talk the mouth is for what she says. */
        if (talk_phase(c) == CHR_PHASE_IDLE) {
            T[P_MC] = mixf(T[P_MC], -1.0f, in); T[P_MO] = mixf(T[P_MO], loud < 0.02f ? 0.0f : 0.25f + 0.5f * loud, in);
        }
        T[P_GX] = mixf(T[P_GX], 0.7f, in); T[P_BROW] += 4.0f * in * loud;
        both_ears(T, mixf(-0.3f, 0.7f, ramp(loud, 0.6f, 1.0f)), in);
        break;
    }
    case CHR_CUE_ERROR:
    case CHR_CUE_SHIVER:
        both_eyes(T, 0.6f, in); T[P_SLANT] = mixf(T[P_SLANT], -0.7f, in); T[P_ARC] = mixf(T[P_ARC], 0, in);
        T[P_MC] = mixf(T[P_MC], -1.0f, in); T[P_MO] = mixf(T[P_MO], 0.15f, in);
        both_ears(T, 0.9f, in); T[P_WAG] = mixf(T[P_WAG], 0, in);
        break;
    case CHR_CUE_NOD:
        /* (A figure with a picture of a nod nods with that.) */
        if (c->pose != CHR_GESTURE_NOD) T[P_HY] += 9.0f * fabsf(sinf(t * PI * 2.0f));
        T[P_ARC] = mixf(T[P_ARC], 1.0f, in);
        T[P_MC] = mixf(T[P_MC], 1.0f, in);
        break;
    case CHR_CUE_STRETCH: {
        const float yawn = window(clampf(age / 1100.0f, 0, 1), 0.2f, 0.3f);
        T[P_ARC] = mixf(T[P_ARC], -1.0f, yawn); T[P_HY] -= 8.0f * yawn; T[P_LEAN] += 7.0f * yawn;
        T[P_MC] = mixf(T[P_MC], -1.0f, yawn); T[P_MO] = mixf(T[P_MO], 0.9f, yawn);
        both_ears(T, 0.8f, yawn); T[P_BROW] += 3.0f * yawn;
        const float smile = ramp(age, 1200, 1500) * in;
        T[P_MC] = mixf(T[P_MC], 1.0f, smile); T[P_WAG] = mixf(T[P_WAG], 1.0f, smile);
        T[P_EAR_L] += 0.3f * smile * sinf(age * 0.03f); T[P_EAR_R] -= 0.3f * smile * sinf(age * 0.03f);
        break;
    }
    case CHR_CUE_CHEER:
        T[P_ARC] = mixf(T[P_ARC], 1.0f, in); T[P_MC] = mixf(T[P_MC], 1.0f, in); T[P_MO] = mixf(T[P_MO], 0.8f, in);
        T[P_WAG] = mixf(T[P_WAG], 1.5f, in); both_ears(T, -0.2f, in); T[P_BROW] += 3.0f * in;
        break;
    case CHR_CUE_POKE: {
        const float wince = 1.0f - ramp(age, 140, 320);
        both_eyes(T, mixf(1.15f, 0.15f, wince), in);
        T[P_MC] = mixf(T[P_MC], -1.0f, in); T[P_MO] = mixf(T[P_MO], 0.3f, in);
        T[P_GX] = mixf(T[P_GX], dir * 0.8f, in); T[P_BLUSH] += 0.3f * in; T[P_BROW] += 3.0f * in;
        break;
    }
    case CHR_CUE_DIZZY: {
        const float turn = age * 0.0135f;
        T[P_GX] = mixf(T[P_GX], cosf(turn), in); T[P_GY] = mixf(T[P_GY], sinf(turn), in);
        T[P_EOL] = mixf(T[P_EOL], 0.65f + 0.35f * sinf(turn), in); T[P_EOR] = mixf(T[P_EOR], 0.65f - 0.35f * sinf(turn), in);
        T[P_LEAN] += in * 15.0f * sinf(age * 0.0062f); T[P_ARC] = mixf(T[P_ARC], 0, in);
        T[P_MC] = mixf(T[P_MC], -1.0f, in); T[P_MO] = mixf(T[P_MO], 0.3f, in);
        both_ears(T, -1.0f, in); T[P_WAG] = mixf(T[P_WAG], 0.2f, in);
        break;
    }
    case CHR_CUE_POUT:
        T[P_SLANT] = mixf(T[P_SLANT], 1.0f, in); both_eyes(T, 0.7f, in); T[P_ARC] = mixf(T[P_ARC], 0, in);
        T[P_GX] = mixf(T[P_GX], -dir * 0.95f, in); T[P_GY] = mixf(T[P_GY], -0.2f, in);
        T[P_MC] = mixf(T[P_MC], -1.0f, in); T[P_MO] = mixf(T[P_MO], 0, in); T[P_BROW] -= 3.0f * in;
        T[P_BLUSH] = mixf(T[P_BLUSH], 0.7f, in); both_ears(T, 0.8f, in); T[P_WAG] = mixf(T[P_WAG], 0, in);
        T[P_LEAN] += -dir * 8.0f * in;
        break;
    case CHR_CUE_FLICK:
        T[dir < 0 ? P_EOR : P_EOL] = mixf(T[dir < 0 ? P_EOR : P_EOL], 0.0f, in);
        T[P_MC] = mixf(T[P_MC], -1.0f, in); T[P_MO] = mixf(T[P_MO], 0.2f, in); T[P_BLUSH] += 0.3f * in;
        T[P_LEAN] += in * 6.0f * sinf(age * 0.03f);
        break;
    case CHR_CUE_TAIL:
        both_eyes(T, 1.3f, in); T[P_ARC] = mixf(T[P_ARC], 0, in); T[P_BROW] += 7.0f * in;
        T[P_MC] = mixf(T[P_MC], -1.0f, in); T[P_MO] = mixf(T[P_MO], 0.7f, in);
        T[P_BLUSH] = maxf(T[P_BLUSH], 0.9f * in); T[P_GX] = mixf(T[P_GX], 0.9f, in); T[P_GY] = mixf(T[P_GY], 0.6f, in);
        T[P_WAG] = mixf(T[P_WAG], 1.5f, ramp(age, 300, 600) * in); both_ears(T, 0.7f, in);
        break;
    case CHR_CUE_SWAY:
        T[P_ARC] = mixf(T[P_ARC], 1.0f, in); T[P_MC] = mixf(T[P_MC], 1, in); T[P_MO] = mixf(T[P_MO], 0.5f, in);
        T[P_GX] = mixf(T[P_GX], dir * 0.8f, in); T[P_WAG] = mixf(T[P_WAG], 1.2f, in);
        break;
    case CHR_CUE_JUMP:
        T[P_ARC] = mixf(T[P_ARC], 1.0f, in); T[P_MO] = mixf(T[P_MO], 0.7f, in); T[P_MC] = mixf(T[P_MC], 1, in);
        both_ears(T, -0.2f, in); T[P_WAG] = mixf(T[P_WAG], 1.3f, in);
        break;
    case CHR_CUE_DUCK: {
        const float down = window(t, 0.15f, 0.3f);
        T[P_BY] += (flat ? 8.0f : 130.0f) * down; T[P_GY] = mixf(T[P_GY], -0.8f, down); both_eyes(T, 1.15f, down);
        both_ears(T, 0.6f, down); T[P_BROW] += 4.0f * down;
        break;
    }
    case CHR_CUE_LOOK: {
        const float x = (float)(int16_t)(c->cue_value & 0xFFFF) / 100.0f;
        const float y = (float)(int16_t)((uint32_t)c->cue_value >> 16) / 100.0f;
        T[P_GX] = mixf(T[P_GX], x, in); T[P_GY] = mixf(T[P_GY], y, in);
        T[P_LEAN] += in * 9.0f * x; both_eyes(T, 1.12f, in); T[P_BROW] += 4.0f * in;
        if (flat) T[P_BROW] = maxf(T[P_BROW], 5.6f * in);       /* (past where a flat figure shows raised brows) */
        T[P_YAW] = mixf(T[P_YAW], x < -0.45f ? -1.0f : x > 0.45f ? 1.0f : 0.0f, in);
        T[x < 0 ? P_EAR_R : P_EAR_L] = mixf(T[x < 0 ? P_EAR_R : P_EAR_L], 0.45f, in);
        T[P_MC] = mixf(T[P_MC], -1.0f, in); T[P_MO] = mixf(T[P_MO], 0.2f, in);
        break;
    }
    case CHR_CUE_HOP:
        T[P_ARC] = mixf(T[P_ARC], 1.0f, in); T[P_MC] = maxf(T[P_MC], in); T[P_WAG] += 0.5f * in;
        break;
    case CHR_CUE_DANCE: {
        /* To a beat: she sways from side to side, comes off the ground between the beats and
         * gives a little on each. A pack with dance pictures also steps through them. */
        const float beat = age * (PI / 500.0f);
        const bool stepping = c->pose == CHR_GESTURE_DANCE && c->clipped;
        T[P_ARC] = mixf(T[P_ARC], 1.0f, in); T[P_MC] = mixf(T[P_MC], 1.0f, in); T[P_MO] = mixf(T[P_MO], flat ? 0.0f : 0.3f, in);
        /* A flat figure that steps through pictures is moved by them (finish()); one without sways a little, and
         * neither leaves the ground. */
        if (!(flat && stepping)) T[P_LEAN] += in * (flat ? 20.0f : 17.0f) * sinf(beat);
        if (!stepping && !flat) {
            /* No pictures to step through: she comes off the ground and gives on the beat herself. */
            T[P_BY] -= in * 12.0f * fabsf(sinf(beat));
            T[P_HY] += in * 2.5f * cosf(2.0f * beat);
        }
        T[P_WAG] = mixf(T[P_WAG], 1.2f, in);
        break;
    }
    case CHR_CUE_HUM:
        T[P_ARC] = mixf(T[P_ARC], 1.0f, in); T[P_LEAN] += in * 9.0f * sinf(age * 0.0058f);
        T[P_MC] = mixf(T[P_MC], 1.0f, in); T[P_MO] = mixf(T[P_MO], 0.15f + 0.12f * sinf(age * 0.011f), in);
        T[P_WAG] = mixf(T[P_WAG], 0.9f, in);
        break;
    case CHR_CUE_YAWN: {
        const float open = window(t, 0.3f, 0.35f);
        T[P_MC] = mixf(T[P_MC], -1.0f, open); T[P_MO] = mixf(T[P_MO], 1.0f, open);
        T[P_ARC] = mixf(T[P_ARC], -1.0f, open); T[P_BROW] += 3.0f * open;
        if (!flat) T[P_HY] -= 5.0f * open;
        both_ears(T, 0.7f, open);
        break;
    }
    case CHR_CUE_EARS: {
        const float beat = sinf(age * 0.021f);
        T[P_EAR_L] = mixf(T[P_EAR_L], 0.5f * beat, in); T[P_EAR_R] = mixf(T[P_EAR_R], -0.5f * beat, in);
        T[P_GX] = mixf(T[P_GX], dir * 0.9f, in); T[P_GY] = mixf(T[P_GY], -0.3f, in); T[P_LEAN] += dir * 5.0f * in;
        break;
    }
    case CHR_CUE_WAG:
        T[P_WAG] = mixf(T[P_WAG], 1.5f, in); T[P_ARC] = mixf(T[P_ARC], 1.0f, in); T[P_MC] = mixf(T[P_MC], 1.0f, in);
        T[P_LEAN] += in * 4.0f * sinf(age * 0.0165f);
        break;
    case CHR_CUE_TWITCH: T[P_GY] = mixf(T[P_GY], -0.5f, in); break;
    case CHR_CUE_OFFER:
        /* She looks at the hand she holds out, then at you, smiling. */
        T[P_GX] = mixf(T[P_GX], age < 700.0f ? dir * 0.8f : 0.0f, in); T[P_GY] = mixf(T[P_GY], age < 700.0f ? 0.3f : 0.0f, in);
        T[P_MC] = mixf(T[P_MC], 1.0f, in); T[P_MO] = mixf(T[P_MO], 0.3f, in); T[P_BROW] += 3.0f * in;
        T[P_LEAN] += dir * 5.0f * in;
        break;
    case CHR_CUE_HIGHFIVE:
        T[P_ARC] = mixf(T[P_ARC], 1.0f, in); T[P_MC] = mixf(T[P_MC], 1.0f, in); T[P_MO] = mixf(T[P_MO], 0.8f, in);
        T[P_BLUSH] = maxf(T[P_BLUSH], 0.4f * in); T[P_WAG] = mixf(T[P_WAG], 1.5f, in);
        break;
    case CHR_CUE_WAVE: {
        /* Leaning in to look for you, then the wave. */
        const float peer = window(clampf(age / 900.0f, 0, 1), 0.3f, 0.3f);
        T[P_HY] += 7.0f * peer; both_eyes(T, 1.2f, peer); T[P_BROW] += 4.0f * peer;
        T[P_GX] = mixf(T[P_GX], 0.0f, in); T[P_GY] = mixf(T[P_GY], 0.0f, in);
        T[P_MC] = mixf(T[P_MC], 1.0f, in); T[P_MO] = mixf(T[P_MO], 0.4f * ramp(age, 900, 1200), in);
        break;
    }
    case CHR_CUE_GLANCE: {
        const float out = window(t, 0.2f, 0.3f);
        T[P_GX] = mixf(T[P_GX], dir, out); T[P_YAW] = mixf(T[P_YAW], dir, out); T[P_LEAN] += dir * 6.0f * out;
        T[P_BROW] += 2.0f * out;
        break;
    }
    case CHR_CUE_NOD_OFF: {
        /* The eyes close and the head sinks; then she starts, and looks about as if nothing happened. */
        const float sink = ramp(age, 0, 1500) * (1.0f - ramp(age, 1650, 1800));
        const float start = ramp(age, 1650, 1800) * (1.0f - ramp(age, 2100, 2600));
        both_eyes(T, 0.0f, sink); T[P_LEAN] += 5.0f * sink;
        if (!flat) T[P_HY] += 12.0f * sink - 6.0f * start;
        T[P_MC] = mixf(T[P_MC], 0.0f, sink); both_eyes(T, 1.25f, start); T[P_BROW] += 5.0f * start;
        break;
    }
    default: break;
    }
}

/* ---------------------------------------------------------------- gestures */

/* The gesture the moment asks for. Reactions come first, then what the device is in, then the
 * conversation, then the mood. A conversation is had with the face: she leans in only when nothing
 * has been said for long, and ponders only when the answer takes a while. A mood shows in the whole
 * body only for a while after it arrives, and not at all when a conversation brought it. */
enum { THINK_AFTER_MS = 3000, LISTEN_AFTER_MS = 6000, MOOD_BODY_MS = 2600, THINK_UNHEARD_MS = 1500 };

static uint8_t gesture_wanted(const chr_t *c)
{
    const mood_t *m = &moods[c->mood];
    const float age = c->cue_age;
    switch (c->cue) {
    case CHR_CUE_GREET: if (age > 450.0f && age < 2300.0f) return CHR_GESTURE_WAVE; break;
    case CHR_CUE_STARTLE: if (age < 1000.0f && talk_phase(c) == CHR_PHASE_IDLE) return CHR_GESTURE_SURPRISE; break;
    case CHR_CUE_NOD: return CHR_GESTURE_NOD;
    case CHR_CUE_BYE: if (age > 150.0f && age < 1450.0f) return CHR_GESTURE_WAVE; break;
    case CHR_CUE_CHEER:
    case CHR_CUE_JUMP: return CHR_GESTURE_CHEER;
    case CHR_CUE_STRETCH: if (age < 1200.0f) return CHR_GESTURE_STRETCH; break;
    case CHR_CUE_POUT: return CHR_GESTURE_SULK;
    case CHR_CUE_OFFER:
    case CHR_CUE_HIGHFIVE: return CHR_GESTURE_HIGHFIVE;
    case CHR_CUE_WAVE: if (age > 800.0f && age < 2000.0f) return CHR_GESTURE_WAVE; break;
    case CHR_CUE_DANCE: if (age < c->cue_len - 500.0f) return CHR_GESTURE_DANCE; break;
    /* A flat figure has no eyes that look round: she stands with her hands as her picture of listening holds them. */
    case CHR_CUE_GLANCE: if ((c->features & CHR_FLAT) && age < c->cue_len - 600.0f) return CHR_GESTURE_LISTEN; break;
    default: break;
    }
    if (c->touch == CHR_TOUCH_HOLD) return CHR_GESTURE_SHY;
    if (c->touch == CHR_TOUCH_PET) return CHR_GESTURE_PET;
    if (c->conditions & CHR_ALARM) return CHR_GESTURE_CHEER;
    if (c->conditions & (CHR_RINGING | CHR_IN_CALL)) return CHR_GESTURE_NONE;
    if (c->phase == CHR_PHASE_THINKING) return c->phase_age > (float)THINK_AFTER_MS ? CHR_GESTURE_THINK : CHR_GESTURE_NONE;
    /* Still thinking as her answer begins to come, she puts her hand down as its first sound is heard; once its first
     * sentence has come she does not begin to. Nothing of it heard (the speaker is off), she does not wait long. */
    if (c->phase == CHR_PHASE_SPEAKING && c->gesture == CHR_GESTURE_THINK && c->answer_at < 0.0 &&
        c->phase_age < (float)THINK_UNHEARD_MS) return CHR_GESTURE_THINK;
    if (c->phase == CHR_PHASE_LISTENING) return c->leant ? CHR_GESTURE_LISTEN : CHR_GESTURE_NONE;
    if (talk_phase(c) != CHR_PHASE_IDLE || c->cue || c->touch != CHR_TOUCH_NONE ||
        (c->conditions & (CHR_OFFLINE | CHR_CONNECTING))) return CHR_GESTURE_NONE;
    if (!c->mood_talk && c->mood_age < (float)MOOD_BODY_MS) {
        if (m->flags & M_SWEAT) return CHR_GESTURE_SHY;
        if (m->flags & M_PUFF) return CHR_GESTURE_SULK;
        if (m->flags & M_QUESTION) return CHR_GESTURE_SHRUG;
        if (!strcmp(m->name, "thinking")) return CHR_GESTURE_THINK;
        if (c->mood_age < 1600.0f) {
            if (m->flags & M_HEARTS) return CHR_GESTURE_HEART;
            if (!strcmp(m->name, "laughing")) return CHR_GESTURE_LAUGH;
            if (!strcmp(m->name, "surprised") || !strcmp(m->name, "shocked")) return CHR_GESTURE_SURPRISE;
        }
    }
    return c->dozing ? CHR_GESTURE_SLEEP : CHR_GESTURE_NONE;
}

/* What a gesture adds to the pose: the arms and the turn of the head for a figure that has them. */
static void gesture_pose(const chr_t *c, float *T)
{
    const float w = ramp(c->gesture_ms, 0.0f, 180.0f), age = c->gesture_ms, away = c->cue_dir < 0 ? 1.0f : -1.0f;
    switch (c->gesture) {
    case CHR_GESTURE_WAVE:
        /* The arm goes up and swings between its two highest pictures; her head sways along, which is
         * all of the wave that a pose picture can move. */
        T[P_ARM_L] = mixf(T[P_ARM_L], 0.78f + 0.22f * sinf(age * 0.019f), w);
        T[P_LEAN] += w * 5.0f * sinf(age * 0.016f);
        break;
    case CHR_GESTURE_CHEER:
    case CHR_GESTURE_STRETCH:
        T[P_ARM_L] = mixf(T[P_ARM_L], 1.0f, w); T[P_ARM_R] = mixf(T[P_ARM_R], 1.0f, w);
        break;
    case CHR_GESTURE_THINK:
        T[P_ARM_R] = mixf(T[P_ARM_R], 0.5f, w); T[P_YAW] = mixf(T[P_YAW], T[P_GX] < 0 ? -1.0f : 1.0f, w);
        break;
    case CHR_GESTURE_SHY:
        T[P_YAW] = mixf(T[P_YAW], -1.0f, w); T[P_ARM_L] = mixf(T[P_ARM_L], 0.35f, w); T[P_ARM_R] = mixf(T[P_ARM_R], 0.35f, w);
        break;
    case CHR_GESTURE_SULK: T[P_YAW] = mixf(T[P_YAW], away, w); break;
    case CHR_GESTURE_HIGHFIVE: T[c->cue_dir < 0 ? P_ARM_R : P_ARM_L] = mixf(T[c->cue_dir < 0 ? P_ARM_R : P_ARM_L], 1.0f, w); break;
    case CHR_GESTURE_SHRUG:
        T[P_ARM_L] = mixf(T[P_ARM_L], 0.5f, w); T[P_ARM_R] = mixf(T[P_ARM_R], 0.5f, w);
        break;
    case CHR_GESTURE_DANCE:
        /* One arm up, then the other. */
        T[P_ARM_L] = mixf(T[P_ARM_L], 0.5f + 0.5f * sinf(age * (PI / 400.0f)), w);
        T[P_ARM_R] = mixf(T[P_ARM_R], 0.5f - 0.5f * sinf(age * (PI / 400.0f)), w);
        break;
    default: break;
    }
}

/* The sprite a flat figure shows for the pose it is in: the picture its clip is at (and the one
 * that picture is a patch over), or the one picture of that gesture. */
static void pose_show(chr_t *c)
{
    chr_frame_t frame;
    c->picture_under = CHR_NO_SPRITE;
    if (!c->pose) c->picture = CHR_NO_SPRITE;
    else if (c->clipped && chr_pack_frame(&c->pack, (unsigned)c->clip.first + c->clip_at, &frame)) {
        c->picture = frame.sprite;
        c->picture_under = frame.under;
    } else c->picture = (uint16_t)chr_pack_slot(&c->pack, SL_POSE + c->pose - 1U);
}

/* Whether two pictures (or one) are in memory; asks for them when they are not. What shows now is
 * kept. A pack that is in memory as a whole has every picture. */
static chr_shelf_state_t at_hand(const chr_t *c, unsigned picture, unsigned under)
{
    const chr_shelf_t *shelf = c->pack.shelf;
    if (!shelf) return CHR_SHELF_READY;
    const uint16_t ids[2] = {(uint16_t)picture, (uint16_t)under}, kept[2] = {c->picture, c->picture_under};
    return shelf->want(shelf->ctx, ids, 2, kept, 2);
}

static chr_shelf_state_t frame_at_hand(const chr_t *c, const chr_clip_t *clip, unsigned index)
{
    chr_frame_t frame;
    if (!chr_pack_frame(&c->pack, (unsigned)clip->first + index, &frame)) return CHR_SHELF_FAILED;
    return at_hand(c, frame.sprite, frame.under);
}

/* Whether the first picture of a clip only leads into the pose (an arm half way up, eyes shut). */
enum { PASSING_MS = 150 };
static bool leads_in(const chr_t *c, const chr_clip_t *clip)
{
    chr_frame_t frame;
    return clip->enter && chr_pack_frame(&c->pack, clip->first, &frame) && frame.ms && frame.ms <= PASSING_MS;
}

/* The first picture of a pose. One that is only led into, to a picture that cannot be had, cannot be
 * had either. */
static chr_shelf_state_t pose_at_hand(const chr_t *c, uint8_t pose)
{
    chr_clip_t clip;
    if (!chr_pack_clip(&c->pack, pose, &clip)) return at_hand(c, chr_pack_slot(&c->pack, SL_POSE + pose - 1U), CHR_NO_SPRITE);
    const chr_shelf_state_t ready = frame_at_hand(c, &clip, 0);
    /* What leads into a pose shows for a moment only: the picture after it is asked for as soon as the
     * first one is there (not along with it, or the first might have to wait for the second). */
    chr_frame_t first, second;
    if (ready == CHR_SHELF_READY && c->pack.shelf && clip.enter + clip.loop > 1 && chr_pack_frame(&c->pack, clip.first, &first) &&
        chr_pack_frame(&c->pack, clip.first + 1U, &second)) {
        const uint16_t ids[2] = {second.sprite, second.under}, kept[4] = {c->picture, c->picture_under, first.sprite, first.under};
        if (c->pack.shelf->want(c->pack.shelf->ctx, ids, 2, kept, 4) == CHR_SHELF_FAILED && leads_in(c, &clip)) return CHR_SHELF_FAILED;
    }
    return ready;
}

static void pose_enter(chr_t *c, uint8_t pose)
{
    CHR_MARK("POSE %u", (unsigned)pose);
    if (pose && talk_phase(c) != CHR_PHASE_IDLE) ++c->round_poses;
    /* Back from a picture with eyes of its own, her own eyes are shut and open from there. */
    if (!pose && c->pose) {
        c->rung[0] = c->rung[1] = c->aim[0] = c->aim[1] = RUNG_SHUT;
        c->rung_ms[0] = c->rung_ms[1] = 0.0f;
        c->face_cut = true;
    }
    c->pose = pose;
    c->pose_ms = 0.0f;
    c->clipped = pose && chr_pack_clip(&c->pack, pose, &c->clip);
    if (c->clipped && c->clip.style >= CHR_STYLE_COUNT) c->clip.style = CHR_STYLE_POP;
    c->leaving = false;
    c->entered = !c->clipped || !c->clip.enter;
    c->clip_at = 0;
    c->frame_ms = 0.0f;
    pose_show(c);
}

/* The numbers of the three ways from one picture to the next: she dips for dip_ms by dip_y of her
 * height (and widens by dip_x of her width) and the picture changes. From there she comes back to
 * her height within settle_ms, evenly and not past it. A beat of a dance instead starts her up_y
 * taller (up_x narrower) and lets her settle like a spring let go, `hz` times a second, fading by
 * `decay` a second; meanwhile she is off the ground by up to `hop` pixels for hop_ms. */
typedef struct { float dip_ms, dip_y, dip_x, settle_ms, up_y, up_x, hz, decay, hop, hop_ms; } style_t;
/* (She no longer dips: her eyes close slowly, the picture changes, they open.) */
static const style_t styles[CHR_STYLE_COUNT] = {
    [CHR_STYLE_POP] = {120.0f, 0.0f, 0.0f, 160.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    [CHR_STYLE_SOFT] = {160.0f, 0.0f, 0.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    [CHR_STYLE_BEAT] = {100.0f, -0.018f, 0.008f, 700.0f, 0.02f, -0.009f, 2.0f, 6.0f, 3.0f, 280.0f},
};
enum { CHANGE_POSE = 0, CHANGE_FRAME, CHANGE_LEAVE };
/* Lids that cover a change close in this time, stay shut for a moment once it has happened, and open. */
#define LID_SHUT_MS 70.0f
#define LID_HOLD_MS 30.0f
#define LID_OPEN_MS 110.0f
#define BLINK_SHUT_MS 60.0f        /* a blink closes a little quicker */
/* A change of the picture of all of her, and a slow blink of surprise: the eyes close in 90 ms and open in 140. */
#define SLOW_SHUT_MS 90.0f
#define SLOW_OPEN_MS 140.0f
static float lid_opens(float since) { return 1.0f - (since - LID_HOLD_MS) / LID_OPEN_MS; }

/* She changes into another picture of all of her with her eyes shut, and comes back to the base
 * picture as she left it; a pose picture has its own eyes. A blink covers a change of mood too. */
float chr_lids(const chr_t *c)
{
    float shut = 0.0f;
    if (c->blink_age >= 0.0f) {
        const float closing = c->slow_blink ? SLOW_SHUT_MS : BLINK_SHUT_MS, opening = c->slow_blink ? SLOW_OPEN_MS : LID_OPEN_MS;
        shut = c->blink_age < closing ? c->blink_age / closing : 1.0f - (c->blink_age - closing) / opening;
    }
    if (c->swap_ms >= 0.0f)
        shut = maxf(shut, c->swap_ms < LID_SHUT_MS ? c->swap_ms / LID_SHUT_MS : lid_opens(c->swap_ms - LID_SHUT_MS));
    if (c->change_ms >= 0.0f && c->change_kind == CHANGE_POSE && !c->pose) {
        const float dip = styles[c->change_style].dip_ms;
        /* (Shut a moment before the picture changes.) */
        shut = maxf(shut, c->changed ? 1.0f - (c->change_ms - dip) / SLOW_OPEN_MS
                                     : maxf(c->from_lids, (c->change_ms - dip + 20.0f) / SLOW_SHUT_MS + 1.0f));
    }
    return clampf(shut, 0.0f, 1.0f);
}

void chr_change_shape(const chr_t *c, float *grow_x, float *grow_y, float *lift)
{
    *grow_x = *grow_y = *lift = 0.0f;
    if (c->change_ms < 0.0f) return;
    const style_t *s = &styles[c->change_style];
    if (c->change_ms < s->dip_ms) {
        const float down = ramp(c->change_ms, 0.0f, s->dip_ms);
        *grow_x = mixf(c->from_x, s->dip_x, down);
        *grow_y = mixf(c->from_y, s->dip_y, down);
        *lift = mixf(c->from_lift, 0.0f, down);
        return;
    }
    const float since = c->change_ms - s->dip_ms, t = since * 0.001f;
    if (s->up_y == 0.0f) {
        const float down = 1.0f - ramp(since, 0.0f, s->settle_ms);
        *grow_x = s->dip_x * down;
        *grow_y = s->dip_y * down;
        return;
    }
    const float spring = expf(-s->decay * t) * cosf(2.0f * PI * s->hz * t);
    /* What is left of the spring at the end is too little to see and not worth a redraw. */
    if (expf(-s->decay * t) > 0.06f) {
        *grow_x = s->up_x * spring;
        *grow_y = s->up_y * spring;
    }
    if (since < s->hop_ms) *lift = s->hop * sinf(PI * since / s->hop_ms);
}

/* She goes on from the shape and the lids the last change left her with, if it has not settled yet. */
static void change_start(chr_t *c, uint8_t kind, uint8_t style)
{
    float grow_x, grow_y, lift;
    chr_change_shape(c, &grow_x, &grow_y, &lift);
    c->from_x = grow_x;
    c->from_y = grow_y;
    c->from_lift = lift;
    c->from_lids = chr_lids(c);
    c->change_kind = kind;
    c->change_style = style;
    c->change_ms = 0.0f;
    c->changed = false;
}

/* Two pictures of a clip of which one is a patch over the other, or both over a third: they differ
 * in a hand or an eye, and one simply follows the other. */
static bool alike(const chr_frame_t *a, const chr_frame_t *b)
{
    return a->under == b->sprite || b->under == a->sprite || (a->under != CHR_NO_SPRITE && a->under == b->under);
}

/* Whether the picture that shows only leads into a pose or out of it. */
static bool in_passing(const chr_t *c)
{
    return c->pose && c->clipped && (c->leaving || (!c->clip_at && leads_in(c, &c->clip)));
}

/* Steps through the pictures of the pose's clip. A clip she was popped into cuts from one picture
 * to the next, and so do two pictures that are alike in any clip and one that only leads into a pose
 * or out of it (she is down already); otherwise a soft clip or a beat changes each picture as she
 * entered it, with a dip that begins early enough for the picture to change on time. True when the
 * clip's way out has come to its end. A picture that only leads into a pose leads out again when the
 * one after it cannot be had, or does not come for too long. */
enum { PASSING_WAIT_MS = 1200 };
static bool clip_step(chr_t *c, float dt)
{
    chr_frame_t frame;
    if (!c->pose || !c->clipped || !chr_pack_frame(&c->pack, (unsigned)c->clip.first + c->clip_at, &frame)) return c->leaving;
    if (!frame.ms) {
        /* A picture that stays; on the way out there is nothing to wait for. */
        c->entered = true;
        return c->leaving;
    }
    const unsigned body = (unsigned)c->clip.enter + c->clip.loop, all = body + c->clip.leave;
    unsigned next = c->clip_at + 1U;
    if (!c->leaving && next >= body) next = c->clip.loop ? c->clip.enter : body - 1U;
    chr_frame_t after;
    const bool dips = c->clip.style != CHR_STYLE_POP && !in_passing(c) &&
                      !(chr_pack_frame(&c->pack, (unsigned)c->clip.first + next, &after) && alike(&frame, &after));
    const float lead = dips ? styles[c->clip.style].dip_ms : 0.0f;
    c->frame_ms += dt;
    /* The next picture is asked for while this one shows, and waited for when it is late. */
    const chr_shelf_state_t ready = next < all && next != c->clip_at ? frame_at_hand(c, &c->clip, next) : CHR_SHELF_READY;
    if (c->frame_ms < (float)frame.ms - lead) return false;
    if (!c->leaving && c->clip_at + 1U >= c->clip.enter) c->entered = true;     /* the last picture of the entry has had its time */
    if (c->leaving && next >= all) return true;
    if (next == c->clip_at) {
        c->frame_ms = 0.0f;
        return false;
    }
    if (ready == CHR_SHELF_COMING) {
        c->lag_max_ms = maxf(c->lag_max_ms, c->frame_ms - ((float)frame.ms - lead));
        if (c->leaving || !in_passing(c) || c->frame_ms < (float)PASSING_WAIT_MS) return false;
    }
    if (ready != CHR_SHELF_READY) {
        /* It cannot be had: she stays in this picture, and may go from here. */
        c->frame_ms = 0.0f;
        c->entered = true;
        if (!c->leaving && in_passing(c)) {
            c->leaving = true;
            c->shunned = c->pose;
        }
        return c->leaving;
    }
    if (dips) {
        c->frame_next = (uint16_t)next;
        change_start(c, CHANGE_FRAME, (uint8_t)c->clip.style);
    } else {
        c->clip_at = (uint16_t)next;
        c->frame_ms = 0.0f;
        pose_show(c);
    }
    return false;
}

/* The pose a flat figure takes for the gesture: one that the pack has (with no picture of a shrug she
 * tilts her head), once the gesture has lasted a moment. */
static uint8_t pose_wanted(const chr_t *c, float lasted)
{
    uint8_t pose = c->gesture;
    if (pose == CHR_GESTURE_SHRUG && chr_pack_slot(&c->pack, SL_POSE + CHR_GESTURE_SHRUG - 1U) == CHR_NO_SPRITE)
        pose = CHR_GESTURE_TILT;
    if (!(c->features & CHR_FLAT) || !pose || chr_pack_slot(&c->pack, SL_POSE + pose - 1U) == CHR_NO_SPRITE ||
        (c->gesture_ms < lasted && c->pose != pose) || pose == c->shunned) return 0;
    return pose;
}

/* Follows the wanted gesture, and for a flat figure chooses the pose picture, which then stays for
 * long enough to be seen. A pose with a clip
 * steps through its pictures; she does not leave it before its entry has played (a nod is nodded
 * to the end), and its way out plays before she does; wanted back by then, she turns round there.
 * While she is in the dip of a change nothing else begins; once the picture has changed and she is
 * settling, the next change may. She stays down while a picture shows that only leads into a pose
 * or out of it, so that the way from one pose to another is one dip. */
enum { BEAT_LEAST_MS = 250 };       /* the last picture of a dance is there to be seen */
enum { POSE_LEAST_MS = 2000 };      /* a pose picture stays for so long at least */
enum { THINK_LEAST_MS = 1000 };     /* ... the hand at her chin, until her answer is heard (then it comes down at once) */

/* In a talk she goes into one pose picture of her own a round at most (back to herself, of course); a hand offered to
 * a touch, or a reaction, is not the talk's. */
static bool one_a_round(const chr_t *c)
{
    return talk_phase(c) != CHR_PHASE_IDLE && c->round_poses && !c->cue && c->touch == CHR_TOUCH_NONE;
}
static void gesture_step(chr_t *c, float dt)
{
    const uint8_t wanted = gesture_wanted(c);
    if (wanted != c->gesture) {
        c->gesture = wanted;
        c->gesture_ms = 0.0f;
        c->shunned = 0;
    } else c->gesture_ms += dt;
    c->pose_ms += dt;
    if (c->change_ms >= 0.0f) {
        const style_t *s = &styles[c->change_style];
        if (!c->changed || !in_passing(c)) c->change_ms += dt;
        if (!c->changed && c->change_ms >= s->dip_ms) {
            if (c->change_kind == CHANGE_POSE) {
                /* Out of one pose and into the next that has come to be wanted meanwhile: she is down already. */
                uint8_t now = c->pose_next ? 0 : pose_wanted(c, 0.0f);
                if (now && one_a_round(c)) now = 0;
                pose_enter(c, now && pose_at_hand(c, now) == CHR_SHELF_READY ? now : c->pose_next);
            } else {
                c->leaving = c->leaving || c->change_kind == CHANGE_LEAVE;
                c->clip_at = c->frame_next;
                c->frame_ms = 0.0f;
                pose_show(c);
            }
            c->changed = true;
            if (in_passing(c)) c->change_ms = s->dip_ms;
        }
        if (c->change_ms >= s->dip_ms + s->settle_ms) c->change_ms = -1.0f;
        if (!c->changed) return;
    }
    uint8_t pose = pose_wanted(c, c->leaving ? 0.0f : 70.0f);
    if (pose && pose != c->pose && one_a_round(c)) pose = 0;
    if (pose != c->wait_for) {
        c->wait_for = pose;
        c->wait_ms = 0.0f;
    }
    /* Whether she leaves the pose is settled before its clip goes on, or a quick clip, always in
     * the dip of its next picture, would never let her go. */
    const bool answered = c->phase == CHR_PHASE_SPEAKING && c->answer_at >= 0.0;
    const float least = c->pose != CHR_GESTURE_THINK ? (float)POSE_LEAST_MS : answered ? 0.0f : (float)THINK_LEAST_MS;
    const bool early = c->pose && (c->pose_ms < least || !c->entered);
    if (!c->leaving && !early && pose != c->pose && c->clipped && (c->clip.style == CHR_STYLE_BEAT || c->clip.loop > 2U) &&
        c->frame_ms < (float)BEAT_LEAST_MS) {
        c->frame_ms += dt;
        return;
    }
    if (c->leaving || pose == c->pose || early) {
        const bool left = clip_step(c, dt);
        if (!c->leaving || !left) return;
    } else if (c->clipped && c->clip.leave) {
        const unsigned first = (unsigned)c->clip.enter + c->clip.loop;
        const chr_shelf_state_t ready = frame_at_hand(c, &c->clip, first);
        if (ready == CHR_SHELF_COMING) return;
        if (ready == CHR_SHELF_READY) {
            c->frame_next = (uint16_t)first;
            change_start(c, CHANGE_LEAVE, c->clip.style < CHR_STYLE_BEAT ? (uint8_t)c->clip.style : (uint8_t)CHR_STYLE_POP);
            return;
        }
    }
    /* She goes to a picture that is there. One that cannot be had is not gone to at all. */
    if (pose == c->shunned) pose = 0;
    if (pose) {
        const chr_shelf_state_t ready = pose_at_hand(c, pose);
        if (ready == CHR_SHELF_COMING) {
            c->wait_ms += dt;
            c->wait_max_ms = maxf(c->wait_max_ms, c->wait_ms);
            return;
        }
        if (ready == CHR_SHELF_FAILED) pose = 0;
        if (pose == c->pose && !c->leaving) return;
    }
    c->wait_ms = 0.0f;
    if (c->leaving && c->change_ms >= 0.0f) {
        /* She is down already: the next picture, and up from there. */
        c->change_kind = CHANGE_POSE;
        pose_enter(c, pose);
        return;
    }
    /* Into a gesture as its clip says, out of one as it was entered; a dance is popped into. */
    chr_clip_t next;
    uint8_t style = CHR_STYLE_POP;
    if (pose && chr_pack_clip(&c->pack, pose, &next)) style = (uint8_t)next.style;
    else if (!pose && c->clipped) style = (uint8_t)c->clip.style;
    if (style >= CHR_STYLE_BEAT) style = CHR_STYLE_POP;
    c->pose_next = pose;
    change_start(c, CHANGE_POSE, style);
}

/* ------------------------------------------------------------------- voice */

/* Amplitudes of 32768, and milliseconds. What is below this, or below this part of the loudest of the
 * last seconds, is silence; the loudest is forgotten slowly and never thought of as less than that. */
#define VOICE_GATE 250.0f
#define VOICE_PART 0.08f
#define VOICE_PEAK_MIN 2000.0f
#define VOICE_PEAK_MS 4000.0f
#define SHUT_AFTER_MS 90.0f        /* silence before the mouth shuts */
#define STEP_MS 60.0f              /* how long a step of the mouth stays at least */
#define SENTENCE_GAP_MS 350.0f     /* what is said after such a pause begins a sentence ... */
#define SENTENCE_EVERY_MS 2500.0f  /* ... if the last one did not just begin */
#define ANSWER_GAP_MS 300.0f       /* a sound after such a pause can begin an answer or a sentence */
#define SENTENCE_WAIT_MS 400.0f    /* how long after its time a sentence waits for its sound, or is late */
#define AHEAD_SEEN_MS 70.0f        /* what is about to be heard is known this far ahead, more or less */
#define UNHEARD_MS 1000.0          /* an answer whose first sentence came this long ago and is not heard has begun */
#define GUESS_SLACK_MS 2000.0      /* the first sound this soon after where an answer was taken to begin is its own */
#define ONSET_WAIT_MS 500.0        /* a sentence of no time that no sound after a pause began is heard this late */
#define USER_VOICE 400.0f          /* the user's voice: the microphone's amplitude after echo cancelling ... */
#define USER_VOICE_MS 50.0f        /* ... for this long */
#define SENTENCE_BROW_MS 320.0f
/* When a talk is over for its page: still for so long she has said what she had to say; she waits this
 * long at least (the page and the device tell it apart by a moment), and at most. */
#define PART_AFTER_MS 500.0f
#define PART_LEAST_MS 150.0f
#define PART_MOST_MS 20000.0f
#define SPOKEN_MS 500.0f           /* heard so lately, she has spoken */

/* ------------------------------------------------------------------ target */

enum { R_OUT = 0, R_BODY, R_HEAD, R_EAR_L, R_EAR_R, R_TAIL, R_ARM_L, R_ARM_R, R_HAND,
       R_WOKEN /* the touch that woke her: it does nothing else */ };

/* ----------------------------------------------------------------- feeling */

/* What lasts longer than a moment. Two slow variables: how awake she is (it runs down while nobody
 * talks to her, touches her or wakes the screen, and comes back when somebody does) and how close she feels (strokes,
 * talks and a hand held raise it, being poked silly lowers it, and left alone it settles in the
 * middle). And the mood the conversation gave her: it is kept until two seconds after she has
 * finished speaking, or the talk has ended, and then gives way to her own mood of the moment, which
 * is a drowsy one when she is tired; a low mood gives way in any case, because she does not sulk for
 * long.
 *
 * A mood changes behind a blink (at once when she is back on the screen). While a talk runs it changes between two sentences, when the
 * speaker has been still for a moment (or when none comes for too long), and not again right away.
 * When a talk ends its page forgets the mood at once; she does not. After it she is as she was given
 * to be before it, without making a show of it. */
enum { MOOD_AFTER_TALK_MS = 2000, MOOD_LOW_MS = 20000, LONELY_MS = 600000, LONELY_AGAIN_MS = 180000, WAVES_MAX = 3,
       DOZE_MS = 25000, MOOD_GAP_MS = 120, MOOD_WAIT_MS = 1200, MOOD_STAYS_MS = 1500, MOOD_BACK_MS = 600 };
/* A flat figure shows a mood lightly as it comes and fully once it has stayed; on its way out she shows it lightly for
 * a moment first. A change reaches her mouth this much after her eyes. */
enum { MOOD_LIGHT = 1, MOOD_FULL = 2, MOOD_FULL_MS = 1500, MOOD_EASE_MS = 300, MOUTH_LAG_MS = 120 };

static bool low_mood(const mood_t *m) { return m->mc < 0.0f && (m->slant != 0.0f || (m->flags & M_TEARS)); }

static size_t own_mood(const chr_t *c)
{
    return c->energy < 0.3f ? chr_mood_find("sleepy") : CHR_MOOD_NEUTRAL;
}

/* She was talked to or touched, or somebody woke the screen. */
static void company(chr_t *c, float energy, float closeness)
{
    c->energy = clampf(c->energy + energy, 0.0f, 1.0f);
    c->closeness = clampf(c->closeness + closeness, 0.0f, 1.0f);
    c->alone_ms = 0.0f;
    c->waves = 0;
    c->lonely_in = (float)LONELY_MS;
    if (c->dozing) {
        c->dozing = false;
        chr_cue(c, CHR_CUE_STRETCH, 0);
    }
}

/* Quietly: in her face, without the marks and the gesture that go with a mood that has just come. */
static void show_mood(chr_t *c, size_t mood, bool quietly)
{
    if (c->mood == mood) return;
    CHR_MARK("FACE %s %u", moods[mood].name, (unsigned)talk_phase(c));
    c->mood = (uint8_t)mood;
    c->mood_age = 0.0f;
    memset(c->emit_in, 0, sizeof(c->emit_in));
    const mood_t *m = &moods[mood];
    c->crying = (m->flags & M_TEARS) != 0;
    c->eyes_look = m->face.eyes;
    c->eyes_full = m->face.full;
    c->brows_look = m->face.brows;
    c->mouth_look = m->face.mouth;
    c->smile_in = chr_uniform(c, 1500, 3500);
    c->snap = true;
    /* It comes lightly, and reaches her mouth a moment after her eyes. Surprise is one slow blink. */
    c->strength = quietly ? (uint8_t)MOOD_FULL : (uint8_t)MOOD_LIGHT;
    c->strength_ms = 0.0f;
    c->mouth_lag = (float)MOUTH_LAG_MS;
    if ((c->features & CHR_FLAT) && (m->flags & M_EXCLAIM) && !quietly && c->blink_age < 0.0f) {
        c->blink_in = 150.0f;
        c->slow_blink = true;
    }
    /* What a conversation brings shows in her face; marks in the air are for herself and for touch. */
    c->mood_talk = talk_phase(c) != CHR_PHASE_IDLE;
    if (c->mood_talk) return;
    if (quietly) {
        c->mood_age = (float)MOOD_BODY_MS;
        return;
    }
    if (c->conditions & (CHR_OFFLINE | CHR_CONNECTING)) return;
    if (m->flags & M_QUESTION) chr_emit(c, FX_QUESTION, chr_side_x(c) + CHR_SIDE * 8, c->fig.hair_top + 80, CHR_SIDE * 14, -46, 38, 1500);
    if ((m->flags & M_EXCLAIM) && fancy(c)) fx_surprise(c);
    else if (m->flags & M_EXCLAIM) chr_emit(c, FX_EXCLAIM, chr_side_x(c) + CHR_SIDE * 8, c->fig.hair_top + 80, CHR_SIDE * 14, -60, 36, 1000);
    if ((m->flags & M_HEARTS) && fancy(c)) fx_praise(c);
    if (m->flags & M_SPARKS) burst(c, FX_SPARK, 3, chr_side_x(c) - CHR_SIDE * 34, c->fig.brow_y - 20, 70, 13);
}

/* The eyes of the mood, as strongly as it shows. A flat figure's eyes fade from one look to the next (chr_draw.c);
 * those of another figure come and go while the lids are shut: in the blink that covers a change of mood, or in one
 * of their own. */
static uint8_t look_wanted(const chr_t *c)
{
    const uint8_t look = (c->features & CHR_FLAT) && c->strength == MOOD_FULL ? c->eyes_full : c->eyes_look;
    if (!(c->features & CHR_FLAT) || talk_phase(c) == CHR_PHASE_IDLE) return look;
    /* In a talk her eyes are open ones: glad eyes (half open in a smile) and lowered ones are soft, serious ones her own;
     * her brows and mouth keep the mood. */
    switch (look) {
    case LOOK_GLAD:
    case LOOK_DOWN:
    case LOOK_COMFORT: return LOOK_SOFT;
    case LOOK_SERIOUS: return LOOK_PLAIN;
    default: return look;
    }
}

static void look_step(chr_t *c)
{
    const uint8_t look = look_wanted(c);
    if (look == c->eyes_shown) return;
    if ((c->features & CHR_FLAT) || chr_lids(c) > 0.7f || c->blink_turned) c->eyes_shown = look;
    else if (c->blink_age < 0.0f && c->swap_ms < 0.0f) c->blink_age = 0.0f;
}

/* Towards the wanted mood: see above. */
static void mood_step(chr_t *c, float dt, size_t wanted)
{
    c->strength_ms = minf(c->strength_ms + dt, 1000000.0f);
    if (c->swap_ms >= 0.0f) {
        const bool open = c->swap_ms < LID_SHUT_MS;
        c->swap_ms += dt;
        if (open && c->swap_ms >= LID_SHUT_MS) show_mood(c, wanted, c->after_ms < (float)MOOD_BACK_MS || (c->conditions & CHR_STARTING));
        if (c->swap_ms >= LID_SHUT_MS + LID_HOLD_MS + LID_OPEN_MS) c->swap_ms = -1.0f;
        else return;
    }
    if (wanted == c->mood) {
        c->mood_wait = 0.0f;
        c->easing = false;
        if (c->strength == MOOD_LIGHT && c->strength_ms >= (float)MOOD_FULL_MS) {
            c->strength = MOOD_FULL;
            c->strength_ms = 0.0f;
        }
        return;
    }
    c->mood_wait += dt;
    /* (Once she has begun to let a full mood go, the next one comes when that is done, and in a pause.) */
    if (c->easing && c->strength_ms < (float)MOOD_EASE_MS) return;
    if (talk_phase(c) != CHR_PHASE_IDLE && !c->held &&
        ((c->mood_age < (float)MOOD_STAYS_MS && !c->easing) || (c->silent_ms < (float)MOOD_GAP_MS && c->mood_wait < (float)MOOD_WAIT_MS))) return;
    if (c->features & CHR_FLAT) {
        /* Without a blink: a mood that shows fully, and fully is more than lightly, goes back to showing lightly, and a
         * moment later the next comes. */
        if (c->strength == MOOD_FULL && !c->easing && (c->eyes_full != c->eyes_look || c->target[P_BLUSH] > 0.0f)) {
            c->strength = MOOD_LIGHT;
            c->strength_ms = 0.0f;
            c->easing = true;
            return;
        }
        c->easing = false;
        c->mood_wait = 0.0f;
        show_mood(c, wanted, c->after_ms < (float)MOOD_BACK_MS || (c->conditions & CHR_STARTING));
        return;
    }
    c->mood_wait = 0.0f;
    c->swap_ms = 0.0f;
}

/* The mood that is hers: the one she was given until its time is over, then her own. */
static uint8_t mood_now(const chr_t *c)
{
    /* In a talk: over two seconds after the last of her answer was heard (whatever the service says of its end; not
     * while more of it is told and not yet heard; nor sooner after her last sentence was shown, where nothing of her
     * is heard), and as soon as the user begins to speak. */
    double heard = c->voice_at > c->answer_at ? c->voice_at : c->answer_at;
    if (c->told_at > heard) heard = c->told_at;
    /* Nothing of the answer is heard (the speaker is off): it lasts for as long as she speaks. */
    if (c->answer_guessed && c->phase == CHR_PHASE_SPEAKING) heard = c->now;
    else if (c->answer_guessed && c->now - (double)c->phase_age > heard) heard = c->now - (double)c->phase_age;
    const bool said = c->answer_at >= 0.0 && !c->sentence_count && c->now - heard > (double)MOOD_AFTER_TALK_MS &&
                      (!c->ahead || c->ahead_silent_ms > (float)MOOD_AFTER_TALK_MS);
    const bool over = !c->held &&
                      (c->phase != CHR_PHASE_IDLE
                           ? c->user_turn || (c->answer_at >= 0.0 ? said
                                                                  : c->spoken && c->phase != CHR_PHASE_SPEAKING &&
                                                                        c->silent_ms > (float)MOOD_AFTER_TALK_MS)
                           : (c->talked && c->silent_ms > (float)MOOD_AFTER_TALK_MS) ||
                                 (low_mood(&moods[c->given]) && c->given_ms > (float)MOOD_LOW_MS));
    /* Her own mood is for when there is no talk. */
    const size_t own = talk_phase(c) == CHR_PHASE_IDLE ? own_mood(c) : CHR_MOOD_NEUTRAL;
    /* What her sentences brought stands in a talk instead of what the page gives, which comes as they are sent: until
     * the first of them is heard she stays as she is. */
    const size_t given = c->phase == CHR_PHASE_IDLE || !c->telling ? c->given : c->told < CHR_MOOD_COUNT ? c->told : c->mood;
    return (uint8_t)(over || (!c->held && given == CHR_MOOD_NEUTRAL) ? own : given);
}

static void feeling_step(chr_t *c, float dt)
{
    const bool calm = talk_phase(c) == CHR_PHASE_IDLE && c->touch == CHR_TOUCH_NONE &&
                      !(c->conditions & (CHR_ALARM | CHR_RINGING | CHR_IN_CALL | CHR_STARTING));
    c->alone_ms += dt;
    /* An hour alone takes most of her energy: wide awake for the first twenty minutes, drowsy after
     * three quarters of an hour. Closeness drifts to the middle over some ten minutes. */
    c->energy = maxf(c->energy - dt * (0.012f / 60000.0f), 0.1f);
    c->closeness += (0.5f - c->closeness) * clampf(dt / 600000.0f, 0, 1);
    if (c->touch == CHR_TOUCH_PET || c->touch == CHR_TOUCH_HOLD) company(c, dt * 0.0001f, dt * 0.0001f);
    if (c->conditions & (CHR_ALARM | CHR_RINGING | CHR_IN_CALL)) company(c, 0.0f, 0.0f);

    /* The mood she shows. */
    c->given_ms += dt;
    /* (A word that comes with the end of her turn is spoken too: she is still heard.) */
    if (c->phase == CHR_PHASE_SPEAKING || (c->phase != CHR_PHASE_IDLE && c->silent_ms < SPOKEN_MS)) c->spoken = true;
    /* The mood of a talk that has ended goes on until its last words are two seconds old (whether she
     * has come to show it yet or not). */
    const bool lingers = !c->held && c->phase == CHR_PHASE_IDLE && c->parted &&
                         (c->parting || c->silent_ms <= (float)MOOD_AFTER_TALK_MS);
    if (!lingers) {
        c->wanted = mood_now(c);
        if (c->phase == CHR_PHASE_IDLE) c->home = c->given;
    }
    c->after_ms = lingers ? 0.0f : minf(c->after_ms + dt, 1000000.0f);
    mood_step(c, dt, c->wanted);

    /* Tired and left alone, she dozes off; anyone coming wakes her (company()). */
    if (!c->dozing && calm && !c->cue && c->energy < 0.3f && c->mood_age > (float)DOZE_MS && c->alone_ms > (float)DOZE_MS &&
        !(c->conditions & (CHR_OFFLINE | CHR_CONNECTING))) c->dozing = true;
    if (c->dozing && (!calm || c->energy >= 0.3f)) company(c, 0.0f, 0.0f);

    /* Left alone while still wide awake, she looks for you and waves: a few times, then lets it be. */
    if (calm && !c->cue && !c->dozing && c->energy >= 0.6f && c->waves < WAVES_MAX && !(moods[c->mood].flags & M_ZED) &&
        !(c->conditions & (CHR_OFFLINE | CHR_CONNECTING)) && (c->lonely_in -= dt) <= 0.0f) {
        ++c->waves;
        c->lonely_in = (float)LONELY_AGAIN_MS;
        chr_cue(c, CHR_CUE_WAVE, 0);
    }
}

#define SMILE_MS 750.0f
#define BROWS_UP 5.2f         /* past where a flat figure shows its raised brows */

static void compose(chr_t *c, float *T)
{
    const mood_t *m = &moods[c->mood];
    const float now = (float)fmod(c->now, 3600000.0);
    const bool flat = (c->features & CHR_FLAT) != 0;
    memset(T, 0, sizeof(float) * P_COUNT);
    T[P_EOL] = m->eol; T[P_EOR] = m->eor; T[P_ARC] = m->arc; T[P_SLANT] = m->slant; T[P_BROW] = m->brow;
    T[P_MC] = m->mc; T[P_MO] = m->mo; T[P_BLUSH] = m->blush; T[P_HY] = m->hy; T[P_LEAN] = m->lean;
    T[P_GX] = m->gx; T[P_GY] = m->gy; T[P_EAR_L] = m->ear_l; T[P_EAR_R] = m->ear_r; T[P_WAG] = m->wag;
    /* A figure in one piece has its moods in her face; her body keeps still, and her mouth rests shut but for an open
     * smile (which is a laugh). Lightly, her cheeks colour less. */
    if (flat) {
        T[P_HY] = T[P_LEAN] = 0.0f;
        T[P_MO] = m->flags & M_LAUGH ? 0.8f : 0.0f;
        const face_t *face = &m->face;
        T[P_BLUSH] = face->eyes | face->full | face->brows | face->mouth ? face->blush : m->blush;
        if (c->strength == MOOD_LIGHT) T[P_BLUSH] *= 0.7f;
        /* Eyes of a look of their own (cast down, with tears, glad) bring their lids with them. */
        if (m->face.eyes != LOOK_PLAIN || m->face.full != LOOK_PLAIN) { T[P_EOL] = T[P_EOR] = 1.0f; T[P_ARC] = 0.0f; }
    }
    if (m->flags & M_SWAY) T[P_LEAN] += 8.0f * sinf(now * 0.0016f);
    if ((m->flags & M_SMILES) && c->smile_in < 0.0f && !flat) T[P_ARC] = mixf(T[P_ARC], 1.0f, window(-c->smile_in / SMILE_MS, 0.15f, 0.25f));
    /* Surprised: a flat figure's mouth is a little round one for a moment, then shut (her brows stay up). Surprised in a
     * talk: eyes wide and a round mouth, which soon ease to raised brows. */
    if (flat && (m->flags & M_EXCLAIM)) {
        const bool round = c->mood_age < 600.0f;
        T[P_MO] = round ? 1.0f : 0.0f; T[P_MC] = round ? -1.0f : 0.5f;
    } else if (c->mood_talk && (m->flags & M_EXCLAIM)) {
        const float eased = ramp(c->mood_age, 800.0f, 1000.0f);
        both_eyes(T, 1.0f, eased); T[P_MC] = mixf(T[P_MC], 0.5f, eased); T[P_MO] = eased < 0.5f ? 1.0f : 0.0f;
    }
    /* Feeling close shows as a readier smile, feeling less so as a plainer face. */
    if (!low_mood(m)) T[P_MC] = clampf(T[P_MC] + (c->closeness - 0.5f) * 0.5f, -1.0f, 1.0f);
    if ((m->flags & M_BOUNCE) && !flat) T[P_HY] += 5.0f * sinf(now * 0.021f);

    /* In a talk the mouth is shut when she does not speak, but for the open smile of someone excited. */
    const float rest = c->mood_talk && (m->flags & (flat ? M_LAUGH : M_OPEN)) ? (flat ? 0.8f : m->mo) : 0.0f;
    const bool astonished = c->mood_talk && (m->flags & M_EXCLAIM) && (!flat || c->mood_age < 600.0f);
    if (c->mood_talk && !astonished) T[P_MO] = rest;

    /* A glance somewhere, when the mood does not hold the eyes. */
    if (m->gx == 0.0f && m->gy == 0.0f) { T[P_GX] = c->look_x; T[P_GY] = c->look_y; }
    if ((c->conditions & CHR_STARTING) && talk_phase(c) == CHR_PHASE_IDLE) T[P_GX] = T[P_GY] = 0.0f;     /* called: she looks at you */

    switch (talk_phase(c)) {
    case CHR_PHASE_LISTENING: {
        const float w = ramp(c->phase_age, 0, 260);
        T[P_EOL] = maxf(T[P_EOL], mixf(T[P_EOL], 1.12f, w)); T[P_EOR] = maxf(T[P_EOR], mixf(T[P_EOR], 1.12f, w));
        T[P_ARC] = mixf(T[P_ARC], 0, w); T[P_MO] = mixf(T[P_MO], rest, w);
        T[P_GX] = mixf(T[P_GX], 0, w); T[P_GY] = mixf(T[P_GY], 0, w);
        /* All ears: the brows go up, and whoever has a head of her own to lean leans it. */
        if (!flat) T[P_LEAN] = mixf(T[P_LEAN], 6.0f, w);
        T[P_BROW] = mixf(T[P_BROW], flat ? BROWS_UP : 3.0f, w);
        both_ears(T, -0.25f, w); T[P_WAG] = mixf(T[P_WAG], 0.3f, w);
        break;
    }
    case CHR_PHASE_THINKING: {
        /* Eyes lowered to one side, always the same one: the one her picture of it looks to. */
        const float w = ramp(c->phase_age, 0, 320), drift = sinf(now * 0.0011f), side = -1.0f;
        T[P_GX] = mixf(T[P_GX], 0.6f * side, w); T[P_GY] = mixf(T[P_GY], 0.15f, w);
        T[P_MC] = mixf(T[P_MC], 0.0f, w); T[P_MO] = mixf(T[P_MO], 0, w); T[P_ARC] = mixf(T[P_ARC], 0, w);
        T[P_SLANT] = mixf(T[P_SLANT], 0.15f, w);
        if (!flat) T[P_LEAN] = mixf(T[P_LEAN], 5.0f + 3.0f * drift, w);
        T[P_EAR_L] = mixf(T[P_EAR_L], side > 0 ? 0.45f : -0.2f, w); T[P_EAR_R] = mixf(T[P_EAR_R], side > 0 ? -0.2f : 0.45f, w);
        T[P_WAG] = mixf(T[P_WAG], 0.15f, w);
        break;
    }
    case CHR_PHASE_SPEAKING: {
        const float w = ramp(c->phase_age, 0, 200);
        /* She looks at you, unless her mood looks elsewhere. Her voice alone opens the mouth (finish()),
         * and the rest of her stays where it is: a syllable redraws the face and not the whole figure. */
        if (m->gx == 0.0f && m->gy == 0.0f) { T[P_GX] = mixf(T[P_GX], 0.15f * sinf(now * 0.0009f), w); T[P_GY] = mixf(T[P_GY], 0, w); }
        if (!astonished) T[P_MO] = mixf(T[P_MO], rest, w);
        T[P_WAG] = mixf(T[P_WAG], 0.45f, w); both_ears(T, -0.2f, w);
        break;
    }
    default: break;
    }

    /* Device conditions outrank the conversation. */
    if (c->conditions & CHR_ALARM) {
        const float beat = sinf(now * 0.03f);
        both_eyes(T, 1.28f, 1); T[P_ARC] = 0; T[P_SLANT] = 0; T[P_BROW] = 8; T[P_MC] = -1; T[P_MO] = 0.7f;
        T[P_EAR_L] = 0.6f + 0.4f * beat; T[P_EAR_R] = 0.6f - 0.4f * beat; T[P_WAG] = 1.5f;
        T[P_GX] = T[P_GY] = 0; T[P_HY] = -4;
    } else if (c->conditions & CHR_RINGING) {
        const float beat = sinf(now * 0.02f);
        both_eyes(T, 1.18f, 1); T[P_ARC] = 0; T[P_BROW] = 6; T[P_MC] = -1; T[P_MO] = 0.35f;
        T[P_EAR_L] = -0.2f + 0.45f * maxf(beat, 0); T[P_EAR_R] = -0.2f + 0.45f * maxf(-beat, 0);
        T[P_BY] -= (flat ? 3.0f : 9.0f) * fabsf(sinf(now * 0.0063f)); T[P_GX] = 0.6f; T[P_GY] = -0.1f; T[P_WAG] = 1.0f;
    } else if (c->conditions & CHR_IN_CALL) {
        T[P_LEAN] = flat ? 0.0f : 6.0f; T[P_MC] = 0.7f; T[P_GX] = 0.25f; T[P_GY] = 0; both_ears(T, -0.25f, 1); T[P_WAG] = 0.4f;
    } else if (talk_phase(c) == CHR_PHASE_IDLE && (c->conditions & CHR_OFFLINE)) {
        T[P_GREY] = 0.85f; both_ears(T, 1.0f, 1); T[P_EOL] = minf(T[P_EOL], 0.7f); T[P_EOR] = minf(T[P_EOR], 0.7f);
        T[P_SLANT] = -0.7f; T[P_ARC] = 0; T[P_BROW] = -2; T[P_MC] = -1; T[P_MO] = 0; T[P_HY] = 8;
        T[P_GY] = 0.3f; T[P_BLUSH] = 0; T[P_WAG] = 0;
    } else if (talk_phase(c) == CHR_PHASE_IDLE && (c->conditions & CHR_CONNECTING)) {
        const float beat = sinf(now * 0.006f);
        T[P_GREY] = 0.35f; T[P_GX] = -0.6f; T[P_GY] = 0.15f; T[P_MC] = 0; T[P_MO] = 0; T[P_ARC] = 0; T[P_SLANT] = 0;
        both_eyes(T, 1.05f, 1); T[P_BROW] = 3; T[P_HY] = -2;
        T[P_EAR_L] = -0.2f + 0.4f * maxf(beat, 0); T[P_EAR_R] = -0.2f + 0.4f * maxf(-beat, 0); T[P_WAG] = 0.2f;
    } else if (talk_phase(c) == CHR_PHASE_IDLE && (c->conditions & CHR_MUTED)) {
        both_ears(T, 0.35f, 1);
    }

    /* Eyes lowered to one side: thinking, the mood of it, or waiting for the service; not while something else
     * has her attention. A flat figure has a picture of them (chr_draw.c). */
    const uint8_t talking = talk_phase(c);
    const bool pondering = !strcmp(m->name, "thinking") || (flat && !strcmp(m->name, "confused"));
    c->ponder = !c->cue && c->touch == CHR_TOUCH_NONE && c->release_ms >= 500.0f && !c->dozing &&
                !(c->conditions & (CHR_ALARM | CHR_RINGING | CHR_IN_CALL | CHR_STARTING)) &&
                (talking == CHR_PHASE_THINKING || (pondering && talking != CHR_PHASE_LISTENING) ||
                 (talking == CHR_PHASE_IDLE && (c->conditions & (CHR_OFFLINE | CHR_CONNECTING)) == CHR_CONNECTING));
    if (c->cue) react(c, T);
    if (c->gesture) gesture_pose(c, T);

    /* Touch last: the hand on her is what she attends to. */
    if (c->touch != CHR_TOUCH_NONE || c->release_ms < 500.0f) {
        const float w = c->touch != CHR_TOUCH_NONE ? 1.0f : 1.0f - c->release_ms / 500.0f;
        T[P_GX] = mixf(T[P_GX], c->watch_x, w); T[P_GY] = mixf(T[P_GY], c->watch_y, w);
        T[P_LEAN] += 7.0f * c->watch_x * w;
    }
    if (c->pet > 0.01f) {
        const float w = clampf(c->pet, 0, 1);
        T[P_ARC] = mixf(T[P_ARC], 1.0f, w); T[P_SLANT] = mixf(T[P_SLANT], 0, w);
        T[P_BLUSH] = maxf(T[P_BLUSH], (flat ? 0.5f : 0.9f) * w); T[P_MC] = mixf(T[P_MC], 1.0f, w); T[P_MO] = mixf(T[P_MO], 0.25f, w);
        T[P_HY] += 5.0f * w; T[P_GREY] *= 1.0f - 0.5f * w; T[P_LEAN] += 9.0f * c->watch_x * w;
        both_ears(T, 0.75f, w); T[P_WAG] = mixf(T[P_WAG], 1.5f, w);
    }
    if (c->squish > 0.01f) {
        const float w = clampf(c->squish, 0, 1);
        T[P_HY] += 13.0f * w; both_eyes(T, 0.3f, w); T[P_SLANT] = mixf(T[P_SLANT], 0.6f, w);
        T[P_ARC] = mixf(T[P_ARC], 0, w); T[P_MC] = mixf(T[P_MC], -1.0f, w); T[P_MO] = mixf(T[P_MO], 0.1f, w);
        T[P_BLUSH] = maxf(T[P_BLUSH], 0.6f * w); both_ears(T, 0.9f, w); T[P_WAG] = mixf(T[P_WAG], 0, w);
    }
    if (c->touch == CHR_TOUCH_HOLD) {
        /* Her hand is held: she looks at it, then away, shy and pleased. */
        const float w = ramp(c->held_ms, 500.0f, 900.0f), away = ramp(c->held_ms, 1400.0f, 1900.0f);
        T[P_BLUSH] = maxf(T[P_BLUSH], 0.9f * w); T[P_MC] = mixf(T[P_MC], 1.0f, w); T[P_MO] = mixf(T[P_MO], 0.0f, w);
        T[P_GX] = mixf(T[P_GX], mixf(c->watch_x, -c->watch_x * 0.6f, away), w);
        T[P_GY] = mixf(T[P_GY], mixf(c->watch_y, -0.2f, away), w);
        both_eyes(T, 0.8f, w * away); T[P_HY] += 3.0f * w; both_ears(T, 0.5f, w); T[P_WAG] = mixf(T[P_WAG], 1.2f, w);
    }
    if (c->dozing) {
        /* Asleep where she stands: eyes shut, head low, breathing slowly. */
        T[P_ARC] = -1.0f; T[P_HY] += 9.0f; T[P_LEAN] += 4.0f; T[P_MC] = 0.2f; T[P_MO] = 0.0f; T[P_GX] = T[P_GY] = 0.0f;
        both_ears(T, 0.8f, 1); T[P_WAG] = 0.0f;
    }
    if (c->touch == CHR_TOUCH_PULL) {
        /* The held ear follows the finger; she squints on that side. */
        const bool left = c->region == R_EAR_L;
        const float middle = 0.5f * (float)(left ? c->fig.ear_lx0 + c->fig.ear_lx1 : c->fig.ear_rx0 + c->fig.ear_rx1);
        const float pull = clampf((c->touch_x - middle) / 45.0f, -1, 1) * (left ? 1.0f : -1.0f);
        T[left ? P_EAR_L : P_EAR_R] = pull;
        T[left ? P_EOL : P_EOR] = 0.0f; T[P_ARC] = 0; T[P_MC] = -1; T[P_MO] = 0.25f;
        T[P_BLUSH] = maxf(T[P_BLUSH], 0.5f); T[P_LEAN] += (left ? 1.0f : -1.0f) * 7.0f; T[P_SLANT] = -0.3f;
    }
    if (flat && talk_phase(c) != CHR_PHASE_IDLE) {
        /* In a talk (listening, thinking, speaking) a flat figure's eyes are open, whatever their look, and shut only
         * to blink: no smile closes them, no mood or touch narrows them, but sleepiness she was given (by the captions
         * or the AI's own choice of face). A wink is a blink of one eye: there for a moment as it comes. */
        const bool sleepy = !strcmp(m->name, "sleepy") && c->given == c->mood;
        const bool wink = (!strcmp(m->name, "winking") || !strcmp(m->name, "funny")) && c->mood_age < 600.0f;
        T[P_ARC] = 0.0f;
        if (!sleepy && !wink) T[P_EOL] = T[P_EOR] = maxf(maxf(T[P_EOL], T[P_EOR]), 1.0f);
    }
}

/* --------------------------------------------------------------- lifecycle */

size_t chr_size(void) { return sizeof(chr_t); }
size_t chr_scratch_entries(uint16_t stage_width) { return chr_target_scratch(stage_width, stage_width); }

void chr_place(chr_t *c, bool raised)
{
    if (!c) return;
    c->raised = raised;
    c->anchor_x = floorf(c->width * 0.5f);
    c->anchor_y = floorf(c->height * (raised ? 0.90f : 1.0f));
}

/* The slots from `first` on, `count` of them, are all filled. */
static bool filled(const chr_pack_t *pack, unsigned first, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (chr_pack_slot(pack, first + i) == CHR_NO_SPRITE) return false;
    return true;
}

bool chr_init(chr_t *c, uint16_t stage_width, uint16_t stage_height, uint32_t seed, const void *pack, size_t pack_size)
{
    chr_pack_t opened;
    if (!c) return false;
    if (chr_pack_open(&opened, pack, pack_size)) return chr_init_from(c, stage_width, stage_height, seed, &opened);
    memset(c, 0, sizeof(*c));
    return false;
}

bool chr_init_from(chr_t *c, uint16_t stage_width, uint16_t stage_height, uint32_t seed, const chr_pack_t *pack)
{
    if (!c) return false;
    memset(c, 0, sizeof(*c));
    if (!pack || !pack->data) return false;
    c->pack = *pack;
    /* Every figure has a body. A layered one has a whole head that faces the viewer; without a face
     * of its own the body is the whole figure, and what the head block holds are patches over it. */
    bool figure = filled(&c->pack, SL_BODY, 1);
    if (filled(&c->pack, SL_HEAD + H_FACE, 1)) figure = figure && filled(&c->pack, SL_HEAD, H_SLOTS);
    else c->features |= CHR_FLAT;
    if (!figure) { c->pack.data = NULL; c->features = 0; return false; }
    for (unsigned pose = 0; pose < CHR_POSES; ++pose)
        if (filled(&c->pack, SL_POSE + pose, 1)) c->features |= CHR_HAS_POSES;
    if (filled(&c->pack, SL_EAR_L, 3) && filled(&c->pack, SL_EAR_R, 3)) c->features |= CHR_HAS_EARS;
    if (filled(&c->pack, SL_TAIL, 1)) c->features |= CHR_HAS_TAIL;
    if (filled(&c->pack, SL_ARM_L, CHR_ARM_STATES) && filled(&c->pack, SL_ARM_R, CHR_ARM_STATES)) c->features |= CHR_HAS_ARMS;
    if (filled(&c->pack, SL_HEAD + H_SLOTS, 2 * H_SLOTS)) c->features |= CHR_HAS_YAW;
    int16_t *values = (int16_t *)&c->fig;
    for (unsigned i = 0; i < CHR_FIGURE_VALUES; ++i) values[i] = (int16_t)chr_pack_layout(&c->pack, i);
    c->width = (float)stage_width;
    c->height = (float)stage_height;
    chr_place(c, false);
    c->random = seed | 1U;
    c->blink_in = 1800.0f;
    c->blink_age = -1.0f;
    c->look_in = 2500.0f;
    c->idle_in = 9000.0f;
    c->release_ms = 10000.0f;
    c->silent_ms = c->sentence_ms = c->unheard_ms = c->hearing_ms = c->voice_step_ms = c->after_ms = 10000.0f;
    c->ahead_silent_ms = 10000.0f;
    c->answer_at = c->voice_at = c->told_at = c->back_at = c->away_at = -1.0;
    c->told = c->sentence_mood = c->asked = CHR_MOOD_COUNT;
    c->voice_peak = VOICE_PEAK_MIN;
    c->cut_ms = c->swap_ms = -1.0f;
    c->gaze = 1;
    c->ear[0] = c->ear[1] = 1;
    c->turn = 1;
    c->mouth_form = 1;
    c->energy = 0.85f;
    c->closeness = 0.5f;
    c->lonely_in = (float)LONELY_MS;
    c->change_ms = -1.0f;
    c->picture = c->picture_under = CHR_NO_SPRITE;
    for (unsigned part = 0; part < PART_COUNT; ++part) c->part_now[part] = c->part_was[part] = CHR_NO_SPRITE;
    c->rung[0] = c->rung[1] = c->aim[0] = c->aim[1] = RUNG_OPEN;
    c->strength = MOOD_FULL;
    c->face_cut = true;
    for (unsigned kind = 0; kind < FX_KINDS; ++kind)
        while (c->fx_frames[kind] < CHR_FX_FRAMES && chr_pack_slot(&c->pack, SL_FX + kind * CHR_FX_FRAMES + c->fx_frames[kind]) != CHR_NO_SPRITE)
            ++c->fx_frames[kind];
    compose(c, c->target);
    memcpy(c->p, c->target, sizeof(c->p));
    memcpy(c->out, c->p, sizeof(c->out));
    return true;
}

void chr_set_mood(chr_t *c, size_t mood)
{
    if (!c || mood >= CHR_MOOD_COUNT || c->given == mood) return;
    CHR_MARK("GIVEN %s", moods[mood].name);
    /* A new mood starts its own time; when she shows it and whether it stays is feeling_step's. */
    c->given = (uint8_t)mood;
    c->given_ms = 0.0f;
    c->talked = c->spoken = false;
}

void chr_hold(chr_t *c, bool held)
{
    if (c) c->held = held;
}

void chr_set_phase(chr_t *c, chr_phase_t phase)
{
    if (!c || phase > CHR_PHASE_SPEAKING || c->phase == phase) return;
    CHR_MARK("PH %u", (unsigned)phase);
    const uint8_t before = c->phase;
    if (phase == CHR_PHASE_THINKING || phase == CHR_PHASE_IDLE) {
        /* The user has had the word (or the talk is over): an answer to come is a new one. */
        c->telling = c->user_turn = c->answer_guessed = false;
        c->told = c->sentence_mood = CHR_MOOD_COUNT;
        c->sentence_count = 0;
        c->answer_at = -1.0;
        c->answer_armed = phase == CHR_PHASE_THINKING;
        if (phase == CHR_PHASE_IDLE) c->asked = CHR_MOOD_COUNT;     /* what the AI asked for was of that talk */
    }
    if (phase == CHR_PHASE_SPEAKING) c->user_turn = false;     /* her answer: the user's turn is over */
    c->phase = (uint8_t)phase;
    c->phase_age = 0.0f;
    c->leant = false;
    /* A round of a talk begins when she is called, and with every turn of yours. */
    if (before == CHR_PHASE_IDLE || phase == CHR_PHASE_LISTENING) c->round_poses = 0;
    /* From speaking straight to listening while she could still be heard: perhaps someone barged in. */
    c->cut_ms = before == CHR_PHASE_SPEAKING && phase == CHR_PHASE_LISTENING && c->silent_ms < 180.0f ? 0.0f : -1.0f;
    if (before == CHR_PHASE_IDLE) {
        /* Called out of a doze she stretches (company()); otherwise her eyes light up. */
        const bool slept = c->dozing;
        company(c, 0.25f, 0.05f);
        if (!slept) chr_cue(c, CHR_CUE_WAKE, 0);
    } else if (phase == CHR_PHASE_IDLE) {
        c->parting = c->parted = true;
        c->parting_ms = 0.0f;
        /* What the page gave before this moment, and not while there was no talk, was the talk's. */
        c->talked = c->given_ms > 0.0f && c->given != c->home;
        return;
    }
    c->parting = c->parted = false;
}

void chr_return(chr_t *c, bool show)
{
    if (!c) return;
    c->phase_age = 1000.0f;
    c->parting = c->parted = c->talked = c->leant = false;
    c->sentence_ms = c->unheard_ms = c->hearing_ms = c->voice_step_ms = 10000.0f;
    c->silent_ms = talk_phase(c) != CHR_PHASE_IDLE && c->env_speaker >= VOICE_GATE ? 0.0f : 10000.0f;    /* still heard, or not */
    c->after_ms = show ? 10000.0f : 0.0f;              /* nor is the page's next word on it made a show of */
    c->cut_ms = c->swap_ms = c->change_ms = -1.0f;
    c->voice_step = 0;
    c->mouth = c->listen = 0.0f;
    c->cue = CHR_CUE_NONE;
    c->down = c->taken = false;
    c->touch = CHR_TOUCH_NONE;
    c->release_ms = 10000.0f;
    c->pet = c->squish = c->sulk_ms = 0.0f;
    c->pokes = 0;
    c->idle_in = maxf(c->idle_in, 3000.0f);
    /* (Asleep she sleeps on.) */
    c->gesture = c->dozing ? CHR_GESTURE_SLEEP : CHR_GESTURE_NONE;
    if (c->pose && c->pose != c->gesture) pose_enter(c, 0);
    c->swing = c->swing_v = 0.0f;
    c->head_known = false;
    for (int i = 0; i < CHR_PARTICLES_MAX; ++i) c->particles[i].live = 0;
    c->round_poses = 0;
    c->easing = false;
    c->face_cut = true;
    if (show) {
        c->given_ms = 0.0f;
        if (c->mood == c->given) show_mood(c, CHR_MOOD_NEUTRAL, true);
    } else {
        c->wanted = mood_now(c);
        show_mood(c, c->wanted, true);
        c->eyes_shown = look_wanted(c);
    }
    c->mood_talk = talk_phase(c) != CHR_PHASE_IDLE;
    c->mood_age = maxf(c->mood_age, (float)MOOD_BODY_MS);
    c->strength = MOOD_FULL;
    c->mouth_lag = 0.0f;
    c->eyes_shown = look_wanted(c);
    compose(c, c->target);
    c->blush = c->blush_from = clampf(c->target[P_BLUSH], 0.0f, 1.0f);
    for (size_t i = 0; i < sizeof(c->emit_in) / sizeof(c->emit_in[0]); ++i) c->emit_in[i] = 1500.0f;
    compose(c, c->target);
    memcpy(c->p, c->target, sizeof(c->p));
    memset(c->v, 0, sizeof(c->v));
    c->snap = false;
}

void chr_woken(chr_t *c, bool stretch)
{
    if (!c) return;
    c->dozing = false;
    company(c, 0.25f, 0.0f);
    if (stretch && talk_phase(c) == CHR_PHASE_IDLE) chr_cue(c, CHR_CUE_STRETCH, 0);
}

void chr_set_level(chr_t *c, uint8_t level)
{
    if (c) c->level = level > 3U ? 3U : level;
}

void chr_set_envelope(chr_t *c, uint16_t microphone, uint16_t speaker)
{
    if (!c) return;
    CHR_MARK("LV %u %u", (unsigned)microphone, (unsigned)speaker);
    c->mic_level = (float)microphone;
    c->env_mic = clampf(((float)microphone - 60.0f) / 1400.0f, 0, 1);
    c->env_speaker = (float)speaker;
    c->sounded = true;
}

void chr_no_envelope(chr_t *c)
{
    if (!c) return;
    c->env_mic = c->env_speaker = c->mic_level = c->env_ahead = 0.0f;
    c->sounded = c->ahead = false;
}

void chr_set_ahead(chr_t *c, uint16_t speaker, bool known)
{
    if (!c) return;
    CHR_MARK("LA %u %u", (unsigned)speaker, (unsigned)known);
    c->env_ahead = known ? (float)speaker : 0.0f;
    c->ahead = known;
}

void chr_pass(chr_t *c, uint32_t ms)
{
    if (!c) return;
    CHR_MARK("PASS %u", (unsigned)ms);
    c->away_at = c->now;
    c->now += (double)ms;
    c->back_at = c->now;
}

static void sentence_fx(chr_t *c);

/* Her face, at once, as a sentence begins to be heard (with its little effect) or as the AI asked for one. */
static void tell(chr_t *c, size_t mood, bool sentence)
{
    c->told = (uint8_t)mood;
    if (c->mood != mood && !c->held) {
        c->easing = false;
        c->mood_wait = 0.0f;
        c->wanted = mood;
        show_mood(c, mood, false);
    }
    if (!sentence) return;
    CHR_MARK("SAY %s", moods[mood].name);
    c->sentence_ms = 0.0f;
    sentence_fx(c);
}

void chr_sentence(chr_t *c, int32_t begin_ms, size_t mood)
{
    if (!c || mood >= CHR_MOOD_COUNT) return;
    CHR_MARK("SENT %d %s", (int)begin_ms, moods[mood].name);
    c->telling = true;
    c->user_turn = false;
    if (begin_ms == CHR_SENTENCE_NOW) {
        c->asked = (uint8_t)mood;
        tell(c, mood, false);
        return;
    }
    if (begin_ms == CHR_SENTENCE_OWN) {
        c->asked = CHR_MOOD_COUNT;
        tell(c, c->sentence_mood < CHR_MOOD_COUNT ? c->sentence_mood : mood, false);
        return;
    }
    if (c->sentence_count == CHR_SENTENCES) {
        memmove(&c->sentence[0], &c->sentence[1], sizeof(c->sentence[0]) * (CHR_SENTENCES - 1U));
        --c->sentence_count;
    }
    c->sentence[c->sentence_count].came = c->now;
    c->sentence[c->sentence_count].begin = begin_ms;
    c->sentence[c->sentence_count].mood = (uint8_t)mood;
    /* One that waited for her while she was away was heard then, as far as is known when she went. */
    c->sentence[c->sentence_count].onset = c->now == c->back_at ? c->away_at : -1.0;
    ++c->sentence_count;
}

/* When the i-th sentence told is to be heard (< 0: not known yet): where its answer's first sound was, and as far into
 * it as the service said; one that it did not say of begins with the first sound after a pause since it came (or the
 * answer's first), or, when none came, a moment after it. Nothing is known while the answer's beginning is not (it
 * is still to come, or another answer is awaited). */
static double sentence_due(const chr_t *c, size_t i)
{
    if (c->answer_at < 0.0 || c->answer_armed) return -1.0;
    if (c->sentence[i].begin >= 0) return c->answer_at + (double)c->sentence[i].begin;
    if (!i && c->sentence_mood >= CHR_MOOD_COUNT) return c->answer_at;
    if (c->sentence[i].onset >= 0.0) return c->sentence[i].onset;
    return c->now - c->sentence[i].came > ONSET_WAIT_MS ? c->sentence[i].came + ONSET_WAIT_MS : -1.0;
}

static void sentence_drop(chr_t *c)
{
    c->sentence_mood = c->sentence[0].mood;
    memmove(&c->sentence[0], &c->sentence[1], sizeof(c->sentence[0]) * (CHR_SENTENCES - 1U));
    --c->sentence_count;
}

/* The sentences told, in their order, each as it is about to be heard. */
static void sentences_step(chr_t *c, bool heard, bool coming)
{
    if (!c->sentence_count) return;
    if (talk_phase(c) == CHR_PHASE_IDLE) {
        c->sentence_count = 0;
        return;
    }
    /* Nothing of the answer is heard (the speaker is off, or its voice is not played): a while after its first
     * sentence came she takes it as begun then; its sentences are late ones, her face without the little effect. */
    if ((c->answer_at < 0.0 || c->answer_armed) && c->now - c->sentence[0].came > UNHEARD_MS) {
        c->answer_at = c->sentence[0].came;
        c->answer_armed = c->answer_foreseen = false;
        c->answer_guessed = true;
    }
    /* The next one's time has come already (she was not on the screen while those before it were heard): the last
     * of them, not each in turn. */
    while (c->sentence_count > 1U) {
        const double next = sentence_due(c, 1);
        if (next < 0.0 || c->now < next - (double)CHR_LEAD_MS) break;
        sentence_drop(c);
    }
    const double due = sentence_due(c, 0);
    if (due < 0.0 || c->now < due - (double)CHR_LEAD_MS) return;
    /* The last told, whose face would be over by now (she was not on the screen while it was heard, and nothing of her
     * is heard now nor coming): it is not shown at all. */
    const double last = c->voice_at > due ? c->voice_at : due;
    if (c->sentence_count == 1U && !heard && !coming && c->now > last + (double)MOOD_AFTER_TALK_MS) {
        sentence_drop(c);
        return;
    }
    /* Its time has come but nothing is on its way to be heard (the service counts a pause before it in): she waits for
     * its sound, a little while. */
    if (c->ahead && !coming && !heard && c->now < due + (double)SENTENCE_WAIT_MS) return;
    sentence_drop(c);
    c->told_at = c->now;
    /* One told after it had begun to be heard (the service was late), or in an answer that is not heard: her face, not
     * the little effect of a start. What the AI asked for stands over it while it lasts. */
    tell(c, c->asked < CHR_MOOD_COUNT ? c->asked : c->sentence_mood,
         !c->answer_guessed && c->now <= due + (double)SENTENCE_WAIT_MS);
}

/* A sound begins (or is about to): the first of an answer, and the one that a sentence nobody said the time of begins
 * with. `real`: heard now, not foreseen; it puts right what was foreseen a moment ago. */
static void sound_begins(chr_t *c, double at, bool real)
{
    if (c->answer_armed) {
        c->answer_at = at;
        c->answer_armed = c->answer_guessed = false;
        c->answer_foreseen = !real;
        CHR_MARK("H0 %u", (unsigned)real);
    } else if (real && c->answer_foreseen && at - c->answer_at < 250.0 && at - c->answer_at > -250.0) {
        c->answer_at = at;
        c->answer_foreseen = false;
    } else if (real && c->answer_guessed) {
        /* Taken as begun for want of a sound: the first that comes soon after is its beginning. */
        if (at - c->answer_at < GUESS_SLACK_MS) c->answer_at = at;
        c->answer_guessed = false;
    }
    for (uint8_t i = 0; i < c->sentence_count; ++i)
        if (c->sentence[i].begin < 0 && c->sentence[i].onset < 0.0 && c->sentence[i].came < c->now) c->sentence[i].onset = at;
}

void chr_set_conditions(chr_t *c, uint32_t conditions)
{
    if (!c) return;
    /* Called: she looks at you, and leaves what she was doing for herself, or the goodbye. */
    if ((conditions & ~c->conditions & CHR_STARTING) && c->phase == CHR_PHASE_IDLE) {
        c->look_x = c->look_y = 0.0f;
        c->mood_age = maxf(c->mood_age, (float)MOOD_BODY_MS);
        if ((c->cue >= CHR_CUE_HOP && c->cue <= CHR_CUE_TWITCH) || (c->cue >= CHR_CUE_WAVE && c->cue <= CHR_CUE_DANCE) ||
            c->cue == CHR_CUE_BYE) c->cue = CHR_CUE_NONE;
    }
    c->conditions = conditions;
}

static float within(float value, float most) { return most * tanhf(value / most); }

float chr_flat_x(const chr_t *c)
{
    return within(c->out[P_BX] + c->out[P_LEAN] * CHR_FLAT_LEAN, c->cue == CHR_CUE_DANCE ? CHR_FLAT_SIDE_DANCE : CHR_FLAT_SIDE);
}

float chr_flat_y(const chr_t *c) { return within(c->out[P_BY], CHR_FLAT_UP); }
float chr_flat_tall(const chr_t *c) { return within(-c->out[P_HY] * CHR_FLAT_GIVE, CHR_FLAT_TALL); }

void chr_head_offset(const chr_t *c, float *x, float *y)
{
    if (c->features & CHR_FLAT) {
        /* All of her moves as one piece: her head is where growing about her feet puts it. */
        const float part = c->fig.hair_top ? (float)c->fig.eye_y / (float)c->fig.hair_top : 1.0f;
        *x = floorf(chr_flat_x(c) + 0.5f);
        *y = chr_flat_y(c) - chr_flat_tall(c) * part;
        return;
    }
    *x = c->out[P_BX] + c->out[P_GX] * 3.0f;
    *y = c->out[P_BY] + c->out[P_HY] + c->out[P_GY] * 2.0f;
}

chr_rect_t chr_body_box(const chr_t *c)
{
    /* From the brows down to 40 pixels above the edge of the page, a little narrower than the head. */
    const int half = c->fig.head_half - 10, top = c->fig.brow_y + 6;
    return (chr_rect_t){(int16_t)(c->anchor_x + c->fig.face_x - half), (int16_t)(c->anchor_y + top), (int16_t)(2 * half),
                        (int16_t)(-40 - top)};
}

uint32_t chr_features(const chr_t *c) { return c ? c->features : 0U; }

/* ------------------------------------------------------------------- touch */

static uint8_t region_at(const chr_t *c, float x, float y)
{
    const chr_figure_t *f = &c->fig;
    float hx, hy;
    chr_head_offset(c, &hx, &hy);
    const float fx = x - hx, fy = y - hy, face = (float)f->face_x, half = (float)f->head_half;
    if ((c->features & CHR_HAS_EARS) && fy > f->ears_top - 8 && fy < f->hair_top + 14) {
        if (fx > f->ear_lx0 && fx < f->ear_lx1) return R_EAR_L;
        if (fx > f->ear_rx0 && fx < f->ear_rx1) return R_EAR_R;
    }
    /* The hand she holds out, while she holds it out. */
    if (c->gesture == CHR_GESTURE_HIGHFIVE && f->hand_half) {
        const float reach = (float)f->hand_half + 14.0f, by = c->out[P_BY];
        if (f->hand_rx && fabsf(x - f->hand_rx) < reach && fabsf(y - by - f->hand_ry) < reach) return R_HAND;
        if (f->hand_lx && fabsf(x - f->hand_lx) < reach && fabsf(y - by - f->hand_ly) < reach) return R_HAND;
    }
    if (fx > face - half - 5 && fx < face + half + 5 && fy >= f->hair_top - 4 && fy < f->brow_y + 6) return R_HEAD;
    if (fx > face - half + 7 && fx < face + half - 7 && fy >= f->brow_y + 6 && fy < f->neck_y) return R_BODY;
    /* Her arms hang beside the body: a sleeve's width around each. */
    if (f->arm_top && y - c->out[P_BY] > f->arm_top && y < -16) {
        if (fabsf(x - f->arm_lx) < 34.0f) return R_ARM_L;
        if (fabsf(x - f->arm_rx) < 34.0f) return R_ARM_R;
    }
    if (x > -f->shoulder_half && x < f->shoulder_half && y - c->out[P_BY] >= f->neck_y && y < -16) return R_BODY;
    if ((c->features & CHR_HAS_TAIL) && x > f->tail_x0 && x < f->tail_x1 + 8 && y > f->tail_top - 6 && y < 0) return R_TAIL;
    return R_OUT;
}

bool chr_pointer(chr_t *c, bool down, int16_t x, int16_t y)
{
    if (!c) return false;
    const float dx = (float)x - c->anchor_x, dy = (float)y - c->anchor_y;
    if (down && !c->down) {
        c->start_x = dx; c->start_y = dy;
        c->travel = 0.0f; c->held_ms = 0.0f; c->taken = false;
        c->region = region_at(c, dx, dy);
        c->touch = CHR_TOUCH_WATCH;
        if (c->dozing) {
            company(c, 0.2f, 0.0f);
            c->region = R_WOKEN;
        }
    } else if (down) {
        c->travel += fabsf(dx - c->touch_x) + fabsf(dy - c->touch_y);
    }
    if (down) { c->touch_x = dx; c->touch_y = dy; }
    c->down = down;
    return c->taken;
}

static void release(chr_t *c)
{
    const float dx = c->touch_x - c->start_x, dy = c->touch_y - c->start_y;
    const uint8_t was = c->touch;
    c->touch = CHR_TOUCH_NONE;
    c->release_ms = 0.0f;
    if (was == CHR_TOUCH_PULL) { chr_cue(c, CHR_CUE_FLICK, c->region == R_EAR_L ? 1 : -1); return; }
    if (was == CHR_TOUCH_SQUISH) { c->v[P_HY] -= 170.0f; push(c, P_BY, -240.0f); return; }
    if (was == CHR_TOUCH_PET || c->region == R_WOKEN) return;
    if (was == CHR_TOUCH_HOLD) { chr_cue(c, CHR_CUE_NOD, 0); return; }
    if (c->region == R_TAIL) { chr_cue(c, CHR_CUE_TAIL, 0); return; }
    if (c->held_ms < 450.0f && c->travel < 20.0f && (c->region == R_ARM_L || c->region == R_ARM_R || c->region == R_HAND)) {
        /* A tap on her arm: she holds a hand out. A tap on that hand, or on the arm again while it
         * is out: a high five. */
        if (c->sulk_ms > 0.0f) return;
        const int32_t side = c->region == R_ARM_R || (c->region == R_HAND && c->cue_dir < 0) ? -1 : 1;
        company(c, 0.1f, 0.02f);
        chr_cue(c, c->cue == CHR_CUE_OFFER ? CHR_CUE_HIGHFIVE : CHR_CUE_OFFER, c->cue == CHR_CUE_OFFER ? (int32_t)c->cue_dir : side);
        return;
    }
    if (c->held_ms < 450.0f && c->travel < 20.0f) {
        if (c->region == R_OUT) {
            const int32_t gx = (int32_t)(clampf(c->touch_x / 300.0f, -1, 1) * 100.0f);
            const int32_t gy = (int32_t)(clampf((c->touch_y - c->fig.eye_y) / 220.0f, -1, 1) * 100.0f);
            chr_cue(c, CHR_CUE_LOOK, (int32_t)(((uint32_t)(uint16_t)(int16_t)gy << 16) | (uint16_t)(int16_t)gx));
            return;
        }
        if (c->sulk_ms > 0.0f) return;
        company(c, 0.1f, 0.0f);
        c->pokes = c->poke_ms < 1800.0f ? (uint8_t)(c->pokes + 1U) : 1U;
        c->poke_ms = 0.0f;
        const int32_t side = c->touch_x < c->fig.face_x ? -1 : 1;
        if (c->pokes >= 6U) { c->pokes = 0; chr_cue(c, CHR_CUE_POUT, side); }
        else if (c->pokes >= 3U) chr_cue(c, CHR_CUE_DIZZY, side);
        else chr_cue(c, CHR_CUE_POKE, side);
        return;
    }
    if (c->travel > 110.0f && c->held_ms < 650.0f) {
        if (fabsf(dx) > fabsf(dy)) chr_cue(c, CHR_CUE_SWAY, dx < 0 ? -1 : 1);
        else chr_cue(c, dy < 0 ? CHR_CUE_JUMP : CHR_CUE_DUCK, 0);
    }
}

static void touch_step(chr_t *c, float dt)
{
    c->poke_ms += dt;
    c->release_ms += dt;
    if (c->sulk_ms > 0.0f) c->sulk_ms -= dt;
    const bool petting = c->touch == CHR_TOUCH_PET, squishing = c->touch == CHR_TOUCH_SQUISH;
    c->pet += ((petting ? 1.0f : 0.0f) - c->pet) * clampf(dt / (petting ? 180.0f : 900.0f), 0, 1);
    c->squish += ((squishing ? 1.0f : 0.0f) - c->squish) * clampf(dt / 110.0f, 0, 1);
    if (!c->down) {
        if (c->touch != CHR_TOUCH_NONE) release(c);
        return;
    }
    c->held_ms += dt;
    c->watch_x = clampf(c->touch_x / 300.0f, -1, 1);
    c->watch_y = clampf((c->touch_y - c->fig.eye_y) / 220.0f, -1, 1);
    if (c->touch == CHR_TOUCH_WATCH) {
        if (c->region == R_EAR_L || c->region == R_EAR_R) {
            c->touch = CHR_TOUCH_PULL; c->taken = true;
        } else if (c->region == R_TAIL) {
            c->taken = true;
        } else if ((c->region == R_ARM_L || c->region == R_ARM_R || c->region == R_HAND) && c->held_ms > 500.0f &&
                   c->travel < 14.0f && c->sulk_ms <= 0.0f) {
            /* A finger resting on her arm: her hand is held. */
            c->touch = CHR_TOUCH_HOLD; c->taken = true;
            c->cue = CHR_CUE_NONE;
        } else if (c->region == R_HEAD && fabsf(c->touch_x - c->start_x) > 14.0f) {
            c->touch = CHR_TOUCH_PET; c->taken = true;
            if (c->sulk_ms > 0.0f) { c->sulk_ms = 0.0f; c->cue = CHR_CUE_NONE; }
            if (fancy(c)) {
                if (talk_phase(c) == CHR_PHASE_IDLE) fx_praise(c);
                chr_spark(c, FX_GLINT, c->touch_x, c->touch_y, 0.0f, 600.0f, 1.2f);
                c->emit_in[FX_HEART] = 1500.0f;
            }
        } else if ((c->region == R_HEAD || c->region == R_BODY) && c->held_ms > 500.0f && c->travel < 14.0f) {
            c->touch = CHR_TOUCH_SQUISH; c->taken = true;
        }
    }
    if (c->touch == CHR_TOUCH_HOLD) {
        c->emit_in[FX_HEART] -= dt;
        if (c->emit_in[FX_HEART] <= 0.0f) {
            chr_emit(c, FX_HEART, c->touch_x + chr_uniform(c, -20, 20), c->touch_y - 30, chr_uniform(c, -10, 10), -50,
                     chr_uniform(c, 12, 16), 1200);
            c->emit_in[FX_HEART] = 900.0f;
        }
    }
    if (c->touch == CHR_TOUCH_PET) {
        c->emit_in[FX_HEART] -= dt;
        if (c->emit_in[FX_HEART] <= 0.0f) {
            chr_emit(c, FX_HEART, c->fig.face_x + chr_uniform(c, -0.85f, 0.85f) * (float)c->fig.head_half, c->fig.hair_top + 30, chr_uniform(c, -16, 16), -62,
                     chr_uniform(c, 14, 20), 1100);
            c->emit_in[FX_HEART] = fancy(c) ? 900.0f : 420.0f;
        }
    }
}

/* -------------------------------------------------------------------- step */

/* `still`: the damping for a figure in one piece, where swinging past the mark would rock all of her.
 * The corners of the mouth and the blush take about 0.3 s. */
static const struct { uint8_t first, last; float omega, zeta, still; } springs[] = {
    {P_BX, P_BY, 15, .55f, .95f}, {P_HY, P_HY, 17, .45f, .95f}, {P_LEAN, P_LEAN, 11, .5f, .95f}, {P_GX, P_GY, 22, .85f, .9f},
    {P_EOL, P_ARC, 26, .9f, .9f}, {P_SLANT, P_BROW, 20, .8f, .9f}, {P_MC, P_MC, 14, 1, 1}, {P_MO, P_MO, 24, .8f, .9f},
    {P_BLUSH, P_BLUSH, 14, 1, 1}, {P_EAR_L, P_EAR_R, 14, .45f, .45f}, {P_WAG, P_WAG, 6, 1, 1}, {P_GREY, P_GREY, 5, 1, 1},
    {P_ARM_L, P_ARM_R, 13, .62f, .62f}, {P_YAW, P_YAW, 16, .8f, .8f},
};

static void follow(chr_t *c, float dt_s)
{
    for (size_t g = 0; g < sizeof(springs) / sizeof(springs[0]); ++g) {
        const float zeta = (c->features & CHR_FLAT) ? springs[g].still : springs[g].zeta;
        const float k = springs[g].omega * springs[g].omega, d = 2.0f * zeta * springs[g].omega;
        for (unsigned i = springs[g].first; i <= springs[g].last; ++i) {
            c->v[i] += (k * (c->target[i] - c->p[i]) - d * c->v[i]) * dt_s;
            c->p[i] += c->v[i] * dt_s;
        }
    }
    /* Hair swings behind the head: it is pushed by how far the head moved. */
    const float head = c->p[P_BX] + c->p[P_LEAN];
    if (c->head_known) c->swing -= (head - c->head_x) * 0.9f;
    c->head_x = head;
    c->head_known = true;
    c->swing_v += (-130.0f * c->swing - 7.0f * c->swing_v) * dt_s;
    c->swing = clampf(c->swing + c->swing_v * dt_s, -40, 40);
}

static void emitters(chr_t *c, float dt)
{
    const mood_t *m = &moods[c->mood];
    const bool calm = talk_phase(c) == CHR_PHASE_IDLE && !(c->conditions & (CHR_ALARM | CHR_RINGING | CHR_IN_CALL));
    static const struct { uint16_t flag; uint8_t kind; uint16_t every; } table[] = {
        {M_HEARTS, FX_HEART, 620}, {M_ZED, FX_ZED, 1500}, {M_NOTES, FX_NOTE, 1700}, {M_TEARS, FX_DROP, 520},
        {M_SWEAT, FX_DROP, 1400}, {M_PUFF, FX_PUFF, 900},
    };
    /* A figure with pictures of effects has her hearts when they come (fx_praise()), and she does not cry, sweat or fume:
     * those are her face's. */
    const uint32_t kept = fancy(c) ? (uint32_t)(M_ZED | M_NOTES) : ~0U;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (!(m->flags & table[i].flag & kept) || !calm || c->mood_talk || (c->conditions & (CHR_OFFLINE | CHR_CONNECTING))) continue;
        float *in = &c->emit_in[table[i].kind];
        *in -= dt;
        if (*in > 0.0f) continue;
        *in = (float)table[i].every * chr_uniform(c, 0.8f, 1.25f);
        const float side = chr_random(c) & 1U ? 1.0f : -1.0f;
        switch (table[i].flag) {
        case M_HEARTS:
            chr_emit(c, FX_HEART, c->fig.face_x + chr_uniform(c, -1.15f, 1.15f) * (float)c->fig.head_half, c->fig.brow_y, chr_uniform(c, -14, 14), -58,
                     chr_uniform(c, 14, 21), 1300);
            break;
        case M_ZED:
            chr_emit(c, FX_ZED, chr_side_x(c) - CHR_SIDE * 32, c->fig.hair_top + 70, CHR_SIDE * 20, -30, 17, 2300);
            /* Now and then a small star shines beside her. */
            if (fancy(c) && (chr_random(c) % 3U) == 0U)
                chr_spark(c, FX_GLINT, face_x(c) + either(c) * chr_uniform(c, 180, 300), top_y(c) + chr_uniform(c, -10, 160), 600.0f, 900.0f, 0.9f);
            break;
        case M_NOTES: chr_emit(c, FX_NOTE, chr_side_x(c) + CHR_SIDE * 7.0f, c->fig.brow_y - 30, CHR_SIDE * chr_uniform(c, 0, 12), -40, 22, 1900); break;
        case M_TEARS: chr_emit(c, FX_DROP, side > 0 ? c->fig.eye_lx + 4 : c->fig.eye_rx - 4, c->fig.eye_y + 16, side * 22, 50, 12, 800); break;
        case M_SWEAT: chr_emit(c, FX_DROP, chr_side_x(c) - CHR_SIDE * 28, c->fig.brow_y - 16, CHR_SIDE * 10, 24, 16, 1000); break;
        default: chr_emit(c, FX_PUFF, (float)c->fig.face_x + side * (float)(c->fig.head_half + 5), c->fig.brow_y - 10, 0, -46, 18, 800); break;
        }
    }
    if (calm && (c->conditions & CHR_OFFLINE) && !c->cue) {
        c->emit_in[FX_QUESTION] -= dt;
        if (c->emit_in[FX_QUESTION] <= 0.0f) {
            chr_emit(c, FX_QUESTION, chr_side_x(c), c->fig.hair_top + 60, CHR_SIDE * 10, -30, 32, 1700);
            c->emit_in[FX_QUESTION] = 5200.0f;
        }
    }
    /* A dance with pictures: an effect on every beat, as the picture changes. */
    const bool stepping = c->pose == CHR_GESTURE_DANCE && c->clipped && fancy(c);
    if (stepping && c->entered && c->beat_seen != c->clip_at + 1U) {
        c->beat_seen = (uint16_t)(c->clip_at + 1U);
        fx_beat(c, c->clip_at);
    } else if (!stepping) c->beat_seen = 0;
    if (c->cue == CHR_CUE_HUM || c->cue == CHR_CUE_DIZZY || c->cue == CHR_CUE_YAWN || (c->cue == CHR_CUE_DANCE && !stepping)) {
        c->emit_in[FX_NOTE] -= dt;
        if (c->emit_in[FX_NOTE] <= 0.0f) {
            c->emit_in[FX_NOTE] = c->cue == CHR_CUE_DIZZY ? 420.0f : c->cue == CHR_CUE_YAWN ? 2500.0f : 640.0f;
            if (c->cue == CHR_CUE_HUM || c->cue == CHR_CUE_DANCE)
                chr_emit(c, FX_NOTE, chr_side_x(c) + CHR_SIDE * chr_uniform(c, -34, 16), c->fig.brow_y - 40, CHR_SIDE * 16, -44, 22, 1700);
            else if (c->cue == CHR_CUE_DIZZY)
                chr_emit(c, FX_SPARK, c->fig.face_x + chr_uniform(c, -0.7f, 0.7f) * (float)c->fig.head_half, c->fig.hair_top + 30, chr_uniform(c, -50, 50), -20, 12, 700);
            else if (c->cue_age > 1300.0f) chr_emit(c, FX_DROP, c->fig.eye_lx + 14, c->fig.eye_y + 10, 12, 30, 10, 700);
        }
    }
}

static void idle_life(chr_t *c, float dt)
{
    const mood_t *m = &moods[c->mood];
    /* Blink. */
    c->blink_turned = false;
    if (c->blink_age >= 0.0f) {
        const float closing = c->slow_blink ? SLOW_SHUT_MS : BLINK_SHUT_MS;
        c->blink_turned = c->blink_age < closing && c->blink_age + dt >= closing;
        c->blink_age += dt;
        if (c->blink_age > (c->slow_blink ? SLOW_SHUT_MS + SLOW_OPEN_MS : 170.0f)) {
            c->blink_age = -1.0f;
            /* A flat figure blinks every three to six seconds. */
            c->blink_in = c->blink_twice ? 120.0f : (c->features & CHR_FLAT) ? chr_uniform(c, 3000, 6000) : chr_uniform(c, 2200, 5600);
            c->blink_twice = false;
            c->slow_blink = false;
        }
    } else if ((c->blink_in -= dt) <= 0.0f) {
        c->blink_age = 0.0f;
        c->blink_twice = !c->slow_blink && c->blink_in > -1000.0f && (chr_random(c) % 7U) == 0U;
    }
    if ((m->flags & M_SMILES) && (c->smile_in -= dt) < -SMILE_MS) c->smile_in = chr_uniform(c, 4500, 8500);
    /* Glances; a flat figure looks at you. */
    if ((c->look_in -= dt) <= 0.0f) {
        const bool away = !(c->features & CHR_FLAT) && (chr_random(c) % 10U) < 6U && talk_phase(c) == CHR_PHASE_IDLE &&
                          !(c->conditions & CHR_STARTING);
        c->look_x = away ? chr_uniform(c, -0.7f, 0.7f) : 0.0f;
        c->look_y = away ? chr_uniform(c, -0.3f, 0.25f) : 0.0f;
        c->look_in = chr_uniform(c, 1600, 4600);
    }
    /* Something small to do. */
    const bool busy = talk_phase(c) != CHR_PHASE_IDLE || c->cue || c->touch != CHR_TOUCH_NONE || c->release_ms < 2500.0f ||
                      (c->conditions & (CHR_ALARM | CHR_RINGING | CHR_IN_CALL | CHR_OFFLINE | CHR_CONNECTING | CHR_STARTING));
    if (busy) return;
    if ((c->idle_in -= dt) > 0.0f) return;
    if (c->dozing) return;
    const int32_t side = chr_random(c) & 1U ? 1 : -1;
    const bool drowsy = !strcmp(m->name, "sleepy") || !strcmp(m->name, "relaxed") || c->energy < 0.3f;
    const bool low = low_mood(m);
    const bool ears = (c->features & CHR_HAS_EARS) != 0, tail = (c->features & CHR_HAS_TAIL) != 0;
    const uint32_t pick = chr_random(c) % 12U;
    /* What she does depends on how awake she is; a low mood keeps her to a look aside. The things
     * that are done with ears or a tail fall to a figure that has them; another one looks round. */
    chr_cue_t act;
    if (c->features & CHR_FLAT) {
        /* A young lady: she hums, stands a while with her hands clasped (a glance, for a figure that looks round), and
         * dances a little when she is in high spirits and close to you. She does not hop, and a low mood keeps still. */
        if (low) act = CHR_CUE_NONE;
        else if (drowsy) act = pick < 6U ? CHR_CUE_YAWN : pick < 10U ? CHR_CUE_NOD_OFF : CHR_CUE_GLANCE;
        else if (pick < 5U) act = CHR_CUE_GLANCE;
        else if (pick < 10U || c->energy < 0.7f || c->closeness < 0.6f) act = CHR_CUE_HUM;
        else act = CHR_CUE_DANCE;
    } else if (low) act = ears ? CHR_CUE_TWITCH : CHR_CUE_GLANCE;
    else if (drowsy) act = pick < 6U ? CHR_CUE_YAWN : pick < 10U ? CHR_CUE_NOD_OFF : CHR_CUE_GLANCE;
    else if (c->energy < 0.6f) act = pick < 5U ? CHR_CUE_GLANCE : pick < 8U ? CHR_CUE_HUM : pick < 10U ? CHR_CUE_YAWN : CHR_CUE_HOP;
    else if (pick < 2U) act = CHR_CUE_HOP;
    else if (pick < 4U) act = CHR_CUE_HUM;
    else if (pick < 6U) act = ears ? CHR_CUE_EARS : CHR_CUE_GLANCE;
    else if (pick < 8U) act = tail ? CHR_CUE_WAG : CHR_CUE_GLANCE;
    else if (pick < 11U) act = ears ? CHR_CUE_TWITCH : pick < 10U ? CHR_CUE_GLANCE : CHR_CUE_HOP;
    else act = c->energy >= 0.7f && c->closeness >= 0.6f ? CHR_CUE_DANCE : CHR_CUE_YAWN;
    if (act == CHR_CUE_NONE) {
        c->idle_in = chr_uniform(c, 7000, 15000);
        return;
    }
    chr_cue(c, act, side);
    /* Close to you, she has more to do; less so, she keeps more to herself. */
    c->idle_in *= c->closeness > 0.7f ? 0.7f : c->closeness < 0.35f ? 1.5f : 1.0f;
}

/* What the mouth makes of loudness (0..1 between the gate and the loudest of late): small or medium.
 * Wider than that is for laughing, not for talking. A step is left a little later than it was entered. */
static uint8_t step_for(float loud, uint8_t now)
{
    const float from = 0.40f + (now == 1U ? 0.06f : now == 2U ? -0.06f : 0.0f);
    return loud >= from ? 2U : 1U;
}

/* As a sentence begins in a talk, a little of what goes with the mood it brought: two small hearts when she is happy,
 * gold sparkles when excited, pink lights when gentle or comforting; nothing bigger while she speaks. */
static void sentence_fx(chr_t *c)
{
    const mood_t *m = &moods[c->mood];
    if (!fancy(c) || !c->mood_talk) return;
    const bool happy = !strcmp(m->name, "happy"), excited = !strcmp(m->name, "excited");
    if (!happy && !excited && strcmp(m->name, "gentle") && strcmp(m->name, "comfort")) return;
    CHR_MARK("SFX %s", m->name);
    const chr_fx_t kind = happy ? FX_HEART_SMALL : excited ? FX_SPARK : FX_ORB_PINK;
    for (int k = 0; k < (excited ? 3 : 2); ++k) {
        const float s = k % 2 ? 1.0f : -1.0f;
        chr_particle_t *q = chr_spark(c, kind, face_x(c) + s * ((float)c->fig.head_half + chr_uniform(c, 40, 90)),
                                      (float)c->fig.eye_y + chr_uniform(c, -40, 30), 150.0f * (float)k, 800.0f, 0.7f);
        q->vy = -40.0f;
        q->sway = 5.0f;
    }
}

/* Her mouth follows what is heard from the speaker for as long as a talk lasts, whatever phase it is
 * in: an answer is heard to its end after the service has finished sending it. What is far below the
 * loudest of the last seconds, or next to nothing, is silence. The mouth opens at once; it shuts when
 * nothing has been heard for a moment, by way of the smallest step, so that it rests in every pause and
 * does not flutter between two syllables. Each step stays for a moment, shut too; only the way to shut
 * is not held up by a slow display. */
static void sound(chr_t *c, float dt)
{
    const bool talk = talk_phase(c) != CHR_PHASE_IDLE;
    float level = talk ? c->env_speaker : 0.0f;
    if (!c->sounded) {
        /* A product with no loudness to follow: the mouth still moves while she speaks. */
        const float t = (float)fmod(c->now, 100000.0);
        level = c->phase == CHR_PHASE_SPEAKING ? 9000.0f * clampf(0.45f + 0.35f * sinf(t * 0.021f) + 0.25f * sinf(t * 0.0337f + 1.3f), 0, 1) : 0.0f;
    }
    c->voice_peak = maxf(level, maxf(c->voice_peak * expf(-dt / VOICE_PEAK_MS), VOICE_PEAK_MIN));
    const float gate = maxf(VOICE_GATE, VOICE_PART * c->voice_peak);
    const bool voiced = level >= gate;
    /* What is about to be heard (CHR_AHEAD_MS ahead, as far as the speaker has been given it): a sound that will
     * begin after a pause begins her answer, or a sentence, a moment early. */
    const bool coming = talk && c->ahead && c->sounded && c->env_ahead >= gate;
    if (coming && c->ahead_silent_ms >= ANSWER_GAP_MS) sound_begins(c, c->now + (double)AHEAD_SEEN_MS, false);
    if (talk && voiced && c->silent_ms >= ANSWER_GAP_MS) sound_begins(c, c->now, true);
    if (voiced) c->voice_at = c->now;
    /* The quiet ahead as far as is seen: where nothing is written that far, what is not heard now is quiet too (a
     * new answer is seen coming after it); and as known, every frame of it, which alone shuts her mouth before a pause
     * is heard (nothing written ahead may only be a driver that holds little). */
    c->ahead_silent_ms = coming || (!c->ahead && voiced) ? 0.0f : minf(c->ahead_silent_ms + dt, 1000000.0f);
    c->ahead_sure_ms = c->ahead && !coming ? minf(c->ahead_sure_ms + dt, 1000000.0f) : 0.0f;
    /* Listening and quiet for a while, the next sound she makes is a new answer's (even without thinking first): where
     * it begins is not known until it does, and the last answer's face is no longer what a sentence brought. Not while
     * a sentence of the last is still to be shown (she was off the screen as it was heard). */
    if (c->phase == CHR_PHASE_LISTENING && !c->answer_armed && !c->sentence_count && c->silent_ms >= 500.0f &&
        (!c->ahead || c->ahead_silent_ms >= 500.0f)) {
        c->answer_armed = true;
        c->told = c->sentence_mood = CHR_MOOD_COUNT;
    }
    /* Listening, she hears the user begin to speak (a voice well above what is left of her own after the echo is
     * cancelled, while nothing of her is heard): what her answer brought ends at once. */
    const bool user = c->phase == CHR_PHASE_LISTENING && !(c->conditions & CHR_MUTED) && c->mic_level >= USER_VOICE &&
                      c->silent_ms >= 150.0f && (!c->ahead || c->ahead_silent_ms >= 150.0f);
    c->user_ms = user ? c->user_ms + dt : 0.0f;
    if (user && c->user_ms >= USER_VOICE_MS && !c->user_turn) {
        CHR_MARK("USER %u", (unsigned)c->mic_level);
        c->user_turn = true;
        c->telling = false;
        c->sentence_count = 0;
        c->told = c->sentence_mood = CHR_MOOD_COUNT;
        if (!c->held && c->mood != CHR_MOOD_NEUTRAL) {
            c->easing = false;
            c->wanted = CHR_MOOD_NEUTRAL;
            show_mood(c, CHR_MOOD_NEUTRAL, true);
        }
    }
    sentences_step(c, voiced, coming);
    c->sentence_ms += dt;
    if (!c->telling && voiced && c->silent_ms >= SENTENCE_GAP_MS && c->sentence_ms >= SENTENCE_EVERY_MS) {
        c->sentence_ms = 0.0f;
        sentence_fx(c);
    }
#if CHR_MARKS
    if (talk && voiced != (c->silent_ms == 0.0f)) CHR_MARK("SND %u", (unsigned)voiced);
#endif
    c->silent_ms = voiced ? 0.0f : minf(c->silent_ms + dt, 1000000.0f);
    c->voice_step_ms += dt;
    uint8_t step = c->voice_step;
    if (voiced) {
        step = step_for((level - gate) / (c->voice_peak - gate), c->voice_step);
    }
    /* A pause long enough: she shuts her mouth (as soon as it is heard where it is known before), by way of the small
     * step for a frame, not held as a step is: shut within two frames. */
    const bool shutting = !voiced && step && (c->silent_ms >= SHUT_AFTER_MS || c->ahead_sure_ms >= SHUT_AFTER_MS);
    if (shutting) step = step > 1U ? 1U : 0U;
    if (step != c->voice_step && (c->voice_step_ms >= STEP_MS || shutting)) {
        if (!step) {
            c->shut_ms = c->silent_ms;
            c->word_shut = true;
        }
        c->voice_step = step;
        c->voice_step_ms = 0.0f;
    }
    c->mouth = 0.25f * (float)c->voice_step;

    /* A microphone that is off hears nobody, whatever the level says; nor is it quiet then. */
    const bool deaf = (c->conditions & CHR_MUTED) != 0;
    if (deaf) {
        c->env_mic = 0.0f;
        c->leant = false;
    }
    c->hearing_ms = deaf ? 0.0f : c->hearing_ms + dt;
    const bool noticed = !deaf && (sqrtf(c->env_mic) > 0.35f || c->level >= 2U);
    c->unheard_ms = noticed ? 0.0f : c->unheard_ms + dt;
    const float quiet = minf(minf(c->phase_age, c->hearing_ms), minf(c->unheard_ms, c->silent_ms));
    if (c->phase == CHR_PHASE_LISTENING && quiet > (float)LISTEN_AFTER_MS) c->leant = true;
    /* Cut off in the middle of a word by a voice: a start. An answer that has ended is heard on. */
    if (c->cut_ms >= 0.0f && (c->cut_ms += dt) >= 220.0f) {
        c->cut_ms = -1.0f;
        if (c->silent_ms >= SHUT_AFTER_MS && c->unheard_ms < 700.0f) chr_cue(c, CHR_CUE_STARTLE, 0);
    }
    float heard = 0.0f;
    if (c->phase == CHR_PHASE_LISTENING || (c->conditions & CHR_IN_CALL))
        heard = deaf ? 0.0f : maxf(sqrtf(c->env_mic), (float)c->level / 3.0f * 0.7f);
    else if (c->phase == CHR_PHASE_IDLE) heard = deaf ? 0.0f : (float)c->level / 3.0f * 0.5f;
    c->listen += (heard - c->listen) * clampf(dt / (heard > c->listen ? 45.0f : 220.0f), 0, 1);
}

/* A talk is over for its page before it is over for whoever hears it: its last words are still on the
 * way, or in the speaker (the device says the first, CHR_PARTING; the loudness the second). She speaks
 * them to the end, then waves goodbye (not at a network that is gone). A call, an alarm or the next
 * talk does not wait for that. */
static void parting_step(chr_t *c, float dt)
{
    if (!c->parting) return;
    c->parting_ms += dt;
    const bool called = (c->conditions & (CHR_ALARM | CHR_RINGING | CHR_IN_CALL | CHR_STARTING)) != 0;
    const bool heard = (c->conditions & CHR_PARTING) || c->silent_ms < PART_AFTER_MS || c->parting_ms < PART_LEAST_MS;
    if (heard && !called && c->parting_ms < PART_MOST_MS) return;
    c->parting = false;
    c->alone_ms = 0.0f;
    if (!called && !(c->conditions & CHR_OFFLINE)) chr_cue(c, CHR_CUE_BYE, 0);
}

static void finish(chr_t *c, float dt)
{
    const float now = (float)fmod(c->now, 3600000.0);
    const mood_t *m = &moods[c->mood];
    float *o = c->out;
    memcpy(o, c->p, sizeof(c->out));
    /* Breathing: slower when drowsy, and deeper for a head of its own. She comes to it, and from it, by
     * and by: eyes that narrow for a moment are no sleep. */
    const float drowsy = o[P_EOL] < 0.4f && o[P_EOR] < 0.4f ? 1.0f : 0.0f;
    c->drowsy += (drowsy - c->drowsy) * clampf(dt / 2000.0f, 0, 1);
    c->breath += dt * mixf(0.0015f, (c->features & CHR_FLAT) ? 0.0012f : 0.00105f, c->drowsy);
    if (c->breath > 2.0f * PI) c->breath -= 2.0f * PI;
    /* All of a figure in one piece breathes: up and down by 3.5 thousandths of her height (two pixels)
     * every 4.2 s. It is the one thing all of her keeps doing. */
    if (c->features & CHR_FLAT) o[P_HY] += sinf(c->breath) * 0.0035f * (float)-c->fig.hair_top / CHR_FLAT_GIVE;
    else o[P_HY] += sinf(c->breath) * mixf(1.5f, 2.4f, c->drowsy);
    /* Speech has the mouth while it lasts, whatever the mood makes of it. */
    if (c->mouth > 0.01f) o[P_MO] = c->mouth;
    /* Where a sentence begins the brows go up for a moment. */
    if (talk_phase(c) != CHR_PHASE_IDLE && c->sentence_ms < SENTENCE_BROW_MS)
        o[P_BROW] = maxf(o[P_BROW], ((c->features & CHR_FLAT) ? BROWS_UP : 3.0f) * window(c->sentence_ms / SENTENCE_BROW_MS, 0.15f, 0.2f));
    /* Listening: the ears turn to the voice she hears. */
    o[P_EAR_L] -= c->listen * 0.3f * (0.6f + 0.4f * sinf(now * 0.019f));
    o[P_EAR_R] -= c->listen * 0.3f * (0.6f - 0.4f * sinf(now * 0.019f));
    /* Blinks. A flat figure's eyes open no wider than her picture's. */
    const float lids = chr_lids(c);
    if (c->features & CHR_FLAT) { o[P_EOL] = minf(o[P_EOL], 1.0f); o[P_EOR] = minf(o[P_EOR], 1.0f); }
    o[P_EOL] *= 1.0f - lids; o[P_EOR] *= 1.0f - lids;
    if (c->features & CHR_FLAT) {
        /* Her cheeks colour over 0.4 s and pale over 0.8 s (a little blush in less), whatever else moves. */
        const float target = clampf(c->target[P_BLUSH], 0, 1);
        if (target > c->blush) c->blush_from = c->blush = minf(target, c->blush + maxf(target, 0.25f) / 400.0f * dt);
        else if (target < c->blush) c->blush = maxf(target, c->blush - maxf(c->blush_from, 0.25f) / 800.0f * dt);
        o[P_BLUSH] = c->blush;
    }
    if ((c->features & CHR_FLAT) && c->pose == CHR_GESTURE_DANCE && c->clipped) {
        /* Her dance: on every beat she gives a pixel and comes back; on the first two of its pictures (to one side and
         * to the other) she is three pixels to that side. */
        chr_frame_t shown, first, second;
        if (chr_pack_frame(&c->pack, (unsigned)c->clip.first + c->clip_at, &shown) && shown.ms) {
            o[P_BY] += 1.2f * sinf(PI * minf(1.0f, 2.0f * c->frame_ms / (float)shown.ms));
            if (chr_pack_frame(&c->pack, c->clip.first, &first) && chr_pack_frame(&c->pack, c->clip.first + 1U, &second)) {
                if (shown.sprite == first.sprite) o[P_BX] -= 3.0f;
                else if (shown.sprite == second.sprite) o[P_BX] += 3.0f;
            }
        }
    }
    /* Trembling. */
    float shake = 0.0f;
    if (c->conditions & CHR_ALARM) shake = 6.0f;
    else if (c->conditions & CHR_RINGING) shake = 3.0f * maxf(0, sinf(now * 0.0063f));
    else if (c->cue == CHR_CUE_ERROR) shake = 12.0f * (1.0f - c->cue_age / c->cue_len);
    else if (c->cue == CHR_CUE_SHIVER) shake = 6.0f * (1.0f - c->cue_age / c->cue_len);
    else if ((m->flags & M_SHAKE) && !c->mood_talk && talk_phase(c) == CHR_PHASE_IDLE && !(c->features & CHR_FLAT)) shake = 2.0f;
    if (c->features & CHR_FLAT) shake = minf(shake, 1.5f);
    if (shake > 0.0f) o[P_BX] += shake * sinf(now * (c->cue == CHR_CUE_ERROR ? 0.034f : 0.075f));
    /* The tail keeps its own time: faster and wider with her spirits. */
    c->wag_phase += dt * 0.001f * 2.0f * PI * (0.35f + 2.4f * clampf(o[P_WAG], 0, 1.5f));
    if (c->wag_phase > 2.0f * PI) c->wag_phase -= 2.0f * PI;
    o[P_BY] = minf(o[P_BY], 400.0f);
    for (int i = P_EOL; i <= P_EOR; ++i) o[i] = clampf(o[i], 0.0f, 1.5f);
    o[P_GX] = clampf(o[P_GX], -1, 1); o[P_GY] = clampf(o[P_GY], -1, 1);
    o[P_MO] = clampf(o[P_MO], 0, 1); o[P_MC] = clampf(o[P_MC], -1, 1);
    o[P_ARC] = clampf(o[P_ARC], -1, 1); o[P_SLANT] = clampf(o[P_SLANT], -1, 1);
    o[P_BLUSH] = clampf(o[P_BLUSH], 0, 1); o[P_GREY] = clampf(o[P_GREY], 0, 1);
    o[P_EAR_L] = clampf(o[P_EAR_L], -1.3f, 1.3f); o[P_EAR_R] = clampf(o[P_EAR_R], -1.3f, 1.3f);
    o[P_WAG] = clampf(o[P_WAG], 0, 1.5f); o[P_LEAN] = clampf(o[P_LEAN], -30, 30);
    o[P_HY] = clampf(o[P_HY], -30, 40); o[P_BROW] = clampf(o[P_BROW], -6, 12);
    o[P_ARM_L] = clampf(o[P_ARM_L], 0, 1); o[P_ARM_R] = clampf(o[P_ARM_R], 0, 1); o[P_YAW] = clampf(o[P_YAW], -1, 1);
}

static bool overlap(const chr_rect_t *a, const chr_rect_t *b)
{
    return a->x < b->x + b->width && b->x < a->x + a->width && a->y < b->y + b->height && b->y < a->y + a->height;
}

size_t chr_step(chr_t *c, uint32_t dt_ms, chr_rect_t damage[CHR_DAMAGE_MAX])
{
    if (!c || !c->pack.data) return 0;
    const float dt = (float)(dt_ms > 120U ? 120U : dt_ms);
    c->now += (double)dt_ms;    /* her clock is the real one: a slow step moves her no further than 120 ms */
    c->phase_age += dt;
    c->mood_age += dt;
    ++c->steps;
    if (c->cue) {
        c->cue_age += dt;
        if (c->cue_age >= c->cue_len) c->cue = CHR_CUE_NONE;
    }
    sound(c, dt);
    parting_step(c, dt);
    touch_step(c, dt);
    feeling_step(c, dt);
    idle_life(c, dt);
    look_step(c);
    emitters(c, dt);
    gesture_step(c, dt);
    compose(c, c->target);
    if (c->snap) {
        /* Behind shut lids eyes need not travel; nor need the gaze and the brows where they are pictures of
         * the face. On a layered figure they move by springs, the head with the gaze. */
        for (unsigned i = P_GX; i <= P_BROW; ++i) {
            if (!(c->features & CHR_FLAT) && (i < P_EOL || i > P_ARC)) continue;
            c->p[i] = c->target[i];
            c->v[i] = 0.0f;
        }
        c->snap = false;
    }
    for (float left = dt; left > 0.0f; left -= 8.0f) follow(c, (left < 8.0f ? left : 8.0f) * 0.001f);
    finish(c, dt);
    c->step_ms = dt;
    chr_effects_step(c, dt);
    memcpy(c->drawn, c->groups, sizeof(c->drawn));
    const bool first = !c->built;
    chr_build(c);
    c->built = true;
    size_t count = 0;
    for (int g = 0; g < G_COUNT && damage; ++g) {
        const chr_group_t *now = &c->groups[g], *was = &c->drawn[g];
        if (!first && now->signature == was->signature) continue;
        int x0 = now->x0, y0 = now->y0, x1 = now->x1, y1 = now->y1;
        if (!first && was->x1 >= was->x0) {
            if (x1 < x0) { x0 = was->x0; y0 = was->y0; x1 = was->x1; y1 = was->y1; }
            else {
                if (was->x0 < x0) x0 = was->x0;
                if (was->y0 < y0) y0 = was->y0;
                if (was->x1 > x1) x1 = was->x1;
                if (was->y1 > y1) y1 = was->y1;
            }
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= (int)c->width) x1 = (int)c->width - 1;
        if (y1 >= (int)c->height) y1 = (int)c->height - 1;
        if (x1 < x0 || y1 < y0) continue;
        chr_rect_t r = {(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0 + 1), (int16_t)(y1 - y0 + 1)};
        /* Parts that overlap are redrawn as one rectangle. */
        for (size_t i = 0; i < count;) {
            if (!overlap(&damage[i], &r)) { ++i; continue; }
            const int ax = damage[i].x < r.x ? damage[i].x : r.x, ay = damage[i].y < r.y ? damage[i].y : r.y;
            const int bx = damage[i].x + damage[i].width > r.x + r.width ? damage[i].x + damage[i].width : r.x + r.width;
            const int by = damage[i].y + damage[i].height > r.y + r.height ? damage[i].y + damage[i].height : r.y + r.height;
            r = (chr_rect_t){(int16_t)ax, (int16_t)ay, (int16_t)(bx - ax), (int16_t)(by - ay)};
            damage[i] = damage[--count];
            i = 0;
        }
        damage[count++] = r;
    }
    if (!count) ++c->quiet_steps;
    return count;
}

void chr_inspect(const chr_t *c, chr_info_t *out)
{
    if (!c || !out) return;
    memset(out, 0, sizeof(*out));
    memcpy(out->pose, c->out, sizeof(out->pose));
    memcpy(out->target, c->target, sizeof(out->target));
    out->mood = c->mood; out->phase = c->phase; out->cue = c->cue; out->touch = c->touch;
    out->eye_left = c->eye[0]; out->eye_right = c->eye[1]; out->gaze = c->gaze;
    out->mouth_form = c->mouth_form; out->mouth_open = c->mouth_open;
    out->ear_left = c->ear[0]; out->ear_right = c->ear[1];
    out->arm_left = c->arm[0]; out->arm_right = c->arm[1]; out->turn = c->turn; out->features = (uint8_t)c->features;
    out->gesture = c->gesture; out->shown = c->pose;
    out->frame = (uint8_t)c->clip_at; out->changing = c->change_ms >= 0.0f;
    out->given = c->given; out->dozing = c->dozing; out->energy = c->energy; out->closeness = c->closeness;
    out->conditions = c->conditions;
    out->items = c->item_count;
    for (int i = 0; i < CHR_PARTICLES_MAX; ++i)
        out->particles = (uint16_t)(out->particles + (c->particles[i].live ? 1U : 0U));
    out->mouth = c->mouth; out->listen = c->listen;
    out->parting = c->parting;
    out->silent = c->silent_ms; out->shut_ms = (uint16_t)minf(c->shut_ms, 65535.0f);
    out->wait_ms = (uint16_t)minf(c->wait_max_ms, 65535.0f); out->lag_ms = (uint16_t)minf(c->lag_max_ms, 65535.0f);
    out->moved_x = c->moved_x; out->moved_feet = c->moved_feet; out->moved_top = c->moved_top;
    out->steps = c->steps; out->quiet_steps = c->quiet_steps;
}
