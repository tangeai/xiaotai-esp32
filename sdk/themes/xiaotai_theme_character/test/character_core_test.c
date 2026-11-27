// SPDX-License-Identifier: MIT
/* theme_character_core: the character without a display. The sprite pack is
 * read and refused when damaged; shapes and sprites stay inside what they may
 * write; moods, conversation phases, device conditions and touch each lead to
 * the pose and the pictures they should; poses move by springs, never by a
 * jump; a still character asks for no redraw.
 *
 * Everything runs twice: on the pack the firmware ships and on a figure made
 * here (chr_fixture.h) with other proportions and other parts. What is said
 * about ears, a tail, arms or a turning head is checked on the figure that
 * has them. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chr.h"
#include "chr_pack_host.h"
#include "chr_fixture.h"
#include "chr_internal.h"
#include "chr_sprites.h"

static int failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

enum { W = 1024, H = 600, FRAME = 33 };
static chr_t *c;
static const void *pack;
static size_t pack_size;

static chr_info_t step(unsigned ms)
{
    chr_rect_t damage[CHR_DAMAGE_MAX];
    for (unsigned t = 0; t < ms; t += FRAME) (void)chr_step(c, FRAME, damage);
    chr_info_t info;
    chr_inspect(c, &info);
    return info;
}

static void fresh(void)
{
    CHECK(chr_init(c, W, H, 7, pack, pack_size));
    (void)step(400);
}

/* Some time of a talk: what the microphone hears and what is heard from the speaker. */
static chr_info_t sounding(unsigned ms, uint16_t microphone, uint16_t speaker)
{
    chr_info_t info;
    chr_inspect(c, &info);
    for (unsigned t = 0; t < ms; t += FRAME) {
        chr_set_envelope(c, microphone, speaker);
        info = step(FRAME);
    }
    return info;
}

/* Steps of another length than FRAME, with a level of the speaker's. */
static chr_info_t paced(unsigned ms, unsigned dt, uint16_t speaker)
{
    chr_rect_t damage[CHR_DAMAGE_MAX];
    chr_info_t info;
    chr_inspect(c, &info);
    for (unsigned t = 0; t < ms; t += dt) {
        chr_set_envelope(c, 0, speaker);
        (void)chr_step(c, dt, damage);
        chr_inspect(c, &info);
    }
    return info;
}

static bool has(uint32_t feature) { return (chr_features(c) & feature) != 0; }
/* Places on this figure, in pixels from its anchor. */
static int cheek_x(void) { return c->fig.eye_lx + c->fig.cheek_dx; }
static int cheek_y(void) { return c->fig.cheek_y; }
static int crown_y(void) { return (c->fig.hair_top + c->fig.brow_y) / 2; }

static void tap(int x, int y)
{
    (void)chr_pointer(c, true, (int16_t)(W / 2 + x), (int16_t)(H + y));
    (void)step(70);
    (void)chr_pointer(c, false, 0, 0);
    (void)step(FRAME);
}

static void test_pack(void)
{
    chr_pack_t opened;
    CHECK(chr_pack_open(&opened, pack, pack_size));
    CHECK(opened.count > H_SLOTS / 2 && opened.figure_height >= 400 && opened.slot_count >= SL_POSE + CHR_POSES);
    CHECK(opened.slot_count <= SL_COUNT);      /* a pack from before a slot was added has fewer: those parts are not there */
    CHECK(opened.layout_count == CHR_FIGURE_VALUES);
    chr_sprite_t sprite;
    const unsigned body = chr_pack_slot(&opened, SL_BODY);
    CHECK(body != CHR_NO_SPRITE && chr_pack_sprite(&opened, body, &sprite) && sprite.width > 60 && sprite.height > 60 && sprite.y < 0);
    CHECK(!chr_pack_sprite(&opened, opened.count, &sprite));
    CHECK(chr_pack_slot(&opened, SL_COUNT) == CHR_NO_SPRITE && chr_pack_layout(&opened, CHR_FIGURE_VALUES) == 0);
    /* The layout is the figure: the head is above the neck, the eyes left and right of the middle. */
    CHECK(chr_init(c, W, H, 3, pack, pack_size));
    CHECK(c->fig.hair_top < c->fig.brow_y && c->fig.brow_y < c->fig.eye_y && c->fig.eye_y < c->fig.mouth_y &&
          c->fig.mouth_y < c->fig.neck_y && c->fig.neck_y < 0 && c->fig.eye_rx < c->fig.face_x && c->fig.face_x < c->fig.eye_lx &&
          c->fig.head_half > 40);
    const chr_pack_t good = opened;
    /* Damaged packs are refused, not drawn: a cut file, a wrong name, a row table that points outside. */
    CHECK(!chr_pack_open(&opened, pack, pack_size / 2));
    CHECK(!chr_pack_open(&opened, pack, 8));
    CHECK(!chr_pack_open(&opened, NULL, pack_size));
    uint8_t *copy = malloc(pack_size);
    memcpy(copy, pack, pack_size);
    copy[0] = 'Y';
    CHECK(!chr_pack_open(&opened, copy, pack_size));
    memcpy(copy, pack, pack_size);
    uint32_t start;
    memcpy(&start, copy + good.table_at + 8, sizeof(start));
    memset(copy + start + 8, 0xFF, 4);
    CHECK(!chr_pack_open(&opened, copy, pack_size));
    /* A slot that names a sprite the pack does not hold. */
    memcpy(copy, pack, pack_size);
    const uint16_t beyond = good.count;
    memcpy(copy + good.slots_at + 2 * SL_BODY, &beyond, sizeof(beyond));
    fixture_seal(copy);
    CHECK(!chr_pack_open(&opened, copy, pack_size));
    /* Anything else in the head that is not as it was written: its checksum says so. */
    memcpy(copy, pack, pack_size);
    copy[good.layout_at] ^= 1;
    CHECK(!chr_pack_open(&opened, copy, pack_size));
    fixture_seal(copy);
    CHECK(chr_pack_open(&opened, copy, pack_size));
    /* Every sprite is what its checksum says, and one changed byte is noticed. */
    memcpy(copy, pack, pack_size);
    for (unsigned i = 0; i < good.count; ++i) {
        uint32_t at, length;
        CHECK(chr_pack_place(&good, i, &at, &length) && chr_pack_intact(&good, i, (const uint8_t *)pack + at));
        if (length > 40000U) continue;
        copy[at + length - 1U] ^= 1;
        CHECK(!chr_pack_intact(&good, i, copy + at));
    }
    CHECK(chr_crc32(0, "123456789", 9) == 0xCBF43926U && chr_crc32(chr_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926U);
    /* An older pack (no slots, no layout) is not this format. */
    memcpy(copy, pack, pack_size);
    copy[4] = 1;
    CHECK(!chr_pack_open(&opened, copy, pack_size));
    free(copy);
    /* A pack that opens but brings no body is not a figure. */
    size_t bodiless_size = 0;
    uint8_t *bodiless = fixture_pack(FIXTURE_EARS | FIXTURE_NO_BODY, &bodiless_size, NULL);
    CHECK(chr_pack_open(&opened, bodiless, bodiless_size) && !chr_init(c, W, H, 1, bodiless, bodiless_size));
    free(bodiless);
    CHECK(!chr_init(c, W, H, 1, "nope", 4));
}

/* The first and last rows and columns of a picture that are not background (zero). */
static void extent(const uint16_t *pixels, int width, int height, int rows[2], int columns[2])
{
    rows[0] = height; rows[1] = -1; columns[0] = width; columns[1] = -1;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            if (!pixels[y * width + x]) continue;
            if (y < rows[0]) rows[0] = y;
            if (y > rows[1]) rows[1] = y;
            if (x < columns[0]) columns[0] = x;
            if (x > columns[1]) columns[1] = x;
        }
}

static void test_shapes(void)
{
    /* An ellipse covers its middle row and nothing above its top. */
    chr_shape_t shape = {.kind = CHR_ELLIPSE, .alpha = 32, .top = 0xFFFFFF, .bottom = 0xFFFFFF, .a = {50, 40, 20, 10}};
    CHECK(chr_shape_bounds(&shape) && shape.x0 <= 30 && shape.x1 >= 70 && shape.y0 <= 30 && shape.y1 >= 50);
    float spans[4];
    CHECK(chr_shape_spans(&shape, 40.0f, spans) == 1 && fabsf(spans[0] - 30.0f) < 0.01f && fabsf(spans[1] - 70.0f) < 0.01f);
    CHECK(chr_shape_spans(&shape, 29.0f, spans) == 0);
    /* The mouth band splits into two spans above its sagging middle. */
    chr_shape_t band = {.kind = CHR_BAND, .alpha = 32, .a = {50, 20, 38, -10, 44, -10}};
    CHECK(chr_shape_bounds(&band));
    CHECK(chr_shape_spans(&band, 33.0f, spans) == 2 && spans[1] < 50.0f && spans[2] > 50.0f);
    CHECK(chr_shape_spans(&band, 41.0f, spans) == 1);
    /* Drawing never leaves the rectangle it was given: guard pixels around it stay as they were. */
    enum { TW = 64, TH = 48, PAD = 8 };
    static uint16_t pixels[(TW + 2 * PAD) * (TH + 2 * PAD)];
    static int16_t scratch[TW + 4];
    memset(pixels, 0x5A, sizeof(pixels));
    const chr_target_t target = {.pixels = (uint8_t *)&pixels[PAD * (TW + 2 * PAD) + PAD], .stride = (TW + 2 * PAD) * 2,
        .x0 = 20, .y0 = 20, .x1 = 20 + TW - 1, .y1 = 20 + TH - 1, .scratch = scratch};
    chr_shape_t big = {.kind = CHR_ELLIPSE, .alpha = 32, .top = 0x102030, .bottom = 0x405060, .a = {50, 40, 300, 300}};
    CHECK(chr_shape_bounds(&big));
    CHECK(chr_shape_fill(&target, &big) == TW * TH);
    unsigned touched = 0, guard_changed = 0;
    for (int y = 0; y < TH + 2 * PAD; ++y)
        for (int x = 0; x < TW + 2 * PAD; ++x) {
            const bool inside = x >= PAD && x < PAD + TW && y >= PAD && y < PAD + TH;
            const bool changed = pixels[y * (TW + 2 * PAD) + x] != 0x5A5A;
            touched += inside && changed;
            guard_changed += !inside && changed;
        }
    CHECK(touched == TW * TH && guard_changed == 0);
    for (int i = 0; i < TW + 4; ++i) CHECK(scratch[i] == 0);
    /* The same for a sprite that hangs over every edge. */
    chr_pack_t opened;
    CHECK(chr_pack_open(&opened, pack, pack_size));
    memset(pixels, 0x5A, sizeof(pixels));
    chr_blit_t blit = {.id = (uint16_t)chr_pack_slot(&opened, SL_BODY), .x = -40, .y = -60, .alpha = 32, .tilt = 0.05f, .tilt_y = 40, .sway = 6,
        .sway_from = 0, .sway_to = 80};
    CHECK(chr_blit_bounds(&opened, &blit));
    CHECK(chr_blit_draw(&target, &opened, &blit) > 0);
    guard_changed = 0;
    for (int y = 0; y < TH + 2 * PAD; ++y)
        for (int x = 0; x < TW + 2 * PAD; ++x)
            if (!(x >= PAD && x < PAD + TW && y >= PAD && y < PAD + TH) && pixels[y * (TW + 2 * PAD) + x] != 0x5A5A) ++guard_changed;
    CHECK(guard_changed == 0);

    /* A picture that grows about a pin (all of a flat figure). Drawn on a stage as large as it
     * needs: it stays on its pin, gets as much taller or wider as asked, is the same picture
     * wherever it stands, and is the same whether one painter draws it or two share its rows. */
    size_t made_size = 0;
    chr_figure_t fig;
    uint8_t *made = fixture_pack(FIXTURE_EARS, &made_size, &fig);
    CHECK(chr_pack_open(&opened, made, made_size));
    enum { SW = 300, SH = 320 };
    static uint16_t stage[SW * SH], still[SW * SH];
    static int16_t line[SW + 4 + 3 * (SW + 1)];
    CHECK(sizeof(line) / sizeof(line[0]) == chr_target_scratch(SW, SW));
    const chr_target_t whole = {.pixels = (uint8_t *)stage, .stride = SW * 2, .x0 = 0, .y0 = 0, .x1 = SW - 1, .y1 = SH - 1,
        .scratch = line, .lines = SW};
    const chr_target_t rest = {.pixels = (uint8_t *)still, .stride = SW * 2, .x0 = 0, .y0 = 0, .x1 = SW - 1, .y1 = SH - 1,
        .scratch = line, .lines = SW};
    const uint16_t body = (uint16_t)chr_pack_slot(&opened, SL_BODY);
    chr_sprite_t size;
    CHECK(chr_pack_sprite(&opened, body, &size) && size.height >= 200 && size.width <= SW - 60 && size.height <= SH - 70);
    const int bx = 30, by = 50, feet = by + (int)size.height, middle = bx + (int)size.width / 2;
    memset(still, 0, sizeof(still));
    chr_blit_t plain = {.id = body, .x = bx, .y = by, .alpha = 32};
    CHECK(chr_blit_bounds(&opened, &plain));
    const uint32_t plain_pixels = chr_blit_draw(&rest, &opened, &plain);
    CHECK(plain_pixels > 0);
    int rows_plain[2], columns_plain[2];
    extent(still, SW, SH, rows_plain, columns_plain);
    CHECK(rows_plain[0] >= by && rows_plain[1] == feet - 1);

    /* A tenth taller: her feet stay, her head is a tenth of her height higher, she is no wider. */
    memset(stage, 0, sizeof(stage));
    chr_blit_t taller = {.id = body, .x = bx, .y = by, .alpha = 32, .grow_y = 0.1f, .pin_x = (float)middle, .pin_y = (float)feet};
    CHECK(chr_blit_bounds(&opened, &taller) && taller.y0 < plain.y0 - (int)size.height / 12 && taller.y1 <= plain.y1 + 1);
    const uint32_t taller_pixels = chr_blit_draw(&whole, &opened, &taller);
    int rows[2], columns[2];
    extent(stage, SW, SH, rows, columns);
    const int head_rise = (feet - rows_plain[0]) / 10;
    CHECK(rows[1] >= feet - 1 && rows[1] <= feet && abs((rows_plain[0] - rows[0]) - head_rise) <= 2);
    CHECK(columns[0] == columns_plain[0] && columns[1] == columns_plain[1]);
    CHECK(taller_pixels > plain_pixels * 107U / 100U && taller_pixels < plain_pixels * 113U / 100U);
    /* A breath: two pixels taller at her head, and the rows in between follow in whole rows. */
    memset(stage, 0, sizeof(stage));
    chr_blit_t breath = taller;
    breath.grow_y = 2.0f / (float)size.height;
    CHECK(chr_blit_bounds(&opened, &breath) && chr_blit_draw(&whole, &opened, &breath) > 0);
    CHECK(memcmp(stage, still, sizeof(stage)) != 0);
    extent(stage, SW, SH, rows, columns);
    CHECK(rows[1] == feet - 1 && rows_plain[0] - rows[0] >= 1 && rows_plain[0] - rows[0] <= 2);

    /* Shorter and wider (a squash): lower, and as much wider on both sides. */
    memset(stage, 0, sizeof(stage));
    chr_blit_t squash = {.id = body, .x = bx, .y = by, .alpha = 32, .grow_x = 0.1f, .grow_y = -0.2f, .pin_x = (float)middle,
        .pin_y = (float)feet};
    CHECK(chr_blit_bounds(&opened, &squash) && chr_blit_draw(&whole, &opened, &squash) > 0);
    extent(stage, SW, SH, rows, columns);
    CHECK(rows[1] >= feet - 1 && rows[1] <= feet && abs((rows[0] - rows_plain[0]) - 2 * head_rise) <= 3);
    CHECK(columns[0] < columns_plain[0] && columns[1] > columns_plain[1]);
    CHECK(abs((columns_plain[0] - columns[0]) - (columns[1] - columns_plain[1])) <= 2);

    /* The same picture wherever it stands: moved with its pin, every pixel moves with it. That is
     * also why her eyes and mouth stay on her face: they share the pin and the growth. */
    memset(still, 0, sizeof(still));
    chr_blit_t moved = squash;
    moved.x += 17; moved.y -= 9; moved.pin_x += 17.0f; moved.pin_y -= 9.0f;
    CHECK(chr_blit_bounds(&opened, &moved) && chr_blit_draw(&rest, &opened, &moved) > 0);
    bool same = true;
    for (int y = 9; y < SH; ++y)
        for (int x = 0; x < SW - 17; ++x) same &= stage[y * SW + x] == still[(y - 9) * SW + x + 17];
    CHECK(same);

    /* Two painters that share her rows (the second core) draw what one draws. */
    memset(still, 0, sizeof(still));
    const int cut = feet - 97;
    chr_target_t upper = rest, lower = rest;
    upper.y1 = (int16_t)(cut - 1);
    lower.y0 = (int16_t)cut;
    lower.pixels = (uint8_t *)&still[cut * SW];
    CHECK(chr_blit_draw(&upper, &opened, &squash) > 0 && chr_blit_draw(&lower, &opened, &squash) > 0);
    CHECK(!memcmp(stage, still, sizeof(stage)));

    /* Half as opaque over a background: between the background and the picture. */
    for (int i = 0; i < SW * SH; ++i) still[i] = 0x001F;
    chr_blit_t faint = squash;
    faint.alpha = 16;
    CHECK(chr_blit_bounds(&opened, &faint) && chr_blit_draw(&rest, &opened, &faint) > 0);
    unsigned drawn = 0, between = 0;
    for (int i = 0; i < SW * SH; ++i) {
        if (!stage[i]) continue;
        ++drawn;
        between += still[i] != 0x001F && still[i] != stage[i];
    }
    CHECK(drawn > 0 && between > drawn / 2);

    /* Without room for its rows a growing picture is not drawn; the scratch stays clean either way. */
    chr_target_t narrow = whole;
    narrow.lines = (uint16_t)(size.width - 1U);
    CHECK(chr_blit_draw(&narrow, &opened, &squash) == 0);
    for (int i = 0; i < SW + 4; ++i) CHECK(line[i] == 0);
    free(made);
}

static void test_moods(void)
{
    /* The names every colour theme accepts, their aliases, and neutral for anything else. */
    static const char *const names[] = {"neutral", "happy", "laughing", "funny", "sad", "angry", "crying", "loving",
        "embarrassed", "surprised", "shocked", "thinking", "winking", "cool", "relaxed", "delicious", "kissy",
        "confident", "sleepy", "silly", "confused", "listening", "ambient"};
    CHECK(sizeof(names) / sizeof(names[0]) == CHR_MOOD_NAMES);
    for (size_t i = 0; i < CHR_MOOD_NAMES; ++i) CHECK(!strcmp(chr_mood_name(chr_mood_find(names[i])), names[i]));
    /* And those of a conversation, which other themes draw as one of these. */
    static const char *const talk[] = {"excited", "gentle", "comfort", "serious", "moved"};
    CHECK(sizeof(talk) / sizeof(talk[0]) == CHR_MOOD_COUNT - CHR_MOOD_NAMES);
    for (size_t i = 0; i < CHR_MOOD_COUNT - CHR_MOOD_NAMES; ++i) CHECK(!strcmp(chr_mood_name(CHR_MOOD_NAMES + i), talk[i]));
    CHECK(chr_mood_name(CHR_MOOD_COUNT) == NULL);
    CHECK(chr_mood_find("proud") == chr_mood_find("confident") && chr_mood_find("calm") == CHR_MOOD_NEUTRAL);
    CHECK(chr_mood_find("no such mood") == CHR_MOOD_NEUTRAL && chr_mood_find(NULL) == CHR_MOOD_NEUTRAL);
    for (int cue = 1; cue < CHR_CUE_COUNT; ++cue) CHECK(chr_cue_find(chr_cue_name((chr_cue_t)cue)) == (chr_cue_t)cue);
    CHECK(chr_cue_find("no such cue") == CHR_CUE_NONE && chr_cue_name(CHR_CUE_NONE) == NULL);

    fresh();
    chr_set_mood(c, chr_mood_find("laughing"));
    chr_info_t info = step(1500);
    /* smiling eyes (a flat figure's are glad ones, open), open smile */
    CHECK((has(CHR_FLAT) ? info.eye_left != 3 : info.eye_left == 3 && info.eye_right == 3) && info.mouth_form == 2 && info.mouth_open >= 3);
    fresh();
    chr_set_mood(c, chr_mood_find("sad"));
    info = step(1500);
    CHECK(info.mouth_form == 0 && info.pose[P_EAR_L] > 0.8f && info.pose[P_WAG] < 0.1f);   /* a pout; ears flop, tail still */
    if (has(CHR_HAS_EARS)) CHECK(info.ear_left == 2 && info.ear_right == 2);
    fresh();
    chr_set_mood(c, chr_mood_find("winking"));
    (void)step(1500);
    bool left_open = false, right_shut = true;   /* over a while: she may blink meanwhile */
    for (int i = 0; i < 40; ++i) {
        info = step(FRAME);
        left_open |= info.eye_left == 0;
        right_shut &= info.eye_right == 4;
    }
    CHECK(left_open && right_shut);
    fresh();
    chr_set_mood(c, chr_mood_find("sleepy"));
    info = step(4000);
    CHECK(info.eye_left >= 2 && info.particles > 0);   /* heavy eyes and a Z in the air */
}

static void test_motion(void)
{
    /* A new mood is reached by springs: no channel jumps in one frame, and it settles near the target. */
    fresh();
    chr_info_t before = step(FRAME);
    chr_set_mood(c, chr_mood_find("sad"));
    chr_info_t after = step(FRAME);
    CHECK(fabsf(after.pose[P_EAR_L] - before.pose[P_EAR_L]) < 0.5f);
    CHECK(fabsf(after.pose[P_MC] - before.pose[P_MC]) < 1.2f);
    if (!has(CHR_FLAT)) {
        /* Brows of their own and a head that goes with the gaze travel too; the eyes change behind the lids. */
        fresh();
        chr_set_mood(c, chr_mood_find("shocked"));
        before = step(FRAME);
        for (int i = 0; i < 30; ++i) {
            after = step(FRAME);
            CHECK(fabsf(after.pose[P_BROW] - before.pose[P_BROW]) < 4.0f && fabsf(after.pose[P_SLANT] - before.pose[P_SLANT]) < 0.5f);
            before = after;
        }
        chr_set_mood(c, chr_mood_find("sad"));
        after = step(FRAME);
    }
    after = step(2500);
    CHECK(fabsf(after.pose[P_EAR_L] - after.target[P_EAR_L]) < 0.08f);
    CHECK(fabsf(after.pose[P_SLANT] - after.target[P_SLANT]) < 0.05f);
    /* She blinks on her own within a few seconds, and does small things when left alone. */
    fresh();
    bool blinked = false, acted = false;
    for (int i = 0; i < 900; ++i) {
        const chr_info_t info = step(FRAME);
        blinked |= info.eye_left == 4;
        acted |= info.cue != CHR_CUE_NONE;
    }
    CHECK(blinked && acted);
}

static void test_talk(void)
{
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    chr_info_t info = step(FRAME);
    CHECK(info.cue == CHR_CUE_WAKE);
    for (int i = 0; i < 30; ++i) { chr_set_envelope(c, 1400, 0); info = step(FRAME); }
    CHECK(info.listen > 0.5f && info.pose[P_EAR_L] < -0.2f);    /* ears pricked towards the voice */
    chr_set_envelope(c, 0, 0);
    chr_set_phase(c, CHR_PHASE_THINKING);
    info = step(1200);
    CHECK(info.pose[P_GX] < -0.3f && info.pose[P_GY] > 0.0f && c->ponder && info.mouth_open == 0);
    /* Speaking: the mouth follows the speaker, loud opens it and silence shuts it. */
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    info = sounding(400, 0, 9000);
    CHECK(info.mouth == 0.5f && info.mouth_open == 2 && info.silent == 0.0f);
    info = sounding(700, 0, 1);
    CHECK(info.mouth == 0.0f && info.mouth_open == 0);
    /* Cut off in the middle of a word by somebody's voice: startled, with the face alone. */
    (void)sounding(1200, 0, 9000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    bool startled = false;
    for (int i = 0; i < 12; ++i) startled |= sounding(FRAME, 1400, 0).cue == CHR_CUE_STARTLE;
    chr_inspect(c, &info);
    CHECK(startled && info.gesture == CHR_GESTURE_NONE && info.particles == 0);
    (void)sounding(1500, 0, 0);
    /* An answer that has come to its end is heard on after the service has finished it: no start,
     * whoever speaks then, and the mouth goes on until it has been heard. */
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1200, 0, 9000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    startled = false;
    for (int i = 0; i < 11; ++i) {
        info = sounding(FRAME, 1400, 9000);
        startled |= info.cue == CHR_CUE_STARTLE;
        CHECK(info.mouth_open >= 2);
    }
    info = sounding(400, 1400, 0);
    CHECK(!startled && info.cue != CHR_CUE_STARTLE && info.mouth_open == 0);
    /* Nor when nobody spoke, or nothing was to be heard any more. */
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(900, 0, 9000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    startled = false;
    for (int i = 0; i < 12; ++i) startled |= sounding(FRAME, 0, 0).cue == CHR_CUE_STARTLE;
    (void)sounding(900, 0, 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(600, 0, 0);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    for (int i = 0; i < 12; ++i) startled |= sounding(FRAME, 1400, 0).cue == CHR_CUE_STARTLE;
    CHECK(!startled);
    /* Ended after a quiet turn: a goodbye. The mouth is for a talk: what else comes out of the speaker
     * (the other side of a call) does not move it. */
    chr_set_phase(c, CHR_PHASE_IDLE);
    info = step(200);
    CHECK(info.cue == CHR_CUE_BYE);
    (void)step(2000);
    chr_set_conditions(c, CHR_IN_CALL);
    (void)step(3000);
    info = sounding(500, 0, 9000);
    CHECK(info.mouth == 0.0f);
    chr_set_conditions(c, 0);
    /* Without any level from the speaker the mouth still moves while speaking, and only then. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    bool opened = false;
    for (int i = 0; i < 60; ++i) opened |= step(FRAME).mouth_open >= 2;
    CHECK(opened);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    CHECK(step(600).mouth == 0.0f);
}

/* The mouth: what it opens for, how wide, how long it stays, and when it shuts. */
static void test_mouth(void)
{
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 0);
    /* With a level to follow she does not pretend to speak. */
    chr_info_t info;
    for (int i = 0; i < 60; ++i) CHECK(sounding(FRAME, 0, 0).mouth == 0.0f);
    /* Next to nothing is silence however quiet it was before; a voice opens the mouth at once. */
    info = sounding(300, 0, 249);
    CHECK(info.mouth == 0.0f);
    info = sounding(FRAME, 0, 1500);
    CHECK(info.mouth > 0.0f && info.mouth_open >= 1);
    /* What is far below the loudest of the last seconds is silence too, and is a voice again once
     * that is some seconds past. */
    (void)sounding(1500, 0, 20000);
    info = sounding(500, 0, 1000);
    CHECK(info.mouth == 0.0f);
    CHECK(sounding(8000, 0, 1000).mouth > 0.0f);
    /* A small or a medium mouth, by how loud it is; wider is for laughing, not for talking. */
    for (int i = 0; i < 20; ++i) { info = sounding(FRAME, 0, 12000); if (i > 3) CHECK(info.mouth_open == 2); }
    for (int i = 0; i < 20; ++i) { info = sounding(FRAME, 0, 7500); if (i > 3) CHECK(info.mouth_open == 2); }
    for (int i = 0; i < 20; ++i) { info = sounding(FRAME, 0, 3000); if (i > 3) CHECK(info.mouth_open == 1); }
    /* It shuts a tenth of a second after the voice, through the small step for a frame, not held as a step is; and
     * a gap between two syllables does not shut it. */
    for (int wide = 0; wide < 2; ++wide) {
        (void)sounding(600, 0, wide ? 12000 : 3000);
        unsigned shut = 0;
        bool small = false;
        for (unsigned t = FRAME; t < 400 && !shut; t += FRAME) {
            info = sounding(FRAME, 0, 0);
            small |= info.mouth_open == 1;
            if (!info.mouth_open) shut = t;
            if (t < 90) CHECK(info.mouth_open > 0);
        }
        CHECK(shut >= 90 && shut <= 90 + 2 * FRAME && small && info.shut_ms >= 90 && info.shut_ms <= 90 + 2 * FRAME);
        CHECK(sounding(600, 0, 0).mouth_open == 0);
    }
    (void)sounding(600, 0, 9000);
    for (int i = 0; i < 12; ++i) {
        CHECK(sounding(2 * FRAME, 0, 0).mouth_open > 0);
        CHECK(sounding(3 * FRAME, 0, 9000).mouth_open > 0);
    }
    /* At whatever speed the display goes: shut within a fifth of a second, and every state, shut too, for
     * sixty milliseconds at least (or one frame), whatever the pauses are. Only on its way to shut the small
     * step gives way sooner. */
    static const unsigned paces[] = {20, 24, 27, 33, 40, 48, 55, 60};
    for (size_t k = 0; k < sizeof(paces) / sizeof(paces[0]); ++k) {
        const unsigned dt = paces[k];
        for (int wide = 0; wide < 2; ++wide) {
            (void)paced(600, dt, wide ? 12000 : 3000);
            unsigned shut = 0;
            for (unsigned t = dt; t < 500 && !shut; t += dt) if (!paced(dt, dt, 0).mouth_open) shut = t;
            CHECK(shut >= 90 && shut <= 200);
        }
        unsigned lasted = 1000, changed = 0, shuts = 0;
        (void)paced(600, dt, 5000);
        chr_inspect(c, &info);
        uint8_t was = info.mouth_open;
        for (unsigned i = 0; i < 1500; ++i) {
            /* words of 240 ms or so, with pauses of 60 to 240 ms between them */
            const unsigned t = i * dt % 2600, pause = t / 520 * 45 + 60;
            info = paced(dt, dt, t % 520 < 520 - pause ? (uint16_t)(3000 + 700 * (i % 13)) : 0);
            lasted += dt;
            if (info.mouth_open == was) continue;
            if (!(was == 1 && info.mouth_open == 0)) CHECK(lasted >= 60);
            shuts += info.mouth_open == 0;
            ++changed;
            lasted = 0;
            was = info.mouth_open;
        }
        CHECK(shuts > 10 && changed > 40);
    }
    (void)sounding(600, 0, 9000);
    /* A product that has no level to go by (any more): the mouth moves by itself while she speaks, and only then. */
    chr_no_envelope(c);
    bool small = false, wider = false;
    for (int i = 0; i < 60; ++i) { info = step(FRAME); small |= info.mouth_open == 1; wider |= info.mouth_open == 2; }
    CHECK(small && wider);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)step(400);
    for (int i = 0; i < 60; ++i) CHECK(step(FRAME).mouth_open == 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(600, 0, 9000);
    chr_inspect(c, &info);
    /* No step is shown for less than sixty milliseconds, whatever the level does. */
    static const uint16_t levels[] = {9000, 800, 12000, 3000, 5000, 11500, 1200, 7000, 12000, 2000, 12000};
    unsigned since = 1000, changes = 0;
    uint8_t before = info.mouth_open;
    (void)sounding(300, 0, 5000);
    chr_inspect(c, &info);
    before = info.mouth_open;
    for (unsigned i = 0; i < 400; ++i) {
        info = sounding(FRAME, 0, levels[(i * 7U) % 11U]);
        since += FRAME;
        if (info.mouth_open != before) {
            CHECK(since >= 60);
            since = 0;
            ++changes;
        }
        before = info.mouth_open;
    }
    CHECK(changes > 40);
    /* The brows go up where a sentence begins, and not along with the syllables. */
    (void)sounding(3000, 0, 0);
    float most = 0.0f, later = 0.0f;
    for (int i = 0; i < 90; ++i) {
        info = sounding(FRAME, 0, i % 5 < 3 ? 9000 : 1500);
        if (i < 12 && info.pose[P_BROW] > most) most = info.pose[P_BROW];
        if (i > 14 && info.pose[P_BROW] > later) later = info.pose[P_BROW];
    }
    CHECK(most > 2.5f && later < 1.0f);
}

/* The moods a conversation brings: in the face, between sentences, behind a blink, not too often. */
static void test_spoken_moods(void)
{
    const uint8_t happy = (uint8_t)chr_mood_find("happy"), serious = (uint8_t)chr_mood_find("serious"),
                  gentle = (uint8_t)chr_mood_find("gentle"), surprised = (uint8_t)chr_mood_find("surprised");
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(2000, 0, 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1000, 0, 9000);
    /* A word in the middle of a sentence waits for the speaker to be still for a moment ... */
    chr_set_mood(c, happy);
    chr_info_t info = sounding(700, 0, 9000);
    CHECK(info.given == happy && info.mood == CHR_MOOD_NEUTRAL);
    info = sounding(99, 0, 0);
    CHECK(info.mood == CHR_MOOD_NEUTRAL);
    bool shut = false;
    uint8_t lids = 0;
    unsigned swapped = 0;
    for (unsigned t = 0; t < 400; t += FRAME) {
        const uint8_t was = info.eye_left;
        info = sounding(FRAME, 0, t < 60 ? 0 : 9000);
        shut |= info.eye_left == 4 && info.mood == CHR_MOOD_NEUTRAL;
        if (info.mood == happy && !swapped) {
            swapped = t;
            lids = was;
        }
    }
    /* The face changed in the pause: behind shut eyes, or a flat figure's part by part by fading. */
    if (has(CHR_FLAT)) CHECK(swapped >= 33 && swapped <= 200);
    else CHECK(shut && lids == 4 && swapped >= 33 && swapped <= 200);
    bool open = false;
    for (int i = 0; i < 12; ++i) { info = sounding(FRAME, 0, 9000); open |= info.eye_left == 0; }
    CHECK(open && info.mouth_form == 2);
    /* ... but not for longer than a second or so, and not before the last one has been there for a while. */
    chr_set_mood(c, serious);
    info = sounding(150, 0, 0);
    CHECK(info.mood == happy);
    info = sounding(500, 0, 9000);
    CHECK(info.mood == happy);
    unsigned waited = 650;
    for (; waited < 3000 && info.mood == happy; waited += FRAME) info = sounding(FRAME, 0, 9000);
    CHECK(info.mood == serious && waited >= 800 && waited < 1500);
    info = sounding(800, 0, 0);
    CHECK(info.mouth_form == 1 && info.mouth_open == 0);
    /* A word that has been taken back before it was shown is not shown. */
    (void)sounding(1500, 0, 9000);
    chr_set_mood(c, gentle);
    (void)sounding(200, 0, 9000);
    chr_set_mood(c, serious);
    for (int i = 0; i < 60; ++i) CHECK(sounding(FRAME, 0, i % 20 < 8 ? 0 : 9000).mood == serious);
    /* Nothing but the face: no pose, no marks in the air, and the body where it was. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(2000, 0, 9000);
    static const char *const words[] = {"happy", "excited", "gentle", "comfort", "thinking", "surprised", "serious", "sleepy", "confused"};
    for (size_t k = 0; k < sizeof(words) / sizeof(words[0]); ++k) {
        chr_set_mood(c, chr_mood_find(words[k]));
        for (int i = 0; i < 70; ++i) {
            info = sounding(FRAME, 0, i % 16 < 5 ? 0 : 9000);
            CHECK(info.gesture == CHR_GESTURE_NONE && info.shown == 0 && info.particles == 0);
            if (has(CHR_FLAT)) CHECK(abs(info.moved_x) <= 2 && abs(info.moved_feet) <= 2 && abs(info.moved_top) <= 2);
        }
        CHECK(info.mood == chr_mood_find(words[k]));
    }
    /* Surprised: a round mouth and wide eyes for a moment, then raised brows. A flat figure does not open her eyes
     * wide. */
    chr_set_mood(c, CHR_MOOD_NEUTRAL);
    (void)sounding(3000, 0, 0);
    chr_set_mood(c, surprised);
    info = sounding(500, 0, 0);
    CHECK(info.mood == surprised && (has(CHR_FLAT) ? info.pose[P_EOL] <= 1.0f : info.pose[P_EOL] > 1.16f) && info.mouth_open == 4 &&
          info.mouth_form == 0 && info.pose[P_BROW] > 5.0f);
    info = sounding(1500, 0, 0);
    CHECK(info.mood == surprised && (has(CHR_FLAT) ? info.pose[P_EOL] <= 1.0f : info.target[P_EOL] < 1.05f) && info.mouth_open == 0 &&
          info.pose[P_BROW] > 5.0f);
    /* She keeps it for two seconds after she has finished, then is herself; the same word in the next
     * answer is shown again. */
    chr_set_mood(c, happy);
    (void)sounding(2500, 0, 9000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    info = sounding(300, 0, 9000);           /* still heard */
    info = sounding(1900, 0, 0);
    CHECK(info.mood == happy);
    info = sounding(500, 0, 0);
    CHECK(info.mood == CHR_MOOD_NEUTRAL && info.given == happy);
    (void)sounding(3000, 1400, 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    info = sounding(1500, 0, 9000);
    CHECK(info.mood == happy);
    /* A happy face closes its eyes in a smile now and then (a flat figure's happy face keeps its glad eyes). */
    bool smiled = false, looked = false;
    for (int i = 0; i < 300; ++i) {
        info = sounding(FRAME, 0, 9000);
        smiled |= info.eye_left == 3;
        looked |= info.eye_left == 0;
    }
    CHECK(looked && smiled == !has(CHR_FLAT));
    chr_set_phase(c, CHR_PHASE_IDLE);
    (void)step(FRAME);
}

/* A talk is over for its page before its last words have been heard. */
static void test_parting(void)
{
    const uint8_t happy = (uint8_t)chr_mood_find("happy");
    /* She speaks to the end, in the mood she was in, whatever the page gives her meanwhile; then she waves. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 9000);
    chr_set_mood(c, happy);
    (void)sounding(1800, 0, 9000);
    (void)sounding(400, 0, 0);
    chr_info_t info = sounding(600, 0, 9000);
    CHECK(info.mood == happy && info.mouth_open > 0);
    chr_set_mood(c, CHR_MOOD_NEUTRAL);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 45; ++i) {
        info = sounding(FRAME, 0, i % 14 < 10 ? 9000 : 0);      /* with pauses in it */
        CHECK(info.parting && info.cue != CHR_CUE_BYE && info.gesture == CHR_GESTURE_NONE && info.mood == happy && info.particles <= 3);
        if (i % 14 > 1 && i % 14 < 10) CHECK(info.mouth_open > 0);
    }
    info = sounding(400, 0, 0);
    CHECK(info.parting && info.mouth_open == 0 && info.cue != CHR_CUE_BYE);
    bool waved = false;
    for (int i = 0; i < 12; ++i) {
        info = sounding(FRAME, 0, 0);
        waved |= info.cue == CHR_CUE_BYE;
    }
    CHECK(waved && !info.parting && info.mood == happy);
    info = sounding(1600, 0, 0);
    CHECK(info.mood == CHR_MOOD_NEUTRAL);
    /* The device says that the last sentence is still on its way: she waits for it, however long it is still. */
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(3000, 0, 0);
    chr_set_conditions(c, CHR_PARTING);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 60; ++i) CHECK(sounding(FRAME, 0, 0).cue != CHR_CUE_BYE);
    info = sounding(700, 0, 9000);
    CHECK(info.parting && info.mouth_open > 0 && info.gesture == CHR_GESTURE_NONE);
    info = sounding(900, 0, 0);
    CHECK(info.parting && info.mouth_open == 0 && info.cue != CHR_CUE_BYE);
    chr_set_conditions(c, 0);
    info = sounding(FRAME, 0, 0);
    CHECK(!info.parting && info.cue == CHR_CUE_BYE);
    /* The page may be a moment ahead of the device. */
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(3000, 0, 0);
    chr_set_phase(c, CHR_PHASE_IDLE);
    (void)sounding(100, 0, 0);
    chr_set_conditions(c, CHR_PARTING);
    CHECK(sounding(1000, 0, 0).parting);
    chr_set_conditions(c, 0);
    (void)step(FRAME);
    /* Stopped where she has nothing to say: the goodbye comes at once. */
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(3000, 0, 0);
    chr_set_phase(c, CHR_PHASE_IDLE);
    CHECK(sounding(200, 0, 0).cue == CHR_CUE_BYE);
    /* What the device is in shows when she has said what she had to say. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_set_mood(c, chr_mood_find("angry"));
    (void)sounding(2500, 0, 9000);
    chr_set_mood(c, CHR_MOOD_NEUTRAL);
    chr_set_phase(c, CHR_PHASE_IDLE);
    chr_set_conditions(c, CHR_OFFLINE);
    for (int i = 0; i < 40; ++i) {
        info = sounding(FRAME, 0, 9000);
        CHECK(info.parting && info.target[P_GREY] == 0.0f && info.mood == chr_mood_find("angry"));
        if (has(CHR_FLAT)) CHECK(info.moved_x == 0);        /* nor does she tremble with anger */
    }
    info = sounding(3000, 0, 0);
    CHECK(!info.parting && info.target[P_GREY] > 0.8f);
    chr_set_conditions(c, 0);
    /* A call does not wait, and is not waved at; nor is a new talk, from where it is connected on. */
    static const uint32_t calls[] = {CHR_RINGING, CHR_STARTING};
    for (int k = 0; k < 2; ++k) {
        fresh();
        chr_set_phase(c, CHR_PHASE_SPEAKING);
        (void)sounding(1500, 0, 9000);
        chr_set_phase(c, CHR_PHASE_IDLE);
        CHECK(sounding(300, 0, 9000).parting);
        chr_set_conditions(c, calls[k]);
        for (int i = 0; i < 40; ++i) {
            info = sounding(FRAME, 0, 0);
            CHECK(!info.parting && info.cue != CHR_CUE_BYE);
        }
        chr_set_conditions(c, 0);
    }
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 9000);
    chr_set_phase(c, CHR_PHASE_IDLE);
    (void)sounding(300, 0, 9000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    for (int i = 0; i < 40; ++i) {
        info = sounding(FRAME, 0, 0);
        CHECK(!info.parting && info.cue != CHR_CUE_BYE);
    }
    /* And it does not go on for ever, whatever the device says. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(500, 0, 9000);
    chr_set_conditions(c, CHR_PARTING);
    chr_set_phase(c, CHR_PHASE_IDLE);
    CHECK(sounding(19000, 0, 9000).parting);
    CHECK(!sounding(1500, 0, 9000).parting);
    chr_set_conditions(c, 0);
}

/* Whose mood it is that she shows when a talk is over: the talk's for as long as it is heard and a little
 * longer, then the one she was given to be without a talk. */
static void test_mood_after_talk(void)
{
    const uint8_t happy = (uint8_t)chr_mood_find("happy"), serious = (uint8_t)chr_mood_find("serious"),
                  gentle = (uint8_t)chr_mood_find("gentle"), cool = (uint8_t)chr_mood_find("cool"),
                  loving = (uint8_t)chr_mood_find("loving"), sleepy = (uint8_t)chr_mood_find("sleepy");
    chr_info_t info;
    /* The word of the last sentence comes while the one before it is still being said, and the talk ends
     * before she could show it: she shows it. */
    for (int early = 0; early < 2; ++early) {
        fresh();
        chr_set_phase(c, CHR_PHASE_SPEAKING);
        (void)sounding(800, 0, 9000);
        chr_set_mood(c, serious);
        (void)sounding(2500, 0, 9000);
        chr_set_mood(c, happy);
        info = sounding(300, 0, 9000);
        CHECK(info.mood == serious);
        /* the page forgets the mood in the same view, before or after the phase */
        if (early) chr_set_mood(c, CHR_MOOD_NEUTRAL);
        chr_set_phase(c, CHR_PHASE_IDLE);
        if (!early) chr_set_mood(c, CHR_MOOD_NEUTRAL);
        bool shown = false;
        for (int i = 0; i < 60; ++i) {
            info = sounding(FRAME, 0, i % 20 < 5 ? 0 : 9000);
            CHECK(info.mood == (shown ? happy : info.mood == happy ? happy : serious));
            shown |= info.mood == happy;
        }
        CHECK(shown && info.parting);
        CHECK(sounding(1500, 0, 0).mood == happy);
        CHECK(sounding(1200, 0, 0).mood == CHR_MOOD_NEUTRAL);
    }
    /* Neutral all through a talk: neutral to its end, then the expression she was given to have. It stays, and
     * she makes no show of it. */
    static const bool words[] = {false, true};
    for (int k = 0; k < 2; ++k) {
        fresh();
        chr_set_mood(c, loving);
        info = step(5000);
        CHECK(info.mood == loving);
        chr_set_mood(c, CHR_MOOD_NEUTRAL);
        chr_set_phase(c, CHR_PHASE_SPEAKING);
        (void)sounding(2500, 0, 9000);
        if (words[k]) {
            chr_set_mood(c, gentle);
            (void)sounding(2500, 0, 9000);
        }
        chr_set_mood(c, loving);
        chr_set_phase(c, CHR_PHASE_IDLE);
        for (int i = 0; i < 90; ++i) {
            info = sounding(FRAME, 0, i % 20 < 5 ? 0 : 9000);
            CHECK(info.mood == (words[k] ? gentle : CHR_MOOD_NEUTRAL));
        }
        bool hearts = false;
        unsigned posed = 0;
        for (int i = 0; i < 900; ++i) {
            info = sounding(FRAME, 0, 0);
            posed += info.gesture == CHR_GESTURE_HEART;
            hearts |= info.particles > 0;
            if (i > 80) CHECK(info.mood == loving);
        }
        CHECK(!posed && hearts);
    }
    /* Also when the talk's last word was that same expression; and one that was the talk's alone is over. */
    for (int k = 0; k < 2; ++k) {
        fresh();
        if (k) chr_set_mood(c, cool);
        (void)step(1000);
        chr_set_phase(c, CHR_PHASE_SPEAKING);
        (void)sounding(1500, 0, 9000);
        chr_set_mood(c, happy);
        (void)sounding(2500, 0, 9000);
        chr_set_mood(c, cool);
        (void)sounding(2500, 0, 9000);
        chr_set_phase(c, CHR_PHASE_IDLE);
        (void)sounding(1500, 0, 0);
        info = sounding(3000, 0, 0);
        CHECK(info.mood == (k ? cool : CHR_MOOD_NEUTRAL) && info.given == cool);
    }
    /* A talk is no time to look sleepy, however tired she is; after it she may be. */
    fresh();
    c->energy = 0.2f;
    info = step(1500);
    CHECK(info.mood == sleepy);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    c->energy = 0.2f;
    (void)sounding(1500, 0, 9000);
    for (int i = 0; i < 60; ++i) CHECK(sounding(FRAME, 0, i % 20 < 5 ? 0 : 9000).mood == CHR_MOOD_NEUTRAL);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    for (int i = 0; i < 120; ++i) CHECK(sounding(FRAME, 0, 0).mood == CHR_MOOD_NEUTRAL);
    chr_set_phase(c, CHR_PHASE_IDLE);
    c->energy = 0.2f;
    CHECK(sounding(3000, 0, 0).mood == sleepy);
    /* Not a pose, nor a mark in the air, when a talk ends right after a word that would bring one. */
    static const char *const strong[] = {"confused", "embarrassed", "surprised", "angry", "thinking"};
    for (size_t k = 0; k < sizeof(strong) / sizeof(strong[0]); ++k) {
        fresh();
        chr_set_phase(c, CHR_PHASE_SPEAKING);
        (void)sounding(1500, 0, 9000);
        chr_set_mood(c, chr_mood_find(strong[k]));
        (void)sounding(150, 0, 0);
        chr_set_mood(c, CHR_MOOD_NEUTRAL);
        chr_set_phase(c, CHR_PHASE_IDLE);
        for (int i = 0; i < 80; ++i) {
            info = sounding(FRAME, 0, i < 20 ? 9000 : 0);
            CHECK((info.gesture == CHR_GESTURE_NONE || info.gesture == CHR_GESTURE_WAVE) && info.particles == 0);
        }
    }
}

/* The capsules beside her head: on both sides alike, dots that keep still while nobody speaks, upright ones that run
 * while somebody does, blue in a talk and green in a call, and none else. */
typedef struct { unsigned left, right; float tallest; int strongest, weakest; uint32_t colour; } beside_t;

static beside_t beside(void)
{
    beside_t b = {.weakest = 99};
    for (unsigned n = 0; n < c->item_count; ++n) {
        const chr_shape_t *s = &c->items[n].as.shape;
        if (c->items[n].sprite || s->kind != CHR_STADIUM || s->a[0] != s->a[2] || s->top == s->bottom) continue;
        const float x = s->a[0] - c->anchor_x - (float)c->fig.face_x, tall = s->a[3] - s->a[1];
        ++*(x < 0 ? &b.left : &b.right);
        CHECK(fabsf(x) > (float)c->fig.head_half);
        if (tall > b.tallest) b.tallest = tall;
        if (s->alpha > b.strongest) b.strongest = s->alpha;
        if (s->alpha < b.weakest) b.weakest = s->alpha;
        b.colour = s->bottom;
        if (x < 0) {
            /* its like on the other side */
            bool like = false;
            for (unsigned m = 0; m < c->item_count; ++m) {
                const chr_shape_t *o = &c->items[m].as.shape;
                like |= !c->items[m].sprite && o->kind == CHR_STADIUM && fabsf(o->a[0] - c->anchor_x - (float)c->fig.face_x + x) < 0.01f &&
                        o->a[1] == s->a[1] && o->a[3] == s->a[3] && o->alpha == s->alpha;
            }
            CHECK(like);
        }
    }
    return b;
}

static void test_voice_beside(void)
{
    fresh();
    (void)step(1000);
    CHECK(beside().left + beside().right == 0);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(1500, 0, 0);
    beside_t b = beside();
    CHECK(b.left == 5 && b.right == 5 && b.tallest == 0.0f && b.strongest > 2 * b.weakest && b.weakest > 0);
    const uint32_t dots = c->groups[G_FX].signature, others = c->groups[G_FX_OTHER].signature;
    for (int i = 0; i < 60; ++i) {
        (void)sounding(FRAME, 0, 0);
        CHECK(c->groups[G_FX].signature == dots && c->groups[G_FX_OTHER].signature == others);
    }
    /* A room that is not quite still is no voice. */
    (void)sounding(800, 75, 0);
    CHECK(beside().tallest == 0.0f);
    const uint32_t talk = b.colour;
    float most = 0.0f, least = 1000.0f;
    unsigned drawn = 0;
    uint32_t was = 0;
    for (int i = 0; i < 90; ++i) {
        (void)sounding(FRAME, 1400, 0);
        if (i < 12) continue;
        b = beside();
        if (b.tallest > most) most = b.tallest;
        if (b.tallest < least) least = b.tallest;
        drawn += c->groups[G_FX].signature != was;
        was = c->groups[G_FX].signature;
    }
    CHECK(most > 30.0f && most <= 46.0f && least > 10.0f);
    CHECK(drawn > 20 && drawn < 60);                 /* a wave, drawn about sixteen times a second and not at every step */
    (void)sounding(1500, 0, 0);
    CHECK(beside().tallest == 0.0f);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 9000);
    CHECK(beside().left + beside().right == 0);
    chr_set_phase(c, CHR_PHASE_IDLE);
    (void)sounding(4000, 0, 0);
    chr_set_conditions(c, CHR_IN_CALL);
    (void)sounding(1000, 1400, 0);
    b = beside();
    CHECK(b.left == 5 && b.right == 5 && b.tallest > 10.0f && b.colour != talk);
    chr_set_conditions(c, 0);
    (void)step(200);
    /* The volume: three steps on one side, lit up to it. */
    chr_cue(c, CHR_CUE_VOLUME, 50);
    (void)step(400);
    b = beside();
    CHECK(b.left == 0 && b.right == 3 && b.strongest > 20 && b.weakest < 10);
    (void)step(1500);
}

/* A talk is had with the face: a pose only when nothing has been said for long, or an answer takes long. */
static void test_talk_poses(void)
{
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(1500, 0, 0);
    chr_info_t info;
    for (int i = 0; i < 120; ++i) CHECK(sounding(FRAME, 0, 0).gesture == CHR_GESTURE_NONE);
    info = sounding(FRAME, 1400, 0);                    /* a word starts the wait again */
    for (int i = 0; i < 170; ++i) CHECK(sounding(FRAME, 0, 0).gesture == CHR_GESTURE_NONE);
    info = sounding(1200, 0, 0);
    CHECK(info.gesture == CHR_GESTURE_LISTEN);
    if (has(CHR_HAS_POSES) && chr_pack_slot(&c->pack, SL_POSE + CHR_GESTURE_LISTEN - 1U) != CHR_NO_SPRITE) CHECK(info.shown == CHR_GESTURE_LISTEN);
    CHECK(sounding(800, 1400, 0).gesture == CHR_GESTURE_LISTEN);      /* and she stays with you while you speak */
    chr_set_phase(c, CHR_PHASE_THINKING);
    CHECK(sounding(2900, 0, 0).gesture == CHR_GESTURE_NONE);     /* a hand to her chin after three seconds */
    CHECK(sounding(400, 0, 0).gesture == CHR_GESTURE_THINK);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    info = sounding(900, 0, 9000);
    CHECK(info.gesture == CHR_GESTURE_NONE && info.shown == 0 && info.mouth_open > 0);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    CHECK(sounding(900, 0, 9000).gesture == CHR_GESTURE_NONE);
    chr_set_phase(c, CHR_PHASE_IDLE);
    (void)step(FRAME);
}

/* A figure in one piece keeps its place: two pixels either way as she breathes, speaks and listens, a few
 * more in a reaction; into another picture she goes with her eyes shut, with a small dip and no bounce. */
static void test_still(void)
{
    fresh();
    if (!has(CHR_FLAT)) return;
    chr_info_t info;
    int left = 0, right = 0, up = 0, down = 0;
    unsigned redrawn = 0;
    chr_rect_t damage[CHR_DAMAGE_MAX];
    for (int i = 0; i < 2400; ++i) {                  /* eighty seconds of idle life, small acts included */
        redrawn += chr_step(c, FRAME, damage) > 0;
        chr_inspect(c, &info);
        if (info.moved_x < left) left = info.moved_x;
        if (info.moved_x > right) right = info.moved_x;
        if (info.moved_top < up) up = info.moved_top;
        if (info.moved_top > down) down = info.moved_top;
        CHECK(info.shown != 0 || (abs(info.moved_x) <= 2 && abs(info.moved_feet) <= 2));
    }
    CHECK(left >= -2 && right <= 2 && up == -2 && down == 2);
    CHECK(redrawn < 2400);
    /* Breathing: up and down once in a little over four seconds, two pixels either way. */
    fresh();
    info = step(1500);
    unsigned turns = 0;
    int last = 0, at = 0;
    for (int i = 0; i < 1000 && info.cue == CHR_CUE_NONE; ++i) {
        info = step(FRAME);
        if (info.cue) break;
        at = info.moved_top;
        CHECK(abs(at) <= 2 && info.moved_x == 0 && info.moved_feet == 0);
        if (at == -2 && last != -2) ++turns;
        if (at == -2 || at == 2) last = at;
    }
    CHECK(turns >= 1);
    /* She breathes on from where she is, however her eyes are: no step takes her further than breathing does. */
    for (unsigned k = 0; k < 48; ++k) {
        fresh();
        (void)step(1000 + k * 91);
        int top = step(FRAME).moved_top;
        for (int i = 0; i < 90; ++i) {
            if (i == 3) chr_set_mood(c, chr_mood_find("sleepy"));
            if (i == 45) chr_set_mood(c, CHR_MOOD_NEUTRAL);
            if (i == 50) chr_set_phase(c, CHR_PHASE_SPEAKING);
            if (i == 52) chr_set_mood(c, chr_mood_find("sleepy"));
            if (i == 80) chr_set_phase(c, CHR_PHASE_LISTENING);
            info = sounding(FRAME, 0, 0);
            if (info.cue == CHR_CUE_NONE && !info.shown) CHECK(abs(info.moved_top - top) <= 1);
            top = info.moved_top;
        }
    }
    /* Listening, her eyes are her own (no wider), and she blinks no more than she does, also where steps are long. */
    for (unsigned dt = 80; dt <= 120; dt += 10) {
        fresh();
        chr_set_phase(c, CHR_PHASE_LISTENING);
        unsigned blinks = 0;
        float lids = 0.0f;
        for (unsigned t = 0; t < 20000; t += dt) {
            (void)paced(dt, dt, 0);
            if (chr_lids(c) > 0.3f && lids <= 0.3f) ++blinks;
            lids = chr_lids(c);
            if (t > 600) CHECK(c->eyes_shown == LOOK_PLAIN);
        }
        CHECK(blinks < 8);
    }
    fresh();
    (void)step(1200);
    /* Speaking and listening: nothing but breathing. */
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(1500, 0, 0);
    for (int i = 0; i < 100; ++i) { info = sounding(FRAME, i % 9 < 4 ? 1400 : 0, 0); CHECK(abs(info.moved_x) <= 1 && abs(info.moved_feet) <= 1 && abs(info.moved_top) <= 2); }
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(600, 0, 0);
    unsigned faces = 0, wholes = 0;
    for (int i = 0; i < 300; ++i) {
        chr_set_envelope(c, 0, i % 7 < 4 ? 9000 : 1500);
        const size_t count = chr_step(c, FRAME, damage);
        chr_inspect(c, &info);
        CHECK(info.moved_x == 0 && info.moved_feet == 0 && abs(info.moved_top) <= 2);
        for (size_t k = 0; k < count; ++k) {
            if (damage[k].height > 400) ++wholes;
            else ++faces;
        }
    }
    CHECK(faces > 60 && wholes < 30);                  /* a syllable redraws the face; all of her, with her breath */
    chr_set_phase(c, CHR_PHASE_IDLE);
    /* Reactions: none takes her further than a few pixels. */
    for (int cue = 1; cue < CHR_CUE_COUNT; ++cue) {
        fresh();
        (void)step(1200);
        chr_cue(c, (chr_cue_t)cue, cue == CHR_CUE_VOLUME ? 60 : 1);
        chr_inspect(c, &info);
        int feet = info.moved_feet, top = info.moved_top;
        for (int i = 0; i < 90; ++i) {
            info = step(FRAME);
            if (cue != CHR_CUE_DANCE) CHECK(abs(info.moved_top - top) <= (cue == CHR_CUE_JUMP ? 5 : 3) && abs(info.moved_feet - feet) <= (cue == CHR_CUE_JUMP ? 5 : 2));
            top = info.moved_top;
            feet = info.moved_feet;
            const int side = cue == CHR_CUE_DANCE ? 4 : 2, lift = cue == CHR_CUE_DANCE ? 10 : 6, tall = cue == CHR_CUE_DANCE ? 24 : 12;
            CHECK(abs(info.moved_x) <= side && abs(info.moved_feet) <= lift && abs(info.moved_top) <= tall);
        }
    }
    /* Into a pose: eyes shut first, a dip of a hundredth, never past her height; back, they open from shut. */
    fresh();
    (void)step(1200);
    chr_cue(c, CHR_CUE_OFFER, 1);
    float deepest = 0.0f, highest = 0.0f, wide, tall, lift;
    bool posed = false, shut = false, opened = false;
    uint8_t eyes = 0, first = 9;
    for (int i = 0; i < 120; ++i) {
        info = step(FRAME);
        chr_change_shape(c, &wide, &tall, &lift);
        if (tall < deepest) deepest = tall;
        if (tall > highest) highest = tall;
        CHECK(lift == 0.0f);
        if (info.shown && !posed) {
            posed = true;
            shut = eyes == 4;
        }
        if (posed && !info.shown && first == 9) first = info.eye_left;
        if (posed && !info.shown && info.eye_left == 0) opened = true;
        eyes = info.eye_left;
    }
    CHECK(posed && shut && first == 4 && opened && deepest == 0.0f && highest == 0.0f);      /* her eyes close, she does not dip */
    /* A pose that is led into by a picture of its own (an arm half way up): her eyes close, that picture shows for a
     * moment, then the pose; and the same on the way out. She does not dip. */
    chr_clip_t clip;
    if (has(CHR_FLAT)) CHECK(chr_pack_clip(&c->pack, CHR_GESTURE_STRETCH, &clip));
    if (!chr_pack_clip(&c->pack, CHR_GESTURE_STRETCH, &clip) || clip.enter != 1 || clip.leave != 1) return;
    fresh();
    (void)step(1200);
    chr_cue(c, CHR_CUE_STRETCH, 0);
    unsigned lead_in = 0, lead_out = 0;
    bool out = false;
    uint8_t after = 9;
    for (int i = 0; i < 160; ++i) {
        const uint8_t eye = info.eye_left;
        info = step(FRAME);
        chr_change_shape(c, &wide, &tall, &lift);
        CHECK(lift == 0.0f && tall == 0.0f && wide == 0.0f);
        if (info.shown == CHR_GESTURE_STRETCH && info.frame == 0) {
            if (!lead_in) CHECK(eye == 4);                  /* she went with her eyes shut */
            lead_in += FRAME;
        }
        if (info.shown == CHR_GESTURE_STRETCH && info.frame == clip.enter + clip.loop) {
            out = true;
            lead_out += FRAME;
        }
        if (out && !info.shown && after == 9) after = info.eye_left;
    }
    CHECK(lead_in >= 66 && lead_in <= 140 && lead_out >= 66 && lead_out <= 140 && after == 4);
}

/* One change after another, at whatever moment the next begins: her shape goes on from where it is, her lids
 * do not flutter, the base picture does not flash between two poses, and nothing stays half way. */
static void test_changes(void)
{
    static const struct { chr_cue_t first, second; } pairs[] = {
        {CHR_CUE_JUMP, CHR_CUE_JUMP}, {CHR_CUE_CHEER, CHR_CUE_NOD}, {CHR_CUE_NOD, CHR_CUE_CHEER},
        {CHR_CUE_OFFER, CHR_CUE_STRETCH}, {CHR_CUE_CHEER, CHR_CUE_GREET}, {CHR_CUE_HIGHFIVE, CHR_CUE_STRETCH},
        {CHR_CUE_POUT, CHR_CUE_CHEER},
    };
    fresh();
    if (!has(CHR_FLAT)) return;
    for (size_t k = 0; k < sizeof(pairs) / sizeof(pairs[0]); ++k) {
        for (unsigned after = 100; after < 3000; after += 50) {
            fresh();
            chr_cue(c, pairs[k].first, 0);
            float was = 0.0f, lids = 0.0f, wide, tall, lift;
            unsigned base = 1000, passing = 0, back = 0;
            for (unsigned t = 0; t < 6400; t += 25) {
                if (t == after) {
                    chr_cue(c, pairs[k].second, 0);
                    if (pairs[k].second == pairs[k].first && c->leaving) back = t + 400;
                }
                const chr_info_t info = paced(25, 25, 0);
                if (t < back) CHECK(info.shown);            /* wanted back on the way out: she turns round */
                chr_change_shape(c, &wide, &tall, &lift);
                CHECK(fabsf(tall - was) < 0.0045f && tall <= 0.0f && lift == 0.0f);
                /* Half way: eyes shut, an arm half up, down in the dip. */
                passing = info.shown && tall < -0.0095f ? passing + 25 : 0;
                CHECK(passing <= 400);
                /* The lids close in 70 ms; they open no faster than in a hundred. */
                const float now = info.shown ? 0.0f : chr_lids(c);
                if (!info.shown && base) CHECK(now - lids < 0.6f && lids - now < 0.42f);
                lids = now;
                if (info.shown) {
                    CHECK(base == 0 || base >= 175);        /* straight from one pose to the next, or by way of herself */
                    base = 0;
                } else base += 25;
                was = tall;
            }
            CHECK(!c->pose && c->change_ms < 0.0f);
        }
    }
    /* Woken from a doze she stretches, without showing her waking face in between; the same when a hand is asked
     * for while she listens closely. */
    for (int k = 0; k < 2; ++k) {
        fresh();
        if (k == 0) {
            c->energy = 0.1f;
            c->dozing = true;
            CHECK(step(2500).shown == CHR_GESTURE_SLEEP);
            tap(cheek_x(), cheek_y());
        } else {
            chr_set_phase(c, CHR_PHASE_LISTENING);
            CHECK(sounding(8500, 0, 0).shown == CHR_GESTURE_LISTEN);
            chr_cue(c, CHR_CUE_OFFER, 1);
        }
        const unsigned first = c->pose;
        float was, wide, tall, lift;
        chr_change_shape(c, &wide, &was, &lift);
        unsigned bare = 0;
        for (unsigned t = 0; t < 1200; t += 25) {
            const chr_info_t info = paced(25, 25, 0);
            chr_change_shape(c, &wide, &tall, &lift);
            CHECK(fabsf(tall - was) < 0.0045f);
            was = tall;
            bare += info.shown == 0;
        }
        CHECK(bare == 0 && c->pose == (k ? CHR_GESTURE_HIGHFIVE : CHR_GESTURE_STRETCH) && c->pose != first);
    }
    /* A shrug is a tilted head here: wanted back on the way out of it, she turns round, and does not stay there. */
    if (chr_pack_slot(&c->pack, SL_POSE + CHR_GESTURE_SHRUG - 1U) == CHR_NO_SPRITE) {
        for (unsigned hold = 25; hold <= 250; hold += 25) {
            fresh();
            chr_set_mood(c, chr_mood_find("confused"));
            chr_info_t info = step(900);
            CHECK(info.shown == CHR_GESTURE_TILT);
            unsigned same = 0, leaving = 0;
            uint16_t picture = c->picture;
            for (unsigned t = 0; t < 3000; t += 25) {
                if (t < hold) (void)chr_pointer(c, true, (int16_t)(W / 2 - 420), (int16_t)(H - 40));
                else if (t < hold + 25) (void)chr_pointer(c, false, 0, 0);
                info = paced(25, 25, 0);
                same = c->picture == picture ? same + 25 : 0;
                picture = c->picture;
                leaving = c->pose && c->leaving ? leaving + 25 : 0;
                CHECK(leaving <= 250);
            }
            CHECK(info.shown == 0);
        }
    }
    /* The end of a dance: the last picture is there to be seen, and she settles from the beat she is in. */
    for (unsigned dt = 24; dt <= 48; dt += 8) {
        fresh();
        chr_cue(c, CHR_CUE_DANCE, 0);
        float was = 0.0f, wide, tall, lift;
        unsigned since = 0;
        bool over = false;
        uint16_t picture = CHR_NO_SPRITE;
        for (unsigned t = 0; t < 12500; t += dt) {
            const chr_info_t info = paced(dt, dt, 0);
            chr_change_shape(c, &wide, &tall, &lift);
            since += dt;
            if (c->picture != picture) {
                if (c->picture == CHR_NO_SPRITE) {
                    CHECK(since >= 250 && !over);
                    over = true;
                }
                picture = c->picture;
                since = 0;
            } else if (info.gesture != CHR_GESTURE_DANCE && t > 8000) CHECK(fabsf(tall - was) < 0.0005f * (float)dt);
            was = tall;
        }
        CHECK(!c->pose && over);
    }
}

/* Whether the picture in this slot is drawn in the last step. */
static bool drawn(unsigned slot)
{
    const unsigned id = chr_pack_slot(&c->pack, slot);
    for (unsigned n = 0; id != CHR_NO_SPRITE && n < c->item_count; ++n)
        if (c->items[n].sprite && c->items[n].as.blit.id == id) return true;
    return false;
}

/* A flat figure with glad eyes and pictures of effects is a composed young lady: she looks at you, her lids go one
 * rung at a time, a mood comes part by part without a blink (lightly, then fully), her cheeks colour slowly, a pose
 * picture comes by a slow blink without a dip and stays, one a round of a talk, and what happens around her stays off
 * her face. */
static void test_lady(void)
{
    if (!has(CHR_FLAT) || chr_pack_slot(&c->pack, SL_EYE_L_GLAD) == CHR_NO_SPRITE || !chr_fx_drawn(c, FX_SPARK)) return;
    const unsigned aside[] = {SL_HEAD + H_EYE_L, SL_HEAD + H_EYE_L + 2U, SL_HEAD + H_EYE_R, SL_HEAD + H_EYE_R + 2U,
                              SL_EYE_L_WIDE, SL_EYE_R_WIDE, SL_EYE_L_UP, SL_EYE_R_UP};
    /* A minute and a half of her own life: she looks ahead, never with eyes to one side, wide or up; she does not hop;
     * her lids go up and down one rung at a time and stay at least 60 ms on each. */
    fresh();
    chr_info_t info;
    uint8_t rung = c->rung[0];
    unsigned held = 1000, blinks = 0, acts = 0, changed = 0;
    for (int i = 0; i < 2700; ++i) {
        info = step(FRAME);
        CHECK(info.gaze == 1 && info.cue != CHR_CUE_HOP);
        acts += info.cue != CHR_CUE_NONE;
        for (size_t k = 0; k < sizeof(aside) / sizeof(aside[0]); ++k)
            if (chr_pack_slot(&c->pack, aside[k]) != CHR_NO_SPRITE) CHECK(!drawn(aside[k]));
        /* (Back from a pose picture her own eyes start shut.) */
        changed = info.changing || info.shown ? 0 : changed + 1;
        if (c->rung[0] != rung) {
            if (changed > 2) CHECK(abs((int)c->rung[0] - (int)rung) == 1 && held >= 60);
            blinks += c->rung[0] == RUNG_SHUT;
            rung = c->rung[0];
            held = 0;
        }
        held += FRAME;
    }
    CHECK(blinks >= 10 && blinks <= 35 && acts > 0);

    /* A mood comes without a blink: lightly (her own eyes, a little less blush), fully after a second and a half (soft
     * eyes), each part fading over a few frames; her cheeks colour over some 0.4 s. */
    fresh();
    chr_hold(c, true);
    c->blink_in = 100000.0f;
    chr_set_mood(c, chr_mood_find("happy"));
    float blush_at[16];
    for (int i = 0; i < 16; ++i) {
        info = step(FRAME);
        CHECK(c->rung[0] == RUNG_OPEN && info.mood == chr_mood_find("happy") && !drawn(SL_EYE_L_SOFT));
        blush_at[i] = info.pose[P_BLUSH];
    }
    CHECK(blush_at[3] > 0.05f && blush_at[3] < 0.15f && blush_at[15] > 0.15f && blush_at[15] <= 0.25f * 0.7f + 0.01f);
    (void)step(1000);
    bool fading = false;
    for (int i = 0; i < 20 && !drawn(SL_EYE_L_SOFT); ++i) (void)step(FRAME);
    for (unsigned n = 0; n < c->item_count; ++n)
        fading |= c->items[n].sprite && c->items[n].as.blit.id == chr_pack_slot(&c->pack, SL_EYE_L_SOFT) && c->items[n].as.blit.alpha < 32;
    CHECK(drawn(SL_EYE_L_SOFT) && fading);
    info = step(400);
    CHECK(drawn(SL_EYE_L_SOFT) && info.pose[P_BLUSH] > 0.22f);
    /* Going, a full mood shows lightly for a moment first; her eyes go back to her own without a blink. */
    chr_set_mood(c, CHR_MOOD_NEUTRAL);
    info = step(231);
    CHECK(info.mood == chr_mood_find("happy") && !drawn(SL_EYE_L_SOFT) && c->rung[0] == RUNG_OPEN);
    info = step(200);
    CHECK(info.mood == CHR_MOOD_NEUTRAL);
    /* Then her cheeks pale, more slowly than they coloured. */
    float before = info.pose[P_BLUSH];
    info = step(264);
    CHECK(info.pose[P_BLUSH] < before - 0.05f && info.pose[P_BLUSH] > 0.2f * before);
    /* Hurt: eyes cast down first, the corners of her mouth a little later. */
    fresh();
    chr_hold(c, true);
    c->blink_in = 100000.0f;
    chr_set_mood(c, chr_mood_find("sad"));
    unsigned eyes_at = 0, mouth_at = 0;
    for (unsigned t = FRAME; t < 600; t += FRAME) {
        (void)step(FRAME);
        if (!eyes_at && drawn(SL_EYE_L_DOWN)) eyes_at = t;
        if (!mouth_at && drawn(SL_MOUTH_FROWN)) mouth_at = t;
    }
    CHECK(eyes_at && mouth_at && mouth_at >= eyes_at + 99 && drawn(SL_BROWS_COMFORT));

    /* Shy, then herself again: her cheeks pale in 0.8 s from where they began to; back on the screen they are as they
     * should be at once. */
    fresh();
    chr_hold(c, true);
    chr_set_mood(c, chr_mood_find("embarrassed"));
    info = step(3000);
    CHECK(info.pose[P_BLUSH] > 0.45f);
    chr_set_mood(c, CHR_MOOD_NEUTRAL);
    unsigned pale = 0;
    for (unsigned t = FRAME; t < 1500 && !pale; t += FRAME)
        if (step(FRAME).pose[P_BLUSH] < 0.001f) pale = t;
    CHECK(pale >= 700 && pale <= 950);
    chr_set_mood(c, chr_mood_find("embarrassed"));
    (void)step(3000);
    chr_set_mood(c, CHR_MOOD_NEUTRAL);
    chr_return(c, false);
    CHECK(step(FRAME).pose[P_BLUSH] < 0.001f);

    /* Back from a pose picture (hands clasped a moment), her own eyes are shut, then open. */
    fresh();
    chr_cue(c, CHR_CUE_GLANCE, 1);
    bool clasped = false, back = false;
    for (int i = 0; i < 150 && !back; ++i) {
        info = step(FRAME);
        clasped |= info.shown != 0;
        if (clasped && !info.shown) {
            back = true;
            CHECK(drawn(SL_HEAD + H_EYE_L + 8U) && drawn(SL_HEAD + H_EYE_R + 8U));
        }
    }
    CHECK(clasped && back);

    /* In a talk a mood comes in a pause: the eyes first, the mouth a moment later, by fading, not at once. (Sad eyes in
     * a talk are soft ones, open: see below.) */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(2000, 0, 9000);
    c->blink_in = 100000.0f;
    chr_set_mood(c, chr_mood_find("sad"));
    (void)sounding(300, 0, 9000);
    unsigned eyes_down = 0, frown = 0;
    for (unsigned t = FRAME; t < 900; t += FRAME) {
        (void)sounding(FRAME, 0, 0);
        if (!eyes_down && drawn(SL_EYE_L_SOFT)) eyes_down = t;
        if (!frown && drawn(SL_MOUTH_FROWN)) {
            frown = t;
            for (unsigned n = 0; n < c->item_count; ++n)
                if (c->items[n].sprite && c->items[n].as.blit.id == chr_pack_slot(&c->pack, SL_MOUTH_FROWN)) CHECK(c->items[n].as.blit.alpha < 32);
        }
    }
    CHECK(eyes_down && frown >= eyes_down + 99);
    chr_set_phase(c, CHR_PHASE_IDLE);
    (void)step(3000);

    /* In a talk (listening, thinking, speaking) her eyes stay open whatever the mood, and shut only to blink: no smile
     * closes them, no glad, lowered or serious eyes narrow them. Only sleepiness she is given half closes them, and a
     * wink is a blink of one eye; at rest the glad eyes are hers again. */
    static const char *const narrowing[] = {"happy", "laughing", "loving", "delicious", "cool", "relaxed", "kissy",
                                            "sad", "angry", "embarrassed", "serious", "comfort", "silly", "crying"};
    static const chr_phase_t phases[] = {CHR_PHASE_LISTENING, CHR_PHASE_THINKING, CHR_PHASE_SPEAKING};
    for (size_t k = 0; k < sizeof(narrowing) / sizeof(narrowing[0]); ++k) {
        fresh();
        chr_set_phase(c, phases[k % 3]);
        (void)sounding(500, 0, 9000);
        c->blink_in = 100000.0f;
        chr_set_mood(c, chr_mood_find(narrowing[k]));
        unsigned open = 0, frames = 0;
        for (unsigned t = 0; t < 4000; t += FRAME) {
            (void)sounding(FRAME, 0, t % 1300 < 900 ? 9000 : 0);
            if (c->pose || c->change_ms >= 0.0f) continue;     /* (into a pose she goes by a slow blink) */
            ++frames;
            open += c->rung[0] == RUNG_OPEN && c->rung[1] == RUNG_OPEN && !drawn(SL_EYE_L_GLAD) && !drawn(SL_EYE_L_DOWN) &&
                    !drawn(SL_EYE_L_SERIOUS) && !drawn(SL_EYE_L_COMFORT);
        }
        /* (Thinking a while, she is in her pose of thought.) */
        CHECK(open == frames && (frames > 60 || phases[k % 3] == CHR_PHASE_THINKING));
    }
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(500, 0, 9000);
    c->blink_in = 100000.0f;
    chr_set_mood(c, chr_mood_find("sleepy"));
    for (unsigned t = 0; t < 3000; t += FRAME) (void)sounding(FRAME, 0, t % 1300 < 900 ? 9000 : 0);
    CHECK(c->rung[0] == RUNG_HALF && c->rung[1] == RUNG_HALF);
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(500, 0, 9000);
    c->blink_in = 100000.0f;
    chr_set_mood(c, chr_mood_find("winking"));
    bool winked = false;
    unsigned wink_ms = 0;
    for (unsigned t = 0; t < 4000; t += FRAME) {
        (void)sounding(FRAME, 0, t % 1300 < 900 ? 9000 : 0);
        winked |= c->rung[0] == RUNG_OPEN && c->rung[1] == RUNG_SHUT;
        wink_ms += c->rung[1] != RUNG_OPEN ? FRAME : 0U;
    }
    CHECK(winked && wink_ms < 900 && c->rung[0] == RUNG_OPEN && c->rung[1] == RUNG_OPEN);
    fresh();
    chr_set_mood(c, chr_mood_find("delicious"));
    (void)step(2500);
    CHECK(drawn(SL_EYE_L_GLAD));

    /* Surprised: one slow blink, a little round mouth for 0.6 s, raised brows; her eyes are never wider than her own. */
    fresh();
    chr_hold(c, true);
    chr_set_mood(c, chr_mood_find("surprised"));
    unsigned shut = 0, round = 0;
    for (unsigned t = FRAME; t < 1500; t += FRAME) {
        info = step(FRAME);
        CHECK(info.pose[P_EOL] <= 1.0f && info.pose[P_EOR] <= 1.0f);
        shut += c->rung[0] == RUNG_SHUT;
        round += info.mouth_open == 4 && info.mouth_form == 0;
        if (t > 900) CHECK(info.mouth_open == 0 && !info.shown && drawn(SL_BROWS));
    }
    CHECK(shut >= 1 && shut <= 4 && round >= 12 && round <= 22);

    /* A pose picture comes and goes by a slow blink, never by a dip, and stays two seconds at least. Listening long, her
     * hands are clasped; in that round of the talk she goes into no other pose. */
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    unsigned posed = 0, thought = 0;
    for (int i = 0; i < 260; ++i) {
        float wide, tall, lift;
        info = sounding(FRAME, 0, 0);
        chr_change_shape(c, &wide, &tall, &lift);
        CHECK(wide == 0.0f && tall == 0.0f && lift == 0.0f);
        if (i == 230) chr_set_phase(c, CHR_PHASE_THINKING);
        posed += info.shown == CHR_GESTURE_LISTEN;
    }
    for (int i = 0; i < 120; ++i) {
        info = sounding(FRAME, 0, 0);
        thought += info.shown == CHR_GESTURE_THINK;
    }
    CHECK(posed * FRAME >= 1000 && thought == 0);
    /* The next round may: thinking three seconds, a hand to her chin, put down as the first sound of her answer is
     * heard (not as it is sent), however soon that is. */
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(1000, 0, 0);
    chr_set_phase(c, CHR_PHASE_THINKING);
    thought = 0;
    for (int i = 0; i < 85; ++i) thought += sounding(FRAME, 0, 0).shown == CHR_GESTURE_THINK;
    CHECK(thought == 0);
    for (int i = 0; i < 35; ++i) thought += sounding(FRAME, 0, 0).shown == CHR_GESTURE_THINK;
    CHECK(thought > 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    unsigned after = 0;
    for (int i = 0; i < 120; ++i) after += sounding(FRAME, 0, 9000).shown == CHR_GESTURE_THINK;
    CHECK(after * FRAME <= 300);                                /* the slow blink that takes her out of it */
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(1000, 0, 0);
    chr_set_phase(c, CHR_PHASE_THINKING);
    (void)sounding(4000, 0, 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    thought = 0;
    for (int i = 0; i < 45; ++i) thought += sounding(FRAME, 0, 0).shown == CHR_GESTURE_THINK;
    CHECK(thought == 45);                                       /* sent, but nothing heard yet */
    unsigned put_down = 0;
    for (int i = 0; i < 30 && !put_down; ++i)
        if (sounding(FRAME, 0, 9000).gesture != CHR_GESTURE_THINK) put_down = (unsigned)(i + 1);
    CHECK(put_down == 1);
    chr_set_phase(c, CHR_PHASE_IDLE);
    (void)sounding(3000, 0, 0);

    /* Greeted: a glow behind her (drawn before anything of her), sparkles and petals, never more than there is room
     * for, and nothing but a ring over her face. */
    fresh();
    chr_cue(c, CHR_CUE_GREET, 0);
    bool glowed = false;
    unsigned most = 0;
    for (int i = 0; i < 120; ++i) {
        info = step(FRAME);
        if (info.particles > most) most = info.particles;
        const chr_figure_t *f = &c->fig;
        for (unsigned n = 0; n < c->item_count; ++n) {
            const chr_item_t *item = &c->items[n];
            if (!item->sprite || item->group < G_FX_UL || item->group > G_FX_DR) continue;
            const float x = 0.5f * (float)(item->as.blit.x0 + item->as.blit.x1) - c->anchor_x;
            const float y = 0.5f * (float)(item->as.blit.y0 + item->as.blit.y1) - c->anchor_y;
            bool ring = false;
            for (unsigned k = 0; k < CHR_FX_FRAMES; ++k)
                ring |= item->as.blit.id == chr_pack_slot(&c->pack, SL_FX + FX_RING_GOLD * CHR_FX_FRAMES + k) ||
                        item->as.blit.id == chr_pack_slot(&c->pack, SL_FX + FX_RING_SKY * CHR_FX_FRAMES + k);
            if (!ring) CHECK(!(fabsf(x - (float)f->face_x) < (float)f->head_half - 2.0f && y > f->brow_y - 28 && y < f->neck_y + 8));
            CHECK(y >= chr_mark_top(c) - 1.0f);       /* nothing above the top of her hair: that is the page's bar */
        }
        if (c->item_count && c->items[0].group == G_AURA) glowed = true;
    }
    CHECK(glowed && most >= 10 && most <= 16);

    /* Marks that belong to her head are on the viewer's left of it, under the top of her hair: thinking, waiting for
     * the network, offline, sleepy, surprised. */
    static const struct { const char *mood; uint8_t phase; uint32_t conditions; } marked[] = {
        {"neutral", CHR_PHASE_THINKING, 0}, {"neutral", CHR_PHASE_IDLE, CHR_CONNECTING}, {"neutral", CHR_PHASE_IDLE, CHR_OFFLINE},
        {"sleepy", CHR_PHASE_IDLE, 0}, {"surprised", CHR_PHASE_IDLE, 0}, {"confused", CHR_PHASE_IDLE, 0},
    };
    for (size_t k = 0; k < sizeof(marked) / sizeof(marked[0]); ++k) {
        fresh();
        chr_hold(c, true);
        chr_set_conditions(c, marked[k].conditions);
        chr_set_phase(c, (chr_phase_t)marked[k].phase);
        chr_set_mood(c, chr_mood_find(marked[k].mood));
        unsigned seen = 0;
        for (int i = 0; i < 200; ++i) {
            (void)step(FRAME);
            for (unsigned n = 0; n < c->item_count; ++n) {
                const chr_item_t *item = &c->items[n];
                if (item->group != G_FX && (item->group < G_FX_UL || item->group > G_FX_DR)) continue;
                const int16_t x0 = item->sprite ? item->as.blit.x0 : item->as.shape.x0, x1 = item->sprite ? item->as.blit.x1 : item->as.shape.x1;
                const int16_t y0 = item->sprite ? item->as.blit.y0 : item->as.shape.y0, y1 = item->sprite ? item->as.blit.y1 : item->as.shape.y1;
                const float x = 0.5f * (float)(x0 + x1) - c->anchor_x, y = 0.5f * (float)(y0 + y1) - c->anchor_y;
                if (y > (float)c->fig.neck_y) continue;     /* (a voice beside her ears, sparkles below) */
                ++seen;
                CHECK(x < (float)c->fig.face_x + (float)c->fig.head_half && y >= chr_mark_top(c) - 1.0f);
            }
        }
        CHECK(seen > 0);
        chr_set_conditions(c, 0);
        chr_set_phase(c, CHR_PHASE_IDLE);
    }

    /* The dance: she keeps her feet on the ground, gives a pixel on the beat and is three to one side on the first two
     * of its pictures; a light rises on every beat. */
    fresh();
    chr_cue(c, CHR_CUE_DANCE, 0);
    int left_most = 0, right_most = 0, lowest = 0, highest = 0;
    unsigned lights = 0;
    for (int i = 0; i < 300; ++i) {
        info = step(FRAME);
        if (info.shown != CHR_GESTURE_DANCE) continue;
        if (info.moved_x < left_most) left_most = info.moved_x;
        if (info.moved_x > right_most) right_most = info.moved_x;
        if (info.moved_feet > lowest) lowest = info.moved_feet;
        if (info.moved_feet < highest) highest = info.moved_feet;
        for (int k = 0; k < CHR_PARTICLES_MAX; ++k)
            lights += c->particles[k].live && c->particles[k].kind >= FX_ORB_GOLD && c->particles[k].kind <= FX_ORB_LILAC && c->particles[k].age <= FRAME;
    }
    CHECK(left_most == -3 && right_most == 3 && lowest >= 1 && lowest <= 2 && highest == 0 && lights >= 12);
}

/* Thinking, or waiting for the service, she lowers her eyes to one side, always the same; she never rolls them
 * up, where the whites under them would show. Something else that has her attention comes first. */
static void test_ponder(void)
{
    const bool pictures = chr_pack_slot(&c->pack, SL_EYE_L_PONDER) != CHR_NO_SPRITE;
    CHECK(chr_pack_slot(&c->pack, SL_EYE_L_UP) == CHR_NO_SPRITE && chr_pack_slot(&c->pack, SL_EYE_R_UP) == CHR_NO_SPRITE);
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)step(1500);
    CHECK(!c->ponder);
    chr_set_phase(c, CHR_PHASE_THINKING);
    chr_info_t info = step(600);
    CHECK(c->ponder && info.gaze == (has(CHR_FLAT) ? 1 : 0) && info.target[P_GY] > 0.0f && info.shown == 0);
    if (pictures && !info.eye_left && !info.eye_right) CHECK(drawn(SL_EYE_L_PONDER) && drawn(SL_EYE_R_PONDER));
    for (int i = 0; i < 300; ++i) {
        info = step(FRAME);
        CHECK(info.gaze != 2 && info.target[P_GY] >= 0.0f);
        if (pictures && !info.shown && !info.eye_left && !info.eye_right) CHECK(drawn(SL_EYE_L_PONDER) && drawn(SL_EYE_R_PONDER));
    }
    if (has(CHR_HAS_POSES)) CHECK(info.shown == CHR_GESTURE_THINK);
    /* A finger on the screen has her eyes. */
    (void)chr_pointer(c, true, (int16_t)(W / 2 + 200), (int16_t)(H - 300));
    (void)step(200);
    CHECK(!c->ponder);
    (void)chr_pointer(c, false, 0, 0);
    (void)step(1000);
    /* Waiting for the service; the mood of thinking, and not while she listens. */
    static const struct { uint32_t conditions; const char *mood; chr_phase_t phase; bool ponder; } cases[] = {
        {CHR_CONNECTING, "neutral", CHR_PHASE_IDLE, true}, {CHR_OFFLINE | CHR_CONNECTING, "neutral", CHR_PHASE_IDLE, false},
        {0, "thinking", CHR_PHASE_IDLE, true}, {0, "thinking", CHR_PHASE_SPEAKING, true}, {0, "thinking", CHR_PHASE_LISTENING, false},
        {CHR_STARTING, "thinking", CHR_PHASE_IDLE, false}, {CHR_RINGING, "thinking", CHR_PHASE_IDLE, false},
        {0, "neutral", CHR_PHASE_IDLE, false}, {0, "surprised", CHR_PHASE_IDLE, false},
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        fresh();
        chr_set_mood(c, chr_mood_find(cases[k].mood));
        chr_set_phase(c, cases[k].phase);
        chr_set_conditions(c, cases[k].conditions);
        for (int i = 0; i < 90; ++i) info = sounding(FRAME, 0, cases[k].phase == CHR_PHASE_SPEAKING && i % 20 < 8 ? 9000 : 0);
        CHECK(c->ponder == cases[k].ponder);
        if (cases[k].ponder) CHECK(info.target[P_GX] < -0.3f && info.target[P_GY] >= 0.0f);
        if (pictures && cases[k].ponder && !info.shown && !info.eye_left && !info.eye_right && !info.cue)
            CHECK(drawn(SL_EYE_L_PONDER) && drawn(SL_EYE_R_PONDER));
        if (!cases[k].ponder) CHECK(!drawn(SL_EYE_L_PONDER) && !drawn(SL_EYE_R_PONDER));
    }
    /* Ducking, a look up from an ear's twitch, or a glance upwards: no eyes rolled up. */
    static const chr_cue_t ups[] = {CHR_CUE_DUCK, CHR_CUE_TWITCH, CHR_CUE_GLANCE};
    for (size_t k = 0; k < sizeof(ups) / sizeof(ups[0]); ++k) {
        fresh();
        chr_cue(c, ups[k], 1);
        for (int i = 0; i < 40; ++i) {
            (void)step(FRAME);
            CHECK(!drawn(SL_EYE_L_UP) && !drawn(SL_EYE_R_UP) && !drawn(SL_EYE_L_PONDER));
        }
    }
}

static void test_conditions(void)
{
    fresh();
    chr_set_conditions(c, CHR_OFFLINE);
    chr_info_t info = step(3000);
    CHECK(info.pose[P_GREY] > 0.7f && info.pose[P_EAR_L] > 0.8f && info.pose[P_WAG] < 0.1f);
    if (has(CHR_HAS_EARS)) CHECK(info.ear_left == 2 && info.ear_right == 2);
    chr_set_conditions(c, CHR_CONNECTING);
    info = step(3000);
    CHECK(info.pose[P_GREY] > 0.2f && info.pose[P_GREY] < 0.5f && info.pose[P_GX] < -0.3f && info.pose[P_GY] > 0.0f && c->ponder);
    chr_set_conditions(c, 0);
    info = step(4000);
    CHECK(info.pose[P_GREY] < 0.05f);
    /* A ringing call and an alarm outrank the conversation. */
    chr_set_phase(c, CHR_PHASE_THINKING);
    chr_set_conditions(c, CHR_ALARM);
    info = step(1200);
    CHECK(info.mouth_open >= 2 && info.pose[P_WAG] > 1.0f && info.target[P_GY] == 0.0f);
    chr_set_conditions(c, CHR_RINGING);
    info = step(1200);
    CHECK(info.target[P_GX] > 0.5f);
}

static void test_touch(void)
{
    /* One poke: a wince. Three in a row: dizzy. Six: she sulks and ignores more poking. */
    fresh();
    tap(cheek_x(), cheek_y());
    chr_info_t info = step(FRAME);
    CHECK(info.cue == CHR_CUE_POKE);
    tap(cheek_x(), cheek_y());
    tap(cheek_x(), cheek_y());
    CHECK(step(FRAME).cue == CHR_CUE_DIZZY);
    tap(cheek_x(), cheek_y()); tap(cheek_x(), cheek_y()); tap(cheek_x(), cheek_y());
    CHECK(step(FRAME).cue == CHR_CUE_POUT);
    tap(cheek_x(), cheek_y());
    CHECK(step(FRAME).cue == CHR_CUE_POUT);
    /* A plain tap is not taken from the control under it; a stroke over the head is. */
    fresh();
    CHECK(!chr_pointer(c, true, (int16_t)(W / 2 + cheek_x()), (int16_t)(H + cheek_y())));
    (void)step(60);
    CHECK(!chr_pointer(c, true, (int16_t)(W / 2 + cheek_x()), (int16_t)(H + cheek_y())));
    (void)chr_pointer(c, false, 0, 0);
    (void)step(FRAME);
    fresh();
    bool taken = false;
    for (int x = -70; x <= 60; x += 13) { taken = chr_pointer(c, true, (int16_t)(W / 2 + x), (int16_t)(H + crown_y())); (void)step(FRAME); }
    info = step(400);
    CHECK(taken && info.touch == CHR_TOUCH_PET && info.eye_left == 3 && info.pose[P_WAG] > 0.8f && info.particles > 0);
    (void)chr_pointer(c, false, 0, 0);
    CHECK(step(FRAME).touch == CHR_TOUCH_NONE);
    /* Holding the face squashes her. */
    fresh();
    for (int i = 0; i < 25; ++i) { taken = chr_pointer(c, true, (int16_t)(W / 2 + cheek_x()), (int16_t)(H + cheek_y())); info = step(FRAME); }
    CHECK(taken && info.touch == CHR_TOUCH_SQUISH && info.pose[P_HY] > 5.0f);
    (void)chr_pointer(c, false, 0, 0);
    (void)step(FRAME);
    /* An ear follows the finger that holds it; without ears the same place is nothing to hold. */
    fresh();
    const int ear_x = (c->fig.ear_lx0 + c->fig.ear_lx1) / 2, ear_y = c->fig.ears_top + 30;
    for (int i = 0; i < 12; ++i) { taken = chr_pointer(c, true, (int16_t)(W / 2 + ear_x + 5 * i), (int16_t)(H + ear_y)); info = step(FRAME); }
    if (has(CHR_HAS_EARS)) {
        CHECK(taken && info.touch == CHR_TOUCH_PULL && info.pose[P_EAR_L] > 0.4f && info.eye_left == 4);
        (void)chr_pointer(c, false, 0, 0);
        CHECK(step(FRAME).cue == CHR_CUE_FLICK);
    } else {
        CHECK(info.touch != CHR_TOUCH_PULL);
        (void)chr_pointer(c, false, 0, 0);
        CHECK(step(FRAME).cue != CHR_CUE_FLICK);
    }
    /* The tail startles her, when she has one. */
    fresh();
    if (has(CHR_HAS_TAIL)) {
        tap((c->fig.tail_x0 + c->fig.tail_x1) / 2, c->fig.tail_top / 2);
        CHECK(step(FRAME).cue == CHR_CUE_TAIL);
    }
    /* A finger anywhere draws her eyes; a tap beside her makes her look; swipes move her. */
    fresh();
    for (int i = 0; i < 12; ++i) { (void)chr_pointer(c, true, W / 2 + 400, H - 300); info = step(FRAME); }
    CHECK(info.touch == CHR_TOUCH_WATCH && info.gaze == (has(CHR_FLAT) ? 1 : 2));     /* (a flat figure looks at you) */
    for (int i = 0; i < 12; ++i) { (void)chr_pointer(c, true, W / 2 - 400, H - 300); info = step(FRAME); }
    CHECK(info.gaze == (has(CHR_FLAT) ? 1 : 0));
    (void)chr_pointer(c, false, 0, 0);
    (void)step(FRAME);
    fresh();
    tap(360, c->fig.hair_top);
    CHECK(step(FRAME).cue == CHR_CUE_LOOK);
    fresh();
    for (int i = 0; i <= 6; ++i) { (void)chr_pointer(c, true, (int16_t)(W / 2 - 330 + 110 * i), H - 60); (void)step(FRAME); }
    (void)chr_pointer(c, false, 0, 0);
    CHECK(step(FRAME).cue == CHR_CUE_SWAY);
    fresh();
    for (int i = 0; i <= 6; ++i) { (void)chr_pointer(c, true, W / 2 + 320, (int16_t)(H - 60 - 45 * i)); (void)step(FRAME); }
    (void)chr_pointer(c, false, 0, 0);
    CHECK(step(FRAME).cue == CHR_CUE_JUMP);
}

static void test_damage(void)
{
    /* The first step redraws everything; a step in which nothing visible moved redraws nothing; what is
     * redrawn stays on the stage. */
    CHECK(chr_init(c, W, H, 7, pack, pack_size));
    chr_rect_t damage[CHR_DAMAGE_MAX];
    CHECK(chr_step(c, FRAME, damage) >= 1);
    unsigned quiet = 0, total = 0;
    for (int i = 0; i < 300; ++i) {
        const size_t count = chr_step(c, FRAME, damage);
        CHECK(count <= CHR_DAMAGE_MAX);
        for (size_t k = 0; k < count; ++k)
            CHECK(damage[k].x >= 0 && damage[k].y >= 0 && damage[k].x + damage[k].width <= W && damage[k].y + damage[k].height <= H);
        quiet += count == 0;
        ++total;
    }
    chr_info_t info;
    chr_inspect(c, &info);
    CHECK(info.quiet_steps == quiet && quiet < total);
    CHECK(info.items >= 1 && info.items <= CHR_ITEMS_MAX);   /* a flat figure at rest is one picture */
    const chr_rect_t box = chr_body_box(c);
    CHECK(box.x > W / 2 - 140 && box.x + box.width < W / 2 + 140 && box.y > 150 && box.y + box.height <= H && box.width > 100);
}

/* Time alone, a tenth of a second a step. Returns how often the given reaction began. */
static unsigned pass(unsigned seconds, chr_cue_t watched)
{
    chr_rect_t damage[CHR_DAMAGE_MAX];
    chr_info_t info;
    unsigned began = 0;
    uint8_t before = CHR_CUE_NONE;
    for (unsigned i = 0; i < seconds * 10U; ++i) {
        (void)chr_step(c, 100, damage);
        chr_inspect(c, &info);
        if (info.cue == watched && before != watched) ++began;
        before = info.cue;
    }
    return began;
}

static void test_feeling(void)
{
    const uint8_t happy = (uint8_t)chr_mood_find("happy"), sad = (uint8_t)chr_mood_find("sad"),
                  sleepy = (uint8_t)chr_mood_find("sleepy");
    /* The mood of a talk stays for two seconds after it, then she is herself again; outside of a talk a
     * new one is hers within a blink. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_set_mood(c, happy);
    (void)step(1800);
    chr_set_phase(c, CHR_PHASE_IDLE);
    chr_info_t info = step(1900);
    CHECK(info.mood == happy && info.given == happy);
    info = step(500);
    CHECK(info.mood == CHR_MOOD_NEUTRAL && info.given == happy);
    chr_set_mood(c, chr_mood_find("laughing"));
    CHECK(step(120).mood == chr_mood_find("laughing"));
    /* Without a talk a mood stays for as long as the page gives it; a low one gives way after twenty
     * seconds, unless the bench holds it. */
    fresh();
    chr_set_mood(c, happy);
    (void)pass(40, CHR_CUE_NONE);
    CHECK(step(FRAME).mood == happy);
    fresh();
    chr_set_mood(c, sad);
    (void)pass(18, CHR_CUE_NONE);
    CHECK(step(FRAME).mood == sad);
    (void)pass(4, CHR_CUE_NONE);
    CHECK(step(FRAME).mood == CHR_MOOD_NEUTRAL);
    fresh();
    chr_hold(c, true);
    chr_set_mood(c, sad);
    (void)pass(40, CHR_CUE_NONE);
    CHECK(step(FRAME).mood == sad);
    chr_hold(c, false);

    /* Left alone she first looks for you and waves, three times at most; then she lets it be, grows
     * tired, and after most of an hour dozes off. */
    fresh();
    info = step(FRAME);
    CHECK(info.energy > 0.8f && fabsf(info.closeness - 0.5f) < 0.01f && !info.dozing);
    CHECK(pass(9 * 60, CHR_CUE_WAVE) == 0);
    const unsigned waves = pass(12 * 60, CHR_CUE_WAVE);
    CHECK(waves >= 2 && waves <= 3);
    CHECK(pass(20 * 60, CHR_CUE_WAVE) + waves == 3);
    info = step(FRAME);
    CHECK(info.energy < 0.6f && info.energy > 0.3f && !info.dozing);
    (void)pass(12 * 60, CHR_CUE_NONE);
    info = step(FRAME);
    CHECK(info.energy < 0.3f && info.mood == sleepy && info.dozing && info.gesture == CHR_GESTURE_SLEEP);
    if (has(CHR_HAS_POSES)) CHECK(info.shown == CHR_GESTURE_SLEEP);
    else CHECK(info.eye_left == 4 && info.eye_right == 4);
    /* The touch that wakes her does nothing else: she stretches, and is awake for a while. */
    tap(cheek_x(), cheek_y());
    info = step(FRAME);
    CHECK(!info.dozing && info.cue == CHR_CUE_STRETCH && info.energy > 0.4f);
    /* Called while dozing, she stretches too, and listens. */
    (void)pass(40 * 60, CHR_CUE_NONE);
    CHECK(step(FRAME).dozing);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    info = step(FRAME);
    CHECK(!info.dozing && info.cue == CHR_CUE_STRETCH && info.energy > 0.3f);
    chr_set_phase(c, CHR_PHASE_IDLE);

    /* Strokes bring her closer; being poked silly, less so. Left alone it settles in the middle. */
    fresh();
    for (int i = 0; i < 4; ++i)
        for (int x = -60; x <= 60; x += 12) { (void)chr_pointer(c, true, (int16_t)(W / 2 + c->fig.face_x + x), (int16_t)(H + crown_y())); (void)step(FRAME); }
    (void)chr_pointer(c, false, 0, 0);
    info = step(FRAME);
    const float stroked = info.closeness;
    CHECK(stroked > 0.6f);
    (void)step(1200);
    tap(cheek_x(), cheek_y()); tap(cheek_x(), cheek_y()); tap(cheek_x(), cheek_y());
    info = step(FRAME);
    CHECK(info.cue == CHR_CUE_DIZZY && info.closeness < stroked);
    (void)pass(40 * 60, CHR_CUE_NONE);
    CHECK(fabsf(step(FRAME).closeness - 0.5f) < 0.03f);

    /* A microphone that is off shows beside her mouth on a figure without ears to droop. */
    fresh();
    info = step(2000);
    const unsigned plain = info.items;
    chr_set_conditions(c, CHR_MUTED);
    info = step(FRAME);
    CHECK(has(CHR_HAS_EARS) ? info.items == plain : info.items == plain + 2);
    chr_set_conditions(c, 0);
}

/* Away from the screen and back: she is as the page says now, and what was under way is over. */
static void test_return(void)
{
    const uint8_t happy = (uint8_t)chr_mood_find("happy"), confused = (uint8_t)chr_mood_find("confused"),
                  laughing = (uint8_t)chr_mood_find("laughing"), sleepy = (uint8_t)chr_mood_find("sleepy"),
                  surprised = (uint8_t)chr_mood_find("surprised");
    /* A talk that ended meanwhile: no last words, no goodbye, and its mood is gone with the first picture. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_set_mood(c, happy);
    chr_info_t info = sounding(2500, 0, 9000);
    CHECK(info.mood == happy && info.mouth_open > 0);
    chr_set_mood(c, CHR_MOOD_NEUTRAL);
    chr_set_phase(c, CHR_PHASE_IDLE);
    chr_return(c, false);
    info = sounding(FRAME, 0, 0);
    CHECK(info.phase == CHR_PHASE_IDLE && info.mood == CHR_MOOD_NEUTRAL && info.mouth_open == 0 && !info.parting && !info.changing);
    CHECK(info.eye_left == info.eye_right && fabsf(info.pose[P_MC] - info.target[P_MC]) < 0.01f &&
          fabsf(info.pose[P_ARC] - info.target[P_ARC]) < 0.01f);
    for (int i = 0; i < 120; ++i) {
        info = sounding(FRAME, 0, 0);
        CHECK(info.cue == CHR_CUE_NONE && info.particles == 0 && info.gesture == CHR_GESTURE_NONE);
    }
    /* A talk that went on meanwhile has not just been cut short. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 9000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    chr_return(c, false);
    for (int i = 0; i < 40; ++i) CHECK(sounding(FRAME, 9000, 0).cue != CHR_CUE_STARTLE);
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 9000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    info = sounding(FRAME, 9000, 0);
    CHECK(info.listen > 0.3f);
    chr_return(c, false);
    info = sounding(FRAME, 0, 0);
    CHECK(info.listen == 0.0f);
    for (int i = 0; i < 40; ++i) CHECK(sounding(FRAME, 9000, 0).cue != CHR_CUE_STARTLE);
    CHECK(sounding(FRAME, 0, 0).shut_ms < 1000U);      /* (nor has her mouth taken all that time to shut) */
    /* Nor does she speak on with what she was saying. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 9000);
    chr_set_phase(c, CHR_PHASE_IDLE);
    CHECK(sounding(300, 0, 9000).parting);
    chr_return(c, false);
    info = sounding(FRAME, 0, 0);
    CHECK(!info.parting && info.mouth_open == 0);
    for (int i = 0; i < 60; ++i) CHECK(sounding(FRAME, 0, 0).cue != CHR_CUE_BYE);
    /* The mood she comes back in is the talk's no more when the talk is over, even if it is the same: it opens
     * the mouth and fills the air as it does at rest. In a talk it has not just come. */
    fresh();
    chr_set_mood(c, happy);
    info = step(1000);
    const float smile = info.target[P_MO];
    CHECK(has(CHR_FLAT) ? smile == 0.0f : smile > 0.05f);      /* (a flat figure smiles with her mouth shut) */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_set_mood(c, happy);
    info = sounding(2500, 0, 9000);
    CHECK(info.mood == happy && info.target[P_MO] == 0.0f);
    chr_set_phase(c, CHR_PHASE_IDLE);
    chr_return(c, false);
    info = sounding(FRAME, 0, 0);
    CHECK(info.mood == happy && fabsf(info.target[P_MO] - smile) < 0.01f);
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 9000);
    chr_set_mood(c, surprised);
    chr_return(c, false);
    info = sounding(FRAME, 0, 0);
    CHECK(info.mood == surprised && info.target[P_MO] == 0.0f && (has(CHR_FLAT) ? info.pose[P_EOL] <= 1.0f : info.target[P_EOL] < 1.15f));
    /* A reaction, its pose and what is in the air, or a pose that was being gone into. */
    for (int part = 0; part < 2; ++part) {
        fresh();
        chr_cue(c, CHR_CUE_CHEER, 0);
        info = step(part ? 120 : 700);
        CHECK(info.cue == CHR_CUE_CHEER && info.particles > 0);
        if (has(CHR_HAS_POSES)) CHECK(part ? info.changing : info.shown == CHR_GESTURE_CHEER);
        chr_return(c, false);
        info = step(FRAME);
        CHECK(info.cue == CHR_CUE_NONE && info.gesture == CHR_GESTURE_NONE && info.shown == 0 && !info.changing && info.particles == 0);
    }
    fresh();
    chr_cue(c, CHR_CUE_DIZZY, 1);
    (void)step(500);
    chr_return(c, false);
    (void)step(FRAME);
    CHECK(fabsf(c->swing) < 0.5f);             /* nor has her hair been left swinging */
    /* A reaction to what has happened now is played as ever: the greeting, on the first page she is on. */
    float most[2] = {0.0f, 0.0f};
    for (int back = 0; back < 2; ++back) {
        fresh();
        if (back) chr_return(c, false);
        chr_cue(c, CHR_CUE_GREET, 0);
        for (int i = 0; i < 30; ++i) {
            info = step(FRAME);
            most[back] = fmaxf(most[back], fabsf(info.pose[P_BY]));
        }
    }
    CHECK(most[0] > 0.5f && fabsf(most[0] - most[1]) < 0.05f * most[0]);
    /* The finger that was on her is not, and what it would have led to does not come. */
    fresh();
    (void)chr_pointer(c, true, (int16_t)(W / 2 + cheek_x()), (int16_t)(H + cheek_y()));
    info = step(900);
    CHECK(info.touch == CHR_TOUCH_SQUISH);
    chr_return(c, false);
    (void)chr_pointer(c, false, 0, 0);
    info = step(FRAME);
    CHECK(info.touch == CHR_TOUCH_NONE && info.cue == CHR_CUE_NONE && fabsf(c->squish) < 0.01f);
    /* The page's mood is hers at once, without the marks and the gesture of a mood that has just come. */
    fresh();
    chr_set_mood(c, confused);
    chr_return(c, false);
    info = step(FRAME);
    CHECK(info.mood == confused && info.particles == 0);
    for (int i = 0; i < 60; ++i) CHECK(step(FRAME).gesture == CHR_GESTURE_NONE);
    chr_set_mood(c, CHR_MOOD_NEUTRAL);
    (void)step(1000);
    chr_set_mood(c, confused);
    CHECK(step(400).particles > 0);            /* (one that comes later has them) */
    fresh();
    chr_set_mood(c, sleepy);
    chr_return(c, false);
    CHECK(step(1000).particles == 0);
    /* On a page that is about the mood she makes a show of it. */
    fresh();
    chr_set_mood(c, confused);
    chr_return(c, true);
    info = step(600);
    CHECK(info.mood == confused && info.particles > 0 && info.gesture == CHR_GESTURE_SHRUG);
    /* ... also when it is the one she has, or had until its twenty seconds were over. */
    (void)step(4000);
    chr_return(c, true);
    info = step(600);
    CHECK(info.mood == confused && info.particles > 0 && info.gesture == CHR_GESTURE_SHRUG);
    (void)pass(30, CHR_CUE_NONE);
    CHECK(step(FRAME).mood == CHR_MOOD_NEUTRAL);
    chr_return(c, true);
    info = step(600);
    CHECK(info.mood == confused && info.gesture == CHR_GESTURE_SHRUG);
    /* Elsewhere the page may be a moment late with its mood. */
    fresh();
    chr_set_mood(c, sleepy);
    chr_return(c, false);
    (void)step(50);
    chr_set_mood(c, confused);
    for (int i = 0; i < 60; ++i) {
        info = step(FRAME);
        CHECK(info.gesture == CHR_GESTURE_NONE && info.particles == 0);
    }
    CHECK(info.mood == confused);
    /* Behind a dialog while she spoke, and still heard: the mood is the talk's until two seconds after. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_set_mood(c, happy);
    (void)sounding(2500, 0, 9000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    chr_set_envelope(c, 0, 9000);
    chr_return(c, false);
    for (int i = 0; i < 60; ++i) CHECK(sounding(FRAME, 0, i < 20 ? 9000 : 0).mood == happy);
    CHECK(sounding(1500, 0, 0).mood == CHR_MOOD_NEUTRAL);
    /* Nor does she go on with the gesture of a mood that came before she went. */
    fresh();
    chr_set_mood(c, laughing);
    CHECK(step(500).gesture == CHR_GESTURE_LAUGH);
    chr_return(c, false);
    for (int i = 0; i < 60; ++i) CHECK(step(FRAME).gesture == CHR_GESTURE_NONE);
    /* Asleep she sleeps on. */
    fresh();
    c->energy = 0.2f;
    (void)pass(60, CHR_CUE_NONE);
    info = step(FRAME);
    CHECK(info.dozing && info.gesture == CHR_GESTURE_SLEEP);
    const uint8_t shown = info.shown;
    chr_return(c, false);
    info = step(FRAME);
    CHECK(info.dozing && info.gesture == CHR_GESTURE_SLEEP && info.shown == shown && info.cue == CHR_CUE_NONE);
    /* But not in the picture of something she did in between. */
    chr_cue(c, CHR_CUE_NOD, 0);
    info = step(400);
    CHECK(info.dozing && info.cue == CHR_CUE_NOD);
    chr_return(c, false);
    info = step(FRAME);
    CHECK(info.dozing && info.cue == CHR_CUE_NONE && info.gesture == CHR_GESTURE_SLEEP && info.shown != CHR_GESTURE_NOD);
    /* Unless she was called meanwhile. */
    chr_set_phase(c, CHR_PHASE_LISTENING);
    chr_return(c, false);
    info = step(FRAME);
    CHECK(!info.dozing && info.cue == CHR_CUE_NONE && info.gesture == CHR_GESTURE_NONE && info.shown == 0 && info.energy > 0.3f);

    /* The screen comes on for someone: she is awake, and stays awake after the stretch. */
    fresh();
    c->energy = 0.1f;
    (void)pass(60, CHR_CUE_NONE);
    CHECK(step(FRAME).dozing);
    chr_return(c, false);
    chr_woken(c, true);
    info = step(FRAME);
    CHECK(!info.dozing && info.cue == CHR_CUE_STRETCH);
    (void)pass(30, CHR_CUE_NONE);
    info = step(FRAME);
    CHECK(!info.dozing && info.mood == CHR_MOOD_NEUTRAL);
    chr_woken(c, false);
    CHECK(step(FRAME).cue != CHR_CUE_STRETCH);
    chr_woken(c, true);
    CHECK(step(FRAME).cue == CHR_CUE_STRETCH);
    /* Some time after it has come on she is awake, and that is all; nor does she stretch in a talk. */
    fresh();
    c->energy = 0.1f;
    (void)pass(60, CHR_CUE_NONE);
    CHECK(step(FRAME).dozing);
    chr_return(c, false);
    chr_woken(c, false);
    for (int i = 0; i < 60; ++i) {
        info = step(FRAME);
        CHECK(!info.dozing && info.cue == CHR_CUE_NONE);
    }
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    chr_return(c, false);
    chr_woken(c, true);
    CHECK(step(FRAME).cue == CHR_CUE_NONE);
}

/* What she leaves alone: in a talk, with the microphone off, half asleep, or without a network. */
static void test_restraint(void)
{
    const uint8_t happy = (uint8_t)chr_mood_find("happy"), sleepy = (uint8_t)chr_mood_find("sleepy"),
                  surprised = (uint8_t)chr_mood_find("surprised"), thinking = (uint8_t)chr_mood_find("thinking"),
                  angry = (uint8_t)chr_mood_find("angry");
    /* The network comes and goes while she talks: no arms in the air, no drops. */
    static const chr_phase_t talking[] = {CHR_PHASE_LISTENING, CHR_PHASE_THINKING, CHR_PHASE_SPEAKING};
    chr_info_t info;
    for (size_t i = 0; i < sizeof(talking) / sizeof(talking[0]); ++i) {
        fresh();
        chr_set_phase(c, talking[i]);
        (void)sounding(1500, 0, 0);
        chr_cue(c, CHR_CUE_CHEER, 0);
        CHECK(sounding(FRAME, 0, 0).cue != CHR_CUE_CHEER);
        chr_cue(c, CHR_CUE_SHIVER, 0);
        info = sounding(FRAME, 0, 0);
        CHECK(info.cue != CHR_CUE_SHIVER && info.particles == 0);
    }
    (void)sounding(500, 0, 9000);
    chr_set_phase(c, CHR_PHASE_IDLE);
    CHECK(sounding(300, 0, 9000).parting);
    chr_cue(c, CHR_CUE_CHEER, 0);
    CHECK(sounding(FRAME, 0, 9000).cue != CHR_CUE_CHEER);
    (void)sounding(4000, 0, 0);
    chr_cue(c, CHR_CUE_CHEER, 0);
    CHECK(step(FRAME).cue == CHR_CUE_CHEER);
    /* The volume is turned up between two words: her mouth stays shut, the bars show. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 9000);
    info = sounding(800, 0, 0);
    const unsigned items = info.items;
    CHECK(info.mouth_open == 0);
    chr_cue(c, CHR_CUE_VOLUME, 90);
    for (int i = 0; i < 30; ++i) {
        info = sounding(FRAME, 0, 0);
        CHECK(info.cue == CHR_CUE_VOLUME && info.mouth_open == 0 && info.items > items);
    }
    fresh();
    chr_cue(c, CHR_CUE_VOLUME, 90);
    CHECK(step(500).target[P_MO] > 0.5f);

    /* A microphone that is off hears nobody: she neither listens nor leans in to hear. */
    for (int off = 0; off < 2; ++off) {
        fresh();
        chr_set_conditions(c, off ? CHR_MUTED : 0);
        chr_set_level(c, 3);
        chr_set_phase(c, CHR_PHASE_LISTENING);
        CHECK(off ? sounding(1500, 9000, 0).listen == 0.0f : sounding(1500, 9000, 0).listen > 0.5f);
        chr_set_level(c, 0);
        CHECK(sounding(8000, 0, 0).gesture == (off ? CHR_GESTURE_NONE : CHR_GESTURE_LISTEN));
        chr_set_phase(c, CHR_PHASE_IDLE);
        (void)sounding(4000, 0, 0);
        chr_set_level(c, 3);
        info = sounding(400, 0, 0);
        CHECK(off ? info.listen < 0.01f : info.listen > 0.2f);
        chr_set_level(c, 0);
        /* Nor has anyone barged in. */
        fresh();
        chr_set_conditions(c, off ? CHR_MUTED : 0);
        chr_set_phase(c, CHR_PHASE_SPEAKING);
        (void)sounding(1500, 0, 9000);
        chr_set_level(c, 3);
        chr_set_phase(c, CHR_PHASE_LISTENING);
        bool started = false;
        for (int i = 0; i < 15; ++i) started |= sounding(FRAME, 9000, 0).cue == CHR_CUE_STARTLE;
        CHECK(started == !off);
        chr_set_level(c, 0);
        chr_set_conditions(c, 0);
    }

    /* A hand at the ear comes down when the microphone is switched off; switched on again, it is quiet from then on. */
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    CHECK(sounding(8000, 0, 0).gesture == CHR_GESTURE_LISTEN);
    chr_set_conditions(c, CHR_MUTED);
    CHECK(sounding(9000, 0, 0).gesture == CHR_GESTURE_NONE);
    chr_set_conditions(c, 0);
    CHECK(sounding(4000, 0, 0).gesture == CHR_GESTURE_NONE);
    CHECK(sounding(3000, 0, 0).gesture == CHR_GESTURE_LISTEN);
    /* Switched on as her answer ends: nobody has barged in. */
    fresh();
    chr_set_conditions(c, CHR_MUTED);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(2500, 0, 9000);
    (void)sounding(100, 0, 0);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    chr_set_conditions(c, 0);
    for (int i = 0; i < 30; ++i) CHECK(sounding(FRAME, 0, 0).cue != CHR_CUE_STARTLE);

    /* Half asleep, or while the service is out of reach, she does not look for anyone. */
    fresh();
    chr_set_mood(c, sleepy);
    CHECK(pass(15 * 60, CHR_CUE_WAVE) == 0 && step(FRAME).energy > 0.6f);
    fresh();
    chr_set_conditions(c, CHR_CONNECTING);
    CHECK(pass(15 * 60, CHR_CUE_WAVE) == 0);
    /* Nor does the mood of the page fill the air around a face that shows the network. */
    static const uint32_t away[] = {CHR_OFFLINE, CHR_CONNECTING, 0};
    for (size_t i = 0; i < sizeof(away) / sizeof(away[0]); ++i) {
        fresh();
        chr_set_mood(c, sleepy);
        (void)step(3000);
        chr_set_conditions(c, away[i]);
        (void)step(4000);
        unsigned seen = 0;
        for (int k = 0; k < 200; ++k) {
            (void)step(FRAME);
            for (int n = 0; n < CHR_PARTICLES_MAX; ++n) seen += c->particles[n].live && c->particles[n].kind == FX_ZED;
        }
        CHECK(away[i] ? seen == 0 : seen > 0);
        /* Nor do the marks of one that has just come. */
        fresh();
        chr_set_conditions(c, away[i]);
        (void)step(1000);
        chr_set_mood(c, surprised);
        seen = 0;
        for (int k = 0; k < 20; ++k) {
            (void)step(FRAME);
            for (int n = 0; n < CHR_PARTICLES_MAX; ++n) seen += c->particles[n].live && c->particles[n].kind == FX_EXCLAIM;
        }
        CHECK(away[i] ? seen == 0 : seen > 0);
        chr_set_conditions(c, 0);
    }
    /* There is no smiling goodbye to a network that is gone. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(1500, 0, 9000);
    chr_set_conditions(c, CHR_OFFLINE);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 100; ++i) {
        info = sounding(FRAME, 0, 0);
        CHECK(info.cue != CHR_CUE_BYE);
    }
    CHECK(!info.parting && info.target[P_GREY] > 0.8f);
    chr_set_conditions(c, 0);

    /* Called, while the talk is being connected: she leaves what she was doing and looks at you, until it is. */
    static const chr_cue_t own[] = {CHR_CUE_HUM, CHR_CUE_YAWN, CHR_CUE_GLANCE, CHR_CUE_WAVE, CHR_CUE_DANCE};
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); ++i) {
        fresh();
        chr_cue(c, own[i], 1);
        c->look_x = 0.6f;
        c->look_y = -0.2f;
        c->look_in = 500.0f;
        CHECK(step(FRAME).cue == own[i]);
        chr_set_conditions(c, CHR_STARTING);
        for (int k = 0; k < 150; ++k) {
            info = step(FRAME);
            CHECK(info.cue == CHR_CUE_NONE && c->look_x == 0.0f && c->look_y == 0.0f);
        }
        chr_set_conditions(c, 0);
    }
    /* Whatever expression the page has chosen for her rest. */
    fresh();
    chr_set_mood(c, thinking);
    CHECK(step(1000).target[P_GX] < -0.4f);
    chr_set_conditions(c, CHR_STARTING);
    info = step(FRAME);
    CHECK(info.target[P_GX] == 0.0f && info.target[P_GY] == 0.0f);
    chr_set_conditions(c, 0);
    /* Nor is a mood made a show of then, whether it comes just now or a moment before. */
    for (int before = 0; before < 2; ++before) {
        fresh();
        chr_set_mood(c, angry);
        if (before) CHECK(step(600).gesture == CHR_GESTURE_SULK);
        chr_set_conditions(c, CHR_STARTING);
        (void)step(FRAME);
        for (int k = 0; k < 60; ++k) CHECK(step(FRAME).gesture == CHR_GESTURE_NONE);
        chr_set_conditions(c, 0);
    }
    /* And the goodbye of the last talk is over. */
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(3000, 0, 0);
    chr_set_phase(c, CHR_PHASE_IDLE);
    CHECK(sounding(200, 0, 0).cue == CHR_CUE_BYE);
    chr_set_conditions(c, CHR_STARTING);
    CHECK(sounding(FRAME, 0, 0).cue == CHR_CUE_NONE);
    chr_set_conditions(c, 0);
    /* What somebody has led to goes on: a poke, the greeting. */
    fresh();
    tap(cheek_x(), cheek_y());
    CHECK(step(FRAME).cue == CHR_CUE_POKE);
    chr_set_conditions(c, CHR_STARTING);
    CHECK(step(FRAME).cue == CHR_CUE_POKE);
    chr_set_conditions(c, 0);
    /* And then her eyes light up, as ever. */
    fresh();
    chr_set_conditions(c, CHR_STARTING);
    (void)step(1500);
    chr_set_conditions(c, 0);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    CHECK(step(FRAME).cue == CHR_CUE_WAKE);

    /* A word of feeling that comes with the end of her turn has been spoken: it gives way like any other. */
    fresh();
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    (void)sounding(2500, 0, 9000);
    chr_set_mood(c, happy);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    info = sounding(1200, 0, 0);
    CHECK(info.mood == happy);
    info = sounding(2000, 0, 0);
    CHECK(info.mood == CHR_MOOD_NEUTRAL && info.given == happy);
    /* One that comes into the quiet is for what she says next. */
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)sounding(3000, 0, 0);
    chr_set_phase(c, CHR_PHASE_THINKING);
    chr_set_mood(c, happy);
    CHECK(sounding(5000, 0, 0).mood == happy);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    CHECK(sounding(1500, 0, 9000).mood == happy);
}

static void test_hands(void)
{
    fresh();
    const int arm_y = c->fig.arm_top + 60;
    if (!c->fig.arm_top) {
        /* No arms in this figure: the place beside her body is her body, or nothing. */
        tap(c->fig.shoulder_half - 20, c->fig.neck_y / 2);
        CHECK(step(FRAME).cue != CHR_CUE_OFFER);
        return;
    }
    /* A tap on her arm: she holds a hand out and waits. */
    tap(c->fig.arm_rx, arm_y);
    chr_info_t info = step(200);
    CHECK(info.cue == CHR_CUE_OFFER && info.gesture == CHR_GESTURE_HIGHFIVE);
    if (has(CHR_HAS_POSES)) CHECK(info.shown == CHR_GESTURE_HIGHFIVE);
    if (has(CHR_HAS_ARMS)) CHECK(step(500).arm_right >= 2);
    const float before = info.closeness;
    /* A tap on that hand (or on the arm again): a high five, with sparks. */
    if (c->fig.hand_rx) tap(c->fig.hand_rx, c->fig.hand_ry);
    else tap(c->fig.arm_rx, arm_y);
    info = step(FRAME);
    CHECK(info.cue == CHR_CUE_HIGHFIVE && info.particles >= 4 && info.closeness > before);
    info = step(2500);
    CHECK(info.cue == CHR_CUE_NONE && info.gesture == CHR_GESTURE_NONE);
    /* Left alone, the hand held out goes back down. */
    tap(c->fig.arm_lx, arm_y);
    CHECK(step(200).cue == CHR_CUE_OFFER);
    info = step(3200);
    CHECK(info.cue == CHR_CUE_NONE && info.gesture == CHR_GESTURE_NONE);
    /* A finger resting on her arm holds her hand: the touch is hers, she blushes; let go, she nods. */
    bool taken = false;
    for (int i = 0; i < 45; ++i) { taken = chr_pointer(c, true, (int16_t)(W / 2 + c->fig.arm_lx), (int16_t)(H + arm_y)); info = step(FRAME); }
    CHECK(taken && info.touch == CHR_TOUCH_HOLD && info.gesture == CHR_GESTURE_SHY && info.pose[P_BLUSH] > 0.5f);
    (void)chr_pointer(c, false, 0, 0);
    info = step(FRAME);
    CHECK(info.touch == CHR_TOUCH_NONE && info.cue == CHR_CUE_NOD);
    /* While she sulks, her arm is not for touching. */
    fresh();
    for (int i = 0; i < 6; ++i) tap(cheek_x(), cheek_y());
    CHECK(step(FRAME).cue == CHR_CUE_POUT);
    tap(c->fig.arm_rx, arm_y);
    CHECK(step(FRAME).cue == CHR_CUE_POUT);
}

static void test_figures(void)
{
    /* The same code, two figures: what each has comes from its pack. */
    size_t size = 0;
    chr_figure_t fig;
    uint8_t *fox = fixture_pack(FIXTURE_EARS | FIXTURE_TAIL, &size, &fig);
    CHECK(chr_init(c, W, H, 5, fox, size));
    CHECK(chr_features(c) == (CHR_HAS_EARS | CHR_HAS_TAIL));
    CHECK(!memcmp(&c->fig, &fig, sizeof(fig)));
    chr_info_t info = step(600);
    CHECK(info.features == (CHR_HAS_EARS | CHR_HAS_TAIL) && info.turn == 1 && info.arm_left == 0);
    const chr_rect_t fox_box = chr_body_box(c);
    free(fox);
    uint8_t *all = fixture_pack(FIXTURE_EARS | FIXTURE_TAIL | FIXTURE_ARMS | FIXTURE_YAW, &size, NULL);
    CHECK(chr_init(c, W, H, 5, all, size));
    CHECK(chr_features(c) == (CHR_HAS_EARS | CHR_HAS_TAIL | CHR_HAS_ARMS | CHR_HAS_YAW));
    /* With arms and a head that turns, a greeting is a raised arm and a tap far to one side turns her
     * head there; a gesture is made with the arms. */
    (void)step(300);
    chr_cue(c, CHR_CUE_GREET, 0);
    bool raised = false, waved = false;
    for (int i = 0; i < 70; ++i) {
        info = step(FRAME);
        raised |= info.arm_left >= 2;
        waved |= info.gesture == CHR_GESTURE_WAVE;
        CHECK(info.shown == 0);                 /* no pose pictures in this pack */
    }
    CHECK(raised && waved);
    (void)step(3000);
    info = step(FRAME);
    CHECK(info.arm_left == 0 && info.gesture == CHR_GESTURE_NONE);
    tap(-400, c->fig.eye_y);
    bool turned = false;
    for (int i = 0; i < 20; ++i) turned |= step(FRAME).turn == 0;
    CHECK(turned);
    (void)step(2500);
    CHECK(step(FRAME).turn == 1);
    chr_cue(c, CHR_CUE_CHEER, 0);
    info = step(500);
    CHECK(info.gesture == CHR_GESTURE_CHEER && info.arm_left == 3 && info.arm_right == 3);
    free(all);

    /* The shipped figure is flat: one picture of her, patches for her face and pictures for her poses,
     * laid out differently from the figure above. */
    size_t shipped_size = 0;
    const void *shipped = chr_host_pack(&shipped_size);
    CHECK(chr_init(c, W, H, 5, shipped, shipped_size));
    CHECK(chr_features(c) == (CHR_FLAT | CHR_HAS_POSES));
    const chr_rect_t box = chr_body_box(c);
    CHECK(c->fig.hair_top != fig.hair_top && c->fig.eye_y != fig.eye_y && (box.y != fox_box.y || box.width != fox_box.width));
    CHECK(c->fig.shoulder_y > c->fig.neck_y && c->fig.shoulder_y < 0);
    /* The same gestures are whole pictures here: she waves in a greeting, and is herself again after. */
    (void)step(300);
    info = step(FRAME);
    CHECK(info.shown == 0 && info.items >= 1);
    chr_cue(c, CHR_CUE_GREET, 0);
    bool posed = false;
    for (int i = 0; i < 70; ++i) posed |= step(FRAME).shown == CHR_GESTURE_WAVE;
    CHECK(posed);
    (void)step(3000);
    CHECK(step(FRAME).shown == 0);
    /* Listening and thinking for long are poses; speaking keeps the base picture, whose mouth can move. */
    chr_set_phase(c, CHR_PHASE_LISTENING);
    CHECK(step(5000).shown == 0);
    CHECK(step(2000).shown == CHR_GESTURE_LISTEN);
    chr_set_phase(c, CHR_PHASE_THINKING);
    CHECK(step(1900).shown != CHR_GESTURE_THINK);      /* one pose a round: she has had hers */
    chr_set_phase(c, CHR_PHASE_LISTENING);
    (void)step(1000);
    chr_set_phase(c, CHR_PHASE_THINKING);
    CHECK(step(3400).shown == CHR_GESTURE_THINK);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    bool spoke = false;
    for (int i = 0; i < 90; ++i) {
        chr_set_envelope(c, 0, 9000);
        info = step(FRAME);
        if (i > 14) spoke |= info.shown == 0 && info.mouth_open == 2;
    }
    CHECK(spoke);
    chr_set_envelope(c, 0, 0);
    /* Her eyes have other looks (soft, bright, glad, cast down, with tears) and her brows and mouth too: each part fades
     * from one picture to the next, and what is drawn is that picture. (In a talk only the open ones: glad, lowered and
     * serious eyes are soft or her own there, while her brows and mouth keep the mood.) */
    (void)step(1500);
    uint8_t shown_eyes = c->eyes_shown;
    /* The faces of a conversation, as drawn: between blinks exactly these eyes, brows and (with the voice still)
     * this mouth, or her own. Once with the pack as it is, once with one from before the eyes of comfort and serious. */
    enum { E = CHR_NO_SPRITE };
    /* (Once the mood has stayed: the full face. An excited mouth rests in an open smile, 14.) */
    static const struct { const char *mood; unsigned eye, old_eye, brows, mouth, old_mouth; int gaze_x, gaze_y; } faces[] = {
        {"neutral", E, E, E, E, E, 0, 0}, {"happy", SL_EYE_L_SOFT, SL_EYE_L_SOFT, E, E, E, 0, 0},
        {"excited", SL_EYE_L_BRIGHT, SL_EYE_L_BRIGHT, SL_BROWS, 14, 14, 0, 0},
        {"gentle", SL_EYE_L_SOFT, SL_EYE_L_SOFT, E, E, E, 0, 0}, {"thinking", SL_EYE_L_PONDER, E, E, 1, 1, -1, 0},
        {"serious", E, E, SL_BROWS_SERIOUS, SL_MOUTH_SERIOUS, 1, 0, 0}, {"confused", SL_EYE_L_PONDER, E, SL_BROWS, 1, 1, 1, 0},
        {"comfort", SL_EYE_L_TEARY, SL_EYE_L_SOFT, SL_BROWS_COMFORT, SL_MOUTH_COMFORT, E, 0, 0},
        {"moved", SL_EYE_L_TEARY, SL_EYE_L_SOFT, SL_BROWS_COMFORT, E, E, 0, 0},
        {"neutral", E, E, E, E, E, 0, 0},
    };
    uint8_t *older = malloc(shipped_size);
    memcpy(older, shipped, shipped_size);
    memset(older + 28 + ((2U * CHR_FIGURE_VALUES + 3U) & ~3U) + 2U * SL_EYE_L_COMFORT, 0xFF, 2U * (SL_COUNT - SL_EYE_L_COMFORT));
    fixture_seal(older);
    for (int old = 0; old < 2; ++old) {
        CHECK(chr_init(c, W, H, 5, old ? (const void *)older : shipped, shipped_size));
        (void)step(400);
        chr_set_phase(c, CHR_PHASE_SPEAKING);
        (void)sounding(1000, 0, 9000);
        (void)sounding(1500, 0, 0);
        shown_eyes = c->eyes_shown;
        for (size_t k = 0; k < sizeof(faces) / sizeof(faces[0]); ++k) {
            chr_set_mood(c, chr_mood_find(faces[k].mood));
            const unsigned eye = old ? faces[k].old_eye : faces[k].eye, rest = old ? faces[k].old_mouth : faces[k].mouth;
            const unsigned mouth = rest < 15U ? SL_HEAD + H_MOUTH + rest : rest;
            unsigned seen = 0;
            size_t mood = info.mood;
            for (int i = 0; i < 130; ++i) {
                /* a voice now and then, so that she is in the middle of a talk all the while */
                info = sounding(FRAME, 0, i % 40 < 6 ? 9000 : 0);
                /* she looks where she is going to look before her eyes open again */
                if (info.mood != mood) CHECK(fabsf(info.pose[P_GX] - info.target[P_GX]) < 0.1f && fabsf(info.pose[P_GY] - info.target[P_GY]) < 0.1f);
                mood = info.mood;
                /* (Not in a blink, nor while her lids are on their way back from one.) */
                if (i < 60 || i % 40 < 16 || info.eye_left || c->rung[0] != RUNG_OPEN || c->rung[1] != RUNG_OPEN ||
                    (eye == E && info.gaze != 1 && !faces[k].gaze_x)) continue;
                bool has_eye = eye == E, has_brows = faces[k].brows == E, has_mouth = mouth == E, other = false;
                for (unsigned n = 0; n < c->item_count; ++n) {
                    if (!c->items[n].sprite) continue;
                    const unsigned id = c->items[n].as.blit.id;
                    has_eye |= eye != E && id == chr_pack_slot(&c->pack, eye);
                    has_brows |= faces[k].brows != E && id == chr_pack_slot(&c->pack, faces[k].brows);
                    has_mouth |= mouth != E && id == chr_pack_slot(&c->pack, mouth);
                    for (unsigned s = SL_HEAD + H_MOUTH; s < SL_HEAD + H_MOUTH + 15U; ++s)
                        other |= id == chr_pack_slot(&c->pack, s) && (mouth == E || id != chr_pack_slot(&c->pack, mouth));
                    static const unsigned looks[] = {SL_EYE_L_SOFT, SL_EYE_L_BRIGHT, SL_EYE_L_COMFORT, SL_EYE_L_SERIOUS, SL_EYE_L_PONDER,
                                                     SL_EYE_L_GLAD, SL_EYE_L_DOWN, SL_EYE_L_TEARY, SL_BROWS, SL_BROWS + 1, SL_BROWS + 2,
                                                     SL_BROWS_COMFORT, SL_BROWS_SERIOUS, SL_MOUTH_COMFORT, SL_MOUTH_SERIOUS, SL_MOUTH_SMIRK,
                                                     SL_MOUTH_FROWN};
                    for (size_t s = 0; s < sizeof(looks) / sizeof(looks[0]); ++s)
                        other |= looks[s] != eye && looks[s] != faces[k].brows && looks[s] != mouth && id != E && id == chr_pack_slot(&c->pack, looks[s]);
                }
                CHECK(has_eye && has_brows && has_mouth && !other);
                CHECK((faces[k].gaze_x > 0 ? info.pose[P_GX] > 0.3f : faces[k].gaze_x < 0 ? info.pose[P_GX] < -0.3f : fabsf(info.pose[P_GX]) < 0.3f) &&
                      (faces[k].gaze_y < 0 ? info.pose[P_GY] < -0.3f : fabsf(info.pose[P_GY]) < 0.3f));
                ++seen;
            }
            CHECK(seen > 8 && info.mood == chr_mood_find(faces[k].mood));
        }
        chr_set_phase(c, CHR_PHASE_IDLE);
        (void)sounding(4000, 0, 0);
    }
    free(older);
    CHECK(chr_init(c, W, H, 5, shipped, shipped_size));
    (void)step(1500);
    shown_eyes = c->eyes_shown;
    chr_set_phase(c, CHR_PHASE_LISTENING);
    /* Listening she raises her brows, and keeps her eyes as they are (not wider). */
    bool brows_up = false;
    for (int i = 0; i < 60; ++i) {
        info = step(FRAME);
        CHECK(c->eyes_shown == shown_eyes);
        brows_up |= drawn(SL_BROWS);
    }
    CHECK(brows_up && shown_eyes == LOOK_PLAIN);
    chr_set_phase(c, CHR_PHASE_IDLE);
    (void)step(3000);
    CHECK(c->eyes_shown == LOOK_PLAIN);
    /* A mood shows in the whole body for a moment, then in the face. */
    chr_set_mood(c, chr_mood_find("embarrassed"));
    CHECK(step(600).shown == CHR_GESTURE_SHY);
    CHECK(step(3500).shown == 0);
    /* A pose is held long enough to be seen: a gesture that ends at once does not flicker the picture. */
    chr_set_mood(c, CHR_MOOD_NEUTRAL);
    (void)step(1000);
    chr_cue(c, CHR_CUE_CHEER, 0);
    (void)step(200);
    CHECK(step(FRAME).shown == CHR_GESTURE_CHEER);
    chr_cue(c, CHR_CUE_NOD, 0);
    CHECK(step(FRAME).shown == CHR_GESTURE_CHEER);
    /* A pose stays two seconds at least: the nod has come and gone meanwhile. */
    CHECK(step(1500).shown == CHR_GESTURE_CHEER);
    CHECK(step(1500).shown == 0);
    chr_cue(c, CHR_CUE_NOD, 0);
    /* A nod is a picture of its own, played twice with the base picture between, by way of one in which her head
     * is half way down. */
    const unsigned down = chr_pack_slot(&c->pack, SL_BODY);
    unsigned nods = 0, halves = 0;
    bool between = false;
    uint16_t last = CHR_NO_SPRITE, half = CHR_NO_SPRITE, whole = CHR_NO_SPRITE;
    for (int i = 0; i < 80; ++i) {
        info = step(FRAME);
        if (info.shown != CHR_GESTURE_NOD || c->picture == last) continue;
        last = c->picture;
        if (half == CHR_NO_SPRITE) half = last;
        if (last == half) ++halves;
        else if (last == down) between = true;
        else {
            CHECK(whole == CHR_NO_SPRITE || whole == last);
            whole = last;
            ++nods;
        }
    }
    CHECK(nods == 2 && between && halves >= 3);
    CHECK(step(1500).shown == 0);
}

/* A pose that is played with several pictures, on a flat fixture that brings a dance. */
static void test_clips(void)
{
    size_t size = 0;
    uint8_t *made = fixture_pack(FIXTURE_FLAT | FIXTURE_CLIP, &size, NULL);
    chr_pack_t opened;
    chr_clip_t clip;
    chr_frame_t frame;
    CHECK(chr_pack_open(&opened, made, size));
    CHECK(chr_pack_clip(&opened, CHR_GESTURE_DANCE, &clip) && clip.first == 0 && clip.enter == 1 && clip.loop == 2 &&
          clip.leave == 1 && clip.style == CHR_STYLE_BEAT);
    CHECK(!chr_pack_clip(&opened, CHR_GESTURE_WAVE, &clip));      /* one picture, no clip */
    CHECK(chr_pack_frame(&opened, 1, &frame) && frame.ms == 300 && frame.sprite < opened.count);
    CHECK(!chr_pack_frame(&opened, 6, &frame));
    /* A clip that runs past the frames, and a frame that is no sprite, are not accepted. */
    uint8_t *copy = malloc(size);
    memcpy(copy, made, size);
    const uint16_t many = 9, none = 0xFFF0;
    memcpy(copy + opened.clips_at + 2 * 5, &many, sizeof(many));
    fixture_seal(copy);
    chr_pack_t damaged;
    CHECK(!chr_pack_open(&damaged, copy, size));
    memcpy(copy, made, size);
    memcpy(copy + opened.clips_at + 2 * 14, &none, sizeof(none));
    fixture_seal(copy);
    CHECK(!chr_pack_open(&damaged, copy, size));
    free(copy);

    /* A picture stored with a palette draws its colours as one stored pixel by pixel would: plain,
     * grown wider (where each pixel is looked up on its own), and half transparent. */
    enum { PW = 400, PH = 520 };
    static uint16_t canvas[PW * PH];
    static int16_t room[PW + 4 + 3 * (PW + 1)];
    const chr_target_t paper = {.pixels = (uint8_t *)canvas, .stride = PW * 2, .x0 = 0, .y0 = 0, .x1 = PW - 1, .y1 = PH - 1,
        .scratch = room, .lines = PW};
    const uint16_t wave = (uint16_t)chr_pack_slot(&opened, SL_POSE + CHR_GESTURE_WAVE - 1U);
    chr_sprite_t waving;
    CHECK(chr_pack_sprite(&opened, wave, &waving) && waving.width + 60 <= PW && waving.height + 20 <= PH);
    chr_blit_t inked = {.id = wave, .x = 30, .y = 10, .alpha = 32};
    memset(canvas, 0, sizeof(canvas));
    CHECK(chr_blit_bounds(&opened, &inked) && chr_blit_draw(&paper, &opened, &inked) == (uint32_t)waving.width * waving.height);
    unsigned inks = 0, others = 0;
    for (int i = 0; i < PW * PH; ++i) {
        inks += canvas[i] == 0xE71C;
        others += canvas[i] != 0xE71C && canvas[i] != 0;
    }
    CHECK(inks == (unsigned)waving.width * waving.height && others == 0);
    memset(canvas, 0, sizeof(canvas));
    inked.grow_x = 0.1f; inked.pin_x = 30.0f + (float)waving.width / 2.0f; inked.pin_y = 10.0f + (float)waving.height;
    CHECK(chr_blit_bounds(&opened, &inked) && chr_blit_draw(&paper, &opened, &inked) > (uint32_t)waving.width * waving.height);
    inks = others = 0;
    for (int i = 0; i < PW * PH; ++i) {
        inks += canvas[i] == 0xE71C;
        others += canvas[i] != 0xE71C && canvas[i] != 0;
    }
    CHECK(inks > (unsigned)waving.width * waving.height && others == 0);
    inked.alpha = 16;
    CHECK(chr_blit_bounds(&opened, &inked) && chr_blit_draw(&paper, &opened, &inked) > 0);
    CHECK(canvas[(10 + waving.height / 2) * PW + 30 + waving.width / 2] == 0xE71C);   /* the same colour over itself */
    /* A second picture stored as a patch over the first. Where its rows have a span, the first
     * picture is not drawn: what the span holds is drawn, and what it leaves empty shows the stage
     * (a hand that moved away). Outside the spans the first picture is as it was. */
    const uint16_t cheer = (uint16_t)chr_pack_slot(&opened, SL_POSE + CHR_GESTURE_CHEER - 1U);
    const uint16_t spans = (uint16_t)chr_pack_slot(&opened, FIXTURE_PATCH_SLOT);
    chr_sprite_t cheering, patching;
    CHECK(chr_pack_frame(&opened, 5, &frame) && frame.sprite == spans && frame.under == cheer);
    CHECK(chr_pack_sprite(&opened, cheer, &cheering) && chr_pack_sprite(&opened, spans, &patching));
    chr_blit_t both = {.id = cheer, .x = 30, .y = 10, .alpha = 32, .patch = (uint16_t)(spans + 1U)};
    memset(canvas, 0, sizeof(canvas));
    CHECK(chr_blit_bounds(&opened, &both) && chr_blit_draw(&paper, &opened, &both) > 0);
    const int px = 30 + patching.x - cheering.x, py = 10 + patching.y - cheering.y;
    unsigned reds = 0, greens = 0;
    for (int i = 0; i < PW * PH; ++i) {
        reds += canvas[i] == 0xF800;
        greens += canvas[i] == 0x07E0;
    }
    CHECK(reds == 80U * 30U + 40U * 30U && greens == (unsigned)cheering.width * cheering.height - 80U * 60U);
    CHECK(canvas[(py + 10) * PW + px + 20] == 0xF800 && canvas[(py + 40) * PW + px + 20] == 0xF800);
    CHECK(canvas[(py + 40) * PW + px + 70] == 0);                                   /* left empty: the stage shows */
    CHECK(canvas[(py + 10) * PW + px + 5] == 0x07E0 && canvas[(py + 40) * PW + px + 95] == 0x07E0);   /* beside the span */
    /* The same when she is drawn wider, where every pixel is looked up. */
    memset(canvas, 0, sizeof(canvas));
    both.grow_x = 0.1f; both.pin_x = 30.0f + (float)cheering.width / 2.0f; both.pin_y = 10.0f + (float)cheering.height;
    CHECK(chr_blit_bounds(&opened, &both) && chr_blit_draw(&paper, &opened, &both) > 0);
    unsigned wide_reds = 0, wide_greens = 0, strays = 0;
    for (int i = 0; i < PW * PH; ++i) {
        wide_reds += canvas[i] == 0xF800;
        wide_greens += canvas[i] == 0x07E0;
        strays += canvas[i] != 0xF800 && canvas[i] != 0x07E0 && canvas[i] != 0;
    }
    CHECK(wide_reds > reds * 105U / 100U && wide_reds < reds * 116U / 100U && wide_greens > greens && strays == 0);
    /* A patch is not a picture on its own. */
    chr_blit_t alone = {.id = spans, .x = 30, .y = 10, .alpha = 32};
    CHECK(chr_blit_bounds(&opened, &alone) && chr_blit_draw(&paper, &opened, &alone) == 0);
    /* A palette sprite too short to hold its palette is not accepted. */
    copy = malloc(size);
    memcpy(copy, made, size);
    const uint32_t stub = 16U | 1U << 24;
    memcpy(copy + opened.table_at + 20U * wave + 12, &stub, sizeof(stub));
    fixture_seal(copy);
    CHECK(!chr_pack_open(&damaged, copy, size));
    free(copy);

    CHECK(chr_init(c, W, H, 7, made, size) && has(CHR_FLAT) && has(CHR_HAS_POSES));
    (void)step(400);
    CHECK(step(FRAME).shown == 0);
    /* Two pictures that are alike (one a patch over the other) take turns without a dip, even in a
     * clip she is brought into softly. */
    chr_set_conditions(c, CHR_ALARM);
    (void)step(1500);
    unsigned flips = 0;
    bool dipping = false;
    uint8_t before = step(FRAME).frame;
    for (int i = 0; i < 30; ++i) {
        const chr_info_t now = step(FRAME);
        flips += now.frame != before;
        before = now.frame;
        dipping |= now.changing;
        CHECK(now.shown == CHR_GESTURE_CHEER);
    }
    CHECK(flips >= 4 && !dipping);
    chr_set_conditions(c, 0);
    (void)step(2500);
    CHECK(step(FRAME).shown == 0);
    /* Asked to dance she dips, and the first picture comes at the bottom of the dip. */
    chr_cue(c, CHR_CUE_DANCE, 0);
    chr_info_t info = step(140);
    CHECK(info.gesture == CHR_GESTURE_DANCE && info.shown == 0 && info.changing);
    info = step(130);
    CHECK(info.shown == CHR_GESTURE_DANCE && info.frame == 0);
    /* The entry plays once; then the two pictures of the loop take turns, never the way out. */
    unsigned seen = 0, turns = 0;
    uint8_t last = info.frame;
    for (int i = 0; i < 60; ++i) {
        info = step(FRAME);
        seen |= 1U << info.frame;
        if (info.frame != last) ++turns;
        if (i > 20) CHECK(info.frame == 1 || info.frame == 2);
        last = info.frame;
        CHECK(info.shown == CHR_GESTURE_DANCE);
    }
    CHECK(seen == 7U && turns >= 4);
    /* Every picture of a beat comes with a dip and a spring back up: she is shorter and wider just
     * before it changes, taller and narrower just after, and off the ground for a moment. */
    float shortest = 0.0f, tallest = 0.0f, widest = 0.0f, highest_hop = 0.0f;
    for (int i = 0; i < 30; ++i) {
        (void)step(FRAME);
        float wide, tall, lift;
        chr_change_shape(c, &wide, &tall, &lift);
        if (tall < shortest) shortest = tall;
        if (tall > tallest) tallest = tall;
        if (wide > widest) widest = wide;
        if (lift > highest_hop) highest_hop = lift;
    }
    CHECK(shortest < -0.009f && shortest >= -0.0181f && tallest > 0.009f && tallest <= 0.0201f);
    CHECK(widest > 0.004f && highest_hop > 1.5f && highest_hop <= 3.0f);
    /* When the dance is over the way out plays before she leaves the pose, and she ends as she began. */
    bool left = false, back = false;
    for (int i = 0; i < 400 && !back; ++i) {
        info = step(FRAME);
        left |= info.shown == CHR_GESTURE_DANCE && info.frame == 3;
        back = left && info.shown == 0 && !info.changing;
    }
    CHECK(left && back && info.gesture == CHR_GESTURE_NONE);
    /* A pose of one picture still works beside it, and changes with the same dip. */
    chr_cue(c, CHR_CUE_GREET, 0);
    bool waved = false, dipped = false;
    for (int i = 0; i < 80; ++i) {
        info = step(FRAME);
        dipped |= info.changing && info.shown == 0;
        waved |= info.shown == CHR_GESTURE_WAVE && info.frame == 0;
    }
    CHECK(dipped && waved);
    /* A figure without dance pictures dances with what it has: it sways (a flat one does not leave the ground). */
    free(made);
    made = fixture_pack(FIXTURE_FLAT, &size, NULL);
    CHECK(chr_init(c, W, H, 7, made, size));
    (void)step(400);
    chr_cue(c, CHR_CUE_DANCE, 0);
    float left_most = 0.0f, right_most = 0.0f, highest = 0.0f;
    for (int i = 0; i < 90; ++i) {
        info = step(FRAME);
        if (info.pose[P_LEAN] < left_most) left_most = info.pose[P_LEAN];
        if (info.pose[P_LEAN] > right_most) right_most = info.pose[P_LEAN];
        if (info.pose[P_BY] < highest) highest = info.pose[P_BY];
    }
    CHECK(left_most < -8.0f && right_most > 8.0f && highest > -1.0f);
    free(made);
}

/* Pictures that are late, or never come: a shelf that hands a picture out a number of steps after it
 * was first asked for, and some not at all. */
typedef struct {
    const uint8_t *pack;
    const chr_pack_t *whole;
    unsigned late, asked[256];
    uint64_t never;
    unsigned lost, lost_after;      /* one that is said to be coming so often, and then not to be had */
} fake_shelf_t;

static bool missing_for_good(const fake_shelf_t *f, unsigned id) { return (id < 64U && f->never >> id & 1U) || id == f->lost; }

static bool resident(const fake_shelf_t *f, unsigned id)
{
    for (unsigned slot = 0; slot < SL_COUNT; ++slot)
        if ((slot < SL_POSE || slot >= SL_POSE + CHR_POSES) && chr_pack_slot(f->whole, slot) == id) return true;
    return false;
}

static const uint8_t *fake_find(void *ctx, unsigned id)
{
    const fake_shelf_t *f = ctx;
    uint32_t at, length;
    if (id >= f->whole->count || !chr_pack_place(f->whole, id, &at, &length)) return NULL;
    if (!resident(f, id) && (missing_for_good(f, id) || f->asked[id] <= f->late)) return NULL;
    return f->pack + at;
}

static chr_shelf_state_t fake_want(void *ctx, const uint16_t *ids, unsigned count, const uint16_t *kept, unsigned kept_count)
{
    fake_shelf_t *f = ctx;
    chr_shelf_state_t all = CHR_SHELF_READY;
    (void)kept;
    (void)kept_count;
    for (unsigned i = 0; i < count; ++i) {
        if (ids[i] == CHR_NO_SPRITE) continue;
        if (!resident(f, ids[i])) ++f->asked[ids[i]];
        if (fake_find(f, ids[i])) continue;
        if (missing_for_good(f, ids[i]) && (ids[i] != f->lost || f->asked[ids[i]] > f->lost_after)) all = CHR_SHELF_FAILED;
        else if (all == CHR_SHELF_READY) all = CHR_SHELF_COMING;
    }
    return all;
}

static unsigned drawn_missing(const fake_shelf_t *f)
{
    unsigned missing = 0;
    for (unsigned n = 0; n < c->item_count; ++n)
        if (c->items[n].sprite) {
            missing += fake_find((void *)f, c->items[n].as.blit.id) == NULL;
            missing += c->items[n].as.blit.patch && !fake_find((void *)f, c->items[n].as.blit.patch - 1U);
        }
    return missing;
}

static void test_late_pictures(void)
{
    size_t size = 0;
    uint8_t *made = fixture_pack(FIXTURE_FLAT | FIXTURE_CLIP, &size, NULL);
    chr_pack_t whole, opened;
    CHECK(chr_pack_open(&whole, made, size) && whole.count <= 64);
    fake_shelf_t fake = {.pack = made, .whole = &whole, .lost = CHR_NO_SPRITE};
    const chr_shelf_t shelf = {.find = fake_find, .want = fake_want, .ctx = &fake};
    CHECK(chr_pack_open_head(&opened, made, size, &shelf) && opened.shelf == &shelf);
    const unsigned dance = chr_pack_slot(&whole, SL_POSE + CHR_GESTURE_DANCE - 1U);
    chr_info_t info;

    /* Late by a good many steps: she stays in her base picture until the first one is there, waits in
     * each picture for the next, and nothing is ever drawn that is not there. */
    fake.late = 12;
    CHECK(chr_init_from(c, W, H, 7, &opened) && has(CHR_FLAT) && has(CHR_HAS_POSES));
    (void)step(400);
    chr_cue(c, CHR_CUE_DANCE, 0);
    unsigned missing = 0, seen = 0, first = 0;
    for (int i = 0; i < 220; ++i) {
        info = step(FRAME);
        missing += drawn_missing(&fake);
        if (info.shown == CHR_GESTURE_DANCE) seen |= 1U << info.frame;
        else if (!seen) ++first;
    }
    CHECK(missing == 0 && first >= 12 && (seen & 7U) == 7U);
    CHECK(step(6000).shown == 0);

    /* One picture in the middle of the dance never comes: she stays in the one before it, and is
     * herself again when the dance is over. If it is the one her way in leads to, she does not dance. */
    for (unsigned which = 1; which <= 2; ++which) {
        chr_frame_t absent;
        CHECK(chr_pack_frame(&whole, which, &absent));
        memset(fake.asked, 0, sizeof(fake.asked));
        fake.late = 0;
        fake.never = 1ULL << absent.sprite;
        CHECK(chr_init_from(c, W, H, 7, &opened));
        (void)step(400);
        chr_cue(c, CHR_CUE_DANCE, 0);
        missing = seen = 0;
        bool over = false;
        for (int i = 0; i < 330; ++i) {
            info = step(FRAME);
            missing += drawn_missing(&fake);
            if (info.shown == CHR_GESTURE_DANCE) seen |= 1U << info.frame;
            over = info.shown == 0 && !info.changing;
        }
        CHECK(missing == 0 && (seen & 7U) == (which == 1U ? 0U : 3U) && over);      /* (bit 3: her way out, which is there) */
    }

    /* The pose itself cannot be had: she keeps to her base picture and goes on living. */
    fake.never = ~0ULL;
    memset(fake.asked, 0, sizeof(fake.asked));
    CHECK(chr_init_from(c, W, H, 7, &opened));
    (void)step(400);
    missing = seen = 0;
    static const chr_cue_t tried[] = {CHR_CUE_DANCE, CHR_CUE_GREET, CHR_CUE_CHEER};
    for (size_t k = 0; k < sizeof(tried) / sizeof(tried[0]); ++k) {
        chr_cue(c, tried[k], 0);
        for (int i = 0; i < 100; ++i) {
            info = step(FRAME);
            missing += drawn_missing(&fake);
            seen |= info.shown != 0;
        }
    }
    CHECK(missing == 0 && !seen && info.items > 0 && fake.asked[dance] > 0);
    /* A pose that fails while she is in another: she leaves that one for her base picture. */
    const unsigned wave = chr_pack_slot(&whole, SL_POSE + CHR_GESTURE_WAVE - 1U);
    fake.never = ~(1ULL << wave);
    CHECK(chr_init_from(c, W, H, 7, &opened));
    (void)step(400);
    chr_cue(c, CHR_CUE_GREET, 0);
    bool waved = false;
    for (int i = 0; i < 50; ++i) waved |= step(FRAME).shown == CHR_GESTURE_WAVE;
    CHECK(waved);
    chr_cue(c, CHR_CUE_CHEER, 0);
    CHECK(step(1800).shown == 0 && drawn_missing(&fake) == 0);
    /* The longest wait for a first picture is one wait: those that were given up do not add up. */
    fake.never = 0;
    fake.late = 1000000;
    memset(fake.asked, 0, sizeof(fake.asked));
    CHECK(chr_init_from(c, W, H, 7, &opened));
    (void)step(400);
    for (int i = 0; i < 6; ++i) {
        chr_cue(c, CHR_CUE_CHEER, 0);
        (void)step(300);
        chr_cue(c, CHR_CUE_GLANCE, 0);
        info = step(300);
    }
    CHECK(info.shown == 0 && info.wait_ms > 100 && info.wait_ms < 350);
    free(made);

    /* On the shipped pack: the picture that thinking leads to fails, at once, after a while, or never comes
     * at all. She is not left half way there, and does not try over and over. */
    size_t shipped_size = 0;
    const uint8_t *shipped = chr_host_pack(&shipped_size);
    chr_clip_t clip;
    chr_frame_t lead, body;
    CHECK(chr_pack_open(&whole, shipped, shipped_size) && whole.count <= 256);
    CHECK(chr_pack_clip(&whole, CHR_GESTURE_THINK, &clip) && clip.enter == 1 && clip.loop + clip.enter > 1);
    CHECK(chr_pack_frame(&whole, clip.first, &lead) && lead.ms <= 150 && chr_pack_frame(&whole, clip.first + 1U, &body));
    static const unsigned until[] = {0, 10, 1000000};
    for (size_t k = 0; k < sizeof(until) / sizeof(until[0]); ++k) {
        fake = (fake_shelf_t){.pack = shipped, .whole = &whole, .lost = body.sprite, .lost_after = until[k]};
        CHECK(chr_pack_open_head(&opened, shipped, shipped_size, &shelf) && chr_init_from(c, W, H, 7, &opened));
        (void)step(400);
        chr_set_phase(c, CHR_PHASE_THINKING);
        unsigned entries = 0, passing = 0, longest = 0;
        missing = 0;
        bool was = false;
        for (int i = 0; i < 900; ++i) {
            info = sounding(FRAME, 0, 0);
            missing += drawn_missing(&fake);
            entries += info.shown && !was;
            was = info.shown != 0;
            passing = info.shown ? passing + FRAME : 0;
            if (passing > longest) longest = passing;
        }
        CHECK(missing == 0 && !info.shown && entries == (until[k] ? 1U : 0U) && longest <= 1600 && info.gesture == CHR_GESTURE_THINK);
        chr_set_phase(c, CHR_PHASE_IDLE);
    }
    chr_no_envelope(c);
}

/* Her face goes with what is heard: each sentence's a moment before it is heard, as far as that is known; the user's
 * voice ends what an answer brought at once; two seconds after her last words she is herself, whatever the service says
 * of its end; once the talk is over a late sentence changes nothing. */
static chr_info_t ahead_frame(uint16_t microphone, uint16_t now, uint16_t ahead)
{
    chr_set_envelope(c, microphone, now);
    chr_set_ahead(c, ahead, true);
    return step(FRAME);
}

/* Nothing is written that far ahead (the driver holds less, or nothing is played). */
static chr_info_t unknown_frame(uint16_t microphone, uint16_t now)
{
    chr_set_envelope(c, microphone, now);
    chr_set_ahead(c, 0, false);
    return step(FRAME);
}

/* Into an answer: listening, thinking for `thinking` ms, then speaking (nothing heard on the way). */
static void answer(unsigned thinking)
{
    chr_set_phase(c, CHR_PHASE_LISTENING);
    for (int i = 0; i < 30; ++i) (void)unknown_frame(0, 0);
    chr_set_phase(c, CHR_PHASE_THINKING);
    for (unsigned t = 0; t < thinking; t += FRAME) (void)unknown_frame(0, 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
}

static void test_timing(void)
{
    const size_t comfort = chr_mood_find("comfort"), serious = chr_mood_find("serious");
    fresh();
    chr_set_phase(c, CHR_PHASE_LISTENING);
    for (int i = 0; i < 30; ++i) (void)ahead_frame(0, 0, 0);
    chr_set_phase(c, CHR_PHASE_THINKING);
    for (int i = 0; i < 30; ++i) (void)ahead_frame(0, 0, 0);
    /* Her answer is sent: its first sentence, comforting, said of no time. Nothing is heard yet: her face waits. */
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_sentence(c, -1, comfort);
    chr_info_t info;
    for (int i = 0; i < 12; ++i) {
        info = ahead_frame(0, 0, 0);
        CHECK(info.mood != comfort);
    }
    /* A sound is about to be heard (three frames ahead): her face changes now, before it. */
    info = ahead_frame(0, 0, 9000);
    CHECK(info.mood == comfort);
    (void)ahead_frame(0, 0, 9000);
    (void)ahead_frame(0, 0, 9000);
    /* Heard from here on. A second sentence, serious, 1500 ms into the answer, is told as the first is heard: it shows
     * 100 ms before its time, not when it came. */
    chr_sentence(c, 1500, serious);
    unsigned shown_at = 0;
    for (unsigned t = 0; t < 2500 && !shown_at; t += FRAME) {
        info = ahead_frame(0, 9000, 9000);
        if (info.mood == serious) shown_at = t;
    }
    CHECK(shown_at + FRAME >= 1400 && shown_at <= 1400 + FRAME);
    /* Her last words: two seconds after them she is herself again, though the service has not said her answer ended. */
    unsigned back = 0;
    for (unsigned t = 0; t < 4000 && !back; t += FRAME) {
        info = ahead_frame(0, 0, 0);
        if (info.mood == CHR_MOOD_NEUTRAL) back = t + FRAME;
    }
    CHECK(back >= 2000 && back <= 2600);
    /* A sentence whose time comes in a pause before it (the service counts the pause in): she waits for its sound, and
     * changes as it is about to be heard; one told after its time is shown at once. */
    chr_set_phase(c, CHR_PHASE_THINKING);
    for (int i = 0; i < 30; ++i) (void)ahead_frame(0, 0, 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_sentence(c, -1, CHR_MOOD_NEUTRAL);
    chr_sentence(c, 900, comfort);
    unsigned changed = 0;
    for (unsigned t = 0; t < 2000 && !changed; t += FRAME) {
        /* heard: 0..700 ms, a pause, again from 1200 ms; known three frames ahead */
        const uint16_t now = t < 700 || t >= 1200 ? 9000 : 0, ahead = t + 3U * FRAME < 700 || t + 3U * FRAME >= 1200 ? 9000 : 0;
        info = ahead_frame(0, now, ahead);
        if (info.mood == comfort) changed = t;
    }
    CHECK(changed + 4U * FRAME >= 1200 && changed < 1200);
    chr_sentence(c, 200, serious);
    info = ahead_frame(0, 9000, 9000);
    CHECK(info.mood == serious);
    for (int i = 0; i < 90; ++i) (void)ahead_frame(0, 0, 0);
    /* Another answer, comforting; listening after it, the user begins to speak: her face is her own in a few frames. */
    chr_set_phase(c, CHR_PHASE_THINKING);
    for (int i = 0; i < 30; ++i) (void)ahead_frame(0, 0, 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_sentence(c, -1, comfort);
    for (int i = 0; i < 30; ++i) info = ahead_frame(0, 9000, 9000);
    CHECK(info.mood == comfort);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    for (int i = 0; i < 6; ++i) info = ahead_frame(0, 0, 0);
    CHECK(info.mood == comfort);
    unsigned undone = 0;
    for (unsigned t = 0; t < 600 && !undone; t += FRAME) {
        info = ahead_frame(1400, 0, 0);
        if (info.mood == CHR_MOOD_NEUTRAL) undone = t + FRAME;
    }
    CHECK(undone && undone <= 250);
    /* What is left of her own voice after the echo is cancelled is no user. */
    chr_set_phase(c, CHR_PHASE_THINKING);
    for (int i = 0; i < 30; ++i) (void)ahead_frame(0, 0, 0);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_sentence(c, -1, comfort);
    for (int i = 0; i < 30; ++i) (void)ahead_frame(150, 9000, 9000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    for (int i = 0; i < 20; ++i) info = ahead_frame(150, 0, 0);
    CHECK(info.mood == comfort);
    /* Over: a sentence that comes late changes nothing. */
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 150; ++i) (void)ahead_frame(0, 0, 0);
    const size_t after = ahead_frame(0, 0, 0).mood;
    chr_sentence(c, -1, serious);
    for (int i = 0; i < 60; ++i) info = ahead_frame(0, 9000, 9000);
    CHECK(info.mood == after && info.mood != serious);
    chr_no_envelope(c);
}

/* Her answer's clock in what the bench does not show: time off the screen, a speaker that is off, a driver that holds
 * less than she looks ahead, an expression the AI asks for, a sentence of no time amid speech, an answer that comes
 * without her thinking first. */
static void test_answer_clock(void)
{
    const size_t comfort = chr_mood_find("comfort"), serious = chr_mood_find("serious"), happy = chr_mood_find("happy"),
                 gentle = chr_mood_find("gentle");
    chr_info_t info;

    /* Off the screen for six seconds of her answer (a dialog over her): the time is hers too. What was heard
     * meanwhile is over when she is back, the last of it shows (not each in turn), and what comes later at its time. */
    fresh();
    answer(900);
    chr_sentence(c, 0, comfort);
    chr_sentence(c, 2000, serious);
    chr_sentence(c, 4000, happy);
    chr_sentence(c, 8000, gentle);
    for (int i = 0; i < 30; ++i) info = ahead_frame(0, 9000, 9000);       /* 990 ms heard */
    CHECK(info.mood == comfort);
    chr_pass(c, 6000);
    bool flipped = false;
    unsigned at = 990 + 6000, gentle_at = 0;
    for (int i = 0; i < 3; ++i) {
        info = ahead_frame(0, 9000, 9000);
        at += FRAME;
        flipped |= info.mood == serious;
    }
    CHECK(info.mood == happy && !flipped);
    for (; at < 10000 && !gentle_at; at += FRAME)
        if (ahead_frame(0, 9000, 9000).mood == gentle) gentle_at = at + FRAME;
    CHECK(gentle_at >= 7900 && gentle_at <= 7900 + 2U * FRAME);
    /* Two told while she was away, one of no time and one whose time has come as she is back: the later only, with
     * one little effect. */
    for (int i = 0; i < 20; ++i) (void)ahead_frame(0, 9000, 9000);
    chr_pass(c, 3000);
    chr_sentence(c, -1, comfort);
    chr_sentence(c, (int32_t)(c->now - c->answer_at) + 50, serious);
    unsigned effects = 0;
    bool comforted = false;
    for (int i = 0; i < 20; ++i) {
        info = ahead_frame(0, 9000, 9000);
        effects += c->sentence_ms < (float)FRAME + 1.0f;
        comforted |= info.mood == comfort;
    }
    CHECK(info.mood == serious && !comforted && effects == 1);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 90; ++i) (void)ahead_frame(0, 0, 0);
    /* The answer ends while she is off the screen, its last sentence still on its way to her: back, she is listening,
     * and that sentence's face, which would be over by now, does not come. */
    fresh();
    answer(900);
    chr_sentence(c, -1, comfort);
    for (int i = 0; i < 30; ++i) info = ahead_frame(0, 9000, 9000);
    CHECK(info.mood == comfort);
    chr_pass(c, 6000);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    chr_sentence(c, 1200, gentle);
    chr_sentence(c, -1, happy);     /* nor one of no time: it was heard while she was away */
    flipped = false;
    effects = 0;
    for (int i = 0; i < 150; ++i) {
        info = unknown_frame(0, 0);
        flipped |= info.mood == gentle || info.mood == happy;
        effects += c->sentence_ms < (float)FRAME + 1.0f;
    }
    CHECK(!flipped && !effects && c->sentence_count == 0 && unknown_frame(0, 0).mood == CHR_MOOD_NEUTRAL);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 90; ++i) (void)ahead_frame(0, 0, 0);

    /* The speaker is off: nothing of her answer is heard, nothing is written ahead. Her face still follows its
     * sentences, a second after the first came and then as each comes, without the little effect of a start, and
     * holds between them however far apart; the hand at her chin comes down; two seconds after she has stopped
     * speaking she is herself. */
    fresh();
    answer(3600);
    CHECK(unknown_frame(0, 0).shown == CHR_GESTURE_THINK || !has(CHR_HAS_POSES));
    chr_sentence(c, 0, comfort);
    unsigned comfort_at = 0, serious_at = 0, down_at = 0, own_at = 0;
    bool between = false;
    for (unsigned t = 0; t < 7000; t += FRAME) {
        if (t >= 4500 && t < 4500 + FRAME) chr_sentence(c, 4500, serious);
        info = unknown_frame(0, 0);
        if (!comfort_at && info.mood == comfort) comfort_at = t + FRAME;
        if (!serious_at && info.mood == serious) {
            serious_at = t + FRAME;
            CHECK(c->sentence_ms > 2.0f * FRAME);
        }
        between |= comfort_at && !serious_at && info.mood != comfort;
        if (!down_at && info.gesture != CHR_GESTURE_THINK) down_at = t + FRAME;
    }
    CHECK(comfort_at >= 1000 && comfort_at <= 1000 + 2U * FRAME);
    CHECK(serious_at >= 4500 && serious_at <= 4500 + 2U * FRAME);
    CHECK(!between && info.mood == serious);
    CHECK(down_at && down_at <= 1500);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    for (unsigned t = 0; t < 4000 && !own_at; t += FRAME)
        if (unknown_frame(0, 0).mood == CHR_MOOD_NEUTRAL) own_at = t + FRAME;
    CHECK(own_at >= 2000 && own_at <= 2600);
    /* An answer with no sentence at all: the hand comes down all the same. */
    answer(3600);
    down_at = 0;
    for (unsigned t = 0; t < 3000 && !down_at; t += FRAME)
        if (unknown_frame(0, 0).gesture != CHR_GESTURE_THINK) down_at = t + FRAME;
    CHECK(down_at && down_at <= 1500 + FRAME);
    /* A first sound that comes late after all (more than a second after the first sentence): the next ones are timed
     * from it, with their little effect. */
    answer(900);
    chr_sentence(c, 0, comfort);
    chr_sentence(c, 1500, serious);
    serious_at = 0;
    for (unsigned t = 0; t < 4000 && !serious_at; t += FRAME) {
        info = t < 1188 ? unknown_frame(0, 0) : ahead_frame(0, 9000, 9000);
        if (info.mood == serious) {
            serious_at = t + FRAME;
            CHECK(c->sentence_ms < 2.0f * FRAME);
        }
    }
    CHECK(serious_at + FRAME >= 1188 + 1400 && serious_at <= 1188 + 1400 + 2U * FRAME);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 90; ++i) (void)unknown_frame(0, 0);

    /* Syllables with stops of 60 ms between them: her mouth does not shut in them, whether what is ahead is known (it
     * shows the stop, no pause) or nothing is written that far (a driver that holds less); it does in a real pause. */
    for (int known = 0; known < 2; ++known) {
        fresh();
        answer(900);
        unsigned shut = 0, open = 0, shut_at = 0;
        for (unsigned t = 0; t < 3600; t += FRAME) {
            const bool voice = t < 3000 && t % 300 < 240, later = t + 100 < 3000 && (t + 100) % 300 < 240;
            chr_set_envelope(c, 0, voice ? 9000 : 0);
            chr_set_ahead(c, known && later ? 9000 : 0, known != 0);
            info = step(FRAME);
            if (t >= 300 && t < 2940) {
                shut += info.mouth_open == 0;
                open += info.mouth_open > 0;
            }
            if (t >= 3000 && !shut_at && info.mouth_open == 0) shut_at = t + FRAME;
        }
        CHECK(shut == 0 && open > 60);
        /* The pause that follows: early where it is known, after 90 ms where it is not. */
        CHECK(shut_at && shut_at <= 3000U + (known ? 2U : 5U) * FRAME);
        chr_set_phase(c, CHR_PHASE_IDLE);
        for (int i = 0; i < 90; ++i) (void)unknown_frame(0, 0);
    }

    /* After an answer, nothing is written ahead while she listens and thinks (that is no sound either): the next
     * answer's first sound is still seen coming, and its face changes before it is heard. */
    fresh();
    answer(900);
    chr_sentence(c, 0, serious);
    for (int i = 0; i < 30; ++i) (void)ahead_frame(0, 9000, 9000);
    (void)ahead_frame(0, 9000, 0);
    answer(1500);
    chr_sentence(c, 0, comfort);
    CHECK(unknown_frame(0, 0).mood != comfort);
    CHECK(ahead_frame(0, 0, 9000).mood == comfort);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 90; ++i) (void)unknown_frame(0, 0);

    /* The AI asks for a face (happy) while a sentence (comfort) is told and not yet heard: heard, it shows the face that
     * was asked for. One told while that face lasts (serious) and heard after it is over shows its own; as it ends,
     * the face is that of the sentence being heard. */
    fresh();
    answer(900);
    chr_sentence(c, 0, comfort);
    (void)ahead_frame(0, 0, 0);
    chr_sentence(c, CHR_SENTENCE_NOW, happy);
    CHECK(ahead_frame(0, 0, 0).mood == happy);
    (void)ahead_frame(0, 0, 9000);
    for (int i = 0; i < 10; ++i) info = ahead_frame(0, 9000, 9000);
    CHECK(info.mood == happy && c->sentence_count == 0);
    chr_sentence(c, 1500, serious);
    for (int i = 0; i < 10; ++i) info = ahead_frame(0, 9000, 9000);
    CHECK(info.mood == happy);
    chr_sentence(c, CHR_SENTENCE_OWN, serious);
    CHECK(ahead_frame(0, 9000, 9000).mood == comfort);
    serious_at = 0;
    for (unsigned t = 0; t < 2000 && !serious_at; t += FRAME)
        if (ahead_frame(0, 9000, 9000).mood == serious) serious_at = t;
    CHECK(serious_at);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 90; ++i) (void)unknown_frame(0, 0);

    /* A sentence the service said no time of, not the answer's first, amid speech with no pause long enough to begin
     * it: it shows half a second after it came, and two seconds after her last words she is herself. */
    fresh();
    answer(900);
    chr_sentence(c, -1, comfort);
    serious_at = 0;
    for (unsigned t = 0; t < 3000; t += FRAME) {
        if (t >= 990 && t < 990 + FRAME) chr_sentence(c, -1, serious);
        const bool voice = t % 400 < 300, later = (t + 100) % 400 < 300;
        info = ahead_frame(0, voice ? 9000 : 0, later ? 9000 : 0);
        if (!serious_at && info.mood == serious) serious_at = t + FRAME;
    }
    CHECK(serious_at >= 990 + 500 && serious_at <= 990 + 500 + 2U * FRAME);
    own_at = 0;
    for (unsigned t = 0; t < 4000 && !own_at; t += FRAME)
        if (ahead_frame(0, 0, 0).mood == CHR_MOOD_NEUTRAL) own_at = t + FRAME;
    CHECK(own_at >= 2000 && own_at <= 2700);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 90; ++i) (void)unknown_frame(0, 0);

    /* An answer that comes after the last without her thinking (speaking, listening, speaking): its first sentence
     * waits for its own first sound, with its little effect, and the last answer's face does not come back meanwhile. */
    fresh();
    answer(900);
    chr_sentence(c, -1, serious);
    for (int i = 0; i < 30; ++i) info = ahead_frame(0, 9000, 9000);
    CHECK(info.mood == serious);
    chr_set_phase(c, CHR_PHASE_LISTENING);
    for (int i = 0; i < 90; ++i) info = ahead_frame(0, 0, 0);
    CHECK(info.mood == CHR_MOOD_NEUTRAL);
    chr_set_phase(c, CHR_PHASE_SPEAKING);
    chr_sentence(c, 0, comfort);
    for (int i = 0; i < 4; ++i) {
        info = ahead_frame(0, 0, 0);
        CHECK(info.mood == CHR_MOOD_NEUTRAL);
    }
    info = ahead_frame(0, 0, 9000);
    CHECK(info.mood == comfort && c->sentence_ms < 2.0f * FRAME);
    chr_set_phase(c, CHR_PHASE_IDLE);
    for (int i = 0; i < 90; ++i) (void)unknown_frame(0, 0);
    chr_no_envelope(c);
}

static void suite(const char *figure)
{
    const int before = failures;
    test_pack();
    test_shapes();
    test_moods();
    test_motion();
    test_talk();
    test_mouth();
    test_spoken_moods();
    test_parting();
    test_mood_after_talk();
    test_voice_beside();
    test_talk_poses();
    test_still();
    test_changes();
    test_conditions();
    test_ponder();
    test_lady();
    test_touch();
    test_feeling();
    test_return();
    test_restraint();
    test_hands();
    test_damage();
    test_timing();
    test_answer_clock();
    if (failures != before) fprintf(stderr, "(the checks above failed on the %s figure)\n", figure);
}

int main(void)
{
    c = malloc(chr_size());
    pack = chr_host_pack(&pack_size);
    CHECK(c && pack && pack_size > 1000);
    suite("shipped");
    uint8_t *fox = fixture_pack(FIXTURE_EARS | FIXTURE_TAIL, &pack_size, NULL);
    pack = fox;
    suite("fixture with ears and a tail");
    free(fox);
    uint8_t *armed = fixture_pack(FIXTURE_ARMS | FIXTURE_YAW, &pack_size, NULL);
    pack = armed;
    suite("fixture with arms and a turning head");
    free(armed);
    test_figures();
    pack = chr_host_pack(&pack_size);
    test_clips();
    test_late_pictures();
    free(c);
    if (failures) {
        fprintf(stderr, "FAIL: theme_character_core: %d checks failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("PASS: theme_character_core\n");
    return EXIT_SUCCESS;
}
