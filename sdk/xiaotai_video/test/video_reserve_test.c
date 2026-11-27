// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_camera_geometry.py
#include <assert.h>
#include "fakes.h"
#include "uplink.h"
#include "uplink_h264.h"
#include "xiaotai/video_profiles.h"

static fake_camera_t *camera;
static int context;
static unsigned opened, closed, reconfigured;
static uint16_t last_width;
static bool fail_open, fallback_reconfigure;
static uint8_t *output;

static size_t internal_bytes(const xiaotai_video_encoder_config_t *c) { return c->width * 64U; }
static int match(const xiaotai_video_encoder_config_t *c)
{
    return c->codec == XIAOTAI_VIDEO_CODEC_H264 && c->pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED ? 10 : 0;
}
static xiaotai_err_t open_encoder(const xiaotai_video_encoder_config_t *c, void **out)
{
    /* Same input backing across incompatible geometries. */
    assert(c->output && c->output_capacity >= 1024U * 1024U && (uintptr_t)c->output % 64U == 0);
    assert(!output || output == c->output);
    output = c->output;
    ++opened;
    last_width = c->width;
    if (fail_open) { *out = NULL; return XIAOTAI_ERR_NO_MEM; }
    *out = &context;
    return XIAOTAI_OK;
}
static xiaotai_err_t reconfigure(void *self, const xiaotai_video_encoder_config_t *config)
{
    assert(self == &context && config->output == output);
    ++reconfigured;
    return fallback_reconfigure ? XIAOTAI_ERR_NOT_SUPPORTED : XIAOTAI_OK;
}
static xiaotai_err_t encode(void *self, const xiaotai_video_image_t *image, bool key, xiaotai_video_packet_t *packet)
{
    (void)self; (void)image; (void)key; (void)packet;
    return XIAOTAI_FAIL;
}
static void close_encoder(void *self) { assert(self == &context); ++closed; }
static const xiaotai_video_encoder_t backend = {
    .name = "reserve-fixture", .load = XIAOTAI_VIDEO_LOAD_HARDWARE, .output_alignment = 64,
    .internal_bytes = internal_bytes, .match = match, .open = open_encoder, .reconfigure = reconfigure,
    .encode = encode, .close = close_encoder,
};

int main(void)
{
    assert(xiaotai_engine_video_init() == XIAOTAI_OK);
    const xiaotai_camera_desc_t desc = {.max_width = 1280, .max_height = 960, .min_width = 1280, .min_height = 960,
        .formats = XIAOTAI_CAMERA_YUV420_PAIRED, .row_align = 16};
    camera = fake_camera_add(&desc, false);
    assert(camera);
    assert(xiaotai_video_encoder_register(&backend) == XIAOTAI_OK);
    const xiaotai_video_profile_t small = XIAOTAI_VIDEO_PROFILE_H264_COMPACT;
    xiaotai_video_profile_t large = XIAOTAI_VIDEO_PROFILE_H264_SENSOR;
    large.upright = false;
    large.max_pixels = 0;
    assert(xiaotai_video_require_uplink(&small) == XIAOTAI_OK);
    assert(xiaotai_video_require_uplink(&large) == XIAOTAI_OK);
    fake_clock_hold(1000000);
    assert(video_uplink_reserve_prepare() == XIAOTAI_OK && opened == 1 && last_width == 1280);
    video_h264_plan_t compact, native;
    assert(video_h264_plan(&desc, &small, 384, 256, &compact));
    assert(compact.transform && compact.mode.width == 1280 && compact.mode.height == 960 && compact.config.rows == 256);
    assert(compact.conversion.crop.x == 0 && compact.conversion.crop.y == 54);
    assert(compact.config.key_interval_frames == 192);
    assert(video_h264_plan(&desc, &large, 1280, 960, &native) && !native.transform);
    assert(native.config.key_interval_frames == 30);
    void *reserved;
    xiaotai_video_encoder_config_t config;
    assert(video_h264_reserve_take(&native, 1750000, 1000000, &reserved, &config) == XIAOTAI_OK);
    assert(reserved == &context && opened == 1 && reconfigured == 1 && config.bitrate_bps == 1750000);
    bool restarted;
    fallback_reconfigure = true;
    assert(video_h264_reserve_rate(1400000, 1000001, &reserved, &config, &restarted) == XIAOTAI_OK);
    assert(restarted && reserved == &context && opened == 2 && closed == 1);
    fallback_reconfigure = false;
    assert(video_h264_reserve_return(1000002) == XIAOTAI_OK);
    assert(video_h264_reserve_take(&compact, 224000, 1000003, &reserved, &config) == XIAOTAI_OK);
    assert(opened == 3 && closed == 2 && config.width == 384 && config.output == output);
    size_t capacity;
    assert(video_uplink_reserve_transform(compact.transform_bytes, 64U, &capacity) && capacity >= compact.transform_bytes);
    assert(video_h264_reserve_return(1000004) == XIAOTAI_OK && opened == 4 && last_width == 1280);
    fail_open = true;
    assert(video_h264_reserve_take(&compact, 224000, 2000000, &reserved, &config) == XIAOTAI_ERR_NO_MEM);
    assert(opened == 5 && closed == 4);
    fail_open = false;
    assert(video_h264_reserve_take(&compact, 224000, 3999999, &reserved, &config) == XIAOTAI_ERR_BUSY && opened == 5);
    assert(video_h264_reserve_take(&compact, 224000, 4000000, &reserved, &config) == XIAOTAI_OK && opened == 6);
    assert(video_h264_reserve_return(4000001) == XIAOTAI_OK && opened == 7 && last_width == 1280);
    video_uplink_reserve_reset();
    assert(closed == 6); /* one of the seven attempts never produced a handle */
    xiaotai_video_reset();
    fake_clock_release();
    return 0;
}
