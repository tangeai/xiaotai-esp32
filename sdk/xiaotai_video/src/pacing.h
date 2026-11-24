// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

/* These helpers take monotonic microseconds from the caller. Nothing sleeps
 * here: the uplink rechecks admission between successive wait slices. */
uint32_t video_pacing_period_us(uint32_t fps);
int64_t video_pacing_next_us(int64_t previous_us, int64_t completed_us, uint32_t period_us);
uint32_t video_pacing_wait_ms(int64_t deadline_us, int64_t now_us);
