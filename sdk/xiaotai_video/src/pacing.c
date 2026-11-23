// SPDX-License-Identifier: MIT
#include "pacing.h"

enum { US_PER_MS = 1000, US_PER_SECOND = 1000000, WAIT_SLICE_MS = 5 };

uint32_t video_pacing_period_us(uint32_t fps)
{
    return fps ? US_PER_SECOND / fps : 0;
}

int64_t video_pacing_next_us(int64_t previous_us, int64_t completed_us, uint32_t period_us)
{
    const int64_t next = previous_us > INT64_MAX - period_us ? INT64_MAX : previous_us + period_us;
    /* Keep the phase when on time. An overrun starts a new phase at this
     * completion, instead of sleeping a whole period or replaying frames. */
    return completed_us > next ? completed_us : next;
}

uint32_t video_pacing_wait_ms(int64_t deadline_us, int64_t now_us)
{
    if (now_us >= deadline_us) return 0;
    const uint64_t remaining = (uint64_t)deadline_us - (uint64_t)now_us;
    if (remaining >= WAIT_SLICE_MS * US_PER_MS) return WAIT_SLICE_MS;
    return (uint32_t)((remaining + US_PER_MS - 1U) / US_PER_MS);
}
