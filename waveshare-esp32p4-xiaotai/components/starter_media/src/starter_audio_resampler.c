#include "starter_audio_resampler.h"

#include <limits.h>
#include <string.h>

/*
 * 31-tap Hamming low-pass, Q15.  This is deliberately stateful: resetting a
 * symmetric filter at every AEC block creates periodic edge artefacts after
 * A-law companding.  It also rejects the 4-8 kHz band before decimation, so
 * it cannot fold back as the "sharp" voice heard at the remote endpoint.
 */
static const int16_t s_decimator_q15[STARTER_AUDIO_RESAMPLER_TAPS] = {
    -9, 65, 33, -132, -112, 261, 305, -438,
    -691, 635, 1429, -813, -3041, 937, 10266, 15378,
    10266, 937, -3041, -813, 1429, 635, -691, -438,
    305, 261, -112, -132, 33, 65, -9,
};

void starter_audio_resampler_16k_to_8k_reset(
    starter_audio_resampler_16k_to_8k_t *resampler)
{
    if (resampler != NULL) {
        memset(resampler, 0, sizeof(*resampler));
    }
}

static int16_t filter_push(starter_audio_resampler_16k_to_8k_t *resampler,
                           int16_t sample, bool emit)
{
    if (!resampler->initialized) {
        for (size_t index = 0; index < STARTER_AUDIO_RESAMPLER_TAPS; ++index) {
            resampler->delay[index] = sample;
        }
        resampler->initialized = true;
    }
    resampler->delay[resampler->write_index] = sample;
    size_t delay_index = resampler->write_index;
    resampler->write_index = (resampler->write_index + 1U) % STARTER_AUDIO_RESAMPLER_TAPS;
    /* Every input updates history, but decimation consumes only alternate
     * outputs. Skip only the unused dot product, never an input sample. The
     * retained outputs, rounding, saturation and phase are bit-identical. */
    if (!emit) return 0;

    int64_t accumulator = 0;
    for (size_t tap = 0; tap < STARTER_AUDIO_RESAMPLER_TAPS; ++tap) {
        accumulator += (int64_t)resampler->delay[delay_index] * s_decimator_q15[tap];
        delay_index = delay_index == 0U ? STARTER_AUDIO_RESAMPLER_TAPS - 1U
                                        : delay_index - 1U;
    }
    accumulator = (accumulator + (1 << 14)) >> 15;
    if (accumulator > INT16_MAX) {
        return INT16_MAX;
    }
    if (accumulator < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)accumulator;
}

size_t starter_audio_resampler_16k_to_8k_process(
    starter_audio_resampler_16k_to_8k_t *resampler,
    const int16_t *input,
    size_t input_count,
    int16_t *output,
    size_t output_capacity)
{
    if (resampler == NULL || input == NULL || output == NULL ||
        (input_count & 1U) != 0U || output_capacity < input_count / 2U) {
        return 0U;
    }
    size_t output_count = 0U;
    for (size_t input_index = 0; input_index < input_count; ++input_index) {
        int16_t filtered = filter_push(resampler, input[input_index], !resampler->emit_phase);
        if (!resampler->emit_phase) {
            output[output_count++] = filtered;
        }
        resampler->emit_phase = !resampler->emit_phase;
    }
    return output_count;
}
