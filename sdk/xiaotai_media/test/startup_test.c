// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/port/os.h"

enum { WAIT_MS = 3000 };
static xiaotai_signal_t s_open, s_started, s_notice;
static atomic_uint s_attempts, s_accepted, s_delivered;
static bool s_started_seen; /* Only accessed by the owner, between boot and reset. */
static bool s_early, s_fail_task, s_observe, s_expected_before_started;
static unsigned s_reject;
static xiaotai_err_t s_open_error;
typedef enum { NO_INPUT, SAVED_MUTE, DEFAULT_INPUT } capture_test_t;
static capture_test_t s_capture_test;
static atomic_uint s_reads, s_taps, s_volume;
static atomic_int s_gain;

static xiaotai_media_status_t media_status(void)
{
    xiaotai_media_status_t status;
    xiaotai_media_status(&status);
    return status;
}

static xiaotai_err_t speaker_open(void *unused)
{
    (void)unused;
    assert(xiaotai_signal_take(&s_open, WAIT_MS));
    return s_open_error;
}

static xiaotai_err_t speaker_close(void *unused)
{
    (void)unused;
    return XIAOTAI_OK;
}

static xiaotai_err_t set_volume(void *unused, uint8_t percent)
{
    (void)unused;
    atomic_store(&s_volume, percent);
    return XIAOTAI_OK;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t speaker = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 1},
        .open = speaker_open, .close = speaker_close, .set_volume = set_volume,
    };
    return index ? NULL : &speaker;
}

static xiaotai_err_t input_open(void *unused) { (void)unused; return XIAOTAI_OK; }

static xiaotai_err_t input_gain(void *unused, int8_t gain)
{
    (void)unused;
    atomic_store(&s_gain, gain);
    return XIAOTAI_OK;
}

static xiaotai_err_t input_read(void *unused, void *pcm, size_t capacity, size_t *size)
{
    (void)unused;
    if (!atomic_load(&s_reads)) {
        const bool saved = s_capture_test == SAVED_MUTE;
        const xiaotai_media_status_t status = media_status();
        assert(status.mic_muted == saved && status.speaker_muted == saved);
        assert(status.volume_percent == (saved ? 27 : 70));
        assert(atomic_load(&s_gain) == (saved ? 30 : 24));
    }
    memset(pcm, 1, capacity);
    *size = capacity;
    atomic_fetch_add(&s_reads, 1);
    xiaotai_delay_ms(1);
    return XIAOTAI_OK;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    static const xiaotai_audio_input_t microphone = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 1, .roles = {XIAOTAI_CHANNEL_MIC}},
        .info = {.gain_db = 24, .gain_min_db = 0, .gain_max_db = 36, .gain_step_db = 3},
        .open = input_open, .close = speaker_close, .read = input_read, .set_gain_db = input_gain,
    };
    return index || s_capture_test == NO_INPUT ? NULL : &microphone;
}

static void tap_pcm(void *ctx, const int16_t *pcm, size_t samples, uint32_t rate_hz, int64_t captured_us)
{
    (void)ctx;
    (void)pcm;
    (void)samples;
    (void)rate_hz;
    (void)captured_us;
    atomic_fetch_add(&s_taps, 1);
}

static void unmute(void *unused)
{
    (void)unused;
    assert(xiaotai_media_set_mic_muted(false) == XIAOTAI_OK);
}

xiaotai_err_t __real_xiaotai_event_emit(const char *, const xiaotai_args_t *, const void *, size_t);
xiaotai_err_t __wrap_xiaotai_event_emit(const char *name, const xiaotai_args_t *args, const void *data, size_t size)
{
    if (strcmp(name, XIAOTAI_EVENT_MEDIA_STARTUP))
        return __real_xiaotai_event_emit(name, args, data, size);
    assert(!args && !data && !size);
    if (atomic_fetch_add(&s_attempts, 1) < s_reject) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t err = __real_xiaotai_event_emit(name, args, data, size);
    if (err == XIAOTAI_OK) atomic_fetch_add(&s_accepted, 1);
    return err;
}

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *, xiaotai_task_t *);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (s_fail_task && (!strcmp(config->name, "xt_playout") || !strcmp(config->name, "xt_capture")))
        return XIAOTAI_ERR_NO_MEM;
    return __real_xiaotai_task_create(config, task);
}

static void wait_for(atomic_uint *counter, unsigned value)
{
    const int64_t deadline = xiaotai_time_us() + INT64_C(1000) * WAIT_MS;
    while (atomic_load(counter) < value && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(atomic_load(counter) >= value);
}

static void event(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner());
    const xiaotai_media_status_t status = media_status();
    if (!strcmp(event->name, XIAOTAI_EVENT_STARTED)) {
        assert(status.startup_complete == (s_early || s_fail_task));
        if (s_fail_task) assert(status.startup_error == XIAOTAI_ERR_NO_MEM);
        s_started_seen = true;
        xiaotai_signal_give(&s_started);
        return;
    }
    assert(status.startup_complete && status.startup_error == s_open_error);
    assert(status.ready == (s_open_error == XIAOTAI_OK));
    assert(!atomic_load(&s_delivered));
    assert(s_started_seen == !s_expected_before_started);
    atomic_fetch_add(&s_delivered, 1);
    xiaotai_signal_give(&s_notice);
}

static xiaotai_err_t observer_init(void)
{
    if (s_capture_test != NO_INPUT) {
        static const xiaotai_media_tap_t raw = {.name = "capture_probe", .flags = XIAOTAI_TAP_RAW, .pcm = tap_pcm};
        assert(xiaotai_media_tap_register(&raw) == XIAOTAI_OK);
        assert(xiaotai_media_tap_enable(&raw, true) == XIAOTAI_OK);
    }
    if (s_capture_test == SAVED_MUTE) {
        assert(xiaotai_media_set_volume(27) == XIAOTAI_OK);
        assert(xiaotai_media_set_speaker_muted(true) == XIAOTAI_OK);
        assert(xiaotai_media_set_mic_muted(true) == XIAOTAI_OK);
        assert(xiaotai_media_set_input_gain(31) == XIAOTAI_OK); /* Media rounds to 30. */
    }
    const xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_STARTED, event, NULL);
    return err == XIAOTAI_OK && s_observe ? xiaotai_event_subscribe(XIAOTAI_EVENT_MEDIA_STARTUP, event, NULL) : err;
}

static void observer_start(void)
{
    if (!s_early) return;
    xiaotai_signal_give(&s_open);
    wait_for(&s_accepted, 1); /* Keep startup on the owner until the early notice is actually queued. */
}

static void exercise(bool early, xiaotai_err_t error, unsigned reject, bool observe, bool fail_task)
{
    s_early = s_expected_before_started = early;
    s_observe = observe;
    s_fail_task = fail_task;
    s_open_error = error;
    s_reject = reject;
    s_started_seen = false;
    atomic_store(&s_attempts, 0);
    atomic_store(&s_accepted, 0);
    atomic_store(&s_delivered, 0);
    atomic_store(&s_reads, 0);
    atomic_store(&s_taps, 0);
    atomic_store(&s_volume, 0);
    atomic_store(&s_gain, 24);
    assert(xiaotai_signal_init(&s_open) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_started) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_notice) == XIAOTAI_OK);
    static const char *const needs_media[] = {"media", NULL};
    static const xiaotai_module_t media = {
        .name = "media", .init = xiaotai_engine_media_init, .start = xiaotai_engine_media_start,
    };
    static const xiaotai_module_t observer = {
        .name = "observer", .depends = needs_media, .init = observer_init, .start = observer_start,
    };
    static const xiaotai_module_t *const modules[] = {&media, &observer};
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 2}) ==
           XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_started, WAIT_MS));
    if (!early && !fail_task) xiaotai_signal_give(&s_open);
    if (fail_task) {
        assert(atomic_load(&s_attempts) == 0);
    } else if (reject == UINT32_MAX) {
        wait_for(&s_attempts, 1);
    } else {
        wait_for(&s_accepted, 1);
        if (observe) assert(xiaotai_signal_take(&s_notice, WAIT_MS));
        assert(atomic_load(&s_attempts) == reject + 1);
        assert(media_status().startup_complete && media_status().startup_error == error);
    }
    if (s_capture_test != NO_INPUT) {
        wait_for(&s_reads, 3);
        assert(!media_status().active);
        if (s_capture_test == SAVED_MUTE) {
            assert(media_status().mic_muted && !atomic_load(&s_taps));
            wait_for(&s_volume, 27);
            assert(atomic_load(&s_volume) == 27);
            assert(xiaotai_run(unmute, NULL) == XIAOTAI_OK);
        }
        wait_for(&s_taps, 1); /* Prove the raw tap works and is not merely disabled. */
    }
    xiaotai_media_reset(); /* Stops the producer even while a notice is still pending. */
    xiaotai_framework_reset();
    assert(!media_status().ready && !media_status().startup_complete);
    if (fail_task || !observe || reject == UINT32_MAX) assert(atomic_load(&s_delivered) == 0);
    else assert(atomic_load(&s_delivered) == 1);
    xiaotai_signal_deinit(&s_notice);
    xiaotai_signal_deinit(&s_started);
    xiaotai_signal_deinit(&s_open);
}

int main(void)
{
    exercise(true, XIAOTAI_OK, 0, true, false);
    exercise(false, XIAOTAI_OK, 1, true, false);
    exercise(false, XIAOTAI_ERR_NOT_SUPPORTED, 1, true, false);
    exercise(false, XIAOTAI_ERR_NOT_SUPPORTED, UINT32_MAX, true, false);
    exercise(false, XIAOTAI_OK, 0, false, false); /* Headless, no UI or startup subscriber. */
    exercise(false, XIAOTAI_OK, 0, true, true);
    exercise(false, XIAOTAI_OK, 0, true, false); /* Reset after failure may boot normally. */
    s_capture_test = SAVED_MUTE;
    exercise(false, XIAOTAI_OK, 0, true, false);
    s_capture_test = DEFAULT_INPUT;
    exercise(false, XIAOTAI_OK, 0, true, false); /* Nothing staged survives the reset. */
    puts("media startup: before/after STARTED, true failures, queue retry, reset, headless OK");
    return 0;
}
