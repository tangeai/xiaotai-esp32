// SPDX-License-Identifier: MIT
/* The life of the face: what it does on its own, how it answers a finger and
 * follows sound, and that its damage covers what it draws.
 *
 *   theme_lcd_face_life                  the checks
 *   theme_lcd_face_life film <scene> <file> <width> <height> <ms>
 *                                        frames of a scene in that many pixels, one every <ms>,
 *                                        RGB565, one after another (tools/face_preview.py) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lcd_face.h"
#include "xiaotai/theme_lcd.h"
#include "xiaotai/view.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)

enum { W = LCD_FACE_WIDTH, H = LCD_FACE_HEIGHT, MOST = 1024 * 600 };
static uint16_t canvas[MOST], previous[MOST];
static int width = W, height = H;
static bool primed;      /* the canvas holds a frame of this face */
static xiaotai_ui_surface_t frame(void)
{
    return (xiaotai_ui_surface_t){.pixels = (uint8_t *)canvas, .width = (uint16_t)width, .height = (uint16_t)height,
        .stride = (uint32_t)width * sizeof(*canvas), .pixel = XIAOTAI_PIXEL_RGB565};
}

static lcd_face_t *create(uint64_t at)
{
    lcd_face_t *face = NULL;
    CHECK(lcd_face_create(0x101418, &face) == XIAOTAI_OK && face);
    CHECK(lcd_face_size(face, (uint16_t)width, (uint16_t)height) == XIAOTAI_OK);
    lcd_face_attach(face, at);
    primed = false;
    return face;
}

static bool covered(const lcd_face_rect_t *rects, size_t count, int x, int y)
{
    for (size_t i = 0; i < count; ++i)
        if (x >= rects[i].x && x < rects[i].x + rects[i].width && y >= rects[i].y && y < rects[i].y + rects[i].height) return true;
    return false;
}

static void step(lcd_face_t *face, uint64_t at)
{
    lcd_face_rect_t rects[LCD_FACE_REGIONS];
    const size_t bytes = (size_t)width * (size_t)height * sizeof(*canvas);
    const xiaotai_ui_surface_t whole = frame();
    memcpy(previous, canvas, bytes);
    const size_t count = lcd_face_step(face, at, true, rects);
    CHECK(count <= LCD_FACE_REGIONS && lcd_face_canvas(face, &whole));   /* none while nothing moves */
    for (size_t i = 0; i < count; ++i)
        CHECK(rects[i].x >= 0 && rects[i].y >= 0 && rects[i].x + rects[i].width <= width && rects[i].y + rects[i].height <= height);
    for (int y = 0; primed && y < height; ++y) for (int x = 0; x < width; ++x)
        if (canvas[y * width + x] != previous[y * width + x]) CHECK(covered(rects, count, x, y));
    primed = true;
}

/* Bright pixels of a part of the canvas: how much of an eye or a mouth shows. */
static unsigned lit(int x0, int y0, int x1, int y1)
{
    unsigned n = 0;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) n += (canvas[y * W + x] >> 11) > 18U;
    return n;
}

static unsigned eyes(void) { return lit(0, 24, 110, 88) + lit(130, 24, W, 88); }
/* How much of the canvas the mouth takes, its inside too. */
static unsigned mouth(void)
{
    unsigned n = 0;
    for (int y = 84; y < H; ++y) for (int x = 94; x < 146; ++x) n += canvas[y * W + x] != canvas[0];
    return n;
}

static void alive(void)
{
    lcd_face_t *face = create(1000);
    unsigned least = ~0U, most = 0, moved = 0, still = 0, stillest = 0;
    for (uint64_t t = 1000; t <= 61000; t += 40) {
        step(face, t);
        if (t < 2000) continue;
        const unsigned n = eyes();
        if (n < least) least = n;
        if (n > most) most = n;
        const bool moves = memcmp(canvas, previous, sizeof(canvas)) != 0;
        moved += moves;
        still = moves ? 0 : still + 1;
        if (still > stillest) stillest = still;
    }
    /* It blinks, and it is never still for long; but it is drawn only when there is something to see. */
    CHECK(least < most / 3 && moved > 400 && stillest < 25);
    /* Every expression, every phase, from any other: damage holds (checked in step). */
    for (size_t i = 0; i < XIAOTAI_THEME_LCD_EMOTION_COUNT; ++i) {
        lcd_face_set(face, xiaotai_theme_lcd_emotion_name(i), (uint8_t)(i % 4U), (uint8_t)(i % 4U), 61000 + i * 1200);
        for (unsigned k = 0; k < 30; ++k) step(face, 61040 + i * 1200 + k * 40);
    }
    static const char *const states[] = {"ringing", "dialing", "offline", "error"};
    for (size_t i = 0; i < 4; ++i) {
        CHECK(lcd_face_manner(states[i]) == LCD_FACE_RINGING + i && lcd_face_resolve(states[i]) != 0);
        lcd_face_set(face, states[i], 0, 0, 100000 + i * 1200);
        for (unsigned k = 0; k < 30; ++k) step(face, 100040 + i * 1200 + k * 40);
    }
    lcd_face_destroy(face);
}

static void speaks(void)
{
    lcd_face_t *face = create(1000);
    lcd_face_set(face, "neutral", XIAOTAI_TALK_SPEAKING, 0, 1000);
    uint64_t t = 1000;
    for (; t < 2500; t += 40) { lcd_face_sense(face, &(lcd_face_senses_t){.hears = true}); step(face, t); }
    const unsigned closed = mouth();
    unsigned widest = 0;
    for (; t < 3500; t += 40) {
        lcd_face_sense(face, &(lcd_face_senses_t){.hears = true, .playout = (uint16_t)(t / 40 % 6 < 3 ? 9000 : 1200)});
        step(face, t);
        if (mouth() > widest) widest = mouth();
    }
    CHECK(widest > closed * 3 / 2);
    /* The mouth stops with the sound, not with the phase. */
    for (unsigned k = 0; k < 14; ++k, t += 40) { lcd_face_sense(face, &(lcd_face_senses_t){.hears = true}); step(face, t); }
    CHECK(mouth() < closed * 3 / 2);
    lcd_face_destroy(face);
}

/* Light in the left iris: blue beyond red. */
static unsigned iris(void)
{
    unsigned n = 0;
    for (int y = 42; y < 72; ++y) for (int x = 44; x < 76; ++x) {
        const unsigned p = canvas[y * W + x], red = p >> 11, green = p >> 5 & 63U;
        if (green / 2U > red + 2U) n += green;
    }
    return n;
}

/* That it listens shows in the face, and nowhere beside it. */
static void listens(void)
{
    lcd_face_t *face = create(1000);
    uint64_t t = 1000;
    for (; t < 2200; t += 20) { lcd_face_sense(face, &(lcd_face_senses_t){.hears = true}); step(face, t); }
    const unsigned idle = iris();
    lcd_face_set(face, "neutral", XIAOTAI_TALK_LISTENING, 0, t);
    unsigned quiet = 0, loud = 0;
    for (unsigned k = 0; k < 110; ++k, t += 20) {
        lcd_face_sense(face, &(lcd_face_senses_t){.hears = true});
        step(face, t);
        if (k > 60 && iris() > quiet) quiet = iris();
    }
    unsigned brow = 0;
    for (int y = 6; y < 26; ++y) for (int x = 40; x < 80; ++x) brow += canvas[y * W + x] != canvas[0];
    for (unsigned k = 0; k < 60; ++k, t += 20) {
        lcd_face_sense(face, &(lcd_face_senses_t){.hears = true, .capture = (uint16_t)(k % 12 < 8 ? 6000 : 900)});
        step(face, t);
        if (iris() > loud) loud = iris();
        for (int y = 0; y < H; ++y) for (int x = 0; x < 12; ++x) CHECK(canvas[y * W + x] == canvas[0] && canvas[y * W + W - 1 - x] == canvas[0]);
    }
    CHECK(quiet > idle * 5U / 4U && loud > quiet * 21U / 20U && brow > 60U);
    /* The speaker pauses: it nods. */
    unsigned top = ~0U, low = 0;
    for (unsigned k = 0; k < 70; ++k, t += 20) {
        lcd_face_sense(face, &(lcd_face_senses_t){.hears = true});
        step(face, t);
        unsigned y = 84;
        while (y < (unsigned)H && canvas[y * W + W / 2] == canvas[0]) ++y;
        if (y < top) top = y;
        if (y > low) low = y;
    }
    CHECK(low >= top + 3U);
    lcd_face_destroy(face);
}

static void press(lcd_face_t *face, uint64_t *t, int x, int y, unsigned frames)
{
    for (unsigned i = 0; i < frames; ++i, *t += 40) {
        lcd_face_sense(face, &(lcd_face_senses_t){.down = true, .x = (int16_t)x, .y = (int16_t)y});
        step(face, *t);
    }
}

static void release(lcd_face_t *face, uint64_t *t, unsigned frames)
{
    for (unsigned i = 0; i < frames; ++i, *t += 40) {
        lcd_face_sense(face, &(lcd_face_senses_t){0});
        step(face, *t);
    }
}

static void answers(void)
{
    /* A poke in the eye closes that eye, and only that one. */
    lcd_face_t *face = create(1000);
    uint64_t t = 1000;
    release(face, &t, 12);
    press(face, &t, 60, 58, 3);
    CHECK(!lcd_face_stroked(face));
    release(face, &t, 5);
    CHECK(!lcd_face_stroked(face));
    const unsigned left = lit(0, 24, 110, 88), right = lit(130, 24, W, 88);
    CHECK(left * 2 < right);
    release(face, &t, 60);
    CHECK(lit(0, 24, 110, 88) * 3 > lit(130, 24, W, 88));
    /* The eyes follow a finger. */
    press(face, &t, 220, 38, 8);
    unsigned towards = 0;
    for (int x = 0; x < W; ++x) if (lit(x, 24, x + 1, 88)) { towards = (unsigned)x; break; }
    release(face, &t, 80);
    press(face, &t, 20, 98, 8);
    unsigned away = 0;
    for (int x = 0; x < W; ++x) if (lit(x, 24, x + 1, 88)) { away = (unsigned)x; break; }
    CHECK(towards > away + 10);
    release(face, &t, 80);
    /* A finger that rests or rubs strokes it: lifting it is no tap. */
    press(face, &t, 120, 68, 20);
    CHECK(lcd_face_stroked(face));
    lcd_face_sense(face, &(lcd_face_senses_t){0});
    CHECK(lcd_face_stroked(face));
    release(face, &t, 8);
    CHECK(!lcd_face_stroked(face));
    lcd_face_destroy(face);
}

/* ------------------------------------------------------------------- scenes */

typedef struct {
    unsigned at_ms;
    const char *mood;           /* NULL: no change */
    uint8_t phase;
    int touch_x, touch_y;       /* a touch while x >= 0 until the next cue */
    float speech, hearing;
} cue_t;

static const cue_t idle_scene[] = {{0, "neutral", 0, -1, 0, 0, 0}, {30000, NULL, 0, -1, 0, 0, 0}};
static const cue_t talk_scene[] = {
    {0, "neutral", XIAOTAI_TALK_IDLE, -1, 0, 0, 0}, {1200, "neutral", XIAOTAI_TALK_LISTENING, -1, 0, 0, 0},
    {1600, NULL, XIAOTAI_TALK_LISTENING, -1, 0, 0, 1}, {4600, NULL, XIAOTAI_TALK_LISTENING, -1, 0, 0, 0},
    {5200, "neutral", XIAOTAI_TALK_THINKING, -1, 0, 0, 0}, {8200, "happy", XIAOTAI_TALK_SPEAKING, -1, 0, 0, 0},
    {8500, NULL, XIAOTAI_TALK_SPEAKING, -1, 0, 1, 0}, {12500, NULL, XIAOTAI_TALK_SPEAKING, -1, 0, 0, 0},
    {13200, "surprised", XIAOTAI_TALK_SPEAKING, -1, 0, 1, 0}, {15600, "loving", XIAOTAI_TALK_SPEAKING, -1, 0, 1, 0},
    {18000, "neutral", XIAOTAI_TALK_IDLE, -1, 0, 0, 0}, {19000, NULL, 0, -1, 0, 0, 0},
};
static const cue_t touch_scene[] = {
    {0, "neutral", 0, -1, 0, 0, 0}, {1000, NULL, 0, 20, 20, 0, 0}, {1500, NULL, 0, 200, 20, 0, 0},
    {2000, NULL, 0, 200, 95, 0, 0}, {2500, NULL, 0, 20, 95, 0, 0}, {3000, NULL, 0, -1, 0, 0, 0},
    {4600, NULL, 0, 50, 50, 0, 0}, {4720, NULL, 0, -1, 0, 0, 0},           /* a poke in the eye */
    {7800, NULL, 0, 110, 92, 0, 0}, {7920, NULL, 0, -1, 0, 0, 0},          /* on the mouth */
    {8500, NULL, 0, 110, 40, 0, 0}, {8620, NULL, 0, -1, 0, 0, 0},
    {9300, NULL, 0, 110, 40, 0, 0}, {9420, NULL, 0, -1, 0, 0, 0},
    {10100, NULL, 0, 110, 40, 0, 0}, {10220, NULL, 0, -1, 0, 0, 0},        /* four in a row: cross */
    {10900, NULL, 0, 110, 40, 0, 0}, {11020, NULL, 0, -1, 0, 0, 0},
    {11700, NULL, 0, 110, 40, 0, 0}, {11820, NULL, 0, -1, 0, 0, 0},        /* six: dizzy */
    {15500, NULL, 0, 100, 60, 0, 0}, {15900, NULL, 0, 120, 64, 0, 0}, {16300, NULL, 0, 100, 60, 0, 0},
    {16700, NULL, 0, 120, 64, 0, 0}, {17900, NULL, 0, -1, 0, 0, 0}, {20000, NULL, 0, -1, 0, 0, 0},
};
static const cue_t state_scene[] = {
    {0, "ringing", 0, -1, 0, 0, 0}, {3000, "dialing", 0, -1, 0, 0, 0}, {6000, "offline", 0, -1, 0, 0, 0},
    {9500, "error", 0, -1, 0, 0, 0}, {12500, "sleepy", 0, -1, 0, 0, 0}, {20500, "neutral", 0, -1, 0, 0, 0},
    {22500, NULL, 0, -1, 0, 0, 0},
};
static const cue_t mood_scene[] = {
    {0, "neutral", 0, -1, 0, 0, 0}, {1500, "happy", 0, -1, 0, 0, 0}, {3500, "laughing", 0, -1, 0, 0, 0},
    {5500, "surprised", 0, -1, 0, 0, 0}, {7300, "shocked", 0, -1, 0, 0, 0}, {9100, "sad", 0, -1, 0, 0, 0},
    {11100, "crying", 0, -1, 0, 0, 0}, {13100, "angry", 0, -1, 0, 0, 0}, {15100, "loving", 0, -1, 0, 0, 0},
    {17100, "embarrassed", 0, -1, 0, 0, 0}, {19100, "confused", 0, -1, 0, 0, 0}, {21100, "cool", 0, -1, 0, 0, 0},
    {23100, "winking", 0, -1, 0, 0, 0}, {25100, "silly", 0, -1, 0, 0, 0}, {27100, "confident", 0, -1, 0, 0, 0},
    {29100, "relaxed", 0, -1, 0, 0, 0}, {31100, "delicious", 0, -1, 0, 0, 0}, {33100, NULL, 0, -1, 0, 0, 0},
};
static const struct { const char *name; const cue_t *cues; size_t count; } scenes[] = {
    {"idle", idle_scene, sizeof(idle_scene) / sizeof(*idle_scene)},
    {"talk", talk_scene, sizeof(talk_scene) / sizeof(*talk_scene)},
    {"touch", touch_scene, sizeof(touch_scene) / sizeof(*touch_scene)},
    {"states", state_scene, sizeof(state_scene) / sizeof(*state_scene)},
    {"moods", mood_scene, sizeof(mood_scene) / sizeof(*mood_scene)},
};

/* Something like the envelope of speech. */
static uint16_t voice(uint64_t t)
{
    const float s = (float)t / 1000.0f;
    const float syllable = fabsf(sinf(s * 11.0f)) * (0.55f + 0.45f * sinf(s * 3.1f));
    const float pause = fmodf(s, 2.3f) < 1.9f ? 1.0f : 0.0f;
    return (uint16_t)(9000.0f * syllable * pause);
}

static int film(const char *name, const char *path, unsigned every_ms)
{
    for (size_t i = 0; i < sizeof(scenes) / sizeof(*scenes); ++i) {
        if (strcmp(scenes[i].name, name)) continue;
        FILE *out = fopen(path, "wb");
        CHECK(out);
        lcd_face_t *face = create(4242);
        lcd_face_pace(face, (uint16_t)every_ms);
        size_t cue = 0;
        cue_t now = {.touch_x = -1};
        const unsigned end = scenes[i].cues[scenes[i].count - 1].at_ms;
        for (unsigned t = 0; t <= end; t += every_ms) {
            while (cue < scenes[i].count && scenes[i].cues[cue].at_ms <= t) {
                now = scenes[i].cues[cue++];
                if (now.mood) lcd_face_set(face, now.mood, now.phase, 0, 4242 + t);
            }
            const lcd_face_senses_t senses = {.down = now.touch_x >= 0, .x = (int16_t)(now.touch_x + 10), .y = (int16_t)(now.touch_y + 8),
                .hears = true, .playout = now.speech > 0 ? voice(t) : 0, .capture = now.hearing > 0 ? voice(t) / 3U : 0};
            lcd_face_sense(face, &senses);
            step(face, 4242 + t);
            CHECK(fwrite(canvas, (size_t)width * (size_t)height * sizeof(*canvas), 1, out) == 1);
        }
        lcd_face_destroy(face);
        return fclose(out) ? 1 : 0;
    }
    return 2;
}

/* One picture of every expression, some time after it came. */
static int stills(const char *path, unsigned after_ms)
{
    FILE *out = fopen(path, "wb");
    CHECK(out);
    for (size_t i = 0; i < LCD_FACE_MANNERS; ++i) {
        static const char *const states[] = {"ringing", "dialing", "offline", "error"};
        lcd_face_t *face = create(777);
        lcd_face_set(face, i < XIAOTAI_THEME_LCD_EMOTION_COUNT ? xiaotai_theme_lcd_emotion_name(i) : states[i - XIAOTAI_THEME_LCD_EMOTION_COUNT], 0, 0, 900);
        lcd_face_pace(face, 20);
        for (unsigned t = 900; t <= 900 + after_ms; t += 20) step(face, t);
        CHECK(fwrite(canvas, (size_t)width * (size_t)height * sizeof(*canvas), 1, out) == 1);
        lcd_face_destroy(face);
    }
    return fclose(out) ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 7) {
        width = atoi(argv[4]);
        height = atoi(argv[5]);
        CHECK(width > 0 && height > 0 && width * height <= MOST);
        if (!strcmp(argv[1], "film")) return film(argv[2], argv[3], (unsigned)atoi(argv[6]));
        if (!strcmp(argv[1], "stills")) return stills(argv[3], (unsigned)atoi(argv[6]));
        return 2;
    }
    alive();
    speaks();
    listens();
    answers();
    return 0;
}
