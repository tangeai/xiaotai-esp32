// SPDX-License-Identifier: MIT
#include "uplink.h"

#include "geometry.h"
#include "internal.h"

enum { PRESSURE_WINDOW_US = 5000000, QUALITY_STEP = 5 };

video_jpeg_step_t video_jpeg_evaluate(video_jpeg_pressure_t *state, const xiaotai_video_profile_t *profile,
                                     bool admitted, int64_t now_us, uint32_t sent, uint32_t *fps_x10)
{
    if (now_us - state->window_us < PRESSURE_WINDOW_US) return VIDEO_JPEG_KEEP;
    *fps_x10 = (uint32_t)((uint64_t)(sent - state->sent_before) * 10000000U / (uint64_t)(now_us - state->window_us));
    const bool overloaded = admitted && profile->pressure_limit && state->pressure >= profile->pressure_limit;
    state->pressure = 0;
    state->sent_before = sent;
    state->window_us = now_us;
    if (!overloaded) return VIDEO_JPEG_KEEP;
    if (state->quality > profile->min_quality) {
        const unsigned gap = (unsigned)state->quality - profile->min_quality;
        state->quality = (uint8_t)(state->quality - (gap < QUALITY_STEP ? gap : QUALITY_STEP));
        return VIDEO_JPEG_LOWER_QUALITY;
    }
    if (!state->smaller && profile->fallback_width && profile->fallback_height &&
        (profile->fallback_width != profile->width || profile->fallback_height != profile->height)) {
        state->smaller = true;
        return VIDEO_JPEG_SMALLER;
    }
    return VIDEO_JPEG_KEEP;
}

bool video_jpeg_complete(const xiaotai_video_packet_t *packet, const uint8_t *output, size_t capacity,
                         size_t max_frame_bytes)
{
    if (!packet || !packet->data || !output || packet->size < 4 || packet->size > capacity ||
        packet->size > max_frame_bytes) return false;
    const uintptr_t start = (uintptr_t)packet->data, base = (uintptr_t)output;
    if (start < base || start - base > capacity - packet->size) return false;
    return packet->data[0] == 0xff && packet->data[1] == 0xd8 &&
           packet->data[packet->size - 2] == 0xff && packet->data[packet->size - 1] == 0xd9;
}

bool video_jpeg_can_direct(const xiaotai_camera_desc_t *camera, const xiaotai_video_profile_t *profile)
{
    return (camera->formats & XIAOTAI_CAMERA_YUV422) && (!profile->upright || !(camera->rotation & 3U)) &&
           (!camera->row_align || profile->height % camera->row_align == 0U) &&
           profile->width >= camera->min_width && profile->height >= camera->min_height &&
           profile->width <= camera->max_width && profile->height <= camera->max_height;
}

const xiaotai_video_transform_t *video_jpeg_native_transform(const xiaotai_camera_desc_t *camera,
    const xiaotai_video_profile_t *profile, uint32_t *format, xiaotai_video_transform_config_t *config)
{
    const xiaotai_video_pixel_t to = XIAOTAI_VIDEO_PIXEL_YUYV422;
    const xiaotai_video_rotation_t rotation = (xiaotai_video_rotation_t)(camera->rotation & 3U);
    const xiaotai_video_transform_t *best = NULL;
    int best_score = 0;
    for (uint32_t bit = 1U; bit && bit <= camera->formats; bit <<= 1U) {
        const xiaotai_video_pixel_t from = camera->formats & bit ? video_camera_pixel(bit) : XIAOTAI_VIDEO_PIXEL_NONE;
        if (from == XIAOTAI_VIDEO_PIXEL_NONE) continue;
        const xiaotai_video_image_t native = {.pixel = from, .width = camera->max_width, .height = camera->max_height,
            .full_range = (camera->flags & XIAOTAI_CAMERA_FULL_RANGE) != 0};
        for (size_t i = 0; i < video_registry_count(VIDEO_BACKEND_TRANSFORM); ++i) {
            const xiaotai_video_transform_t *backend = video_transform_at(i);
            xiaotai_video_scale_limits_t limits;
            xiaotai_video_transform_config_t trial;
            if (!backend->limits(from, to, &limits) ||
                !video_geometry_uplink(&native, profile, rotation, to, &limits, &trial) ||
                trial.out_width != profile->width || trial.out_height != profile->height) continue;
            const int score = backend->match(&trial);
            if (score <= best_score) continue;
            best = backend;
            best_score = score;
            *format = bit;
            *config = trial;
        }
    }
    return best;
}
