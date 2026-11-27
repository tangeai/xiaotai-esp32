// SPDX-License-Identifier: MIT
#pragma once

#include "internal.h"
#include "present.h"

typedef struct {
    xiaotai_video_image_t image;
    uint32_t generation;
    uint32_t revision;
    uint32_t reference;
    uint32_t timestamp_ms;
    uint64_t lease_cookie;
    uint8_t slot;
    bool key_frame;
} video_downlink_frame_t;

/* Boot reservation and session tick. Reset runs after the worker tasks exit. */
xiaotai_err_t video_downlink_prepare(void);
bool video_downlink_reset(void);
void video_downlink_monitor(int64_t now_us);
/* Session task, while no session is open: gives back the memory taken on
 * demand. False: something still uses it, ask again. */
bool video_downlink_release_idle(void);
void video_downlink_relay_task(void *arg);
void video_downlink_decode_sw_task(void *arg);
void video_downlink_decode_hw_task(void *arg);

/* The W8 converter takes one frame without copying its pixels, then releases
 * that exact lease. A session switch makes queued images stale but doesn't
 * invalidate pixels already borrowed by the converter. */
bool video_downlink_take_decoded(video_downlink_frame_t *out);
bool video_downlink_take_for_conversion(bool hardware, video_downlink_frame_t *out);
void video_downlink_release_decoded(video_downlink_frame_t *frame);
void video_downlink_finish_conversion(const video_downlink_frame_t *decoded, video_present_write_t *write,
                                      const xiaotai_video_frame_t *frame);
