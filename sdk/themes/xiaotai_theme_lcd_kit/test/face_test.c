// SPDX-License-Identifier: MIT
// Replaces legacy: tests/lvgl/test_draw_hook.c::draw_hook
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lcd_face.h"
#include "xiaotai/theme_lcd.h"
#include "xiaotai_mem_host.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)

enum { W = LCD_FACE_WIDTH, H = LCD_FACE_HEIGHT, PAD = 8, ROW = W + 2 * PAD, SLICES = H / 9 };
static uint16_t canvas[W * H], previous[W * H];
static uint16_t direct[ROW * (H + 2 * PAD)];
static const xiaotai_ui_surface_t frame = {.pixels = (uint8_t *)canvas, .width = W, .height = H,
    .stride = W * sizeof(*canvas), .pixel = XIAOTAI_PIXEL_RGB565};
static const uint64_t fnv_offset = UINT64_C(14695981039346656037);

/* These schedules are checked against RGB565 from the original 55-channel
 * source, using the pinned LVGL 9.5.0 software arithmetic and no real time.
 * The two attach epochs choose both variants through the real PRNG; these
 * names define vector order, not an application or theme index contract. */
static const char *const names[] = {"neutral", "happy", "laughing", "funny", "sad", "angry", "crying", "loving",
    "embarrassed", "surprised", "shocked", "thinking", "winking", "cool", "relaxed", "delicious", "kissy",
    "confident", "sleepy", "silly", "confused", "listening", "ambient"};
static const uint32_t samples[] = {40, 160, 480, 960, 1320, 1800, 2600, 3000, 4800, 7000, 10400};

static uint64_t capture(lcd_face_t *face, uint64_t seed)
{
    CHECK(lcd_face_canvas(face, &frame));
    for (size_t i = 0; i < sizeof(canvas) / sizeof(*canvas); ++i) {
        seed = (seed ^ (canvas[i] & UINT16_C(0xff))) * UINT64_C(1099511628211);
        seed = (seed ^ (canvas[i] >> 8)) * UINT64_C(1099511628211);
    }
    return seed;
}

static lcd_face_t *create(uint32_t background, uint64_t at)
{
    lcd_face_t *face = NULL;
    CHECK(lcd_face_create(background, &face) == XIAOTAI_OK && face);
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
    char line[160];
    lcd_face_report(face, line, sizeof(line));
    const char *value = strstr(line, key);
    CHECK(value);
    return (unsigned)strtoul(value + strlen(key), NULL, 10);
}

static void catalog(void)
{
    CHECK(sizeof(names) / sizeof(*names) == XIAOTAI_THEME_LCD_EMOTION_COUNT);
    for (size_t i = 0; i < XIAOTAI_THEME_LCD_EMOTION_COUNT; ++i) {
        const char *name = xiaotai_theme_lcd_emotion_name(i);
        CHECK(name && *name && lcd_face_resolve(name) == i);
        CHECK(lcd_face_has_alternate(i) == (strcmp(name, "thinking") != 0 && strcmp(name, "relaxed") != 0));
    }
    CHECK(!xiaotai_theme_lcd_emotion_name(XIAOTAI_THEME_LCD_EMOTION_COUNT));
    CHECK(lcd_face_resolve(NULL) == lcd_face_resolve("neutral"));
    CHECK(lcd_face_resolve(NULL) == lcd_face_resolve("not-a-face"));
    CHECK(lcd_face_resolve("calm") == lcd_face_resolve("neutral"));
    CHECK(lcd_face_resolve("excited") == lcd_face_resolve("laughing"));
    CHECK(lcd_face_resolve("curious") == lcd_face_resolve("confused"));
    CHECK(lcd_face_resolve("proud") == lcd_face_resolve("confident"));
    CHECK(lcd_face_resolve("moved") == lcd_face_resolve("loving"));
    CHECK(lcd_face_resolve("speech") == lcd_face_resolve("listening"));
}

static void compare_corpus(uint32_t background, uint64_t expected)
{
    uint64_t hash = fnv_offset;
    unsigned variants = 0, visits = 0;
    lcd_face_rect_t rs[LCD_FACE_REGIONS];
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        const char *name = names[i];
        const bool neutral = strcmp(name, "neutral") == 0;
        const unsigned count = lcd_face_has_alternate(lcd_face_resolve(name)) ? 2 : 1;
        for (unsigned variant = 0; variant < count; ++variant) {
            const uint64_t t = variant != neutral ? 1008 : 1000;
            lcd_face_t *face = create(background, t);
            CHECK(step(face, t, true, rs));
            if (neutral) lcd_face_set(face, "surprised", XIAOTAI_TALK_IDLE, 0, t + 40);
            CHECK(step(face, t + 40, true, rs));
            lcd_face_set(face, name, XIAOTAI_TALK_IDLE, 3, t + 80);
            size_t next = 0;
            for (uint32_t dt = 0; next < sizeof(samples) / sizeof(samples[0]); dt += 40) {
                CHECK(step(face, t + 80 + dt, true, rs));
                if (dt == samples[next]) {
                    hash = capture(face, hash);
                    ++next;
                    ++visits;
                }
            }
            lcd_face_destroy(face);
            ++variants;
        }
    }
    CHECK(variants == 44 && visits == 484);

    const char *const moods[] = {"happy", "confused", "laughing"};
    for (uint8_t phase = XIAOTAI_TALK_LISTENING; phase <= XIAOTAI_TALK_SPEAKING; ++phase) {
        lcd_face_t *face = create(background, 2000);
        CHECK(step(face, 2000, true, rs));
        lcd_face_set(face, moods[phase - XIAOTAI_TALK_LISTENING], phase, 3, 2040);
        for (uint64_t t = 2040; t <= 3040; t += 40) {
            CHECK(step(face, t, true, rs));
            if (t == 2040 || t == 2240 || t == 2520 || t == 3040) {
                hash = capture(face, hash);
                ++visits;
            }
        }
        lcd_face_destroy(face);
    }
    CHECK(visits == 496);
    if (hash != expected)
        fprintf(stderr, "backdrop=%06x corpus=%016" PRIx64 " expected=%016" PRIx64 "\n", background, hash, expected);
    CHECK(hash == expected);
}

static bool covered(const lcd_face_rect_t *rs, size_t count, int x, int y)
{
    for (size_t i = 0; i < count; ++i)
        if (x >= rs[i].x && x < rs[i].x + rs[i].width && y >= rs[i].y && y < rs[i].y + rs[i].height) return true;
    return false;
}

static void exact_damage_and_clips(void)
{
    lcd_face_t *face = create(0x141719, 1000);
    lcd_face_rect_t rs[LCD_FACE_REGIONS];
    CHECK(step(face, 1000, true, rs));
    CHECK(lcd_face_canvas(face, &frame));
    memcpy(previous, canvas, sizeof(previous));
    unsigned changed = 0;
    size_t draws = 1;
    for (uint64_t t = 1040; t <= 3400; t += 40) {
        if (t == 1040) lcd_face_set(face, "surprised", XIAOTAI_TALK_SPEAKING, 3, t);
        if (t == 1400) lcd_face_set(face, "crying", XIAOTAI_TALK_IDLE, 1, t);
        if (t == 1840) lcd_face_set(face, "laughing", XIAOTAI_TALK_LISTENING, 3, t);
        if (t == 2600) lcd_face_set(face, "ambient", XIAOTAI_TALK_THINKING, 0, t);
        const size_t count = step(face, t, true, rs);
        CHECK(count && lcd_face_canvas(face, &frame));
        ++draws;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t i = (size_t)y * W + x;
                if (canvas[i] != previous[i]) {
                    CHECK(covered(rs, count, x, y));
                    ++changed;
                }
            }
        }
        memcpy(previous, canvas, sizeof(previous));
    }
    CHECK(changed > 1000);

    const uint16_t guard = UINT16_C(0xaa55);
    for (size_t i = 0; i < sizeof(direct) / sizeof(*direct); ++i) direct[i] = guard;
    for (int y = 0; y < H; y += 9) {
        const xiaotai_ui_surface_t slice = {.pixels = (uint8_t *)(direct + (y + PAD) * ROW + 3),
            .x = 3, .y = (uint16_t)(y + PAD), .width = ROW - 3, .height = 9,
            .stride = ROW * sizeof(*direct), .pixel = XIAOTAI_PIXEL_RGB565, .swap_bytes = true};
        CHECK(lcd_face_paint(face, &slice, PAD, PAD));
    }
    for (int y = 0; y < H + 2 * PAD; ++y) {
        for (int x = 0; x < ROW; ++x) {
            uint16_t expected = guard;
            if (x >= PAD && x < PAD + W && y >= PAD && y < PAD + H) {
                const uint16_t p = canvas[(y - PAD) * W + x - PAD];
                expected = (uint16_t)(p >> 8 | p << 8);
            }
            CHECK(direct[(size_t)y * ROW + x] == expected);
        }
    }
    CHECK(counter(face, "direct=") == SLICES && counter(face, "direct_pixels=") == W * H);
    CHECK(counter(face, "fallback=") == draws && counter(face, "face_late=") == 0);

    const xiaotai_ui_surface_t clipped = {.pixels = (uint8_t *)direct, .width = ROW, .height = H + 2 * PAD,
        .stride = ROW * sizeof(*direct), .pixel = XIAOTAI_PIXEL_RGB565};
    for (size_t i = 0; i < sizeof(direct) / sizeof(*direct); ++i) direct[i] = guard;
    CHECK(lcd_face_paint(face, &clipped, -13, -9));
    for (int y = 0; y < H + 2 * PAD; ++y) {
        for (int x = 0; x < ROW; ++x) {
            const uint16_t expected = x < W - 13 && y < H - 9 ? canvas[(size_t)(y + 9) * W + x + 13] : guard;
            CHECK(direct[(size_t)y * ROW + x] == expected);
        }
    }

    xiaotai_ui_surface_t invalid = {.pixels = (uint8_t *)direct, .width = W, .height = H,
        .stride = ROW * sizeof(*direct), .pixel = XIAOTAI_PIXEL_MONO1};
    CHECK(!lcd_face_paint(face, &invalid, PAD, PAD));
    invalid.pixel = XIAOTAI_PIXEL_RGB565;
    CHECK(!lcd_face_paint(face, &invalid, 1000, 1000));
    invalid.stride = 2;
    CHECK(!lcd_face_paint(face, &invalid, PAD, PAD));
    lcd_face_destroy(face);
}

static void independent_backdrops(void)
{
    lcd_face_rect_t rs[LCD_FACE_REGIONS];
    lcd_face_t *a = create(0x141719, 1000), *b = create(0x1c1f22, 1000);
    CHECK(step(a, 1000, true, rs) && step(b, 1000, true, rs));
    const uint64_t a_first = capture(a, fnv_offset), b_first = capture(b, fnv_offset);
    CHECK(a_first != b_first);
    lcd_face_set(a, "sad", XIAOTAI_TALK_THINKING, 3, 1020);
    CHECK(capture(a, fnv_offset) == a_first && capture(b, fnv_offset) == b_first);
    CHECK(step(a, 1040, true, rs) && capture(b, fnv_offset) == b_first);
    lcd_face_destroy(a);
    a = create(0x141719, 1000);
    CHECK(step(a, 1000, true, rs));
    lcd_face_set_background(a, 0x1c1f22);
    CHECK(step(a, 1040, true, rs) && step(b, 1040, true, rs));
    const uint64_t after = capture(b, fnv_offset);
    CHECK(capture(a, fnv_offset) == after);
    lcd_face_destroy(a);
    lcd_face_destroy(b);
}

static void timeline(void)
{
    lcd_face_rect_t rs[LCD_FACE_REGIONS];
    lcd_face_t *face = create(0x141719, 1000);
    CHECK(step(face, 1000, true, rs));
    CHECK(!step(face, 1039, true, rs) && lcd_face_delay(face, 1039) == 1);
    lcd_face_set(face, "neutral", XIAOTAI_TALK_LISTENING, 3, 1040);
    CHECK(step(face, 1040, true, rs) && step(face, 1080, true, rs));
    CHECK(capture(face, fnv_offset) == UINT64_C(0xcfee211be04f027b));
    for (uint64_t t = 1120; t <= 14000; t += 40) CHECK(!step(face, t, false, rs));
    CHECK(lcd_face_delay(face, 14000) == 0 && capture(face, fnv_offset) == UINT64_C(0xcfee211be04f027b));
    CHECK(step(face, 14000, true, rs));
    CHECK(capture(face, fnv_offset) == UINT64_C(0x29bfeb17e1d711ae));
    CHECK(!step(face, 14039, true, rs) && counter(face, "face_late=") == 0);

    lcd_face_detach(face);
    CHECK(!step(face, 15999, false, rs));
    lcd_face_attach(face, 16000);
    CHECK(step(face, 16000, true, rs));
    uint64_t h = capture(face, fnv_offset);
    for (uint64_t t = 16040; t <= 16720; t += 40) CHECK(step(face, t, true, rs));
    CHECK(capture(face, h) == UINT64_C(0xbb1117a4f490b75f));
    CHECK(counter(face, "face_late=") == 0);
    lcd_face_destroy(face);

    const uint64_t at = UINT32_MAX - UINT64_C(20);
    face = create(0x141719, at);
    CHECK(step(face, at, true, rs));
    h = capture(face, fnv_offset);
    CHECK(!step(face, at + 39, true, rs));
    lcd_face_set(face, "crying", XIAOTAI_TALK_THINKING, 2, at + 40);
    for (uint64_t t = at + 40; t <= at + 800; t += 40) CHECK(step(face, t, true, rs));
    CHECK(capture(face, h) == UINT64_C(0x6de92d06ad8b48ed));
    CHECK(counter(face, "face_late=") == 0 && step(face, at + 881, true, rs));
    CHECK(counter(face, "face_late=") == 1);
    lcd_face_destroy(face);
}

int main(void)
{
    catalog();
    lcd_face_t *face = NULL;
    xiaotai_mem_host_clear();
    xiaotai_mem_host_fail_nth(1);
    CHECK(lcd_face_create(0x141719, &face) == XIAOTAI_ERR_NO_MEM && !face);
    CHECK(xiaotai_mem_host_outstanding() == 0);
    compare_corpus(0x141719, UINT64_C(0x8fdb96c47ef64d1d));
    compare_corpus(0x1c1f22, UINT64_C(0x3a27df07cb823750));
    exact_damage_and_clips();
    independent_backdrops();
    timeline();
    CHECK(xiaotai_mem_host_outstanding() == 0);
    return 0;
}
