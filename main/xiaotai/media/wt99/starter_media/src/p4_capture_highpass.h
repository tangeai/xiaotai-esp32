#ifndef P4_CAPTURE_HIGHPASS_H
#define P4_CAPTURE_HIGHPASS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* P4-only comparison switch; no shared S3 or sdkconfig changes are needed.
 * Override to 0 when building the unfiltered acoustic comparison. */
#ifndef P4_CAPTURE_HIGHPASS_ENABLED
#define P4_CAPTURE_HIGHPASS_ENABLED 1
#endif

typedef struct {
    int32_t previous_input;
    int32_t previous_output_q15;
    bool initialized;
} p4_capture_highpass_t;

void p4_capture_highpass_reset(p4_capture_highpass_t *filter);

/* In-place mono 16 kHz PCM, after AEC and before AGC. Retains history across
 * blocks; returns the number of saturated output samples, not dropped frames.
 * Bypass leaves every sample unchanged and resets only the filter history. */
size_t p4_capture_highpass_process(p4_capture_highpass_t *filter, int16_t *pcm,
                                 size_t samples, bool enabled);

#endif
