// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VIDEO_PLAYOUT_HOLD = 0,
    VIDEO_PLAYOUT_LATEST,
    VIDEO_PLAYOUT_OLDEST,
} video_playout_choice_t;

typedef struct {
    int64_t last_arrival_us, suspect_until_us, buffered_until_us, next_due_us;
    uint32_t source_period_us;
    uint8_t fps, max_depth, target_depth;
    bool has_arrival, has_suspect, started;
} video_playout_t;

/* One state per session, serialized by the frame owner. reservable_frames
 * is the number of ready (unpinned) slots the owner can supply. With fewer
 * than two, latest-frame presentation is the only feasible policy. */
void video_playout_reset(video_playout_t *state, uint8_t fps, bool adaptive, size_t reservable_frames);

/* Observe compressed-frame ingress, not decoder/renderer completion. Returns
 * whether arrivals are currently accumulating an ordered reserve. */
bool video_playout_arrival(video_playout_t *state, int64_t now_us);
bool video_playout_buffering(const video_playout_t *state, int64_t now_us);

/* Choose and commit the next presentation while holding the same owner lock
 * used to select the exact slot. ready_frames excludes the pinned image;
 * has_presented refers only to this session. HOLD leaves that pin intact. */
video_playout_choice_t video_playout_select(video_playout_t *state, int64_t now_us,
                                            size_t ready_frames, bool has_presented);
