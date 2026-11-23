// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_rtc/test_rtc.c::rtc
// Replaces legacy: tests/xmini/test_audio_stream.c::audio_stream
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "audio_workers.h"
#include "g711.h"
#include "pacer.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/media_uplink.h"
#include "xiaotai/port/os.h"

enum {
    RATE = 8000, SAMPLES = 160, PERIOD_MS = 20, WAIT_MS = 3000, GEN = 7,
    RX_PRIME = 6, DUPLEX_BLOCKS = 500, MIC_SAMPLE = 12000, CLIP_SAMPLE = 3200,
    SEND_STALL_MS = 200, GEN_STALL = 8, GEN_NEXT = 9,
};

typedef struct {
    xiaotai_task_t handle;
    xiaotai_task_fn_t entry;
    void *arg;
    xiaotai_signal_t created;
    uint32_t stack_bytes;
    unsigned starts;
} task_probe_t;

static task_probe_t capture, uplink;
static xiaotai_queue_t hardware;
static xiaotai_signal_t release_sender, clock_done, owner_done, started, drained, cue_done, record_done;
static atomic_bool clock_stop, input_open, output_open, speaker_on, feed_rx, check_speaker, hold_sender;
static atomic_uint clock_ticks, reads, hw_overruns, steady_blocks, cue_blocks, sends, tx_with_signal;
static atomic_uint rx_offered, tx_queue_alloc, tx_queue_depth, tx_queue_item;
static _Atomic(xiaotai_queue_t *) send_queue;
static xiaotai_signal_t send_entered, send_resume;
static xiaotai_signal_t read_entered, read_resume;
static atomic_bool hold_send_call, hold_read_call;
static atomic_uint calls_for_old, calls_for_next, peer_generation;
static xiaotai_task_t executor_task;
static xiaotai_signal_t executor_wake, executor_done;
static atomic_bool executor_stop;
static bool delegated;
static bool test_tx_age, expire_tx_age;
static atomic_int_fast64_t tx_age_clock;
static atomic_uint tx_age_processed_reads;
static enum { REGULAR, COMPACT, COMPACT_DECODER, COMPACT_OVERRIDE } stack_case;
static unsigned last_write_read, playout_starts, separate_clean;
static uint32_t last_sent_stamp, last_sent_generation;
static bool provisioning;
static uint8_t pcma[SAMPLES];
static int16_t decoded[SAMPLES], recorded[SAMPLES * 2];
static xiaotai_err_t cue_result;
static uint32_t cue_id, record_id;

bool __wrap_xiaotai_net_provisioning_boot(void);
bool __wrap_xiaotai_mem_has_external(void);
bool __wrap_xiaotai_net_provisioning_boot(void) { return provisioning; }
bool __wrap_xiaotai_mem_has_external(void) { return false; }
int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    const int64_t mocked = test_tx_age ? atomic_load(&tx_age_clock) : 0;
    return mocked ? mocked : __real_xiaotai_time_us();
}

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task);
xiaotai_err_t __real_xiaotai_queue_init(xiaotai_queue_t *queue, size_t depth, size_t item, xiaotai_mem_kind_t storage);
xiaotai_err_t __wrap_xiaotai_queue_init(xiaotai_queue_t *queue, size_t depth, size_t item, xiaotai_mem_kind_t storage);
bool __real_xiaotai_queue_receive(xiaotai_queue_t *queue, void *item, uint32_t wait);
bool __wrap_xiaotai_queue_receive(xiaotai_queue_t *queue, void *item, uint32_t wait);

static void run_media_task(void *arg)
{
    task_probe_t *probe = arg;
    assert(xiaotai_signal_take(&probe->created, WAIT_MS));
    probe->entry(probe->arg);
}

xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task)
{
    task_probe_t *probe = NULL;
    if (!strcmp(cfg->name, "xt_capture")) {
        probe = &capture;
        static const uint32_t expected[] = {6144, 3584, 6656, 4608};
        assert(cfg->priority == 6 && cfg->core == 1 && cfg->stack_bytes == expected[stack_case]);
    } else if (!strcmp(cfg->name, "xt_uplink")) {
        probe = &uplink;
        assert(cfg->priority == 5 && cfg->core == XIAOTAI_CORE_ANY && cfg->stack_bytes == 3072);
    } else if (!strcmp(cfg->name, "xt_playout")) ++playout_starts;
    else if (!strcmp(cfg->name, "xt_clean")) ++separate_clean;
    if (!probe) return __real_xiaotai_task_create(cfg, task);

    const xiaotai_task_config_t launch = {.name = cfg->name, .entry = run_media_task, .arg = probe,
        .stack_bytes = cfg->stack_bytes, .stack = cfg->stack, .priority = cfg->priority, .core = cfg->core};
    probe->entry = cfg->entry;
    probe->arg = cfg->arg;
    probe->stack_bytes = cfg->stack_bytes;
    ++probe->starts;
    const xiaotai_err_t result = __real_xiaotai_task_create(&launch, task);
    if (result == XIAOTAI_OK) {
        probe->handle = *task;
        xiaotai_signal_give(&probe->created);
    }
    return result;
}

xiaotai_err_t __wrap_xiaotai_queue_init(xiaotai_queue_t *queue, size_t depth, size_t item, xiaotai_mem_kind_t storage)
{
    if (item == SAMPLES + 24U) {
        atomic_fetch_add(&tx_queue_alloc, 1);
        atomic_store(&tx_queue_depth, (unsigned)depth);
        atomic_store(&tx_queue_item, (unsigned)item);
        atomic_store(&send_queue, queue);
    }
    return __real_xiaotai_queue_init(queue, depth, item, storage);
}

bool __wrap_xiaotai_queue_receive(xiaotai_queue_t *queue, void *item, uint32_t wait)
{
    if (test_tx_age && queue == &hardware) atomic_store(&tx_age_processed_reads, atomic_load(&reads));
    const bool received = __real_xiaotai_queue_receive(queue, item, wait);
    if (received && queue == atomic_load(&send_queue) && atomic_exchange(&hold_sender, false)) {
        /* Delay the sender once after an actual dequeue, outside send().
         * This leaves idle park/resume intact and makes a full queue repeatable. */
        assert(xiaotai_task_is_current(&uplink.handle));
        assert(xiaotai_signal_take(&release_sender, WAIT_MS));
    }
    return received;
}

static void push_downlink(void)
{
    const unsigned sequence = atomic_fetch_add(&rx_offered, 1);
    const xiaotai_media_packet_t packet = {
        .encoding = XIAOTAI_CODEC_G711A, .rate_hz = RATE, .stream = 1, .timestamp_ms = sequence * PERIOD_MS,
        .data = pcma, .size = sizeof(pcma),
    };
    const uint32_t generation = atomic_load(&peer_generation);
    assert(xiaotai_media_push(generation ? generation : GEN, &packet));
}

static void device_clock(void *arg)
{
    (void)arg;
    for (unsigned ticket = 1; !atomic_load(&clock_stop); ++ticket) {
        xiaotai_delay_ms(PERIOD_MS);
        if (atomic_load(&feed_rx)) push_downlink();
        if (!test_tx_age && atomic_load(&input_open) && !xiaotai_queue_send(&hardware, &ticket, 0))
            atomic_fetch_add(&hw_overruns, 1);
        atomic_store(&clock_ticks, ticket);
    }
    xiaotai_signal_give(&clock_done);
}

#define ON_CAPTURE() assert(!xiaotai_on_owner() && xiaotai_task_is_current(&capture.handle))

static xiaotai_err_t in_open(void *ctx)
{
    (void)ctx;
    ON_CAPTURE();
    xiaotai_queue_reset(&hardware);
    assert(!atomic_exchange(&input_open, true));
    return XIAOTAI_OK;
}

static xiaotai_err_t in_close(void *ctx)
{
    (void)ctx;
    ON_CAPTURE();
    assert(atomic_exchange(&input_open, false));
    return XIAOTAI_OK;
}

static xiaotai_err_t in_read(void *ctx, void *pcm, size_t capacity, size_t *size)
{
    (void)ctx;
    ON_CAPTURE();
    unsigned tick;
    assert(atomic_load(&input_open) && capacity == SAMPLES * sizeof(int16_t));
    if (!xiaotai_queue_receive(&hardware, &tick, 100)) return XIAOTAI_ERR_TIMEOUT;
    assert(tick);
    if (atomic_exchange(&hold_read_call, false)) {
        xiaotai_signal_give(&read_entered);
        assert(xiaotai_signal_take(&read_resume, WAIT_MS));
    }
    int16_t *dst = pcm;
    /* Speech bursts leave an actual noise-floor sample for the echo gate;
     * recording outside the call always receives the full-level reference. */
    const int16_t amplitude = atomic_load(&feed_rx) && tick % 25U >= 20U ? 0 : MIC_SAMPLE;
    for (size_t i = 0; i < SAMPLES; ++i) dst[i] = (i & 1U) ? amplitude : -amplitude;
    atomic_fetch_add(&reads, 1);
    *size = capacity;
    return XIAOTAI_OK;
}

static void in_stats(void *ctx, xiaotai_audio_stats_t *stats)
{
    (void)ctx;
    const uint32_t lost = atomic_load(&hw_overruns);
    *stats = (xiaotai_audio_stats_t){.overruns = lost, .lost_frames = lost * SAMPLES};
}

static xiaotai_err_t out_open(void *ctx)
{
    (void)ctx;
    ON_CAPTURE();
    assert(!atomic_exchange(&output_open, true));
    return XIAOTAI_OK;
}

static xiaotai_err_t out_close(void *ctx)
{
    (void)ctx;
    ON_CAPTURE();
    assert(atomic_exchange(&output_open, false));
    atomic_store(&speaker_on, false);
    return XIAOTAI_OK;
}

static xiaotai_err_t out_enable(void *ctx, bool enabled)
{
    (void)ctx;
    ON_CAPTURE();
    assert(atomic_load(&output_open));
    atomic_store(&speaker_on, enabled);
    return XIAOTAI_OK;
}

static xiaotai_err_t out_write(void *ctx, void *data, size_t size)
{
    (void)ctx;
    ON_CAPTURE();
    assert(atomic_load(&output_open) && atomic_load(&speaker_on));
    assert(size && size <= sizeof(decoded) && size % sizeof(int16_t) == 0);
    const unsigned current_read = atomic_load(&reads);
    assert(current_read > last_write_read); /* At most one write per capture clock tick. */
    last_write_read = current_read;
    const int16_t *pcm = data;
    if (atomic_load(&check_speaker)) {
        assert(size == sizeof(decoded));
        const unsigned blocks = atomic_load(&steady_blocks);
        /* The first one can be faded. Every following block must be actual
         * downlink content, so the fixed pacer's silence cannot hide underruns. */
        if (blocks > 1) assert(!memcmp(pcm, decoded, size));
        atomic_store(&steady_blocks, blocks + 1U);
    } else if (pcm[size / sizeof(*pcm) - 1U] == CLIP_SAMPLE) atomic_fetch_add(&cue_blocks, 1);
    return XIAOTAI_OK;
}

static size_t out_flush(void *ctx) { (void)ctx; return SAMPLES; }

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    static const xiaotai_audio_input_t part = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1, .roles = {XIAOTAI_CHANNEL_MIC}},
        .info = {.group = 1, .block_frames = SAMPLES, .read_timeout_ms = 100, .echo_coupling_db = -40},
        .open = in_open, .read = in_read, .close = in_close, .stats = in_stats,
    };
    return index ? NULL : &part;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t part = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1, .roles = {XIAOTAI_CHANNEL_MONO}},
        .info = {.group = 1, .max_write_frames = SAMPLES, .flags = XIAOTAI_AUDIO_OUTPUT_PACED},
        .open = out_open, .write = out_write, .close = out_close, .set_enabled = out_enable, .flush_frames = out_flush,
    };
    return index ? NULL : &part;
}

static xiaotai_err_t send(void *ctx, uint32_t gen, uint32_t ts, const uint8_t *data, size_t size)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && xiaotai_task_is_current(delegated ? &executor_task : &uplink.handle) &&
           !xiaotai_task_is_current(&capture.handle));
    assert((gen == GEN || gen == GEN_STALL || gen == GEN_NEXT) && data && size == SAMPLES);
    atomic_fetch_add(gen == GEN_NEXT ? &calls_for_next : &calls_for_old, 1);
    if (last_sent_generation == gen && last_sent_stamp)
        assert(ts >= last_sent_stamp + PERIOD_MS && (ts - last_sent_stamp) % PERIOD_MS == 0);
    last_sent_generation = gen;
    last_sent_stamp = ts;
    if (atomic_exchange(&hold_send_call, false)) {
        uint8_t original[SAMPLES];
        memcpy(original, data, size);
        if (delegated) {
            xiaotai_media_reset(); /* Must reject waiting for its own admitted frame. */
            assert(xiaotai_media_can_send(&(xiaotai_media_format_t)XIAOTAI_MEDIA_FORMAT_G711A_8K));
        }
        xiaotai_signal_give(&send_entered);
        assert(xiaotai_signal_take(&send_resume, WAIT_MS));
        assert(!memcmp(original, data, size));
    }
    if (data[0] == xiaotai_media_alaw_encode_sample(-MIC_SAMPLE) &&
        data[SAMPLES - 1] == xiaotai_media_alaw_encode_sample(MIC_SAMPLE)) atomic_fetch_add(&tx_with_signal, 1);
    xiaotai_delay_ms(PERIOD_MS);
    atomic_fetch_add(&sends, 1);
    return XIAOTAI_OK;
}

static void session_event(uint32_t gen, xiaotai_media_event_t kind, xiaotai_err_t err, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && (gen == GEN || gen == GEN_STALL || gen == GEN_NEXT) && err == XIAOTAI_OK);
    if (kind == XIAOTAI_MEDIA_EVENT_STARTED) xiaotai_signal_give(&started);
    else {
        assert(kind == XIAOTAI_MEDIA_EVENT_DRAINED);
        xiaotai_signal_give(&drained);
    }
}

static void on_cue(uint32_t id, xiaotai_err_t err, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    cue_id = id;
    cue_result = err;
    xiaotai_signal_give(&cue_done);
}

static void on_record(uint32_t id, xiaotai_err_t err, uint32_t samples, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == recorded && err == XIAOTAI_OK && samples == 2U * SAMPLES);
    record_id = id;
    xiaotai_signal_give(&record_done);
}

static const xiaotai_media_sink_t sink = {.send = send};
static void wake_executor(void)
{
    if (!test_tx_age || atomic_load(&executor_stop)) xiaotai_signal_give(&executor_wake);
}
static bool is_executor(void) { return xiaotai_task_is_current(&executor_task); }
static const xiaotai_media_uplink_executor_t executor = {
    .sink = &sink, .wake = wake_executor, .is_current = is_executor,
};

static void run_executor(void *arg)
{
    (void)arg;
    while (!atomic_load(&executor_stop)) {
        (void)xiaotai_signal_take(&executor_wake, XIAOTAI_WAIT_FOREVER);
        xiaotai_media_uplink_work_t work = XIAOTAI_MEDIA_UPLINK_PENDING;
        while (!atomic_load(&executor_stop) && work != XIAOTAI_MEDIA_UPLINK_IDLE) {
            work = xiaotai_media_uplink_step(&executor);
            if (work == XIAOTAI_MEDIA_UPLINK_CONTENDED) xiaotai_delay_ms(1);
        }
        if (test_tx_age) xiaotai_signal_give(&release_sender);
    }
    xiaotai_signal_give(&executor_done);
}

typedef struct {
    enum { OPEN, ACTIVATE, DRAIN, CLOSE, RECORD, ECHO_OFF, ECHO_ON } op;
    xiaotai_err_t ret;
    uint32_t id, generation;
} owner_call_t;

static void on_owner(void *arg)
{
    assert(xiaotai_on_owner());
    owner_call_t *call = arg;
    const uint32_t generation = call->generation ? call->generation : GEN;
    xiaotai_media_session_t session = {
        .generation = generation, .flags = XIAOTAI_MEDIA_HOLD, .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
        .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .latency = XIAOTAI_MEDIA_LATENCY_LOW,
        .downlink_burst_ms = 1000, .duplex = true, .sink = &sink, .event = session_event,
    };
    if (test_tx_age) {
        session.downlink = (xiaotai_media_format_t){0};
        session.downlink_burst_ms = 0;
        session.duplex = false;
    }
    const xiaotai_media_record_t request = {.pcm = recorded, .samples = 2U * SAMPLES, .done = on_record, .ctx = recorded};
    switch (call->op) {
        case OPEN: call->ret = xiaotai_media_open(&session); break;
        case ACTIVATE: call->ret = xiaotai_media_activate(generation); break;
        case DRAIN: call->ret = xiaotai_media_drain(generation, WAIT_MS); break;
        case CLOSE: xiaotai_media_close(generation); break;
        case RECORD: call->ret = xiaotai_media_record(&request, &call->id); break;
        case ECHO_OFF:
        case ECHO_ON:
            call->ret = xiaotai_media_echo_gate_set(call->op == ECHO_ON);
            assert(call->ret != XIAOTAI_OK || xiaotai_media_echo_gate_enabled() == (call->op == ECHO_ON));
            break;
    }
    xiaotai_signal_give(&owner_done);
}

static owner_call_t call(owner_call_t call)
{
    assert(xiaotai_run(on_owner, &call) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_done, WAIT_MS));
    return call;
}

static xiaotai_media_status_t status(void)
{
    xiaotai_media_status_t snapshot;
    xiaotai_media_status(&snapshot);
    return snapshot;
}

#define AWAIT(condition, ms) do {                                                               \
    const int64_t limit = __real_xiaotai_time_us() + 1000 * (int64_t)(ms);                       \
    while (!(condition) && __real_xiaotai_time_us() < limit) xiaotai_delay_ms(1);                 \
    assert(condition);                                                                        \
} while (0)

static int prefer_decoder(const xiaotai_media_format_t *format) { return format->rate_hz == RATE ? 2 : 0; }

static xiaotai_err_t configure(void)
{
    static const xiaotai_media_param_t tune[] = {
        {"task.capture.priority", 6}, {"task.capture.core", 1},
        {"task.playout.priority", 4}, {"task.playout.core", 0},
        {"task.uplink.priority", 5}, {"task.uplink.sink_stack_bytes", XIAOTAI_MEDIA_SINK_STACK_DEFAULT},
        {"playout.pacer_policy", MEDIA_PACER_FIXED}, {"power.idle_close_ms", 1},
    };
    static const xiaotai_media_requirement_t requirement = {
        .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
        .latency = XIAOTAI_MEDIA_LATENCY_LOW, .downlink_burst_ms = 1000,
    };
    assert(xiaotai_media_tune(tune, sizeof(tune) / sizeof(*tune)) == XIAOTAI_OK);
    if (expire_tx_age) {
        static const xiaotai_media_param_t max_age = {"uplink.max_queue_age_ms", 100};
        assert(xiaotai_media_tune(&max_age, 1) == XIAOTAI_OK);
    }
    if (stack_case != REGULAR) {
        static const xiaotai_media_param_t invalid = {MEDIA_COMPACT_CAPTURE_STACK_KEY, 2};
        static const xiaotai_media_param_t compact = {MEDIA_COMPACT_CAPTURE_STACK_KEY, 1};
        assert(xiaotai_media_tune(&invalid, 1) == XIAOTAI_ERR_INVALID_ARG);
        assert(xiaotai_media_tune(&compact, 1) == XIAOTAI_OK);
    }
    if (stack_case == COMPACT_DECODER) {
        static xiaotai_audio_decoder_t decoder;
        decoder = xiaotai_media_g711a_decoder;
        decoder.name = "large-decoder";
        decoder.match = prefer_decoder;
        decoder.stack_bytes = 1536;
        assert(xiaotai_media_decoder_register(&decoder) == XIAOTAI_OK);
    }
    if (stack_case == COMPACT_OVERRIDE) {
        static const xiaotai_media_param_t large_stack = {"task.capture.stack_bytes", 4608};
        assert(xiaotai_media_tune(&large_stack, 1) == XIAOTAI_OK);
    }
    if (delegated) {
        const xiaotai_media_requirement_t optional = {.uplink = XIAOTAI_MEDIA_FORMAT_OPUS_16K};
        assert(xiaotai_media_require(&optional) == XIAOTAI_OK);
        assert(xiaotai_media_uplink_attach(&executor) == XIAOTAI_OK);
    }
    return xiaotai_media_require(&requirement);
}

static void boot(bool provisioning_boot)
{
    static const xiaotai_module_t events = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const char *const media_deps[] = {"eventlog", NULL};
    static const xiaotai_module_t media = {
        .name = "media", .depends = media_deps, .init = xiaotai_engine_media_init, .start = xiaotai_engine_media_start,
    };
    static const char *const deps[] = {"media", NULL};
    static const xiaotai_module_t fixture = {.name = "hardware.fixture", .depends = deps, .init = configure};
    static const xiaotai_module_t *const modules[] = {&events, &media, &fixture};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 3, .queue_depth = 16};
    provisioning = provisioning_boot;
    atomic_store(&clock_stop, false);
    const xiaotai_task_config_t tick = {.name = "i2s.fixture", .entry = device_clock, .stack_bytes = 2048,
        .stack = XIAOTAI_MEM_INTERNAL, .core = XIAOTAI_CORE_ANY};
    xiaotai_task_t clock_task;
    assert(xiaotai_task_create(&tick, &clock_task) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    AWAIT(status().ready, WAIT_MS);
    AWAIT(!atomic_load(&input_open) && !atomic_load(&output_open), WAIT_MS);
    assert(!playout_starts && !separate_clean);
}

static void shutdown(void)
{
    xiaotai_media_reset();
    atomic_store(&clock_stop, true);
    assert(xiaotai_signal_take(&clock_done, WAIT_MS));
    xiaotai_framework_reset();
    xiaotai_eventlog_reset();
    xiaotai_queue_reset(&hardware);
    assert(!atomic_load(&input_open) && !atomic_load(&output_open) && !atomic_load(&speaker_on));
}

static void check_record(void)
{
    const owner_call_t run = call((owner_call_t){.op = RECORD});
    assert(run.ret == XIAOTAI_OK && run.id);
    assert(xiaotai_signal_take(&record_done, WAIT_MS) && record_id == run.id);
    for (size_t i = 0; i < sizeof(recorded) / sizeof(*recorded); ++i) {
        assert(recorded[i] == ((i & 1U) ? MIC_SAMPLE : -MIC_SAMPLE));
    }
    AWAIT(!atomic_load(&input_open) && !atomic_load(&output_open), WAIT_MS);
}

static uint32_t play_cue(bool repeat)
{
    static uint8_t little_endian[SAMPLES * 4U];
    static const xiaotai_sound_t cue = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1,
        .size = sizeof(little_endian), .data = little_endian};
    for (size_t i = 0; i < sizeof(little_endian); i += 2) {
        little_endian[i] = (uint8_t)CLIP_SAMPLE;
        little_endian[i + 1] = CLIP_SAMPLE >> 8;
    }
    const xiaotai_media_play_t request = {.clips = {{.sound = &cue}}, .clip_count = 1, .done = on_cue,
        .repeat = repeat ? XIAOTAI_PLAY_FOREVER : 0};
    uint32_t id = 0;
    assert(xiaotai_media_play_clips(&request, &id) == XIAOTAI_OK && id);
    return id;
}

static void assert_no_capture_damage(const xiaotai_media_stats_t *before, const xiaotai_media_stats_t *after)
{
    assert(after->capture_errors == before->capture_errors && after->capture_overruns == before->capture_overruns);
    assert(after->capture_lost_samples == before->capture_lost_samples && after->rx_underruns == before->rx_underruns);
    assert(!after->rx_dropped && !after->write_failed);
    assert(after->tx_dropped == after->tx_full);
}

static void check_duplex(void)
{
    assert(atomic_load(&tx_queue_alloc) == 1 && atomic_load(&tx_queue_depth) == 4 && atomic_load(&tx_queue_item) == 184);
    assert(capture.starts == 1 && uplink.starts == 1);
    assert(call((owner_call_t){.op = OPEN}).ret == XIAOTAI_OK);
    assert(status().generation == GEN && !status().active);
    const uint32_t armed_cue = play_cue(true);
    AWAIT(atomic_load(&cue_blocks) > 0, WAIT_MS);

    for (unsigned i = 0; i < RX_PRIME; ++i) push_downlink();
    atomic_store(&feed_rx, true);
    atomic_store(&hold_sender, true);
    assert(call((owner_call_t){.op = ACTIVATE}).ret == XIAOTAI_OK);
    assert(xiaotai_signal_take(&started, WAIT_MS));
    assert(xiaotai_signal_take(&cue_done, WAIT_MS) && cue_id == armed_cue && cue_result == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_media_stats_t before, after;
    xiaotai_media_stats(&before);
    const unsigned overrun_before = atomic_load(&hw_overruns);
    const int64_t t0 = xiaotai_time_us();
    atomic_store(&check_speaker, true);
    AWAIT(atomic_load(&steady_blocks) >= 8, WAIT_MS);
    xiaotai_media_stats(&after);
    assert(!atomic_load(&hold_sender) && !atomic_load(&sends) && after.tx_full > 0);
    assert_no_capture_damage(&before, &after);
    xiaotai_signal_give(&release_sender);
    AWAIT(atomic_load(&steady_blocks) >= DUPLEX_BLOCKS, 16000);
    const int64_t elapsed = xiaotai_time_us() - t0;
    xiaotai_media_stats(&after);
    assert(elapsed >= 9900000 && status().active && status().uplink_open);
    assert(atomic_load(&hw_overruns) == overrun_before && atomic_load(&sends) > 450);
    assert(after.pcm_written >= (DUPLEX_BLOCKS - 1U) * SAMPLES);
    assert(after.tx_send_max_us >= 20000 && after.tx_sent >= 450 && after.tx_full > 0);
    assert_no_capture_damage(&before, &after);
    printf("{\"test\":\"media_folded\",\"requested_capture_stack\":%u,\"requested_uplink_stack\":%u,"
           "\"tx_queue_bytes\":%u,\"elapsed_ms\":%lld,\"speaker_blocks\":%u,\"rx_underruns\":%u,"
           "\"capture_overruns\":%u,\"tx_sent\":%u,\"tx_full\":%u,\"tx_drop_other\":%u,"
           "\"tx_signal_blocks\":%u}\n",
           capture.stack_bytes, uplink.stack_bytes, atomic_load(&tx_queue_depth) * atomic_load(&tx_queue_item),
           (long long)(elapsed / 1000), atomic_load(&steady_blocks), after.rx_underruns, after.capture_overruns,
           after.tx_sent, after.tx_full, after.tx_dropped - after.tx_full, atomic_load(&tx_with_signal));
    fflush(stdout);
    assert(atomic_load(&tx_with_signal) > 300);

    atomic_store(&check_speaker, false);
    atomic_store(&feed_rx, false);
    const unsigned tick = atomic_load(&clock_ticks);
    AWAIT(atomic_load(&clock_ticks) > tick, WAIT_MS);
    assert(call((owner_call_t){.op = DRAIN}).ret == XIAOTAI_OK && xiaotai_signal_take(&drained, WAIT_MS));
    (void)call((owner_call_t){.op = CLOSE});
    AWAIT(!status().generation && !atomic_load(&speaker_on), WAIT_MS);
    AWAIT(!atomic_load(&input_open) && !atomic_load(&output_open), WAIT_MS);
}

static void check_stalled_send_and_generation(void)
{
    assert(!status().generation && !xiaotai_queue_count(atomic_load(&send_queue)));
    const unsigned resume_calls = atomic_load(&calls_for_old);
    const unsigned overruns = atomic_load(&hw_overruns);
    atomic_store(&hold_send_call, true);
    atomic_store(&steady_blocks, 0);
    atomic_store(&check_speaker, true);
    assert(call((owner_call_t){.op = OPEN, .generation = GEN_STALL}).ret == XIAOTAI_OK);
    atomic_store(&peer_generation, GEN_STALL);
    for (unsigned i = 0; i < RX_PRIME; ++i) push_downlink();
    atomic_store(&feed_rx, true);
    assert(call((owner_call_t){.op = ACTIVATE, .generation = GEN_STALL}).ret == XIAOTAI_OK);
    assert(xiaotai_signal_take(&started, WAIT_MS));
    assert(xiaotai_signal_take(&send_entered, WAIT_MS));

    xiaotai_media_stats_t before, after;
    xiaotai_media_stats(&before);
    const unsigned first_read = atomic_load(&reads);
    const unsigned first_write = atomic_load(&steady_blocks);
    const int64_t blocked_at = xiaotai_time_us();
    AWAIT(xiaotai_time_us() - blocked_at >= 1000 * SEND_STALL_MS &&
          atomic_load(&reads) >= first_read + SEND_STALL_MS / PERIOD_MS &&
          atomic_load(&steady_blocks) >= first_write + SEND_STALL_MS / PERIOD_MS, WAIT_MS);
    xiaotai_media_stats(&after);
    assert_no_capture_damage(&before, &after);
    assert(atomic_load(&hw_overruns) == overruns && after.capture_deadline_misses == before.capture_deadline_misses);
    assert(after.tx_full > before.tx_full && xiaotai_queue_count(atomic_load(&send_queue)) == 4);
    assert(atomic_load(&calls_for_old) == resume_calls + 1U);

    atomic_store(&check_speaker, false);
    atomic_store(&feed_rx, false);
    const unsigned tick = atomic_load(&clock_ticks);
    AWAIT(atomic_load(&clock_ticks) > tick, WAIT_MS);
    (void)call((owner_call_t){.op = CLOSE, .generation = GEN_STALL});
    assert(!status().generation);
    AWAIT(!atomic_load(&speaker_on), WAIT_MS);
    assert(atomic_load(&input_open)); /* Idle close must not outlive the consumer lease. */
    assert(call((owner_call_t){.op = OPEN, .generation = GEN_NEXT}).ret == XIAOTAI_OK);
    atomic_store(&peer_generation, GEN_NEXT);
    for (unsigned i = 0; i < RX_PRIME; ++i) push_downlink();
    assert(call((owner_call_t){.op = ACTIVATE, .generation = GEN_NEXT}).ret == XIAOTAI_OK);
    assert(xiaotai_signal_take(&started, WAIT_MS));
    assert(!atomic_load(&calls_for_next)); /* The only consumer is still stuck in the old send. */
    xiaotai_signal_give(&send_resume);
    AWAIT(atomic_load(&calls_for_next) >= 2, WAIT_MS);
    assert(atomic_load(&calls_for_old) == resume_calls + 1U);
    printf("{\"case\":\"blocked_send_and_generation\",\"stall_min_ms\":%d,"
           "\"queue_depth\":%zu,\"queue_full\":%u,\"capture_overruns\":%u,\"stale_sent\":0}\n",
           SEND_STALL_MS, (size_t)atomic_load(&tx_queue_depth), after.tx_full, after.capture_overruns);
    fflush(stdout);
    (void)call((owner_call_t){.op = CLOSE, .generation = GEN_NEXT});
    atomic_store(&peer_generation, 0);
    AWAIT(!status().generation && !atomic_load(&input_open) && !atomic_load(&output_open), WAIT_MS);
}

static void check_tx_queue_age(void)
{
    assert(call((owner_call_t){.op = OPEN}).ret == XIAOTAI_OK);
    assert(call((owner_call_t){.op = ACTIVATE}).ret == XIAOTAI_OK);
    assert(xiaotai_signal_take(&started, WAIT_MS));
    AWAIT(atomic_load(&input_open), WAIT_MS);
    assert(xiaotai_media_uplink_gate(GEN, true, 0) == XIAOTAI_OK && status().uplink_open);
    const unsigned ages_us[] = {99999U, 100000U, 0U};
    unsigned sent[3] = {0};
    const int64_t now = __real_xiaotai_time_us();
    /* The exact expiration case crosses the folded clock's low word wrap. */
    const int64_t baseline = (int64_t)(((uint64_t)now | UINT32_MAX) + 1U) - 450000;
    xiaotai_media_stats_t prior;
    xiaotai_media_stats(&prior);
    for (unsigned i = 0; i < sizeof(ages_us) / sizeof(*ages_us); ++i) {
        const int64_t enqueued = baseline + 400000 * (int64_t)i;
        atomic_store(&tx_age_clock, enqueued);
        /* The first read can have begun before activation; wait until the
         * capture task has processed each input before attempting another. */
        xiaotai_queue_t *const queue = atomic_load(&send_queue);
        for (unsigned j = 0; j < 4U && !xiaotai_queue_count(queue); ++j) {
            const unsigned tick = atomic_load(&reads) + 1U;
            assert(xiaotai_queue_send(&hardware, &tick, 0));
            AWAIT(atomic_load(&tx_age_processed_reads) >= tick, WAIT_MS);
        }
        assert(xiaotai_queue_count(queue) == 1U);
        const unsigned before = atomic_load(&sends);
        atomic_store(&tx_age_clock, enqueued + ages_us[i]);
        xiaotai_signal_give(&executor_wake);
        assert(xiaotai_signal_take(&release_sender, WAIT_MS));
        assert(!xiaotai_queue_count(atomic_load(&send_queue)) && status().generation == GEN);
        sent[i] = atomic_load(&sends) - before;
        printf("same-generation encoded uplink: age_us=%u sink_calls=%u\n", ages_us[i], sent[i]);
    }
    fflush(stdout);
    assert(sent[0] == 1U && sent[1] == (expire_tx_age ? 0U : 1U) && sent[2] == 1U);
    xiaotai_media_stats_t measured;
    xiaotai_media_stats(&measured);
    assert(measured.tx_queue_age_max_ms == 100U);
    assert(measured.tx_stale == prior.tx_stale + (unsigned)expire_tx_age);
    atomic_store(&tx_age_clock, 0);
    (void)call((owner_call_t){.op = CLOSE});
}

static void pause_codec_read(void)
{
    atomic_store(&hold_read_call, true);
    assert(xiaotai_signal_take(&read_entered, WAIT_MS));
    AWAIT(atomic_load(&sends) == atomic_load(&calls_for_old) && !xiaotai_queue_count(atomic_load(&send_queue)),
          WAIT_MS);
}

static void check_blocked_capture(void)
{
    assert(xiaotai_media_echo_gate_set(false) == XIAOTAI_ERR_INVALID_STATE);
    assert(call((owner_call_t){.op = OPEN, .generation = GEN}).ret == XIAOTAI_OK);
    assert(call((owner_call_t){.op = ACTIVATE, .generation = GEN}).ret == XIAOTAI_OK);
    assert(xiaotai_signal_take(&started, WAIT_MS));
    AWAIT(atomic_load(&sends) >= 2, WAIT_MS);
    pause_codec_read();
    xiaotai_processor_stats_t before, after;
    xiaotai_media_processor_stats(&before);
    assert(before.closed_frames && before.gain_q15 < 32768U && !atomic_load(&tx_with_signal));
    assert(call((owner_call_t){.op = ECHO_OFF}).ret == XIAOTAI_OK);
    assert(status().active && status().generation == GEN);
    xiaotai_signal_give(&read_resume);
    AWAIT(atomic_load(&tx_with_signal) >= 1, WAIT_MS);

    pause_codec_read();
    xiaotai_media_processor_stats(&after);
    assert(after.closed_frames == before.closed_frames && after.gain_q15 == 32768U);
    assert(call((owner_call_t){.op = ECHO_ON}).ret == XIAOTAI_OK);
    xiaotai_signal_give(&read_resume);
    const unsigned full = atomic_load(&tx_with_signal);
    const unsigned sent = atomic_load(&sends);
    AWAIT(atomic_load(&sends) > sent, WAIT_MS);

    pause_codec_read();
    xiaotai_media_processor_stats(&after);
    assert(after.closed_frames > before.closed_frames && after.gain_q15 < 32768U);
    assert(atomic_load(&tx_with_signal) == full);
    const unsigned old = atomic_load(&calls_for_old), acquired = atomic_load(&reads);
    (void)call((owner_call_t){.op = CLOSE, .generation = GEN});
    assert(!status().generation);
    xiaotai_signal_give(&read_resume);
    AWAIT(atomic_load(&reads) > acquired && !atomic_load(&input_open), WAIT_MS);
    assert(atomic_load(&calls_for_old) == old && atomic_load(&sends) == old);
    puts("Live echo bypass and restore use the next capture block; close during read submits zero old TX");
}

int main(int argc, char **argv)
{
    assert(argc <= 2);
    bool blocked_capture = false;
    if (argc == 2) {
        if (!strcmp(argv[1], "delegated")) delegated = true;
        else if (!strcmp(argv[1], "compact")) stack_case = COMPACT;
        else if (!strcmp(argv[1], "compact_decoder")) stack_case = COMPACT_DECODER;
        else if (!strcmp(argv[1], "compact_override")) stack_case = COMPACT_OVERRIDE;
        else if (!strcmp(argv[1], "blocked_capture")) blocked_capture = true;
        else if (!strcmp(argv[1], "tx_age")) expire_tx_age = test_tx_age = delegated = true;
        else if (!strcmp(argv[1], "tx_age_off")) test_tx_age = delegated = true;
        else assert(!"unknown test case");
    }
    for (size_t i = 0; i < SAMPLES; ++i) {
        pcma[i] = xiaotai_media_alaw_encode_sample((i & 1U) ? 4700 : -4700);
        decoded[i] = xiaotai_media_alaw_decode_sample(pcma[i]);
    }
    xiaotai_signal_t *const signals[] = {&capture.created, &uplink.created, &release_sender, &clock_done,
        &owner_done, &started, &drained, &cue_done, &record_done, &send_entered, &send_resume,
        &executor_wake, &executor_done, &read_entered, &read_resume};
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) assert(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    assert(xiaotai_queue_init(&hardware, 1, sizeof(unsigned), XIAOTAI_MEM_INTERNAL) == XIAOTAI_OK);
    if (delegated) {
        const xiaotai_task_config_t start = {.name = "tx.fixture", .entry = run_executor,
            .stack_bytes = 4096, .stack = XIAOTAI_MEM_INTERNAL, .core = XIAOTAI_CORE_ANY};
        assert(xiaotai_task_create(&start, &executor_task) == XIAOTAI_OK);
        boot(false);
        assert(capture.starts == 1 && !uplink.starts && atomic_load(&tx_queue_alloc) == 1);
        assert(xiaotai_media_uplink_step(&executor) == XIAOTAI_MEDIA_UPLINK_IDLE);
        if (test_tx_age) check_tx_queue_age();
        else check_stalled_send_and_generation();
        xiaotai_media_uplink_detach(&executor);
        shutdown();
        atomic_store(&executor_stop, true);
        wake_executor();
        assert(xiaotai_signal_take(&executor_done, WAIT_MS));
        goto done;
    }
    boot(false);
    if (blocked_capture) {
        check_blocked_capture();
        shutdown();
        goto done;
    }
    check_record();
    if (stack_case == COMPACT_DECODER || stack_case == COMPACT_OVERRIDE) {
        shutdown();
        goto done;
    }
    check_duplex();
    check_stalled_send_and_generation();
    shutdown();

    boot(true);
    assert(capture.starts == 2 && uplink.starts == 1 && atomic_load(&tx_queue_alloc) == 1);
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    const xiaotai_media_format_t format = XIAOTAI_MEDIA_FORMAT_G711A_8K;
    assert(info.capture_pcm && info.playback && !info.downlink_ms && !info.preroll_ms);
    assert(!xiaotai_media_can_send(&format) && !xiaotai_media_can_receive(&format));
    assert(call((owner_call_t){.op = OPEN}).ret == XIAOTAI_ERR_INVALID_STATE);
    const uint32_t cue = play_cue(false);
    assert(xiaotai_signal_take(&cue_done, WAIT_MS) && cue_id == cue && cue_result == XIAOTAI_OK);
    check_record();
    shutdown();
done:
    xiaotai_queue_deinit(&hardware);
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) xiaotai_signal_deinit(signals[i]);
    return 0;
}
