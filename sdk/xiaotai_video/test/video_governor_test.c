// SPDX-License-Identifier: MIT
#include <assert.h>
#include "xiaotai/video_governor.h"

static void expect_range(uint16_t w, uint16_t h, uint32_t bps, uint32_t min, uint32_t start)
{
    xiaotai_video_bitrate_range_t got;
    xiaotai_video_bitrate_range(w, h, bps, &got);
    assert(got.min_bps == min && got.start_bps == start && got.max_bps == bps);
}

static void ranges(void)
{
    expect_range(320, 240, 256000, 96000, 224000);
    expect_range(640, 480, 256000, 96000, 224000);
    expect_range(640, 481, 2000000, 750000, 1750000);
    expect_range(1280, 960, 4000000, 1000000, 3400000);
    expect_range(960, 1280, 2000000, 750000, 1750000);
    expect_range(UINT16_MAX, UINT16_MAX, UINT32_MAX, 1073741823, 3650722200U);
    expect_range(320, 240, 3, 1, 2);
    expect_range(320, 240, 96000, 95998, 95999);
}

static void convergence(void)
{
    xiaotai_video_governor_t g;
    xiaotai_video_governor_reset(&g, 640, 480, 256000);
    assert(g.bitrate_bps == 256000 && xiaotai_video_governor_level(&g) == 0);
    assert(!xiaotai_video_governor_target(&g, 0, 0) && !g.requested_bps);
    assert(!xiaotai_video_governor_target(&g, 230401, 0)); /* one below the 10% step */
    assert(xiaotai_video_governor_target(&g, 230400, 0));
    assert(g.bitrate_bps == 230400 && xiaotai_video_governor_level(&g) == 1);
    assert(!xiaotai_video_governor_target(&g, 180000, 999999));
    assert(!xiaotai_video_governor_step(&g, 999999));
    assert(xiaotai_video_governor_step(&g, 1000000));
    assert(g.bitrate_bps == 180000 && xiaotai_video_governor_level(&g) == 2);
    assert(!xiaotai_video_governor_target(&g, 135001, 1000001)); /* just under 25% */
    assert(xiaotai_video_governor_target(&g, 135000, 1000001)); /* exact emergency threshold */
    assert(g.bitrate_bps == 135000 && xiaotai_video_governor_level(&g) == 3);
    assert(!xiaotai_video_governor_target(&g, 119001, 2000001)); /* one below 16k floor */
    assert(xiaotai_video_governor_target(&g, 119000, 2000001));
    assert(g.bitrate_bps == 119000);
    assert(xiaotai_video_governor_target(&g, 1, 3000001)); /* clamp to range min */
    assert(g.requested_bps == 96000 && g.bitrate_bps == 96000);
    assert(!xiaotai_video_governor_target(&g, UINT32_MAX, 8000000));
    assert(g.requested_bps == g.range.max_bps);
    assert(xiaotai_video_governor_step(&g, 8000001) && g.bitrate_bps == 160000);
    assert(!xiaotai_video_governor_step(&g, 11000000));
    assert(xiaotai_video_governor_step(&g, 11000001) && g.bitrate_bps == 224000);
    assert(xiaotai_video_governor_step(&g, 14000001) && g.bitrate_bps == 256000);
    assert(!xiaotai_video_governor_step(&g, 20000000));

    xiaotai_video_governor_reset(&g, 1280, 960, 2000000);
    assert(xiaotai_video_governor_target(&g, g.range.start_bps, 0));
    assert(g.bitrate_bps == 1750000);
    assert(xiaotai_video_governor_target(&g, 750000, 1000));
    assert(!xiaotai_video_governor_target(&g, 760000, 6000000)); /* in deadband */
    assert(!xiaotai_video_governor_target(&g, 750001, 7000000));
    assert(xiaotai_video_governor_target(&g, 2000000, 8000000));
    assert(g.bitrate_bps == 862500); /* 15% step beats the absolute floor */
    assert(!xiaotai_video_governor_target(&g, 850000, 10000000)); /* lower target, but ignored by deadband */
    assert(!xiaotai_video_governor_target(&g, 2000000, 14999999));
    assert(xiaotai_video_governor_step(&g, 15000000));
}

static void backpressure(void)
{
    xiaotai_video_governor_t g;
    xiaotai_video_governor_reset(&g, 640, 480, 256000);
    assert(!xiaotai_video_governor_backpressured(&g, 0));
    xiaotai_video_governor_backpressure(&g, 0);
    assert(xiaotai_video_governor_backpressured(&g, 79999));
    assert(!xiaotai_video_governor_backpressured(&g, 80000));
    xiaotai_video_governor_backpressure(&g, 70000);
    assert(g.backpressure_events == 2 && g.bitrate_bps == 256000);
    assert(xiaotai_video_governor_backpressured(&g, 149999));
    assert(!xiaotai_video_governor_backpressured(&g, 150000));
    xiaotai_video_governor_backpressure(&g, INT64_MAX - 9);
    assert(xiaotai_video_governor_backpressured(&g, INT64_MAX - 1));
    assert(!xiaotai_video_governor_backpressured(&g, INT64_MAX));
    xiaotai_video_governor_reset(&g, 640, 480, 256000);
    assert(!g.backpressure_events && !g.backpressure_until_us && !g.requested_bps);
}

int main(void) { ranges(); convergence(); backpressure(); return 0; }
