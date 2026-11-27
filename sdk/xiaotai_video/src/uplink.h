// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/hal.h"
#include "xiaotai/video.h"
#include "xiaotai/video_backend.h"

/* The engine owner waits for the camera task to prepare all registered parts.
 * Only the camera task calls the part; a failure disables the uplink, not boot. */
bool video_uplink_prepare_boot(void);
void video_uplink_task(void *arg);
void video_uplink_reset(void);
xiaotai_err_t video_uplink_reserve_prepare(void);
uint8_t *video_uplink_reserve_transform(size_t required, size_t alignment, size_t *capacity);
void video_uplink_reserve_reset(void);

typedef enum {
    VIDEO_JPEG_KEEP,
    VIDEO_JPEG_LOWER_QUALITY,
    VIDEO_JPEG_SMALLER,
} video_jpeg_step_t;

typedef struct {
    int64_t window_us;
    uint32_t pressure, sent_before;
    uint8_t quality;
    bool smaller;
} video_jpeg_pressure_t;

video_jpeg_step_t video_jpeg_evaluate(video_jpeg_pressure_t *state, const xiaotai_video_profile_t *profile,
                                     bool admitted, int64_t now_us, uint32_t sent, uint32_t *fps_x10);
bool video_jpeg_complete(const xiaotai_video_packet_t *packet, const uint8_t *output, size_t capacity,
                         size_t max_frame_bytes);

bool video_jpeg_can_direct(const xiaotai_camera_desc_t *camera, const xiaotai_video_profile_t *profile);
const xiaotai_video_transform_t *video_jpeg_native_transform(const xiaotai_camera_desc_t *camera,
    const xiaotai_video_profile_t *profile, uint32_t *format, xiaotai_video_transform_config_t *config);
