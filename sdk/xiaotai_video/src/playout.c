// SPDX-License-Identifier: MIT
#include "playout.h"

enum {
    MIN_DEPTH = 2, MAX_DEPTH = 16, GAP_PERIODS = 4, IMMEDIATE_PERIODS = 6,
    CONFIRM_WINDOW_US = 2000000, BUFFER_HOLD_US = 8000000,
    US_PER_SECOND = 1000000,
};

static uint32_t period_us(unsigned fps) { return (US_PER_SECOND + fps - 1U) / fps; }

static int64_t after(int64_t now, uint32_t interval)
{
    return now > INT64_MAX - interval ? INT64_MAX : now + interval;
}

static uint64_t elapsed(int64_t later, int64_t earlier)
{
    return (uint64_t)later - (uint64_t)earlier;
}

void video_playout_reset(video_playout_t *state, uint8_t fps, bool adaptive, size_t reservable_frames)
{
    *state = (video_playout_t){0};
    if (!adaptive || !fps || reservable_frames < MIN_DEPTH) return;
    state->max_depth = (uint8_t)(reservable_frames < MAX_DEPTH ? reservable_frames : MAX_DEPTH);
    state->fps = fps;
    state->source_period_us = period_us(fps);
}

bool video_playout_buffering(const video_playout_t *state, int64_t now_us)
{
    return state->target_depth != 0 && now_us < state->buffered_until_us;
}

bool video_playout_arrival(video_playout_t *state, int64_t now_us)
{
    if (!state->source_period_us) return false;
    const bool active = video_playout_buffering(state, now_us);
    if (!state->has_arrival) {
        state->has_arrival = true;
        state->last_arrival_us = now_us;
        return active;
    }
    if (now_us <= state->last_arrival_us) return active;
    const uint64_t gap = elapsed(now_us, state->last_arrival_us);
    state->last_arrival_us = now_us;
    const uint64_t source = state->source_period_us;
    if (gap < GAP_PERIODS * source) return active;
    if (!active && gap < IMMEDIATE_PERIODS * source &&
        (!state->has_suspect || now_us > state->suspect_until_us)) {
        state->suspect_until_us = after(now_us, CONFIRM_WINDOW_US);
        state->has_suspect = true;
        return false;
    }

    /* Ceil without overflow on a long idle, then reserve one more picture. */
    const uint64_t depth = gap / source + (gap % source != 0U) + 1U;
    const uint8_t target = (uint8_t)(depth < state->max_depth ? depth : state->max_depth);
    if (!active) {
        state->started = false;
        state->target_depth = target;
    } else if (target > state->target_depth) {
        state->target_depth = target;  /* Grow reserve without re-priming. */
    }
    state->buffered_until_us = after(now_us, BUFFER_HOLD_US);
    state->has_suspect = false;
    return video_playout_buffering(state, now_us);
}

video_playout_choice_t video_playout_select(video_playout_t *state, int64_t now_us,
                                            size_t ready_frames, bool has_presented)
{
    if (!video_playout_buffering(state, now_us)) {
        state->started = false;
        return ready_frames ? VIDEO_PLAYOUT_LATEST : VIDEO_PLAYOUT_HOLD;
    }
    if (!state->started) {
        if (!has_presented && ready_frames < state->target_depth) return VIDEO_PLAYOUT_HOLD;
        state->started = true;
        state->next_due_us = now_us;
    }
    if (!ready_frames || now_us < state->next_due_us) return VIDEO_PLAYOUT_HOLD;

    const size_t remaining = ready_frames - 1U;
    unsigned slowdown = 0;
    if (state->target_depth >= 4U && remaining <= state->target_depth / 4U) slowdown = 3;
    else if (remaining <= state->target_depth / 2U) slowdown = 1;
    const unsigned fps = state->fps > slowdown ? state->fps - slowdown : 1U;
    const uint32_t interval = period_us(fps);
    const bool missed = elapsed(now_us, state->next_due_us) > 2U * interval;
    state->next_due_us = after(missed ? now_us : state->next_due_us, interval);
    return VIDEO_PLAYOUT_OLDEST;
}
