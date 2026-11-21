// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_format.py
/* G.711 A-law: the table code against the ITU-T reference algorithm (the
 * alaw_compress / alaw_expand loops of the G.191 software tools), value by
 * value over every 16-bit input and every code; published table points; the
 * built-in codec descriptors (match, packet duration, encode and decode with
 * their error paths) and their acceptance by the codec registries; and the
 * platform voice format (8 kHz mono, 20 ms, one byte a sample: 160 bytes). */
#include <stdio.h>
#include <string.h>
#include "g711.h"
#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

/* ITU-T G.191 alaw_compress for one sample: the 12 most significant bits,
 * one's complement for negative input, segment by repeated shifting. */
static uint8_t reference_compress(int16_t linear)
{
    int ix = linear < 0 ? (~linear) >> 4 : linear >> 4;
    if (ix > 15) {
        int iexp = 1;
        while (ix > 16 + 15) {
            ix >>= 1;
            iexp++;
        }
        ix -= 16;
        ix += iexp << 4;
    }
    if (linear >= 0) ix |= 0x0080;
    return (uint8_t)(ix ^ 0x0055);
}

/* ITU-T G.191 alaw_expand for one code: interval centre, 16-bit scale. */
static int16_t reference_expand(uint8_t code)
{
    int ix = code ^ 0x0055;
    int iexp = (ix & 0x7F) >> 4;
    int mant = ix & 0x0F;
    if (iexp > 0) mant = mant + 16;
    mant = (mant << 4) + 0x0008;
    if (iexp > 1) mant = mant << (iexp - 1);
    return (int16_t)(ix > 127 ? mant : -mant);
}

static int check_tables(void)
{
    for (int32_t x = INT16_MIN; x <= INT16_MAX; ++x) {
        const uint8_t code = xiaotai_media_alaw_encode_sample((int16_t)x);
        if (code != reference_compress((int16_t)x)) {
            fprintf(stderr, "encode(%d) = 0x%02x, reference 0x%02x\n", (int)x, code,
                    reference_compress((int16_t)x));
            return 1;
        }
    }
    for (int c = 0; c < 256; ++c) {
        CHECK(xiaotai_media_alaw_decode_sample((uint8_t)c) == reference_expand((uint8_t)c));
        /* Every interval centre encodes back to its own code. */
        CHECK(xiaotai_media_alaw_encode_sample(xiaotai_media_alaw_decode_sample((uint8_t)c)) == c);
    }
    /* Published points: the smallest steps around zero and both extremes. */
    CHECK(xiaotai_media_alaw_encode_sample(0) == 0xD5 && xiaotai_media_alaw_decode_sample(0xD5) == 8);
    CHECK(xiaotai_media_alaw_encode_sample(-1) == 0x55 && xiaotai_media_alaw_decode_sample(0x55) == -8);
    CHECK(xiaotai_media_alaw_encode_sample(INT16_MAX) == 0xAA && xiaotai_media_alaw_decode_sample(0xAA) == 32256);
    CHECK(xiaotai_media_alaw_encode_sample(INT16_MIN) == 0x2A && xiaotai_media_alaw_decode_sample(0x2A) == -32256);
    /* Only the 12 most significant bits count: 0..15 share a code, 16 does not. */
    CHECK(xiaotai_media_alaw_encode_sample(15) == 0xD5 && xiaotai_media_alaw_encode_sample(16) == 0xD4);
    /* Quantization error is at most half a step: 512 in the top segment (step
     * 1024), 8 in the two lowest (step 16). */
    int32_t worst = 0;
    for (int32_t x = INT16_MIN; x <= INT16_MAX; ++x) {
        int32_t error = x - xiaotai_media_alaw_decode_sample(xiaotai_media_alaw_encode_sample((int16_t)x));
        if (error < 0) error = -error;
        if (x > -512 && x < 512) CHECK(error <= 8);
        if (error > worst) worst = error;
    }
    CHECK(worst == 512);
    return 0;
}

static int check_blocks(void)
{
    int16_t pcm[160], decoded[160];
    uint8_t alaw[160];
    for (int i = 0; i < 160; ++i) pcm[i] = (int16_t)((i * 7919) % 65536 - 32768);
    xiaotai_media_alaw_encode(pcm, 160, alaw);
    for (int i = 0; i < 160; ++i) CHECK(alaw[i] == reference_compress(pcm[i]));
    xiaotai_media_alaw_decode(alaw, 160, decoded);
    for (int i = 0; i < 160; ++i) CHECK(decoded[i] == reference_expand(alaw[i]));
    return 0;
}

static int check_descriptors(void)
{
    const xiaotai_audio_encoder_t *enc = &xiaotai_media_g711a_encoder;
    const xiaotai_audio_decoder_t *dec = &xiaotai_media_g711a_decoder;
    CHECK(strcmp(enc->name, "g711a") == 0 && strcmp(dec->name, "g711a") == 0);
    CHECK(enc->encoding == XIAOTAI_CODEC_G711A && dec->encoding == XIAOTAI_CODEC_G711A);
    CHECK(enc->stack_bytes == XIAOTAI_MEDIA_G711_STACK_BYTES && dec->stack_bytes == XIAOTAI_MEDIA_G711_STACK_BYTES);
    CHECK(enc->reset == NULL && dec->reset == NULL); /* stateless */

    /* The platform voice format: A-law, 8 kHz, mono, 20 ms. */
    const xiaotai_media_format_t voice = XIAOTAI_MEDIA_FORMAT_G711A_8K;
    CHECK(voice.encoding == XIAOTAI_CODEC_G711A && voice.rate_hz == 8000U && voice.channels == 1U &&
          voice.packet_ms == 20U);
    const xiaotai_media_format_t wide = {XIAOTAI_CODEC_G711A, 16000U, 1U, 20U};
    const xiaotai_media_format_t stereo = {XIAOTAI_CODEC_G711A, 8000U, 2U, 20U};
    const xiaotai_media_format_t opus = XIAOTAI_MEDIA_FORMAT_OPUS_16K;
    const xiaotai_media_format_t none = XIAOTAI_MEDIA_FORMAT_NONE;
    const xiaotai_media_format_t mulaw = {XIAOTAI_CODEC_G711U, 8000U, 1U, 20U};
    CHECK(enc->match(&voice) == 1 && dec->match(&voice) == 1);
    CHECK(enc->match(&wide) == 1 && dec->match(&wide) == 1);
    CHECK(enc->match(&stereo) == 0 && enc->match(&opus) == 0 && enc->match(&none) == 0 && enc->match(&mulaw) == 0);
    CHECK(dec->match(&stereo) == 0 && dec->match(&opus) == 0 && dec->match(&none) == 0 && dec->match(&mulaw) == 0);
    CHECK(enc->match(NULL) == 0 && dec->match(NULL) == 0);

    /* Duration from the size alone: one sample a byte, at most 120 ms. */
    uint8_t packet[1500];
    memset(packet, 0xD5, sizeof(packet));
    CHECK(dec->packet_samples(packet, 160, 8000) == 160);
    CHECK(dec->packet_samples(packet, 80, 8000) == 80);
    CHECK(dec->packet_samples(packet, 960, 8000) == 960); /* 120 ms */
    CHECK(dec->packet_samples(packet, 961, 8000) == 0);
    CHECK(dec->packet_samples(packet, 1500, 16000) == 1500); /* 93.75 ms */
    CHECK(dec->packet_samples(packet, 0, 8000) == 0);
    CHECK(dec->packet_samples(NULL, 160, 8000) == 0);
    CHECK(dec->packet_samples(packet, 160, 0) == 0);

    /* Encoder: one 20 ms packet is 160 bytes. */
    void *state = (void *)1;
    CHECK(enc->open(&opus, &state) == XIAOTAI_ERR_INVALID_ARG && state == NULL);
    CHECK(enc->open(&voice, NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(enc->open(&voice, &state) == XIAOTAI_OK);
    int16_t pcm[160];
    for (int i = 0; i < 160; ++i) pcm[i] = (int16_t)(i * 409 - 32768);
    uint8_t out[160];
    size_t size = 99;
    CHECK(enc->encode(state, pcm, 160, out, 159, &size) == XIAOTAI_ERR_INVALID_SIZE && size == 0);
    CHECK(enc->encode(state, pcm, 0, out, 160, &size) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(enc->encode(state, NULL, 160, out, 160, &size) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(enc->encode(state, pcm, 160, out, 160, &size) == XIAOTAI_OK && size == 160U);
    for (int i = 0; i < 160; ++i) CHECK(out[i] == reference_compress(pcm[i]));
    enc->close(state);

    /* Decoder: whole packet consumed, samples equal packet_samples(). */
    CHECK(dec->open(&mulaw, &state) == XIAOTAI_ERR_INVALID_ARG && state == NULL);
    CHECK(dec->open(&voice, &state) == XIAOTAI_OK);
    int16_t decoded[1500];
    size_t samples = 99;
    CHECK(dec->decode(state, out, 160, decoded, 159, &samples) == XIAOTAI_ERR_INVALID_SIZE && samples == 0);
    CHECK(dec->decode(state, out, 160, decoded, 160, &samples) == XIAOTAI_OK && samples == 160U);
    for (int i = 0; i < 160; ++i) CHECK(decoded[i] == reference_expand(out[i]));
    CHECK(dec->decode(state, packet, 960, decoded, 1500, &samples) == XIAOTAI_OK && samples == 960U);
    /* Longer than 120 ms at this instance's rate: refused, as packet_samples() refuses it. */
    CHECK(dec->decode(state, packet, 961, decoded, 1500, &samples) == XIAOTAI_ERR_INVALID_ARG && samples == 0);
    CHECK(dec->decode(state, packet, 0, decoded, 1500, &samples) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(dec->decode(state, NULL, 160, decoded, 1500, &samples) == XIAOTAI_ERR_INVALID_ARG);
    dec->close(state);
    void *wide_state = NULL;
    CHECK(dec->open(&wide, &wide_state) == XIAOTAI_OK);
    CHECK(dec->decode(wide_state, packet, 1500, decoded, 1500, &samples) == XIAOTAI_OK && samples == 1500U);
    dec->close(wide_state);

    /* Complete descriptors: the registries accept them. */
    CHECK(xiaotai_media_encoder_register(enc) == XIAOTAI_OK);
    CHECK(xiaotai_media_decoder_register(dec) == XIAOTAI_OK);
    xiaotai_media_reset();
    return 0;
}

int main(void)
{
    if (check_tables() || check_blocks() || check_descriptors()) return 1;
    printf("PASS: media_g711\n");
    return 0;
}
