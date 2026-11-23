// SPDX-License-Identifier: MIT
#include <assert.h>

#include "sound_decode.h"

extern const xiaotai_sound_pack_t xiaotai_sound_pack_guide_zh_alaw8k;
extern const xiaotai_sound_pack_t xiaotai_sound_pack_tones_pcm8k;

static int16_t reference_alaw(uint8_t code)
{
    code ^= 0x55;
    const unsigned segment = (code >> 4) & 7U;
    const int step = (code & 15) * 16;
    const int magnitude = segment ? (step + 264) * (1 << (segment - 1U)) : step + 8;
    return (int16_t)((code & 0x80) ? magnitude : -magnitude);
}

static int16_t reference_pcm(const uint8_t *bytes)
{
    const int high = (bytes[1] >= 128) ? (int)bytes[1] - 256 : bytes[1];
    return (int16_t)(high * 256 + bytes[0]);
}

static void real_assets(void)
{
    xiaotai_sounds_reset();
    assert(xiaotai_sounds_register(&xiaotai_sound_pack_guide_zh_alaw8k) == XIAOTAI_OK);
    assert(xiaotai_sounds_register(&xiaotai_sound_pack_tones_pcm8k) == XIAOTAI_OK);
    const xiaotai_sound_t *guide = xiaotai_sounds_find(XIAOTAI_SOUND_GUIDE_WIFI_SETUP);
    const xiaotai_sound_t *voice = xiaotai_sounds_find(XIAOTAI_SOUND_ACK_FEMALE);
    assert(guide && guide->size == 40800U && guide->encoding == XIAOTAI_CODEC_G711A);
    assert(voice && voice->encoding == XIAOTAI_CODEC_PCM16 && voice->container == XIAOTAI_SOUND_WAV);

    media_sound_cursor_t cursor;
    int16_t pcm[137];
    assert(media_sound_cursor_open(&cursor, guide) == XIAOTAI_OK);
    assert(cursor.format.rate_hz == 8000U && cursor.format.channels == 1U && !cursor.decoder);
    size_t samples = 91, processed = 0;
    assert(media_sound_cursor_read(&cursor, NULL, NULL, 0, &samples) == XIAOTAI_ERR_INVALID_SIZE);
    assert(!samples && !cursor.offset);
    unsigned non_silent = 0;
    do {
        assert(media_sound_cursor_read(&cursor, NULL, pcm, 137, &samples) == XIAOTAI_OK);
        for (size_t i = 0; i < samples; ++i) {
            assert(pcm[i] == reference_alaw(guide->data[processed + i]));
            if (pcm[i] > 512 || pcm[i] < -512) ++non_silent;
        }
        processed += samples;
    } while (samples);
    assert(processed == 40800U && non_silent > 1000U && cursor.offset == guide->size);
    assert(media_sound_cursor_read(&cursor, NULL, NULL, 0, &samples) == XIAOTAI_OK && !samples);

    assert(media_sound_cursor_open(&cursor, voice) == XIAOTAI_OK);
    assert(cursor.format.rate_hz == 8000U && cursor.channels == 1U && cursor.payload == voice->data + 44U);
    processed = 0;
    do {
        assert(media_sound_cursor_read(&cursor, NULL, pcm, 137, &samples) == XIAOTAI_OK);
        for (size_t i = 0; i < samples; ++i) assert(pcm[i] == reference_pcm(voice->data + 44U + 2U * (processed + i)));
        processed += samples;
    } while (samples);
    assert(processed == 9792U);
    xiaotai_sounds_reset();
}

static void stereo_wav(void)
{
    static const uint8_t wav[] = {
        'R', 'I', 'F', 'F', 52, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xbb, 0, 0, 0, 0xee, 2, 0, 4, 0, 16, 0,
        'd', 'a', 't', 'a', 16, 0, 0, 0,
        0, 0x80, 0xff, 0x7f, 1, 0, 0, 0, 0, 0x80, 0, 0x80, 0xff, 0x7f, 0xff, 0x7f,
    };
    const xiaotai_sound_t stereo = {.encoding = XIAOTAI_CODEC_PCM16, .container = XIAOTAI_SOUND_WAV,
                                    .data = wav, .size = sizeof(wav)};
    media_sound_cursor_t cursor;
    assert(media_sound_cursor_open(&cursor, &stereo) == XIAOTAI_OK);
    assert(cursor.format.rate_hz == 48000U && cursor.format.channels == 1 && cursor.channels == 2);
    size_t count;
    int16_t pcm[3];
    assert(media_sound_cursor_read(&cursor, NULL, pcm, 3, &count) == XIAOTAI_OK && count == 3);
    assert(pcm[0] == -1 && pcm[1] == 1 && pcm[2] == INT16_MIN && cursor.offset == 12U);
    assert(media_sound_cursor_read(&cursor, NULL, pcm, 3, &count) == XIAOTAI_OK && count == 1);
    assert(pcm[0] == INT16_MAX && cursor.offset == 16U);
}

typedef struct { unsigned calls; xiaotai_err_t error; bool wrong_count; } decoder_fixture_t;
static decoder_fixture_t fixture;

static int packet_match(const xiaotai_media_format_t *format)
{
    return format->encoding == XIAOTAI_CODEC_OPUS && format->rate_hz == 16000U &&
           format->channels == 1U && format->packet_ms == 20U;
}

static xiaotai_err_t packet_open(const xiaotai_media_format_t *format, void **state)
{
    assert(packet_match(format));
    *state = &fixture;
    return XIAOTAI_OK;
}

static uint32_t packet_samples(const uint8_t *data, size_t size, uint32_t rate_hz)
{
    assert(data && rate_hz == 16000U);
    return size != 2 ? 0 : data[0] == 255 ? 1921U : data[0];
}

static xiaotai_err_t packet_decode(void *state, const uint8_t *data, size_t size, int16_t *pcm,
                                    size_t capacity, size_t *samples)
{
    assert(state == &fixture && data && size == 2 && capacity >= data[0]);
    ++fixture.calls;
    if (fixture.error != XIAOTAI_OK) return fixture.error;
    *samples = fixture.wrong_count ? 0 : data[0];
    for (size_t i = 0; i < *samples; ++i) pcm[i] = (int16_t)(data[1] * 8 + (int)i);
    return XIAOTAI_OK;
}

static void packet_close(void *state) { assert(state == &fixture); }

static const xiaotai_audio_decoder_t codec = {
    .name = "framed.fixture", .encoding = XIAOTAI_CODEC_OPUS, .match = packet_match,
    .open = packet_open, .packet_samples = packet_samples, .decode = packet_decode, .close = packet_close,
};

static void packets(void)
{
    static const uint8_t chunks[] = {2, 0, 4, 11, 2, 0, 2, 22};
    xiaotai_sound_t sound = {.encoding = XIAOTAI_CODEC_OPUS, .container = XIAOTAI_SOUND_PACKETS,
                             .rate_hz = 16000U, .channels = 1, .packet_ms = 20, .data = chunks, .size = sizeof(chunks)};
    media_sound_cursor_t cursor;
    assert(media_sound_cursor_open(&cursor, &sound) == XIAOTAI_ERR_NOT_SUPPORTED && !cursor.payload);
    assert(xiaotai_media_decoder_register(&codec) == XIAOTAI_OK);
    assert(media_sound_cursor_open(&cursor, &sound) == XIAOTAI_OK);
    assert(cursor.decoder == &codec && cursor.format.packet_ms == 20 && cursor.format.rate_hz == 16000U);
    void *state;
    assert(cursor.decoder->open(&cursor.format, &state) == XIAOTAI_OK);
    int16_t pcm[8];
    size_t count = 17;
    assert(media_sound_cursor_read(&cursor, state, pcm, 3U, &count) == XIAOTAI_ERR_INVALID_SIZE);
    assert(!count && !cursor.offset && !fixture.calls);
    assert(media_sound_cursor_read(&cursor, state, pcm, 8U, &count) == XIAOTAI_OK);
    assert(count == 4 && cursor.offset == 4 && fixture.calls == 1 && pcm[0] == 88 && pcm[3] == 91);
    fixture.error = XIAOTAI_ERR_TIMEOUT;
    assert(media_sound_cursor_read(&cursor, state, pcm, 8U, &count) == XIAOTAI_ERR_TIMEOUT);
    assert(!count && cursor.offset == 4 && fixture.calls == 2);
    fixture.error = XIAOTAI_OK;
    fixture.wrong_count = true;
    assert(media_sound_cursor_read(&cursor, state, pcm, 8U, &count) == XIAOTAI_FAIL);
    assert(!count && cursor.offset == 4 && fixture.calls == 3);
    cursor.decoder->close(state);
    fixture.wrong_count = false;
    assert(cursor.decoder->open(&cursor.format, &state) == XIAOTAI_OK);
    assert(media_sound_cursor_read(&cursor, state, pcm, 8U, &count) == XIAOTAI_OK);
    assert(count == 2 && cursor.offset == sizeof(chunks) && fixture.calls == 4 && pcm[0] == 176 && pcm[1] == 177);
    assert(media_sound_cursor_read(&cursor, state, pcm, 8U, &count) == XIAOTAI_OK && !count && fixture.calls == 4);
    cursor.decoder->close(state);

    static const uint8_t invalid[][4] = {{0, 0, 0, 0}, {1, 2, 0, 0}, {3, 0, 0, 0}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        sound.data = invalid[i];
        sound.size = sizeof(invalid[i]);
        assert(media_sound_cursor_open(&cursor, &sound) == XIAOTAI_ERR_INVALID_SIZE && !cursor.payload);
        assert(media_sound_cursor_read(&cursor, NULL, pcm, 8U, &count) == XIAOTAI_ERR_INVALID_STATE && !count);
    }
    static const uint8_t overlong_pcm[] = {2, 0, 255, 11};
    sound.data = overlong_pcm;
    assert(media_sound_cursor_open(&cursor, &sound) == XIAOTAI_OK);
    int16_t long_pcm[1921];
    assert(media_sound_cursor_read(&cursor, NULL, long_pcm, 1921U, &count) == XIAOTAI_ERR_INVALID_SIZE);
    assert(!count && !cursor.offset && fixture.calls == 4);
    sound.packet_ms = XIAOTAI_MEDIA_PACKET_MS_MAX + 1U;
    assert(media_sound_cursor_open(&cursor, &sound) == XIAOTAI_ERR_NOT_SUPPORTED && !cursor.payload);
    xiaotai_sounds_reset();
}

static int ulaw_match(const xiaotai_media_format_t *format)
{
    return format->encoding == XIAOTAI_CODEC_G711U && format->rate_hz == 8000U &&
           format->channels == 1U && format->packet_ms == 20U;
}

static xiaotai_err_t ulaw_open(const xiaotai_media_format_t *format, void **state)
{
    assert(ulaw_match(format));
    *state = &fixture;
    return XIAOTAI_OK;
}

static uint32_t ulaw_samples(const uint8_t *data, size_t size, uint32_t rate_hz)
{
    assert(data && rate_hz == 8000U && size <= 160U);
    return (uint32_t)size;
}

static xiaotai_err_t ulaw_decode(void *state, const uint8_t *data, size_t size, int16_t *pcm,
                                  size_t capacity, size_t *samples)
{
    assert(state == &fixture && size <= 160U && capacity >= size);
    for (size_t i = 0; i < size; ++i) pcm[i] = (int16_t)((int)data[i] - 128);
    *samples = size;
    return XIAOTAI_OK;
}

static void registered_raw_codec(void)
{
    static const xiaotai_audio_decoder_t ulaw = {
        .name = "ulaw.fixture", .encoding = XIAOTAI_CODEC_G711U, .match = ulaw_match,
        .open = ulaw_open, .packet_samples = ulaw_samples, .decode = ulaw_decode, .close = packet_close,
    };
    static const uint8_t raw[161] = {[159] = 250, [160] = 10};
    const xiaotai_sound_t sound = {
        .container = XIAOTAI_SOUND_RAW, .encoding = XIAOTAI_CODEC_G711U,
        .rate_hz = 8000U, .channels = 1U, .data = raw, .size = sizeof(raw),
    };
    media_sound_cursor_t cursor;
    assert(media_sound_cursor_open(&cursor, &sound) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(xiaotai_media_decoder_register(&ulaw) == XIAOTAI_OK);
    assert(media_sound_cursor_open(&cursor, &sound) == XIAOTAI_OK && cursor.decoder == &ulaw);
    void *state;
    assert(cursor.decoder->open(&cursor.format, &state) == XIAOTAI_OK);
    int16_t pcm[161];
    size_t count;
    assert(media_sound_cursor_read(&cursor, state, pcm, 161U, &count) == XIAOTAI_OK && count == 160U);
    assert(pcm[0] == -128 && pcm[159] == 122);
    assert(media_sound_cursor_read(&cursor, state, pcm, 161U, &count) == XIAOTAI_OK && count == 1U);
    assert(pcm[0] == -118 && cursor.offset == sizeof(raw));
    cursor.decoder->close(state);
}

int main(void)
{
    real_assets();
    stereo_wav();
    packets();
    registered_raw_codec();
    return 0;
}
