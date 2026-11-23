// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#include "diag.h"
#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"
#include "xiaotai_os_host.h"

enum { WAIT_MS = 3000, GENERATION = 7, CLIP_SAMPLES = 800, MARKER = 1234 };
static xiaotai_signal_t s_in_write, s_release_write, s_owner, s_started, s_session_audio;
static atomic_uint s_marker_samples, s_started_count;
static atomic_int s_done_result;
static atomic_bool s_first_write, s_preempted_early;
static uint8_t s_tone[CLIP_SAMPLES * 2];
static bool s_hold, s_missing;
static xiaotai_err_t s_result;

bool __wrap_xiaotai_net_provisioning_boot(void) { return false; }

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    (void)index;
    return NULL;
}

static xiaotai_err_t write_pcm(void *self, void *pcm, size_t bytes)
{
    (void)self;
    const int16_t *samples = pcm;
    bool marker = false, network = false;
    for (size_t i = 0; i < bytes / sizeof(*samples); ++i) {
        if (samples[i] == MARKER) {
            atomic_fetch_add(&s_marker_samples, 1);
            marker = true;
        } else if (samples[i]) {
            network = true;
        }
    }
    if (marker && !atomic_exchange(&s_first_write, true)) {
        xiaotai_signal_give(&s_in_write);
        assert(xiaotai_signal_take(&s_release_write, WAIT_MS));
    }
    if (network) {
        if (media_diag_audio_active()) atomic_store(&s_preempted_early, true);
        xiaotai_signal_give(&s_session_audio);
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t speaker_lifecycle(void *self)
{
    (void)self;
    return XIAOTAI_OK;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t out = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 8000, .channels = 1},
        .open = speaker_lifecycle, .write = write_pcm, .close = speaker_lifecycle,
    };
    return index ? NULL : &out;
}

static xiaotai_err_t declare_audio(void)
{
    const xiaotai_media_requirement_t requirement = {
        .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink_burst_ms = 40,
    };
    if (s_missing) assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    return xiaotai_media_require(&requirement);
}

static void done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    atomic_store(&s_done_result, result);
    media_diag_diagnostic_end(false, id);
}

static void play(void *ctx)
{
    (void)ctx;
    static const xiaotai_sound_t clip = {
        .encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 8000, .channels = 1, .data = s_tone, .size = sizeof(s_tone),
    };
    const xiaotai_media_play_t request = {
        .clips = {{.sound = &clip}}, .clip_count = 1, .priority = XIAOTAI_PLAY_URGENT, .done = done,
    };
    uint32_t id;
    s_result = xiaotai_media_play_clips(&request, &id);
    if (s_result == XIAOTAI_OK) media_diag_diagnostic_begin(false, id);
    xiaotai_signal_give(&s_owner);
}

static void session_event(uint32_t generation, xiaotai_media_event_t type, xiaotai_err_t result, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && generation == GENERATION);
    assert(type == XIAOTAI_MEDIA_EVENT_STARTED && result == XIAOTAI_OK);
    assert(atomic_fetch_add(&s_started_count, 1) == 0);
    xiaotai_signal_give(&s_started);
}

static void start_call(void *ctx)
{
    (void)ctx;
    const xiaotai_media_session_t session = {
        .generation = GENERATION, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink_burst_ms = 40,
        .latency = {.start_ms = 20, .base_ms = 20, .min_ms = 20, .max_ms = 60},
        .event = session_event,
    };
    s_result = xiaotai_media_open(&session);
    xiaotai_signal_give(&s_owner);
}

static void end_call(void *ctx)
{
    (void)ctx;
    xiaotai_media_close(GENERATION);
    xiaotai_signal_give(&s_owner);
}

static unsigned no_stream_events(void)
{
    xiaotai_eventlog_entry_t events[16];
    const size_t count = xiaotai_eventlog_read(0, events, sizeof(events) / sizeof(*events));
    unsigned found = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(events[i].source, "media") && !strcmp(events[i].label, "playout.no_stream")) {
            assert(events[i].generation == GENERATION && events[i].value == 5000);
            ++found;
        }
    }
    return found;
}

static void check_no_stream(void)
{
    assert(xiaotai_run(start_call, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_owner, WAIT_MS) && s_result == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_started, WAIT_MS));
    xiaotai_os_host_advance_us(4000000);
    media_session_wake_playout();
    assert(!no_stream_events());
    xiaotai_os_host_advance_us(1200000);
    media_session_wake_playout();
    const int64_t until = xiaotai_time_us() + (int64_t)WAIT_MS * 1000;
    while (!no_stream_events() && xiaotai_time_us() < until) xiaotai_delay_ms(1);
    assert(no_stream_events() == 1);
    xiaotai_os_host_advance_us(6000000);
    media_session_wake_playout();
    xiaotai_delay_ms(5);
    assert(no_stream_events() == 1);
}

int main(int argc, char **argv)
{
    s_hold = argc != 2 || strcmp(argv[1], "default") != 0;
    s_missing = argc == 2 && strcmp(argv[1], "no-stream") == 0;
    static const xiaotai_media_param_t tuning[] = {{"diagnostics.keep_until_done", 1}};
    if (s_hold) assert(xiaotai_media_tune(tuning, 1) == XIAOTAI_OK);
    for (size_t i = 0; i < sizeof(s_tone); i += 2) {
        s_tone[i] = MARKER & 0xff;
        s_tone[i + 1] = MARKER >> 8;
    }
    xiaotai_signal_t *const signals[] = {&s_in_write, &s_release_write, &s_owner, &s_started, &s_session_audio};
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) assert(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    atomic_store(&s_done_result, XIAOTAI_ERR_NOT_FOUND);
    static const xiaotai_module_t media = {.name = "media", .init = xiaotai_engine_media_init, .start = xiaotai_engine_media_start};
    static const xiaotai_module_t fixture = {.name = "fixture", .init = declare_audio};
    static const xiaotai_module_t *const modules[] = {&media, &fixture};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 2}) == XIAOTAI_OK);
    xiaotai_media_status_t status;
    const int64_t until = xiaotai_time_us() + (int64_t)WAIT_MS * 1000;
    do {
        xiaotai_media_status(&status);
        if (!status.ready) xiaotai_delay_ms(1);
    } while (!status.ready && xiaotai_time_us() < until);
    assert(status.ready);
    if (s_missing) {
        check_no_stream();
        goto cleanup;
    }
    assert(xiaotai_run(play, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_owner, WAIT_MS) && s_result == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_in_write, WAIT_MS));

    assert(xiaotai_run(start_call, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_owner, WAIT_MS) && s_result == XIAOTAI_OK);
    static const uint8_t packet[160]; /* decoded value is distinct from MARKER */
    for (uint32_t i = 1; i <= 2; ++i) {
        assert(xiaotai_media_push(GENERATION, &(xiaotai_media_packet_t){
            .encoding = XIAOTAI_CODEC_G711A, .data = packet, .size = sizeof(packet), .timestamp_ms = i * 20,
        }));
    }
    assert(!atomic_load(&s_started_count));
    xiaotai_signal_give(&s_release_write);
    assert(xiaotai_signal_take(&s_started, WAIT_MS));
    assert(xiaotai_signal_take(&s_session_audio, WAIT_MS));
    if (s_hold) {
        assert(!atomic_load(&s_preempted_early));
        assert(atomic_load(&s_marker_samples) == CLIP_SAMPLES);
        assert(atomic_load(&s_done_result) == XIAOTAI_OK);
    } else {
        assert(atomic_load(&s_marker_samples) < CLIP_SAMPLES);
        assert(atomic_load(&s_done_result) == XIAOTAI_ERR_INVALID_STATE);
    }
cleanup:
    assert(xiaotai_run(end_call, NULL) == XIAOTAI_OK && xiaotai_signal_take(&s_owner, WAIT_MS));
    xiaotai_media_reset();
    xiaotai_framework_reset();
    if (s_missing) xiaotai_eventlog_reset();
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) xiaotai_signal_deinit(signals[i]);
    return 0;
}
