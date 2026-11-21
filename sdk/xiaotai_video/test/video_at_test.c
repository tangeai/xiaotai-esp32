// SPDX-License-Identifier: MIT
#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "fakes.h"
#include "internal.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/video_profiles.h"

static struct {
    const char *input;
    size_t left, used;
    char output[4096];
} wire;

static const uint8_t complete[] = {0,0,1,0x67,66,0x40,31,0x80, 0,0,1,0x68,0x80, 0,0,1,0x65,0x80};
static const uint8_t delta[] = {0,0,1,0x61,0x80};
static uint8_t raw[384U * 256U * 3U / 2U];
static atomic_uint credits, sends, keys;
static uint8_t *encoded;
static bool fixture_capture;

#define WAIT(condition) do { \
    for (unsigned i = 0; !(condition) && i < 2000U; ++i) xiaotai_delay_ms(1); \
    assert(condition); \
} while (0)

static size_t read_bytes(void *self, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)self;
    if (!wire.left) {
        fake_clock_advance_us((int64_t)timeout_ms * 1000);
        xiaotai_delay_ms(timeout_ms);
        return 0;
    }
    const size_t bytes = wire.left < capacity ? wire.left : capacity;
    memcpy(data, wire.input, bytes);
    wire.input += bytes;
    wire.left -= bytes;
    return bytes;
}

static xiaotai_err_t record(void *self, const char *text, size_t size)
{
    (void)self;
    assert(!xiaotai_on_owner() && size >= 2 && !memcmp(text + size - 2, "\r\n", 2));
    assert(size <= XIAOTAI_AT_OUT_LINE_MAX + 2U && wire.used + size < sizeof(wire.output));
    memcpy(wire.output + wire.used, text, size);
    wire.used += size;
    wire.output[wire.used] = '\0';
    return XIAOTAI_OK;
}

static void command(const char *input, const char *part, bool ok)
{
    wire.input = input;
    wire.left = strlen(input);
    wire.used = 0;
    wire.output[0] = '\0';
    for (unsigned i = 0; wire.left && i < 10U; ++i) assert(xiaotai_at_service(4) == XIAOTAI_OK);
    assert(!wire.left);
    const bool got_ok = strncmp(wire.output, "OK\r\n", 4U) == 0 || strstr(wire.output, "\r\nOK\r\n") != NULL;
    if (!strstr(wire.output, part) || got_ok != ok)
        fprintf(stderr, "%s expected (%d): %s\nactual: %s\n", input, ok, part, wire.output);
    assert(strstr(wire.output, part) && got_ok == ok);
}

static xiaotai_err_t borrow(void *self, xiaotai_camera_frame_t *f)
{
    assert(self && xiaotai_task_is_current(&video_engine.tasks[VIDEO_TASK_UPLINK].handle));
    if (!atomic_exchange(&credits, 0U)) return XIAOTAI_ERR_NOT_FOUND;
    *f = (xiaotai_camera_frame_t){.format = XIAOTAI_CAMERA_YUV420_PAIRED, .data = raw, .size = sizeof(raw),
        .width = 384, .height = 256, .rows = 256, .captured_us = xiaotai_time_us()};
    return XIAOTAI_OK;
}

static xiaotai_err_t return_frame(void *self, xiaotai_camera_frame_t *f)
{
    assert(self && f->data == raw);
    *f = (xiaotai_camera_frame_t){0};
    return XIAOTAI_OK;
}

static int match(const xiaotai_video_encoder_config_t *c)
{
    return c->codec == XIAOTAI_VIDEO_CODEC_H264 && c->pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED ? 1 : 0;
}

static xiaotai_err_t open_encoder(const xiaotai_video_encoder_config_t *c, void **state)
{
    assert(c->output && c->output_capacity >= sizeof(complete));
    encoded = c->output;
    *state = c->output;
    return XIAOTAI_OK;
}

static xiaotai_err_t encode(void *state, const xiaotai_video_image_t *image, bool key, xiaotai_video_packet_t *p)
{
    assert(state == encoded && image->data == raw);
    const size_t size = key ? sizeof(complete) : sizeof(delta);
    memcpy(encoded, key ? complete : delta, size);
    *p = (xiaotai_video_packet_t){.data = encoded, .size = size, .key_frame = key};
    return XIAOTAI_OK;
}

static void close_encoder(void *state) { assert(state == encoded); }

static const xiaotai_video_encoder_t h264 = {.name = "video-at-encoder", .load = XIAOTAI_VIDEO_LOAD_HARDWARE,
    .output_alignment = 64, .match = match, .open = open_encoder, .encode = encode, .close = close_encoder};
static const xiaotai_video_profile_t profile = XIAOTAI_VIDEO_PROFILE_H264_COMPACT;

static bool ready(void *ctx, uint32_t generation) { (void)ctx; return generation == 4; }

static xiaotai_err_t send(void *ctx, uint32_t generation, const xiaotai_video_packet_t *p, uint32_t limit)
{
    (void)ctx; (void)limit;
    assert(generation == 4 && p->data == encoded);
    if (p->key_frame) atomic_fetch_add(&keys, 1U);
    atomic_fetch_add(&sends, 1U);
    return XIAOTAI_OK;
}

static xiaotai_err_t diagnostics_init(void)
{
    const xiaotai_err_t err = xiaotai_engine_video_init();
    if (err != XIAOTAI_OK || !fixture_capture) return err;
    return xiaotai_video_require_uplink(&profile);
}

static void setup(bool capture_supported)
{
    memset(&wire, 0, sizeof(wire));
    memset(raw, 0, sizeof(raw));
    fake_hal_reset();
    fake_backends_reset();
    fake_clock_hold(1000000);
    atomic_store(&sends, 0U);
    atomic_store(&keys, 0U);
    atomic_store(&credits, 0U);
    encoded = NULL;
    fixture_capture = capture_supported;
    xiaotai_camera_desc_t desc = {.min_width = 384, .max_width = 384, .min_height = 256, .max_height = 256,
        .formats = XIAOTAI_CAMERA_YUV420_PAIRED, .rotation = 3};
    fake_camera_t *camera = fake_camera_add(&desc, false);
    assert(camera);
    camera->part.borrow = borrow;
    camera->part.release = return_frame;
    if (capture_supported) assert(xiaotai_video_encoder_register(&h264) == XIAOTAI_OK);
    static const xiaotai_at_io_t io = {.name = "video", .read = read_bytes, .write = record};
    const xiaotai_at_config_t cfg = {.task_stack_bytes = XIAOTAI_AT_NO_TASK, .no_platform_console = true};
    assert(xiaotai_at_configure(&cfg) == XIAOTAI_OK && xiaotai_at_attach(&io) == XIAOTAI_OK);
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    static const xiaotai_module_t eventlog = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const xiaotai_module_t video = {.name = "video", .init = diagnostics_init, .start = xiaotai_engine_video_start};
    static const xiaotai_module_t *const modules[] = {&eventlog, &at, &video};
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 3}) == XIAOTAI_OK);
}

static void cleanup(void)
{
    xiaotai_video_reset();
    xiaotai_at_reset();
    xiaotai_framework_reset();
    xiaotai_eventlog_reset();
    fake_clock_release();
}

static void worst_case_counts(void)
{
    xiaotai_critical_enter(&video_engine.lock);
    xiaotai_video_downlink_status_t *d = &video_engine.status.downlink;
    *d = (xiaotai_video_downlink_status_t){.phase = XIAOTAI_VIDEO_STOPPING, .error = XIAOTAI_ERR_NOT_SUPPORTED,
        .codec = XIAOTAI_VIDEO_CODEC_H264, .waiting_key_frame = true, .unsupported = true, .quarantined = true,
        .buffering = true, .source_width = UINT16_MAX, .source_height = UINT16_MAX, .received_bytes = UINT64_MAX,
        .received = UINT32_MAX, .stale = UINT32_MAX, .rejected = UINT32_MAX, .overflow = UINT32_MAX, .discarded = UINT32_MAX,
        .decoded = UINT32_MAX, .decode_failed = UINT32_MAX, .converted = UINT32_MAX, .convert_failed = UINT32_MAX,
        .presented = UINT32_MAX, .skipped = UINT32_MAX, .discontinuities = UINT32_MAX, .key_requests = UINT32_MAX,
        .queue_depth = 255, .playout_depth = 255};
    xiaotai_critical_exit(&video_engine.lock);
    command("AT+VIDEO\r", " queue_depth=255 playout_depth=255\r\nOK\r\n", true);
    assert(strstr(wire.output, "received_bytes=18446744073709551615"));
}

static void unused_reply(void *ctx, const char *line) { (void)ctx; (void)line; }

static void capture_session(void)
{
    static const xiaotai_video_link_t link = {.ready = ready, .send = send};
    assert(xiaotai_video_open(&(const xiaotai_video_session_t){.generation = 4, .uplink = &profile, .link = &link}) == XIAOTAI_OK);
    WAIT(video_session_diagnostics_busy());
    for (unsigned i = 0; atomic_load(&sends) == 0 && i < 1000U; ++i) {
        atomic_store(&credits, 1U);
        fake_clock_advance_us(5000);
        xiaotai_delay_ms(1);
    }
    assert(atomic_load(&sends) == 1 && atomic_load(&keys) == 1);
    command("AT+VIDEO\r", "up_y_state=valid up_y_mean=0 up_y_n=64", true);
    const xiaotai_reply_t reply = {.write = unused_reply};
    assert(xiaotai_command_run("AT+VIDEO CAPTURE START", &reply, 2000) == XIAOTAI_ERR_NOT_ALLOWED);
    command("AT+VIDEO CAPTURE START\r", "phase=armed generation=4 bytes=0 frames=0", true);
    command("AT+VIDEO CAPTURE START\r", "ERROR: INVALID_STATE CAPTURE_STATE", false);
    memset(raw, 128, sizeof(raw));
    atomic_store(&credits, 1U);
    fake_clock_advance_us(100000);
    WAIT(atomic_load(&sends) == 2 && atomic_load(&keys) == 2);
    command("AT+VIDEO\r", "up_y_state=valid up_y_mean=0 up_y_n=64", true);
    command("AT+VIDEO CAPTURE STOP\r", "phase=stopped generation=4 bytes=18 frames=1", true);
    command("AT+VIDEO CAPTURE DUMP\r", "ERROR: INVALID_STATE SESSION", false);
    command("AT+VIDEO PROBE\r", "OK\r\n", true);
    for (unsigned i = 0; i < 50U && !strstr(wire.output, "error=BUSY"); ++i)
        assert(xiaotai_at_service(2) == XIAOTAI_OK);
    assert(strstr(wire.output, "OK\r\n+VIDEO-PROBE: result=failed frames=0 error=BUSY\r\n"));
    xiaotai_video_close(4);
    WAIT(!video_session_diagnostics_busy());
    command("AT+VIDEO\r", "up_y_state=no_frame up_y_mean=-1 up_y_n=0", true);
    command("AT+VIDEO CAPTURE DUMP\r", "+VIDEO-CAPTURE: format=h264 generation=4 width=384 height=256 frames=1 bytes=18 fnv=", true);
    char expected[128], hex[2U * sizeof(complete) + 1U];
    assert(xiaotai_at_hex_encode(complete, sizeof(complete), hex, sizeof(hex)) == 2U * sizeof(complete));
    (void)snprintf(expected, sizeof(expected), "+VIDEO-CAPTURE-DATA: seq=0 hex=%s", hex);
    assert(strstr(wire.output, expected));
    uint32_t fnv = UINT32_C(2166136261);
    for (size_t i = 0; i < sizeof(complete); ++i) fnv = (fnv ^ complete[i]) * UINT32_C(16777619);
    (void)snprintf(expected, sizeof(expected), "+VIDEO-CAPTURE-END: chunks=1 fnv=%08" PRIx32, fnv);
    assert(strstr(wire.output, expected));
    command("AT+VIDEO CAPTURE CLEAR\r", "phase=empty generation=0 bytes=0 frames=0", true);
    command("AT+VIDEO CAPTURE DUMP\r", "ERROR: INVALID_STATE CAPTURE_STATE", false);
}

int main(void)
{
    setup(true);
    command("AT+HELP\r", "+HELP: AT+VIDEO\r\n+HELP: AT+VIDEO CAPTURE START|STOP|STATUS|CLEAR|DUMP\r\n+HELP: AT+VIDEO PROBE\r\n", true);
    assert(!strstr(wire.output, "AT+CAMERA"));
    command("AT+VIDEO\r", "camera_width=384 camera_height=256 camera_rotation=270", true);
    command("AT+STATUS video\r", "+VIDEO-DOWN: phase=off", true);
    command("AT+VIDEO CAPTURE OTHER\r", "ERROR: INVALID_ARG CAPTURE_VERB", false);
    command("AT+VIDEO CAPTURE\r", "ERROR: INVALID_ARG ARGUMENT_COUNT", false);
    command("AT+VIDEO CAPTURE START\r", "ERROR: INVALID_STATE H264_UPLINK", false);
    worst_case_counts();
    capture_session();
    cleanup();
    setup(false);
    command("AT+VIDEO CAPTURE STATUS\r", "ERROR: NOT_SUPPORTED H264_UPLINK", false);
    assert(video_engine.tasks[VIDEO_TASK_UPLINK].created && !video_engine.tasks[VIDEO_TASK_SESSION].created);
    cleanup();
    return 0;
}
