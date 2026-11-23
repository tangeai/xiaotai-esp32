// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_h264_feedback.py
#include <assert.h>
#include <string.h>
#include "downlink.h"
#include "fakes.h"
#include "h264_au.h"
#include "xiaotai/framework.h"

static const uint8_t sps[] = {0, 0, 1, 0x67, 66, 0x40, 31, 0x80};
static const uint8_t full[] = {0, 0, 1, 0x67, 66, 0x40, 31, 0x80,
                              0, 0, 1, 0x68, 0x80, 0, 0, 1, 0x65, 0x80};
static const uint8_t high[] = {0, 0, 0, 1, 0x67, 100, 0, 31, 0x80};
static const uint8_t p[] = {0, 0, 1, 0x61, 0x80};
static const uint8_t idr[] = {0, 0, 1, 0x65, 0x80};
static xiaotai_signal_t entered, proceed;
static atomic_bool hold, fail;
static atomic_bool intentional_buffering;
static atomic_uint opens, closes;
static atomic_uint unsupported_events, recovered_events;
static int64_t now = INT64_C(1000000);

/* A real receiver and workers, with the session clock/loop manually driven
 * so timeouts and generation changes neither race nor wait on wall time. */
void __wrap_video_session_task(void *arg);
void __wrap_video_session_task(void *arg)
{
    video_task_t *task = arg;
    while (!atomic_load(&task->stop)) (void)xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_give(&task->exited);
    xiaotai_task_exit();
}

/* W7 tests the decoder's leases directly; W8 covers the actual consumer. */
void __wrap_video_convert_task(void *arg);
void __wrap_video_convert_task(void *arg) { __wrap_video_session_task(arg); }
void __wrap_video_convert_kick(bool hardware);
void __wrap_video_convert_kick(bool hardware) { (void)hardware; }
bool __wrap_video_session_downlink_buffering(uint32_t revision);
bool __wrap_video_session_downlink_buffering(uint32_t revision) { return revision && atomic_load(&intentional_buffering); }

static int h264_match(const xiaotai_video_decoder_config_t *c) { return c->codec == XIAOTAI_VIDEO_CODEC_H264 ? 10 : 0; }
static int jpeg_match(const xiaotai_video_decoder_config_t *c) { return c->codec == XIAOTAI_VIDEO_CODEC_JPEG ? 10 : 0; }

static xiaotai_err_t backend_open(const xiaotai_video_decoder_config_t *c, void **state)
{
    static int h264, jpeg;
    if (c->codec == XIAOTAI_VIDEO_CODEC_JPEG) assert(!c->max_width && !c->max_height);
    *state = c->codec == XIAOTAI_VIDEO_CODEC_H264 ? &h264 : &jpeg;
    (void)atomic_fetch_add(&opens, 1U);
    return XIAOTAI_OK;
}

static void backend_close(void *state)
{
    assert(state);
    (void)atomic_fetch_add(&closes, 1U);
}

static xiaotai_err_t backend_decode(void *state, const xiaotai_video_packet_t *packet, uint8_t *out,
                                   size_t capacity, xiaotai_video_image_t *image)
{
    assert(state);
    if (atomic_exchange(&hold, false)) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&proceed, 3000));
    }
    if (atomic_exchange(&fail, false)) return XIAOTAI_FAIL;
    const bool h264 = packet->codec == XIAOTAI_VIDEO_CODEC_H264;
    uint16_t width = packet->width, height = packet->height;
    if (h264) {
        const video_h264_au_t au = video_h264_inspect(packet->data, packet->size);
        assert(au.valid);  /* Nothing malformed reaches the decoder. */
        if (!au.idr && au.sps) return XIAOTAI_ERR_NOT_FOUND;
        width = height = 32;
    }
    const xiaotai_video_pixel_t pixel = h264 ? XIAOTAI_VIDEO_PIXEL_I420 : XIAOTAI_VIDEO_PIXEL_RGB565;
    size_t bytes = xiaotai_video_image_bytes(pixel, width, height);
    if (!bytes || bytes > capacity) return XIAOTAI_ERR_INVALID_SIZE;
    memset(out, packet->data[packet->size - 1U], bytes);
    if (h264) ((uint8_t *)packet->data)[0] = 0xFF; /* The worker owns its copy, not the producer's frame. */
    *image = (xiaotai_video_image_t){.pixel = pixel, .width = width, .height = height, .rows = height,
        .data = out, .size = bytes};
    return XIAOTAI_OK;
}

static xiaotai_err_t backend_decode_jpeg(void *state, const xiaotai_video_decoder_config_t *c,
                                        const xiaotai_video_packet_t *packet, uint8_t *out,
                                        size_t capacity, xiaotai_video_image_t *image)
{
    assert(c->codec == XIAOTAI_VIDEO_CODEC_JPEG && c->max_width == 64 && c->max_height == 48);
    return backend_decode(state, packet, out, capacity, image);
}

static const xiaotai_video_decoder_t h264_decoder = {.name = "w7-h264", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
    .flags = XIAOTAI_VIDEO_DECODER_BASELINE_ONLY | XIAOTAI_VIDEO_DECODER_MUTATES_INPUT, .output = XIAOTAI_VIDEO_PIXEL_I420,
    .match = h264_match, .open = backend_open, .decode = backend_decode, .close = backend_close};
static const xiaotai_video_decoder_t jpeg_decoder = {.name = "w7-jpeg", .load = XIAOTAI_VIDEO_LOAD_HARDWARE,
    .flags = XIAOTAI_VIDEO_DECODER_PERSISTENT, .output = XIAOTAI_VIDEO_PIXEL_RGB565, .output_alignment = 16,
    .match = jpeg_match, .open = backend_open, .close = backend_close, .decode_configured = backend_decode_jpeg};

static const xiaotai_video_downlink_t h264_desc = {.codec = XIAOTAI_VIDEO_CODEC_H264, .max_width = 32,
    .max_height = 32, .fps = 12, .max_frame_bytes = 32, .burst_ms = 2000, .key_refresh_ms = 1000,
    .recover_h264_profile = true, .preserve_h264_parameter_chain = true};
static const xiaotai_video_downlink_t jpeg_desc = {.codec = XIAOTAI_VIDEO_CODEC_JPEG, .max_width = 64,
    .max_height = 48, .max_frame_bytes = 32};

static xiaotai_video_status_t status(void)
{
    xiaotai_video_status_t value;
    xiaotai_video_status(&value);
    return value;
}

#define WAIT_UNTIL(ok) do { \
    for (unsigned retries = 0; !(ok) && retries < 2000U; ++retries) xiaotai_delay_ms(1); \
    assert(ok); \
} while (0)

static void tick(unsigned ms)
{
    now += ms * INT64_C(1000);
    fake_clock_hold(now);
    (void)video_session_step(now);
}

static void request(uint32_t generation, const xiaotai_video_downlink_t *description)
{
    assert(xiaotai_video_open(&(const xiaotai_video_session_t){.generation = generation, .downlink = description,
        .link = fake_link()}) == XIAOTAI_OK);
    tick(0);
}

static bool push(uint32_t generation, const uint8_t *data, size_t bytes)
{
    return xiaotai_video_push(generation, &(const xiaotai_video_packet_t){.codec = XIAOTAI_VIDEO_CODEC_H264,
        .data = data, .size = bytes, .timestamp_ms = 10});
}

#define PUSH(generation, array) push(generation, array, sizeof(array))

static bool jpeg(uint32_t generation, uint16_t w, uint16_t h, uint8_t sample)
{
    return xiaotai_video_push(generation, &(const xiaotai_video_packet_t){.codec = XIAOTAI_VIDEO_CODEC_JPEG,
        .width = w, .height = h, .data = &sample, .size = 1, .timestamp_ms = sample});
}

static void event_seen(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_video_event_t *payload = event->data;
    assert(event->size == sizeof(*payload));
    if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_UNSUPPORTED) == 0) {
        assert(payload->generation == 1 || payload->generation == 2 || payload->generation == 6);
        assert(payload->error == XIAOTAI_ERR_NOT_SUPPORTED);
        (void)atomic_fetch_add(&unsupported_events, 1U);
    } else {
        assert(payload->generation == 1 && payload->error == XIAOTAI_OK);
        (void)atomic_fetch_add(&recovered_events, 1U);
    }
}

static xiaotai_err_t subscribe(void)
{
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_UNSUPPORTED, event_seen, NULL) == XIAOTAI_OK);
    return xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED, event_seen, NULL);
}

static void setup(void)
{
    fake_backends_reset();
    fake_clock_hold(now);
    static const xiaotai_module_t observer = {.name = "w7-observer", .init = subscribe};
    static const xiaotai_module_t *const modules[] = {&observer};
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 1}) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&entered) == XIAOTAI_OK && xiaotai_signal_init(&proceed) == XIAOTAI_OK);
    assert(xiaotai_engine_video_init() == XIAOTAI_OK);
    assert(xiaotai_video_decoder_register(&h264_decoder) == XIAOTAI_OK);
    assert(xiaotai_video_decoder_register(&jpeg_decoder) == XIAOTAI_OK);
    assert(fake_display_add(96, 64, XIAOTAI_PIXEL_RGB565, 0));
    xiaotai_video_viewport_t viewport;
    assert(xiaotai_video_viewport_for_display(0, &viewport) == XIAOTAI_OK);
    assert(xiaotai_video_require_viewport(&viewport) == XIAOTAI_OK);
    assert(xiaotai_video_require_downlink(&h264_desc) == XIAOTAI_OK);
    assert(xiaotai_video_require_downlink(&jpeg_desc) == XIAOTAI_OK);
    xiaotai_engine_video_start();
    assert(video_started() && video_engine.downlink_available && video_session_has_receiver());
}

static void wait_frame(video_downlink_frame_t *frame)
{
    for (unsigned retries = 0; retries < 2000U; ++retries) {
        if (video_downlink_take_decoded(frame)) return;
        xiaotai_delay_ms(1);
    }
    assert(false);
}

static void format_recovery(void)
{
    request(1, &h264_desc);
    assert(status().generation == 1 && status().downlink.waiting_key_frame);
    assert(status().downlink.key_requests == 1);
    assert(!PUSH(2, full) && !push(1, full, 0));
    assert(status().downlink.stale == 1 && status().downlink.rejected == 1);
    assert(PUSH(1, idr));
    WAIT_UNTIL(status().downlink.discarded == 1);
    const uint8_t broken[] = {0, 0, 1, 0x67, 66};
    assert(PUSH(1, broken));
    WAIT_UNTIL(status().downlink.rejected == 2);
    assert(!status().downlink.unsupported && status().downlink.key_requests == 1);
    assert(PUSH(1, high));
    WAIT_UNTIL(status().downlink.unsupported);
    assert(PUSH(1, high));
    WAIT_UNTIL(status().downlink.rejected == 4);
    const uint8_t main_profile[] = {0, 0, 1, 0x67, 77, 0x40, 31, 0x80};
    const uint8_t baseline[] = {0, 0, 1, 0x67, 66, 0, 31, 0x80};
    assert(PUSH(1, main_profile));
    WAIT_UNTIL(status().downlink.rejected == 5);
    assert(PUSH(1, baseline));
    WAIT_UNTIL(status().downlink.rejected == 6);
    assert(!PUSH(2, sps) && status().downlink.stale == 2);
    tick(2000);
    assert(status().downlink.unsupported && !status().downlink.decoded && !atomic_load(&recovered_events));
    assert(status().downlink.key_requests == 1 && fake_link_state.key_requests == 1);
    WAIT_UNTIL(atomic_load(&unsupported_events) == 1);
    assert(PUSH(1, sps));
    WAIT_UNTIL(!status().downlink.unsupported && status().downlink.waiting_key_frame);
    tick(0);
    assert(status().generation == 1 && status().downlink.key_requests == 2);
    WAIT_UNTIL(atomic_load(&recovered_events) == 1);
    assert(PUSH(1, idr));
    WAIT_UNTIL(status().downlink.discarded == 2);
    tick(999);
    assert(status().downlink.key_requests == 2 && fake_link_state.key_requests == 2);
    tick(1);
    assert(status().downlink.key_requests == 3 && fake_link_state.key_requests == 3);
    assert(!status().downlink.unsupported);
    assert(PUSH(1, full));
    WAIT_UNTIL(status().downlink.decoded == 1);
    video_downlink_frame_t frame;
    wait_frame(&frame);
    assert(frame.revision && frame.timestamp_ms == 10 && frame.key_frame && frame.image.data[0] == 0x80);
    const unsigned opened = atomic_load(&opens);
    atomic_store(&fail, true);
    assert(PUSH(1, p));
    WAIT_UNTIL(status().downlink.discontinuities == 1 && status().downlink.waiting_key_frame);
    assert(status().downlink.decode_failed == 1 && atomic_load(&closes));
    assert(PUSH(1, idr));
    WAIT_UNTIL(status().downlink.discarded == 3);
    uint8_t changed[sizeof(full)];
    memcpy(changed, full, sizeof(changed));
    changed[sizeof(changed) - 1] = 0xc0;
    assert(PUSH(1, changed));
    memset(changed, 0xa5, sizeof(changed));
    WAIT_UNTIL(status().downlink.decoded == 2);
    assert(atomic_load(&opens) == opened + 1);
    assert(frame.image.data[0] == 0x80);  /* Never reuse a leased output. */
    video_downlink_frame_t stale = frame;
    video_downlink_release_decoded(&frame);
    wait_frame(&frame);
    assert(frame.image.data[0] == 0xc0);
    video_downlink_release_decoded(&frame);
    assert(PUSH(1, p));
    wait_frame(&frame);
    assert(frame.slot == stale.slot && frame.lease_cookie != stale.lease_cookie);
    video_downlink_release_decoded(&stale);
    const uint8_t other[] = {0, 0, 1, 0x61, 0x81};
    assert(PUSH(1, other));
    WAIT_UNTIL(status().downlink.decoded == 4);
    assert(frame.image.data[0] == 0x80);
    video_downlink_release_decoded(&frame);

    assert(PUSH(1, high));
    WAIT_UNTIL(status().downlink.unsupported);
    tick(0);
    WAIT_UNTIL(atomic_load(&unsupported_events) == 2);
    assert(atomic_load(&recovered_events) == 1);

    xiaotai_video_downlink_t strict = h264_desc;
    strict.recover_h264_profile = false;
    request(2, &strict);
    assert(status().generation == 2 && PUSH(2, high));
    WAIT_UNTIL(status().downlink.unsupported);
    assert(!PUSH(2, sps));
    tick(1500);
    assert(status().generation == 2 && status().downlink.unsupported && status().downlink.key_requests == 1);
    WAIT_UNTIL(atomic_load(&unsupported_events) == 3);
    assert(atomic_load(&recovered_events) == 1);
}

static void saturation_and_stall(void)
{
    request(3, &h264_desc);
    assert(status().generation == 3);
    atomic_store(&hold, true);
    assert(PUSH(3, full) && xiaotai_signal_take(&entered, 3000));
    fake_port_clear();
    for (unsigned i = 0; i < 23; ++i) assert(PUSH(3, p));
    assert(!PUSH(3, p) && status().downlink.overflow == 1);
    assert(!fake_mem_allocations(XIAOTAI_MEM_BULK) && !fake_mem_allocations(XIAOTAI_MEM_INTERNAL) &&
           !fake_mem_allocations(XIAOTAI_MEM_DMA));
    tick(100);
    xiaotai_signal_give(&proceed);
    WAIT_UNTIL(status().downlink.discarded == 23 && status().downlink.waiting_key_frame);
    assert(status().downlink.discontinuities == 1);
    assert(PUSH(3, full));
    WAIT_UNTIL(status().downlink.decoded == 2);

    atomic_store(&hold, true);
    assert(PUSH(3, p) && xiaotai_signal_take(&entered, 3000));
    tick(1999);
    assert(!status().downlink.quarantined);
    tick(1);
    assert(status().downlink.quarantined && !PUSH(3, p));
    xiaotai_signal_give(&proceed);
    WAIT_UNTIL(!status().downlink.quarantined && status().downlink.decode_failed == 1);
    assert(status().downlink.waiting_key_frame && status().downlink.discontinuities == 2);
    assert(PUSH(3, full));
    WAIT_UNTIL(status().downlink.decoded == 3);
}

static void latency_recovery(void)
{
    request(4, &h264_desc);
    atomic_store(&hold, true);
    assert(PUSH(4, full) && xiaotai_signal_take(&entered, 3000));
    for (unsigned i = 0; i < 22; ++i) assert(PUSH(4, p));
    tick(1100);  /* 12 frames of queue age, but not a 2 s decoder stall. */
    xiaotai_signal_give(&proceed);
    WAIT_UNTIL(status().downlink.discarded == 15U);
    assert(!status().downlink.overflow && status().downlink.discontinuities == 1 && status().downlink.waiting_key_frame);
    assert(PUSH(4, full));
    WAIT_UNTIL(!status().downlink.waiting_key_frame && status().downlink.decoded == 9);

    /* W8's intentional playout buffering must not trigger W7 latency cuts. */
    atomic_store(&intentional_buffering, true);
    atomic_store(&hold, true);
    assert(PUSH(4, p) && xiaotai_signal_take(&entered, 3000));
    for (unsigned i = 0; i < 22; ++i) assert(PUSH(4, p));
    tick(1100);
    xiaotai_signal_give(&proceed);
    WAIT_UNTIL(status().downlink.decoded == 32);
    assert(status().downlink.discontinuities == 1 && status().downlink.discarded == 15U);
    atomic_store(&intentional_buffering, false);
}

static void jpeg_and_switch(void)
{
    request(5, &jpeg_desc);
    assert(status().generation == 5);
    assert(jpeg(5, 48, 64, 4));
    video_downlink_frame_t frame;
    wait_frame(&frame);
    assert(frame.image.width == 48 && frame.image.height == 64 && frame.image.data[0] == 4);
    assert((uintptr_t)frame.image.data % 16 == 0);
    video_downlink_release_decoded(&frame);
    assert(jpeg(5, 32, 72, 3)); /* Fits in bytes, exceeds the long-side limit. */
    WAIT_UNTIL(status().downlink.rejected == 1);
    assert(jpeg(5, 8, 32, 9)); /* JPEG has no H.264 minimum edge. */
    wait_frame(&frame);
    assert(frame.image.width == 8 && frame.image.height == 32 && frame.image.data[0] == 9);
    video_downlink_release_decoded(&frame);
    tick(3000);
    assert(!status().downlink.key_requests && !status().downlink.waiting_key_frame);

    atomic_store(&hold, true);
    assert(jpeg(5, 48, 64, 5) && xiaotai_signal_take(&entered, 3000));
    for (uint8_t i = 7; i < 34; ++i) assert(jpeg(5, 48, 64, i));
    xiaotai_signal_give(&proceed);
    WAIT_UNTIL(status().downlink.decoded == 4);
    assert(status().downlink.discarded == 26 && !status().downlink.key_requests && !status().downlink.overflow);
    wait_frame(&frame);
    assert(frame.timestamp_ms == 33 && frame.image.data[0] == 33);
    video_downlink_release_decoded(&frame);

    atomic_store(&hold, true);
    assert(jpeg(5, 48, 64, 6) && xiaotai_signal_take(&entered, 3000));
    request(6, &h264_desc);
    assert(status().requested == 6 && status().generation == 0);
    tick(2500);
    assert(status().generation == 0 && !PUSH(6, full));
    xiaotai_signal_give(&proceed);
    for (unsigned i = 0; i < 1000U && status().generation != 6; ++i) { xiaotai_delay_ms(1); tick(500); }
    assert(status().generation == 6 && !video_downlink_take_decoded(&frame));
    assert(PUSH(6, full));
    wait_frame(&frame);
    assert(frame.key_frame && frame.image.data[0] == 0x80);
    video_downlink_release_decoded(&frame);

    assert(PUSH(6, high));
    WAIT_UNTIL(status().downlink.unsupported);
    tick(0);
    WAIT_UNTIL(atomic_load(&unsupported_events) == 4);
    const unsigned key_requests = fake_link_state.key_requests;
    request(7, &jpeg_desc);
    assert(xiaotai_video_push(7, &(const xiaotai_video_packet_t){.codec = XIAOTAI_VIDEO_CODEC_JPEG,
        .width = 48, .height = 64, .data = high, .size = sizeof(high), .timestamp_ms = 7}));
    wait_frame(&frame);
    assert(frame.image.width == 48 && frame.image.height == 64 && frame.image.data[0] == 0x80);
    video_downlink_release_decoded(&frame);
    tick(2000);
    assert(status().generation == 7 && !status().downlink.unsupported && !status().downlink.waiting_key_frame);
    assert(!status().downlink.key_requests && fake_link_state.key_requests == key_requests);
    assert(atomic_load(&unsupported_events) == 4 && atomic_load(&recovered_events) == 1);
}

int main(void)
{
    setup();
    format_recovery();
    saturation_and_stall();
    latency_recovery();
    jpeg_and_switch();
    xiaotai_video_reset();
    assert(!video_started() && atomic_load(&closes) == atomic_load(&opens));
    xiaotai_signal_deinit(&entered);
    xiaotai_signal_deinit(&proceed);
    return 0;
}
