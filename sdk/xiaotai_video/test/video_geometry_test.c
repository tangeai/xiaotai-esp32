// SPDX-License-Identifier: MIT
// Replaces legacy: tests/native/wt99/test_camera_frame_layout.c
#include <assert.h>
#include "geometry.h"
#include "xiaotai/video_profiles.h"

static const xiaotai_video_scale_limits_t any = {0}, ppa420 = {16, 2, 16}, ppa = {16, 1, 255};

static void test_camera_luma(void)
{
    const uint8_t yuyv[] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    const uint8_t paired[] = {255, 0, 16, 255, 32, 48, 255, 64, 80, 255, 96, 112,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    xiaotai_camera_frame_t frame = {.format = XIAOTAI_CAMERA_YUV422, .width = 4, .height = 2, .rows = 2,
                                    .data = yuyv, .size = sizeof(yuyv)};
    video_luma_t luma = video_luma_sample(&frame);
    assert(luma.state == VIDEO_LUMA_VALID && video_luma_value(luma) == 0 && luma.samples == 8);
    frame.format = XIAOTAI_CAMERA_YUV420_PAIRED;
    frame.data = paired;
    frame.size = sizeof(paired);  /* padding rows and a one-byte device tail, all bright */
    frame.rows = 4;
    luma = video_luma_sample(&frame);
    assert(luma.state == VIDEO_LUMA_VALID && video_luma_value(luma) == 56 && luma.samples == 8);
    frame.size -= 2U;
    luma = video_luma_sample(&frame);
    assert(luma.state == VIDEO_LUMA_INVALID && video_luma_value(luma) == -1 && !luma.samples);
    frame.size = sizeof(paired);
    frame.rows = 3;
    assert(video_luma_sample(&frame).state == VIDEO_LUMA_INVALID);
    frame.rows = 2;
    frame.format = XIAOTAI_CAMERA_JPEG;
    assert(video_luma_sample(&frame).state == VIDEO_LUMA_UNAVAILABLE);
    assert(video_luma_sample(NULL).state == VIDEO_LUMA_NO_FRAME);
}

int main(void)
{
    test_camera_luma();
    xiaotai_video_image_t src = {.pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED, .width = 1280, .height = 960,
                                .rows = 976, .full_range = true};
    const xiaotai_video_profile_t compact = XIAOTAI_VIDEO_PROFILE_H264_COMPACT;
    const xiaotai_video_profile_t sensor = XIAOTAI_VIDEO_PROFILE_H264_SENSOR;
    xiaotai_video_transform_config_t plan;
    assert(video_geometry_uplink(&src, &compact, XIAOTAI_VIDEO_ROTATE_270, src.pixel, &ppa420, &plan));
    assert(plan.in_stride == 1280 && plan.in_height == 960 && plan.in_full_range && !plan.fill);
    assert(plan.crop.x == 256 && plan.crop.y == 224 && plan.crop.width == 768 && plan.crop.height == 512);
    assert(plan.out_width == 384 && plan.out_height == 256 && plan.rotation == XIAOTAI_VIDEO_ROTATE_0);
    assert(video_geometry_uplink(&src, &sensor, XIAOTAI_VIDEO_ROTATE_270, src.pixel, &ppa420, &plan));
    assert(plan.crop.width == 1280 && plan.crop.height == 960 && !plan.crop.x && !plan.crop.y);
    assert(plan.out_width == 720 && plan.out_height == 960 && plan.rotation == XIAOTAI_VIDEO_ROTATE_270);

    src.width = 1024; src.height = 600; src.rows = 608;
    assert(video_geometry_uplink(&src, &compact, XIAOTAI_VIDEO_ROTATE_0, src.pixel, &ppa420, &plan));
    assert(plan.crop.x == 128 && plan.crop.y == 44 && plan.crop.width == 768 && plan.crop.height == 512);
    assert(video_geometry_uplink(&src, &sensor, XIAOTAI_VIDEO_ROTATE_0, src.pixel, &ppa420, &plan));
    assert(plan.in_height == 600);
    assert(plan.out_width == 1024 && plan.out_height == 600 && plan.crop.width == 1024 && plan.crop.height == 600);
    src.width = 640; src.height = 480; src.rows = 480;
    assert(video_geometry_uplink(&src, &compact, XIAOTAI_VIDEO_ROTATE_0, src.pixel, &ppa420, &plan));
    assert(plan.crop.x == 128 && plan.crop.y == 112 && plan.crop.width == 384 && plan.crop.height == 256);
    assert(video_geometry_uplink(&src, &compact, XIAOTAI_VIDEO_ROTATE_0, src.pixel, &any, &plan));
    assert(plan.crop.x == 2 && plan.crop.y == 28 && plan.crop.width == 636 && plan.crop.height == 424);

    uint16_t w = 99, h = 98;
    assert(!video_geometry_derive(0, 480, 0, &any, &w, &h));
    assert(!video_geometry_derive(1280, 960, 47, &any, &w, &h));
    assert(w == 99 && h == 98);
    assert(video_geometry_derive(1280, 960, 720U * 960U, &any, &w, &h) && w == 960 && h == 720);
    assert(video_geometry_derive(65534, 65534, UINT32_MAX, &any, &w, &h) && w == 65534 && h == 65534);
    assert(!video_geometry_derive(65535, 65533, UINT32_MAX, &any, &w, &h)); /* no even exact scale */
    const xiaotai_video_scale_limits_t bad = {16, 0, 16};
    assert(!video_geometry_derive(1024, 600, 0, &bad, &w, &h));
    xiaotai_video_profile_t malformed = compact;
    malformed.width = 385;
    assert(!video_geometry_uplink(&src, &malformed, XIAOTAI_VIDEO_ROTATE_0, src.pixel, &ppa420, &plan));

    xiaotai_video_viewport_t viewport = {.width = 640, .height = 360, .pixel = XIAOTAI_VIDEO_PIXEL_RGB888};
    assert(video_geometry_downlink(&src, &viewport, NULL, &ppa, &plan));
    assert(plan.rotation == XIAOTAI_VIDEO_ROTATE_0 && plan.crop.width == 640 && plan.crop.height == 480);
    assert(plan.place.x == 80 && plan.place.y == 0 && plan.place.width == 480 && plan.place.height == 360);
    xiaotai_video_placement_t placement = {.fit = XIAOTAI_VIDEO_FIT_COVER};
    assert(video_geometry_downlink(&src, &viewport, &placement, &ppa, &plan));
    assert(plan.crop.x == 0 && plan.crop.y == 60 && plan.crop.width == 640 && plan.crop.height == 360 &&
           plan.place.x == 0 && plan.place.y == 0 && plan.place.width == 640 && plan.place.height == 360);
    viewport.width = 360; viewport.height = 640;
    placement.rotation = XIAOTAI_VIDEO_ROTATE_90;
    placement.match_orientation = true;
    assert(video_geometry_downlink(&src, &viewport, &placement, &ppa, &plan));
    assert(plan.rotation == XIAOTAI_VIDEO_ROTATE_90 && plan.place.width == 360 && plan.place.height == 640);
    viewport.width = 640; viewport.height = 360;
    assert(video_geometry_downlink(&src, &viewport, &placement, &ppa, &plan));
    assert(plan.rotation == XIAOTAI_VIDEO_ROTATE_180 && plan.place.width == 640 && plan.place.height == 360);
    src.pixel = XIAOTAI_VIDEO_PIXEL_RGB888; src.width = 641; src.height = 479;
    viewport.width = 319; viewport.height = 241;
    assert(video_geometry_downlink(&src, &viewport, NULL, &any, &plan));
    assert(plan.crop.width == 640 && plan.crop.height == 478 && plan.out_width == 319 && plan.out_height == 241);
    assert(plan.place.width == 318 && plan.place.height == 236);
    src.stride = 630;
    assert(!video_geometry_downlink(&src, &viewport, NULL, &any, &plan));
    assert(video_geometry_render_scale(960, 640, 160, 240) == 4);
    assert(video_geometry_render_scale(1024, 600, 320, 240) == 2);
    assert(video_geometry_render_scale(1023, 599, 160, 120) == 1);
    assert(video_geometry_render_scale(960, 640, 1280, 960) == 1);
    assert(video_geometry_render_scale(0, 640, 320, 240) == 0);
    return 0;
}
