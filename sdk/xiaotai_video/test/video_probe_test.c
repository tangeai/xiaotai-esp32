// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_camera_frame_contract.py
#include <stdarg.h>
#include <string.h>

#include "fakes.h"
#include "internal.h"
#include "probe.h"
#include "xiaotai/at.h"
#include "xiaotai/at_command.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/video_profiles.h"

enum { WAIT_MS = 2500, MAX_REPORTS = 4 };

static const xiaotai_video_profile_t k_stream = XIAOTAI_VIDEO_PROFILE_JPEG_STREAM;
static const xiaotai_camera_frame_t *outstanding;
static xiaotai_camera_mode_t opened_mode;
static xiaotai_signal_t entered, unblocked, done;
static char reports[MAX_REPORTS][160];
static char luma_report[96];
static unsigned lines, prepares, powers, opens, closes, releases;
static bool powered, attempted_open, boot_prepared;
static bool fallback, small_frame, mismatch, delayed, timeout, reject_open, drop_report, hold_borrow, returned_on_error;
static bool brighten_after_first;
static atomic_bool no_frames;
static atomic_int release_error, close_error;
static uint8_t bytes[320U * 240U * 2U + 1U];

static void on_worker(void)
{
    CHECK(xiaotai_task_is_current(&video_engine.tasks[VIDEO_TASK_UPLINK].handle));
}

xiaotai_err_t __wrap_xiaotai_at_notifyf(const char *format, ...) __attribute__((format(printf, 1, 2)));
xiaotai_err_t __wrap_xiaotai_at_notifyf(const char *format, ...)
{
    on_worker();
    char line[sizeof(reports[0])];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    CHECK(length > 0 && (size_t)length < sizeof(line));
    if (!strncmp(line, "+VIDEO-PROBE-LUMA: ", sizeof("+VIDEO-PROBE-LUMA: ") - 1U)) {
        CHECK(!luma_report[0] && (size_t)length < sizeof(luma_report));
        memcpy(luma_report, line, (size_t)length + 1U);
        return drop_report ? XIAOTAI_ERR_NO_MEM : XIAOTAI_OK;
    }
    CHECK(lines < MAX_REPORTS);
    memcpy(reports[lines], line, (size_t)length + 1U);
    const bool final = !strncmp(line, "+VIDEO-PROBE: ", 14);
    ++lines;
    if (final) xiaotai_signal_give(&done);
    return drop_report && !final ? XIAOTAI_ERR_NO_MEM : XIAOTAI_OK;
}

static xiaotai_err_t prepare_camera(void *context)
{
    on_worker();
    CHECK(context && !powered);
    ++prepares;
    return XIAOTAI_OK;
}

static xiaotai_err_t power_camera(void *context, bool enable)
{
    on_worker();
    CHECK(context && enable != powered && !attempted_open && !outstanding && prepares == (unsigned)boot_prepared);
    powered = enable;
    ++powers;
    return XIAOTAI_OK;
}

static xiaotai_err_t open_camera(void *context, const xiaotai_camera_mode_t *mode)
{
    on_worker();
    CHECK(context && powered && !attempted_open && !outstanding);
    opened_mode = *mode;
    attempted_open = true;
    ++opens;
    if (fallback && mode->width > 8U) return XIAOTAI_ERR_NOT_SUPPORTED;
    return reject_open ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
}

static xiaotai_err_t finish_camera(void *context)
{
    on_worker();
    CHECK(context && powered && attempted_open && !outstanding);
    ++closes;
    const xiaotai_err_t result = atomic_load(&close_error);
    if (result == XIAOTAI_OK) attempted_open = false;
    return result;
}

static xiaotai_err_t take_frame(void *context, xiaotai_camera_frame_t *frame)
{
    on_worker();
    CHECK(context && powered && attempted_open && !outstanding);
    if (hold_borrow) {
        hold_borrow = false;
        xiaotai_signal_give(&entered);
        CHECK(xiaotai_signal_take(&unblocked, WAIT_MS));
    }
    if (timeout) {
        fake_clock_advance_us(3000000);
        return XIAOTAI_ERR_NOT_FOUND;
    }
    if (delayed || atomic_load(&no_frames)) { delayed = false; return XIAOTAI_ERR_NOT_FOUND; }
    const bool paired = opened_mode.format == XIAOTAI_CAMERA_YUV420_PAIRED;
    const uint16_t rows = paired ? 16U : opened_mode.height;
    size_t size = (size_t)opened_mode.width * rows * (paired ? 3U : 4U) / 2U;
    if (paired) ++size; /* the camera contract permits a tail */
    CHECK(size <= sizeof(bytes));
    if (brighten_after_first && releases) memset(bytes, 255, size);
    *frame = (xiaotai_camera_frame_t){.data = bytes, .size = small_frame ? size - 2U : size, .width = opened_mode.width,
        .height = opened_mode.height, .rows = rows, .format = mismatch ? XIAOTAI_CAMERA_RGB565 : opened_mode.format};
    outstanding = frame;
    return XIAOTAI_OK;
}

static xiaotai_err_t put_frame(void *context, xiaotai_camera_frame_t *frame)
{
    on_worker();
    CHECK(context && powered && attempted_open && frame == outstanding && frame->data == bytes);
    ++releases;
    const xiaotai_err_t error = atomic_load(&release_error);
    if (error == XIAOTAI_OK || returned_on_error) {
        outstanding = NULL;
        *frame = (xiaotai_camera_frame_t){0};
    }
    return error;
}

static xiaotai_err_t fixture_video_init(void)
{
    const xiaotai_err_t result = xiaotai_engine_video_init();
    if (result != XIAOTAI_OK || !boot_prepared) return result;
    fake_encoder_ctl[0].codec = XIAOTAI_VIDEO_CODEC_JPEG;
    fake_encoder_ctl[0].pixel = XIAOTAI_VIDEO_PIXEL_YUYV422;
    fake_encoder_ctl[0].score = 2;
    CHECK(xiaotai_video_encoder_register(&fake_encoders[0]) == XIAOTAI_OK);
    return xiaotai_video_require_uplink(&k_stream);
}

static void setup(bool paired, bool real_session)
{
    CHECK(!powered && !attempted_open && !outstanding);
    lines = prepares = powers = opens = closes = releases = 0;
    boot_prepared = real_session;
    fallback = small_frame = mismatch = delayed = timeout = reject_open = drop_report = hold_borrow = false;
    brighten_after_first = false;
    returned_on_error = false;
    atomic_store(&no_frames, false);
    atomic_store(&release_error, XIAOTAI_OK);
    atomic_store(&close_error, XIAOTAI_OK);
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
    fake_clock_hold(1000000);
    fake_hal_reset();
    fake_backends_reset();
    const xiaotai_camera_desc_t desc = paired ? (xiaotai_camera_desc_t){.formats = XIAOTAI_CAMERA_YUV420_PAIRED,
        .max_width = 16, .max_height = 12, .min_width = 16, .min_height = 12, .row_align = 16} :
        (xiaotai_camera_desc_t){.formats = XIAOTAI_CAMERA_YUV422, .max_width = real_session ? 320 : 16,
            .max_height = real_session ? 240 : 12, .min_width = real_session ? 160 : 8, .min_height = real_session ? 120 : 6};
    fake_camera_t *part = fake_camera_add(&desc, true);
    part->part.prepare = prepare_camera;
    part->part.set_power = power_camera;
    part->part.open = open_camera;
    part->part.close = finish_camera;
    part->part.borrow = take_frame;
    part->part.release = put_frame;
    CHECK(xiaotai_signal_init(&entered) == XIAOTAI_OK && xiaotai_signal_init(&unblocked) == XIAOTAI_OK &&
          xiaotai_signal_init(&done) == XIAOTAI_OK);
    static const xiaotai_module_t eventlog = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init};
    static const char *const dependencies[] = {"eventlog", "at", NULL};
    static const xiaotai_module_t video = {.name = "video", .depends = dependencies, .init = fixture_video_init,
        .start = xiaotai_engine_video_start};
    static const xiaotai_module_t *const modules[] = {&eventlog, &at, &video};
    CHECK(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 3}) == XIAOTAI_OK);
    CHECK(video_engine.tasks[VIDEO_TASK_UPLINK].created && prepares == (unsigned)boot_prepared && !powers);
}

static void teardown(void)
{
    xiaotai_video_reset();
    xiaotai_at_reset();
    xiaotai_framework_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&entered);
    xiaotai_signal_deinit(&unblocked);
    xiaotai_signal_deinit(&done);
    fake_clock_release();
    CHECK(!outstanding && !powered && !attempted_open);
}

static void request(void)
{
    xiaotai_err_t result;
    luma_report[0] = '\0';
    for (unsigned i = 0; i < WAIT_MS; ++i) {
        result = video_probe_request();
        if (result != XIAOTAI_ERR_BUSY) { CHECK(result == XIAOTAI_OK); return; }
        xiaotai_delay_ms(1);
    }
    CHECK(false, "previous job never finished");
}

static void expect(unsigned expected_lines, const char *final)
{
    CHECK(xiaotai_signal_take(&done, WAIT_MS));
    CHECK(lines == expected_lines && !strcmp(reports[lines - 1U], final), "got %u lines, last: %s", lines,
          lines ? reports[lines - 1U] : "(none)");
}

static void test_sizes_and_reports(void)
{
    setup(true, false);
    hold_borrow = delayed = drop_report = true;
    request();
    CHECK(xiaotai_signal_take(&entered, WAIT_MS));
    CHECK(video_probe_request() == XIAOTAI_ERR_BUSY); /* remains pending during sensor IO */
    xiaotai_signal_give(&unblocked);
    expect(4, "+VIDEO-PROBE: result=passed frames=3 error=NONE");
    CHECK(powers == 2 && opens == 1 && closes == 1 && releases == 3 && video_probe_dropped_reports() == 4);
    CHECK(!strcmp(luma_report, "+VIDEO-PROBE-LUMA: state=valid y_mean=113 samples=64"));
    CHECK(!strcmp(reports[0], "+VIDEO-PROBE-FRAME: index=0 width=16 height=12 format=yuv420_paired bytes=385 crc32=14318a98 ok=1"));
    CHECK(strstr(reports[2], "index=2 width=16"));
    teardown();

    setup(false, false);
    fallback = true;
    brighten_after_first = true;
    request();
    expect(4, "+VIDEO-PROBE: result=passed frames=3 error=NONE");
    CHECK(powers == 2 && opens == 2 && closes == 2 && releases == 3);
    CHECK(!strcmp(reports[0], "+VIDEO-PROBE-FRAME: index=0 width=8 height=6 format=yuv422 bytes=96 crc32=51c87372 ok=1"));
    CHECK(!strcmp(luma_report, "+VIDEO-PROBE-LUMA: state=valid y_mean=47 samples=48"));
    teardown();
}

static void test_failures(void)
{
    setup(true, false);
    small_frame = true;
    request();
    expect(2, "+VIDEO-PROBE: result=failed frames=1 error=INVALID_SIZE");
    CHECK(releases == 1 && closes == 1 && strstr(reports[0], "bytes=383 crc32=06cb17ff ok=0"));
    CHECK(!strcmp(luma_report, "+VIDEO-PROBE-LUMA: state=invalid y_mean=-1 samples=0"));
    teardown();

    setup(false, false);
    mismatch = true;
    request();
    expect(2, "+VIDEO-PROBE: result=failed frames=1 error=INVALID_SIZE");
    CHECK(strstr(reports[0], "format=rgb565") && strstr(reports[0], "ok=0") && releases == 1 && closes == 1);
    teardown();

    setup(false, false);
    reject_open = true;
    request();
    expect(1, "+VIDEO-PROBE: result=failed frames=0 error=TIMEOUT");
    CHECK(powers == 2 && closes == 1 && !releases);
    teardown();

    setup(false, false);
    timeout = true;
    request();
    expect(1, "+VIDEO-PROBE: result=failed frames=0 error=TIMEOUT");
    CHECK(powers == 2 && closes == 1 && !releases);
    CHECK(!strcmp(luma_report, "+VIDEO-PROBE-LUMA: state=no_frame y_mean=-1 samples=0"));
    teardown();
}

static void test_retained_cleanup(void)
{
    setup(false, false);
    atomic_store(&release_error, XIAOTAI_FAIL);
    request();
    expect(2, "+VIDEO-PROBE: result=failed frames=1 error=FAIL");
    CHECK(releases == 1 && !closes && powers == 1 && outstanding);
    lines = 0;
    request();
    expect(1, "+VIDEO-PROBE: result=failed frames=0 error=BUSY");
    CHECK(!luma_report[0]);
    CHECK(releases == 1 && !closes && powers == 1);
    atomic_store(&release_error, XIAOTAI_OK);
    teardown();
    CHECK(releases == 2 && closes == 1 && powers == 2);

    setup(false, false);
    returned_on_error = true; /* requeue failed but the camera consumed the descriptor */
    atomic_store(&release_error, XIAOTAI_FAIL);
    request();
    expect(2, "+VIDEO-PROBE: result=failed frames=1 error=FAIL");
    CHECK(releases == 1 && closes == 1 && powers == 2 && !outstanding);
    teardown();

    setup(false, false);
    atomic_store(&close_error, XIAOTAI_ERR_INVALID_STATE);
    request();
    expect(4, "+VIDEO-PROBE: result=failed frames=3 error=INVALID_STATE");
    CHECK(releases == 3 && closes == 1 && powers == 1);
    atomic_store(&close_error, XIAOTAI_OK);
    teardown();
    CHECK(closes == 2 && powers == 2);
}

static void test_running_session(void)
{
    setup(false, true);
    fake_link_state.ready = true;
    atomic_store(&no_frames, true);
    CHECK(xiaotai_video_open(&(const xiaotai_video_session_t){.generation = 7, .uplink = &k_stream,
          .link = fake_link()}) == XIAOTAI_OK);
    xiaotai_video_status_t status;
    for (unsigned i = 0; i < WAIT_MS; ++i) {
        xiaotai_video_status(&status);
        if (status.uplink.phase == XIAOTAI_VIDEO_RUNNING) break;
        xiaotai_delay_ms(1);
    }
    CHECK(status.uplink.phase == XIAOTAI_VIDEO_RUNNING && status.generation == 7);
    request();
    expect(1, "+VIDEO-PROBE: result=failed frames=0 error=BUSY");
    xiaotai_video_status(&status);
    CHECK(status.uplink.phase == XIAOTAI_VIDEO_RUNNING && opens == 1 && !closes && powers == 1);
    teardown();
}

static void test_cancel_for_new_session(void)
{
    setup(false, true);
    fake_link_state.ready = false;
    hold_borrow = true;
    request();
    CHECK(xiaotai_signal_take(&entered, WAIT_MS));
    CHECK(xiaotai_video_open(&(const xiaotai_video_session_t){.generation = 9, .uplink = &k_stream,
          .link = fake_link()}) == XIAOTAI_OK);
    xiaotai_signal_give(&unblocked);
    expect(2, "+VIDEO-PROBE: result=failed frames=1 error=BUSY");
    CHECK(releases == 1 && closes == 1 && powers == 2 && !outstanding);
    teardown();
}

int main(void)
{
    test_sizes_and_reports();
    test_failures();
    test_retained_cleanup();
    test_running_session();
    test_cancel_for_new_session();
    CHECK(video_probe_request() == XIAOTAI_ERR_INVALID_STATE);
    return 0;
}
