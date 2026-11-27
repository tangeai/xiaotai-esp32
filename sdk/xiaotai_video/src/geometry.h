// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/hal.h"
#include "xiaotai/video.h"
#include "xiaotai/video_backend.h"

/* Exact proportional size of the whole sensor picture, in sensor axes.
 * A limit of zero leaves the original size. No output changes on failure. */
bool video_geometry_derive(uint16_t width, uint16_t height, uint32_t max_pixels,
                           const xiaotai_video_scale_limits_t *limits, uint16_t *out_width, uint16_t *out_height);

/* Source dimensions and stride are visible picture properties. Padded camera
 * rows are deliberately not part of the geometry. Width/height in profile
 * are in output axes (after mounting rotation when upright is true). */
bool video_geometry_uplink(const xiaotai_video_image_t *source, const xiaotai_video_profile_t *profile,
                           xiaotai_video_rotation_t mounting, xiaotai_video_pixel_t output,
                           const xiaotai_video_scale_limits_t *limits, xiaotai_video_transform_config_t *plan);

bool video_geometry_downlink(const xiaotai_video_image_t *source, const xiaotai_video_viewport_t *viewport,
                             const xiaotai_video_placement_t *placement, const xiaotai_video_scale_limits_t *limits,
                             xiaotai_video_transform_config_t *plan);

/* A render target must have integer scale in both axes. For JPEG either input
 * orientation may be placed in the viewport without upsampling its buffer. */
uint8_t video_geometry_render_scale(uint16_t width, uint16_t height, uint16_t max_width, uint16_t max_height);

/* Checked byte size (on both 32- and 64-bit cores). Unlike a camera descriptor,
 * this function describes a storage layout and honors width/row parity. */
size_t video_geometry_image_bytes(xiaotai_video_pixel_t pixel, uint16_t stride, uint16_t rows);
unsigned video_geometry_rgb_bytes(xiaotai_video_pixel_t pixel);

typedef enum {
    VIDEO_LUMA_NO_FRAME = 0, VIDEO_LUMA_INVALID, VIDEO_LUMA_UNAVAILABLE, VIDEO_LUMA_VALID,
} video_luma_state_t;

typedef struct {
    video_luma_state_t state;
    uint8_t mean, samples;
} video_luma_t;

/* Raw Y over a bounded grid of visible pixels; never reads chroma or padded rows. */
video_luma_t video_luma_sample(const xiaotai_camera_frame_t *frame);
const char *video_luma_name(video_luma_state_t state);
int video_luma_value(video_luma_t luma); /* -1 means absent, never a Y sample */

extern const xiaotai_video_transform_t video_transform_sw;
