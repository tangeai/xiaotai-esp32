// SPDX-License-Identifier: MIT
#include <assert.h>
#include <string.h>

#include "fakes.h"
#include "geometry.h"
#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"

static const xiaotai_video_downlink_t h264 = {.codec = XIAOTAI_VIDEO_CODEC_H264, .max_width = 30,
    .max_height = 30, .fps = 12, .max_frame_bytes = 32, .burst_ms = 2000};
static const xiaotai_video_downlink_t jpeg = {.codec = XIAOTAI_VIDEO_CODEC_JPEG, .max_width = 64,
    .max_height = 48, .max_frame_bytes = 32};

static atomic_bool block_convert;
static atomic_uint transform_opens, transform_runs;
static atomic_uint jpeg_transform_opens, jpeg_transform_updates, jpeg_transform_runs;
static xiaotai_signal_t entered, proceed;
static int64_t now;

#define WAIT(condition) do { \
    for (unsigned tries = 0; !(condition) && tries < 2000U; ++tries) xiaotai_delay_ms(1); \
    assert(condition); \
} while (0)

static xiaotai_video_status_t status(void)
{
    xiaotai_video_status_t result;
    xiaotai_video_status(&result);
    return result;
}

static int find_h264(const xiaotai_video_decoder_config_t *c) { return c->codec == h264.codec; }
static int find_jpeg(const xiaotai_video_decoder_config_t *c) { return c->codec == jpeg.codec; }

static xiaotai_err_t open_decoder(const xiaotai_video_decoder_config_t *c, void **out)
{
    static const uint8_t ids[] = {0, 1};
    *out = (void *)&ids[c->codec == jpeg.codec];
    return XIAOTAI_OK;
}

static xiaotai_err_t decode(void *state, const xiaotai_video_packet_t *p, uint8_t *out,
                            size_t capacity, xiaotai_video_image_t *image)
{
    const bool hardware = *(const uint8_t *)state != 0;
    assert(xiaotai_task_is_current(&video_engine.tasks[hardware ? VIDEO_TASK_DECODE_HARDWARE : VIDEO_TASK_DECODE_SOFTWARE].handle));
    const uint16_t width = hardware ? p->width : 30, height = hardware ? p->height : 30;
    const uint16_t stride = hardware ? width : 32, rows = hardware ? height : 32;
    const uint8_t value = p->data[p->size - 1U];
    const xiaotai_video_pixel_t pixel = hardware ? XIAOTAI_VIDEO_PIXEL_RGB565 : XIAOTAI_VIDEO_PIXEL_I420;
    const size_t bytes = video_geometry_image_bytes(pixel, stride, rows);
    assert(bytes && bytes <= capacity);
    memset(out, value, hardware ? bytes : (size_t)stride * rows);
    if (!hardware) memset(out + (size_t)stride * rows, 128, bytes - (size_t)stride * rows);
    *image = (xiaotai_video_image_t){.pixel = pixel, .width = width, .height = height, .rows = rows,
        .stride = stride, .data = out, .size = bytes};
    return XIAOTAI_OK;
}

static void close_decoder(void *state) { assert(state); }

static const xiaotai_video_decoder_t sd = {.name = "source-sw", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
    .output = XIAOTAI_VIDEO_PIXEL_I420, .match = find_h264, .open = open_decoder, .decode = decode, .close = close_decoder};
static const xiaotai_video_decoder_t hd = {.name = "source-hw", .load = XIAOTAI_VIDEO_LOAD_HARDWARE,
    .output = XIAOTAI_VIDEO_PIXEL_RGB565, .match = find_jpeg, .open = open_decoder, .decode = decode, .close = close_decoder};

static bool accel_limits(xiaotai_video_pixel_t in, xiaotai_video_pixel_t out, xiaotai_video_scale_limits_t *limits)
{
    *limits = (xiaotai_video_scale_limits_t){.denominator = 16, .step = 1, .max = 255};
    return in == XIAOTAI_VIDEO_PIXEL_I420 && out == XIAOTAI_VIDEO_PIXEL_RGB565;
}

static int accel_match(const xiaotai_video_transform_config_t *c)
{
    return c->in_pixel == XIAOTAI_VIDEO_PIXEL_I420 && c->out_pixel == XIAOTAI_VIDEO_PIXEL_RGB565 ? 10 : 0;
}

static xiaotai_err_t accel_open(const xiaotai_video_transform_config_t *plan, void **out)
{
    assert(plan->out_width == 48 && plan->out_height == 32);
    static int context;
    *out = &context;
    atomic_fetch_add(&transform_opens, 1U);
    return XIAOTAI_OK;
}

static xiaotai_err_t accel_run(void *state, const uint8_t *in, size_t in_size, uint8_t *out, size_t capacity)
{
    assert(state && in && in_size == 1350 && in[899] == in[0] && in[900] == 128 && in[1349] == 128);
    assert(out && capacity >= 3072 && (uintptr_t)out % 64U == 0U);
    assert(xiaotai_task_is_current(&video_engine.tasks[VIDEO_TASK_CONVERT].handle));
    atomic_fetch_add(&transform_runs, 1U);
    if (atomic_exchange(&block_convert, false)) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&proceed, 3000));
    }
    memset(out, 0xa5, 3072);
    return XIAOTAI_ERR_TIMEOUT; /* A failed accelerator may leave partial pixels. */
}

static const xiaotai_video_transform_t accel = {.name = "failing-transform", .load = XIAOTAI_VIDEO_LOAD_HARDWARE,
    .output_alignment = 64, .match = accel_match, .limits = accel_limits, .open = accel_open,
    .run = accel_run, .close = close_decoder};

static bool rgb_limits(xiaotai_video_pixel_t in, xiaotai_video_pixel_t out, xiaotai_video_scale_limits_t *limits)
{
    *limits = (xiaotai_video_scale_limits_t){0};
    return in == XIAOTAI_VIDEO_PIXEL_RGB565 && out == XIAOTAI_VIDEO_PIXEL_RGB565;
}

static int rgb_match(const xiaotai_video_transform_config_t *c)
{
    return c->in_pixel == XIAOTAI_VIDEO_PIXEL_RGB565 && c->out_pixel == XIAOTAI_VIDEO_PIXEL_RGB565 ? 10 : 0;
}

static xiaotai_err_t rgb_open(const xiaotai_video_transform_config_t *c, void **out)
{
    static int resource;
    assert(c->in_width == 64 && c->in_height == 48); /* declared budget at boot */
    *out = &resource;
    atomic_fetch_add(&jpeg_transform_opens, 1U);
    return XIAOTAI_OK;
}

static xiaotai_err_t rgb_update(void *state, const xiaotai_video_transform_config_t *c)
{
    assert(state && c->in_width == 48 && c->in_height == 48);
    assert(xiaotai_task_is_current(&video_engine.tasks[VIDEO_TASK_DECODE_HARDWARE].handle));
    atomic_fetch_add(&jpeg_transform_updates, 1U);
    return XIAOTAI_OK;
}

static xiaotai_err_t rgb_run(void *state, const uint8_t *src, size_t span, uint8_t *dst, size_t capacity)
{
    assert(state && src && dst && span == 48U * 48U * 2U && capacity >= 96U * 64U * 2U);
    assert(xiaotai_task_is_current(&video_engine.tasks[VIDEO_TASK_DECODE_HARDWARE].handle));
    atomic_fetch_add(&jpeg_transform_runs, 1U);
    return XIAOTAI_ERR_TIMEOUT;
}

static const xiaotai_video_transform_t rgb_accel = {.name = "changing-transform", .load = XIAOTAI_VIDEO_LOAD_HARDWARE,
    .output_alignment = 64, .match = rgb_match, .limits = rgb_limits, .open = rgb_open,
    .run = rgb_run, .close = close_decoder, .reconfigure = rgb_update};

static void setup(bool independent, bool with_h264)
{
    now = 1000000;
    fake_clock_hold(now);
    fake_hal_reset();
    fake_port_clear();
    atomic_store(&transform_opens, 0U);
    atomic_store(&transform_runs, 0U);
    atomic_store(&jpeg_transform_opens, 0U);
    atomic_store(&jpeg_transform_updates, 0U);
    atomic_store(&jpeg_transform_runs, 0U);
    assert(xiaotai_signal_init(&entered) == XIAOTAI_OK && xiaotai_signal_init(&proceed) == XIAOTAI_OK);
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){0}) == XIAOTAI_OK);
    assert(xiaotai_engine_video_init() == XIAOTAI_OK);
    assert(xiaotai_video_configure(&(const xiaotai_video_config_t){.buffer_independent_frames = independent}) == XIAOTAI_OK);
    assert(fake_display_add(96, 64, XIAOTAI_PIXEL_RGB565, 0));
    xiaotai_video_viewport_t view;
    assert(xiaotai_video_viewport_for_display(0, &view) == XIAOTAI_OK);
    assert(xiaotai_video_require_viewport(&view) == XIAOTAI_OK);
    if (with_h264) {
        assert(xiaotai_video_decoder_register(&sd) == XIAOTAI_OK);
        assert(xiaotai_video_transform_register(&accel) == XIAOTAI_OK);
        assert(xiaotai_video_require_downlink(&h264) == XIAOTAI_OK);
    }
    assert(xiaotai_video_transform_register(&rgb_accel) == XIAOTAI_OK);
    assert(xiaotai_video_decoder_register(&hd) == XIAOTAI_OK);
    assert(xiaotai_video_require_downlink(&jpeg) == XIAOTAI_OK);
    xiaotai_engine_video_start();
    assert(video_engine.downlink_available && video_started());
    assert(atomic_load(&transform_opens) == (with_h264 ? 1U : 0U));
    assert(atomic_load(&jpeg_transform_opens) == 1U && !atomic_load(&jpeg_transform_updates));
}

static void teardown(void)
{
    xiaotai_video_frame_release();
    xiaotai_video_reset();
    xiaotai_framework_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&entered);
    xiaotai_signal_deinit(&proceed);
    fake_clock_release();
}

static void open_session(uint32_t generation, const xiaotai_video_downlink_t *config,
                         const xiaotai_video_placement_t *placement)
{
    assert(xiaotai_video_open(&(const xiaotai_video_session_t){.generation = generation,
        .downlink = config, .link = fake_link(), .placement = placement}) == XIAOTAI_OK);
    WAIT(status().generation == generation);
}

static void advance(int64_t us) { now += us; fake_clock_hold(now); }

static void feed(uint32_t generation, bool independent, bool key, uint8_t value)
{
    const uint8_t idr[] = {0,0,1,0x67,66,0x40,31,0x80,0,0,1,0x68,0x80,0,0,1,0x65,value};
    const uint8_t delta[] = {0,0,1,0x61,value};
    assert(xiaotai_video_push(generation, &(const xiaotai_video_packet_t){.codec = independent ? jpeg.codec : h264.codec,
        .width = 48, .height = independent ? 48 : 32, .timestamp_ms = value, .data = independent ? &value : (key ? idr : delta),
        .size = independent ? 1U : (key ? sizeof(idr) : sizeof(delta))}));
}

static uint16_t pixel(const xiaotai_video_frame_t *frame, unsigned x, unsigned y)
{
    uint16_t p;
    memcpy(&p, (const uint8_t *)frame->pixels + 2U * ((size_t)y * frame->width + x), sizeof(p));
    return p;
}

static xiaotai_video_frame_t acquire(void)
{
    xiaotai_video_frame_t frame;
    assert(xiaotai_video_frame_acquire(&frame));
    return frame;
}

static void convert_and_switch(void)
{
    setup(false, true);
    open_session(1, &h264, NULL);
    feed(1, false, true, 64);
    WAIT(status().downlink.converted == 1 && xiaotai_video_remote_generation() == 1);
    xiaotai_video_frame_t pin = acquire();
    assert(pin.generation == 1 && pin.sequence == 1 && pin.width == 48 && pin.height == 32 && pin.scale == 2);
    assert(pin.picture.x == 8 && pin.picture.y == 0 && pin.picture.width == 32 && pin.picture.height == 32);
    const uint16_t previous = pixel(&pin, 8, 0);
    assert(pixel(&pin, 7, 0) == 0 && previous != 0 && previous != 0xa5a5);
    uint16_t painted[96] = {0};
    const xiaotai_video_area_t area = {0,0,96,64};
    assert(xiaotai_video_frame_paint(&(const xiaotai_video_surface_t){.pixels = (uint8_t *)painted,
        .width = 96, .height = 1, .stride = sizeof(painted), .pixel = XIAOTAI_VIDEO_PIXEL_RGB565}, &area));
    assert(!painted[15] && painted[16] == previous && painted[17] == previous);

    /* A blocked converter owns one source. Outside buffering, its next wake
     * takes the newest pending source, not all three. */
    atomic_store(&block_convert, true);
    feed(1, false, false, 72);
    assert(xiaotai_signal_take(&entered, 3000));
    feed(1, false, false, 88);
    feed(1, false, false, 112);
    feed(1, false, false, 144);
    WAIT(status().downlink.decoded == 5);
    xiaotai_signal_give(&proceed);
    WAIT(status().downlink.converted == 3);
    assert(atomic_load(&transform_runs) == 3 && atomic_load(&transform_opens) == 1);
    assert(pixel(&pin, 8, 0) == previous);
    pin = acquire();
    assert(pin.sequence == 2 && pixel(&pin, 8, 0) > previous);

    /* Close while converting: held old output must not cross to the new
     * session even as JPEG converts independently on its decoder task. */
    atomic_store(&block_convert, true);
    feed(1, false, false, 152);
    assert(xiaotai_signal_take(&entered, 3000));
    open_session(2, &jpeg, &(const xiaotai_video_placement_t){.fit = XIAOTAI_VIDEO_FIT_COVER,
                                                          .rotation = XIAOTAI_VIDEO_ROTATE_90});
    assert(!xiaotai_video_frame_acquire(NULL) && xiaotai_video_remote_generation() == 0);
    feed(2, true, false, 81);
    WAIT(status().downlink.converted == 1 && xiaotai_video_remote_generation() == 2);
    xiaotai_signal_give(&proceed);
    assert(!xiaotai_video_frame_acquire_for(1, NULL));
    assert(pin.generation == 1);
    assert(xiaotai_video_frame_acquire_for(2, &pin));
    assert(pin.generation == 2 && pin.sequence == 1 && pin.width == 96 && pin.height == 64 && pin.scale == 1);
    assert(pin.picture.width == 96 && pin.picture.height == 64 && pixel(&pin, 40, 40) == 0x5151);
    xiaotai_video_close(2);
    WAIT(!status().generation);
    assert(!xiaotai_video_frame_acquire_for(2, NULL) && pixel(&pin, 40, 40) == 0x5151);
    assert(!xiaotai_video_remote_generation());
    teardown();
}

static void buffered_jpeg(bool enabled)
{
    setup(enabled, false);
    assert(!fake_task_named("video_convert"));
    open_session(3, &jpeg, NULL);
    feed(3, true, false, 40);
    WAIT(status().downlink.converted == 1);
    (void)acquire();
    advance(700000);
    feed(3, true, false, 51);
    WAIT(status().downlink.converted == 2);
    feed(3, true, false, 61);
    WAIT(status().downlink.converted == 3 && status().downlink.buffering == enabled);
    xiaotai_video_frame_t next = acquire();
    assert(pixel(&next, 50, 30) == (enabled ? 0x3333 : 0x3d3d));
    if (enabled) {
        assert(!xiaotai_video_frame_acquire(NULL));
        advance(7999999);
        assert(status().downlink.buffering);
        advance(1);
        WAIT(!status().downlink.buffering);
        next = acquire();
        assert(pixel(&next, 50, 30) == 0x3d3d);
    }
    assert(!status().downlink.key_requests);
    assert(atomic_load(&jpeg_transform_opens) == 1U && atomic_load(&jpeg_transform_updates) == 1U &&
           atomic_load(&jpeg_transform_runs) == 3U);
    teardown();
}

static void buffered_h264(void)
{
    setup(false, true);
    open_session(4, &h264, NULL);
    feed(4, false, true, 64);
    WAIT(status().downlink.converted == 1);
    const xiaotai_video_frame_t previous = acquire();
    const uint16_t initial = pixel(&previous, 8, 0);
    advance(700000);
    atomic_store(&block_convert, true);
    feed(4, false, false, 72);
    assert(xiaotai_signal_take(&entered, 3000));
    feed(4, false, false, 88);
    feed(4, false, false, 112);
    feed(4, false, false, 144);
    WAIT(status().downlink.decoded == 5);
    xiaotai_signal_give(&proceed);
    WAIT(status().downlink.converted == 5 && status().downlink.buffering);
    const xiaotai_video_frame_t first = acquire();
    const uint16_t earliest = pixel(&first, 8, 0);
    assert(earliest > initial && first.sequence == 2);
    advance(90909); /* Low-water 11 fps: round up, rather than rushing. */
    assert(!xiaotai_video_frame_acquire_for(4, NULL));
    advance(1);
    const xiaotai_video_frame_t second = acquire();
    assert(pixel(&second, 8, 0) > earliest && second.sequence == 3);
    teardown();
}

int main(void)
{
    convert_and_switch();
    buffered_h264();
    buffered_jpeg(false);
    buffered_jpeg(true);
    return 0;
}
