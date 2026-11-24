// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_camera_pacing.py
// Replaces legacy: tests/native/cores3/test_camera_pacing.py
// Absolute deadlines preserve cadence under jitter and skip catch-up bursts.
#include <assert.h>
#include "pacing.h"
#include "xiaotai/video_profiles.h"

static void cores3_legacy_cadence(void)
{
    const xiaotai_video_profile_t view = XIAOTAI_VIDEO_PROFILE_JPEG_STREAM;
    const uint32_t p = video_pacing_period_us(view.fps);
    assert(view.fps == 12 && p == 83333);
    const int64_t start = INT64_C(123456789012);
    int64_t deadline = start;
    for (int64_t i = 0; i < 100000; ++i) {
        deadline = video_pacing_next_us(deadline, deadline + i % 5000 + 50000, p);
        assert(deadline == start + (i + 1) * p);
    }
    for (int64_t cost = 0; cost < 10 * p; cost += 997) {
        const int64_t next = video_pacing_next_us(deadline, deadline + cost, p);
        assert(next >= deadline + cost && next > deadline && next >= deadline + p);
        if (cost > p) assert(next == deadline + cost);
        assert(next - (deadline + cost) <= p);
    }
}

static void deadlines(uint32_t fps)
{
    const uint32_t p = video_pacing_period_us(fps);
    assert(video_pacing_next_us(0, 0, p) == p);
    assert(video_pacing_next_us(0, p, p) == p);
    assert(video_pacing_next_us(0, p + 1, p) == p + 1);
    assert(video_pacing_next_us(0, 20 * p + 99, p) == 20 * p + 99);

    const int64_t origin = INT64_C(123456789012);
    int64_t scheduled = origin;
    for (int64_t i = 0; i < 100000; ++i) {
        const int64_t jitter = i % 5000;
        scheduled = video_pacing_next_us(scheduled, scheduled + p / 2 + jitter, p);
        assert(scheduled == origin + (i + 1) * p);
    }
    const int64_t cost[] = {25000, p, p + 1, 4 * p, 19 * p + 99};
    for (unsigned i = 0; i < sizeof(cost) / sizeof(cost[0]); ++i) {
        const int64_t done = scheduled + cost[i];
        const int64_t next = video_pacing_next_us(scheduled, done, p);
        assert(next >= done && next >= scheduled + p && next - done <= p);
        if (cost[i] > p) assert(next == done);
        scheduled = next;
    }
    /* A long overrun is sent just once and the next iteration resumes phase. */
    assert(video_pacing_next_us(0, 10 * p, p) == 10 * p);
    assert(video_pacing_next_us(10 * p, 10 * p + 10, p) == 11 * p);
}

int main(void)
{
    assert(video_pacing_period_us(0) == 0 && video_pacing_period_us(1) == 1000000);
    assert(video_pacing_period_us(12) == 83333 && video_pacing_period_us(15) == 66666 &&
           video_pacing_period_us(30) == 33333);
    deadlines(1);
    deadlines(12);
    deadlines(15);
    deadlines(30);
    cores3_legacy_cadence();
    assert(!video_pacing_wait_ms(20000, 20001) && !video_pacing_wait_ms(20000, 20000));
    assert(video_pacing_wait_ms(20000, 19999) == 1);
    assert(video_pacing_wait_ms(20000, 18999) == 2);
    assert(video_pacing_wait_ms(20000, 15001) == 5 && video_pacing_wait_ms(20000, 0) == 5);
    assert(video_pacing_wait_ms(INT64_MAX, INT64_MIN) == 5);
    assert(video_pacing_next_us(INT64_MAX - 5, INT64_MAX - 3, 12) == INT64_MAX);
    return 0;
}
