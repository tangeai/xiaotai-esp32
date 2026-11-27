// SPDX-License-Identifier: MIT
/* Clock of the engine's timers on ESP-IDF: the port's monotonic clock. */
#include "internal.h"

int64_t xt_now_ms(void)
{
    return xiaotai_time_us() / 1000;
}

uint32_t xt_clock_wait_ms(int64_t delta_ms)
{
    if (delta_ms <= 0) return 0;
    return delta_ms >= (int64_t)XT_EVALUATE_PERIOD_MS ? XT_EVALUATE_PERIOD_MS : (uint32_t)delta_ms;
}
