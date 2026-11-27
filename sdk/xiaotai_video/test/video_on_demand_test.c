// SPDX-License-Identifier: MIT
/* Memory of the downlink taken when a session begins and given back after. */
#include <assert.h>
#include <string.h>
#include "downlink.h"
#include "fakes.h"
#include "xiaotai/framework.h"
#include "xiaotai_mem_host.h"

enum { WIDTH = 32, HEIGHT = 32 };

static const uint8_t full[] = {0, 0, 1, 0x67, 66, 0x40, 31, 0x80,
                              0, 0, 1, 0x68, 0x80, 0, 0, 1, 0x65, 0x80};
static const uint8_t p[] = {0, 0, 1, 0x61, 0x80};
static int64_t now = INT64_C(1000000);

void __wrap_video_session_task(void *arg);
void __wrap_video_session_task(void *arg)
{
    video_task_t *task = arg;
    while (!atomic_load(&task->stop)) (void)xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_give(&task->exited);
    xiaotai_task_exit();
}

void __wrap_video_convert_task(void *arg);
void __wrap_video_convert_task(void *arg) { __wrap_video_session_task(arg); }
void __wrap_video_convert_kick(bool hardware);
void __wrap_video_convert_kick(bool hardware) { (void)hardware; }
bool __wrap_video_session_downlink_buffering(uint32_t revision);
bool __wrap_video_session_downlink_buffering(uint32_t revision) { (void)revision; return false; }

static int match(const xiaotai_video_decoder_config_t *c)
{
    (void)c;
    return 10;
}

static atomic_bool starving;
static atomic_uint opens;
static xiaotai_mem_stats_t stats;
void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *out);
void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *out)
{
    assert(kind == XIAOTAI_MEM_BULK);
    *out = stats;
}

static xiaotai_err_t open(const xiaotai_video_decoder_config_t *c, void **state)
{
    static int decoder;
    (void)c;
    atomic_fetch_add(&opens, 1U);
    *state = &decoder;
    return XIAOTAI_OK;
}

static void close(void *state) { assert(state); }

static xiaotai_err_t decode(void *state, const xiaotai_video_packet_t *packet, uint8_t *out, size_t capacity,
                           xiaotai_video_image_t *image)
{
    assert(state && packet->size <= 24U);
    if (atomic_load(&starving)) return XIAOTAI_ERR_NO_MEM;
    const size_t bytes = xiaotai_video_image_bytes(XIAOTAI_VIDEO_PIXEL_I420, WIDTH, HEIGHT);
    assert(bytes <= capacity);
    memset(out, packet->data[packet->size - 1U], bytes);
    *image = (xiaotai_video_image_t){.pixel = XIAOTAI_VIDEO_PIXEL_I420, .width = WIDTH, .height = HEIGHT, .rows = HEIGHT,
        .data = out, .size = bytes};
    return XIAOTAI_OK;
}

static const xiaotai_video_decoder_t backend = {.name = "demand", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
    .output = XIAOTAI_VIDEO_PIXEL_I420, .match = match, .open = open, .decode = decode, .close = close};
static const xiaotai_video_downlink_t desc = {.codec = XIAOTAI_VIDEO_CODEC_H264, .max_width = WIDTH, .max_height = HEIGHT,
    .fps = 12, .max_frame_bytes = 4096, .burst_ms = 2000, .key_refresh_ms = 1000};

static void tick(unsigned ms)
{
    now += ms * INT64_C(1000);
    fake_clock_hold(now);
    (void)video_session_step(now);
}

static bool push(uint32_t generation, const uint8_t *data, size_t bytes)
{
    return xiaotai_video_push(generation, &(const xiaotai_video_packet_t){.codec = XIAOTAI_VIDEO_CODEC_H264,
        .data = data, .size = bytes, .timestamp_ms = 10});
}

static void request(uint32_t generation)
{
    assert(xiaotai_video_open(&(const xiaotai_video_session_t){.generation = generation, .downlink = &desc,
        .link = fake_link()}) == XIAOTAI_OK);
    tick(0);
}

static xiaotai_video_status_t status(void)
{
    xiaotai_video_status_t value;
    xiaotai_video_status(&value);
    return value;
}

static void frame_decoded(video_downlink_frame_t *frame)
{
    for (unsigned tries = 0; tries < 2000U; ++tries) {
        if (video_downlink_take_decoded(frame)) return;
        xiaotai_delay_ms(1);
    }
    assert(false);
}

int main(void)
{
    fake_backends_reset();
    fake_clock_hold(now);
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){0}) == XIAOTAI_OK);
    xiaotai_video_config_t config = {.downlink_packets = 3};
    assert(xiaotai_video_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    config.downlink_packets = 4;
    config.downlink_pictures = 1;
    assert(xiaotai_video_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    config = (xiaotai_video_config_t){.downlink_on_demand = true, .downlink_packets = 4, .downlink_packet_bytes = 24,
                                      .downlink_pictures = 2};
    assert(xiaotai_video_configure(&config) == XIAOTAI_OK);
    assert(xiaotai_engine_video_init() == XIAOTAI_OK);
    assert(xiaotai_video_decoder_register(&backend) == XIAOTAI_OK);
    assert(fake_display_add(96, 64, XIAOTAI_PIXEL_RGB565, 0));
    xiaotai_video_viewport_t viewport;
    assert(xiaotai_video_viewport_for_display(0, &viewport) == XIAOTAI_OK);
    assert(xiaotai_video_require_viewport(&viewport) == XIAOTAI_OK);
    assert(xiaotai_video_require_downlink(&desc) == XIAOTAI_OK);
    xiaotai_engine_video_start();
    assert(video_engine.downlink_available && video_session_has_receiver());
    const int64_t idle = xiaotai_mem_host_outstanding();

    /* Four packets, two decoder outputs, two pictures; no more. */
    fake_port_clear();
    request(1);
    assert(status().generation == 1 && fake_mem_allocations(XIAOTAI_MEM_BULK) == 8U);
    assert(push(1, full, sizeof(full)));
    video_downlink_frame_t frame;
    frame_decoded(&frame);
    assert(frame.image.data[0] == 0x80);

    /* A frame over the limit breaks the chain: the next ones wait for a key frame. */
    uint8_t large[25] = {0, 0, 1, 0x61, 0x80};
    assert(!push(1, large, sizeof(large)));
    assert(push(1, p, sizeof(p)));
    for (unsigned i = 0; i < 2000U && !status().downlink.waiting_key_frame; ++i) xiaotai_delay_ms(1);
    assert(status().downlink.waiting_key_frame && status().downlink.discontinuities == 1);

    /* The lent picture keeps everything; its end gives it all back. */
    xiaotai_video_close(1);
    tick(0);
    assert(status().generation == 0 && xiaotai_mem_host_outstanding() > idle && frame.image.data[0] == 0x80);
    tick(20);
    assert(xiaotai_mem_host_outstanding() > idle);
    video_downlink_release_decoded(&frame);
    for (unsigned i = 0; i < 2000U && xiaotai_mem_host_outstanding() != idle; ++i) { xiaotai_delay_ms(1); tick(20); }
    assert(xiaotai_mem_host_outstanding() == idle);

    /* A picture on the display keeps its memory until the display lets it go. */
    request(2);
    assert(push(2, full, sizeof(full)));
    frame_decoded(&frame);
    const uint32_t revision = frame.revision;
    video_downlink_release_decoded(&frame);
    video_present_write_t write;
    const size_t bytes = 48U * 32U * 2U;
    assert(video_present_claim(revision, bytes, &write));
    const xiaotai_video_frame_t shown = {.generation = 2, .pixel = XIAOTAI_VIDEO_PIXEL_RGB565, .width = 48, .height = 32,
        .scale = 2, .picture = {0, 0, 48, 32}, .pixels = write.pixels, .size = bytes};
    memset(write.pixels, 0x5a, bytes);
    assert(video_present_publish(&write, &shown));
    xiaotai_video_frame_t pinned;
    assert(xiaotai_video_frame_acquire(&pinned));
    /* A finished picture waits for the display in the other slot; what follows
     * finds one again when it was shown. The shown one stays meanwhile. */
    for (uint8_t marker = 1; marker < 4U; ++marker) {
        assert(video_present_writable() && video_present_claim(revision, bytes, &write) && write.pixels != pinned.pixels);
        xiaotai_video_frame_t later = shown;
        later.pixels = write.pixels;
        memset(write.pixels, marker, bytes);
        assert(!video_present_writable());
        assert(video_present_publish(&write, &later));
        video_present_write_t none;
        assert(!video_present_writable() && !video_present_claim(revision, bytes, &none));
        assert(((const uint8_t *)pinned.pixels)[0] == (marker == 1U ? 0x5a : marker - 1U));
        assert(xiaotai_video_frame_acquire(&pinned) && ((const uint8_t *)pinned.pixels)[0] == marker);
    }
    xiaotai_video_close(2);
    tick(0);
    for (unsigned i = 0; i < 100U; ++i) { xiaotai_delay_ms(1); tick(20); }
    assert(xiaotai_mem_host_outstanding() > idle && ((const uint8_t *)pinned.pixels)[0] == 3U);
    xiaotai_video_frame_release();
    tick(20);
    assert(xiaotai_mem_host_outstanding() == idle);

    /* No room: the session is tried again until there is. */
    xiaotai_mem_host_fail_nth(6);
    request(3);
    assert(status().requested == 3 && status().generation == 0);
    assert(xiaotai_mem_host_outstanding() == idle);     /* all or nothing */
    tick(400);
    assert(status().generation == 0);
    tick(100);
    assert(status().generation == 3 && push(3, full, sizeof(full)));
    frame_decoded(&frame);
    video_downlink_release_decoded(&frame);
    /* ... or it ends without: nothing is kept. */
    xiaotai_video_close(3);
    for (unsigned i = 0; i < 2000U && xiaotai_mem_host_outstanding() != idle; ++i) { xiaotai_delay_ms(1); tick(20); }
    xiaotai_mem_host_fail_nth(7);
    request(4);
    xiaotai_video_close(4);
    tick(0);
    assert(status().generation == 0 && xiaotai_mem_host_outstanding() == idle);

    /* Without enough beside it nothing is asked for, and the tries get slower. */
    stats = (xiaotai_mem_stats_t){.total = 1U << 20, .free = 64U * 1024U, .largest_block = 64U * 1024U};
    fake_port_clear();
    request(5);
    for (unsigned i = 0; i < 12U; ++i) tick(500);
    assert(!status().generation && !fake_mem_allocations(XIAOTAI_MEM_BULK));
    stats = (xiaotai_mem_stats_t){.total = 1U << 20, .free = 1U << 20, .largest_block = 1024U};
    tick(5000);
    assert(!status().generation && !fake_mem_allocations(XIAOTAI_MEM_BULK));     /* in pieces */
    stats.largest_block = 1U << 20;
    tick(4000);
    assert(!status().generation);
    tick(1000);
    assert(status().generation == 5);

    /* A decoder without memory: two decodes, then nothing is held for a while. */
    atomic_store(&starving, true);
    atomic_store(&opens, 0);
    for (unsigned i = 0; i < 2000U && status().generation; ++i) {
        (void)push(5, full, sizeof(full));
        xiaotai_delay_ms(1);
        tick(1);
    }
    assert(!status().generation && status().downlink.error == XIAOTAI_ERR_NO_MEM && status().downlink.decode_failed >= 2U);
    assert(atomic_load(&opens) == 2U);
    for (unsigned i = 0; i < 200U && xiaotai_mem_host_outstanding() != idle; ++i) { xiaotai_delay_ms(1); tick(10); }
    assert(xiaotai_mem_host_outstanding() == idle && !push(5, full, sizeof(full)));
    atomic_store(&starving, false);
    tick(10000);
    assert(status().generation == 5 && push(5, full, sizeof(full)));
    frame_decoded(&frame);
    video_downlink_release_decoded(&frame);
    xiaotai_video_close(5);
    for (unsigned i = 0; i < 2000U && xiaotai_mem_host_outstanding() != idle; ++i) { xiaotai_delay_ms(1); tick(20); }
    assert(xiaotai_mem_host_outstanding() == idle);

    xiaotai_video_reset();
    assert(!video_started());
    return 0;
}
