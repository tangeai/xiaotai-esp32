// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/port/err.h"
#include "xiaotai/video.h"

typedef enum {
    VIDEO_CAPTURE_EMPTY = 0,
    VIDEO_CAPTURE_ARMED,
    VIDEO_CAPTURE_RECORDING,
    VIDEO_CAPTURE_STOPPED,
} video_capture_phase_t;

typedef struct {
    video_capture_phase_t phase;
    size_t bytes;
    uint32_t frames, generation, revision;
    uint16_t width, height;
    const char *reason; /* stable diagnostic label */
} video_capture_status_t;

typedef struct {
    const uint8_t *data;
    video_capture_status_t status;
    void *ctx; /* opaque lease identity for the blob release callback */
} video_capture_dump_t;

/* The engine initializes this before publishing the AT commands and resets
 * after worker shutdown. A leased blob must be released before reset. */
xiaotai_err_t video_capture_init(void);
bool video_capture_reset(void);

/* START is admitted only by the AT handler after checking the running uplink. */
xiaotai_err_t video_capture_start(uint32_t generation, uint32_t revision, int64_t now_us);
void video_capture_status(video_capture_status_t *out, int64_t now_us);
void video_capture_stop(void);
xiaotai_err_t video_capture_clear(void);
xiaotai_err_t video_capture_dump_take(bool session_open, video_capture_dump_t *out);
void video_capture_dump_release(void *ctx);

/* Worker-side hooks never wait, log, or allocate. */
void video_capture_offer(uint32_t generation, uint32_t revision, const xiaotai_video_packet_t *packet,
                         int64_t now_us);
void video_capture_session_ended(uint32_t revision);
void video_capture_tick(int64_t now_us);
