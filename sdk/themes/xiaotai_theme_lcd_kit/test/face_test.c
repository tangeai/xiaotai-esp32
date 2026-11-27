// SPDX-License-Identifier: MIT
// Replaces legacy: tests/lvgl/test_draw_hook.c::draw_hook
/* The face as a picture: names, every size, damage, strips, edges and time.
 * What it does on its own is in face_life_test.c. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lcd_face.h"
#include "lcd_face_look.h"
#include "xiaotai/theme_lcd.h"
#include "xiaotai_mem_host.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)

enum { MOST_W = 600, MOST_H = 310, PAD = 8, ROW = MOST_W + 2 * PAD };
static const uint32_t PAPER = 0x101418;
static uint16_t canvas[MOST_W * MOST_H], previous[MOST_W * MOST_H];
static uint16_t direct[ROW * (MOST_H + 2 * PAD)];
static int W = LCD_FACE_WIDTH, H = LCD_FACE_HEIGHT;

static xiaotai_ui_surface_t frame(void)
{
    return (xiaotai_ui_surface_t){.pixels = (uint8_t *)canvas, .width = (uint16_t)W, .height = (uint16_t)H,
        .stride = (uint32_t)W * sizeof(*canvas), .pixel = XIAOTAI_PIXEL_RGB565};
}

static void draw(lcd_face_t *face)
{
    const xiaotai_ui_surface_t whole = frame();
    CHECK(lcd_face_canvas(face, &whole));
}

static lcd_face_t *create(uint32_t background, uint64_t at)
{
    lcd_face_t *face = NULL;
    CHECK(lcd_face_create(background, &face) == XIAOTAI_OK);
    CHECK(lcd_face_size(face, (uint16_t)W, (uint16_t)H) == XIAOTAI_OK);
    lcd_face_attach(face, at);
    return face;
}

static size_t step(lcd_face_t *face, uint64_t at, bool show, lcd_face_rect_t rs[LCD_FACE_REGIONS])
{
    const size_t count = lcd_face_step(face, at, show, rs);
    CHECK(count <= LCD_FACE_REGIONS);
    for (size_t i = 0; i < count; ++i) {
        CHECK(rs[i].x >= 0 && rs[i].y >= 0 && rs[i].width > 0 && rs[i].height > 0);
        CHECK(rs[i].x + rs[i].width <= W && rs[i].y + rs[i].height <= H);
    }
    return count;
}

static unsigned counter(lcd_face_t *face, const char *key)
{
    char line[224];
    lcd_face_report(face, line, sizeof(line));
    const char *value = strstr(line, key);
    CHECK(value);
    return (unsigned)strtoul(value + strlen(key), NULL, 10);
}

static void catalog(void)
{
    static const char *const names[] = {"neutral", "happy", "laughing", "funny", "sad", "angry", "crying", "loving",
        "embarrassed", "surprised", "shocked", "thinking", "winking", "cool", "relaxed", "delicious", "kissy",
        "confident", "sleepy", "silly", "confused", "listening", "ambient"};
    static const struct { const char *alias, *of; } aliases[] = {{"calm", "neutral"}, {"excited", "laughing"}, {"curious", "confused"},
        {"proud", "confident"}, {"moved", "loving"}, {"speech", "listening"}, {"gentle", "relaxed"}, {"comfort", "loving"},
        {"serious", "confident"}};
    CHECK(sizeof(names) / sizeof(*names) == XIAOTAI_THEME_LCD_EMOTION_COUNT);
    for (size_t i = 0; i < XIAOTAI_THEME_LCD_EMOTION_COUNT; ++i) {
        CHECK(xiaotai_theme_lcd_emotion_name(i) && lcd_face_resolve(names[i]) == i && lcd_face_manner(names[i]) == i);
        CHECK(!strcmp(lcd_face_looks[i].name, names[i]));
    }
    CHECK(!xiaotai_theme_lcd_emotion_name(XIAOTAI_THEME_LCD_EMOTION_COUNT));
    CHECK(!lcd_face_resolve(NULL) && !lcd_face_resolve("not-a-face"));
    for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); ++i) CHECK(lcd_face_resolve(aliases[i].alias) == lcd_face_resolve(aliases[i].of));
    static const char *const states[] = {"ringing", "dialing", "offline", "error"};
    for (size_t i = 0; i < 4; ++i) CHECK(lcd_face_manner(states[i]) == LCD_FACE_RINGING + i && !strcmp(lcd_face_looks[LCD_FACE_RINGING + i].name, states[i]));
}

static bool covered(const lcd_face_rect_t *rs, size_t count, int x, int y)
{
    for (size_t i = 0; i < count; ++i)
        if (x >= rs[i].x && x < rs[i].x + rs[i].width && y >= rs[i].y && y < rs[i].y + rs[i].height) return true;
    return false;
}

/* Damage holds through every change, and the frame is the same whole, in strips, clipped and swapped. */
static void damage_and_clips(int width, int height)
{
    W = width;
    H = height;
    lcd_face_t *face = create(PAPER, 1000);
    lcd_face_rect_t rs[LCD_FACE_REGIONS];
    CHECK(step(face, 1000, true, rs) == 1 && rs[0].width == W && rs[0].height == H);     /* the first frame is all of it */
    draw(face);
    memcpy(previous, canvas, sizeof(previous));
    unsigned changed = 0, still = 0, whole = 0;
    size_t draws = 1;
    for (uint64_t t = 1020; t <= 5000; t += 20) {
        if (t == 1040) lcd_face_set(face, "surprised", XIAOTAI_TALK_SPEAKING, 3, t);
        if (t == 1600) lcd_face_set(face, "crying", XIAOTAI_TALK_IDLE, 1, t);
        if (t == 2200) lcd_face_set(face, "laughing", XIAOTAI_TALK_LISTENING, 3, t);
        if (t == 2800) lcd_face_set(face, "cool", XIAOTAI_TALK_IDLE, 0, t);
        if (t == 3400) lcd_face_set(face, "loving", XIAOTAI_TALK_IDLE, 0, t);
        if (t == 4000) lcd_face_set(face, "neutral", XIAOTAI_TALK_IDLE, 0, t);
        const size_t count = step(face, t, true, rs);
        draw(face);
        ++draws;
        still += !count;
        for (size_t i = 0; i < count; ++i) whole += rs[i].width == W && rs[i].height == H;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const size_t i = (size_t)y * (size_t)W + (size_t)x;
            if (canvas[i] == previous[i]) continue;
            CHECK(covered(rs, count, x, y));
            ++changed;
        }
        memcpy(previous, canvas, sizeof(previous));
    }
    CHECK(changed > 1000 && whole == 0);
    (void)still;

    const uint16_t guard = UINT16_C(0xaa55);
    for (size_t i = 0; i < sizeof(direct) / sizeof(*direct); ++i) direct[i] = guard;
    unsigned slices = 0;
    for (int y = 0; y < H; y += 9, ++slices) {
        const xiaotai_ui_surface_t slice = {.pixels = (uint8_t *)(direct + (y + PAD) * ROW + 3),
            .x = 3, .y = (uint16_t)(y + PAD), .width = ROW - 3, .height = (uint16_t)(H - y < 9 ? H - y : 9),
            .stride = ROW * sizeof(*direct), .pixel = XIAOTAI_PIXEL_RGB565, .swap_bytes = true};
        CHECK(lcd_face_paint(face, &slice, PAD, PAD));
    }
    for (int y = 0; y < H + 2 * PAD; ++y) for (int x = 0; x < ROW; ++x) {
        uint16_t expected = guard;
        if (x >= PAD && x < PAD + W && y >= PAD && y < PAD + H) {
            const uint16_t p = canvas[(y - PAD) * W + x - PAD];
            expected = (uint16_t)(p >> 8 | p << 8);
        }
        CHECK(direct[(size_t)y * ROW + (size_t)x] == expected);
    }
    CHECK(counter(face, " direct=") == slices && counter(face, "direct_pixels=") == (unsigned)(W * H));
    CHECK(counter(face, "fallback=") == draws && counter(face, "face_late=") == 0);

    const xiaotai_ui_surface_t clipped = {.pixels = (uint8_t *)direct, .width = ROW, .height = (uint16_t)(H + 2 * PAD),
        .stride = ROW * sizeof(*direct), .pixel = XIAOTAI_PIXEL_RGB565};
    for (size_t i = 0; i < sizeof(direct) / sizeof(*direct); ++i) direct[i] = guard;
    CHECK(lcd_face_paint(face, &clipped, -13, -9));
    for (int y = 0; y < H + 2 * PAD; ++y) for (int x = 0; x < ROW; ++x) {
        const uint16_t expected = x < W - 13 && y < H - 9 ? canvas[(size_t)(y + 9) * (size_t)W + (size_t)x + 13] : guard;
        CHECK(direct[(size_t)y * ROW + (size_t)x] == expected);
    }
    xiaotai_ui_surface_t invalid = {.pixels = (uint8_t *)direct, .width = (uint16_t)W, .height = (uint16_t)H,
        .stride = ROW * sizeof(*direct), .pixel = XIAOTAI_PIXEL_MONO1};
    CHECK(!lcd_face_paint(face, &invalid, PAD, PAD));
    invalid.pixel = XIAOTAI_PIXEL_RGB565;
    CHECK(!lcd_face_paint(face, &invalid, 1000, 1000));
    invalid.stride = 2;
    CHECK(!lcd_face_paint(face, &invalid, PAD, PAD));
    lcd_face_destroy(face);
}

static unsigned green(int x, int y) { return canvas[(size_t)y * (size_t)W + (size_t)x] >> 5 & 63U; }

/* However many pixels it gets, an edge is one pixel wide, softened and not blurred; the white has no bands. */
static void edges(int width, int height)
{
    W = width;
    H = height;
    lcd_face_t *face = create(PAPER, 1000);
    lcd_face_rect_t rs[LCD_FACE_REGIONS];
    CHECK(step(face, 1000, true, rs));
    draw(face);
    const unsigned paper = green(0, 0), white = 52;
    unsigned rows = 0, soft = 0;
    for (int y = H * 2 / 5; y < H * 11 / 20; ++y) {
        int x = 0, between = 0;
        while (x < W / 2 && green(x, y) == paper) ++x;
        while (x < W / 2 && green(x, y) < white) ++x, ++between;
        CHECK(x < W / 2 && between <= 2);               /* from paper to white over one pixel or two */
        ++rows;
    }
    /* Round its corner the edge is softened, in many shades. */
    uint64_t shades = 0;
    for (int y = H * 29 / 100; y < H * 38 / 100; ++y)
        for (int x = 0; x < W / 2; ++x) if (green(x, y) > paper && green(x, y) < white) { shades |= UINT64_C(1) << green(x, y); ++soft; }
    unsigned kinds = 0;
    for (unsigned k = 0; k < 64U; ++k) kinds += (unsigned)(shades >> k & 1U);
    CHECK(rows > 8 && soft > 8 && kinds > 6);
    /* Down the white beside the iris: no step of more than one shade between the averages of neighbouring rows. */
    unsigned last = 0;
    const int left = W / 4 - (int)(W * 24 / LCD_FACE_WIDTH);
    for (int y = H * 2 / 5; y < H * 11 / 20; ++y) {
        unsigned sum = 0;
        for (int x = left; x < left + 8; ++x) sum += green(x, y);
        if (last) CHECK(sum + 10U > last && last + 10U > sum);
        last = sum;
    }
    lcd_face_destroy(face);
}

static void independent_backdrops(void)
{
    W = LCD_FACE_WIDTH;
    H = LCD_FACE_HEIGHT;
    const size_t bytes = (size_t)W * (size_t)H * sizeof(*canvas);
    lcd_face_rect_t rs[LCD_FACE_REGIONS];
    lcd_face_t *a = create(0x141719, 1000), *b = create(0x1c1f22, 1000);
    CHECK(step(a, 1000, true, rs) && step(b, 1000, true, rs));
    draw(a);
    memcpy(previous, canvas, bytes);
    draw(b);
    CHECK(memcmp(canvas, previous, bytes));
    lcd_face_set(a, "sad", XIAOTAI_TALK_THINKING, 3, 1010);
    draw(a);
    CHECK(!memcmp(canvas, previous, bytes));                /* nothing before its next frame */
    lcd_face_destroy(a);
    a = create(0x141719, 1000);
    CHECK(step(a, 1000, true, rs));
    lcd_face_set_background(a, 0x1c1f22);
    CHECK(step(a, 1020, true, rs) == 1 && rs[0].width == W && rs[0].height == H);
    (void)step(b, 1020, true, rs);
    draw(a);
    memcpy(previous, canvas, bytes);
    draw(b);
    CHECK(!memcmp(canvas, previous, bytes));
    lcd_face_destroy(a);
    lcd_face_destroy(b);
}

static void timeline(void)
{
    const size_t bytes = (size_t)W * (size_t)H * sizeof(*canvas);
    lcd_face_rect_t rs[LCD_FACE_REGIONS];
    lcd_face_t *face = create(PAPER, 1000);
    lcd_face_pace(face, 20);
    CHECK(step(face, 1000, true, rs));
    CHECK(!step(face, 1018, true, rs) && lcd_face_delay(face, 1018) == 2);
    lcd_face_set(face, "listening", XIAOTAI_TALK_LISTENING, 3, 1020);
    (void)step(face, 1020, true, rs);
    CHECK(step(face, 1040, true, rs));
    draw(face);
    memcpy(previous, canvas, bytes);
    for (uint64_t t = 1060; t <= 14000; t += 20) CHECK(!step(face, t, false, rs));
    draw(face);
    CHECK(lcd_face_delay(face, 14000) == 0 && !memcmp(canvas, previous, bytes));     /* paused */
    CHECK(step(face, 14000, true, rs));
    CHECK(!step(face, 14018, true, rs) && counter(face, "face_late=") == 0);
    lcd_face_detach(face);
    CHECK(!step(face, 15999, false, rs));
    lcd_face_attach(face, 16000);
    CHECK(step(face, 16000, true, rs) == 1);
    for (uint64_t t = 16020; t <= 16720; t += 20) (void)step(face, t, true, rs);
    CHECK(counter(face, "face_late=") == 0);
    (void)step(face, 16761, true, rs);
    CHECK(counter(face, "face_late=") == 1);
    /* Slower when asked. */
    lcd_face_pace(face, 50);
    CHECK(!step(face, 16800, true, rs) && lcd_face_delay(face, 16800) == 11);
    lcd_face_destroy(face);

    /* Past the 32 bits of a millisecond clock. */
    const uint64_t at = UINT32_MAX - UINT64_C(20);
    face = create(PAPER, at);
    CHECK(step(face, at, true, rs));
    lcd_face_set(face, "crying", XIAOTAI_TALK_THINKING, 2, at + 20);
    unsigned moved = 0;
    for (uint64_t t = at + 20; t <= at + 800; t += 20) moved += step(face, t, true, rs) > 0;
    CHECK(moved > 25 && counter(face, "face_late=") == 0);
    lcd_face_destroy(face);
}

static void memory(void)
{
    for (unsigned nth = 1; nth <= 4; ++nth) {
        lcd_face_t *face = NULL;
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(nth);
        /* The two row buffers fall back to slower memory. */
        const xiaotai_err_t err = lcd_face_create(PAPER, &face);
        CHECK(nth <= 2 ? err == XIAOTAI_ERR_NO_MEM && !face : err == XIAOTAI_OK && face);
        lcd_face_destroy(face);
        CHECK(xiaotai_mem_host_outstanding() == 0);
    }
    xiaotai_mem_host_clear();
    /* A size that does not fit leaves the old one. */
    lcd_face_t *face = NULL;
    CHECK(lcd_face_create(PAPER, &face) == XIAOTAI_OK);
    xiaotai_mem_host_fail_nth(1);
    CHECK(lcd_face_size(face, 600, 310) == XIAOTAI_ERR_NO_MEM && lcd_face_size(face, 0, 10) == XIAOTAI_ERR_INVALID_ARG);
    W = LCD_FACE_WIDTH;
    H = LCD_FACE_HEIGHT;
    lcd_face_attach(face, 1000);
    lcd_face_rect_t rs[LCD_FACE_REGIONS];
    CHECK(step(face, 1000, true, rs) == 1 && rs[0].width == W);
    draw(face);
    lcd_face_destroy(face);
    xiaotai_mem_host_clear();
}

int main(void)
{
    catalog();
    memory();
    damage_and_clips(LCD_FACE_WIDTH, LCD_FACE_HEIGHT);
    damage_and_clips(101, 52);                      /* beside a call */
    damage_and_clips(321, 165);
    damage_and_clips(600, 310);
    edges(LCD_FACE_WIDTH, LCD_FACE_HEIGHT);
    edges(320, 165);
    edges(600, 310);
    independent_backdrops();
    timeline();
    CHECK(xiaotai_mem_host_outstanding() == 0);
    return 0;
}
