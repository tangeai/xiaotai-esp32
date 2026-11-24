// SPDX-License-Identifier: MIT
/* The sound index: registration checks (atomic: a refused pack changes
 * nothing), override order (the last pack that has a name answers), lookups
 * that stay valid, enumeration of distinct names in first-appearance order,
 * digit names, the pack limit, reset, the selected built-ins at engine init,
 * and no port allocation anywhere. */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/sounds.h"
#include "xiaotai/sounds_config.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

static const uint8_t k_bytes[64] = {1, 2, 3, 4};

#define ENTRY(entry_name, codec)                                                                 \
    {.name = (entry_name), .encoding = (codec), .container = XIAOTAI_SOUND_RAW, .rate_hz = 8000, \
     .channels = 1, .data = k_bytes, .size = 2}

/* One-entry pack built on the stack around a mutable entry. */
static xiaotai_err_t register_one(const xiaotai_sound_t *entry)
{
    const xiaotai_sound_pack_t pack = {.name = "probe", .sounds = entry, .count = 1};
    xiaotai_err_t err = xiaotai_sounds_register(&pack);
    /* The pack lives on this stack frame: never keep it registered. */
    xiaotai_sounds_reset();
    return err;
}

static int test_digits(void)
{
    const char *expected[10] = {"digit.0", "digit.1", "digit.2", "digit.3", "digit.4",
                                "digit.5", "digit.6", "digit.7", "digit.8", "digit.9"};
    for (unsigned d = 0; d < 10; ++d) {
        CHECK(xiaotai_sounds_digit(d) != NULL);
        CHECK(!strcmp(xiaotai_sounds_digit(d), expected[d]));
        CHECK(xiaotai_sounds_digit(d) == xiaotai_sounds_digit(d)); /* static, not rebuilt */
    }
    CHECK(xiaotai_sounds_digit(10) == NULL);
    CHECK(xiaotai_sounds_digit(UINT_MAX) == NULL);
    return 0;
}

static int test_entry_checks(void)
{
    xiaotai_sounds_reset();
    /* Pack-level refusals. */
    const xiaotai_sound_t one = ENTRY("cue.tap", XIAOTAI_CODEC_PCM16);
    CHECK(xiaotai_sounds_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_sound_pack_t no_name = {.name = NULL, .sounds = &one, .count = 1};
    const xiaotai_sound_pack_t no_table = {.name = "p", .sounds = NULL, .count = 1};
    const xiaotai_sound_pack_t empty = {.name = "p", .sounds = &one, .count = 0};
    CHECK(xiaotai_sounds_register(&no_name) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_sounds_register(&no_table) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_sounds_register(&empty) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_sounds_count() == 0);

    /* Every audio codec of the vocabulary is accepted. */
    const xiaotai_codec_t audio[] = {XIAOTAI_CODEC_PCM16, XIAOTAI_CODEC_G711A, XIAOTAI_CODEC_G711U,
                                     XIAOTAI_CODEC_OPUS};
    for (size_t i = 0; i < COUNT_OF(audio); ++i) {
        xiaotai_sound_t entry = ENTRY("cue.tap", audio[i]);
        CHECK(register_one(&entry) == XIAOTAI_OK);
    }
    /* NONE (a zero-filled entry), video and unknown values are refused. */
    const int not_audio[] = {XIAOTAI_CODEC_NONE, XIAOTAI_CODEC_JPEG, XIAOTAI_CODEC_H264, 7, 99};
    for (size_t i = 0; i < COUNT_OF(not_audio); ++i) {
        xiaotai_sound_t entry = ENTRY("cue.tap", (xiaotai_codec_t)not_audio[i]);
        CHECK(register_one(&entry) == XIAOTAI_ERR_INVALID_ARG);
    }
    xiaotai_sound_t zero = {0};
    CHECK(register_one(&zero) == XIAOTAI_ERR_INVALID_ARG);

    /* Entries without name, data or size. */
    xiaotai_sound_t entry = ENTRY(NULL, XIAOTAI_CODEC_PCM16);
    CHECK(register_one(&entry) == XIAOTAI_ERR_INVALID_ARG);
    entry = (xiaotai_sound_t)ENTRY("cue.tap", XIAOTAI_CODEC_PCM16);
    entry.data = NULL;
    CHECK(register_one(&entry) == XIAOTAI_ERR_INVALID_ARG);
    entry = (xiaotai_sound_t)ENTRY("cue.tap", XIAOTAI_CODEC_PCM16);
    entry.size = 0;
    CHECK(register_one(&entry) == XIAOTAI_ERR_INVALID_ARG);

    /* Size limit: exactly XIAOTAI_SOUND_BYTES_MAX is accepted (registration
     * never reads the data), one more byte is not. */
    entry = (xiaotai_sound_t)ENTRY("cue.tap", XIAOTAI_CODEC_PCM16);
    entry.size = XIAOTAI_SOUND_BYTES_MAX;
    CHECK(register_one(&entry) == XIAOTAI_OK);
    entry.size = XIAOTAI_SOUND_BYTES_MAX + 1U;
    CHECK(register_one(&entry) == XIAOTAI_ERR_INVALID_ARG);

    /* Name length: 31 characters fit, 32 do not. */
    char name[XIAOTAI_SOUND_NAME_MAX + 2];
    memset(name, 'a', sizeof name);
    name[XIAOTAI_SOUND_NAME_MAX] = '\0';
    CHECK(strlen(name) == 31);
    entry = (xiaotai_sound_t)ENTRY(name, XIAOTAI_CODEC_PCM16);
    CHECK(register_one(&entry) == XIAOTAI_OK);
    name[XIAOTAI_SOUND_NAME_MAX] = 'a';
    name[XIAOTAI_SOUND_NAME_MAX + 1] = '\0';
    CHECK(strlen(name) == 32);
    CHECK(register_one(&entry) == XIAOTAI_ERR_INVALID_ARG);

    /* Name form: lower case, digits and '_' in non-empty dot-separated
     * segments. */
    const char *good[] = {"ring.incoming", "guide.wifi_setup", "digit.0", "a", "x.y.z", "a_1.b2"};
    for (size_t i = 0; i < COUNT_OF(good); ++i) {
        entry = (xiaotai_sound_t)ENTRY(good[i], XIAOTAI_CODEC_PCM16);
        CHECK(register_one(&entry) == XIAOTAI_OK);
    }
    const char *bad[] = {"", "Ring.incoming", "ring.", ".ring", "ring..incoming", "ring incoming",
                         "ring-incoming", "ring/incoming", "ring.incoming\n"};
    for (size_t i = 0; i < COUNT_OF(bad); ++i) {
        entry = (xiaotai_sound_t)ENTRY(bad[i], XIAOTAI_CODEC_PCM16);
        if (register_one(&entry) != XIAOTAI_ERR_INVALID_ARG) {
            fprintf(stderr, "name \"%s\" accepted\n", bad[i]);
            return 1;
        }
    }
    CHECK(xiaotai_sounds_count() == 0);
    return 0;
}

/* A pack with one bad entry, or a name twice, is refused as a whole. */
static int test_atomic(void)
{
    xiaotai_sounds_reset();
    static const xiaotai_sound_t bad_second[] = {
        ENTRY("cue.tap", XIAOTAI_CODEC_PCM16),
        ENTRY("cue.success", XIAOTAI_CODEC_NONE),
    };
    static const xiaotai_sound_pack_t bad_pack = {.name = "bad", .sounds = bad_second, .count = 2};
    CHECK(xiaotai_sounds_register(&bad_pack) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_sounds_find("cue.tap") == NULL);
    CHECK(xiaotai_sounds_count() == 0);

    static const xiaotai_sound_t twice[] = {
        ENTRY("cue.tap", XIAOTAI_CODEC_PCM16),
        ENTRY("cue.success", XIAOTAI_CODEC_PCM16),
        ENTRY("cue.tap", XIAOTAI_CODEC_OPUS),
    };
    static const xiaotai_sound_pack_t twice_pack = {.name = "twice", .sounds = twice, .count = 3};
    CHECK(xiaotai_sounds_register(&twice_pack) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_sounds_find("cue.success") == NULL);
    CHECK(xiaotai_sounds_count() == 0);
    return 0;
}

static const xiaotai_sound_t k_first[] = {
    ENTRY("ring.incoming", XIAOTAI_CODEC_PCM16),
    ENTRY("digit.1", XIAOTAI_CODEC_PCM16),
    ENTRY("ack.male", XIAOTAI_CODEC_PCM16),
};
static const xiaotai_sound_t k_second[] = {
    ENTRY("digit.1", XIAOTAI_CODEC_G711A),
    ENTRY("cue.tap", XIAOTAI_CODEC_G711A),
};
static const xiaotai_sound_t k_third[] = {
    ENTRY("cue.tap", XIAOTAI_CODEC_OPUS),
    ENTRY("guide.binding", XIAOTAI_CODEC_OPUS),
    ENTRY("ring.incoming", XIAOTAI_CODEC_OPUS),
};
static const xiaotai_sound_pack_t k_pack_first = {.name = "first", .sounds = k_first, .count = 3};
static const xiaotai_sound_pack_t k_pack_second = {.name = "second", .sounds = k_second, .count = 2};
static const xiaotai_sound_pack_t k_pack_third = {.name = "third", .sounds = k_third, .count = 3};

static int test_override_and_listing(void)
{
    xiaotai_sounds_reset();
    CHECK(xiaotai_sounds_find("ring.incoming") == NULL);
    CHECK(xiaotai_sounds_count() == 0);
    CHECK(xiaotai_sounds_at(0) == NULL);

    CHECK(xiaotai_sounds_register(&k_pack_first) == XIAOTAI_OK);
    const xiaotai_sound_t *kept = xiaotai_sounds_find("ack.male");
    CHECK(kept == &k_first[2]);
    CHECK(xiaotai_sounds_find("digit.1") == &k_first[1]);
    CHECK(xiaotai_sounds_count() == 3);

    CHECK(xiaotai_sounds_register(&k_pack_second) == XIAOTAI_OK);
    CHECK(xiaotai_sounds_register(&k_pack_third) == XIAOTAI_OK);

    /* The last pack that has a name answers; names only one pack has come
     * from it; a stored result stays the same pointer. */
    CHECK(xiaotai_sounds_find("digit.1") == &k_second[0]);
    CHECK(xiaotai_sounds_find("cue.tap") == &k_third[0]);
    CHECK(xiaotai_sounds_find("ring.incoming") == &k_third[2]);
    CHECK(xiaotai_sounds_find("guide.binding") == &k_third[1]);
    CHECK(xiaotai_sounds_find("ack.male") == kept);
    CHECK(xiaotai_sounds_find("ack.female") == NULL);
    CHECK(xiaotai_sounds_find("ring") == NULL);
    CHECK(xiaotai_sounds_find("ring.incoming.x") == NULL);
    CHECK(xiaotai_sounds_find("") == NULL);
    CHECK(xiaotai_sounds_find(NULL) == NULL);
    CHECK(xiaotai_sounds_find(XIAOTAI_SOUND_RING_INCOMING) == &k_third[2]);
    CHECK(xiaotai_sounds_find(xiaotai_sounds_digit(1)) == &k_second[0]);

    /* Distinct names in first-appearance order, each answered by its winner. */
    const xiaotai_sound_t *expected[] = {&k_third[2], &k_second[0], &k_first[2], &k_third[0], &k_third[1]};
    CHECK(xiaotai_sounds_count() == COUNT_OF(expected));
    for (size_t i = 0; i < COUNT_OF(expected); ++i) {
        if (xiaotai_sounds_at(i) != expected[i]) {
            fprintf(stderr, "listing %zu: %s\n", i, xiaotai_sounds_at(i) ? xiaotai_sounds_at(i)->name : "(null)");
            return 1;
        }
    }
    CHECK(xiaotai_sounds_at(COUNT_OF(expected)) == NULL);
    CHECK(xiaotai_sounds_at(SIZE_MAX) == NULL);
    return 0;
}

static int test_pack_limit_and_reset(void)
{
    static xiaotai_sound_t entries[9];
    static xiaotai_sound_pack_t packs[9];
    static char names[9][16];
    xiaotai_sounds_reset();
    for (int i = 0; i < 9; ++i) {
        snprintf(names[i], sizeof names[i], "slot.n%d", i);
        entries[i] = (xiaotai_sound_t)ENTRY(names[i], XIAOTAI_CODEC_G711U);
        packs[i] = (xiaotai_sound_pack_t){.name = "slot", .sounds = &entries[i], .count = 1};
    }
    for (int i = 0; i < 8; ++i) CHECK(xiaotai_sounds_register(&packs[i]) == XIAOTAI_OK);
    CHECK(xiaotai_sounds_register(&packs[8]) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_sounds_find("slot.n8") == NULL);
    CHECK(xiaotai_sounds_find("slot.n7") == &entries[7]);
    CHECK(xiaotai_sounds_count() == 8);
    for (size_t i = 0; i < 8; ++i) CHECK(xiaotai_sounds_at(i) == &entries[i]);
    /* An invalid pack is refused for what it is, even when the registry is full. */
    CHECK(xiaotai_sounds_register(NULL) == XIAOTAI_ERR_INVALID_ARG);

    xiaotai_sounds_reset();
    CHECK(xiaotai_sounds_count() == 0);
    CHECK(xiaotai_sounds_find("slot.n0") == NULL);
    CHECK(xiaotai_sounds_register(&packs[8]) == XIAOTAI_OK);
    CHECK(xiaotai_sounds_find("slot.n8") == &entries[8]);
    xiaotai_sounds_reset();
    return 0;
}

static int test_engine_init(void)
{
    xiaotai_sounds_reset();
    CHECK(xiaotai_engine_sounds_init() == XIAOTAI_OK);
    const bool tones = XIAOTAI_SOUNDS_PACK_TONES_PCM8K;
    const bool guide = XIAOTAI_SOUNDS_PACK_GUIDE_ZH_ALAW8K;
    CHECK(xiaotai_sounds_count() == 4U * tones + 23U * guide);
    CHECK(!!xiaotai_sounds_find(XIAOTAI_SOUND_ACK_FEMALE) == tones);
    CHECK(!!xiaotai_sounds_find(XIAOTAI_SOUND_GUIDE_STARTUP) == guide);
    /* An application's pack registered after the engine init is found. */
    CHECK(xiaotai_sounds_register(&k_pack_second) == XIAOTAI_OK);
    CHECK(xiaotai_sounds_find("cue.tap") == &k_second[1]);
    xiaotai_sounds_reset();
    return 0;
}

int main(void)
{
    xiaotai_mem_host_clear();
    if (test_digits() || test_entry_checks() || test_atomic() || test_override_and_listing() ||
        test_pack_limit_and_reset() || test_engine_init())
        return 1;
    /* The index never allocates: registry, tables and names are static. */
    CHECK(xiaotai_mem_host_allocations() == 0);
    printf("sounds_index: ok\n");
    return 0;
}
