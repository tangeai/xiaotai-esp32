// SPDX-License-Identifier: MIT
#include "uplink_h264.h"

#include "xiaotai/video_governor.h"

enum { STARTUP_GUARD_US = 2500000, US_PER_MS = 1000 };

void video_h264_policy_start(video_h264_policy_t *state, int64_t now)
{
    *state = (video_h264_policy_t){.opened_us = now, .key_needed = true};
}

uint32_t video_h264_period_us(uint8_t fps)
{
    return fps ? ((1000U + fps - 1U) / fps) * US_PER_MS : 0;
}

int64_t video_h264_next_us(int64_t previous, int64_t now, uint32_t period)
{
    if (!period) return now;
    uint64_t periods = 1;
    if (now >= previous) periods = ((uint64_t)now - (uint64_t)previous) / period + 1U;
    if (periods > (uint64_t)INT64_MAX / period) return INT64_MAX;
    const int64_t advance = (int64_t)(periods * period);
    return previous > INT64_MAX - advance ? INT64_MAX : previous + advance;
}

bool video_h264_protect(video_h264_policy_t *state, uint32_t limit, int64_t now,
                        const xiaotai_video_packet_t *packet, bool *oversize)
{
    *oversize = false;
    if (packet->key_frame) return true;
    if (limit && now >= state->opened_us &&
        (uint64_t)now - (uint64_t)state->opened_us < STARTUP_GUARD_US) limit = limit > 1U ? limit / 2U : 1U;
    if (limit && packet->size > limit) {
        state->key_needed = true;
        *oversize = true;
        return false;
    }
    return !state->key_needed;
}

void video_h264_send_result(video_h264_policy_t *state, const xiaotai_video_packet_t *packet,
                            xiaotai_err_t result, int64_t now)
{
    if (result == XIAOTAI_OK) {
        if (packet->key_frame) state->key_needed = false;
    } else if (result == XIAOTAI_ERR_INVALID_STATE) {
        /* No congestion for a withdrawn link, but a produced frame that
         * never reached the peer cannot be its next reference. */
        state->key_needed = true;
    } else {
        const int64_t hold = XIAOTAI_VIDEO_BACKPRESSURE_HOLD_MS * US_PER_MS;
        state->key_needed = true;
        state->pause_until_us = now > INT64_MAX - hold ? INT64_MAX : now + hold;
    }
}
