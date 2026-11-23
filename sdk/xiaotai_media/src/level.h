// SPDX-License-Identifier: MIT
#pragma once
/* Integer PCM measurements for the diagnostics: the level of a block (RMS,
 * peak, DC mean) and a running playback meter (peak, samples near full scale,
 * largest step between adjacent samples). Full scale is 32768; these are
 * sample amplitudes, not calibrated acoustic levels. Header-only, no state
 * outside the caller's objects, real-time safe: the capture and playout tasks
 * measure, the diagnostics read what they published. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t rms;
    uint32_t peak;  /* largest magnitude; 32768 for a sample of -32768 */
    int32_t dc;     /* mean, truncated toward zero */
} xiaotai_media_level_t;

/* Integer square root, rounded down. */
static inline uint32_t xiaotai_media_isqrt(uint32_t value)
{
    uint32_t result = 0, bit = 1U << 30;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

/* Level of frames samples taken every stride samples from pcm (stride 1: a
 * mono block; stride n: one channel of an n-channel interleaved block, pcm
 * pointing at its first sample). All zero for no samples. */
static inline xiaotai_media_level_t xiaotai_media_level_measure(const int16_t *pcm, size_t frames, size_t stride)
{
    xiaotai_media_level_t level = {0, 0, 0};
    if (pcm == NULL || frames == 0 || stride == 0) return level;
    uint64_t squares = 0;
    int64_t sum = 0;
    for (size_t i = 0; i < frames; ++i) {
        const int32_t sample = pcm[i * stride];
        const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        if (magnitude > level.peak) level.peak = magnitude;
        squares += (uint32_t)(sample * sample);
        sum += sample;
    }
    level.rms = xiaotai_media_isqrt((uint32_t)(squares / frames));
    level.dc = (int32_t)(sum / (int64_t)frames);
    return level;
}

/* Magnitude from which a sample counts as near full scale. */
#define XIAOTAI_MEDIA_METER_NEAR_FULL 32000U

/* Amplitude evidence only: a large adjacent step can be valid high-frequency
 * content, and near-full-scale samples do not on their own prove clipping. */
typedef struct {
    uint32_t samples;
    uint32_t near_full;
    uint32_t peak;
    uint32_t step_max;
    int16_t last;
    bool have_last;
} xiaotai_media_meter_t;

/* Adds frames samples taken every stride samples. */
static inline void xiaotai_media_meter_add(xiaotai_media_meter_t *meter, const int16_t *pcm, size_t frames,
                                           size_t stride)
{
    for (size_t i = 0; i < frames; ++i) {
        const int32_t sample = pcm[i * stride];
        const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        if (magnitude > meter->peak) meter->peak = magnitude;
        if (magnitude >= XIAOTAI_MEDIA_METER_NEAR_FULL) ++meter->near_full;
        if (meter->have_last) {
            const int32_t delta = sample - meter->last;
            const uint32_t step = (uint32_t)(delta < 0 ? -delta : delta);
            if (step > meter->step_max) meter->step_max = step;
        }
        meter->last = (int16_t)sample;
        meter->have_last = true;
    }
    meter->samples += (uint32_t)frames;
}

/* Starts a new diagnostic window. Adjacency is kept across windows, so the
 * first step of the new window is measured against the last sample of the
 * old one; only a new stream forgets it (xiaotai_media_meter_reset). */
static inline void xiaotai_media_meter_clear_window(xiaotai_media_meter_t *meter)
{
    meter->samples = meter->near_full = meter->peak = meter->step_max = 0;
}

/* Forgets everything, adjacency included: a new session or stream. */
static inline void xiaotai_media_meter_reset(xiaotai_media_meter_t *meter)
{
    xiaotai_media_meter_clear_window(meter);
    meter->last = 0;
    meter->have_last = false;
}

#ifdef __cplusplus
}
#endif
