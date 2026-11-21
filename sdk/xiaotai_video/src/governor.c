// SPDX-License-Identifier: MIT
#include "xiaotai/video_governor.h"

static uint32_t larger(uint32_t a, uint32_t b) { return a > b ? a : b; }

static uint32_t percent(uint32_t value, unsigned ratio)
{
    return (uint32_t)((uint64_t)value * ratio / 100U);
}

static int64_t after_ms(int64_t now, uint32_t ms)
{
    const int64_t us = (int64_t)ms * 1000;
    return now > INT64_MAX - us ? INT64_MAX : now + us;
}

void xiaotai_video_bitrate_range(uint16_t width, uint16_t height, uint32_t bitrate_bps,
                                 xiaotai_video_bitrate_range_t *range)
{
    if (!range) return;
    if (bitrate_bps < 3U) {
        *range = (xiaotai_video_bitrate_range_t){0};
        return;
    }
    const uint32_t pixels = (uint32_t)width * height;
    const uint32_t floor = pixels <= XIAOTAI_VIDEO_BITRATE_COMPACT_PIXELS ?
        XIAOTAI_VIDEO_BITRATE_COMPACT_FLOOR_BPS : XIAOTAI_VIDEO_BITRATE_LARGE_FLOOR_BPS;
    uint32_t min = larger(bitrate_bps / XIAOTAI_VIDEO_BITRATE_FLOOR_DIVISOR, floor);
    if (min > bitrate_bps - 2U) min = bitrate_bps - 2U;
    const uint32_t start = min + larger(1U, percent(bitrate_bps - min, XIAOTAI_VIDEO_BITRATE_START_PERCENT));
    *range = (xiaotai_video_bitrate_range_t){min, start, bitrate_bps};
}

void xiaotai_video_governor_reset(xiaotai_video_governor_t *g, uint16_t width, uint16_t height,
                                  uint32_t bitrate_bps)
{
    if (!g) return;
    *g = (xiaotai_video_governor_t){0};
    xiaotai_video_bitrate_range(width, height, bitrate_bps, &g->range);
    g->bitrate_bps = g->range.max_bps;
}

bool xiaotai_video_governor_step(xiaotai_video_governor_t *g, int64_t now_us)
{
    if (!g || !g->requested_bps || g->requested_bps == g->bitrate_bps) return false;
    const uint32_t current = g->bitrate_bps;
    const uint32_t target = g->requested_bps;
    const bool down = target < current;
    const uint32_t delta = down ? current - target : target - current;
    const uint32_t threshold = larger(XIAOTAI_VIDEO_BITRATE_MIN_STEP_BPS,
                                      percent(current, XIAOTAI_VIDEO_BITRATE_MIN_STEP_PERCENT));
    if (delta < threshold && target != g->range.min_bps && target != g->range.max_bps) return false;
    if (down) {
        const bool emergency = (uint64_t)delta * 100U >= (uint64_t)current * XIAOTAI_VIDEO_BITRATE_EMERGENCY_PERCENT;
        if (!emergency && g->decrease_not_before_us && now_us < g->decrease_not_before_us) return false;
        g->bitrate_bps = target;
        g->decrease_not_before_us = after_ms(now_us, XIAOTAI_VIDEO_BITRATE_DECREASE_INTERVAL_MS);
        g->increase_not_before_us = after_ms(now_us, XIAOTAI_VIDEO_BITRATE_RECOVERY_HOLD_MS);
    } else {
        if (g->increase_not_before_us && now_us < g->increase_not_before_us) return false;
        const uint32_t step = larger(XIAOTAI_VIDEO_BITRATE_RECOVERY_MIN_STEP_BPS,
                                     percent(current, XIAOTAI_VIDEO_BITRATE_RECOVERY_STEP_PERCENT));
        g->bitrate_bps = delta > step ? current + step : target;
        g->increase_not_before_us = after_ms(now_us, XIAOTAI_VIDEO_BITRATE_RECOVERY_INTERVAL_MS);
    }
    return true;
}

bool xiaotai_video_governor_target(xiaotai_video_governor_t *g, uint32_t target_bps, int64_t now_us)
{
    if (!g || !target_bps || !g->range.max_bps) return false;
    if (target_bps < g->range.min_bps) target_bps = g->range.min_bps;
    if (target_bps > g->range.max_bps) target_bps = g->range.max_bps;
    g->requested_bps = target_bps;
    if (target_bps < g->bitrate_bps) {
        const int64_t hold = after_ms(now_us, XIAOTAI_VIDEO_BITRATE_RECOVERY_HOLD_MS);
        if (hold > g->increase_not_before_us) g->increase_not_before_us = hold;
    }
    return xiaotai_video_governor_step(g, now_us);
}

uint8_t xiaotai_video_governor_level(const xiaotai_video_governor_t *g)
{
    if (!g || g->bitrate_bps >= g->range.max_bps) return 0;
    const uint64_t rate = (uint64_t)g->bitrate_bps * 100U;
    if (rate >= (uint64_t)g->range.max_bps * 85U) return 1;
    return rate >= (uint64_t)g->range.max_bps * 70U ? 2 : 3;
}

void xiaotai_video_governor_backpressure(xiaotai_video_governor_t *g, int64_t now_us)
{
    if (!g) return;
    if (g->backpressure_events != UINT32_MAX) ++g->backpressure_events;
    const int64_t until = after_ms(now_us, XIAOTAI_VIDEO_BACKPRESSURE_HOLD_MS);
    if (until > g->backpressure_until_us) g->backpressure_until_us = until;
}

bool xiaotai_video_governor_backpressured(const xiaotai_video_governor_t *g, int64_t now_us)
{
    return g && g->backpressure_until_us && now_us < g->backpressure_until_us;
}
