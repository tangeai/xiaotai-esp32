// SPDX-License-Identifier: MIT
#pragma once

#include "internal.h"

typedef struct {
    const xiaotai_video_encoder_t *encoder;
    xiaotai_video_encoder_config_t config;
    const xiaotai_video_transform_t *transform;
    xiaotai_video_transform_config_t conversion;
    xiaotai_camera_mode_t mode;
    size_t transform_bytes;
} video_h264_plan_t;

bool video_h264_plan(const xiaotai_camera_desc_t *camera, const xiaotai_video_profile_t *profile,
                     uint16_t width, uint16_t height, video_h264_plan_t *plan);

/* Warm encoder loaned to the single camera task. */
xiaotai_err_t video_h264_reserve_take(const video_h264_plan_t *plan, uint32_t bitrate, int64_t now,
                                     void **encoder, xiaotai_video_encoder_config_t *config);
xiaotai_err_t video_h264_reserve_rate(uint32_t bitrate, int64_t now, void **encoder,
                                     xiaotai_video_encoder_config_t *config, bool *reopened);
xiaotai_err_t video_h264_reserve_return(int64_t now);

typedef struct {
    int64_t opened_us, pause_until_us;
    bool key_needed;
} video_h264_policy_t;

void video_h264_policy_start(video_h264_policy_t *state, int64_t now);
uint32_t video_h264_period_us(uint8_t fps);
int64_t video_h264_next_us(int64_t previous, int64_t now, uint32_t period);
bool video_h264_protect(video_h264_policy_t *state, uint32_t limit, int64_t now,
                        const xiaotai_video_packet_t *packet, bool *oversize);
void video_h264_send_result(video_h264_policy_t *state, const xiaotai_video_packet_t *packet,
                            xiaotai_err_t result, int64_t now);

/* Only writable transform output is passed to the overlay. */
bool video_overlay_draw(uint8_t *buffer, size_t size, const xiaotai_video_image_t *image, const char *stamp);
bool video_overlay_clock(uint8_t *buffer, size_t size, const xiaotai_video_image_t *image);
