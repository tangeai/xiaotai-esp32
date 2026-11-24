// SPDX-License-Identifier: MIT
/* The media engine names encodings with the device's one codec vocabulary
 * (xiaotai/codec.h): format macros carry its values, a used direction must be
 * an audio codec, the codec registries refuse anything else, and an encoding
 * of one byte per sample other than the built-in A-law (mu-law) is sized as
 * nominal pieces once a decoder serves it. */
#include <stdio.h>
#include <string.h>
#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

static int ulaw_match(const xiaotai_media_format_t *format) { return format->encoding == XIAOTAI_CODEC_G711U; }
static uint32_t ulaw_samples(const uint8_t *data, size_t size, uint32_t rate_hz)
{
    (void)data;
    (void)rate_hz;
    return (uint32_t)size;
}
static xiaotai_err_t codec_open(const xiaotai_media_format_t *format, void **state)
{
    (void)format;
    *state = NULL;
    return XIAOTAI_OK;
}
static xiaotai_err_t ulaw_decode(void *state, const uint8_t *data, size_t size, int16_t *pcm, size_t capacity,
                                 size_t *samples)
{
    (void)state;
    (void)data;
    (void)pcm;
    (void)capacity;
    *samples = size;
    return XIAOTAI_OK;
}
static xiaotai_err_t pass_encode(void *state, const int16_t *pcm, size_t samples, uint8_t *out, size_t capacity,
                                 size_t *size)
{
    (void)state;
    (void)pcm;
    (void)out;
    (void)capacity;
    *size = samples;
    return XIAOTAI_OK;
}
static void codec_close(void *state) { (void)state; }

static size_t engine_bytes(void)
{
    if (xiaotai_engine_media_init() != XIAOTAI_OK) return SIZE_MAX;
    xiaotai_engine_media_start();
    return xiaotai_media_downlink() ? xiaotai_downlink_capacity(xiaotai_media_downlink()) : 0;
}

int main(void)
{
    /* The macros carry the vocabulary's values, so a format passes to a link
     * (xiaotai_link_codec_t) and from a part descriptor unconverted. */
    const xiaotai_media_format_t g711 = XIAOTAI_MEDIA_FORMAT_G711A_8K, opus = XIAOTAI_MEDIA_FORMAT_OPUS_16K;
    const xiaotai_media_format_t none = XIAOTAI_MEDIA_FORMAT_NONE;
    CHECK(g711.encoding == XIAOTAI_CODEC_G711A && (int)g711.encoding == 1 && g711.rate_hz == 8000U);
    CHECK(opus.encoding == XIAOTAI_CODEC_OPUS && (int)opus.encoding == 2 && opus.rate_hz == 16000U);
    /* A zero-filled direction is the unused one. */
    xiaotai_media_format_t zero;
    memset(&zero, 0, sizeof(zero));
    CHECK(zero.encoding == none.encoding && none.encoding == XIAOTAI_CODEC_NONE && none.rate_hz == 0U);

    /* A used direction must be an audio codec; an unused one is not looked at. */
    const xiaotai_media_requirement_t used_none = {.downlink = {XIAOTAI_CODEC_NONE, 8000U, 1U, 20U}};
    const xiaotai_media_requirement_t video = {.uplink = {XIAOTAI_CODEC_JPEG, 8000U, 1U, 20U}};
    const xiaotai_media_requirement_t unknown = {.uplink = {(xiaotai_codec_t)99, 8000U, 1U, 20U}};
    const xiaotai_media_requirement_t uplink_only = {.uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
                                                     .downlink = XIAOTAI_MEDIA_FORMAT_NONE};
    CHECK(xiaotai_media_require(&used_none) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_require(&video) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_require(&unknown) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_require(&uplink_only) == XIAOTAI_OK);
    CHECK(engine_bytes() == 0); /* nothing receives: no downlink */
    xiaotai_media_reset();

    /* The registries take audio codecs only. */
    xiaotai_audio_decoder_t decoder = {
        .name = "g711u", .encoding = XIAOTAI_CODEC_G711U, .match = ulaw_match, .packet_samples = ulaw_samples,
        .open = codec_open, .decode = ulaw_decode, .close = codec_close};
    xiaotai_audio_encoder_t encoder = {
        .name = "pcm16", .encoding = XIAOTAI_CODEC_PCM16, .match = ulaw_match, .open = codec_open,
        .encode = pass_encode, .close = codec_close};
    static const xiaotai_codec_t refused[] = {XIAOTAI_CODEC_NONE, XIAOTAI_CODEC_JPEG, XIAOTAI_CODEC_H264,
                                              (xiaotai_codec_t)99};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        xiaotai_audio_decoder_t bad_decoder = decoder;
        xiaotai_audio_encoder_t bad_encoder = encoder;
        bad_decoder.encoding = refused[i];
        bad_encoder.encoding = refused[i];
        CHECK(xiaotai_media_decoder_register(&bad_decoder) == XIAOTAI_ERR_INVALID_ARG);
        CHECK(xiaotai_media_encoder_register(&bad_encoder) == XIAOTAI_ERR_INVALID_ARG);
    }
    xiaotai_audio_decoder_t no_samples = decoder;
    no_samples.packet_samples = NULL;
    CHECK(xiaotai_media_decoder_register(&no_samples) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_decoder_register(NULL) == XIAOTAI_ERR_INVALID_ARG);

    /* Mu-law is one byte per sample like A-law: it needs no packet bound, is
     * not counted while no decoder serves it, and once one does it is stored as
     * nominal 160-byte pieces whatever max_packet_bytes says. */
    const xiaotai_media_requirement_t ulaw = {
        .downlink_burst_ms = 1000, .packet_ms = 20, .max_packet_bytes = 640,
        .downlink = {XIAOTAI_CODEC_G711U, 8000U, 1U, 20U}};
    const xiaotai_media_requirement_t ulaw_unbounded = {
        .packet_ms = 20, .downlink = {XIAOTAI_CODEC_G711U, 8000U, 1U, 20U}};
    CHECK(xiaotai_media_require(&ulaw) == XIAOTAI_OK && xiaotai_media_require(&ulaw_unbounded) == XIAOTAI_OK);
    CHECK(engine_bytes() == 0);
    xiaotai_media_reset();
    CHECK(xiaotai_media_decoder_register(&decoder) == XIAOTAI_OK);
    CHECK(xiaotai_media_encoder_register(&encoder) == XIAOTAI_OK);
    CHECK(xiaotai_media_require(&ulaw) == XIAOTAI_OK);
    CHECK(engine_bytes() == XIAOTAI_DOWNLINK_BYTES(60U, 160U));
    /* Registration closes with the start. */
    CHECK(xiaotai_media_decoder_register(&decoder) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_media_encoder_register(&encoder) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_media_reset();

    printf("PASS: codec vocabulary in formats, audio-only registries and requirements, mu-law sizing\n");
    return 0;
}
