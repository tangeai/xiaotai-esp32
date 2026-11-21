// SPDX-License-Identifier: MIT
#pragma once

#include "internal.h"

typedef struct {
    uint8_t *pixels;
    size_t capacity;
    uint64_t cookie;
    uint8_t slot;
} video_present_write_t;

/* The owner reserves final frames once, from the declared decoder/viewport
 * pairs. Reset follows worker shutdown, and refuses to free a held lease. */
xiaotai_err_t video_present_prepare(void);
bool video_present_reset(void);
uint8_t video_present_render_scale(const xiaotai_video_viewport_t *viewport,
                                   const xiaotai_video_downlink_t *downlink);

xiaotai_err_t video_present_begin(const video_session_config_t *config);
void video_present_stop(uint32_t revision);
/* Drop unpublished/queued pictures of a broken reference chain, not the
 * picture the presenter is still using. */
void video_present_flush(uint32_t revision);
void video_present_arrival(uint32_t revision, int64_t arrival_us);
bool video_present_buffering(uint32_t revision, int64_t now_us);

/* The converter alone writes a claimed slot. Publish and cancel consume the
 * lease; a publish after a flush or a revision change returns false. */
bool video_present_claim(uint32_t revision, size_t required, video_present_write_t *out);
bool video_present_publish(video_present_write_t *write, const xiaotai_video_frame_t *frame);
void video_present_cancel(video_present_write_t *write);
