// SPDX-License-Identifier: MIT
/* The pack behind an image on a slow medium: it is found from the image's own headers, what must
 * stay in memory does, a pose is read when it is wanted and checked, the pictures being drawn are
 * never given up, and damage anywhere ends in "no picture" and never in a wrong one. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chr.h"
#include "chr_fixture.h"
#include "chr_shelf.h"
#include "xiaotai/port/os.h"

static int failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

enum { WAIT = 4000, PAGE = 0x10000 };

/* A partition: an application image, and a pack on the next page behind it. */
typedef struct {
    uint8_t *bytes;
    uint32_t size, pack_at, image_end;
    atomic_uint reads, fail_from;       /* reads at and past fail_from fail; 0: none does */
    atomic_bool slow;
} media_t;

static uint32_t media_open(void *ctx) { return ((media_t *)ctx)->size; }

static bool media_read(void *ctx, uint32_t at, void *to, size_t bytes)
{
    media_t *m = ctx;
    atomic_fetch_add(&m->reads, 1U);
    const unsigned fail_from = atomic_load(&m->fail_from);
    if ((fail_from && at >= fail_from) || at > m->size || bytes > m->size - at) return false;
    if (atomic_load(&m->slow)) xiaotai_delay_ms(30);
    memcpy(to, m->bytes + at, bytes);
    return true;
}

static media_t *media_make(const uint8_t *pack, size_t size, unsigned segments, bool hashed)
{
    media_t *m = calloc(1, sizeof(*m));
    m->size = 4U * PAGE + (uint32_t)size;
    m->bytes = malloc(m->size);
    memset(m->bytes, 0xFF, m->size);
    uint32_t at = 24;
    memset(m->bytes, 0, 24);
    m->bytes[0] = 0xE9;
    m->bytes[1] = (uint8_t)segments;
    m->bytes[23] = hashed;
    for (unsigned i = 0; i < segments; ++i) {
        const uint32_t load = 0x40000000U, length = 1000U + 37U * i;
        memcpy(m->bytes + at, &load, 4);
        memcpy(m->bytes + at + 4, &length, 4);
        at += 8U + length;
    }
    at = (at + 16U) & ~15U;
    if (hashed) at += 32U;
    m->image_end = at;
    m->pack_at = (at + PAGE - 1U) / PAGE * PAGE;
    if (pack) memcpy(m->bytes + m->pack_at, pack, size);
    return m;
}

static void media_free(media_t *m)
{
    free(m->bytes);
    free(m);
}

static chr_shelves_t *shelves_of(media_t *m)
{
    const chr_medium_t medium = {.open = media_open, .read = media_read, .ctx = m};
    return chr_shelves_open(&medium, WAIT);
}

static chr_shelf_state_t want(const chr_pack_t *p, unsigned a, unsigned b, unsigned keep)
{
    const uint16_t ids[2] = {(uint16_t)a, (uint16_t)b}, kept[1] = {(uint16_t)keep};
    return p->shelf->want(p->shelf->ctx, ids, 2, kept, 1);
}

static chr_shelf_state_t settle(const chr_pack_t *p, unsigned a, unsigned b, unsigned keep)
{
    chr_shelf_state_t state = CHR_SHELF_COMING;
    for (int i = 0; i < 400 && (state = want(p, a, b, keep)) == CHR_SHELF_COMING; ++i) xiaotai_delay_ms(5);
    return state;
}

static const uint8_t *find(const chr_pack_t *p, unsigned id) { return p->shelf->find(p->shelf->ctx, id); }

static void test_finding(const uint8_t *pack, size_t size)
{
    for (unsigned segments = 0; segments < 5; ++segments)
        for (int hashed = 0; hashed < 2; ++hashed) {
            media_t *m = media_make(pack, size, segments, hashed);
            chr_medium_t medium = {.open = media_open, .read = media_read, .ctx = m, .size = m->size};
            CHECK(chr_image_end(&medium) == m->image_end);
            chr_shelves_t *s = shelves_of(m);
            chr_shelves_info_t info;
            CHECK(s != NULL);
            chr_shelves_info(s, &info);
            CHECK(info.at == m->pack_at && info.bytes == size && info.places > 0 && info.resident > 0 && info.resident < size);
            chr_shelves_close(s);
            media_free(m);
        }
    /* Nothing behind the image; no image; a partition that ends too early; a pack cut short by its end. */
    media_t *m = media_make(NULL, size, 2, true);
    CHECK(shelves_of(m) == NULL);
    media_free(m);
    m = media_make(pack, size, 2, true);
    m->bytes[0] = 0;
    CHECK(shelves_of(m) == NULL);
    m->bytes[0] = 0xE9;
    m->size = m->pack_at + (uint32_t)size - 1U;
    CHECK(shelves_of(m) == NULL);
    m->size = m->pack_at + 8U;
    CHECK(shelves_of(m) == NULL);
    m->size = 10;
    CHECK(shelves_of(m) == NULL);
    m->size = 0;
    CHECK(shelves_of(m) == NULL);
    m->size = m->pack_at + (uint32_t)size;
    const uint32_t beyond = 0x7FFFFFFFU;        /* a segment longer than the partition */
    memcpy(m->bytes + 28, &beyond, 4);
    CHECK(shelves_of(m) == NULL);
    media_free(m);
    /* Damage: one byte of the head, one of her base picture; a medium that cannot be read that far. */
    m = media_make(pack, size, 2, true);
    m->bytes[m->pack_at + 40] ^= 1;
    CHECK(shelves_of(m) == NULL);
    m->bytes[m->pack_at + 40] ^= 1;
    chr_pack_t whole;
    uint32_t start, length;
    CHECK(chr_pack_open(&whole, pack, size) && chr_pack_place(&whole, chr_pack_slot(&whole, SL_BODY), &start, &length));
    m->bytes[m->pack_at + start + length / 2U] ^= 4;
    CHECK(shelves_of(m) == NULL);
    m->bytes[m->pack_at + start + length / 2U] ^= 4;
    atomic_store(&m->fail_from, m->pack_at + 64U);
    CHECK(shelves_of(m) == NULL);
    atomic_store(&m->fail_from, 0);
    chr_shelves_t *s = shelves_of(m);
    CHECK(s != NULL);
    chr_shelves_close(s);
    media_free(m);
}

static void test_reading(const uint8_t *pack, size_t size)
{
    chr_pack_t whole;
    CHECK(chr_pack_open(&whole, pack, size));
    media_t *m = media_make(pack, size, 3, true);
    chr_shelves_t *s = shelves_of(m);
    const chr_pack_t *p = chr_shelves_pack(s);
    CHECK(p && p->count == whole.count);
    if (!p) return;
    /* What is not a pose is there from the start; what is, is not. */
    const unsigned body = chr_pack_slot(p, SL_BODY), wave = chr_pack_slot(p, SL_POSE + CHR_GESTURE_WAVE - 1U);
    const unsigned dance = chr_pack_slot(p, SL_POSE + CHR_GESTURE_DANCE - 1U), cheer = chr_pack_slot(p, SL_POSE + CHR_GESTURE_CHEER - 1U);
    const unsigned patch = chr_pack_slot(p, FIXTURE_PATCH_SLOT);
    CHECK(find(p, body) && chr_pack_intact(&whole, body, find(p, body)));
    CHECK(!find(p, wave) && !find(p, dance) && !find(p, p->count) && !find(p, CHR_NO_SPRITE));
    CHECK(want(p, body, CHR_NO_SPRITE, CHR_NO_SPRITE) == CHR_SHELF_READY);
    CHECK(want(p, p->count, CHR_NO_SPRITE, CHR_NO_SPRITE) == CHR_SHELF_FAILED);
    /* A pose comes when it is wanted, byte for byte, and the asking does not wait for it. */
    atomic_store(&m->slow, true);
    CHECK(want(p, wave, CHR_NO_SPRITE, CHR_NO_SPRITE) == CHR_SHELF_COMING && !find(p, wave));
    CHECK(want(p, wave, CHR_NO_SPRITE, CHR_NO_SPRITE) == CHR_SHELF_COMING);
    atomic_store(&m->slow, false);
    CHECK(settle(p, wave, CHR_NO_SPRITE, CHR_NO_SPRITE) == CHR_SHELF_READY);
    uint32_t start, length;
    CHECK(chr_pack_place(p, wave, &start, &length) && find(p, wave) && !memcmp(find(p, wave), pack + start, length));
    /* Two at once: a picture and the one it is a patch over. */
    CHECK(settle(p, patch, cheer, wave) == CHR_SHELF_READY && find(p, patch) && find(p, cheer) && find(p, wave));
    /* More than there is room for: what is kept stays where it is, however much else comes by. */
    const uint8_t *held = find(p, wave);
    unsigned others = 0;
    chr_frame_t frame;
    for (int round = 0; round < 3; ++round)
        for (unsigned i = 0; chr_pack_frame(p, i, &frame); ++i) {
            CHECK(settle(p, frame.sprite, frame.under, wave) == CHR_SHELF_READY);
            CHECK(find(p, frame.sprite) && chr_pack_intact(&whole, frame.sprite, find(p, frame.sprite)));
            CHECK(find(p, wave) == held && chr_pack_intact(&whole, wave, held));
            ++others;
        }
    CHECK(others >= 12);
    /* Not kept any more, it makes room. */
    bool gone = false;
    for (unsigned i = 0; chr_pack_frame(p, i, &frame) && !gone; ++i) {
        CHECK(settle(p, frame.sprite, CHR_NO_SPRITE, CHR_NO_SPRITE) == CHR_SHELF_READY);
        gone = !find(p, wave);
    }
    CHECK(gone);
    chr_shelves_info_t info;
    chr_shelves_info(s, &info);
    CHECK(info.loads >= 12 && info.failed == 0);
    chr_shelves_close(s);
    media_free(m);
}

static void test_damage(const uint8_t *pack, size_t size)
{
    chr_pack_t whole;
    CHECK(chr_pack_open(&whole, pack, size));
    const unsigned wave = chr_pack_slot(&whole, SL_POSE + CHR_GESTURE_WAVE - 1U);
    const unsigned dance = chr_pack_slot(&whole, SL_POSE + CHR_GESTURE_DANCE - 1U);
    uint32_t start, length;
    CHECK(chr_pack_place(&whole, wave, &start, &length));
    media_t *m = media_make(pack, size, 1, true);
    m->bytes[m->pack_at + start + length - 1U] ^= 0x80;
    chr_shelves_t *s = shelves_of(m);
    const chr_pack_t *p = chr_shelves_pack(s);
    CHECK(p != NULL);
    if (!p) return;
    /* A damaged picture is tried a few times, then not any more; it is never handed out. */
    CHECK(settle(p, wave, CHR_NO_SPRITE, CHR_NO_SPRITE) == CHR_SHELF_FAILED && !find(p, wave));
    const unsigned reads = atomic_load(&m->reads);
    for (int i = 0; i < 20; ++i) CHECK(want(p, wave, CHR_NO_SPRITE, CHR_NO_SPRITE) == CHR_SHELF_FAILED);
    xiaotai_delay_ms(30);
    CHECK(atomic_load(&m->reads) == reads);
    chr_shelves_info_t info;
    chr_shelves_info(s, &info);
    CHECK(info.failed == 3);
    /* The others are not affected. */
    CHECK(settle(p, dance, CHR_NO_SPRITE, CHR_NO_SPRITE) == CHR_SHELF_READY && chr_pack_intact(&whole, dance, find(p, dance)));
    /* A read that fails once and then works is made up for. */
    const unsigned cheer = chr_pack_slot(&whole, SL_POSE + CHR_GESTURE_CHEER - 1U);
    atomic_store(&m->fail_from, 1U);
    CHECK(want(p, cheer, CHR_NO_SPRITE, dance) == CHR_SHELF_COMING);
    xiaotai_delay_ms(60);
    atomic_store(&m->fail_from, 0U);
    CHECK(settle(p, cheer, CHR_NO_SPRITE, dance) == CHR_SHELF_READY && find(p, cheer));
    chr_shelves_close(s);

    /* Closed while it reads: it ends, and nothing is left behind. */
    atomic_store(&m->slow, true);
    s = shelves_of(m);
    p = chr_shelves_pack(s);
    CHECK(p && want(p, dance, cheer, CHR_NO_SPRITE) == CHR_SHELF_COMING);
    chr_shelves_close(s);
    media_free(m);
}

/* The character on a slow medium: every picture she is drawn with is there when she is drawn. */
static void test_character(const uint8_t *pack, size_t size)
{
    media_t *m = media_make(pack, size, 2, true);
    chr_shelves_t *s = shelves_of(m);
    const chr_pack_t *p = chr_shelves_pack(s);
    chr_t *c = malloc(chr_size());
    CHECK(p && chr_init_from(c, 1024, 600, 7, p));
    atomic_store(&m->slow, true);
    static const chr_cue_t cues[] = {CHR_CUE_DANCE, CHR_CUE_GREET, CHR_CUE_CHEER, CHR_CUE_DANCE};
    unsigned shown = 0, missing = 0;
    for (size_t k = 0; k < sizeof(cues) / sizeof(cues[0]); ++k) {
        chr_cue(c, cues[k], 0);
        for (int i = 0; i < 130; ++i) {
            chr_rect_t damage[CHR_DAMAGE_MAX];
            chr_info_t info;
            (void)chr_step(c, 33, damage);
            chr_inspect(c, &info);
            shown |= 1U << info.shown;
            for (unsigned n = 0; n < c->item_count; ++n)
                if (c->items[n].sprite) {
                    missing += find(p, c->items[n].as.blit.id) == NULL;
                    missing += c->items[n].as.blit.patch && find(p, c->items[n].as.blit.patch - 1U) == NULL;
                }
            xiaotai_delay_ms(2);
        }
    }
    CHECK(missing == 0);
    CHECK((shown & 1U << CHR_GESTURE_DANCE) && (shown & 1U << CHR_GESTURE_WAVE) && (shown & 1U << CHR_GESTURE_CHEER));
    free(c);
    chr_shelves_close(s);
    media_free(m);
}

int main(void)
{
    size_t size = 0;
    uint8_t *pack = fixture_pack(FIXTURE_FLAT | FIXTURE_CLIP, &size, NULL);
    test_finding(pack, size);
    test_reading(pack, size);
    test_damage(pack, size);
    test_character(pack, size);
    free(pack);
    if (failures) {
        fprintf(stderr, "FAIL: theme_character_shelf: %d checks failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("PASS: theme_character_shelf\n");
    return EXIT_SUCCESS;
}
