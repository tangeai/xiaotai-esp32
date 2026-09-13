#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Capture-owner only. AEC input/reference remain untouched. The independent
 * post-AEC output can serve both wake recognition and the uplink resampler. */
#define STARTER_AGC_DELAY_MS 10U
esp_err_t starter_agc_init(void);
void starter_agc_deinit(void);
/* Drop only buffered samples at privacy/session boundaries, not AEC state. */
void starter_agc_discard_pending(void);
esp_err_t starter_agc_process(const int16_t *clean, int16_t *uplink, size_t samples);

/* TX-owner only, after resampling and before G.711. In-place 8 kHz PCM,
 * multiples of 80 samples. No queue, added frame delay, or wake-path change. */
esp_err_t starter_agc_boost_uplink(int16_t *pcm, size_t samples);
