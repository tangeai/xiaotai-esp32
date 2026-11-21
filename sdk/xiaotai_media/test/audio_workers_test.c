// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_hardware_boundaries.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_afe_pipeline_counters
// Replaces legacy: tests/regression/test_voice_reliability.py::test_wake_diagnostics
// Replaces legacy: tests/regression/test_p4_pcma.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "g711.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/port/os.h"

#define WAIT 2500U
#define SAMPLES 160U

#ifndef AUDIO_WORKERS_ASYNC
#define AUDIO_WORKERS_ASYNC 0
#endif
#ifndef AUDIO_WORKERS_PCMA_LEGACY
#define AUDIO_WORKERS_PCMA_LEGACY 0
#endif

#if AUDIO_WORKERS_PCMA_LEGACY
#include <inttypes.h>
#else
#include "diag.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#endif

enum {
    CAPTURE_RATE = AUDIO_WORKERS_PCMA_LEGACY ? 16000U : 8000U,
    CAPTURE_SAMPLES = CAPTURE_RATE * 20U / 1000U,
};

static xiaotai_task_t capture_task, clean_task, uplink_task;
static xiaotai_queue_t inputs, processed;
static xiaotai_signal_t owner, started, sent, entered, release, failed;
static xiaotai_signal_t read_entered, read_release;
static atomic_bool peer_ready, hold_send;
static atomic_uint feeds, packets, last_loss, completed, encodes, encoder_mode;
static atomic_int sink_result;
// Replaces legacy: tests/regression/test_voice_reliability.py::test_s3_audio_controls
static uint8_t last_packet[SAMPLES];
static uint32_t timestamps[16];
static int16_t clean[CAPTURE_SAMPLES];
static int64_t captured_at;
static bool pending_clean_frame;
static xiaotai_processor_stats_t processor_counters;
#if AUDIO_WORKERS_PCMA_LEGACY
static xiaotai_signal_t encoding, encode_resume, queue_wait, queue_resume;
static atomic_bool hold_encode, hold_receive;
static int16_t encoded_pcm[16][SAMPLES];
static uint32_t last_generation;
#endif

typedef struct { int64_t captured_us; uint32_t lost; } processed_t;

typedef enum { DATA, READ_ERROR, OVERRUN, FAIL_PROCESSOR, READ_CHECKPOINT,
               LEGACY_FIRST, LEGACY_SECOND, DIAG_RAMP, DIAG_SILENCE, DIAG_EXTREMA } sample_case_t;
static _Atomic(sample_case_t) read_case;
static atomic_uint overruns;
static unsigned diagnostic_blocks;

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    const xiaotai_err_t result = __real_xiaotai_task_create(config, task);
    if (result == XIAOTAI_OK && !strcmp(config->name, "xt_capture")) capture_task = *task;
    if (result == XIAOTAI_OK && !strcmp(config->name, "xt_clean")) clean_task = *task;
    if (result == XIAOTAI_OK && !strcmp(config->name, "xt_uplink")) {
        assert(config->stack_bytes >= 6656U + 2048U + 512U);
        uplink_task = *task;
    }
    return result;
}

static xiaotai_err_t input_side(void *self) { (void)self; return XIAOTAI_OK; }

static int16_t legacy_capture(size_t i) { return (int16_t)(i % 300U * 30U); }

static xiaotai_err_t input_read(void *self, void *dst, size_t size, size_t *written)
{
    (void)self;
    assert(xiaotai_task_is_current(&capture_task));
    assert(size == CAPTURE_SAMPLES * 3U * sizeof(int16_t));
    sample_case_t item;
    do {
        if (!xiaotai_queue_receive(&inputs, &item, 100)) return XIAOTAI_ERR_TIMEOUT;
        if (item == READ_CHECKPOINT) {
            xiaotai_signal_give(&read_entered);
            assert(xiaotai_signal_take(&read_release, WAIT));
        }
    } while (item == READ_CHECKPOINT);
    atomic_store(&read_case, item);
    if (item == READ_ERROR) return XIAOTAI_FAIL;
    if (item == OVERRUN) atomic_fetch_add(&overruns, 2);
    int16_t *pcm = dst;
    const int16_t ramp = item == DIAG_RAMP ? (int16_t)((int)diagnostic_blocks++ - 131) : 0;
    for (size_t i = 0; i < CAPTURE_SAMPLES; ++i) {
        if (item == DIAG_RAMP || item == DIAG_SILENCE || item == DIAG_EXTREMA) {
            pcm[i * 3U] = item == DIAG_EXTREMA && !i ? INT16_MIN : ramp;
            pcm[i * 3U + 1U] = item == DIAG_EXTREMA && !i ? INT16_MAX : ramp;
            pcm[i * 3U + 2U] = ramp;
        } else {
            pcm[i * 3U] = item == LEGACY_FIRST ? legacy_capture(i) :
                          item == LEGACY_SECOND ? legacy_capture(i + CAPTURE_SAMPLES) : 1000;
            pcm[i * 3U + 1U] = 2000;
            pcm[i * 3U + 2U] = 3000;
        }
    }
    *written = size;
    return XIAOTAI_OK;
}

static void input_stats(void *self, xiaotai_audio_stats_t *stats)
{
    (void)self;
    *stats = (xiaotai_audio_stats_t){.overruns = atomic_load(&overruns)};
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    static const xiaotai_audio_input_t part = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = CAPTURE_RATE, .channels = 3,
                 .roles = {XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE, XIAOTAI_CHANNEL_MONITOR}},
        .info = {.read_timeout_ms = 100, .block_frames = CAPTURE_SAMPLES},
        .open = input_side, .read = input_read, .close = input_side, .stats = input_stats,
    };
    return index ? NULL : &part;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index) { (void)index; return NULL; }

static int match(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{
    return in && in->channels == 3 && !out ? 100 : 0;
}

static xiaotai_err_t processor_open(const xiaotai_audio_input_t *in, const xiaotai_audio_output_t *out,
                                     xiaotai_processor_info_t *info, void **state)
{
    (void)in;
    (void)out;
    *info = (xiaotai_processor_info_t){.rate_hz = CAPTURE_RATE, .feed_frames = CAPTURE_SAMPLES,
                                       .fetch_max = CAPTURE_SAMPLES};
    if (AUDIO_WORKERS_ASYNC) info->flags |= XIAOTAI_PROCESSOR_ASYNC;
    *state = clean;
    return XIAOTAI_OK;
}

static xiaotai_err_t feed(void *state, const void *pcm, size_t size, int64_t end, uint32_t lost)
{
    assert(state == clean && size == 3U * CAPTURE_SAMPLES * sizeof(int16_t));
    if (atomic_load(&read_case) == FAIL_PROCESSOR) return XIAOTAI_ERR_INVALID_STATE;
    assert(xiaotai_task_is_current(&capture_task));
    if (AUDIO_WORKERS_ASYNC) {
        assert(xiaotai_queue_send(&processed, &(processed_t){.captured_us = end, .lost = lost}, 0));
    } else {
        const int16_t *input = pcm;
        for (size_t i = 0; i < CAPTURE_SAMPLES; ++i) clean[i] = input[i * 3U];
        captured_at = end;
    }
    atomic_store(&last_loss, lost);
    atomic_fetch_add(&feeds, 1);
    return XIAOTAI_OK;
}

static xiaotai_err_t fetch(void *state, xiaotai_processor_frame_t *frame, uint32_t timeout)
{
    assert(state == clean);
    if (AUDIO_WORKERS_ASYNC) {
        assert(timeout && xiaotai_task_is_current(&clean_task) && !xiaotai_task_is_current(&capture_task));
        if (pending_clean_frame) {
            pending_clean_frame = false;
            atomic_fetch_add(&completed, 1);
        }
        processed_t ready;
        if (!xiaotai_queue_receive(&processed, &ready, timeout)) return XIAOTAI_ERR_TIMEOUT;
        pending_clean_frame = true;
        for (size_t i = 0; i < CAPTURE_SAMPLES; ++i) clean[i] = 1000;
        *frame = (xiaotai_processor_frame_t){.encoding = XIAOTAI_CODEC_PCM16, .data = clean,
            .size = sizeof(clean), .samples = CAPTURE_SAMPLES, .captured_us = ready.captured_us,
            .lost_samples = ready.lost, .process_us = 25000};
        return XIAOTAI_OK;
    }
    assert(!timeout && xiaotai_task_is_current(&capture_task));
    *frame = (xiaotai_processor_frame_t){.encoding = XIAOTAI_CODEC_PCM16, .data = clean,
        .size = sizeof(clean), .samples = CAPTURE_SAMPLES, .captured_us = captured_at};
    return XIAOTAI_OK;
}

static void processor_close(void *state) { assert(state == clean); }

static void processor_stats(void *state, xiaotai_processor_stats_t *out)
{
    assert(state == clean);
    *out = processor_counters;
}

static bool is_ready(void *ctx, uint32_t generation)
{
    (void)ctx;
    assert(AUDIO_WORKERS_PCMA_LEGACY ? generation >= 7U : generation == 5U);
    return atomic_load(&peer_ready);
}

static xiaotai_err_t send(void *ctx, uint32_t gen, uint32_t stamp, const uint8_t *data, size_t size)
{
    (void)ctx;
    assert(xiaotai_task_is_current(&uplink_task) && !xiaotai_task_is_current(&capture_task) && !xiaotai_on_owner());
    assert((AUDIO_WORKERS_PCMA_LEGACY ? gen >= 7U : gen == 5U) && size == SAMPLES);
    if (atomic_exchange(&hold_send, false)) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&release, WAIT));
    }
    const unsigned slot = atomic_load(&packets);
    assert(slot < sizeof(timestamps) / sizeof(*timestamps));
    timestamps[slot] = stamp;
    memcpy(last_packet, data, size);
#if AUDIO_WORKERS_PCMA_LEGACY
    last_generation = gen;
#endif
    atomic_fetch_add(&packets, 1);
    xiaotai_signal_give(&sent);
    return atomic_load(&sink_result);
}

static void event(uint32_t gen, xiaotai_media_event_t ev, xiaotai_err_t ret, void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner() && (AUDIO_WORKERS_PCMA_LEGACY ? gen >= 7U : gen == 5U));
    if (ev == XIAOTAI_MEDIA_EVENT_STARTED) {
        assert(ret == XIAOTAI_OK);
        xiaotai_signal_give(&started);
    } else {
        assert(ev == XIAOTAI_MEDIA_EVENT_FAILED && ret == XIAOTAI_FAIL);
        xiaotai_signal_give(&failed);
    }
}

static void open_session(void *context)
{
    static const xiaotai_media_sink_t sink = {.ready = is_ready, .send = send, .send_stack_bytes = 6656};
    const xiaotai_media_session_t desc = {.generation = context ? *(const uint32_t *)context : 5U,
        .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .sink = &sink, .event = event,
        .duplex = AUDIO_WORKERS_PCMA_LEGACY != 0};
    xiaotai_media_sink_t oversize = sink;
    ++oversize.send_stack_bytes;
    xiaotai_media_session_t rejected = desc;
    rejected.sink = &oversize;
    assert(xiaotai_media_open(&rejected) == XIAOTAI_ERR_INVALID_SIZE);
    assert(xiaotai_media_open(&desc) == XIAOTAI_OK);
    xiaotai_signal_give(&owner);
}

static xiaotai_err_t setup(void)
{
    static const xiaotai_audio_processor_t processor = {.name = "capture.fixture", .match = match,
        .open = processor_open, .close = processor_close, .feed = feed, .fetch = fetch, .stats = processor_stats};
    const xiaotai_media_requirement_t req = {.uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K};
    assert(xiaotai_media_processor_register(&processor) == XIAOTAI_OK);
    return xiaotai_media_require(&req);
}

static int encoder_match(const xiaotai_media_format_t *fmt)
{
    return xiaotai_media_g711a_encoder.match(fmt) * 10;
}

static xiaotai_err_t encoder_open(const xiaotai_media_format_t *fmt, void **state)
{
    return xiaotai_media_g711a_encoder.open(fmt, state);
}

static void encoder_close(void *state) { xiaotai_media_g711a_encoder.close(state); }

static xiaotai_err_t encode(void *state, const int16_t *pcm, size_t samples, uint8_t *out, size_t capacity, size_t *size)
{
    assert(xiaotai_task_is_current(&uplink_task));
    const unsigned mode = atomic_load(&encoder_mode);
    const xiaotai_err_t err = xiaotai_media_g711a_encoder.encode(state, pcm, samples, out, capacity, size);
#if AUDIO_WORKERS_PCMA_LEGACY
    const unsigned slot = atomic_load(&encodes);
    assert(samples == SAMPLES && slot < sizeof(encoded_pcm) / sizeof(*encoded_pcm));
    memcpy(encoded_pcm[slot], pcm, sizeof(encoded_pcm[slot]));
    if (atomic_exchange(&hold_encode, false)) {
        xiaotai_signal_give(&encoding);
        assert(xiaotai_signal_take(&encode_resume, WAIT));
    }
#endif
    if (!AUDIO_WORKERS_ASYNC && mode == 3) assert(xiaotai_media_uplink_gate(5, false, 0) == XIAOTAI_OK);
    if (mode == 2) --*size;
    atomic_fetch_add(&encodes, 1);
    return mode == 1 ? XIAOTAI_FAIL : err;
}

static xiaotai_err_t register_test_encoder(void)
{
    static const xiaotai_audio_encoder_t encoder = {.name = "fixture.tx", .encoding = XIAOTAI_CODEC_G711A,
        .stack_bytes = 512, .match = encoder_match, .open = encoder_open, .encode = encode, .close = encoder_close};
    assert(xiaotai_media_encoder_register(&encoder) == XIAOTAI_OK);
#if AUDIO_WORKERS_PCMA_LEGACY
    static const xiaotai_media_param_t tuning[] = {{"uplink.sync_on_dequeue", 1}};
    assert(xiaotai_media_tune(tuning, sizeof(tuning) / sizeof(*tuning)) == XIAOTAI_OK);
#endif
    return setup();
}

static void sample(sample_case_t value) { assert(xiaotai_queue_send(&inputs, &value, 0)); }

static void check_afe_snapshot(void)
{
    xiaotai_media_stats_t read_before, read_after;
    sample(READ_CHECKPOINT);
    assert(xiaotai_signal_take(&read_entered, WAIT));
    xiaotai_media_stats(&read_before);
    sample(READ_ERROR);
    sample(READ_ERROR);
    sample(READ_CHECKPOINT);
    xiaotai_signal_give(&read_release);
    assert(xiaotai_signal_take(&read_entered, WAIT));

    processor_counters = (xiaotai_processor_stats_t){.feeds = 17, .frames = 34, .input_full = 4,
        .fetch_timeouts = 7, .process_max_us = 63125, .failed = true};
    xiaotai_processor_stats_t first, second;
    xiaotai_media_processor_stats(&first);
    xiaotai_media_stats(&read_after);
    assert(read_after.capture_errors - read_before.capture_errors == 2);
    assert(first.feeds == 17 && first.frames == 34 && first.input_full == 4);
    assert(first.fetch_timeouts == 7 && first.process_max_us == 63125 && first.failed);

    processor_counters.input_full = 9;
    processor_counters.failed = false;
    xiaotai_media_processor_stats(&second);
    assert(second.feeds == 17 && second.frames == 34 && second.input_full == 9);
    assert(second.fetch_timeouts == 7 && second.process_max_us == 63125 && !second.failed);
    assert(first.input_full == 4 && first.failed);
    sample(OVERRUN);
    xiaotai_signal_give(&read_release);
}

static void wait_feeds(unsigned expected)
{
    const int64_t deadline = xiaotai_time_us() + WAIT * 1000U;
    while ((atomic_load(&feeds) < expected || (AUDIO_WORKERS_ASYNC && atomic_load(&completed) < expected)) &&
           xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(atomic_load(&feeds) == expected);
    if (AUDIO_WORKERS_ASYNC) assert(atomic_load(&completed) == expected);
}

#if !AUDIO_WORKERS_PCMA_LEGACY
static xiaotai_eventlog_topic_snapshot_t capture_topic(void)
{
    xiaotai_eventlog_topic_snapshot_t topic;
    for (size_t i = 0; xiaotai_eventlog_topic_read(i, &topic); ++i) {
        if (!strcmp(topic.desc->name, "capture")) return topic;
    }
    assert(!"capture topic not registered");
    return (xiaotai_eventlog_topic_snapshot_t){0};
}

static void verify_capture_window(bool enabled)
{
    media_diag_level_window_t levels[4];
    assert(!media_diag_capture_levels_snapshot(levels));
    for (unsigned i = 1; i <= 96U; ++i) {
        sample(i <= 32U ? DIAG_RAMP : i == 65U ? DIAG_EXTREMA : DIAG_SILENCE);
        wait_feeds(i);
        sample(READ_CHECKPOINT);
        assert(xiaotai_signal_take(&read_entered, WAIT));
        const xiaotai_eventlog_topic_snapshot_t topic = capture_topic();
        assert(topic.posts == i / 32U);
        assert(media_diag_capture_levels_snapshot(levels) == (enabled && i >= 32U));
        if (i == 32U) {
            assert(topic.desc->count == 17);
            assert(!strcmp(topic.desc->fields[0], "mic_rms") && !strcmp(topic.desc->fields[1], "mic_peak"));
            assert(!strcmp(topic.desc->fields[13], "mic_dc_offset"));
            for (unsigned ch = 0; ch < 3U; ++ch) {
                assert(topic.values[2U * ch] == (enabled ? 115U : 100U));
                assert(topic.values[2U * ch + 1U] == (enabled ? 131U : 100U));
                assert(topic.values[13U + ch] == 32768 - 115);
                if (enabled) assert(levels[ch].rms_max == 131U);
            }
            assert(topic.values[6] == (AUDIO_WORKERS_ASYNC ? 1000U : enabled ? 115U : 100U));
            xiaotai_media_stats_t stats;
            xiaotai_media_stats(&stats);
            assert(stats.mic_rms == 100U && stats.mic_peak == 100U);
            printf("frame 32: window=%u async=%u rms=%u peak=%u dc=%d; live rms=%u peak=%u\n",
                   (unsigned)enabled, AUDIO_WORKERS_ASYNC, (unsigned)topic.values[0], (unsigned)topic.values[1],
                   (int)topic.values[13] - 32768, (unsigned)stats.mic_rms, (unsigned)stats.mic_peak);
        } else if (i > 32U && i < 64U && enabled) {
            assert(levels[0].rms == 115U && levels[0].peak == 131U && levels[0].dc == -115);
        } else if (i == 64U) {
            assert(topic.values[0] == 0 && topic.values[1] == 0 && topic.values[13] == 32768);
            if (enabled) assert(levels[0].rms == 0 && levels[0].rms_max == 0 && levels[0].peak == 0);
        } else if (i == 96U) {
            assert(topic.values[0] == (enabled ? 80U : 0U));
            assert(topic.values[1] == (enabled ? 32768U : 0U));
            assert(topic.values[13] == 32768 - 6 && topic.values[14] == 32768 + 6);
            assert(topic.values[4] == 0 && topic.values[5] == 0 && topic.values[15] == 32768);
            if (enabled) {
                assert(levels[0].rms_max == 2590U && levels[1].rms_max == 2590U);
                assert(levels[1].peak == 32767U);
                if (!AUDIO_WORKERS_ASYNC) assert(levels[3].rms_max == 2590U && levels[3].peak == 32768U);
            }
        }
        xiaotai_signal_give(&read_release);
    }
}
#endif

#if AUDIO_WORKERS_PCMA_LEGACY
bool __real_xiaotai_queue_receive(xiaotai_queue_t *queue, void *item, uint32_t wait_ms);
bool __wrap_xiaotai_queue_receive(xiaotai_queue_t *queue, void *item, uint32_t wait_ms);
bool __wrap_xiaotai_queue_receive(xiaotai_queue_t *queue, void *item, uint32_t wait_ms)
{
    if (atomic_load(&hold_receive) && xiaotai_task_is_current(&uplink_task) &&
        atomic_exchange(&hold_receive, false)) {
        xiaotai_signal_give(&queue_wait);
        assert(xiaotai_signal_take(&queue_resume, WAIT));
    }
    return __real_xiaotai_queue_receive(queue, item, wait_ms);
}

static void legacy_open(uint32_t generation)
{
    assert(xiaotai_run(open_session, &generation) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner, WAIT) && xiaotai_signal_take(&started, WAIT));
}

static void close_session(void *context)
{
    xiaotai_media_close(*(const uint32_t *)context);
    xiaotai_signal_give(&owner);
}

static void legacy_replace(uint32_t *generation)
{
    assert(xiaotai_run(close_session, generation) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner, WAIT));
    legacy_open(++*generation);
}

static void legacy_capture_barrier(void)
{
    sample(READ_CHECKPOINT);
    assert(xiaotai_signal_take(&read_entered, WAIT));
    xiaotai_signal_give(&read_release);
}

typedef struct { uint64_t fingerprint; int16_t first; } legacy_pcm_t;

/* FNV-1a over the 160 little-endian samples recorded from both legacy P4
 * workers for their original 640-sample ramp. */
static const legacy_pcm_t first = {UINT64_C(0xbed3f567e51bb4bc), 0};
static const legacy_pcm_t continued = {UINT64_C(0xb5a68b8afaf07c3c), 127};
static const legacy_pcm_t restarted = {UINT64_C(0xf17ae2837f5b7f17), 600};

static uint64_t pcm_fingerprint(const int16_t pcm[SAMPLES])
{
    uint64_t v = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < SAMPLES; ++i) {
        const uint16_t sample = (uint16_t)pcm[i];
        v = (v ^ (uint8_t)sample) * UINT64_C(1099511628211);
        v = (v ^ (uint8_t)(sample >> 8)) * UINT64_C(1099511628211);
    }
    return v;
}

static void compare_sent(uint32_t generation, unsigned encodes_expected, unsigned sends_expected,
                         legacy_pcm_t expected)
{
    assert(xiaotai_signal_take(&sent, WAIT));
    assert(last_generation == generation && atomic_load(&encodes) == encodes_expected &&
           atomic_load(&packets) == sends_expected);
    const int16_t *pcm = encoded_pcm[encodes_expected - 1U];
    assert(pcm[0] == expected.first && pcm_fingerprint(pcm) == expected.fingerprint);
    for (size_t i = 0; i < SAMPLES; ++i)
        assert(last_packet[i] == xiaotai_media_alaw_encode_sample(pcm[i]));
}

static xiaotai_media_stats_t tx_after(uint32_t successes, uint32_t errors, uint32_t failed_sends)
{
    xiaotai_media_stats_t stats;
    const int64_t deadline = xiaotai_time_us() + WAIT * 1000U;
    do {
        xiaotai_media_stats(&stats);
        if (stats.tx_sent == successes && stats.tx_encode_errors == errors && stats.tx_send_errors == failed_sends)
            return stats;
        assert(xiaotai_time_us() < deadline);
        xiaotai_delay_ms(1);
    } while (true);
}

static void restart_mic(void)
{
    assert(xiaotai_media_set_mic_muted(true) == XIAOTAI_OK);
    assert(xiaotai_media_set_mic_muted(false) == XIAOTAI_OK);
    /* Capture reports the end of a whole block; its start must be after unmute. */
    xiaotai_delay_ms(21);
}

static void pause_before_receive(void)
{
    atomic_store(&hold_receive, true);
    assert(xiaotai_signal_take(&queue_wait, WAIT));
}

static int compare_wait_probe(const char *name, unsigned slot, legacy_pcm_t reference)
{
    const int16_t observed = encoded_pcm[slot][0];
    const uint64_t fingerprint = pcm_fingerprint(encoded_pcm[slot]);
    printf("legacy %s during empty uplink wait: first sample old=%d new=%d "
           "fingerprint old=%016" PRIx64 " new=%016" PRIx64 "\n",
           name, reference.first, observed, reference.fingerprint, fingerprint);
    return observed != reference.first || fingerprint != reference.fingerprint;
}

static void legacy_temporal_gate(uint32_t *generation, bool change_generation)
{
    const unsigned before_encode = atomic_load(&encodes);
    const unsigned before_send = atomic_load(&packets);
    atomic_store(&hold_encode, true);
    sample(LEGACY_FIRST);
    assert(xiaotai_signal_take(&encoding, WAIT));
    sample(LEGACY_FIRST); /* A not-yet-encoded frame of the same old generation/epoch. */
    legacy_capture_barrier();
    if (change_generation) legacy_replace(generation);
    else restart_mic();
    sample(LEGACY_SECOND);
    legacy_capture_barrier();
    xiaotai_signal_give(&encode_resume);
    compare_sent(*generation, before_encode + 2U, before_send + 1U, restarted);
}

static int legacy_wait_probe(uint32_t generation, bool change_generation)
{
    /* Advance filter history, then change the epoch after the sender's old
     * view was read but before its empty receive can return the next frame. */
    sample(LEGACY_FIRST);
    compare_sent(generation, 1, 1, first);
    pause_before_receive();
    if (change_generation) legacy_replace(&generation);
    else restart_mic();
    sample(LEGACY_SECOND);
    legacy_capture_barrier();
    xiaotai_signal_give(&queue_resume);
    assert(xiaotai_signal_take(&sent, WAIT));
    assert(last_generation == generation && atomic_load(&encodes) == 2U && atomic_load(&packets) == 2U);
    return compare_wait_probe(change_generation ? "generation" : "mute epoch", 1U, restarted);
}

static int verify_legacy_pcma(const char *probe)
{
    uint32_t generation = 7;
    atomic_store(&peer_ready, true);
    if (probe && !strcmp(probe, "start-wait")) {
        pause_before_receive();
        legacy_open(generation);
        sample(LEGACY_FIRST);
        legacy_capture_barrier();
        xiaotai_signal_give(&queue_resume);
        compare_sent(generation, 1, 1, first);
        sample(LEGACY_SECOND);
        assert(xiaotai_signal_take(&sent, WAIT));
        assert(last_generation == generation && atomic_load(&encodes) == 2U && atomic_load(&packets) == 2U);
        assert(timestamps[1] - timestamps[0] == 20U);
        return compare_wait_probe("startup", 1U, continued);
    }
    legacy_open(generation);
    /* Establish the active snapshot for the normal checks. The probes
     * separately inject transitions while the sender waits on an old view. */
    pause_before_receive();
    xiaotai_signal_give(&queue_resume);
    pause_before_receive();
    xiaotai_signal_give(&queue_resume);
    if (probe) {
        assert(!strcmp(probe, "generation-wait") || !strcmp(probe, "mute-wait"));
        return legacy_wait_probe(generation, !strcmp(probe, "generation-wait"));
    }

    sample(LEGACY_FIRST);
    compare_sent(generation, 1, 1, first);
    sample(LEGACY_SECOND);
    compare_sent(generation, 2, 2, continued);
    assert(timestamps[1] - timestamps[0] == 20U);
    (void)tx_after(2, 0, 0);
    puts("PASS case 0: two 20 ms frames, exact legacy PCM and 160-byte packets");

    assert(xiaotai_media_set_mic_muted(true) == XIAOTAI_OK);
    sample(LEGACY_FIRST);
    legacy_capture_barrier();
    assert(!xiaotai_signal_take(&sent, 20) && atomic_load(&encodes) == 2U);
    assert(xiaotai_media_set_mic_muted(false) == XIAOTAI_OK);
    assert(xiaotai_media_uplink_gate(generation, false, 0) == XIAOTAI_OK);
    sample(LEGACY_FIRST);
    legacy_capture_barrier();
    assert(!xiaotai_signal_take(&sent, 20) && atomic_load(&encodes) == 2U);
    assert(xiaotai_media_uplink_gate(generation, true, 0) == XIAOTAI_OK);
    xiaotai_delay_ms(21);
    puts("PASS cases 3,4: microphone mute and closed PTT do not encode or send");

    for (unsigned mode = 1; mode <= 2; ++mode) {
        atomic_store(&encoder_mode, mode);
        sample(LEGACY_FIRST);
        (void)tx_after(2, mode, 0);
        assert(atomic_load(&encodes) == 2U + mode && atomic_load(&packets) == 2U);
    }
    atomic_store(&encoder_mode, 0);
    atomic_store(&sink_result, XIAOTAI_FAIL);
    sample(LEGACY_FIRST);
    assert(xiaotai_signal_take(&sent, WAIT));
    (void)tx_after(2, 2, 1);
    assert(atomic_load(&encodes) == 5U && atomic_load(&packets) == 3U);
    atomic_store(&sink_result, XIAOTAI_OK);
    sample(LEGACY_FIRST);
    assert(xiaotai_signal_take(&sent, WAIT));
    (void)tx_after(3, 2, 1);
    assert(atomic_load(&encodes) == 6U && atomic_load(&packets) == 4U);
    puts("PASS cases 5,6,10: encoding error, short output, and send failure leave successful accounting intact");

    legacy_temporal_gate(&generation, true);
    legacy_temporal_gate(&generation, false);
    puts("PASS cases 1,2,7,8,9: reject queued and in-encode stale audio; the next frame starts a fresh FIR");
    return 0;
}
#endif

int main(int argc, char **argv)
{
#if !AUDIO_WORKERS_PCMA_LEGACY
    assert(argc == 1 || (argc == 2 && (!strcmp(argv[1], "window") || !strcmp(argv[1], "window_off"))));
    const bool window = argc == 2;
    const bool window_enabled = window && !strcmp(argv[1], "window");
#endif
    xiaotai_signal_t *const signals[] = {&owner, &started, &sent, &entered, &release, &failed,
        &read_entered, &read_release,
#if AUDIO_WORKERS_PCMA_LEGACY
        &encoding, &encode_resume, &queue_wait, &queue_resume,
#endif
    };
    int result = 0;
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) assert(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    assert(xiaotai_queue_init(&inputs, 8, sizeof(sample_case_t), XIAOTAI_MEM_INTERNAL) == XIAOTAI_OK);
    if (AUDIO_WORKERS_ASYNC) assert(xiaotai_queue_init(&processed, 8, sizeof(processed_t), XIAOTAI_MEM_INTERNAL) == XIAOTAI_OK);
    static const xiaotai_module_t media = {.name = "media", .init = xiaotai_engine_media_init, .start = xiaotai_engine_media_start};
    static const char *const deps[] = {"media", NULL};
    static const xiaotai_module_t fixture = {.name = "fixture.test", .depends = deps, .init = register_test_encoder};
    const xiaotai_module_t *const modules[] = {&media, &fixture};
    xiaotai_framework_config_t fw = {.modules = modules, .module_count = 2, .queue_depth = 8};
#if !AUDIO_WORKERS_PCMA_LEGACY
    static const xiaotai_module_t eventlog = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    const xiaotai_module_t *const diagnostic_modules[] = {&eventlog, &at, &media, &fixture};
    static const xiaotai_media_param_t tuning[] = {{"diagnostics.capture_window", 1}};
    if (window) {
        assert(xiaotai_at_configure(&(const xiaotai_at_config_t){
            .task_stack_bytes = XIAOTAI_AT_NO_TASK, .no_platform_console = true,
        }) == XIAOTAI_OK);
        if (window_enabled) assert(xiaotai_media_tune(tuning, 1) == XIAOTAI_OK);
        fw.modules = diagnostic_modules;
        fw.module_count = sizeof(diagnostic_modules) / sizeof(*diagnostic_modules);
    }
#endif
    assert(xiaotai_framework_start(&fw) == XIAOTAI_OK);
#if !AUDIO_WORKERS_PCMA_LEGACY
    if (window) {
        verify_capture_window(window_enabled);
        goto cleanup;
    }
#endif
    sample(DATA);
    wait_feeds(1);
#if AUDIO_WORKERS_PCMA_LEGACY
    assert(argc <= 2);
    setvbuf(stdout, NULL, _IOLBF, 0);
    result = verify_legacy_pcma(argc == 2 ? argv[1] : NULL);
    goto cleanup;
#endif
    assert(xiaotai_run(open_session, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner, WAIT) && xiaotai_signal_take(&started, WAIT));
    sample(DATA);
    wait_feeds(2);
    assert(!atomic_load(&packets) && !atomic_load(&encodes));

    assert(xiaotai_media_uplink_gate(5, true, 0) == XIAOTAI_OK);
    sample(DATA); /* Sink still not ready: encode/send are both skipped. */
    wait_feeds(3);
    assert(!atomic_load(&packets) && !atomic_load(&encodes));
    atomic_store(&peer_ready, true);
    sample(DATA);
    assert(xiaotai_signal_take(&sent, WAIT));
    assert(atomic_load(&packets) == 1 && last_packet[SAMPLES - 1U] == xiaotai_media_alaw_encode_sample(1000));
    check_afe_snapshot();
    assert(xiaotai_signal_take(&sent, WAIT));
    assert(atomic_load(&last_loss) >= SAMPLES * 3U);
    assert(timestamps[1] - timestamps[0] >= 80);
    xiaotai_media_stats_t stats;
    xiaotai_media_stats(&stats);
    assert(stats.capture_errors && stats.capture_overruns == 2 && stats.capture_lost_samples >= SAMPLES * 3U);
    assert(stats.mic_peak == 1000 && stats.reference_peak == 2000 && stats.monitor_peak == 3000);
    if (AUDIO_WORKERS_ASYNC) assert(stats.capture_deadline_misses && stats.capture_process_max_us == 25000);

    atomic_store(&hold_send, true);
    sample(DATA);
    assert(xiaotai_signal_take(&entered, WAIT));
    sample(DATA);
    wait_feeds(7);
    assert(xiaotai_media_uplink_gate(5, false, 0) == XIAOTAI_OK);
    xiaotai_signal_give(&release);
    assert(xiaotai_signal_take(&sent, WAIT));
    assert(atomic_load(&packets) == 3);
    sample(DATA);
    wait_feeds(8);
    assert(!xiaotai_signal_take(&sent, 20));
    assert(xiaotai_media_uplink_gate(5, true, 0) == XIAOTAI_OK);
    assert(xiaotai_media_uplink_tone(5, 20) == XIAOTAI_OK);
    sample(DATA);
    assert(xiaotai_signal_take(&sent, WAIT));
    assert(last_packet[0] != xiaotai_media_alaw_encode_sample(1000));
    xiaotai_media_stats(&stats);
    assert(stats.tx_injected == 1 && stats.ptt_opens == 2 && stats.ptt_closes == 1);
    atomic_store(&hold_send, true);
    sample(DATA);
    assert(xiaotai_signal_take(&entered, WAIT));
    sample(DATA);
    wait_feeds(11);
    xiaotai_delay_ms(5);
    assert(xiaotai_media_uplink_gate(5, false, 100) == XIAOTAI_OK);
    sample(DATA); /* This block was captured after closing; it must not send. */
    xiaotai_signal_give(&release);
    const int64_t deadline = xiaotai_time_us() + WAIT * 1000U;
    while (atomic_load(&packets) < 6U && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(atomic_load(&packets) == 6U);
    wait_feeds(12);
    xiaotai_delay_ms(25);
    xiaotai_media_stats(&stats);
    assert(atomic_load(&packets) == 6U && stats.ptt_tail_packets == 1);
    assert(xiaotai_media_uplink_gate(5, true, 0) == XIAOTAI_OK);
    atomic_store(&sink_result, XIAOTAI_FAIL);
    sample(DATA);
    while (atomic_load(&packets) < 7U) assert(xiaotai_signal_take(&sent, WAIT));
    xiaotai_delay_ms(20);
    xiaotai_media_stats(&stats);
    assert(stats.tx_send_errors == 1 && atomic_load(&packets) == 7U);
    if (!AUDIO_WORKERS_ASYNC) {
        atomic_store(&sink_result, XIAOTAI_OK);
        const unsigned previous = atomic_load(&encodes);
        for (unsigned mode = 1; mode <= 3; ++mode) {
            atomic_store(&encoder_mode, mode);
            sample(DATA);
            const int64_t encoded_deadline = xiaotai_time_us() + WAIT * 1000U;
            while (atomic_load(&encodes) < previous + mode && xiaotai_time_us() < encoded_deadline) xiaotai_delay_ms(1);
            assert(atomic_load(&encodes) == previous + mode);
            xiaotai_delay_ms(20);
            assert(atomic_load(&packets) == 7U);
        }
        xiaotai_media_stats(&stats);
        assert(stats.tx_encode_errors == 2 && stats.tx_stale && stats.ptt_closes == 3);
    }
    sample(FAIL_PROCESSOR);
    assert(xiaotai_signal_take(&failed, WAIT));
    xiaotai_media_status_t status;
    xiaotai_media_status(&status);
    assert(!status.ready && !status.uplink_open);
cleanup:
    xiaotai_media_reset();
#if !AUDIO_WORKERS_PCMA_LEGACY
    if (window) {
        xiaotai_at_reset();
        xiaotai_eventlog_reset();
    }
#endif
    xiaotai_framework_reset();
    xiaotai_queue_deinit(&inputs);
    if (AUDIO_WORKERS_ASYNC) xiaotai_queue_deinit(&processed);
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) xiaotai_signal_deinit(signals[i]);
    return result;
}
