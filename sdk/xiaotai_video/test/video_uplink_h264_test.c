// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include "fakes.h"
#include "uplink_h264.h"
#include "xiaotai/video_profiles.h"

static uint8_t pixels[384U * 256U * 3U / 2U];
static fake_camera_t *camera;
static uint8_t *output;
static atomic_uint borrowed, released, sent, opened, polls;
static atomic_bool pending, loaned, requested_key, encoded_force;
static atomic_int send_result, encode_result;
static xiaotai_camera_frame_t *loan;
static uint32_t captured_stamp;

static bool in_uplink(void) { return xiaotai_task_is_current(&video_engine.tasks[VIDEO_TASK_UPLINK].handle); }

static xiaotai_err_t take(void *self, xiaotai_camera_frame_t *frame)
{
    assert(self == camera->part.self && in_uplink());
    atomic_fetch_add(&polls, 1U);
    if (!atomic_exchange(&pending, false)) return XIAOTAI_ERR_NOT_FOUND;
    assert(!atomic_exchange(&loaned, true));
    loan = frame;
    *frame = (xiaotai_camera_frame_t){.data = pixels, .size = sizeof(pixels), .width = 384, .height = 256,
        .rows = 256, .format = XIAOTAI_CAMERA_YUV420_PAIRED, .captured_us = xiaotai_time_us() - 1000};
    captured_stamp = (uint32_t)(frame->captured_us / 1000);
    atomic_fetch_add(&borrowed, 1U);
    return XIAOTAI_OK;
}
static xiaotai_err_t release(void *self, xiaotai_camera_frame_t *frame)
{
    assert(self == camera->part.self && in_uplink() && loan == frame && frame->data == pixels);
    assert(atomic_exchange(&loaned, false));
    atomic_fetch_add(&released, 1U);
    return XIAOTAI_OK;
}
static int match(const xiaotai_video_encoder_config_t *c)
{
    return c->codec == XIAOTAI_VIDEO_CODEC_H264 && c->pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED ? 10 : 0;
}
static xiaotai_err_t encoder_open(const xiaotai_video_encoder_config_t *c, void **out)
{
    assert(in_uplink() && camera->log_count && camera->log[0] == FAKE_CAMERA_PREPARE);
    assert(c->key_interval_frames == 255 && c->min_qp == 34 && c->max_qp == 51);
    assert(c->output && c->output_capacity >= 1024U * 1024U);
    output = c->output;
    *out = camera;
    atomic_fetch_add(&opened, 1U);
    return XIAOTAI_OK;
}
static xiaotai_err_t encoder_apply(void *ctx, const xiaotai_video_encoder_config_t *config)
{
    assert(ctx == camera && config->output == output && in_uplink());
    return XIAOTAI_OK;
}
static xiaotai_err_t encode(void *ctx, const xiaotai_video_image_t *image, bool force,
                            xiaotai_video_packet_t *packet)
{
    assert(ctx == camera && in_uplink() && atomic_load(&loaned));
    assert(image->data == pixels && image->rows == 256 && image->size == sizeof(pixels));
    atomic_store(&encoded_force, force);
    const xiaotai_err_t result = atomic_load(&encode_result);
    if (result != XIAOTAI_OK) return result;
    *packet = (xiaotai_video_packet_t){.data = output, .size = 128, .key_frame = force};
    return XIAOTAI_OK;
}
static void encoder_close(void *ctx) { assert(ctx == camera && in_uplink()); }
static const xiaotai_video_encoder_t codec = {.name = "h264-fixture", .load = XIAOTAI_VIDEO_LOAD_HARDWARE,
    .output_alignment = 64, .match = match, .open = encoder_open, .reconfigure = encoder_apply,
    .encode = encode, .close = encoder_close};

static bool ready(void *ctx, uint32_t generation)
{
    (void)ctx;
    assert(generation == 12U && in_uplink());
    return true;
}
static bool peer_key(void *ctx, uint32_t generation)
{
    assert(ready(ctx, generation));
    return atomic_exchange(&requested_key, false);
}
static xiaotai_err_t send(void *ctx, uint32_t generation, const xiaotai_video_packet_t *packet, uint32_t limit)
{
    (void)limit;
    assert(ready(ctx, generation) && !atomic_load(&loaned));
    assert(atomic_load(&released) == atomic_load(&borrowed));
    assert(packet->data == output && packet->width == 384 && packet->height == 256 && packet->size == 128);
    assert(packet->timestamp_ms == captured_stamp);
    atomic_fetch_add(&sent, 1U);
    return atomic_load(&send_result);
}
static const xiaotai_video_link_t link = {.ready = ready, .send = send, .take_key_frame_request = peer_key};

static xiaotai_video_status_t status(void)
{
    xiaotai_video_status_t sample;
    xiaotai_video_status(&sample);
    return sample;
}

static void wait_sent(unsigned expected, bool advance)
{
    for (unsigned i = 0U; i < 1000U && atomic_load(&sent) < expected; ++i) {
        if (advance) fake_clock_advance_us(5000);
        xiaotai_delay_ms(1);
    }
    assert(atomic_load(&sent) == expected);
}

static void wait_failures(uint32_t capture, uint32_t encode)
{
    for (unsigned i = 0; i < 1000U; ++i) {
        const xiaotai_video_uplink_status_t value = status().uplink;
        if (value.capture_failed == capture && value.encode_failed == encode) return;
        xiaotai_delay_ms(1);
    }
    assert(false);
}

static void policy(void)
{
    video_h264_policy_t state;
    video_h264_policy_start(&state, 1000000);
    xiaotai_video_packet_t packet = {.size = 128};
    bool oversize;
    assert(!video_h264_protect(&state, 256, 3499999, &packet, &oversize) && !oversize);
    packet.key_frame = true;
    assert(video_h264_protect(&state, 256, 1100000, &packet, &oversize));
    video_h264_send_result(&state, &packet, XIAOTAI_OK, 1100000);
    packet.key_frame = false;
    ++packet.size;
    assert(!video_h264_protect(&state, 256, 3499999, &packet, &oversize) && oversize);
    state.key_needed = false;
    assert(video_h264_protect(&state, 256, 3500000, &packet, &oversize));
    video_h264_send_result(&state, &packet, XIAOTAI_ERR_INVALID_STATE, 3500000);
    assert(state.key_needed && !state.pause_until_us);
    video_h264_send_result(&state, &packet, XIAOTAI_ERR_BUSY, 3500000);
    assert(state.pause_until_us == 3580000);
    assert(video_h264_period_us(12) == 84000 && video_h264_period_us(30) == 34000);
    assert(video_h264_next_us(1000000, 1200000, 84000) == 1252000);
}

int main(void)
{
    policy();
    assert(xiaotai_engine_video_init() == XIAOTAI_OK);
    const xiaotai_camera_desc_t desc = {.max_width = 384, .max_height = 256, .min_width = 384, .min_height = 256,
        .formats = XIAOTAI_CAMERA_YUV420_PAIRED, .row_align = 16};
    camera = fake_camera_add(&desc, true);
    assert(camera);
    camera->part.borrow = take;
    camera->part.release = release;
    assert(xiaotai_video_encoder_register(&codec) == XIAOTAI_OK);
    xiaotai_video_profile_t profile = XIAOTAI_VIDEO_PROFILE_H264_COMPACT;
    profile.fps = 30;
    assert(xiaotai_video_require_uplink(&profile) == XIAOTAI_OK);
    fake_clock_hold(1000000);
    xiaotai_engine_video_start();
    assert(opened == 1 && camera->log_count == 1); /* boot reservation followed prepare, no streaming */
    const xiaotai_video_session_t session = {.generation = 12, .uplink = &profile, .link = &link};
    assert(xiaotai_video_open(&session) == XIAOTAI_OK);
    for (unsigned i = 0; i < 1000U && !atomic_load(&polls); ++i) {
        fake_clock_advance_us(5000);
        xiaotai_delay_ms(1);
    }
    assert(atomic_load(&polls) && !status().uplink.capture_failed);
    fake_clock_advance_us(3000000);
    wait_failures(1, 0);
    atomic_store(&pending, true);
    wait_sent(1, true);
    for (unsigned i = 0; i < 1000U && !status().uplink.first_frame_recovered; ++i) xiaotai_delay_ms(1);
    assert(status().uplink.first_frame_reopens == 1 && status().uplink.first_frame_recovered == 1 &&
           !status().uplink.first_frame_exhausted);
    assert(camera->log_count == 7 && camera->log[3] == FAKE_CAMERA_CLOSE && camera->log[4] == FAKE_CAMERA_POWER_OFF &&
           camera->log[5] == FAKE_CAMERA_POWER_ON && camera->log[6] == FAKE_CAMERA_OPEN);
    assert(atomic_load(&encoded_force));
    fake_clock_advance_us(40000);
    atomic_store(&requested_key, true);
    atomic_store(&send_result, XIAOTAI_ERR_BUSY);
    atomic_store(&pending, true);
    wait_sent(2, false);
    assert(atomic_load(&encoded_force));
    for (unsigned i = 0; i < 1000U && !status().uplink.backpressure; ++i) xiaotai_delay_ms(1);
    assert(status().uplink.backpressure == 1);
    atomic_store(&send_result, XIAOTAI_OK);
    atomic_store(&pending, true);
    fake_clock_advance_us(79999);
    xiaotai_delay_ms(20);
    assert(atomic_load(&borrowed) == 2U);
    fake_clock_advance_us(1);
    wait_sent(3, false);
    assert(atomic_load(&encoded_force));
    fake_clock_advance_us(40000);
    atomic_store(&requested_key, true);
    atomic_store(&pending, true);
    wait_sent(4, false);
    assert(!atomic_load(&encoded_force)); /* repeated peer request in the window */
    fake_clock_advance_us(40000);
    xiaotai_video_request_key_frame(12, XIAOTAI_VIDEO_KEY_NEW_VIEWER);
    atomic_store(&pending, true);
    wait_sent(5, false);
    assert(atomic_load(&encoded_force) && opened == 1);

    unsigned before = atomic_load(&polls);
    fake_clock_advance_us(2999999);
    for (unsigned i = 0; i < 1000U && atomic_load(&polls) <= before; ++i) xiaotai_delay_ms(1);
    assert(atomic_load(&polls) > before && status().uplink.capture_failed == 1);
    fake_clock_advance_us(1);
    wait_failures(2, 0);
    assert(status().uplink.first_frame_reopens == 1 && camera->log_count == 7);
    atomic_store(&encode_result, XIAOTAI_ERR_INVALID_SIZE);
    atomic_store(&pending, true);
    fake_clock_advance_us(199999);
    xiaotai_delay_ms(10);
    assert(atomic_load(&borrowed) == 5U);
    fake_clock_advance_us(1);
    wait_failures(2, 1);
    assert(atomic_load(&released) == 6U && atomic_load(&sent) == 5U);
    atomic_store(&encode_result, XIAOTAI_OK);
    atomic_store(&pending, true);
    fake_clock_advance_us(199999);
    xiaotai_delay_ms(10);
    assert(atomic_load(&borrowed) == 6U);
    fake_clock_advance_us(1);
    wait_sent(6, false);
    assert(atomic_load(&encoded_force) && opened == 1);

    xiaotai_video_close(12);
    for (unsigned i = 0; i < 1000U && status().uplink.phase != XIAOTAI_VIDEO_OFF; ++i) xiaotai_delay_ms(1);
    assert(status().uplink.phase == XIAOTAI_VIDEO_OFF);
    assert(!atomic_load(&loaned));
    xiaotai_video_reset();
    fake_clock_release();
    return 0;
}
