// SPDX-License-Identifier: MIT
#include <assert.h>
#include "playout.h"

static void latest_only(void)
{
    video_playout_t state;
    video_playout_reset(&state, 12, false, 19);
    assert(!video_playout_arrival(&state, 0));
    assert(!video_playout_arrival(&state, 600000));
    assert(video_playout_select(&state, 600000, 0, true) == VIDEO_PLAYOUT_HOLD);
    assert(video_playout_select(&state, 600000, 3, true) == VIDEO_PLAYOUT_LATEST);

    video_playout_reset(&state, 0, true, 19);
    assert(!video_playout_arrival(&state, 800000));
    assert(video_playout_select(&state, 800000, 1, false) == VIDEO_PLAYOUT_LATEST);
    video_playout_reset(&state, 12, true, 1);
    assert(!video_playout_arrival(&state, 800000));
    assert(video_playout_select(&state, 800000, 1, false) == VIDEO_PLAYOUT_LATEST);
}

static void moderate_gaps(void)
{
    video_playout_t state;
    video_playout_reset(&state, 10, true, 19);
    assert(!video_playout_arrival(&state, 0));
    assert(!video_playout_arrival(&state, 399999));
    assert(!state.has_suspect);
    assert(!video_playout_arrival(&state, 799999));
    assert(state.has_suspect && state.suspect_until_us == 2799999);
    assert(!video_playout_arrival(&state, 799999)); /* Same-tick burst. */
    assert(!video_playout_arrival(&state, 100000)); /* Stale receipt. */
    assert(state.last_arrival_us == 799999);
    for (int64_t stamp = 899999; stamp <= 2399999; stamp += 100000)
        assert(!video_playout_arrival(&state, stamp));
    assert(video_playout_arrival(&state, 2799999)); /* Exactly on the 2 s limit. */
    assert(state.target_depth == 5 && !state.has_suspect);

    video_playout_reset(&state, 10, true, 19);
    assert(!video_playout_arrival(&state, 0));
    assert(!video_playout_arrival(&state, 400000));
    for (int64_t stamp = 500000; stamp <= 2000000; stamp += 100000)
        assert(!video_playout_arrival(&state, stamp));
    assert(!video_playout_arrival(&state, 2400001)); /* Too late, starts new window. */
    assert(state.suspect_until_us == 4400001);
    assert(video_playout_arrival(&state, 2800002));
    assert(state.target_depth == 6); /* Non-integer gap rounds upward. */
}

static void prime_and_pace(void)
{
    video_playout_t state;
    video_playout_reset(&state, 12, true, 19);
    const int64_t first = 6 * 83334;
    assert(!video_playout_arrival(&state, 0));
    assert(!video_playout_arrival(&state, first - 1));
    assert(!video_playout_buffering(&state, first - 1));

    video_playout_reset(&state, 12, true, 19);
    assert(!video_playout_arrival(&state, 0));
    assert(video_playout_arrival(&state, first));
    assert(state.target_depth == 7 && state.buffered_until_us == first + 8000000);
    assert(video_playout_select(&state, first, 6, false) == VIDEO_PLAYOUT_HOLD);
    assert(video_playout_select(&state, first, 7, false) == VIDEO_PLAYOUT_OLDEST);
    const int64_t second = state.next_due_us;
    assert(second == first + 83334);
    assert(video_playout_select(&state, second - 1, 7, true) == VIDEO_PLAYOUT_HOLD);
    assert(video_playout_select(&state, second + 20, 6, true) == VIDEO_PLAYOUT_OLDEST);
    const int64_t third = state.next_due_us;
    assert(third == second + 83334); /* Late poll does not skew the cadence. */
    assert(video_playout_select(&state, third, 4, true) == VIDEO_PLAYOUT_OLDEST);
    assert(state.next_due_us == third + 90910); /* <= half after taking one: 11 fps. */
    const int64_t fourth = state.next_due_us;
    assert(video_playout_select(&state, fourth, 2, true) == VIDEO_PLAYOUT_OLDEST);
    assert(state.next_due_us == fourth + 111112); /* <= quarter: 9 fps. */

    const int64_t due = state.next_due_us;
    assert(video_playout_select(&state, due, 0, true) == VIDEO_PLAYOUT_HOLD && state.next_due_us == due);
    assert(video_playout_select(&state, due + 2 * 111112, 2, true) == VIDEO_PLAYOUT_OLDEST);
    assert(state.next_due_us == due + 111112); /* Exactly two intervals: maintain phase. */
    const int64_t late = state.next_due_us + 2 * 111112 + 1;
    assert(video_playout_select(&state, late, 2, true) == VIDEO_PLAYOUT_OLDEST);
    assert(state.next_due_us == late + 111112); /* Three intervals missed: restart phase. */
}

static void continue_and_expire(void)
{
    video_playout_t state;
    video_playout_reset(&state, 10, true, 19);
    assert(!video_playout_arrival(&state, 0));
    assert(video_playout_arrival(&state, 600000));
    assert(video_playout_select(&state, 600000, 1, true) == VIDEO_PLAYOUT_OLDEST); /* Already visible. */
    const int64_t due = state.next_due_us;
    assert(video_playout_arrival(&state, 1400000));
    assert(state.target_depth == 9 && state.started && state.next_due_us == due);
    assert(video_playout_select(&state, 1400000, 1, true) == VIDEO_PLAYOUT_OLDEST);
    assert(video_playout_arrival(&state, 1800000)); /* One moderate sample refreshes while active. */
    assert(state.target_depth == 9 && state.buffered_until_us == 9800000);
    assert(video_playout_buffering(&state, 9799999));
    assert(!video_playout_buffering(&state, 9800000));
    assert(video_playout_select(&state, 9800000, 3, true) == VIDEO_PLAYOUT_LATEST);
    video_playout_reset(&state, 10, true, 19); /* New revision has no old buffering or due time. */
    assert(!video_playout_arrival(&state, 10000000));
    assert(video_playout_select(&state, 10000000, 1, false) == VIDEO_PLAYOUT_LATEST);
}

static void depth_and_low_rate(void)
{
    video_playout_t state;
    video_playout_reset(&state, 1, true, 2);
    assert(!video_playout_arrival(&state, 0));
    assert(video_playout_arrival(&state, 6000000) && state.target_depth == 2);
    assert(video_playout_select(&state, 6000000, 1, false) == VIDEO_PLAYOUT_HOLD);
    assert(video_playout_select(&state, 6000000, 2, false) == VIDEO_PLAYOUT_OLDEST);
    assert(state.next_due_us == 7000000); /* Slowdown cannot divide by zero. */

    video_playout_reset(&state, 255, true, 300);
    assert(!video_playout_arrival(&state, INT64_MIN));
    assert(video_playout_arrival(&state, INT64_MAX - 1));
    assert(state.target_depth == 16 && state.buffered_until_us == INT64_MAX);
    assert(!video_playout_buffering(&state, INT64_MAX));
}

int main(void)
{
    latest_only();
    moderate_gaps();
    prime_and_pace();
    continue_and_expire();
    depth_and_low_rate();
    return 0;
}
