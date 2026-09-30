#ifndef STARTER_AUDIO_RESAMPLER_H
#define STARTER_AUDIO_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Stateful 16 kHz mono -> 8 kHz mono decimator for the H5 G.711 boundary. */
#define STARTER_AUDIO_RESAMPLER_TAPS 31U

typedef struct {
    int16_t delay[STARTER_AUDIO_RESAMPLER_TAPS];
    size_t write_index;
    bool emit_phase;
    bool initialized;
} starter_audio_resampler_16k_to_8k_t;

void starter_audio_resampler_16k_to_8k_reset(
    starter_audio_resampler_16k_to_8k_t *resampler);

/* Returns input_count / 2 for an even input block and sufficient output space. */
size_t starter_audio_resampler_16k_to_8k_process(
    starter_audio_resampler_16k_to_8k_t *resampler,
    const int16_t *input,
    size_t input_count,
    int16_t *output,
    size_t output_capacity);

#endif
