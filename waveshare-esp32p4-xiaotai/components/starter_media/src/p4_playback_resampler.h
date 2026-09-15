#ifndef P4_PLAYBACK_RESAMPLER_H
#define P4_PLAYBACK_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsps_fir.h"

#define P4_PLAYBACK_FIR_TAPS 64U
#define P4_PLAYBACK_FIR_BATCH 32U
#define P4_PLAYBACK_FIR_TAIL_INPUT 31U

/* One output owner, with caller-owned PSRAM storage. No per-block allocation. */
typedef struct {
    fir_f32_t fir;
    float delay[P4_PLAYBACK_FIR_TAPS / 2U];
    float input[P4_PLAYBACK_FIR_BATCH];
    float output[P4_PLAYBACK_FIR_BATCH * 2U];
    uint32_t clipped;
    bool initialized;
    bool pending_tail;
} p4_playback_resampler_t;

esp_err_t p4_playback_resampler_reset(p4_playback_resampler_t *s, int16_t first);
/* Returns interleaved stereo values, exactly 4 * count; zero on invalid input. */
size_t p4_playback_resampler_process(p4_playback_resampler_t *s,
    const int16_t *input, size_t count, int16_t *stereo, size_t capacity);
/* Natural gap/end only. Never drain a cancelled or superseded conversation. */
size_t p4_playback_resampler_finish(p4_playback_resampler_t *s,
    int16_t *stereo, size_t capacity);

#endif
