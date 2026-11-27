// SPDX-License-Identifier: MIT
/* Golden names and timings of the bundled PCM and A-law packs. */
#include "xiaotai/sounds.h"
#include <assert.h>
#include <string.h>

extern const xiaotai_sound_pack_t xiaotai_sound_pack_tones_pcm8k;
extern const xiaotai_sound_pack_t xiaotai_sound_pack_guide_zh_alaw8k;

typedef struct { const char *name; uint32_t ms; } expectation_t;
static const expectation_t tones[] = {
    {"ring.incoming", 1280}, {"ring.outgoing", 930}, {"ack.female", 1224}, {"ack.male", 1584},
};
static const expectation_t guide[] = {
    {"guide.startup", 1420}, {"guide.wifi_setup", 5100}, {"guide.wifi_connecting", 2220},
    {"guide.wifi_connected", 1620}, {"guide.wifi_failed", 4140}, {"guide.binding", 9220},
    {"guide.bound", 1640}, {"guide.offline", 2180}, {"guide.online", 1220}, {"guide.incoming", 1180},
    {"guide.dialing", 1260}, {"guide.ended", 1180}, {"guide.failed", 2820},
    {"digit.0", 640}, {"digit.1", 680}, {"digit.2", 560}, {"digit.3", 720}, {"digit.4", 700},
    {"digit.5", 700}, {"digit.6", 780}, {"digit.7", 620}, {"digit.8", 620}, {"digit.9", 620},
};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static void check_pack(const xiaotai_sound_pack_t *pack, const expectation_t *expected, size_t count,
                       xiaotai_codec_t codec, xiaotai_sound_container_t container, size_t total)
{
    xiaotai_sounds_reset();
    assert(pack->count == count && xiaotai_sounds_register(pack) == XIAOTAI_OK);
    assert(xiaotai_sounds_count() == count && xiaotai_sounds_at(count) == NULL);
    size_t size = 0;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_sound_t *sound = xiaotai_sounds_find(expected[i].name);
        assert(sound == &pack->sounds[i] && sound == xiaotai_sounds_at(i));
        assert(sound->encoding == codec && sound->container == container);
        xiaotai_sound_info_t info;
        assert(xiaotai_sounds_inspect(sound, &info) == XIAOTAI_OK);
        assert(info.channels == 1 && info.rate_hz == 8000);
        assert(info.duration_ms == expected[i].ms);
        if (container == XIAOTAI_SOUND_WAV) {
            assert(sound->rate_hz == 0 && sound->channels == 0 && sound->packet_ms == 0);
            assert(info.payload == sound->data + 44 && info.payload_size + 44 == sound->size);
        } else {
            assert(info.payload == sound->data && info.payload_size == sound->size);
            assert(container == XIAOTAI_SOUND_RAW);
            assert(sound->data == pack->sounds[0].data + size && sound->size % 160 == 0);
            assert(info.frames == sound->size && info.frames == expected[i].ms * 8U);
        }
        size += sound->size;
    }
    assert(size == total);
    if (codec != XIAOTAI_CODEC_PCM16) {
        assert(xiaotai_sounds_find(XIAOTAI_SOUND_GUIDE_BINDING));
        for (unsigned d = 0; d < 10; ++d) assert(xiaotai_sounds_find(xiaotai_sounds_digit(d)));
    }
}

int main(void)
{
    check_pack(&xiaotai_sound_pack_tones_pcm8k, tones, COUNT(tones), XIAOTAI_CODEC_PCM16, XIAOTAI_SOUND_WAV, 80464);
    check_pack(&xiaotai_sound_pack_guide_zh_alaw8k, guide, COUNT(guide), XIAOTAI_CODEC_G711A, XIAOTAI_SOUND_RAW, 334720);
    xiaotai_sounds_reset();
    assert(xiaotai_engine_sounds_init() == XIAOTAI_OK && xiaotai_sounds_count() == COUNT(tones) + COUNT(guide));
    assert(xiaotai_sounds_find(XIAOTAI_SOUND_RING_INCOMING)->encoding == XIAOTAI_CODEC_PCM16);
    assert(xiaotai_sounds_find(XIAOTAI_SOUND_GUIDE_BINDING)->encoding == XIAOTAI_CODEC_G711A);
    assert(xiaotai_sounds_find(XIAOTAI_SOUND_ACK_FEMALE)->encoding == XIAOTAI_CODEC_PCM16);
    assert(xiaotai_sounds_find(XIAOTAI_SOUND_GUIDE_STARTUP)->encoding == XIAOTAI_CODEC_G711A);
    return 0;
}
