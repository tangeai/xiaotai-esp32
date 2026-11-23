// SPDX-License-Identifier: MIT
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pacer.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"

/* Production media and framework each own a process-wide singleton. Fork
 * before boot so two real instances can talk without a test-only runtime API. */
enum { RATE = 8000, FRAMES = 160, WAIT = 5000, AI20 = 20, AI40 = 40, CALL = 51, REPLACEMENT = 52 };
enum waveform { IGNORE, BURST, CONVERSATION };
enum lane { CAPTURE, PLAYOUT, UPLINK, LANE_COUNT };
enum operation {
    STATUS, OPEN_AI, OPEN_CALL, ACTIVATE, DRAIN, CLOSE, CUE, MUTE, UNMUTE,
    HOLD_SEND, RELEASE_SEND, SEND_BURST, SEND_STALE, SHUTDOWN,
};

typedef struct {
    enum operation op;
    uint32_t generation;
    unsigned value;
} request_t;

typedef struct {
    uint32_t generation, timestamp;
    uint16_t size;
    uint8_t data[400];
} wire_audio_t;

typedef struct {
    xiaotai_media_status_t status;
    xiaotai_media_stats_t stats;
    xiaotai_err_t result, cue_result;
    uint32_t started, drained, cue_id, cue_done_id, cue_callbacks;
    unsigned accepted, rejected, tx_entries, tx_delivered, capturing, matches, cue_blocks, last_gen;
    bool blocked, speaker;
    int64_t burst_us;
} reply_t;

static xiaotai_signal_t s_owner_done, s_release_send;
static atomic_bool s_input_open, s_output_open, s_speaker, s_hold_send, s_blocked;
static xiaotai_task_t s_task[LANE_COUNT];
static atomic_bool s_task_ready[LANE_COUNT];
static atomic_uint s_started, s_drained, s_cue_done, s_cue_callbacks, s_cue_blocks;
static atomic_uint s_capture, s_tx_entries, s_tx_delivered, s_matches, s_last_gen, s_variant, s_mic_variant;
static atomic_int s_cue_result, s_wave;
static uint32_t s_cue_id;
static unsigned s_side;
static int s_audio;

bool __wrap_xiaotai_net_provisioning_boot(void);
bool __wrap_xiaotai_net_provisioning_boot(void) { return false; }

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task)
{
    const xiaotai_err_t err = __real_xiaotai_task_create(cfg, task);
    static const char *const lanes[] = {"xt_capture", "xt_playout", "xt_uplink"};
    if (err == XIAOTAI_OK) for (unsigned i = 0; i < LANE_COUNT; ++i) {
        if (strcmp(cfg->name, lanes[i])) continue;
        assert(!atomic_load(&s_task_ready[i]));
        s_task[i] = *task;
        atomic_store(&s_task_ready[i], true);
    }
    return err;
}

static void assert_lane(enum lane lane)
{
    const int64_t deadline = xiaotai_time_us() + WAIT * INT64_C(1000);
    while (!atomic_load(&s_task_ready[lane]) && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(atomic_load(&s_task_ready[lane]) && !xiaotai_on_owner() && xiaotai_task_is_current(&s_task[lane]));
}

static uint8_t burst_code(size_t sample)
{
    return (uint8_t)((sample & 1U ? 0x80 : 0) | 0x10U | (sample * 5U + sample / 113U) % 16U);
}

static uint8_t mic_code(unsigned side, unsigned variant, unsigned sample)
{
    static const uint8_t tones[][2] = {{0x18, 0x26}, {0x2a, 0x07}, {0x39, 0x0c}};
    return (uint8_t)(tones[variant][side] | (sample & 1U ? 0x80 : 0));
}

/* Test oracle for G.711 A-law, independent of the engine's encoder/decoder. */
static int16_t decoded(uint8_t code)
{
    const unsigned raw = code ^ 0x55U;
    const unsigned band = (raw >> 4) & 7U;
    const int step = (int)((raw & 15U) * 16U);
    const int value = band == 0 ? step + 8 : (step + 264) << (band - 1U);
    return (int16_t)(raw & 0x80U ? value : -value);
}

static void put_message(int fd, const void *data, size_t size)
{
    ssize_t sent;
    do sent = send(fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL); while (sent < 0 && errno == EINTR);
    assert(sent == (ssize_t)size);
}

static void get_message(int fd, void *data, size_t size)
{
    struct pollfd watch = {.fd = fd, .events = POLLIN};
    assert(poll(&watch, 1, WAIT) == 1 && (watch.revents & POLLIN));
    assert(recv(fd, data, size, MSG_TRUNC) == (ssize_t)size);
}

static xiaotai_err_t input_open(void *ctx)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && !atomic_exchange(&s_input_open, true));
    return XIAOTAI_OK;
}

static xiaotai_err_t input_close(void *ctx)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && atomic_exchange(&s_input_open, false));
    return XIAOTAI_OK;
}

static xiaotai_err_t read_pcm(void *ctx, void *data, size_t capacity, size_t *size)
{
    (void)ctx;
    assert_lane(CAPTURE);
    assert(atomic_load(&s_input_open) && capacity == FRAMES * sizeof(int16_t));
    xiaotai_delay_ms(20);
    const unsigned variant = atomic_load(&s_mic_variant);
    int16_t *samples = data;
    for (unsigned i = 0; i < FRAMES; ++i) samples[i] = decoded(mic_code(s_side, variant, i));
    atomic_fetch_add(&s_capture, 1);
    *size = capacity;
    return XIAOTAI_OK;
}

static xiaotai_err_t output_open(void *ctx)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && !atomic_exchange(&s_output_open, true));
    return XIAOTAI_OK;
}

static xiaotai_err_t output_close(void *ctx)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && atomic_exchange(&s_output_open, false));
    atomic_store(&s_speaker, false);
    return XIAOTAI_OK;
}

static xiaotai_err_t enable_output(void *ctx, bool enabled)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && atomic_load(&s_output_open));
    atomic_store(&s_speaker, enabled);
    return XIAOTAI_OK;
}

static xiaotai_err_t write_pcm(void *ctx, void *data, size_t size)
{
    (void)ctx;
    assert_lane(PLAYOUT);
    assert(atomic_load(&s_speaker));
    assert(size && size <= FRAMES * sizeof(int16_t) && size % sizeof(int16_t) == 0);
    const int16_t *pcm = data;
    const unsigned n = (unsigned)(size / sizeof(*pcm));
    const enum waveform wave = atomic_load(&s_wave);
    const unsigned already = atomic_load(&s_matches);
    bool zero = true, cue = true;
    for (unsigned i = 0; i < n; ++i) {
        zero &= pcm[i] == 0;
        cue &= pcm[i] == 1800;
    }
    if (cue) atomic_fetch_add(&s_cue_blocks, 1);
    if (wave == BURST) {
        assert(!zero && already + n <= RATE);
        for (unsigned i = 0; i < n; ++i) {
            const unsigned pos = already + i;
            const int16_t clean = decoded(burst_code(pos));
            const int16_t expected = pos < 40U ? (int16_t)(clean * (int)(pos + 1U) / 40) : clean;
            if (pcm[i] != expected) fprintf(stderr, "AI endpoint=%u sample=%u actual=%d expected=%d\n",
                                            s_side, pos, pcm[i], expected);
            assert(pcm[i] == expected);
        }
        atomic_fetch_add(&s_matches, n);
    } else if (wave == CONVERSATION && !zero && !cue) {
        /* The first forty samples of a newly started stream may be faded.
         * Verify both the original and exact documented ramp, never silence. */
        assert(n == FRAMES);
        const unsigned variant = atomic_load(&s_variant);
        for (unsigned i = 0; i < n; ++i) {
            const int16_t clean = decoded(mic_code(1U - s_side, variant, i));
            assert(pcm[i] == clean || (i < 40U && pcm[i] == clean * (int)(i + 1U) / 40));
        }
        atomic_fetch_add(&s_matches, n);
    }
    return XIAOTAI_OK;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    static const xiaotai_audio_input_t part = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1, .roles = {XIAOTAI_CHANNEL_MIC}},
        .info = {.block_frames = FRAMES, .read_timeout_ms = 40},
        .open = input_open, .read = read_pcm, .close = input_close,
    };
    return index ? NULL : &part;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t part = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1, .roles = {XIAOTAI_CHANNEL_MONO}},
        .info = {.max_write_frames = FRAMES, .latency_ms = 20},
        .open = output_open, .write = write_pcm, .close = output_close, .set_enabled = enable_output,
    };
    return index ? NULL : &part;
}

static xiaotai_err_t on_send(void *ctx, uint32_t generation, uint32_t timestamp, const uint8_t *data, size_t size)
{
    (void)ctx;
    assert_lane(UPLINK);
    assert((generation == CALL || generation == REPLACEMENT) && size == FRAMES);
    const unsigned variant = atomic_load(&s_variant);
    wire_audio_t packet = {.generation = generation, .timestamp = timestamp, .size = FRAMES};
    for (unsigned i = 0; i < FRAMES; ++i) assert(data[i] == mic_code(s_side, variant, i));
    memcpy(packet.data, data, size);
    atomic_store(&s_last_gen, generation);
    atomic_fetch_add(&s_tx_entries, 1);
    if (atomic_exchange(&s_hold_send, false)) {
        atomic_store(&s_blocked, true);
        assert(xiaotai_signal_take(&s_release_send, WAIT));
        atomic_store(&s_blocked, false);
    }
    put_message(s_audio, &packet, sizeof(packet));
    atomic_fetch_add(&s_tx_delivered, 1);
    return XIAOTAI_OK;
}

static void session_event(uint32_t generation, xiaotai_media_event_t event, xiaotai_err_t error, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && generation && error == XIAOTAI_OK);
    if (event == XIAOTAI_MEDIA_EVENT_STARTED) atomic_store(&s_started, generation);
    else {
        assert(event == XIAOTAI_MEDIA_EVENT_DRAINED);
        atomic_store(&s_drained, generation);
    }
}

static void cue_done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && id != 0);
    atomic_store(&s_cue_result, result);
    atomic_store(&s_cue_done, id);
    atomic_fetch_add(&s_cue_callbacks, 1);
}

static xiaotai_err_t open_cue(void)
{
    static uint8_t pcm[FRAMES * 4U];
    static const xiaotai_sound_t sound = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1,
        .size = sizeof(pcm), .data = pcm};
    for (unsigned i = 0; i < sizeof(pcm); i += 2U) { pcm[i] = 8; pcm[i + 1] = 7; }
    const xiaotai_media_play_t cue = {.clips = {{.sound = &sound}}, .clip_count = 1, .done = cue_done,
        .priority = XIAOTAI_PLAY_NORMAL, .repeat = XIAOTAI_PLAY_FOREVER};
    uint32_t id = 0;
    const xiaotai_err_t err = xiaotai_media_play_clips(&cue, &id);
    if (err == XIAOTAI_OK) s_cue_id = id;
    return err;
}

typedef struct { request_t request; xiaotai_err_t result; } owner_job_t;

static void owner_call(void *ctx)
{
    owner_job_t *job = ctx;
    const request_t req = job->request;
    assert(xiaotai_on_owner());
    static const xiaotai_media_sink_t sink = {.send = on_send};
    switch (req.op) {
        case OPEN_AI:
        case OPEN_CALL: {
            const bool call = req.op == OPEN_CALL;
            const xiaotai_media_session_t cfg = {.generation = req.generation, .flags = XIAOTAI_MEDIA_HOLD,
                .uplink = call ? (xiaotai_media_format_t)XIAOTAI_MEDIA_FORMAT_G711A_8K : (xiaotai_media_format_t){0},
                .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .latency = XIAOTAI_MEDIA_LATENCY_LOW,
                .downlink_burst_ms = call ? 0 : 1000, .duplex = call, .event = session_event, .sink = call ? &sink : NULL};
            job->result = xiaotai_media_open(&cfg);
            if (job->result == XIAOTAI_OK) {
                atomic_store(&s_variant, req.value);
                atomic_store(&s_mic_variant, req.value);
                atomic_store(&s_matches, 0);
            }
            break;
        }
        case ACTIVATE:
            atomic_store(&s_wave, req.value);
            job->result = xiaotai_media_activate(req.generation);
            break;
        case DRAIN: job->result = xiaotai_media_drain(req.generation, WAIT); break;
        case CLOSE:
            xiaotai_media_close(req.generation);
            if (!req.value) atomic_store(&s_wave, IGNORE);
            break;
        case CUE: job->result = open_cue(); break;
        case MUTE:
            job->result = xiaotai_media_set_mic_muted(true);
            atomic_store(&s_mic_variant, 2);
            break;
        case UNMUTE:
            atomic_store(&s_mic_variant, atomic_load(&s_variant));
            job->result = xiaotai_media_set_mic_muted(false);
            break;
        default: assert(false);
    }
    xiaotai_signal_give(&s_owner_done);
}

static xiaotai_err_t configure(void)
{
    /* Keep the production processor/codecs/tasks; neutral settings let the
     * output prove sample conservation rather than compare echo policies. */
    static const xiaotai_media_param_t tune[] = {
        {"echo_gate.enabled", 0}, {"playout.pacer_policy", MEDIA_PACER_FIXED}, {"playout.mixer", 0},
    };
    static const xiaotai_media_requirement_t profile = {
        .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
        .latency = XIAOTAI_MEDIA_LATENCY_LOW, .downlink_burst_ms = 1000,
    };
    assert(xiaotai_media_tune(tune, sizeof(tune) / sizeof(*tune)) == XIAOTAI_OK);
    return xiaotai_media_require(&profile);
}

static void boot(void)
{
    static const xiaotai_module_t events = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const char *const media_deps[] = {"eventlog", NULL};
    static const xiaotai_module_t media = {.name = "media", .depends = media_deps,
        .init = xiaotai_engine_media_init, .start = xiaotai_engine_media_start};
    static const char *const fixture_deps[] = {"media", NULL};
    static const xiaotai_module_t fixture = {.name = "pipeline.fixture", .depends = fixture_deps, .init = configure};
    static const xiaotai_module_t *const modules[] = {&events, &media, &fixture};
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 3, .queue_depth = 16};
    assert(xiaotai_signal_init(&s_owner_done) == XIAOTAI_OK && xiaotai_signal_init(&s_release_send) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    const int64_t deadline = xiaotai_time_us() + WAIT * INT64_C(1000);
    xiaotai_media_status_t status;
    do {
        xiaotai_media_status(&status);
        if (status.ready) {
            xiaotai_media_info_t info;
            xiaotai_media_info(&info);
            assert(!info.mixer);
            return;
        }
        xiaotai_delay_ms(1);
    } while (xiaotai_time_us() < deadline);
    assert(false);
}

static bool accept_audio(reply_t *known)
{
    wire_audio_t incoming;
    const ssize_t n = recv(s_audio, &incoming, sizeof(incoming), MSG_TRUNC | MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
    if (!n) return false;
    assert(n == (ssize_t)sizeof(incoming) && incoming.size && incoming.size <= sizeof(incoming.data));
    const xiaotai_media_packet_t pkt = {.encoding = XIAOTAI_CODEC_G711A, .rate_hz = RATE,
        .stream = (uint16_t)(2U - s_side), .timestamp_ms = incoming.timestamp,
        .data = incoming.data, .size = incoming.size};
    if (xiaotai_media_push(incoming.generation, &pkt)) ++known->accepted;
    else ++known->rejected;
    return true;
}

static int64_t emit_burst(unsigned count)
{
    assert(count == AI20 || count == AI40);
    const unsigned frames = RATE / count;
    wire_audio_t packet = {.generation = count, .size = (uint16_t)frames};
    const int64_t started = xiaotai_time_us();
    for (unsigned i = 0; i < count; ++i) {
        packet.timestamp = i * 1000U / count;
        for (unsigned j = 0; j < frames; ++j) packet.data[j] = burst_code(i * frames + j);
        put_message(s_audio, &packet, sizeof(packet));
    }
    return xiaotai_time_us() - started;
}

static void respond(int ctrl, reply_t *reply)
{
    xiaotai_media_status(&reply->status);
    xiaotai_media_stats(&reply->stats);
    reply->started = atomic_load(&s_started);
    reply->drained = atomic_load(&s_drained);
    reply->cue_id = s_cue_id;
    reply->cue_done_id = atomic_load(&s_cue_done);
    reply->cue_result = atomic_load(&s_cue_result);
    reply->cue_callbacks = atomic_load(&s_cue_callbacks);
    reply->cue_blocks = atomic_load(&s_cue_blocks);
    reply->capturing = atomic_load(&s_capture);
    reply->matches = atomic_load(&s_matches);
    reply->tx_entries = atomic_load(&s_tx_entries);
    reply->tx_delivered = atomic_load(&s_tx_delivered);
    reply->last_gen = atomic_load(&s_last_gen);
    reply->blocked = atomic_load(&s_blocked);
    reply->speaker = atomic_load(&s_speaker);
    put_message(ctrl, reply, sizeof(*reply));
}

static int endpoint(unsigned side, int ctrl, int audio)
{
    s_side = side;
    s_audio = audio;
    boot();
    reply_t reply = {0};
    bool done = false, audio_live = true;
    while (!done) {
        struct pollfd watch[] = {{.fd = ctrl, .events = POLLIN}, {.fd = audio_live ? audio : -1, .events = POLLIN}};
        assert(poll(watch, 2, WAIT) > 0);
        if (watch[1].revents & POLLIN) audio_live = accept_audio(&reply);
        else if (watch[1].revents & POLLHUP) audio_live = false;
        if (!(watch[0].revents & POLLIN)) {
            assert(!(watch[0].revents & (POLLERR | POLLHUP)));
            continue;
        }
        request_t request;
        assert(recv(ctrl, &request, sizeof(request), MSG_TRUNC) == (ssize_t)sizeof(request));
        reply.result = XIAOTAI_OK;
        switch (request.op) {
            case STATUS: break;
            case HOLD_SEND: atomic_store(&s_hold_send, true); break;
            case RELEASE_SEND: xiaotai_signal_give(&s_release_send); break;
            case SEND_BURST: reply.burst_us = emit_burst(request.generation); break;
            case SEND_STALE: {
                wire_audio_t stale = {.generation = request.generation, .size = FRAMES, .timestamp = 180};
                memset(stale.data, 0xa5, FRAMES);
                put_message(audio, &stale, sizeof(stale));
                break;
            }
            case SHUTDOWN:
                xiaotai_media_reset();
                xiaotai_framework_reset();
                xiaotai_eventlog_reset();
                xiaotai_signal_deinit(&s_owner_done);
                xiaotai_signal_deinit(&s_release_send);
                assert(!atomic_load(&s_input_open) && !atomic_load(&s_output_open) && !atomic_load(&s_speaker));
                done = true;
                break;
            default: {
                owner_job_t job = {.request = request};
                assert(xiaotai_run(owner_call, &job) == XIAOTAI_OK);
                assert(xiaotai_signal_take(&s_owner_done, WAIT));
                reply.result = job.result;
                break;
            }
        }
        respond(ctrl, &reply);
    }
    close(ctrl);
    close(audio);
    return 0;
}

static int controls[2];

static reply_t command(unsigned side, enum operation op, uint32_t generation, unsigned value, xiaotai_err_t expected)
{
    const request_t request = {op, generation, value};
    reply_t reply;
    put_message(controls[side], &request, sizeof(request));
    get_message(controls[side], &reply, sizeof(reply));
    if (reply.result != expected) fprintf(stderr, "endpoint=%u op=%d actual=%d expected=%d\n", side, op, reply.result, expected);
    assert(reply.result == expected);
    return reply;
}

#define DO(side, op, gen, value) command(side, op, gen, value, XIAOTAI_OK)
#define AWAIT(side, result, condition) do {                                                   \
    const int64_t deadline = xiaotai_time_us() + WAIT * INT64_C(1000);                         \
    do {                                                                                     \
        result = DO(side, STATUS, 0, 0);                                                      \
        if (condition) break;                                                                \
        xiaotai_delay_ms(2);                                                                  \
    } while (xiaotai_time_us() < deadline);                                                  \
    assert(condition);                                                                       \
} while (0)

static void verify_burst(unsigned receiver, unsigned count)
{
    const unsigned sender = 1U - receiver;
    const unsigned entries = count * ((RATE / count + FRAMES - 1U) / FRAMES);
    const reply_t before = DO(receiver, OPEN_AI, count, 0);
    assert(before.status.generation == count && !before.status.active && !before.speaker);
    const int64_t send_us = DO(sender, SEND_BURST, count, 0).burst_us;
    reply_t r;
    AWAIT(receiver, r, r.accepted == before.accepted + count);
    assert(r.rejected == before.rejected && r.stats.rx_before_active == entries && r.stats.rx_received == entries);
    assert(!r.matches && !r.speaker && !r.stats.rx_dropped && r.stats.rx_packet_max_bytes == RATE / count);
    DO(receiver, ACTIVATE, count, BURST);
    AWAIT(receiver, r, r.started == count);
    DO(receiver, DRAIN, count, 0);
    AWAIT(receiver, r, r.drained == count);
    assert(r.matches == RATE && r.stats.rx_received == r.stats.rx_played && r.stats.rx_played == entries);
    assert(!r.stats.rx_dropped && !r.stats.pcm_pending && !r.stats.pcm_discarded && !r.stats.write_failed);
    assert(r.stats.pcm_decoded == RATE && r.stats.pcm_written == RATE);
    assert(r.stats.rx_bursts > 0 && !r.stats.rx_ts_repeats && !r.stats.rx_ts_backwards);
    printf("media_pipeline burst packets=%u entries=%u emit_us=%lld written_samples=%u drops=%u\n",
           count, entries, (long long)send_us, r.stats.pcm_written, r.stats.rx_dropped);
    DO(receiver, CLOSE, count, 0);
    AWAIT(receiver, r, !r.status.generation && !r.speaker);
}

static void verify_call(void)
{
    reply_t a, b;
    const uint32_t cue = DO(0, CUE, 0, 0).cue_id;
    assert(cue);
    AWAIT(0, a, a.cue_blocks >= 2);
    const reply_t armed = DO(0, OPEN_CALL, CALL, 0);
    assert(!armed.status.active && armed.status.playing == cue && armed.cue_callbacks == 0);
    AWAIT(0, a, a.cue_blocks >= armed.cue_blocks + 2);
    DO(1, OPEN_CALL, CALL, 0);
    DO(0, ACTIVATE, CALL, CONVERSATION);
    DO(1, ACTIVATE, CALL, CONVERSATION);
    AWAIT(0, a, a.started == CALL && a.matches >= 6 * FRAMES && a.cue_callbacks == 1);
    AWAIT(1, b, b.started == CALL && b.matches >= 6 * FRAMES);
    assert(a.cue_done_id == cue && a.cue_result == XIAOTAI_ERR_INVALID_STATE && !a.status.playing);
    const unsigned cue_writes = a.cue_blocks;
    assert(a.stats.tx_sent >= 6 && b.stats.tx_sent >= 6 && !a.stats.rx_dropped && !b.stats.rx_dropped);
    command(0, CUE, 0, 0, XIAOTAI_ERR_BUSY);
    assert(command(0, OPEN_CALL, REPLACEMENT, 0, XIAOTAI_ERR_INVALID_STATE).status.generation == CALL);

    DO(0, HOLD_SEND, 0, 0);
    AWAIT(0, a, a.blocked);
    const unsigned at_entry = a.tx_entries, queued_after = a.capturing + 4;
    AWAIT(0, a, a.capturing >= queued_after);
    assert(a.tx_entries == at_entry && a.tx_delivered + 1U == at_entry);
    a = DO(0, MUTE, 0, 0);
    assert(a.status.mic_muted && !a.status.uplink_open);
    DO(0, RELEASE_SEND, 0, 0);
    AWAIT(0, a, !a.blocked && a.tx_delivered == at_entry);
    const unsigned muted_capture = a.capturing + 12, playing = a.matches + 6U * FRAMES;
    AWAIT(0, a, a.capturing >= muted_capture && a.matches >= playing);
    assert(a.tx_entries == at_entry && a.tx_delivered == at_entry && a.status.active && !a.status.uplink_open);
    const unsigned peer_playing = DO(1, STATUS, 0, 0).matches;
    AWAIT(0, a, a.capturing >= muted_capture + 10);
    b = DO(1, STATUS, 0, 0);
    assert(a.tx_entries == at_entry && b.matches == peer_playing);
    DO(0, UNMUTE, 0, 0);
    AWAIT(0, a, a.tx_entries >= at_entry + 6U);
    AWAIT(1, b, b.matches >= peer_playing + 6U * FRAMES);
    assert(a.status.uplink_open && !a.status.mic_muted && a.cue_blocks == cue_writes && a.cue_callbacks == 1);
    printf("media_pipeline duplex pcm_a=%u pcm_b=%u muted_capture_blocks_min=22 muted_sends_after_in_flight=0\n",
           a.matches, b.matches);

    DO(0, CLOSE, CALL, 0);
    DO(1, CLOSE, CALL, 0);
    AWAIT(0, a, !a.status.generation && !a.speaker);
    AWAIT(1, b, !b.status.generation && !b.speaker);
    DO(0, OPEN_CALL, REPLACEMENT, 1);
    DO(1, OPEN_CALL, REPLACEMENT, 1);
    a = DO(0, CLOSE, CALL, 1);
    assert(a.status.generation == REPLACEMENT && !a.status.active && a.stats.stale_calls > 0);
    DO(1, SEND_STALE, CALL, 0);
    AWAIT(0, a, a.stats.rx_stale > 0);
    assert(!a.matches && !a.speaker && !a.stats.rx_received);
    const unsigned rejected = a.stats.rx_stale;
    DO(0, ACTIVATE, REPLACEMENT, CONVERSATION);
    DO(1, ACTIVATE, REPLACEMENT, CONVERSATION);
    AWAIT(0, a, a.started == REPLACEMENT && a.matches >= 6 * FRAMES);
    AWAIT(1, b, b.started == REPLACEMENT && b.matches >= 6 * FRAMES);
    assert(a.last_gen == REPLACEMENT && b.last_gen == REPLACEMENT && a.stats.rx_dropped == rejected);
    assert(!b.stats.rx_invalid && !a.stats.rx_flushed && !a.stats.rx_decode_failed && a.cue_blocks == cue_writes);
    printf("media_pipeline replacement stale_rx=%u media_a=%u media_b=%u cue_callbacks=%u\n",
           a.stats.rx_stale, a.matches, b.matches, a.cue_callbacks);
    DO(0, CLOSE, REPLACEMENT, 0);
    DO(1, CLOSE, REPLACEMENT, 0);
    AWAIT(0, a, !a.status.generation && !a.speaker);
    AWAIT(1, b, !b.status.generation && !b.speaker);
}

static void verify_cue_admission(void)
{
    for (unsigned held = 0; held <= 1; ++held) {
        const uint32_t generation = 60U + held;
        const reply_t before = DO(0, STATUS, 0, 0);
        assert(!before.status.generation && !before.status.playing);
        if (held) DO(0, OPEN_AI, generation, 0);
        const uint32_t cue = DO(0, CUE, 0, 0).cue_id;
        assert(cue);
        reply_t current;
        AWAIT(0, current, current.cue_blocks >= before.cue_blocks + 2U);
        assert(!current.status.active && current.status.playing == cue && current.cue_callbacks == before.cue_callbacks);
        printf("media_pipeline mixer=0 cue_pcm_blocks held=%u writes=%u\n", held, current.cue_blocks - before.cue_blocks);
        if (!held) DO(0, OPEN_AI, generation, 0);
        DO(0, ACTIVATE, generation, IGNORE);
        AWAIT(0, current, current.cue_callbacks == before.cue_callbacks + 1U);
        assert(current.cue_done_id == cue && current.cue_result == XIAOTAI_ERR_INVALID_STATE && !current.status.playing);
        command(0, CUE, 0, 0, XIAOTAI_ERR_BUSY);
        DO(0, CLOSE, generation, 0);
        AWAIT(0, current, !current.status.generation && !current.speaker);
    }
}

int main(void)
{
    int audio[2], ctrl[2][2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, audio) == 0);
    for (unsigned i = 0; i < 2; ++i) assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctrl[i]) == 0);
    pid_t child[2];
    for (unsigned i = 0; i < 2; ++i) {
        child[i] = fork();
        assert(child[i] >= 0);
        if (child[i] == 0) {
            close(audio[1U - i]);
            close(ctrl[1U - i][1]);
            close(ctrl[0][0]);
            close(ctrl[1][0]);
            return endpoint(i, ctrl[i][1], audio[i]);
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        close(audio[i]);
        close(ctrl[i][1]);
        controls[i] = ctrl[i][0];
    }
    for (unsigned i = 0; i < 2; ++i) assert(DO(i, STATUS, 0, 0).status.ready);
    verify_burst(0, AI20);
    verify_burst(1, AI40);
    verify_call();
    verify_cue_admission();
    for (unsigned i = 0; i < 2; ++i) {
        DO(i, SHUTDOWN, 0, 0);
        close(controls[i]);
        int code = 0;
        assert(waitpid(child[i], &code, 0) == child[i] && WIFEXITED(code) && WEXITSTATUS(code) == 0);
    }
    return 0;
}
