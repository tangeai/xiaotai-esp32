// SPDX-License-Identifier: MIT
#include "xiaotai/hal.h"

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    (void)index;
    return NULL;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t speaker = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 1}
    };
    return index ? NULL : &speaker;
}
