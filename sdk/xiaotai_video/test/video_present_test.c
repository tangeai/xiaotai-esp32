// SPDX-License-Identifier: MIT
#include <assert.h>
#include <string.h>

#include "fakes.h"
#include "present.h"

static const xiaotai_video_downlink_t h264 = {.codec = XIAOTAI_VIDEO_CODEC_H264, .max_width = 384,
    .max_height = 256, .fps = 12, .max_frame_bytes = 4096, .burst_ms = 2000};
static const xiaotai_video_downlink_t jpeg = {.codec = XIAOTAI_VIDEO_CODEC_JPEG, .max_width = 1024,
    .max_height = 608, .max_frame_bytes = 4096};

static void setup(uint16_t width, uint16_t height, uint8_t frames, bool independent)
{
    fake_hal_reset();
    fake_backends_reset();
    assert(xiaotai_engine_video_init() == XIAOTAI_OK);
    const xiaotai_video_config_t config = {.buffer_independent_frames = independent};
    assert(xiaotai_video_configure(&config) == XIAOTAI_OK);
    assert(fake_display_add(width, height, XIAOTAI_PIXEL_RGB565, 0));
    const xiaotai_video_viewport_t viewport = {.width = width, .height = height,
        .pixel = XIAOTAI_VIDEO_PIXEL_RGB565, .frames = frames};
    assert(xiaotai_video_require_viewport(&viewport) == XIAOTAI_OK);
    fake_decoder_ctl[0].codec = XIAOTAI_VIDEO_CODEC_H264;
    fake_decoder_ctl[0].score = 5;
    fake_decoder_ctl[1].codec = XIAOTAI_VIDEO_CODEC_JPEG;
    fake_decoder_ctl[1].score = 5;
    assert(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_OK);
    assert(xiaotai_video_decoder_register(&fake_decoders[1]) == XIAOTAI_OK);
    assert(xiaotai_video_require_downlink(&h264) == XIAOTAI_OK);
    assert(xiaotai_video_require_downlink(&jpeg) == XIAOTAI_OK);
}

static void teardown(void)
{
    assert(video_present_reset());
    xiaotai_video_reset();
    fake_clock_release();
}

static void begin_display(uint32_t revision, uint32_t generation, uint8_t display,
                          const xiaotai_video_downlink_t *downlink)
{
    assert(video_present_begin(&(const video_session_config_t){.revision = revision, .generation = generation,
        .has_downlink = true, .downlink = *downlink, .display = display}) == XIAOTAI_OK);
}

static void begin(uint32_t revision, uint32_t generation, const xiaotai_video_downlink_t *downlink)
{
    begin_display(revision, generation, 0, downlink);
}

static xiaotai_video_frame_t describe(const video_present_write_t *write, uint32_t generation,
                                      uint16_t width, uint16_t height, uint8_t scale)
{
    return (xiaotai_video_frame_t){.generation = generation, .pixel = XIAOTAI_VIDEO_PIXEL_RGB565,
        .width = width, .height = height, .scale = scale, .picture = {0, 0, width, height},
        .pixels = write->pixels, .size = (size_t)width * height * 2U};
}

static void ready(uint32_t revision, uint32_t generation, uint8_t marker)
{
    video_present_write_t write;
    assert(video_present_claim(revision, 32U * 32U * 2U, &write));
    const xiaotai_video_frame_t frame = describe(&write, generation, 32, 32, 1);
    memset(write.pixels, marker, frame.size);
    assert(video_present_publish(&write, &frame));
    assert(!write.cookie);
}

static void sizing(bool independent)
{
    setup(1024, 600, 0, independent);
    /* Both default frame counts must be represented even with a later plain
     * declaration for the same (otherwise identical) viewport. */
    const xiaotai_video_viewport_t explicit = {.width = 1024, .height = 600,
        .pixel = XIAOTAI_VIDEO_PIXEL_RGB565, .frames = 3};
    assert(xiaotai_video_require_viewport(&explicit) == XIAOTAI_OK);
    assert(fake_display_add(640, 480, XIAOTAI_PIXEL_RGB888, 0));
    const xiaotai_video_viewport_t other = {.display = 1, .width = 640, .height = 480,
        .pixel = XIAOTAI_VIDEO_PIXEL_RGB888, .frames = 5};
    assert(xiaotai_video_require_viewport(&other) == XIAOTAI_OK);
    fake_port_clear();
    assert(video_present_prepare() == XIAOTAI_OK);
    assert(fake_mem_allocations(XIAOTAI_MEM_BULK) == 21);
    assert(fake_mem_allocations(XIAOTAI_MEM_INTERNAL) == 0);
    assert(video_present_prepare() == XIAOTAI_OK && fake_mem_allocations(XIAOTAI_MEM_BULK) == 21);
    assert(video_present_render_scale(&explicit, &h264) == 2);
    assert(video_present_render_scale(&explicit, &jpeg) == 1);
    begin(1, 71, &h264);
    const size_t narrow = 512U * 300U * 2U, medium = 640U * 480U * 3U, wide = 1024U * 600U * 2U;
    video_present_write_t writes[20];
    for (unsigned i = 0; i < 20U; ++i) {
        assert(video_present_claim(1, narrow, &writes[i]));
        assert((uintptr_t)writes[i].pixels % 64U == 0);
        const bool wide_slot = independent || writes[i].slot < 3U;
        assert(writes[i].capacity == (wide_slot ? wide : (writes[i].slot < 5U ? medium : narrow)));
    }
    video_present_write_t extra;
    assert(!video_present_claim(1, narrow, &extra));
    for (unsigned i = 0; i < 20U; ++i) video_present_cancel(&writes[i]);
    video_present_stop(1);

    begin(2, 71, &jpeg);
    const unsigned expected = independent ? 20U : 3U;
    for (unsigned i = 0; i < expected; ++i) assert(video_present_claim(2, wide, &writes[i]));
    assert(!video_present_claim(2, wide, &extra));
    for (unsigned i = 0; i < expected; ++i) video_present_cancel(&writes[i]);
    video_present_stop(2);

    begin_display(3, 71, 1, &jpeg);
    for (unsigned i = 0; i < 5U; ++i) assert(video_present_claim(3, medium, &writes[i]));
    assert(!video_present_claim(3, medium, &extra));
    assert(!video_present_reset()); /* Strict lifetime: a worker lease still owns each range. */
    for (unsigned i = 0; i < 5U; ++i) video_present_cancel(&writes[i]);
    teardown();
}

static void pin_and_eviction(void)
{
    setup(32, 32, 3, false);
    assert(video_present_prepare() == XIAOTAI_OK);
    begin(1, 42, &jpeg);
    fake_clock_hold(INT64_C(1000000));
    ready(1, 42, 1);
    xiaotai_video_frame_t captured;
    assert(xiaotai_video_frame_acquire(&captured) && captured.sequence == 1U);
    const uint8_t *first = captured.pixels;
    const size_t first_bytes = captured.size;
    ready(1, 42, 2);
    ready(1, 42, 3);
    video_present_write_t overwrite;
    assert(video_present_claim(1, first_bytes, &overwrite));
    assert(overwrite.pixels != first && overwrite.pixels[0] == 2U);
    const xiaotai_video_frame_t updated = describe(&overwrite, 42, 32, 32, 1);
    memset(overwrite.pixels, 4, updated.size);
    assert(video_present_publish(&overwrite, &updated));
    for (size_t i = 0; i < first_bytes; ++i) assert(first[i] == 1U);
    assert(xiaotai_video_frame_acquire(&captured) && captured.sequence == 2U);
    const uint8_t *retained = captured.pixels;
    assert(retained[0] == 4U && !xiaotai_video_frame_acquire(NULL));

    video_present_write_t pending;
    assert(video_present_claim(1, 2048, &pending));
    video_present_write_t stale = pending;
    video_present_flush(1);
    const xiaotai_video_frame_t invalid = describe(&pending, 42, 32, 32, 1);
    assert(!video_present_publish(&pending, &invalid));
    ready(1, 42, 5);
    video_present_stop(1);
    assert(!xiaotai_video_frame_acquire(NULL));
    assert(!video_present_reset()); /* Public frame outlives the session. */
    uint8_t target[] = {0, 0};
    const xiaotai_video_surface_t surface = {.pixels = target, .width = 1, .height = 1,
        .stride = 2, .pixel = XIAOTAI_VIDEO_PIXEL_RGB565};
    const xiaotai_video_area_t area = {0, 0, 32, 32};
    assert(xiaotai_video_frame_paint(&surface, &area) && target[0] == 4U && target[1] == 4U);

    /* Reuse a transport generation with a new revision: leaked write handles
     * and old stop notifications must not touch the new session's state. */
    begin(2, 42, &jpeg);
    video_present_stop(1);
    video_present_write_t fresh;
    assert(video_present_claim(2, 2048, &fresh));
    video_present_cancel(&stale);
    xiaotai_video_frame_t frame = describe(&fresh, 42, 32, 32, 1);
    memset(fresh.pixels, 9, frame.size);
    assert(video_present_publish(&fresh, &frame));
    assert(xiaotai_video_frame_acquire(&captured) && captured.sequence == 1U && captured.generation == 42U);
    assert(((const uint8_t *)captured.pixels)[0] == 9U);
    xiaotai_video_frame_release();
    xiaotai_video_frame_release();
    assert(!xiaotai_video_frame_paint(&surface, &area));
    teardown();
}

static void playout(bool independent)
{
    setup(32, 32, 7, independent);
    assert(video_present_prepare() == XIAOTAI_OK);
    begin(3, 72, &jpeg);
    const int64_t at = INT64_C(2000000), start = at + 6 * INT64_C(83334);
    video_present_arrival(3, at);
    video_present_arrival(3, start);
    fake_clock_hold(start);
    ready(3, 72, 21);
    ready(3, 72, 22);
    xiaotai_video_frame_t frame;
    assert(video_present_buffering(3, start) == independent);
    if (!independent) {
        assert(xiaotai_video_frame_acquire(&frame) && ((const uint8_t *)frame.pixels)[0] == 22U);
    } else {
        assert(!xiaotai_video_frame_acquire(NULL)); /* Not enough resident frames for initial playout. */
        for (uint8_t marker = 23; marker <= 26; ++marker) ready(3, 72, marker);
        assert(xiaotai_video_frame_acquire(&frame) && ((const uint8_t *)frame.pixels)[0] == 21U);
        assert(!xiaotai_video_frame_acquire(NULL));
        fake_clock_hold(start + INT64_C(83334));
        assert(xiaotai_video_frame_acquire(&frame) && ((const uint8_t *)frame.pixels)[0] == 22U);
        fake_clock_hold(start + INT64_C(7999999));
        assert(video_present_buffering(3, xiaotai_time_us()));
        fake_clock_hold(start + INT64_C(8000000));
        assert(!video_present_buffering(3, xiaotai_time_us()));
        assert(xiaotai_video_frame_acquire(&frame) && ((const uint8_t *)frame.pixels)[0] == 26U);
    }
    xiaotai_video_frame_release();
    teardown();
}

int main(void)
{
    sizing(false);
    sizing(true);
    pin_and_eviction();
    playout(false);
    playout(true);
    return 0;
}
