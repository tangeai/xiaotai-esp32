// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recording.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_preroll
// Replaces legacy: tests/regression/test_voice_reliability.py::test_preroll_handoff
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"
#include "xiaotai_os_host.h"
#include "g711.h"
#include "record.h"

#ifndef XIAOTAI_TEST_RECORD_LEGACY
#define XIAOTAI_TEST_RECORD_LEGACY 0
#endif
enum { RATE = XIAOTAI_TEST_RECORD_LEGACY ? 16000 : 8000, BLOCK = XIAOTAI_TEST_RECORD_LEGACY ? 512 : 160,
       BLOCK_US = BLOCK * 1000000 / RATE, WAIT_MS = 3000, QUEUE_DEPTH = 8 };
enum lane { CAPTURE, UPLINK, PLAYOUT, LANES };
#ifndef XIAOTAI_TEST_PREROLL
#define XIAOTAI_TEST_PREROLL 0
#endif
#ifndef XIAOTAI_TEST_RECORD_PROGRESS
#define XIAOTAI_TEST_RECORD_PROGRESS 0
#endif

typedef struct {
    int64_t end_us;
    uint32_t lost;
    uint16_t samples;
    int16_t first;
} capture_t;

static xiaotai_queue_t s_capture;
static xiaotai_signal_t s_acted, s_completed, s_lost, s_bound_lost, s_owner_blocked, s_owner_resume;
static xiaotai_signal_t s_effect_entered, s_effect_resume, s_progress;
static capture_t s_this;
static int16_t s_clean[BLOCK];
static atomic_uint s_tap_frames, s_done, s_token_lost, s_uplink_frames;
static atomic_uint s_bound_loss_count;
static atomic_uint s_progress_calls, s_progress_id, s_progress_samples;
static _Atomic(struct xiaotai_host_task *) s_tasks[LANES];
static atomic_bool s_hold_effect;
static atomic_uint s_processed;
static bool s_inflight;
static unsigned s_submitted;
static uint16_t s_effect_delay;
static struct { uint32_t stamp; uint8_t data[BLOCK]; } s_uplink[360];
static const xiaotai_media_requirement_t s_need = {
    .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .preroll_ms = 4000,
};

bool __wrap_xiaotai_net_provisioning_boot(void);
bool __wrap_xiaotai_net_provisioning_boot(void) { return false; }
bool __wrap_xiaotai_mem_has_external(void);
bool __wrap_xiaotai_mem_has_external(void) { return true; }

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    const xiaotai_err_t result = __real_xiaotai_task_create(config, task);
    static const char *const names[] = {"xt_capture", "xt_uplink", "xt_playout"};
    if (result == XIAOTAI_OK) for (unsigned i = 0; i < LANES; ++i) {
        if (!strcmp(config->name, names[i])) assert(!atomic_exchange(&s_tasks[i], task->task));
    }
    return result;
}

static void assert_lane(enum lane lane)
{
    xiaotai_task_t handle = {0};
    const int64_t deadline = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
    while (!(handle.task = atomic_load(&s_tasks[lane])) && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(!xiaotai_on_owner() && xiaotai_task_is_current(&handle));
}

static xiaotai_err_t input_open(void *self) { (void)self; return XIAOTAI_OK; }
static xiaotai_err_t input_close(void *self) { (void)self; return XIAOTAI_OK; }

static xiaotai_err_t input_read(void *self, void *data, size_t capacity, size_t *size)
{
    (void)self;
    assert_lane(CAPTURE);
    assert(capacity >= sizeof(s_clean));
    if (s_inflight) {
        s_inflight = false;
        atomic_fetch_add(&s_processed, 1);
    }
    if (!xiaotai_queue_receive(&s_capture, &s_this, 100)) return XIAOTAI_ERR_TIMEOUT;
    s_inflight = true;
    int16_t *pcm = data;
    for (int n = 0; n < BLOCK; ++n) pcm[n] = (int16_t)(s_this.first + n);
    *size = sizeof(s_clean);
    return XIAOTAI_OK;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    static const xiaotai_audio_input_t input = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1},
        .open = input_open, .read = input_read, .close = input_close,
    };
    return index ? NULL : &input;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index) { (void)index; return NULL; }

static int match(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{
    (void)out;
    return in && in->rate_hz == RATE ? 1000 : 0;
}

static xiaotai_err_t processor_open(const xiaotai_audio_input_t *in, const xiaotai_audio_output_t *out,
                                     xiaotai_processor_info_t *info, void **state)
{
    (void)in;
    (void)out;
    *info = (xiaotai_processor_info_t){.rate_hz = RATE, .feed_frames = BLOCK, .fetch_max = BLOCK};
    *state = s_clean;
    return XIAOTAI_OK;
}

static xiaotai_err_t feed(void *state, const void *data, size_t size, int64_t captured_us, uint32_t lost)
{
    (void)captured_us;
    (void)lost;
    assert(state == s_clean && size == sizeof(s_clean));
    if (!s_this.samples) return XIAOTAI_ERR_INVALID_STATE;
    memcpy(s_clean, data, size);
    return XIAOTAI_OK;
}

static xiaotai_err_t fetch(void *state, xiaotai_processor_frame_t *frame, uint32_t timeout)
{
    (void)timeout;
    assert(state == s_clean);
    *frame = (xiaotai_processor_frame_t){
        .encoding = XIAOTAI_CODEC_PCM16, .data = s_clean, .size = s_this.samples * sizeof(*s_clean),
        .samples = s_this.samples, .captured_us = s_this.end_us, .lost_samples = s_this.lost,
    };
    return XIAOTAI_OK;
}

static void processor_close(void *state) { assert(state == s_clean); }

static xiaotai_err_t effect_open(const xiaotai_processor_info_t *info, xiaotai_effect_info_t *out, void **state)
{
    assert(info->rate_hz == RATE);
    *out = (xiaotai_effect_info_t){.delay_samples = s_effect_delay};
    *state = NULL;
    return XIAOTAI_OK;
}

static xiaotai_err_t process(void *state, int16_t *pcm, size_t samples)
{
    (void)state;
    if (atomic_exchange(&s_hold_effect, false)) {
        xiaotai_signal_give(&s_effect_entered);
        assert(xiaotai_signal_take(&s_effect_resume, WAIT_MS));
    }
    for (size_t n = 0; n < samples; ++n) pcm[n] = (int16_t)(pcm[n] + 37);
    return XIAOTAI_OK;
}

static void effect_close(void *state) { assert(!state); }

static void tap_pcm(void *ctx, const int16_t *pcm, size_t samples, uint32_t rate_hz, int64_t end_us)
{
    (void)ctx;
    (void)end_us;
    assert(pcm && samples <= BLOCK && rate_hz == RATE && !xiaotai_on_owner());
    atomic_fetch_add(&s_tap_frames, 1);
}

static const xiaotai_media_tap_t s_tap = {.name = "fixture", .pcm = tap_pcm};

static xiaotai_err_t sink(void *ctx, uint32_t generation, uint32_t stamp, const uint8_t *data, size_t size)
{
    (void)ctx;
    assert_lane(UPLINK);
    assert(generation && data && size == BLOCK);
    const unsigned index = atomic_load(&s_uplink_frames);
    assert(index < sizeof(s_uplink) / sizeof(*s_uplink));
    s_uplink[index].stamp = stamp;
    memcpy(s_uplink[index].data, data, size);
    atomic_store(&s_uplink_frames, index + 1);
    return XIAOTAI_OK;
}

static const xiaotai_media_sink_t s_sink = {.send = sink};

static xiaotai_err_t configure(void)
{
    static const xiaotai_audio_processor_t processor = {
        .name = "fixture", .match = match, .open = processor_open, .close = processor_close, .feed = feed, .fetch = fetch,
    };
    static const xiaotai_audio_effect_t effect = {
        .name = "fixture", .stage = XIAOTAI_EFFECT_CAPTURE, .open = effect_open, .process = process,
        .close = effect_close,
    };
    assert(xiaotai_media_require(&s_need) == XIAOTAI_OK);
    assert(xiaotai_media_processor_register(&processor) == XIAOTAI_OK);
    assert(xiaotai_media_effect_register(&effect) == XIAOTAI_OK);
    assert(xiaotai_media_tap_register(&s_tap) == XIAOTAI_OK);
    return xiaotai_media_tap_enable(&s_tap, true);
}

typedef struct { uint32_t id, samples; xiaotai_err_t result; } completion_t;
static completion_t s_last;

static void recorded(uint32_t id, xiaotai_err_t result, uint32_t samples, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == &s_last);
    s_last = (completion_t){id, samples, result};
    atomic_fetch_add(&s_done, 1);
    xiaotai_signal_give(&s_completed);
}

static void progressed(uint32_t id, uint32_t samples, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == &s_last && id);
    assert(samples && samples % RATE == 0);
    atomic_store(&s_progress_id, id);
    atomic_store(&s_progress_samples, samples);
    atomic_fetch_add(&s_progress_calls, 1);
    xiaotai_signal_give(&s_progress);
}

static void history_lost(uint32_t token, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == &s_lost);
    assert(!atomic_exchange(&s_token_lost, token));
    xiaotai_signal_give(&s_lost);
}

static void bound_session_event(uint32_t generation, xiaotai_media_event_t kind, xiaotai_err_t err, void *ctx)
{
    assert(xiaotai_on_owner() && generation == 7 && ctx == &s_bound_lost);
    if (kind == XIAOTAI_MEDIA_EVENT_STARTED) {
        assert(err == XIAOTAI_OK);
        return;
    }
    assert(kind == XIAOTAI_MEDIA_EVENT_PREROLL_LOST && err == XIAOTAI_ERR_INVALID_STATE);
    atomic_fetch_add(&s_bound_loss_count, 1);
    xiaotai_signal_give(&s_bound_lost);
}

typedef struct {
    enum { RECORD, AL_RECORD, STOP, MUTE, HOLD, OPEN, ACTIVATE, CLOSE } type;
    const xiaotai_media_record_t *record;
    uint8_t *alaw;
    const xiaotai_media_preroll_t *preroll;
    const xiaotai_media_session_t *session;
    uint32_t id;
    int64_t before_us, after_us;
    xiaotai_err_t result;
} action_t;

static void owner(void *arg)
{
    action_t *a = arg;
    a->before_us = xiaotai_time_us();
    switch (a->type) {
        case RECORD: a->result = xiaotai_media_record(a->record, &a->id); break;
        case AL_RECORD: a->result = media_session_record_alaw(a->alaw, BLOCK, recorded, &s_last, &a->id); break;
        case STOP: xiaotai_media_record_stop(a->id); break;
        case MUTE: a->result = xiaotai_media_set_mic_muted(a->id != 0); break;
        case HOLD: a->result = xiaotai_media_preroll_hold(a->preroll, &a->id); break;
        case OPEN: a->result = xiaotai_media_open(a->session); break;
        case ACTIVATE: a->result = xiaotai_media_activate(a->id); break;
        case CLOSE: xiaotai_media_close(a->id); break;
    }
    a->after_us = xiaotai_time_us();
    xiaotai_signal_give(&s_acted);
}

static action_t run(action_t action)
{
    assert(xiaotai_run(owner, &action) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_acted, WAIT_MS));
    return action;
}

static xiaotai_media_status_t status(void)
{
    xiaotai_media_status_t snapshot = {0};
    xiaotai_media_status(&snapshot);
    return snapshot;
}

static void give_partial(int16_t first, int64_t end, uint32_t lost, uint16_t samples)
{
    const int64_t now = xiaotai_time_us();
    if (end > now) xiaotai_os_host_advance_us(end - now);
    const capture_t command = {.end_us = end, .lost = lost, .samples = samples, .first = first};
    assert(xiaotai_queue_send(&s_capture, &command, 0));
    ++s_submitted;
}

static void give(int16_t first, int64_t end, uint32_t lost) { give_partial(first, end, lost, BLOCK); }

static void expect(uint32_t id, xiaotai_err_t result, uint32_t samples)
{
    assert(xiaotai_signal_take(&s_completed, WAIT_MS));
    assert(s_last.id == id && s_last.result == result && s_last.samples == samples);
}

static void block_owner(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&s_owner_blocked);
    assert(xiaotai_signal_take(&s_owner_resume, WAIT_MS));
}

static void noop(void *ctx) { (void)ctx; }

static void saturate_owner(void)
{
    assert(xiaotai_run(block_owner, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_owner_blocked, WAIT_MS));
    for (unsigned n = 0; n < QUEUE_DEPTH; ++n) {
        const xiaotai_err_t err = xiaotai_run(noop, NULL);
        if (err == XIAOTAI_ERR_NO_MEM) break;
        assert(err == XIAOTAI_OK);
    }
    assert(xiaotai_run(noop, NULL) == XIAOTAI_ERR_NO_MEM);
}

#define AWAIT(condition) do {                                                                 \
    const int64_t until_us = xiaotai_time_us() + 1000 * WAIT_MS;                                \
    while (!(condition) && xiaotai_time_us() < until_us) xiaotai_delay_ms(1);                   \
    assert(condition);                                                                       \
} while (0)

static void record_until(uint32_t *copied, int64_t *stamp, uint32_t target, bool finish)
{
    while (*copied < target) {
        const uint32_t remaining = target - *copied;
        const uint16_t block = remaining < BLOCK ? (uint16_t)remaining : BLOCK;
        *stamp += (int64_t)block * 1000000 / RATE;
        give_partial(1000, *stamp, 0, block);
        *copied += block;
        if (finish && *copied == target) AWAIT(!status().recording);
        else AWAIT(status().recorded_samples == *copied);
    }
}

static void expect_progress(uint32_t id, uint32_t samples, unsigned count)
{
    assert(xiaotai_signal_take(&s_progress, WAIT_MS));
    assert(atomic_load(&s_progress_id) == id && atomic_load(&s_progress_samples) == samples);
    assert(atomic_load(&s_progress_calls) == count);
}

static void check_record_progress(void)
{
    static int16_t pcm[5 * RATE];
    xiaotai_media_record_t request = {.pcm = pcm, .samples = 5 * RATE, .done = recorded,
        .ctx = &s_last, .progress = progressed};
    const action_t first = run((action_t){.type = RECORD, .record = &request});
    assert(first.result == XIAOTAI_OK && first.id);
    int64_t stamp = first.after_us;
    uint32_t copied = 0;
    record_until(&copied, &stamp, RATE - 1, false);
    xiaotai_os_host_advance_us(4000000);
    assert(!xiaotai_signal_take(&s_progress, 0));
    record_until(&copied, &stamp, RATE + 1, false);
    expect_progress(first.id, RATE, 1);

    xiaotai_media_stats_t before, now;
    xiaotai_media_stats(&before);
    saturate_owner();
    record_until(&copied, &stamp, 3 * RATE + 1, false);
    AWAIT((xiaotai_media_stats(&now), now.owner_post_retries > before.owner_post_retries));
    assert(!xiaotai_signal_take(&s_progress, 0));
    xiaotai_signal_give(&s_owner_resume);
    expect_progress(first.id, 3 * RATE, 2);

    assert(xiaotai_run(block_owner, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_owner_blocked, WAIT_MS));
    record_until(&copied, &stamp, 4 * RATE, false);
    AWAIT(atomic_load(&s_processed) >= s_submitted);
    give(1000, stamp + BLOCK_US, 1);
    AWAIT(!status().recording);
    xiaotai_signal_give(&s_owner_resume);
    expect(first.id, XIAOTAI_FAIL, 4 * RATE);
    assert(!xiaotai_signal_take(&s_progress, 0) && atomic_load(&s_progress_calls) == 2);

    request.samples = 2 * RATE;
    const action_t second = run((action_t){.type = RECORD, .record = &request});
    assert(second.result == XIAOTAI_OK && second.id != first.id);
    stamp = second.after_us;
    copied = 0;
    record_until(&copied, &stamp, RATE, false);
    expect_progress(second.id, RATE, 3);
    record_until(&copied, &stamp, 2 * RATE, true);
    expect(second.id, XIAOTAI_OK, 2 * RATE);
    assert(!xiaotai_signal_take(&s_progress, 0) && atomic_load(&s_done) == 2);
}

static void check_record(void)
{
    int16_t pcm[2 * BLOCK + 1];
    for (size_t n = 0; n < sizeof(pcm) / sizeof(*pcm); ++n) pcm[n] = -1;
    xiaotai_media_record_t request = {.pcm = pcm, .samples = RATE * 60U + 1, .done = recorded, .ctx = &s_last};
    assert(run((action_t){.type = RECORD, .record = &request}).result == XIAOTAI_ERR_INVALID_ARG);
    request.samples = 200;
    action_t first = run((action_t){.type = RECORD, .record = &request});
    assert(first.result == XIAOTAI_OK && first.id);
    assert(run((action_t){.type = RECORD, .record = &request}).result == XIAOTAI_ERR_BUSY);
    (void)run((action_t){.type = STOP, .id = first.id + 1});

    const unsigned taps = atomic_load(&s_tap_frames);
    give(1000, first.before_us + BLOCK_US / 2, 0); /* Whole PCM block crosses the start. */
    give(2000, first.after_us + BLOCK_US, 0);
    AWAIT(status().recorded_samples == BLOCK);
    assert(atomic_load(&s_tap_frames) == taps && !atomic_load(&s_uplink_frames));

    saturate_owner();
    give(3000, first.after_us + BLOCK_US * 2, 0);
    AWAIT(!status().recording);
    xiaotai_media_stats_t stats;
    AWAIT((xiaotai_media_stats(&stats), stats.owner_post_retries > 0));
    assert(!atomic_load(&s_done));
    xiaotai_signal_give(&s_owner_resume);
    expect(first.id, XIAOTAI_OK, 200);
    for (int n = 0; n < BLOCK; ++n) assert(pcm[n] == 2000 + n + 37);
    for (int n = BLOCK; n < 200; ++n) assert(pcm[n] == 3000 + n - BLOCK + 37);
    assert(pcm[200] == -1);

    for (unsigned reason = 0; reason < 5; ++reason) {
        action_t next = run((action_t){.type = RECORD, .record = &request});
        assert(next.result == XIAOTAI_OK && next.id > first.id);
        const int64_t end = next.after_us + BLOCK_US;
        give(4000, end, 0);
        AWAIT(status().recorded_samples == BLOCK);
        if (reason == 0) give(5000, end + BLOCK_US, 1);
        if (reason == 1) give(5000, end, 0);
        if (reason == 2) (void)run((action_t){.type = STOP, .id = next.id});
        if (reason == 3) {
            const xiaotai_media_session_t session = {
                .generation = 3, .uplink = s_need.uplink, .sink = &s_sink, .flags = XIAOTAI_MEDIA_HOLD,
            };
            assert(run((action_t){.type = OPEN, .session = &session}).result == XIAOTAI_OK);
        }
        if (reason == 4) assert(run((action_t){.type = MUTE, .id = 1}).result == XIAOTAI_OK);
        expect(next.id, reason < 2 ? XIAOTAI_FAIL : XIAOTAI_ERR_INVALID_STATE, BLOCK);
        if (reason == 3) (void)run((action_t){.type = CLOSE, .id = 3});
    }
    assert(run((action_t){.type = RECORD, .record = &request}).result == XIAOTAI_ERR_INVALID_STATE);
    assert(run((action_t){.type = MUTE}).result == XIAOTAI_OK);
    assert(atomic_load(&s_done) == 6);
    xiaotai_media_stats(&stats);
    assert(stats.recordings == 6 && stats.recordings_completed == 1 && stats.recordings_failed == 2);

    uint8_t alaw[BLOCK + 1] = {[BLOCK] = 0xAA};
    action_t diagnostic = run((action_t){.type = AL_RECORD, .alaw = alaw});
    assert(diagnostic.result == XIAOTAI_OK && diagnostic.id);
    give(6000, diagnostic.after_us + BLOCK_US, 0);
    expect(diagnostic.id, XIAOTAI_OK, BLOCK);
    for (int n = 0; n < BLOCK; ++n) assert(alaw[n] == xiaotai_media_alaw_encode_sample((int16_t)(6037 + n)));
    assert(alaw[BLOCK] == 0xAA);

    action_t failed = run((action_t){.type = RECORD, .record = &request});
    assert(failed.result == XIAOTAI_OK && failed.id);
    give(7000, failed.after_us + BLOCK_US, 0);
    AWAIT(status().recorded_samples == BLOCK);
    give_partial(0, failed.after_us + 2 * BLOCK_US, 0, 0); /* Processor fails permanently. */
    expect(failed.id, XIAOTAI_FAIL, BLOCK);
    assert(atomic_load(&s_done) == 8);
}

static void give_legacy(int16_t first, int64_t end, uint32_t lost)
{
    give(first, end + (int64_t)s_effect_delay * 1000000 / RATE, lost);
    AWAIT(atomic_load(&s_processed) == s_submitted);
}

static void check_legacy_window(void)
{
    int16_t *const guarded = malloc((RATE + 2U) * sizeof(*guarded));
    assert(guarded);
    int16_t *const pcm = guarded + 1;
    guarded[0] = guarded[RATE + 1U] = 12345;
    memset(pcm, 0x55, RATE * sizeof(*pcm));
    xiaotai_media_record_t request = {.pcm = NULL, .samples = RATE, .done = recorded, .ctx = &s_last};
    assert(run((action_t){.type = RECORD, .record = &request}).result == XIAOTAI_ERR_INVALID_ARG);
    request.pcm = (int16_t *)((unsigned char *)pcm + 1);
    assert(run((action_t){.type = RECORD, .record = &request}).result == XIAOTAI_ERR_INVALID_ARG);
    request.pcm = pcm;
    const action_t first = run((action_t){.type = RECORD, .record = &request});
    assert(first.result == XIAOTAI_OK && first.id);
    const xiaotai_media_preroll_t hold = {
        .wake_us = first.after_us, .keep_before_ms = 32, .ttl_ms = 1000,
    };
    assert(run((action_t){.type = HOLD, .preroll = &hold}).result == XIAOTAI_ERR_INVALID_STATE);
    if (s_effect_delay) {
        /* Raw stamp fits the window; PCM at the compensated stamp does not. */
        give(0, first.after_us + BLOCK_US, 0);
        AWAIT(atomic_load(&s_processed) == s_submitted);
        assert(status().recorded_samples == 0 && pcm[0] == 0x5555);
    }
    give_legacy(0, first.after_us + BLOCK_US - 1000, 0);
    assert(status().recorded_samples == 0 && pcm[0] == 0x5555);

    saturate_owner();
    for (unsigned part = 0; part < 32; ++part)
        give_legacy((int16_t)((int)part * BLOCK - 37), first.after_us + (int64_t)(part + 1) * BLOCK_US, 0);
    assert(!status().recording && atomic_load(&s_done) == 0);
    xiaotai_media_stats_t stats;
    AWAIT((xiaotai_media_stats(&stats), stats.owner_post_retries > 0));
    xiaotai_signal_give(&s_owner_resume);
    expect(first.id, XIAOTAI_OK, RATE);
    for (int i = 0; i < RATE; ++i) assert(pcm[i] == i);
    assert(guarded[0] == 12345 && guarded[RATE + 1U] == 12345);
    free(guarded);
    give_legacy(0, first.after_us + 33 * BLOCK_US, 0);
    assert(atomic_load(&s_done) == 1);

    int16_t failed[2 * BLOCK];
    memset(failed, 0x55, sizeof(failed));
    request.pcm = failed;
    request.samples = 2 * BLOCK;
    const action_t gap = run((action_t){.type = RECORD, .record = &request});
    assert(gap.result == XIAOTAI_OK && gap.id != first.id);
    give_legacy(1000, gap.after_us + BLOCK_US, 0);
    give_legacy(2000, gap.after_us + 2 * BLOCK_US, 1);
    expect(gap.id, XIAOTAI_FAIL, BLOCK);
    give_legacy(3000, gap.after_us + 3 * BLOCK_US, 0);
    for (int i = 0; i < BLOCK; ++i) assert(failed[i] == 1037 + i && failed[i + BLOCK] == 0x5555);

    /* Cancellation returns ownership even while capture is still processing. */
    for (unsigned round = 0; round < 20; ++round) {
        request.pcm = malloc(request.samples * sizeof(*request.pcm));
        assert(request.pcm);
        const action_t next = run((action_t){.type = RECORD, .record = &request});
        assert(next.result == XIAOTAI_OK && next.id > gap.id);
        atomic_store(&s_hold_effect, true);
        give(4200, next.after_us + BLOCK_US + (int64_t)s_effect_delay * 1000000 / RATE, 0);
        assert(xiaotai_signal_take(&s_effect_entered, WAIT_MS));
        (void)run((action_t){.type = STOP, .id = next.id});
        expect(next.id, XIAOTAI_ERR_INVALID_STATE, 0);
        free(request.pcm);
        xiaotai_signal_give(&s_effect_resume);
        AWAIT(atomic_load(&s_processed) == s_submitted);
    }
    assert(atomic_load(&s_done) == 22);
    printf("PASS: 16000 samples, 512/fetch, effect delay %u, busy owner and 20 detach races\n", s_effect_delay);
}

static void check_held_history(void)
{
    int16_t pcm[BLOCK];
    const xiaotai_media_record_t request = {.pcm = pcm, .samples = BLOCK, .done = recorded, .ctx = &s_last};
    const unsigned before = atomic_load(&s_tap_frames);
    const int64_t end = xiaotai_time_us();
    give(1200, end, 0);
    AWAIT(status().preroll_ms >= 20);
    AWAIT(atomic_load(&s_tap_frames) > before);
    const xiaotai_media_preroll_t hold = {
        .wake_us = end, .keep_before_ms = 20, .ttl_ms = 1000, .lost = history_lost, .ctx = &s_lost,
    };
    const action_t token = run((action_t){.type = HOLD, .preroll = &hold});
    assert(token.result == XIAOTAI_OK && token.id);
    assert(run((action_t){.type = RECORD, .record = &request}).result == XIAOTAI_ERR_INVALID_STATE);
    const xiaotai_media_session_t named = {
        .generation = 4, .uplink = s_need.uplink, .duplex = true, .sink = &s_sink,
        .preroll_token = token.id, .flags = XIAOTAI_MEDIA_HOLD,
    };
    assert(run((action_t){.type = OPEN, .session = &named}).result == XIAOTAI_OK);
    assert(!status().preroll_bound && status().preroll_token == token.id);
    (void)run((action_t){.type = CLOSE, .id = named.generation});
    assert(!status().generation && status().preroll_token == token.id);
    assert(run((action_t){.type = MUTE, .id = 1}).result == XIAOTAI_OK);
    assert(!status().preroll_token && status().mic_muted);
    assert(xiaotai_signal_take(&s_lost, WAIT_MS) && atomic_load(&s_token_lost) == token.id);
    const action_t unmuted = run((action_t){.type = MUTE});
    assert(unmuted.result == XIAOTAI_OK);

    const unsigned tap_before_mute = atomic_load(&s_tap_frames);
    atomic_store(&s_hold_effect, true);
    give(1300, unmuted.after_us + BLOCK_US, 0);
    assert(xiaotai_signal_take(&s_effect_entered, WAIT_MS));
    assert(run((action_t){.type = MUTE, .id = 1}).result == XIAOTAI_OK);
    assert(run((action_t){.type = MUTE}).result == XIAOTAI_OK);
    xiaotai_signal_give(&s_effect_resume);
    AWAIT(atomic_load(&s_processed) == s_submitted);
    assert(!status().preroll_ms && atomic_load(&s_tap_frames) == tap_before_mute);

    xiaotai_media_session_t unnamed = named;
    unnamed.generation++;
    unnamed.preroll_token = 0;
    assert(run((action_t){.type = OPEN, .session = &unnamed}).result == XIAOTAI_OK);
    assert(run((action_t){.type = RECORD, .record = &request}).result == XIAOTAI_ERR_BUSY);
    (void)run((action_t){.type = CLOSE, .id = unnamed.generation});
}

static int16_t timeline_sample(unsigned index) { return (int16_t)((int)index - 25000); }

static unsigned replay_block(unsigned index, int64_t end)
{
    const unsigned first = atomic_load(&s_uplink_frames);
    const unsigned before_ms = status().preroll_ms;
    give(timeline_sample(index), end, 0);
    AWAIT(atomic_load(&s_processed) == s_submitted);
    const unsigned left = status().preroll_ms;
    assert(left <= before_ms + 20);
    const unsigned replayed = (before_ms + 20 - left) / 20;
    if (!(replayed > 0 && replayed <= 3))
        fprintf(stderr, "preroll replay: before=%u ms left=%u ms replayed=%u frames=%u first=%u\n", before_ms, left,
                replayed, atomic_load(&s_uplink_frames), first);
    assert(replayed > 0 && replayed <= 3);
    AWAIT(atomic_load(&s_uplink_frames) == first + replayed);
    return first + replayed;
}

static void check_replay_after_idle(void)
{
    assert(xiaotai_media_tap_enable(&s_tap, false) == XIAOTAI_OK);
    assert(run((action_t){.type = MUTE, .id = 1}).result == XIAOTAI_OK);
    const action_t unmuted = run((action_t){.type = MUTE});
    assert(unmuted.result == XIAOTAI_OK);
    assert(xiaotai_media_tap_enable(&s_tap, true) == XIAOTAI_OK);
    int64_t end = unmuted.after_us + BLOCK_US;
    give(timeline_sample(0), end, 0);
    AWAIT(status().preroll_ms == 20);
    const xiaotai_media_preroll_t hold = {.wake_us = end, .keep_before_ms = 20, .ttl_ms = 10000};
    const action_t token = run((action_t){.type = HOLD, .preroll = &hold});
    assert(token.result == XIAOTAI_OK);
    assert(xiaotai_media_tap_enable(&s_tap, false) == XIAOTAI_OK);
    unsigned index = BLOCK;
    const unsigned taps = atomic_load(&s_tap_frames);
    for (unsigned frame = 0; frame < 150; ++frame) {
        give(timeline_sample(index), end += BLOCK_US, 0);
        index += BLOCK;
        AWAIT(status().preroll_ms == (frame + 2U) * 20U);
    }
    assert(!status().generation && status().preroll_token == token.id);
    assert(atomic_load(&s_tap_frames) == taps);
    give_partial(timeline_sample(index), end += 2125, 0, 17);
    index += 17;
    AWAIT(status().preroll_ms == 3022);

    const xiaotai_media_session_t session = {
        .generation = 6, .uplink = s_need.uplink, .duplex = true, .sink = &s_sink,
        .preroll_token = token.id, .flags = XIAOTAI_MEDIA_HOLD,
    };
    assert(run((action_t){.type = OPEN, .session = &session}).result == XIAOTAI_OK);
    assert(run((action_t){.type = ACTIVATE, .id = session.generation}).result == XIAOTAI_OK);
    AWAIT(status().active);
    assert(!atomic_load(&s_uplink_frames));
    unsigned sent = 0;
    for (unsigned frame = 0; frame < 154 && status().preroll_bound; ++frame) {
        sent = replay_block(index, end += BLOCK_US);
        index += BLOCK;
    }
    assert(!status().preroll_bound && !status().preroll_token && status().active);
    give(timeline_sample(index), end + BLOCK_US, 0);
    AWAIT(atomic_load(&s_uplink_frames) > sent);
    sent = atomic_load(&s_uplink_frames);
    assert(sent >= 152);
    for (unsigned p = 0; p < sent; ++p) {
        if (p) assert(s_uplink[p].stamp - s_uplink[p - 1].stamp == 20U);
        for (unsigned n = 0; n < BLOCK; ++n) {
            const int16_t sample = timeline_sample(p * BLOCK + n);
            assert(s_uplink[p].data[n] == xiaotai_media_alaw_encode_sample((int16_t)(sample + 37)));
        }
    }
    (void)run((action_t){.type = CLOSE, .id = session.generation});
}

static void check_bound_gap(void)
{
    const unsigned previous_token = atomic_load(&s_token_lost);
    const unsigned previous_tx = atomic_load(&s_uplink_frames);
    assert(xiaotai_media_tap_enable(&s_tap, true) == XIAOTAI_OK);
    int64_t end = xiaotai_time_us();
    give(8000, end, 0);
    AWAIT(status().preroll_ms == 20);
    const xiaotai_media_preroll_t hold = {
        .wake_us = end, .keep_before_ms = 20, .ttl_ms = 10000, .lost = history_lost, .ctx = &s_lost,
    };
    const action_t token = run((action_t){.type = HOLD, .preroll = &hold});
    assert(token.result == XIAOTAI_OK && token.id);
    const xiaotai_media_session_t session = {
        .generation = 7, .uplink = s_need.uplink, .duplex = true, .sink = &s_sink,
        .preroll_token = token.id, .event = bound_session_event, .ctx = &s_bound_lost,
    };
    assert(run((action_t){.type = OPEN, .session = &session}).result == XIAOTAI_OK);
    AWAIT(status().active && status().preroll_bound);

    xiaotai_media_stats_t before, after;
    xiaotai_media_stats(&before);
    saturate_owner();
    give(9000, end + BLOCK_US + 125, 1);
    AWAIT(atomic_load(&s_uplink_frames) == previous_tx + 1);
    AWAIT((xiaotai_media_stats(&after), after.owner_post_retries > before.owner_post_retries));
    assert(after.preroll_lost == before.preroll_lost + 1 && !atomic_load(&s_bound_loss_count));
    assert(status().active && !status().preroll_token);
    xiaotai_signal_give(&s_owner_resume);
    assert(xiaotai_signal_take(&s_bound_lost, WAIT_MS));
    assert(atomic_load(&s_bound_loss_count) == 1 && atomic_load(&s_token_lost) == previous_token);
    for (int n = 0; n < BLOCK; ++n) {
        assert(s_uplink[previous_tx].data[n] == xiaotai_media_alaw_encode_sample((int16_t)(9037 + n)));
    }
    (void)run((action_t){.type = CLOSE, .id = session.generation});
}

int main(int argc, char **argv)
{
    if (XIAOTAI_TEST_RECORD_LEGACY && argc == 2 && !strcmp(argv[1], "agc")) s_effect_delay = 160;
    else assert(argc == 1);
    static const char *const depends[] = {"media", NULL};
    static const xiaotai_module_t engine = {
        .name = "media", .init = xiaotai_engine_media_init, .start = xiaotai_engine_media_start,
    };
    static const xiaotai_module_t fixture = {.name = "fixture", .depends = depends, .init = configure};
    static const xiaotai_module_t *const modules[] = {&engine, &fixture};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 2, .queue_depth = QUEUE_DEPTH};
    xiaotai_signal_t *const signals[] = {
        &s_acted, &s_completed, &s_lost, &s_bound_lost, &s_owner_blocked, &s_owner_resume,
        &s_effect_entered, &s_effect_resume, &s_progress,
    };
    for (size_t n = 0; n < sizeof(signals) / sizeof(*signals); ++n) assert(xiaotai_signal_init(signals[n]) == XIAOTAI_OK);
    assert(xiaotai_queue_init(&s_capture, QUEUE_DEPTH, sizeof(capture_t), XIAOTAI_MEM_INTERNAL) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    AWAIT(status().ready);
    assert(atomic_load(&s_tasks[PLAYOUT]));
    if (XIAOTAI_TEST_RECORD_LEGACY) check_legacy_window();
    else if (XIAOTAI_TEST_PREROLL) {
        check_held_history();
        check_replay_after_idle();
        check_bound_gap();
    } else if (XIAOTAI_TEST_RECORD_PROGRESS) check_record_progress();
    else check_record();
    xiaotai_media_reset();
    xiaotai_framework_reset();
    xiaotai_queue_deinit(&s_capture);
    for (size_t n = 0; n < sizeof(signals) / sizeof(*signals); ++n) xiaotai_signal_deinit(signals[n]);
    return 0;
}
