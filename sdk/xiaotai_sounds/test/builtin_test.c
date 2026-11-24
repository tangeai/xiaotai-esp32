// SPDX-License-Identifier: MIT
/* xiaotai_engine_sounds_init registers the packs whose option is on, in the
 * order of xiaotai/sounds_config.h, so the later pack answers a shared name.
 * Built three times against fake packs with the generated names (see
 * test/CMakeLists.txt): every option on, one option on, and every option on
 * with a defective final pack, which stops the init with the refusal and
 * leaves only the packs before it. */
#include <stdio.h>
#include "xiaotai/sounds.h"
#include "xiaotai/sounds_config.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#ifndef SOUNDS_TEST_BAD_GUIDE
#define SOUNDS_TEST_BAD_GUIDE 0
#endif

static const uint8_t k_bytes[4] = {1, 2, 3, 4};

#define SOUND(entry_name, codec, kind)                                                         \
    {.name = (entry_name), .encoding = (codec), .container = (kind), .rate_hz = 8000, .channels = 1, \
     .data = k_bytes, .size = sizeof k_bytes}

/* Two fake packs share a name so the override stays covered. */
static const xiaotai_sound_t k_tones[] = {
    SOUND("ring.incoming", XIAOTAI_CODEC_PCM16, XIAOTAI_SOUND_WAV),
    SOUND("ring.outgoing", XIAOTAI_CODEC_PCM16, XIAOTAI_SOUND_WAV),
    SOUND("ack.female", XIAOTAI_CODEC_PCM16, XIAOTAI_SOUND_WAV),
    SOUND("ack.male", XIAOTAI_CODEC_PCM16, XIAOTAI_SOUND_WAV),
};
static const xiaotai_sound_t k_guide[] = {
    SOUND("guide.binding", XIAOTAI_CODEC_G711A, XIAOTAI_SOUND_RAW),
    SOUND("digit.0", XIAOTAI_CODEC_G711A, XIAOTAI_SOUND_RAW),
    SOUND("digit.3", XIAOTAI_CODEC_G711A, XIAOTAI_SOUND_RAW),
    SOUND("ring.incoming", SOUNDS_TEST_BAD_GUIDE ? XIAOTAI_CODEC_NONE : XIAOTAI_CODEC_G711A, XIAOTAI_SOUND_RAW),
};

/* The symbols the pack generator defines; unused when the option is off. */
const xiaotai_sound_pack_t xiaotai_sound_pack_tones_pcm8k = {"tones_pcm8k", k_tones, 4};
const xiaotai_sound_pack_t xiaotai_sound_pack_guide_zh_alaw8k = {"guide_zh_alaw8k", k_guide, 4};

enum {
    TONES = XIAOTAI_SOUNDS_PACK_TONES_PCM8K,
    GUIDE = XIAOTAI_SOUNDS_PACK_GUIDE_ZH_ALAW8K,
    /* What ends up registered: the init stops at the refused guide pack. */
    REFUSED = GUIDE && SOUNDS_TEST_BAD_GUIDE,
    HAS_TONES = TONES,
    HAS_GUIDE = GUIDE && !REFUSED,
};

int main(void)
{
    xiaotai_sounds_reset();
    CHECK(xiaotai_engine_sounds_init() == (REFUSED ? XIAOTAI_ERR_INVALID_ARG : XIAOTAI_OK));

    const xiaotai_sound_t *ring = HAS_GUIDE ? &k_guide[3] : HAS_TONES ? &k_tones[0] : NULL;
    const xiaotai_sound_t *binding = HAS_GUIDE ? &k_guide[0] : NULL;
    const xiaotai_sound_t *three = HAS_GUIDE ? &k_guide[2] : NULL;
    CHECK(xiaotai_sounds_find(XIAOTAI_SOUND_RING_INCOMING) == ring);
    CHECK(xiaotai_sounds_find(XIAOTAI_SOUND_GUIDE_BINDING) == binding);
    CHECK(xiaotai_sounds_find(xiaotai_sounds_digit(3)) == three);
    CHECK(xiaotai_sounds_find(XIAOTAI_SOUND_ACK_FEMALE) == (HAS_TONES ? &k_tones[2] : NULL));
    CHECK(xiaotai_sounds_find(xiaotai_sounds_digit(0)) == (HAS_GUIDE ? &k_guide[1] : NULL));

    size_t expected = 4U * HAS_TONES + 4U * HAS_GUIDE - (HAS_TONES && HAS_GUIDE);
    CHECK(xiaotai_sounds_count() == expected);
    /* The first listed name is the first pack's first entry, answered by the
     * pack that wins it. */
    if (expected) CHECK(xiaotai_sounds_at(0) == (HAS_TONES ? ring : binding));
    printf("sounds_builtin: ok (packs %d%d)\n", HAS_TONES, HAS_GUIDE);
    return 0;
}
