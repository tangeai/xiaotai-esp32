// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MEDIA_Q15_UNITY 32768

static inline uint32_t media_ac_power(int64_t sum, uint64_t squares, size_t count)
{
    if (count == 0) return 0;
    const int64_t dc = sum / (int64_t)count;
    return (uint32_t)(squares / count - (uint64_t)(dc * dc));
}

static inline uint32_t media_pcm_power(const int16_t *pcm, size_t count)
{
    int64_t sum = 0;
    uint64_t squares = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t sample = pcm[i];
        sum += sample;
        squares += (uint32_t)(sample * sample);
    }
    return media_ac_power(sum, squares, count);
}

static inline uint16_t media_ramp_q15(int16_t *pcm, size_t count, int32_t gain, int32_t target,
                                       int32_t up, int32_t down)
{
    for (size_t i = 0; i < count; ++i) {
        if (gain < target) gain += target - gain < up ? target - gain : up;
        else if (gain > target) gain -= gain - target < down ? gain - target : down;
        if (gain != MEDIA_Q15_UNITY) pcm[i] = (int16_t)(((int32_t)pcm[i] * gain + 16384) >> 15);
    }
    return (uint16_t)gain;
}

static inline uint32_t media_add_sat_u32(uint32_t a, uint32_t b)
{
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}
