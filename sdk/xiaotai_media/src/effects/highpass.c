// SPDX-License-Identifier: MIT
#include "portable_audio.h"

#include <math.h>
#include "power.h"
#include "xiaotai/media.h"
#include "xiaotai/port/mem.h"

typedef struct {
    int64_t previous_output_q15;
    int32_t previous_input, alpha_q15;
    bool initialized;
} highpass_t;

static xiaotai_err_t open_filter(const xiaotai_processor_info_t *processor, xiaotai_effect_info_t *info, void **state)
{
    if (!processor || !info || !state) return XIAOTAI_ERR_INVALID_ARG;
    const int32_t hz = xiaotai_media_param("capture.highpass_hz", 0);
    if (hz == 0 || (processor->flags & XIAOTAI_PROCESSOR_ENCODED)) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (hz < 0 || (uint32_t)hz >= processor->rate_hz / 2U) return XIAOTAI_ERR_INVALID_ARG;
    highpass_t *filter = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*filter));
    if (!filter) return XIAOTAI_ERR_NO_MEM;
    /* Preserve the measured legacy 100 Hz coefficient bit-for-bit. */
    filter->alpha_q15 = hz == 100 && processor->rate_hz == 16000U ? 31506
        : (int32_t)(MEDIA_Q15_UNITY * exp(-6.283185307179586 * hz / processor->rate_hz) + 0.5);
    if (filter->alpha_q15 >= MEDIA_Q15_UNITY) filter->alpha_q15 = MEDIA_Q15_UNITY - 1;
    *info = (xiaotai_effect_info_t){.stack_bytes = 512};
    *state = filter;
    return XIAOTAI_OK;
}

static void reset(void *opaque)
{
    highpass_t *filter = opaque;
    filter->previous_output_q15 = 0;
    filter->previous_input = 0;
    filter->initialized = false;
}

static xiaotai_err_t process(void *opaque, int16_t *pcm, size_t samples)
{
    if (!opaque || (!pcm && samples)) return XIAOTAI_ERR_INVALID_ARG;
    highpass_t *filter = opaque;
    for (size_t i = 0; i < samples; ++i) {
        const int32_t input = pcm[i];
        if (!filter->initialized) {
            filter->previous_input = input;
            filter->initialized = true;
        }
        const int64_t delta_q15 = (int64_t)(input - filter->previous_input) * MEDIA_Q15_UNITY;
        const int64_t product = (delta_q15 + filter->previous_output_q15) * filter->alpha_q15;
        filter->previous_output_q15 = (product + (product >= 0 ? MEDIA_Q15_UNITY / 2 : -MEDIA_Q15_UNITY / 2)) /
                                      MEDIA_Q15_UNITY;
        filter->previous_input = input;
        const int64_t output = filter->previous_output_q15 / MEDIA_Q15_UNITY;
        pcm[i] = (int16_t)(output > INT16_MAX ? INT16_MAX : output < INT16_MIN ? INT16_MIN : output);
    }
    return XIAOTAI_OK;
}

const xiaotai_audio_effect_t media_highpass = {.name = "highpass", .stage = XIAOTAI_EFFECT_CAPTURE, .order = 10,
    .open = open_filter, .process = process, .reset = reset, .close = xiaotai_mem_free};
