// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_MIXER_H
#define XIAOTAI_MEDIA_MIXER_H

#include <stddef.h>
#include <stdint.h>

static inline void media_mix_pcm16(int16_t *base, const int16_t *overlay, size_t frames)
{
    for (size_t i = 0; i < frames; ++i) {
        const int32_t sum = (int32_t)base[i] + overlay[i];
        base[i] = sum > INT16_MAX ? INT16_MAX : sum < INT16_MIN ? INT16_MIN : (int16_t)sum;
    }
}

#endif
