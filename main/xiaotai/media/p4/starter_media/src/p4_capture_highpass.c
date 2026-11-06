#include "p4_capture_highpass.h"

#include <limits.h>

/* Same pole as the old P4 audio_capture_high_pass_filter: about 100 Hz at
 * 16 kHz. This removes DC/low-frequency rumble, not broadband hiss or echo.
 * Retain Q15 feedback precision instead of rounding each frame/sample to an
 * integer PCM state; signed rounding is symmetric to avoid a DC bias. */
#define HIGHPASS_ALPHA_Q15 31506
#define Q15_SCALE 32768

void p4_capture_highpass_reset(p4_capture_highpass_t *filter)
{
    if (filter) *filter = (p4_capture_highpass_t){0};
}

size_t p4_capture_highpass_process(p4_capture_highpass_t *filter, int16_t *pcm,
                                 size_t samples, bool enabled)
{
    if (!filter || !pcm || !samples) return 0;
    if (!enabled) {
        p4_capture_highpass_reset(filter);
        return 0;
    }
    size_t clipped = 0;
    for (size_t i = 0; i < samples; ++i) {
        int32_t input = pcm[i];
        if (!filter->initialized) {
            /* Seed the DC estimate once, never at each AEC block boundary. */
            filter->previous_input = input;
            filter->initialized = true;
        }
        int64_t delta_q15 = (int64_t)(input - filter->previous_input) * Q15_SCALE;
        int64_t product = (delta_q15 + filter->previous_output_q15) * HIGHPASS_ALPHA_Q15;
        int32_t output_q15 = (int32_t)((product + (product >= 0 ? Q15_SCALE / 2 : -Q15_SCALE / 2)) / Q15_SCALE);
        filter->previous_input = input;
        /* Keep unclipped filter history; clipping the speaker/upload PCM is
         * separate from the recurrence. Bounded int16 input fits int32 Q15. */
        filter->previous_output_q15 = output_q15;
        int32_t output = output_q15 / Q15_SCALE;
        if (output > INT16_MAX) {
            output = INT16_MAX;
            ++clipped;
        } else if (output < INT16_MIN) {
            output = INT16_MIN;
            ++clipped;
        }
        pcm[i] = (int16_t)output;
    }
    return clipped;
}
