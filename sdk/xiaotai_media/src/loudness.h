// SPDX-License-Identifier: MIT
#pragma once
/* What the audio tasks publish for xiaotai_media_loudness (xiaotai/media.h). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/media.h"

/* [rt, capture task] The level of the clean frame it has just measured. */
void media_loudness_capture(uint32_t rms, int64_t now_us);
/* The peak of a mono block (every fourth sample is looked at). */
uint32_t media_loudness_peak(const int16_t *pcm, size_t frames);
/* [rt, playout task] The block it has just written: how loud its voice is, when it will begin to be
 * heard (the driver plays what it holds first) and for how long. */
void media_loudness_playout(uint32_t peak, int64_t heard_us, int64_t duration_us, int64_t now_us);
/* xiaotai_media_loudness() at a given time. */
void media_loudness_read(xiaotai_media_loudness_t *out, int64_t now_us);
/* The speaker's level as heard at at_us; false when no block is filed for that moment (the level is 0 then, and says
 * nothing). */
bool media_loudness_playout_at(uint16_t *level, int64_t at_us);
