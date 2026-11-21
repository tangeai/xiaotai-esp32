// SPDX-License-Identifier: MIT
/* G.711 A-law by table look-up. The decode table holds the 256 interval
 * centres of the standard; the encoder finds the segment of the 11-bit
 * magnitude in a 128-entry table and takes the four mantissa bits below the
 * segment's leading one. Both reproduce the reference software exactly, value
 * by value (test media_g711 checks all 65536 inputs and all 256 codes). */
#include "g711.h"

#include <stdbool.h>

/* Interval centre of every code, already XOR-ed with the even-bit mask 0x55
 * of the line format. */
static const int16_t s_alaw_to_linear[256] = {
    -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
    -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
    -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
    -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
    -22016, -20992, -24064, -23040, -17920, -16896, -19968, -18944,
    -30208, -29184, -32256, -31232, -26112, -25088, -28160, -27136,
    -11008, -10496, -12032, -11520, -8960, -8448, -9984, -9472,
    -15104, -14592, -16128, -15616, -13056, -12544, -14080, -13568,
    -344, -328, -376, -360, -280, -264, -312, -296,
    -472, -456, -504, -488, -408, -392, -440, -424,
    -88, -72, -120, -104, -24, -8, -56, -40,
    -216, -200, -248, -232, -152, -136, -184, -168,
    -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
    -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
    -688, -656, -752, -720, -560, -528, -624, -592,
    -944, -912, -1008, -976, -816, -784, -880, -848,
    5504, 5248, 6016, 5760, 4480, 4224, 4992, 4736,
    7552, 7296, 8064, 7808, 6528, 6272, 7040, 6784,
    2752, 2624, 3008, 2880, 2240, 2112, 2496, 2368,
    3776, 3648, 4032, 3904, 3264, 3136, 3520, 3392,
    22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
    30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
    11008, 10496, 12032, 11520, 8960, 8448, 9984, 9472,
    15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
    344, 328, 376, 360, 280, 264, 312, 296,
    472, 456, 504, 488, 408, 392, 440, 424,
    88, 72, 120, 104, 24, 8, 56, 40,
    216, 200, 248, 232, 152, 136, 184, 168,
    1376, 1312, 1504, 1440, 1120, 1056, 1248, 1184,
    1888, 1824, 2016, 1952, 1632, 1568, 1760, 1696,
    688, 656, 752, 720, 560, 528, 624, 592,
    944, 912, 1008, 976, 816, 784, 880, 848,
};

/* Segment (exponent) of an 11-bit magnitude, indexed by magnitude >> 4:
 * segment 0 and 1 are 16 wide, segment n >= 2 starts at 16 << (n - 1). */
static const uint8_t s_segment[128] = {
    0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
};

uint8_t xiaotai_media_alaw_encode_sample(int16_t pcm)
{
    /* Negative values use the one's complement, so the code is symmetric
     * around -0.5 of the 12-bit scale, as the standard defines it. */
    const int32_t value = pcm;
    const uint32_t magnitude = (uint32_t)(value < 0 ? ~value : value) >> 4; /* 0..2047 */
    const uint32_t segment = s_segment[magnitude >> 4];
    const uint32_t mantissa = (segment ? magnitude >> (segment - 1U) : magnitude) & 0x0FU;
    uint32_t code = segment << 4 | mantissa;
    if (value >= 0) code |= 0x80U;
    return (uint8_t)(code ^ 0x55U);
}

int16_t xiaotai_media_alaw_decode_sample(uint8_t code) { return s_alaw_to_linear[code]; }

void xiaotai_media_alaw_encode(const int16_t *pcm, size_t samples, uint8_t *alaw)
{
    for (size_t i = 0; i < samples; ++i) alaw[i] = xiaotai_media_alaw_encode_sample(pcm[i]);
}

void xiaotai_media_alaw_decode(const uint8_t *alaw, size_t size, int16_t *pcm)
{
    for (size_t i = 0; i < size; ++i) pcm[i] = s_alaw_to_linear[alaw[i]];
}

/* ------------------------------------------------------- codec descriptors */

static bool alaw_format(const xiaotai_media_format_t *format)
{
    return format && format->encoding == XIAOTAI_CODEC_G711A && format->rate_hz != 0 && format->channels == 1;
}

/* Longest packet, in samples (bytes), at a rate. */
static uint32_t longest_packet(uint32_t rate_hz)
{
    return (uint32_t)((uint64_t)rate_hz * XIAOTAI_MEDIA_PACKET_MS_MAX / 1000U);
}

static int g711a_match(const xiaotai_media_format_t *format) { return alaw_format(format) ? 1 : 0; }

static uint32_t g711a_packet_samples(const uint8_t *data, size_t size, uint32_t rate_hz)
{
    if (!data || size == 0 || rate_hz == 0 || size > longest_packet(rate_hz)) return 0;
    return (uint32_t)size;
}

static xiaotai_err_t g711a_encoder_open(const xiaotai_media_format_t *format, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    return alaw_format(format) ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
}

static xiaotai_err_t g711a_encode(void *state, const int16_t *pcm, size_t samples, uint8_t *out, size_t capacity,
                                  size_t *size)
{
    (void)state;
    if (!pcm || !out || !size || samples == 0) return XIAOTAI_ERR_INVALID_ARG;
    *size = 0;
    if (samples > capacity) return XIAOTAI_ERR_INVALID_SIZE;
    xiaotai_media_alaw_encode(pcm, samples, out);
    *size = samples;
    return XIAOTAI_OK;
}

/* The decoder keeps no history; its state pointer carries the instance's
 * rate, so decode() applies the bound of packet_samples() without an
 * allocation. */
static xiaotai_err_t g711a_decoder_open(const xiaotai_media_format_t *format, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    if (!alaw_format(format)) return XIAOTAI_ERR_INVALID_ARG;
    *state = (void *)(uintptr_t)format->rate_hz;
    return XIAOTAI_OK;
}

static xiaotai_err_t g711a_decode(void *state, const uint8_t *data, size_t size, int16_t *pcm, size_t capacity,
                                  size_t *samples)
{
    if (!data || !pcm || !samples) return XIAOTAI_ERR_INVALID_ARG;
    *samples = 0;
    if (g711a_packet_samples(data, size, (uint32_t)(uintptr_t)state) == 0) return XIAOTAI_ERR_INVALID_ARG;
    if (size > capacity) return XIAOTAI_ERR_INVALID_SIZE;
    xiaotai_media_alaw_decode(data, size, pcm);
    *samples = size;
    return XIAOTAI_OK;
}

static void g711a_close(void *state) { (void)state; }

const xiaotai_audio_encoder_t xiaotai_media_g711a_encoder = {
    .name = "g711a",
    .encoding = XIAOTAI_CODEC_G711A,
    .stack_bytes = XIAOTAI_MEDIA_G711_STACK_BYTES,
    .match = g711a_match,
    .open = g711a_encoder_open,
    .encode = g711a_encode,
    .reset = NULL,
    .close = g711a_close,
    .capture_safe = true,
};

const xiaotai_audio_decoder_t xiaotai_media_g711a_decoder = {
    .name = "g711a",
    .encoding = XIAOTAI_CODEC_G711A,
    .stack_bytes = XIAOTAI_MEDIA_G711_STACK_BYTES,
    .match = g711a_match,
    .packet_samples = g711a_packet_samples,
    .open = g711a_decoder_open,
    .decode = g711a_decode,
    .reset = NULL,
    .close = g711a_close,
};
