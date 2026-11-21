// SPDX-License-Identifier: MIT
#pragma once
/* RIFF/WAVE parser of the sound index (internal).
 *
 * Validates a complete file in place: no allocation, no resampling, no
 * patching of truncated or streaming headers. Accepts PCM, 16 bits, 1 or 2
 * channels, 8000..48000 Hz; exactly one "fmt " chunk before exactly one
 * non-empty "data" chunk whose length is whole frames; the RIFF length equals
 * the file length; unknown chunks are skipped together with their padding
 * byte. The accept/reject decisions are those of the parser every product
 * used for downloaded and built-in WAV files, unchanged; only the result codes
 * follow xiaotai/sounds.h:
 *   XIAOTAI_ERR_INVALID_SIZE   the file is shorter or longer than its headers
 *                              say, or the data is not whole frames
 *   XIAOTAI_ERR_NOT_SUPPORTED  not a RIFF/WAVE file, a format other than the
 *                              one above, or a malformed chunk layout
 * Reads bytes one at a time, so the file may sit at any address. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "xiaotai/port/err.h"

typedef struct {
    const uint8_t *samples; /* little-endian PCM16 inside the file, any alignment */
    size_t frames;
    uint32_t sample_rate;
    unsigned channels;
} xiaotai_sounds_wav_t;

/* Smallest file: the RIFF header, a 16-byte "fmt " chunk and a "data" header. */
#define XIAOTAI_SOUNDS_WAV_HEADER_MIN 44U

static inline uint32_t xiaotai_sounds_wav_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline unsigned xiaotai_sounds_wav_u16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/* out is all zero unless the result is XIAOTAI_OK. */
static inline xiaotai_err_t xiaotai_sounds_wav_parse(const void *data, size_t bytes, xiaotai_sounds_wav_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = (xiaotai_sounds_wav_t){0};
    if (!data) return XIAOTAI_ERR_INVALID_ARG;
    const uint8_t *file = data;
    if (bytes < XIAOTAI_SOUNDS_WAV_HEADER_MIN) return XIAOTAI_ERR_INVALID_SIZE;
    if (memcmp(file, "RIFF", 4) || memcmp(file + 8, "WAVE", 4)) return XIAOTAI_ERR_NOT_SUPPORTED;
    if ((uint64_t)xiaotai_sounds_wav_u32(file + 4) + 8U != bytes) return XIAOTAI_ERR_INVALID_SIZE;
    xiaotai_sounds_wav_t value = {0};
    bool format = false, samples = false;
    size_t offset = 12;
    unsigned align = 0;
    while (offset < bytes) {
        if (bytes - offset < 8) return XIAOTAI_ERR_INVALID_SIZE;
        const uint8_t *chunk = file + offset;
        uint32_t length = xiaotai_sounds_wav_u32(chunk + 4);
        offset += 8;
        if (length > bytes - offset) return XIAOTAI_ERR_INVALID_SIZE;
        const uint8_t *body = file + offset;
        if (!memcmp(chunk, "fmt ", 4)) {
            if (format || samples || length < 16) return XIAOTAI_ERR_NOT_SUPPORTED;
            if (xiaotai_sounds_wav_u16(body) != 1 || xiaotai_sounds_wav_u16(body + 14) != 16)
                return XIAOTAI_ERR_NOT_SUPPORTED;
            value.channels = xiaotai_sounds_wav_u16(body + 2);
            value.sample_rate = xiaotai_sounds_wav_u32(body + 4);
            align = xiaotai_sounds_wav_u16(body + 12);
            if (value.channels < 1 || value.channels > 2 || value.sample_rate < 8000 ||
                value.sample_rate > 48000)
                return XIAOTAI_ERR_NOT_SUPPORTED;
            if (align != value.channels * 2 || xiaotai_sounds_wav_u32(body + 8) != value.sample_rate * align)
                return XIAOTAI_ERR_NOT_SUPPORTED;
            format = true;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!format || samples || !length) return XIAOTAI_ERR_NOT_SUPPORTED;
            if (length % align) return XIAOTAI_ERR_INVALID_SIZE;
            value.samples = body;
            value.frames = length / align;
            samples = true;
        }
        offset += length;
        if (length & 1U) {
            if (offset == bytes) return XIAOTAI_ERR_INVALID_SIZE;
            ++offset;
        }
    }
    if (!format || !samples) return XIAOTAI_ERR_NOT_SUPPORTED;
    *out = value;
    return XIAOTAI_OK;
}
