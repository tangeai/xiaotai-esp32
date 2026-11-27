// SPDX-License-Identifier: MIT
// Replaces legacy: tests/native/cores3/test_cores3_afe_continuity.py
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_agc.h"
#include "g711.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/port/os.h"

enum { RATE = 16000, DELAY = 160, PACKET = 160, WAIT = 2000, FIRST = 1100, FAILED = 2300, NEXT = 3700 };
static unsigned block, input_frames, agc_failures, tap_count, gate_count;
static bool gates[8];
static int16_t input_pcm[512];
static int64_t captured, last_tap_us;
static xiaotai_queue_t capture, packets;
static xiaotai_signal_t read_entry, started;
static uintptr_t vendor;

extern const xiaotai_audio_effect_t media_agc;

void *esp_agc_open(agc_mode_t mode, int rate)
{
    assert(mode == AGC_MODE_2 && rate == RATE);
    return &vendor;
}

void set_agc_config(void *handle, int gain_db, int limiter, int target)
{
    assert(handle == &vendor && gain_db == 15 && limiter == 1 && target == 6);
}

int esp_agc_process(void *handle, short *in, short *out, int samples, int rate)
{
    assert(handle == &vendor && in != out && samples == DELAY && rate == RATE);
    for (int i = 0; i < samples; ++i)
        if (in[i] == FAILED) { ++agc_failures; return -1; }
    memcpy(out, in, (size_t)samples * sizeof(*out));
    return ESP_AGC_SUCCESS;
}

void esp_agc_close(void *handle) { assert(handle == &vendor); }

static xiaotai_err_t input_side(void *self) { (void)self; return XIAOTAI_OK; }

static xiaotai_err_t input_read(void *self, void *buffer, size_t capacity, size_t *written)
{
    (void)self;
    assert(capacity == block * sizeof(int16_t));
    xiaotai_signal_give(&read_entry);
    int16_t marker;
    if (!xiaotai_queue_receive(&capture, &marker, 1000) || marker == -1) return XIAOTAI_ERR_TIMEOUT;
    int16_t *pcm = buffer;
    for (size_t i = 0; i < block; ++i) pcm[i] = marker;
    *written = capacity;
    return XIAOTAI_OK;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    static const xiaotai_audio_input_t input = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = RATE, .channels = 1, .roles = {XIAOTAI_CHANNEL_MIC}},
        .info = {.read_timeout_ms = 1000}, .read = input_read, .open = input_side, .close = input_side,
    };
    return index ? NULL : &input;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index) { (void)index; return NULL; }

static int processor_match(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{ return in && in->rate_hz == RATE && in->channels == 1 && !out ? 100 : 0; }

static xiaotai_err_t processor_open(const xiaotai_audio_input_t *in, const xiaotai_audio_output_t *out,
                                    xiaotai_processor_info_t *info, void **state)
{
    assert(in && !out);
    *info = (xiaotai_processor_info_t){.rate_hz = RATE, .feed_frames = block, .fetch_max = block};
    *state = input_pcm;
    return XIAOTAI_OK;
}

static xiaotai_err_t feed(void *state, const void *pcm, size_t bytes, int64_t end, uint32_t lost)
{
    (void)end;
    assert(state == input_pcm && bytes == block * sizeof(*input_pcm) && !lost);
    memcpy(input_pcm, pcm, bytes);
    captured = INT64_C(1000000) + (int64_t)++input_frames * block * 1000000 / RATE;
    return XIAOTAI_OK;
}

static xiaotai_err_t fetch(void *state, xiaotai_processor_frame_t *frame, uint32_t timeout)
{
    assert(state == input_pcm && !timeout);
    *frame = (xiaotai_processor_frame_t){.encoding = XIAOTAI_CODEC_PCM16, .data = input_pcm,
        .samples = block, .size = block * sizeof(*input_pcm), .captured_us = captured};
    return XIAOTAI_OK;
}

static void processor_close(void *state) { assert(state == input_pcm); }

static void tap_gate(void *ctx, bool open)
{
    (void)ctx;
    assert(gate_count < sizeof(gates) / sizeof(*gates));
    gates[gate_count++] = open;
}

static void tap_pcm(void *ctx, const int16_t *pcm, size_t samples, uint32_t rate, int64_t at)
{
    (void)ctx;
    assert(samples == block && rate == RATE && at > last_tap_us);
    assert(input_frames == (tap_count == 0 ? 1U : tap_count == 1 ? 3U : 4U));
    const int16_t marker = tap_count ? NEXT : FIRST;
    const bool first_after_reset = tap_count++ < 2U;
    for (size_t i = 0; i < samples; ++i)
        assert(pcm[i] == (first_after_reset && i < DELAY ? 0 : marker));
    last_tap_us = at;
}

typedef struct { uint32_t stamp; unsigned old; } packet_t;

static xiaotai_err_t send(void *ctx, uint32_t gen, uint32_t stamp, const uint8_t *data, size_t bytes)
{
    (void)ctx;
    assert(gen == 1U && bytes == PACKET);
    packet_t observation = {.stamp = stamp};
    for (size_t i = 0; i < bytes; ++i)
        observation.old += data[i] == xiaotai_media_alaw_encode_sample(FIRST);
    assert(xiaotai_queue_send(&packets, &observation, 0));
    return XIAOTAI_OK;
}

static void event(uint32_t gen, xiaotai_media_event_t event, xiaotai_err_t error, void *ctx)
{
    (void)ctx;
    assert(gen == 1U && event == XIAOTAI_MEDIA_EVENT_STARTED && error == XIAOTAI_OK);
    xiaotai_signal_give(&started);
}

static void open_session(void *ctx)
{
    (void)ctx;
    static const xiaotai_media_sink_t sink = {.send = send};
    const xiaotai_media_session_t session = {.generation = 1, .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
        .duplex = true, .sink = &sink, .event = event};
    assert(xiaotai_media_open(&session) == XIAOTAI_OK);
}

static xiaotai_err_t register_fixture(void)
{
    static const xiaotai_audio_processor_t processor = {.name = "host.fake_sr", .match = processor_match,
        .open = processor_open, .feed = feed, .fetch = fetch, .close = processor_close};
    static const xiaotai_media_tap_t tap = {.name = "host.agc_gap", .flags = XIAOTAI_TAP_IN_SESSION,
        .gate = tap_gate, .pcm = tap_pcm};
    static const xiaotai_media_param_t params[] = {{"capture.agc_db", 15}, {"capture.agc_stack_bytes", 6144},
        {"capture.account_effect_gaps", 1}};
    static const xiaotai_media_requirement_t need = {.uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K};
    assert(xiaotai_media_tune(params, sizeof(params) / sizeof(*params)) == XIAOTAI_OK);
    assert(xiaotai_media_processor_register(&processor) == XIAOTAI_OK);
    assert(xiaotai_media_effect_register(&media_agc) == XIAOTAI_OK);
    assert(xiaotai_media_tap_register(&tap) == XIAOTAI_OK);
    assert(xiaotai_media_tap_enable(&tap, true) == XIAOTAI_OK);
    return xiaotai_media_require(&need);
}

static void submit(int16_t marker)
{
    assert(xiaotai_queue_send(&capture, &marker, 0));
    assert(xiaotai_signal_take(&read_entry, WAIT));
}

int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "256")));
    block = argc == 1 ? 512U : 256U;
    assert(xiaotai_queue_init(&capture, 1, sizeof(int16_t), XIAOTAI_MEM_INTERNAL) == XIAOTAI_OK);
    assert(xiaotai_queue_init(&packets, 6, sizeof(packet_t), XIAOTAI_MEM_INTERNAL) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&read_entry) == XIAOTAI_OK && xiaotai_signal_init(&started) == XIAOTAI_OK);
    static const xiaotai_module_t engine = {.name = "media", .init = xiaotai_engine_media_init,
        .start = xiaotai_engine_media_start};
    static const char *const deps[] = {"media", NULL};
    static const xiaotai_module_t fixture = {.name = "fixture", .depends = deps, .init = register_fixture};
    const xiaotai_module_t *const modules[] = {&engine, &fixture};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 2,
        .queue_depth = 8}) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&read_entry, WAIT)); /* Capture worker ready. */
    assert(xiaotai_run(open_session, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&started, WAIT));
    submit(FIRST);
    packet_t packet;
    if (block == 512) assert(xiaotai_queue_receive(&packets, &packet, WAIT) && packet.stamp == 990U);
    submit(FAILED);
    assert(input_frames == 2 && agc_failures == 1 && tap_count == 1 && gate_count == 2 && gates[0] && !gates[1]);
    assert(!xiaotai_queue_count(&packets));
    submit(NEXT);
    assert(input_frames == 3 && tap_count == 2 && gate_count == 3 && gates[2]);
    assert(last_tap_us == INT64_C(990000) + (int64_t)3 * block * 1000000 / RATE);
    const uint32_t first_resumed_pts = 990U + 2U * block * 1000U / RATE;
    unsigned old_samples = 0, resumed_packets = 0;
    while (xiaotai_queue_receive(&packets, &packet, 40)) {
        assert(packet.stamp == first_resumed_pts + 20U * resumed_packets);
        ++resumed_packets;
        old_samples += packet.old;
        printf("resume packet: pts=%u old_pcm_bytes=%u\n", packet.stamp, packet.old);
    }
    xiaotai_media_stats_t stats;
    xiaotai_media_stats(&stats);
    assert(stats.capture_errors == 1U);
    printf("block=%u agc_failures=%u resumed_clean_tap=%u gap_samples=%u expected=%u resumed_packets=%u old_pcm_bytes=%u\n",
           block, agc_failures, tap_count - 1U, stats.capture_lost_samples, block, resumed_packets, old_samples);
    fflush(stdout);
    assert(stats.capture_lost_samples == block && old_samples == 0);
    assert(resumed_packets == block / (2 * PACKET));
    if (block == 256U) {
        /* With no stale prefix, one short fetch cannot emit a complete packet. */
        submit(NEXT);
        assert(xiaotai_queue_receive(&packets, &packet, WAIT));
        assert(packet.stamp == first_resumed_pts && packet.old == 0);
        assert(!xiaotai_queue_count(&packets));
    }

    const int16_t stop = -1;
    assert(xiaotai_queue_send(&capture, &stop, 0));
    xiaotai_media_reset();
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&started);
    xiaotai_signal_deinit(&read_entry);
    xiaotai_queue_deinit(&capture);
    xiaotai_queue_deinit(&packets);
    return EXIT_SUCCESS;
}
