// SPDX-License-Identifier: MIT
#include "xiaotai/parts/display.h"

static uint16_t follow(uint16_t stable, uint16_t raw, const xiaotai_touch_filter_t *filter)
{
    const int32_t delta = (int32_t)raw - stable;
    const uint32_t distance = delta < 0 ? (uint32_t)-delta : (uint32_t)delta;
    if (distance <= filter->hold_px) return stable;
    if (distance >= filter->follow_px) return raw;
    return (uint16_t)((int32_t)stable + delta / 2);
}

void xiaotai_touch_filter_step(const xiaotai_touch_filter_t *filter, xiaotai_touch_filter_state_t *state,
                               int64_t now_ms, xiaotai_touch_point_t *point)
{
    if (!filter || !state) return;
    if (!point) {
        state->pressed = false;
        state->sampled_ms = now_ms;
        return;
    }
    if (state->pressed && now_ms >= state->sampled_ms &&
        (uint64_t)now_ms - (uint64_t)state->sampled_ms <= filter->press_gap_ms) {
        point->x = follow(state->x, point->x, filter);
        point->y = follow(state->y, point->y, filter);
    }
    *state = (xiaotai_touch_filter_state_t){.pressed = true, .x = point->x, .y = point->y,
                                           .sampled_ms = now_ms};
}
