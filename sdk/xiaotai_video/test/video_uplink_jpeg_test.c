// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_h5_video_send.py
// Replaces legacy: tests/regression/test_camera_pacing.py
#include <stdatomic.h>
#include <string.h>

#include "fakes.h"
#include "internal.h"
#include "uplink.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/video_profiles.h"

enum { PREPARE = 1, POWER, OPEN, BORROW, ENCODE, RELEASE, SEND, CLOSE, ENC_CLOSE, OFF, QUALITY, ENC_OPEN };
enum { TRACE_MAX = 192, TEST_WAIT_MS = 2000 };

static unsigned trace[TRACE_MAX];
static atomic_uint trace_count, frames, revoke, bad_raw, raw_age_us, late_encode, bad_jpeg, close_error, send_result;
static atomic_uint backlog, send_backlog, sent_ts, sent_flags, frame_count, release_error;
static atomic_bool allowed;
static bool prepare_error, restore_on_resize;
static enum { DVP_DIRECT, DVP_NATIVE_ONLY, CSI_NATIVE } camera_case;
static bool powered, attempted_open;
static uint8_t raw[1024U * 608U * 3U / 2U];
static const uint8_t *boot_transform;
static xiaotai_camera_mode_t mode;
static xiaotai_camera_frame_t *outstanding;
static xiaotai_video_encoder_config_t encoder_config;
static xiaotai_signal_t owner_done;
static xiaotai_err_t owner_result;
static xiaotai_video_profile_t profile;
static const xiaotai_video_profile_t stream = XIAOTAI_VIDEO_PROFILE_JPEG_STREAM;
static const xiaotai_video_profile_t still = XIAOTAI_VIDEO_PROFILE_JPEG_STILL;
static const xiaotai_video_downlink_t downlink = XIAOTAI_VIDEO_DOWNLINK_H264_COMPACT;

static void record(unsigned action)
{
    CHECK(xiaotai_task_is_current(&video_engine.tasks[VIDEO_TASK_UPLINK].handle));
    CHECK(!xiaotai_on_owner());
    const unsigned offset = atomic_load(&trace_count);
    CHECK(offset < TRACE_MAX);
    trace[offset] = action;
    atomic_store(&trace_count, offset + 1);
    if (atomic_load(&revoke) == action) atomic_store(&allowed, false);
}

static unsigned count(unsigned action)
{
    unsigned matches = 0;
    const unsigned end = atomic_load(&trace_count);
    for (unsigned i = 0; i < end; ++i) if (trace[i] == action) ++matches;
    return matches;
}

static void wait_calls(unsigned action, unsigned minimum)
{
    for (unsigned i = 0; i < TEST_WAIT_MS && count(action) < minimum; ++i) xiaotai_delay_ms(1);
    CHECK(count(action) >= minimum, "wanted %u calls to %u; got %u", minimum, action, count(action));
}

static xiaotai_err_t camera_prepare(void *arg)
{
    (void)arg; record(PREPARE);
    return prepare_error ? XIAOTAI_ERR_NO_MEM : XIAOTAI_OK;
}
static xiaotai_err_t camera_power(void *arg, bool on)
{
    CHECK(arg && !attempted_open && on != powered);
    powered = on;
    record(on ? POWER : OFF);
    return XIAOTAI_OK;
}
static xiaotai_err_t camera_open(void *arg, const xiaotai_camera_mode_t *new_mode)
{
    CHECK(arg && powered && !attempted_open);
    attempted_open = true;
    mode = *new_mode; record(OPEN);
    if (camera_case == CSI_NATIVE) {
        CHECK(mode.width == 1024 && mode.height == 600 && mode.format == XIAOTAI_CAMERA_YUV420_PAIRED);
    } else {
        CHECK(mode.width <= 320 && mode.height <= 240 && mode.format == XIAOTAI_CAMERA_YUV422);
        if (camera_case == DVP_NATIVE_ONLY && mode.width != 320) return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    return XIAOTAI_OK;
}
static xiaotai_err_t camera_close(void *arg)
{
    CHECK(arg && powered && attempted_open);
    record(CLOSE);
    const xiaotai_err_t err = outstanding ? XIAOTAI_ERR_INVALID_STATE : (xiaotai_err_t)atomic_load(&close_error);
    if (err == XIAOTAI_OK) attempted_open = false;
    return err;
}
static xiaotai_err_t camera_borrow(void *arg, xiaotai_camera_frame_t *frame)
{
    (void)arg;
    atomic_fetch_add(&frame_count, 1);
    unsigned available = atomic_load(&frames);
    if (!available || !atomic_compare_exchange_strong(&frames, &available, available - 1U)) return XIAOTAI_ERR_NOT_FOUND;
    CHECK(!outstanding);
    const bool invalid = atomic_exchange(&bad_raw, 0U);
    const uint32_t age = atomic_exchange(&raw_age_us, 0U);
    const int64_t at = xiaotai_time_us() - (age ? age : 17000U);
    const uint16_t rows = camera_case == CSI_NATIVE ? 608U : mode.height;
    const size_t bytes = (size_t)mode.width * rows * (camera_case == CSI_NATIVE ? 3U : 4U) / 2U;
    *frame = (xiaotai_camera_frame_t){.format = mode.format, .data = raw,
        .width = mode.width, .height = mode.height, .rows = rows,
        .size = bytes - (invalid ? 1U : 0U), .captured_us = at};
    outstanding = frame;
    record(BORROW);
    return XIAOTAI_OK;
}
static xiaotai_err_t camera_release(void *arg, xiaotai_camera_frame_t *frame)
{
    (void)arg;
    CHECK(frame == outstanding && frame->data == raw);
    const xiaotai_err_t err = (xiaotai_err_t)atomic_load(&release_error);
    if (err == XIAOTAI_OK) {
        *frame = (xiaotai_camera_frame_t){0};
        outstanding = NULL;
    }
    record(RELEASE);
    return err;
}

static int encoder_match(const xiaotai_video_encoder_config_t *config)
{
    return config->codec == XIAOTAI_VIDEO_CODEC_JPEG && config->pixel == XIAOTAI_VIDEO_PIXEL_YUYV422 ? 5 : 0;
}
static xiaotai_err_t encoder_open(const xiaotai_video_encoder_config_t *config, void **state)
{
    CHECK(config->output && !((uintptr_t)config->output & 15U));
    CHECK(config->output_capacity == profile.max_frame_bytes && config->fps == profile.fps);
    encoder_config = *config;
    *state = &encoder_config;
    record(ENC_OPEN);
    return XIAOTAI_OK;
}
static xiaotai_err_t encoder_quality(void *state, const xiaotai_video_encoder_config_t *config)
{
    CHECK(state == &encoder_config && config->quality < encoder_config.quality);
    encoder_config = *config;
    record(QUALITY);
    return XIAOTAI_OK;
}
static xiaotai_err_t encoder_encode(void *state, const xiaotai_video_image_t *image, bool forced,
                                   xiaotai_video_packet_t *packet)
{
    CHECK(state == &encoder_config && forced);
    CHECK(image->pixel == XIAOTAI_VIDEO_PIXEL_YUYV422 && image->width == encoder_config.width &&
          image->height == encoder_config.height && image->stride == image->width && image->rows == image->height &&
          image->size == (size_t)image->width * image->height * 2U);
    CHECK((image->data == raw) == (camera_case == DVP_DIRECT));
    if (camera_case == CSI_NATIVE) {
        const uint8_t pair[] = {36, 89, 36, 173};
        CHECK(image->data == boot_transform && !memcmp(image->data, pair, sizeof(pair)));
    }
    if (atomic_exchange(&late_encode, 0U)) fake_clock_advance_us(90000);
    const uint8_t jpeg[] = {0xff, 0xd8, 0xff, atomic_load(&bad_jpeg) ? 0U : 0xd9};
    memcpy(encoder_config.output, jpeg, sizeof(jpeg));
    *packet = (xiaotai_video_packet_t){.data = encoder_config.output, .size = sizeof(jpeg)};
    record(ENCODE);
    return XIAOTAI_OK;
}
static void encoder_close(void *state) { CHECK(state == &encoder_config); record(ENC_CLOSE); }
static const xiaotai_video_encoder_t encoder = {.name = "jpeg-fixture", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
    .match = encoder_match, .open = encoder_open, .reconfigure = encoder_quality, .encode = encoder_encode,
    .close = encoder_close};

static bool link_ready(void *arg, uint32_t generation)
{
    (void)arg; CHECK(generation && xiaotai_task_is_current(&video_engine.tasks[VIDEO_TASK_UPLINK].handle));
    return atomic_load(&allowed);
}
static size_t link_backlog(void *arg, uint32_t generation)
{
    (void)arg; (void)generation;
    return atomic_load(&backlog) || (atomic_load(&send_backlog) && count(ENCODE)) ? profile.backlog_limit_bytes : 0;
}
static xiaotai_err_t link_send(void *arg, uint32_t generation, const xiaotai_video_packet_t *packet, uint32_t limit)
{
    (void)arg;
    CHECK(generation && packet->codec == XIAOTAI_VIDEO_CODEC_JPEG && packet->key_frame && packet->size == 4);
    CHECK(packet->width == encoder_config.width && packet->height == encoder_config.height);
    CHECK(packet->data[0] == 0xFF && packet->data[3] == 0xD9 && limit == profile.backlog_limit_bytes);
    const xiaotai_err_t result = (xiaotai_err_t)atomic_load(&send_result);
    atomic_store(&sent_ts, packet->timestamp_ms);
    atomic_store(&sent_flags, 1);
    record(SEND);
    return result;
}
static const xiaotai_video_link_t link = {.ready = link_ready, .backlog = link_backlog, .send = link_send};

static void boot_owner(void *arg) { (void)arg; xiaotai_engine_video_start(); xiaotai_signal_give(&owner_done); }
static void open_owner(void *arg) { owner_result = xiaotai_video_open(arg); xiaotai_signal_give(&owner_done); }
static void close_owner(void *arg) { xiaotai_video_close(*(uint32_t *)arg); xiaotai_signal_give(&owner_done); }
typedef struct { uint32_t generation; bool on; } camera_switch_t;
static void switch_owner(void *arg)
{
    const camera_switch_t *operation = arg;
    owner_result = xiaotai_video_set_camera_enabled(operation->generation, operation->on);
    xiaotai_signal_give(&owner_done);
}
static void owner_call(void (*fn)(void *), void *arg)
{
    CHECK(xiaotai_run(fn, arg) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&owner_done, TEST_WAIT_MS));
}

static xiaotai_video_status_t wait_phase(xiaotai_video_phase_t phase)
{
    xiaotai_video_status_t status;
    for (unsigned i = 0; i < TEST_WAIT_MS; ++i) {
        xiaotai_video_status(&status);
        if (status.uplink.phase == phase) return status;
        xiaotai_delay_ms(1);
    }
    CHECK(false, "uplink phase %u, wanted %u (error %d)", status.uplink.phase, phase, status.uplink.error);
    return status;
}

static xiaotai_video_uplink_status_t wait_frames(unsigned sent, unsigned congested)
{
    xiaotai_video_status_t status;
    for (unsigned i = 0; i < TEST_WAIT_MS; ++i) {
        xiaotai_video_status(&status);
        if (status.uplink.sent >= sent && status.uplink.congested >= congested) return status.uplink;
        xiaotai_delay_ms(1);
    }
    CHECK(false, "expected %u/%u sent/congested frames; got %u/%u", sent, congested,
          status.uplink.sent, status.uplink.congested);
    return status.uplink;
}

static void end(void)
{
    xiaotai_video_reset();
    xiaotai_framework_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&owner_done);
    fake_clock_release();
}

static void begin(bool prepare, bool broken_prepare, bool has_uplink, const xiaotai_video_profile_t *requested)
{
    fake_hal_reset(); fake_backends_reset(); fake_port_clear();
    CHECK(!outstanding && !powered && !attempted_open);
    atomic_store(&trace_count, 0); atomic_store(&frames, 0); atomic_store(&revoke, 0);
    atomic_store(&bad_raw, 0); atomic_store(&raw_age_us, 0); atomic_store(&bad_jpeg, 0);
    atomic_store(&late_encode, 0); atomic_store(&close_error, 0); atomic_store(&send_result, 0);
    atomic_store(&backlog, 0); atomic_store(&send_backlog, 0); atomic_store(&sent_ts, 0);
    atomic_store(&sent_flags, 0); atomic_store(&frame_count, 0); atomic_store(&allowed, true);
    atomic_store(&release_error, 0);
    profile = *requested;
    prepare_error = broken_prepare;
    fake_clock_hold(3000000);
    const xiaotai_camera_desc_t desc = camera_case == CSI_NATIVE ? (xiaotai_camera_desc_t){
        .formats = XIAOTAI_CAMERA_YUV420_PAIRED, .min_width = 1024, .max_width = 1024,
        .min_height = 600, .max_height = 600, .row_align = 16} : (xiaotai_camera_desc_t){
        .formats = XIAOTAI_CAMERA_YUV422, .max_width = 320, .max_height = 240, .min_width = 160, .min_height = 120};
    fake_camera_t *camera = fake_camera_add(&desc, prepare);
    camera->part.prepare = prepare ? camera_prepare : NULL;
    camera->part.set_power = camera_power;
    camera->part.open = camera_open;
    camera->part.close = camera_close;
    camera->part.borrow = camera_borrow;
    camera->part.release = camera_release;
    CHECK(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    if (restore_on_resize) {
        CHECK(xiaotai_video_configure(&(const xiaotai_video_config_t){.restore_initial_quality_on_resolution_reopen = true}) == XIAOTAI_OK);
    }
    CHECK(xiaotai_video_encoder_register(&encoder) == XIAOTAI_OK);
    if (has_uplink) CHECK(xiaotai_video_require_uplink(&profile) == XIAOTAI_OK);
    if (!has_uplink || broken_prepare) {
        fake_display_add(480, 480, XIAOTAI_PIXEL_RGB565, 0);
        const xiaotai_video_viewport_t viewport = {.width = 384, .height = 256, .pixel = XIAOTAI_VIDEO_PIXEL_RGB565};
        CHECK(xiaotai_video_require_viewport(&viewport) == XIAOTAI_OK);
        CHECK(xiaotai_video_require_downlink(&downlink) == XIAOTAI_OK);
        fake_decoder_ctl[0].codec = XIAOTAI_VIDEO_CODEC_H264;
        fake_decoder_ctl[0].score = 5;
        CHECK(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_OK);
    }
    CHECK(xiaotai_signal_init(&owner_done) == XIAOTAI_OK);
    CHECK(xiaotai_framework_start(&(xiaotai_framework_config_t){0}) == XIAOTAI_OK);
    owner_call(boot_owner, NULL);
    owner_call(boot_owner, NULL);
    CHECK(count(POWER) == 0 && count(OPEN) == 0 && count(ENC_OPEN) == 0);
    CHECK(count(PREPARE) == (prepare && has_uplink ? 1U : 0U));
}

static xiaotai_err_t request(uint32_t generation, bool send)
{
    xiaotai_video_session_t cfg = {.generation = generation, .uplink = send ? &profile : NULL,
        .downlink = send ? NULL : &downlink, .link = &link};
    owner_call(open_owner, &cfg);
    return owner_result;
}

static void advance(int64_t us, unsigned credit)
{
    fake_clock_advance_us(us);
    atomic_fetch_add(&frames, credit);
    xiaotai_task_notify(&video_engine.tasks[VIDEO_TASK_UPLINK].handle);
}

static void overload_window(unsigned window)
{
    /* Advance with frames in sub-threshold steps so the fixture cannot
     * inject an unrelated no-frame timeout across the five-second window. */
    for (unsigned j = 1; j <= 6; ++j) {
        advance(850000, 1);
        wait_frames(0, (window - 1U) * 6U + j);
    }
}

static void force_smaller(void)
{
    atomic_store(&backlog, 1);
    for (unsigned window = 1; window <= 4; ++window) {
        overload_window(window);
        if (window < 4) wait_calls(QUALITY, window);
    }
    wait_calls(OPEN, 2);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
}

static void test_prepare(void)
{
    begin(true, true, true, &stream);
    xiaotai_video_status_t status;
    xiaotai_video_status(&status);
    CHECK(status.started);
    CHECK(request(1, true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(request(1, false) == XIAOTAI_OK);
    xiaotai_eventlog_entry_t entries[16];
    const size_t total = xiaotai_eventlog_read(0, entries, 16);
    unsigned reserve_errors = 0;
    for (size_t i = 0; i < total; ++i)
        if (!strcmp(entries[i].label, "reserve.camera") && entries[i].value == XIAOTAI_ERR_NO_MEM) ++reserve_errors;
    CHECK(reserve_errors == 1 && count(PREPARE) == 1 && count(POWER) == 0);
    end();

    begin(true, false, false, &stream);
    CHECK(!video_engine.tasks[VIDEO_TASK_UPLINK].created);
    end();

    begin(true, false, true, &stream);
    CHECK(request(2, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    CHECK(trace[0] == PREPARE && trace[1] == POWER && count(PREPARE) == 1);
    end();
}

static void test_sequence(void)
{
    begin(false, false, true, &stream);
    size_t capacity;
    CHECK(!video_uplink_reserve_transform(1, 16, &capacity));
    CHECK(request(3, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    advance(0, 1);
    wait_calls(SEND, 1);
    uint32_t gen = 3;
    owner_call(close_owner, &gen);
    wait_phase(XIAOTAI_VIDEO_OFF);
    const unsigned expected[] = {POWER, OPEN, ENC_OPEN, BORROW, ENCODE, RELEASE, SEND, CLOSE, ENC_CLOSE, OFF};
    CHECK(atomic_load(&trace_count) == sizeof(expected) / sizeof(expected[0]));
    CHECK(!memcmp(expected, trace, sizeof(expected)));
    CHECK(atomic_load(&sent_ts) == 2983 && atomic_load(&sent_flags) == 1);
    end();
}

static void test_native_transform(void)
{
    size_t capacity;
    camera_case = DVP_NATIVE_ONLY;
    begin(false, false, true, &stream);
    CHECK(request(21, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    advance(0, 1);
    wait_frames(1, 0);
    const unsigned fallback[] = {POWER, OPEN, CLOSE, OPEN, ENC_OPEN, BORROW, ENCODE, RELEASE, SEND};
    CHECK(atomic_load(&trace_count) == sizeof(fallback) / sizeof(fallback[0]));
    CHECK(!memcmp(trace, fallback, sizeof(fallback)));
    end();

    camera_case = CSI_NATIVE;
    const size_t row = 1024U * 3U / 2U;
    for (size_t i = 0; i < sizeof(raw); i += 3U) {
        raw[i] = (i / row) & 1U ? 173U : 89U;
        raw[i + 1U] = raw[i + 2U] = 36U;
    }
    begin(true, false, true, &still);
    boot_transform = video_uplink_reserve_transform(320U * 240U * 2U, 64U, &capacity);
    CHECK(boot_transform && capacity == 320U * 240U * 2U);
    CHECK(request(22, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    advance(0, 1);
    const xiaotai_video_uplink_status_t status = wait_frames(1, 0);
    CHECK(status.width == 320 && status.height == 240 && status.capture_failed == 0 && status.encode_failed == 0);
    CHECK(count(OPEN) == 1 && count(RELEASE) == 1 && mode.width == 1024 && mode.height == 600);
    CHECK(raw[0] == 89 && raw[1] == 36 && raw[row] == 173);
    end();
    camera_case = DVP_DIRECT;
}

static void test_admission_edges(void)
{
    const unsigned actions[] = {POWER, OPEN, BORROW, ENCODE, RELEASE};
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        begin(false, false, true, &stream);
        atomic_store(&allowed, false);
        atomic_store(&revoke, actions[i]);
        CHECK(request(4, true) == XIAOTAI_OK);
        xiaotai_delay_ms(40);
        CHECK(count(POWER) == 0);
        atomic_store(&allowed, true);
        advance(0, 1);
        wait_calls(OFF, 1);
        CHECK(count(SEND) == 0);
        if (actions[i] == POWER) CHECK(count(OPEN) == 0);
        if (actions[i] == OPEN) CHECK(count(ENC_OPEN) == 0);
        if (actions[i] == BORROW) CHECK(count(ENCODE) == 0 && count(RELEASE) == 1);
        if (actions[i] == ENCODE || actions[i] == RELEASE) CHECK(count(RELEASE) == 1);
        end();
    }
}

static void test_failure(void)
{
    begin(false, false, true, &stream);
    CHECK(request(5, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    atomic_store(&bad_raw, 1);
    advance(0, 1);
    xiaotai_video_status_t status = wait_phase(XIAOTAI_VIDEO_ERROR);
    CHECK(status.uplink.capture_failed == 1 && count(RELEASE) == 1 && count(ENCODE) == 0 && count(SEND) == 0);
    advance(6000000, 1);
    xiaotai_delay_ms(40);
    CHECK(count(POWER) == 1);
    CHECK(request(6, true) == XIAOTAI_OK);
    wait_calls(SEND, 1);
    end();

    begin(false, false, true, &stream);
    CHECK(request(7, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    atomic_store(&bad_jpeg, 1);
    advance(0, 1);
    status = wait_phase(XIAOTAI_VIDEO_ERROR);
    CHECK(status.uplink.encode_failed == 1 && count(RELEASE) == 1 && count(SEND) == 0);
    end();
}

static void test_ai_freshness(void)
{
    CHECK(still.fps == 1 && still.max_age_ms == 750 && still.refetch_stale);
    begin(false, false, true, &still);
    CHECK(request(8, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    /* The legacy camera measures whole milliseconds: age 750 is allowed. */
    atomic_store(&raw_age_us, 750999U);
    advance(0, 1);
    xiaotai_video_uplink_status_t status = wait_frames(1, 0);
    CHECK(status.stale == 0 && count(BORROW) == 1 && count(ENCODE) == 1);
    end();

    begin(false, false, true, &still);
    CHECK(request(14, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    atomic_store(&raw_age_us, 751000U);
    /* Keep the clock fixed: a retry scheduled for the next second cannot pass. */
    advance(0, 2);
    status = wait_frames(1, 0);
    const unsigned expected[] = {POWER, OPEN, ENC_OPEN, BORROW, RELEASE, BORROW, ENCODE, RELEASE, SEND};
    CHECK(atomic_load(&trace_count) == sizeof(expected) / sizeof(expected[0]));
    CHECK(!memcmp(expected, trace, sizeof(expected)));
    CHECK(status.stale == 1 && atomic_load(&sent_ts) == 2983);
    end();
}

static void test_pressure_and_close_error(void)
{
    xiaotai_video_profile_t fast = stream;
    fast.pressure_limit = 1;
    begin(false, false, true, &fast);
    CHECK(request(9, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    force_smaller();
    xiaotai_video_status_t status;
    xiaotai_video_status(&status);
    CHECK(status.uplink.width == 160 && status.uplink.height == 120 && status.uplink.quality == 30);
    CHECK(status.uplink.congested == 24 && count(ENCODE) == 0 && count(SEND) == 0);
    atomic_store(&backlog, 0);
    atomic_store(&send_backlog, 1);
    advance(100000, 1);
    const uint32_t late_before = wait_frames(0, 25).late;
    CHECK(count(ENCODE) == 1 && count(SEND) == 0);
    atomic_store(&send_backlog, 0);
    atomic_store(&late_encode, 1);
    advance(100000, 1);
    status.uplink = wait_frames(1, 25);
    CHECK(status.uplink.late == late_before + 1U && status.uplink.encode_max_us == 90000,
          "late %u to %u; encode peak %u", late_before, status.uplink.late, status.uplink.encode_max_us);
    atomic_store(&close_error, XIAOTAI_ERR_INVALID_STATE);
    uint32_t gen = 9;
    owner_call(close_owner, &gen);
    wait_calls(CLOSE, 2);
    xiaotai_delay_ms(25);
    CHECK(count(OFF) == 1, "failed close must not power down");
    atomic_store(&close_error, XIAOTAI_OK);
    end();
    CHECK(count(OFF) == 2);
}

static void test_legacy_quality_resize(void)
{
    xiaotai_video_profile_t fast = stream;
    fast.pressure_limit = 1;
    restore_on_resize = true;
    memset(raw, 20, sizeof(raw));
    begin(false, false, true, &fast);
    CHECK(request(20, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    force_smaller();
    xiaotai_video_status_t status;
    video_luma_t luma;
    video_status_with_luma(&status, &luma);
    CHECK(status.uplink.width == 160 && status.uplink.height == 120 && status.uplink.quality == 45);
    CHECK(status.uplink.congested == 24);
    CHECK(luma.state == VIDEO_LUMA_NO_FRAME);
    memset(raw, 80, sizeof(raw));
    overload_window(5);
    wait_calls(QUALITY, 4);
    video_status_with_luma(&status, &luma);
    CHECK(status.uplink.width == 160 && status.uplink.quality == 40 && count(OPEN) == 2);
    CHECK(luma.state == VIDEO_LUMA_VALID && luma.mean == 80 && luma.samples == 64);

    camera_switch_t operation = {.generation = 20, .on = false};
    owner_call(switch_owner, &operation);
    CHECK(owner_result == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_OFF);
    operation.on = true;
    owner_call(switch_owner, &operation);
    CHECK(owner_result == XIAOTAI_OK);
    wait_calls(OPEN, 3);
    status = wait_phase(XIAOTAI_VIDEO_RUNNING);
    CHECK(status.uplink.width == 160 && status.uplink.quality == 40 && status.uplink.congested == 30);
    video_status_with_luma(&status, &luma);
    CHECK(luma.state == VIDEO_LUMA_NO_FRAME);
    end();
    restore_on_resize = false;
}

static void test_no_frame_and_jpeg_bounds(void)
{
    const uint8_t bytes[] = {0xff, 0xd8, 0xff, 0xd9};
    xiaotai_video_packet_t packet = {.data = bytes, .size = 4};
    CHECK(video_jpeg_complete(&packet, bytes, sizeof(bytes), sizeof(bytes)));
    packet.data++;
    CHECK(!video_jpeg_complete(&packet, bytes, sizeof(bytes), sizeof(bytes)));
    packet.data = bytes;
    CHECK(!video_jpeg_complete(&packet, bytes, sizeof(bytes), 3));

    xiaotai_video_profile_t unchanged = stream;
    unchanged.fallback_width = unchanged.width;
    unchanged.fallback_height = unchanged.height;
    video_jpeg_pressure_t pressure = {.quality = unchanged.min_quality, .pressure = unchanged.pressure_limit};
    uint32_t fps = 0;
    CHECK(video_jpeg_evaluate(&pressure, &unchanged, true, 5000000, 0, &fps) == VIDEO_JPEG_KEEP && !pressure.smaller);

    begin(false, false, true, &stream);
    CHECK(request(10, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    for (unsigned i = 0; i < TEST_WAIT_MS && !atomic_load(&frame_count); ++i) xiaotai_delay_ms(1);
    CHECK(atomic_load(&frame_count) != 0);
    advance(1500000, 0);
    const xiaotai_video_status_t status = wait_phase(XIAOTAI_VIDEO_ERROR);
    CHECK(status.uplink.error == XIAOTAI_ERR_TIMEOUT && status.uplink.capture_failed == 1);
    end();
}

static void test_send_results(void)
{
    begin(false, false, true, &stream);
    CHECK(request(11, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    atomic_store(&send_result, XIAOTAI_ERR_INVALID_STATE);
    advance(0, 1);
    wait_calls(SEND, 1);
    atomic_store(&send_result, XIAOTAI_ERR_BUSY);
    advance(100000, 1);
    wait_frames(0, 1);
    atomic_store(&send_result, XIAOTAI_ERR_TIMEOUT);
    advance(100000, 1);
    wait_calls(SEND, 3);
    /* An accepted fourth frame publishes the completed third-frame counters. */
    atomic_store(&send_result, XIAOTAI_OK);
    advance(100000, 1);
    const xiaotai_video_uplink_status_t status = wait_frames(1, 1);
    CHECK(status.stale == 1 && status.congested == 1 && status.send_failed == 1 && status.key_frames == 1);
    end();
}

static void test_failed_loan(void)
{
    begin(false, false, true, &stream);
    CHECK(request(12, true) == XIAOTAI_OK);
    wait_phase(XIAOTAI_VIDEO_RUNNING);
    atomic_store(&release_error, XIAOTAI_ERR_INVALID_STATE);
    advance(0, 1);
    wait_calls(CLOSE, 1);
    xiaotai_delay_ms(25);
    CHECK(count(RELEASE) == 2 && count(SEND) == 0 && count(OFF) == 0);
    xiaotai_video_status_t status;
    xiaotai_video_status(&status);
    CHECK(status.uplink.phase == XIAOTAI_VIDEO_STOPPING && status.uplink.error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(request(13, true) == XIAOTAI_OK);
    xiaotai_delay_ms(25);
    CHECK(count(OPEN) == 1 && count(POWER) == 1 && count(OFF) == 0);
    atomic_store(&release_error, XIAOTAI_OK);
    end();
    CHECK(!outstanding && count(RELEASE) == 3 && count(OFF) == 1);
}

int main(void)
{
    test_prepare();
    test_sequence();
    test_native_transform();
    test_admission_edges();
    test_failure();
    test_ai_freshness();
    test_pressure_and_close_error();
    test_legacy_quality_resize();
    test_no_frame_and_jpeg_bounds();
    test_send_results();
    test_failed_loan();
    return 0;
}
