#ifndef STARTER_SIGNAL_LEVEL_H
#define STARTER_SIGNAL_LEVEL_H

#include <stddef.h>
#include <stdint.h>

/* [DEBUG-wake49] Read-only integer PCM measurements. Full scale is 32768;
 * these are sample amplitudes, not calibrated acoustic dB SPL. */
typedef struct {
    uint32_t rms;
    uint32_t peak;
    int32_t dc;
} starter_signal_level_t;

static inline uint32_t starter_signal_isqrt(uint32_t value)
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

static inline starter_signal_level_t starter_signal_measure(const int16_t *pcm, size_t samples)
{
    starter_signal_level_t level = {0};
    if (pcm == NULL || samples == 0) return level;
    uint64_t squares = 0;
    int64_t sum = 0;
    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = pcm[i];
        uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        if (magnitude > level.peak) level.peak = magnitude;
        squares += (uint32_t)(sample * sample);
        sum += sample;
    }
    level.rms = starter_signal_isqrt((uint32_t)(squares / samples));
    level.dc = (int32_t)(sum / (int64_t)samples);
    return level;
}

#endif
