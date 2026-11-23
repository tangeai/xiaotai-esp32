// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_file_playback.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_s3_audio_controls
// Replaces legacy: tests/regression/test_ai_downlink_start.py
// Replaces legacy: tests/regression/test_ai_audio_drain.py
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"
#include "xiaotai_os_host.h"

#define WAIT_MS 3000U

static const xiaotai_media_requirement_t s_need = {
    .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
    .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
    .downlink_burst_ms = 1000,
    .preroll_ms = 20000,
};

static bool s_provisioning, s_reject_input, s_idle_policy;
static atomic_bool s_opened, s_powered, s_hold_enable;
static atomic_uint s_opens, s_closes, s_disables, s_input_opens, s_input_closes, s_input_reads;
static atomic_uint s_flush_frames;
static atomic_uint s_hw_volume, s_disable_attempts;
static atomic_bool s_hold_disable, s_reject_disable;
static _Atomic(xiaotai_err_t) s_enable_result;
static xiaotai_signal_t s_owner_done, s_event_seen, s_enable_entered, s_enable_release;
static xiaotai_signal_t s_disabled, s_closed, s_output_free;
static xiaotai_signal_t s_disable_entered, s_disable_release;
static bool s_test_codec_mute, s_mute_after_accept;
static atomic_bool s_reject_codec_mute, s_hold_codec_mute, s_hold_output_write;
static xiaotai_signal_t s_mute_entered, s_mute_release, s_codec_refused;
static xiaotai_signal_t s_write_entered, s_write_release;

typedef struct {
    xiaotai_signal_t done, failed;
    atomic_uint completions, errors;
    uint32_t completed_id;
} file_caller_t;

static file_caller_t *s_file_caller;

enum { BURST_WINDOW_US = 15000, BURST_PAYLOAD_BYTES = 20 };
static _Thread_local int64_t s_rx_arrival_us;
static struct {
    uint32_t generation, packets, first_pts;
    int64_t first_arrival_us;
    atomic_uint consumed;
    atomic_bool observe;
} s_burst;
static xiaotai_signal_t s_burst_consumed;
static xiaotai_signal_t s_copy_entered, s_copy_release;
static _Thread_local const void *s_copy_source;
static atomic_bool s_pause_writer_count;
static xiaotai_signal_t s_writer_count_entered, s_writer_count_release;

unsigned __real_media_rx_writers(void);
unsigned __wrap_media_rx_writers(void);
unsigned __wrap_media_rx_writers(void)
{
    if (atomic_exchange(&s_pause_writer_count, false)) {
        xiaotai_signal_give(&s_writer_count_entered);
        assert(xiaotai_signal_take(&s_writer_count_release, WAIT_MS));
    }
    return __real_media_rx_writers();
}

void *__real_memcpy(void *destination, const void *source, size_t size);
void *__wrap_memcpy(void *destination, const void *source, size_t size);
void *__wrap_memcpy(void *destination, const void *source, size_t size)
{
    if (s_copy_source && source == s_copy_source) {
        s_copy_source = NULL;
        xiaotai_signal_give(&s_copy_entered);
        assert(xiaotai_signal_take(&s_copy_release, WAIT_MS));
    }
    return __real_memcpy(destination, source, size);
}

bool __wrap_xiaotai_net_provisioning_boot(void);
bool __wrap_xiaotai_net_provisioning_boot(void) { return s_provisioning; }

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    return s_rx_arrival_us ? s_rx_arrival_us : __real_xiaotai_time_us();
}

static uint8_t burst_tag(uint32_t generation, uint32_t pts, unsigned byte)
{
    return (uint8_t)(generation * 43U + pts * 17U + byte * 19U);
}

bool __real_media_downlink_pop_tagged(xiaotai_downlink_t *ring, void *data, size_t capacity,
                                      xiaotai_downlink_packet_t *packet, uint16_t *stream,
                                      uint8_t *format, uint16_t *samples);
bool __wrap_media_downlink_pop_tagged(xiaotai_downlink_t *ring, void *data, size_t capacity,
                                      xiaotai_downlink_packet_t *packet, uint16_t *stream,
                                      uint8_t *format, uint16_t *samples);
bool __wrap_media_downlink_pop_tagged(xiaotai_downlink_t *ring, void *data, size_t capacity,
                                      xiaotai_downlink_packet_t *packet, uint16_t *stream,
                                      uint8_t *format, uint16_t *samples)
{
    const bool popped = __real_media_downlink_pop_tagged(ring, data, capacity, packet, stream, format, samples);
    if (popped && atomic_load(&s_burst.observe)) {
        const unsigned i = atomic_load(&s_burst.consumed);
        assert(i < s_burst.packets && packet->generation == s_burst.generation);
        assert(packet->timestamp_ms == s_burst.first_pts + i * 20U);
        assert(packet->received_us == s_burst.first_arrival_us + (int64_t)i * (BURST_WINDOW_US / s_burst.packets));
        assert(packet->size == BURST_PAYLOAD_BYTES && packet->duration_us == 2500U);
        assert(*stream == 0 && *samples == BURST_PAYLOAD_BYTES);
        for (unsigned b = 0; b < BURST_PAYLOAD_BYTES; ++b)
            assert(((const uint8_t *)data)[b] == burst_tag(packet->generation, packet->timestamp_ms, b));
        atomic_store(&s_burst.consumed, i + 1U);
        if (i + 1U == s_burst.packets) xiaotai_signal_give(&s_burst_consumed);
    }
    return popped;
}

static xiaotai_err_t output_open(void *self)
{
    (void)self;
    assert(!atomic_exchange(&s_opened, true));
    assert(!atomic_load(&s_powered));
    atomic_fetch_add(&s_opens, 1);
    return XIAOTAI_OK;
}

static xiaotai_err_t output_enable(void *self, bool enabled)
{
    (void)self;
    assert(!xiaotai_on_owner() && atomic_load(&s_opened));
    if (enabled) {
        if (atomic_exchange(&s_hold_enable, false)) {
            xiaotai_signal_give(&s_enable_entered);
            assert(xiaotai_signal_take(&s_enable_release, WAIT_MS));
        }
        const xiaotai_err_t result = atomic_exchange(&s_enable_result, XIAOTAI_OK);
        if (result != XIAOTAI_OK) return result;
    } else {
        atomic_fetch_add(&s_disable_attempts, 1);
        if (atomic_exchange(&s_hold_disable, false)) {
            xiaotai_signal_give(&s_disable_entered);
            assert(xiaotai_signal_take(&s_disable_release, WAIT_MS));
        }
        if (atomic_load(&s_reject_disable)) return XIAOTAI_ERR_TIMEOUT;
    }
    atomic_store(&s_powered, enabled);
    if (!enabled) {
        atomic_fetch_add(&s_disables, 1);
        xiaotai_signal_give(&s_disabled);
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t output_volume(void *self, uint8_t percent)
{
    (void)self;
    assert(atomic_load(&s_opened) && percent <= 100);
    atomic_store(&s_hw_volume, percent);
    return XIAOTAI_OK;
}

static xiaotai_err_t output_mute(void *self, bool muted)
{
    (void)self;
    assert(!xiaotai_on_owner() && atomic_load(&s_opened));
    if (muted && atomic_exchange(&s_hold_codec_mute, false)) {
        xiaotai_signal_give(&s_mute_entered);
        assert(xiaotai_signal_take(&s_mute_release, WAIT_MS));
    }
    return muted && atomic_exchange(&s_reject_codec_mute, false) ? XIAOTAI_FAIL : XIAOTAI_OK;
}

static xiaotai_err_t output_write(void *self, void *data, size_t size)
{
    (void)self;
    assert(!xiaotai_on_owner() && atomic_load(&s_opened));
    assert(data && size && !(size % sizeof(int16_t)));
    if (atomic_exchange(&s_hold_output_write, false)) {
        xiaotai_signal_give(&s_write_entered);
        assert(xiaotai_signal_take(&s_write_release, WAIT_MS));
    }
    return XIAOTAI_OK;
}

static size_t output_flush_frames(void *self)
{
    (void)self;
    return atomic_load(&s_flush_frames);
}

static xiaotai_err_t output_close(void *self)
{
    (void)self;
    assert(atomic_exchange(&s_opened, false));
    atomic_store(&s_powered, false);
    atomic_fetch_add(&s_closes, 1);
    xiaotai_signal_give(&s_closed);
    return XIAOTAI_OK;
}

static xiaotai_err_t input_open(void *self)
{
    (void)self;
    assert(atomic_load(&s_opened));
    atomic_fetch_add(&s_input_opens, 1);
    return XIAOTAI_ERR_TIMEOUT;
}

static xiaotai_err_t input_read(void *self, void *data, size_t capacity, size_t *size)
{
    (void)self;
    (void)data;
    (void)capacity;
    (void)size;
    atomic_fetch_add(&s_input_reads, 1);
    return XIAOTAI_ERR_INVALID_STATE;
}

static xiaotai_err_t input_close(void *self)
{
    (void)self;
    atomic_fetch_add(&s_input_closes, 1);
    return XIAOTAI_OK;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    static const xiaotai_audio_input_t input = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 8000, .channels = 1},
        .open = input_open, .read = input_read, .close = input_close,
    };
    return index == 0 && s_reject_input ? &input : NULL;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t output = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 8000, .channels = 1},
        .open = output_open, .write = output_write, .set_volume = output_volume,
        .set_muted = output_mute, .set_enabled = output_enable,
        .flush_frames = output_flush_frames, .close = output_close,
    };
    return index ? NULL : &output;
}

typedef struct {
    uint32_t generation;
    xiaotai_media_event_t kind;
    xiaotai_err_t result;
} event_t;

static event_t s_events[16];
static atomic_uint s_event_count, s_free_count;

static void session_event(uint32_t generation, xiaotai_media_event_t kind, xiaotai_err_t result, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == s_events);
    const unsigned i = atomic_load(&s_event_count);
    assert(i < sizeof(s_events) / sizeof(*s_events));
    s_events[i] = (event_t){generation, kind, result};
    atomic_store(&s_event_count, i + 1);
    xiaotai_signal_give(&s_event_seen);
}

static void expect_event(unsigned i, uint32_t generation, xiaotai_media_event_t kind, xiaotai_err_t result)
{
    while (atomic_load(&s_event_count) <= i) assert(xiaotai_signal_take(&s_event_seen, WAIT_MS));
    assert(s_events[i].generation == generation);
    assert(s_events[i].kind == kind && s_events[i].result == result);
}

static void output_free(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    assert(xiaotai_on_owner() && !atomic_load(&s_powered));
    atomic_fetch_add(&s_free_count, 1);
    xiaotai_signal_give(&s_output_free);
}

static void expect_output_free(unsigned count)
{
    while (atomic_load(&s_free_count) < count) assert(xiaotai_signal_take(&s_output_free, WAIT_MS));
    assert(atomic_load(&s_free_count) == count);
}

typedef enum { OWNER_OPEN, OWNER_ACTIVATE, OWNER_DRAIN, OWNER_FLUSH, OWNER_CLOSE } operation_t;

typedef struct {
    operation_t op;
    const xiaotai_media_session_t *session;
    uint32_t generation, timeout_ms;
    xiaotai_err_t result;
} call_t;

static void owner_call(void *ctx)
{
    assert(xiaotai_on_owner());
    call_t *call = ctx;
    switch (call->op) {
        case OWNER_OPEN: call->result = xiaotai_media_open(call->session); break;
        case OWNER_ACTIVATE: call->result = xiaotai_media_activate(call->generation); break;
        case OWNER_DRAIN: call->result = xiaotai_media_drain(call->generation, call->timeout_ms); break;
        case OWNER_FLUSH: call->result = xiaotai_media_flush(call->generation); break;
        case OWNER_CLOSE: xiaotai_media_close(call->generation); break;
    }
    xiaotai_signal_give(&s_owner_done);
}

static xiaotai_err_t invoke(call_t call)
{
    assert(xiaotai_run(owner_call, &call) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_owner_done, WAIT_MS));
    return call.result;
}

static xiaotai_err_t perform(operation_t op, uint32_t generation)
{
    return invoke((call_t){.op = op, .generation = generation, .timeout_ms = 5000});
}

static xiaotai_media_status_t status(void)
{
    xiaotai_media_status_t view;
    xiaotai_media_status(&view);
    return view;
}

static void file_done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    file_caller_t *caller = ctx;
    assert(xiaotai_on_owner() && caller == s_file_caller && id && result == XIAOTAI_OK);
    assert(atomic_fetch_add(&caller->completions, 1U) == 0);
    caller->completed_id = id;
    xiaotai_signal_give(&caller->done);
}

static void file_output_error(const xiaotai_event_t *event, void *ctx)
{
    file_caller_t *caller = ctx;
    assert(xiaotai_on_owner() && caller == s_file_caller && atomic_load(&caller->completions) == 1U);
    assert(!strcmp(event->args.text, "power") && event->args.value == XIAOTAI_ERR_TIMEOUT);
    assert(status().output_error == XIAOTAI_ERR_TIMEOUT);
    assert(atomic_fetch_add(&caller->errors, 1U) == 0);
    xiaotai_signal_give(&caller->failed);
}

static void mute_error(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner() && !strcmp(event->args.text, "mute") && event->args.value == XIAOTAI_FAIL);
    xiaotai_signal_give(&s_codec_refused);
}

static xiaotai_err_t require_audio(void)
{
    if (s_mute_after_accept) {
        static const xiaotai_media_param_t apply_gate[] = {{"output.mute_after_accept", 1}};
        const xiaotai_err_t tuned = xiaotai_media_tune(apply_gate, sizeof(apply_gate) / sizeof(*apply_gate));
        if (tuned != XIAOTAI_OK) return tuned;
    }
    if (s_idle_policy) {
        static const xiaotai_media_param_t idle[] = {{"power.idle_close_ms", 2}, {"downlink.alaw_max_packet_samples", 750}};
        const xiaotai_err_t tuned = xiaotai_media_tune(idle, sizeof(idle) / sizeof(*idle));
        if (tuned != XIAOTAI_OK) return tuned;
    }
    xiaotai_err_t err = xiaotai_media_require(&s_need);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_event_subscribe(XIAOTAI_EVENT_MEDIA_OUTPUT_FREE, output_free, NULL);
    if (err == XIAOTAI_OK && s_file_caller)
        err = xiaotai_event_subscribe(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, file_output_error, s_file_caller);
    if (err == XIAOTAI_OK && s_test_codec_mute)
        err = xiaotai_event_subscribe(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, mute_error, NULL);
    return err;
}

static void boot_framework(bool provisioning)
{
    static const char *const deps[] = {"media", NULL};
    static const xiaotai_module_t engine = {
        .name = "media", .init = xiaotai_engine_media_init, .start = xiaotai_engine_media_start,
    };
    static const xiaotai_module_t fixture = {.name = "fixture", .depends = deps, .init = require_audio};
    static const xiaotai_module_t *const modules[] = {&engine, &fixture};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 2, .queue_depth = 8};
    s_provisioning = provisioning;
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
}

static void boot(bool provisioning)
{
    const unsigned opens = atomic_load(&s_opens);
    boot_framework(provisioning);
    const int64_t until_us = xiaotai_time_us() + (int64_t)WAIT_MS * 1000;
    while (!status().ready && xiaotai_time_us() < until_us) xiaotai_delay_ms(1);
    assert(status().ready && status().generation == 0);
    assert(atomic_load(&s_opens) == opens + 1 && !atomic_load(&s_powered));
}

static void reset(void)
{
    const unsigned closes = atomic_load(&s_closes);
    xiaotai_media_reset();
    xiaotai_framework_reset();
    assert(atomic_load(&s_closes) == closes + 1);
    assert(!atomic_load(&s_opened) && !status().ready);
}

static void check_file_output_error(void)
{
    /* Same 1001-frame 8 kHz mono WAV as the legacy file playback oracle. */
    enum { HEADER = 44, FRAMES = 1001 };
    uint8_t wav[HEADER + FRAMES * 2] = {
        'R', 'I', 'F', 'F', 0xf6, 0x07, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0x40, 0x1f, 0, 0,
        0x80, 0x3e, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0xd2, 0x07, 0, 0,
    };
    for (size_t i = 0; i < FRAMES; ++i) {
        const uint16_t sample = (uint16_t)((int)((i * 139U) % 16000U) - 8000);
        wav[HEADER + i * 2] = (uint8_t)sample;
        wav[HEADER + i * 2 + 1] = (uint8_t)(sample >> 8);
    }
    const xiaotai_sound_t sound = {.encoding = XIAOTAI_CODEC_PCM16, .container = XIAOTAI_SOUND_WAV,
        .data = wav, .size = sizeof(wav)};
    file_caller_t caller = {0};
    assert(xiaotai_signal_init(&caller.done) == XIAOTAI_OK && xiaotai_signal_init(&caller.failed) == XIAOTAI_OK);
    s_file_caller = &caller;
    boot(false);
    assert(!status().playing && status().output_error == XIAOTAI_OK);
    atomic_store(&s_hold_disable, true);
    atomic_store(&s_reject_disable, true);
    const xiaotai_media_play_t request = {.clips = {{.sound = &sound}}, .clip_count = 1,
        .priority = XIAOTAI_PLAY_NORMAL, .done = file_done, .ctx = &caller};
    uint32_t id = 0;
    assert(xiaotai_media_play_clips(&request, &id) == XIAOTAI_OK && id);
    assert(xiaotai_signal_take(&s_disable_entered, WAIT_MS));
    assert(xiaotai_signal_take(&caller.done, WAIT_MS) && caller.completed_id == id);
    assert(atomic_load(&s_powered) && !atomic_load(&caller.errors) && status().output_error == XIAOTAI_OK);
    xiaotai_signal_give(&s_disable_release);
    assert(xiaotai_signal_take(&caller.failed, WAIT_MS));
    assert(atomic_load(&s_disable_attempts) == 1U && atomic_load(&s_powered));
    reset();
    assert(atomic_load(&caller.errors) == 1U && atomic_load(&caller.completions) == 1U);
    s_file_caller = NULL;
    xiaotai_signal_deinit(&caller.done);
    xiaotai_signal_deinit(&caller.failed);
}

static bool receive_packet(uint32_t generation, size_t size, uint32_t timestamp)
{
    static const uint8_t packet[1600];
    return xiaotai_media_push(generation, &(xiaotai_media_packet_t){
        .encoding = XIAOTAI_CODEC_G711A, .data = packet, .size = size, .timestamp_ms = timestamp,
    });
}

static void make_pending_packet(xiaotai_downlink_t *downlink, uint32_t generation)
{
    assert(receive_packet(generation, 160, 12));
    assert(xiaotai_downlink_count(downlink) == 1);
}

static void await_power_off_after(unsigned previous_disables)
{
    while (atomic_load(&s_disables) == previous_disables)
        assert(xiaotai_signal_take(&s_disabled, WAIT_MS));
    assert(!atomic_load(&s_powered));
}

static void check_open_errors(const xiaotai_media_session_t *valid)
{
    assert(invoke((call_t){.op = OWNER_OPEN}) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_media_session_t invalid = *valid;
    call_t call = {.op = OWNER_OPEN, .session = &invalid};
    invalid.generation = 0;
    assert(invoke(call) == XIAOTAI_ERR_INVALID_ARG);
    invalid = *valid;
    invalid.downlink = (xiaotai_media_format_t)XIAOTAI_MEDIA_FORMAT_NONE;
    assert(invoke(call) == XIAOTAI_ERR_INVALID_ARG);
    invalid.uplink = s_need.uplink;
    assert(invoke(call) == XIAOTAI_ERR_INVALID_ARG); /* No sink. */
    invalid = *valid;
    invalid.downlink.encoding = XIAOTAI_CODEC_JPEG;
    assert(invoke(call) == XIAOTAI_ERR_INVALID_ARG);
    invalid = *valid;
    invalid.downlink = (xiaotai_media_format_t)XIAOTAI_MEDIA_FORMAT_OPUS_16K;
    assert(invoke(call) == XIAOTAI_ERR_NOT_SUPPORTED);
    invalid = *valid;
    invalid.downlink_burst_ms++;
    assert(invoke(call) == XIAOTAI_ERR_INVALID_SIZE);
    invalid = *valid;
    invalid.preroll_token = 97;
    assert(invoke(call) == XIAOTAI_ERR_INVALID_STATE);
    assert(status().generation == 0 && !atomic_load(&s_powered));
}

static void check_failed_start(uint32_t generation)
{
    const unsigned opens = atomic_load(&s_opens), closes = atomic_load(&s_closes);
    const unsigned events = atomic_load(&s_event_count), free_count = atomic_load(&s_free_count);
    s_reject_input = true;
    boot_framework(false);
    while (atomic_load(&s_closes) == closes) assert(xiaotai_signal_take(&s_closed, WAIT_MS));
    assert(atomic_load(&s_opens) == opens + 1 && atomic_load(&s_closes) == closes + 1);
    assert(atomic_load(&s_input_opens) == 1);
    assert(!atomic_load(&s_opened) && !atomic_load(&s_powered));
    assert(!status().ready && status().generation == 0);
    assert(!xiaotai_media_can_receive(&s_need.downlink) && !xiaotai_media_downlink());
    const xiaotai_media_session_t session = {
        .generation = generation, .downlink = s_need.downlink, .downlink_burst_ms = 1000,
        .event = session_event, .ctx = s_events,
    };
    assert(invoke((call_t){.op = OWNER_OPEN, .session = &session}) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_media_reset();
    xiaotai_framework_reset();
    assert(atomic_load(&s_closes) == closes + 1 && !atomic_load(&s_input_closes) && !atomic_load(&s_input_reads));
    assert(!status().ready && atomic_load(&s_event_count) == events && atomic_load(&s_free_count) == free_count);
    s_reject_input = false;
}

static void check_idle_reopen(void)
{
    s_idle_policy = true;
    const unsigned closes = atomic_load(&s_closes);
    const unsigned opens = atomic_load(&s_opens);
    boot_framework(false);
    while (atomic_load(&s_closes) == closes) assert(xiaotai_signal_take(&s_closed, WAIT_MS));
    assert(!atomic_load(&s_opened) && status().ready);
    const unsigned prior_applied = atomic_load(&s_hw_volume);
    assert(xiaotai_media_set_volume(37) == XIAOTAI_OK);
    xiaotai_delay_ms(10);
    assert(!atomic_load(&s_opened) && atomic_load(&s_hw_volume) == prior_applied);
    const xiaotai_media_session_t session = {.generation = 20, .downlink = s_need.downlink,
        .event = session_event, .ctx = s_events};
    const unsigned event = atomic_load(&s_event_count);
    assert(invoke((call_t){.op = OWNER_OPEN, .session = &session}) == XIAOTAI_OK);
    expect_event(event, 20, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);
    assert(atomic_load(&s_opens) == opens + 2 && atomic_load(&s_hw_volume) == 37);
    assert(receive_packet(20, 750, 10) && !receive_packet(20, 751, 110));
    xiaotai_media_stats_t stats;
    xiaotai_media_stats(&stats);
    assert(stats.rx_packet_max_bytes == 751 && stats.rx_invalid == 1);
    const unsigned attempts = atomic_load(&s_disable_attempts);
    atomic_store(&s_reject_disable, true);
    assert(perform(OWNER_CLOSE, 20) == XIAOTAI_OK);
    const int64_t until = xiaotai_time_us() + WAIT_MS * 1000U;
    while (status().output_error == XIAOTAI_OK && xiaotai_time_us() < until) xiaotai_delay_ms(1);
    assert(status().output_error == XIAOTAI_ERR_TIMEOUT);
    xiaotai_delay_ms(50);
    assert(atomic_load(&s_disable_attempts) == attempts + 1U);
    assert(atomic_load(&s_opened)); /* The failed driver operation is not retried. */
    atomic_store(&s_reject_disable, false);
    reset();
    s_idle_policy = false;
}

static void push_burst(uint32_t generation, uint32_t packets, uint32_t first_pts)
{
    s_burst.generation = generation;
    s_burst.packets = packets;
    s_burst.first_pts = first_pts;
    s_burst.first_arrival_us = xiaotai_time_us();
    atomic_store(&s_burst.consumed, 0);
    atomic_store(&s_burst.observe, true);
    uint8_t payload[BURST_PAYLOAD_BYTES];
    for (uint32_t i = 0; i < packets; ++i) {
        const uint32_t pts = first_pts + i * 20U;
        for (unsigned b = 0; b < sizeof(payload); ++b) payload[b] = burst_tag(generation, pts, b);
        s_rx_arrival_us = s_burst.first_arrival_us + (int64_t)i * (BURST_WINDOW_US / packets);
        assert(xiaotai_media_push(generation, &(xiaotai_media_packet_t){
            .encoding = XIAOTAI_CODEC_G711A, .data = payload, .size = sizeof(payload), .timestamp_ms = pts,
        }));
    }
    s_rx_arrival_us = 0;
}

static void finish_burst(void)
{
    assert(xiaotai_signal_take(&s_burst_consumed, WAIT_MS));
    assert(atomic_load(&s_burst.consumed) == s_burst.packets);
    atomic_store(&s_burst.observe, false);
}

static void check_ai_burst_order(void)
{
    const uint32_t counts[] = {20, 40, 50, 64};
    boot(false);
    xiaotai_downlink_t *const ring = xiaotai_media_downlink();
    assert(ring);
    for (unsigned i = 0; i < sizeof(counts) / sizeof(*counts); ++i) {
        const uint32_t generation = 30 + i;
        const unsigned event = atomic_load(&s_event_count), free_count = atomic_load(&s_free_count);
        const xiaotai_media_session_t session = {
            .generation = generation, .downlink = s_need.downlink, .downlink_burst_ms = 1000,
            .flags = XIAOTAI_MEDIA_HOLD, .event = session_event, .ctx = s_events,
        };
        assert(invoke((call_t){.op = OWNER_OPEN, .session = &session}) == XIAOTAI_OK);
        assert(!receive_packet(generation + 1U, 20, 0));
        push_burst(generation, counts[i], 0);
        assert(xiaotai_downlink_count(ring) == counts[i] && !atomic_load(&s_burst.consumed));
        assert(!atomic_load(&s_powered) && atomic_load(&s_event_count) == event);
        assert(perform(OWNER_ACTIVATE, generation) == XIAOTAI_OK);
        expect_event(event, generation, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);
        finish_burst();
        for (uint32_t utterance = 1; utterance <= 3; ++utterance) {
            xiaotai_os_host_advance_us(1000000);
            push_burst(generation, counts[i], 2000U * utterance);
            finish_burst();
        }
        xiaotai_media_stats_t stats;
        xiaotai_media_stats(&stats);
        assert(stats.rx_before_active == counts[i] && stats.rx_received == 4U * counts[i]);
        assert(stats.rx_stale == 1 && stats.rx_overflow == 0 && xiaotai_downlink_count(ring) == 0);
        (void)perform(OWNER_CLOSE, generation);
        expect_output_free(free_count + 1U);
    }
    reset();
}

static void check_armed_close_and_capacity(void)
{
    boot(false);
    xiaotai_downlink_t *const ring = xiaotai_media_downlink();
    xiaotai_media_session_t session = {
        .generation = 48, .downlink = s_need.downlink, .downlink_burst_ms = 1000,
        .flags = XIAOTAI_MEDIA_HOLD, .event = session_event, .ctx = s_events,
    };
    const call_t open = {.op = OWNER_OPEN, .session = &session};
    const unsigned events = atomic_load(&s_event_count), free_count = atomic_load(&s_free_count);
    assert(invoke(open) == XIAOTAI_OK);
    for (unsigned i = 0; i < 64; ++i) assert(receive_packet(session.generation, BURST_PAYLOAD_BYTES, i * 20U));
    const bool accepted_65 = receive_packet(session.generation, BURST_PAYLOAD_BYTES, 64U * 20U);
    printf("AI HOLD legacy slot comparison: packet 65 accepted=%u, held=%zu\n",
           (unsigned)accepted_65, xiaotai_downlink_count(ring));
    assert(xiaotai_downlink_count(ring) == 64U + (unsigned)accepted_65);
    assert(!atomic_load(&s_powered) && atomic_load(&s_event_count) == events);
    (void)perform(OWNER_CLOSE, session.generation);
    expect_output_free(free_count + 1U);
    assert(status().generation == 0 && !xiaotai_downlink_count(ring) && !atomic_load(&s_powered));
    assert(!receive_packet(session.generation, BURST_PAYLOAD_BYTES, 0));

    ++session.generation;
    assert(invoke(open) == XIAOTAI_OK);
    const unsigned packets = (unsigned)(xiaotai_downlink_capacity(ring) / XIAOTAI_DOWNLINK_ENTRY_BYTES(160U));
    assert(packets == 61U);
    for (unsigned i = 0; i < packets; ++i) assert(receive_packet(session.generation, 160, i * 20U));
    assert(!receive_packet(session.generation, 160, packets * 20U));
    xiaotai_media_stats_t stats;
    xiaotai_media_stats(&stats);
    assert(stats.rx_overflow == 1 && stats.rx_received == packets && xiaotai_downlink_count(ring) == packets);
    assert(xiaotai_downlink_queued_us(ring) == packets * 20000U && atomic_load(&s_event_count) == events);
    (void)perform(OWNER_CLOSE, session.generation);
    expect_output_free(free_count + 2U);
    assert(!xiaotai_downlink_count(ring) && !atomic_load(&s_powered));
    puts("AI HOLD: true bound rejects newest; unactivated close releases slots, never powers the speaker");
    reset();
}

typedef struct {
    uint32_t generation;
    bool accepted;
} paused_packet_t;

static void *push_paused_packet(void *ctx)
{
    paused_packet_t *push = ctx;
    const uint8_t payload[BURST_PAYLOAD_BYTES] = {0x73};
    s_copy_source = payload;
    push->accepted = xiaotai_media_push(push->generation, &(xiaotai_media_packet_t){
        .encoding = XIAOTAI_CODEC_G711A, .data = payload, .size = sizeof(payload), .timestamp_ms = 20,
    });
    assert(!s_copy_source);
    return NULL;
}

static void check_revoke_during_copy(void)
{
    boot(false);
    xiaotai_downlink_t *const ring = xiaotai_media_downlink();
    xiaotai_media_session_t session = {
        .generation = 40, .downlink = s_need.downlink, .downlink_burst_ms = 1000,
        .flags = XIAOTAI_MEDIA_HOLD, .event = session_event, .ctx = s_events,
    };
    const call_t open = {.op = OWNER_OPEN, .session = &session};
    assert(invoke(open) == XIAOTAI_OK);
    paused_packet_t paused = {.generation = session.generation};
    pthread_t push;
    assert(pthread_create(&push, NULL, push_paused_packet, &paused) == 0);
    assert(xiaotai_signal_take(&s_copy_entered, WAIT_MS));
    assert(xiaotai_downlink_count(ring) == 0 && !atomic_load(&s_powered));
    const unsigned free_count = atomic_load(&s_free_count);
    (void)perform(OWNER_CLOSE, session.generation);
    expect_output_free(free_count + 1U);
    ++session.generation;
    assert(invoke(open) == XIAOTAI_OK);
    xiaotai_signal_give(&s_copy_release);
    assert(pthread_join(push, NULL) == 0);
    assert(!paused.accepted && xiaotai_downlink_count(ring) == 0 && !atomic_load(&s_powered));
    assert(!receive_packet(paused.generation, BURST_PAYLOAD_BYTES, 0));
    const unsigned event = atomic_load(&s_event_count);
    push_burst(session.generation, 20, 0);
    assert(perform(OWNER_ACTIVATE, session.generation) == XIAOTAI_OK);
    expect_event(event, session.generation, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);
    finish_burst();
    assert(xiaotai_downlink_count(ring) == 0);
    (void)perform(OWNER_CLOSE, session.generation);
    expect_output_free(free_count + 2U);
    reset();
}

static void check_drain_of_admitted_packet(void)
{
    boot(false);
    const xiaotai_media_session_t session = {
        .generation = 42, .downlink = s_need.downlink, .downlink_burst_ms = 1000,
        .event = session_event, .ctx = s_events,
    };
    const unsigned event = atomic_load(&s_event_count), free_count = atomic_load(&s_free_count);
    xiaotai_downlink_t *const ring = xiaotai_media_downlink();
    assert(invoke((call_t){.op = OWNER_OPEN, .session = &session}) == XIAOTAI_OK);
    expect_event(event, session.generation, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);

    paused_packet_t admitted = {.generation = session.generation};
    pthread_t push;
    assert(pthread_create(&push, NULL, push_paused_packet, &admitted) == 0);
    assert(xiaotai_signal_take(&s_copy_entered, WAIT_MS));
    atomic_store(&s_pause_writer_count, true);
    assert(perform(OWNER_DRAIN, session.generation) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_writer_count_entered, WAIT_MS));
    assert(!receive_packet(session.generation, BURST_PAYLOAD_BYTES, 40));
    assert(atomic_load(&s_event_count) == event + 1U && xiaotai_downlink_count(ring) == 0);

    /* Publish the admitted packet after playout sampled the empty ring. */
    xiaotai_signal_give(&s_copy_release);
    assert(pthread_join(push, NULL) == 0 && admitted.accepted);
    assert(xiaotai_downlink_count(ring) == 1);
    xiaotai_signal_give(&s_writer_count_release);
    expect_event(event + 1U, session.generation, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK);

    xiaotai_media_stats_t stats;
    xiaotai_media_stats(&stats);
    assert(xiaotai_downlink_count(ring) == 0);
    assert(stats.rx_received == 1 && stats.rx_played == 1 && stats.rx_stale == 1);
    assert(stats.pcm_written == BURST_PAYLOAD_BYTES);
    (void)perform(OWNER_CLOSE, session.generation);
    expect_output_free(free_count + 1U);
    reset();
}

static void check_refused_mute(bool after_accept)
{
    enum { GEN = 19, PACKETS = 20, BASE_SAMPLES = 160 };
    s_test_codec_mute = true;
    s_mute_after_accept = after_accept;
    boot(false);
    const xiaotai_media_session_t session = {.generation = GEN, .downlink = s_need.downlink,
        .downlink_burst_ms = 1000, .flags = XIAOTAI_MEDIA_HOLD, .event = session_event, .ctx = s_events};
    assert(invoke((call_t){.op = OWNER_OPEN, .session = &session}) == XIAOTAI_OK);
    assert(receive_packet(GEN, BASE_SAMPLES, 0));
    atomic_store(&s_hold_output_write, true);
    assert(perform(OWNER_ACTIVATE, GEN) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_write_entered, WAIT_MS));
    assert(atomic_load(&s_powered) && !status().speaker_muted && !status().speaker_muted_applied);
    const unsigned disables = atomic_load(&s_disable_attempts);
    xiaotai_downlink_t *const ring = xiaotai_media_downlink();
    push_burst(GEN, PACKETS, 20);
    assert(xiaotai_downlink_count(ring) == PACKETS);

    atomic_store(&s_hold_codec_mute, true);
    atomic_store(&s_reject_codec_mute, true);
    assert(xiaotai_media_set_speaker_muted(true) == XIAOTAI_OK);
    xiaotai_signal_give(&s_write_release);
    assert(xiaotai_signal_take(&s_mute_entered, WAIT_MS));
    expect_event(0, GEN, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);
    const size_t queued = xiaotai_downlink_count(ring);
    printf("awaiting codec mute: gate=%u applied=%u queued=%zu\n",
           status().speaker_muted, status().speaker_muted_applied, queued);
    fflush(stdout);
    assert(status().speaker_muted == !after_accept && !status().speaker_muted_applied);
    assert(queued == (after_accept ? PACKETS : 0U));
    xiaotai_signal_give(&s_mute_release);
    assert(xiaotai_signal_take(&s_codec_refused, WAIT_MS));
    const xiaotai_media_status_t view = status();
    printf("codec refused mute: gate=%u applied=%u powered=%u extra_amp_disables=%u\n",
           view.speaker_muted, view.speaker_muted_applied,
           atomic_load(&s_powered), atomic_load(&s_disable_attempts) - disables);
    assert(!view.speaker_muted_applied && view.output_error == XIAOTAI_FAIL);
    assert(view.speaker_muted == !after_accept && atomic_load(&s_powered) == after_accept);
    assert(atomic_load(&s_disable_attempts) == disables + (after_accept ? 0U : 1U));

    xiaotai_media_stats_t stats;
    xiaotai_media_stats(&stats);
    assert(stats.rx_flushed == (after_accept ? 0U : PACKETS));
    if (after_accept) {
        finish_burst();
        const int64_t deadline = xiaotai_time_us() + WAIT_MS * 1000U;
        const uint32_t expected = BASE_SAMPLES + PACKETS * BURST_PAYLOAD_BYTES;
        while (stats.pcm_written < expected && xiaotai_time_us() < deadline) {
            xiaotai_delay_ms(1);
            xiaotai_media_stats(&stats);
        }
        printf("codec refusal preserved RX: packets=%u written=%u discarded=%u flushed=%u\n",
               stats.rx_played, stats.pcm_written, stats.pcm_discarded, stats.rx_flushed);
        assert(stats.rx_played == PACKETS + 1U && stats.pcm_written == expected);
        assert(!stats.rx_flushed && !stats.pcm_discarded);

        assert(xiaotai_media_set_speaker_muted(true) == XIAOTAI_OK);
        await_power_off_after(disables);
        assert(status().speaker_muted && status().speaker_muted_applied);
        assert(!receive_packet(GEN, BASE_SAMPLES, 2000));
        assert(xiaotai_media_set_speaker_muted(false) == XIAOTAI_OK);
        const int64_t unmute_deadline = xiaotai_time_us() + WAIT_MS * 1000U;
        while (!atomic_load(&s_powered) && xiaotai_time_us() < unmute_deadline) xiaotai_delay_ms(1);
        assert(atomic_load(&s_powered) && !status().speaker_muted && !status().speaker_muted_applied);
        assert(receive_packet(GEN, BASE_SAMPLES, 2020));
    } else {
        assert(!receive_packet(GEN, BASE_SAMPLES, 2000));
    }
    reset();
}

int main(int argc, char **argv)
{
    xiaotai_signal_t *const barriers[] = {
        &s_owner_done, &s_event_seen, &s_enable_entered, &s_enable_release, &s_disabled, &s_closed,
        &s_output_free, &s_burst_consumed, &s_copy_entered, &s_copy_release,
        &s_writer_count_entered, &s_writer_count_release, &s_disable_entered, &s_disable_release,
        &s_mute_entered, &s_mute_release, &s_codec_refused, &s_write_entered, &s_write_release,
    };
    for (size_t i = 0; i < sizeof(barriers) / sizeof(*barriers); ++i)
        assert(xiaotai_signal_init(barriers[i]) == XIAOTAI_OK);
    const bool deferred_mute = argc == 2 && !strcmp(argv[1], "deferred-codec-mute");
    const bool immediate_mute = argc == 2 && !strcmp(argv[1], "immediate-codec-mute");
    if (deferred_mute || immediate_mute || (argc == 2 && !strcmp(argv[1], "file-output-error"))) {
        if (deferred_mute || immediate_mute) check_refused_mute(deferred_mute);
        else check_file_output_error();
        for (size_t i = 0; i < sizeof(barriers) / sizeof(*barriers); ++i)
            xiaotai_signal_deinit(barriers[i]);
        return 0;
    }
    assert(argc == 1);

    boot(false);
    xiaotai_downlink_t *downlink = xiaotai_media_downlink();
    assert(downlink && xiaotai_media_can_receive(&s_need.downlink));
    assert(xiaotai_downlink_capacity(downlink) == XIAOTAI_DOWNLINK_BYTES(60U, 160U));
    xiaotai_media_session_t session = {
        .generation = 1, .downlink = s_need.downlink, .downlink_burst_ms = 1000,
        .flags = XIAOTAI_MEDIA_HOLD, .event = session_event, .ctx = s_events,
    };
    check_open_errors(&session);
    const call_t open = {.op = OWNER_OPEN, .session = &session};
    assert(invoke(open) == XIAOTAI_OK);
    assert(status().generation == 1 && !status().active && !status().draining && !status().uplink_open);
    assert(!atomic_load(&s_powered) && !atomic_load(&s_event_count));
    make_pending_packet(downlink, 1);
    xiaotai_media_stats_t before_stale, after_stale;
    xiaotai_media_stats(&before_stale);
    assert(perform(OWNER_ACTIVATE, 111) == XIAOTAI_ERR_INVALID_STATE);
    assert(perform(OWNER_DRAIN, 111) == XIAOTAI_ERR_INVALID_STATE);
    assert(perform(OWNER_FLUSH, 111) == XIAOTAI_ERR_INVALID_STATE);
    (void)perform(OWNER_CLOSE, 111);
    xiaotai_media_stats(&after_stale);
    assert(after_stale.stale_calls == before_stale.stale_calls + 4);
    assert(status().generation == 1 && xiaotai_downlink_count(downlink) == 1);
    assert(perform(OWNER_DRAIN, 1) == XIAOTAI_ERR_INVALID_STATE);
    session.generation = 2;
    assert(invoke(open) == XIAOTAI_ERR_INVALID_STATE);
    assert(perform(OWNER_FLUSH, 1) == XIAOTAI_OK && xiaotai_downlink_count(downlink) == 0);
    const size_t lengths[] = {80, 100, 160, 640, 751};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(*lengths); ++i) assert(receive_packet(1, lengths[i], (uint32_t)i * 20U));
    assert(xiaotai_downlink_count(downlink) == 12);
    assert(!receive_packet(1, 1600, 80));
    assert(!receive_packet(987, 1500, 80));
    xiaotai_media_stats(&after_stale);
    assert(after_stale.rx_packet_max_bytes == 1600 && after_stale.rx_invalid == 1 && after_stale.rx_stale == 1);
    assert(perform(OWNER_FLUSH, 1) == XIAOTAI_OK && xiaotai_downlink_count(downlink) == 0);
    for (uint32_t i = 0; i < 50U; ++i) assert(receive_packet(1, 160, 20U * i));
    xiaotai_media_stats(&after_stale);
    assert(xiaotai_downlink_count(downlink) == 50U && after_stale.rx_overflow == 0);
    assert(perform(OWNER_FLUSH, 1) == XIAOTAI_OK && xiaotai_downlink_count(downlink) == 0);
    for (uint32_t burst = 20; burst <= 40; burst += 20) {
        const uint32_t ms = 1000U / burst;
        for (uint32_t i = 0; i < burst; ++i) assert(receive_packet(1, 8000U / burst, ms * i));
        xiaotai_media_stats(&after_stale);
        assert(xiaotai_downlink_queued_us(downlink) == 1000000U && after_stale.rx_overflow == 0);
        assert(perform(OWNER_FLUSH, 1) == XIAOTAI_OK);
    }
    make_pending_packet(downlink, 1);

    atomic_store(&s_hold_enable, true);
    assert(perform(OWNER_ACTIVATE, 1) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_enable_entered, WAIT_MS));
    assert(!atomic_load(&s_event_count)); /* The driver has not completed. */
    assert(perform(OWNER_ACTIVATE, 1) == XIAOTAI_ERR_INVALID_STATE);
    const unsigned prior_disable = atomic_load(&s_disables);
    (void)perform(OWNER_CLOSE, 1); /* Close returns while the same driver call is held. */
    assert(status().generation == 0 && xiaotai_downlink_count(downlink) == 0);
    assert(!atomic_load(&s_free_count));
    xiaotai_signal_give(&s_enable_release);
    await_power_off_after(prior_disable);
    /* The worker posts OUTPUT_FREE only after processing the old completion. */
    expect_output_free(1);
    assert(!atomic_load(&s_event_count));
    session.generation = 1;
    assert(invoke(open) == XIAOTAI_ERR_INVALID_STATE);

    session.generation = 2;
    assert(invoke(open) == XIAOTAI_OK);
    xiaotai_media_stats(&after_stale);
    assert(after_stale.rx_packet_max_bytes == 0);
    make_pending_packet(downlink, 2);
    assert(perform(OWNER_ACTIVATE, 2) == XIAOTAI_OK);
    expect_event(0, 2, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);
    assert(status().active && atomic_load(&s_powered));
    /* The spec drains real DMA silence now. Hold enough to force the timeout
     * while the consumer remains healthy; a queued packet alone is not a stall. */
    atomic_store(&s_flush_frames, 10000);
    assert(perform(OWNER_DRAIN, 2) == XIAOTAI_OK);
    assert(status().draining && !status().uplink_open);
    assert(perform(OWNER_DRAIN, 2) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_os_host_advance_us(5100000);
    expect_event(1, 2, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_ERR_TIMEOUT);
    atomic_store(&s_flush_frames, 0);
    assert(status().generation == 2);
    const unsigned drain_disable = atomic_load(&s_disables);
    (void)perform(OWNER_CLOSE, 2);
    await_power_off_after(drain_disable);
    expect_output_free(2);
    assert(status().generation == 0 && xiaotai_downlink_count(downlink) == 0);

    session.generation = 3;
    session.flags = 0;
    atomic_store(&s_enable_result, XIAOTAI_FAIL);
    assert(invoke(open) == XIAOTAI_OK);
    expect_event(2, 3, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL);
    assert(!atomic_load(&s_powered) && status().output_error == XIAOTAI_FAIL);
    (void)perform(OWNER_CLOSE, 3);
    expect_output_free(3);
    /* Closing woke the worker, but did not issue another power command. */
    assert(status().output_error == XIAOTAI_FAIL);

    session.generation = 4;
    assert(invoke(open) == XIAOTAI_OK);
    expect_event(3, 4, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);
    assert(perform(OWNER_DRAIN, 4) == XIAOTAI_OK);
    expect_event(4, 4, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK);
    (void)perform(OWNER_CLOSE, 4);
    expect_output_free(4);
    reset();

    boot(true);
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    assert(info.playback && info.downlink_ms == 0 && info.preroll_ms == 0);
    assert(!xiaotai_media_downlink() && !xiaotai_media_can_receive(&s_need.downlink));
    assert(!xiaotai_media_can_send(&s_need.uplink));
    session.generation = 5;
    assert(invoke(open) == XIAOTAI_ERR_INVALID_STATE);
    reset();
    assert(atomic_load(&s_event_count) == 5);
    check_failed_start(6);
    check_idle_reopen();
    check_ai_burst_order();
    check_armed_close_and_capacity();
    check_revoke_during_copy();
    check_drain_of_admitted_packet();

    for (size_t i = 0; i < sizeof(barriers) / sizeof(*barriers); ++i)
        xiaotai_signal_deinit(barriers[i]);
    return 0;
}
