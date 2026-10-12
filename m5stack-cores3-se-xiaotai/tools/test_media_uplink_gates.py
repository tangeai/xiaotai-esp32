#!/usr/bin/env python3
"""Room PTT capture-time drain, uplink gap timeline and the prompt upsampler.

Real starter_media.c functions with atomics, clock and codec boundaries faked.
This checks gating/timeline logic and interpolation order, not device timing.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_media.c").read_text(encoding="utf-8")


def function(name):
    match = re.search(r"^(?:static )?[A-Za-z_][^;{}\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    assert match, f"production function missing: {name}"
    depth, cursor = 1, match.end()
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[match.start():cursor]


def define(name):
    return re.search(r"^#define " + name + r" [^\n]*\n", source, re.M)[0]


assert define("ROOM_PTT_DRAIN_MS") == "#define ROOM_PTT_DRAIN_MS 600U\n"
diag = re.search(r"EXT_RAM_BSS_ATTR static struct \{[^}]+\} s_room_audio_diag;",
                 source, re.S)[0].replace("EXT_RAM_BSS_ATTR", "")
gates = "".join(function(name) for name in (
    "same_session", "uplink_session_current", "room_ptt_drain_open",
    "uplink_frame_admitted", "starter_media_room_ptt", "uplink_timeline_skip"))
prompt = source[source.index("esp_err_t starter_media_play_pcm8k_at_epoch("):
                source.index("esp_err_t starter_media_start(")]
# Every uplink admission point uses capture-time gating, never processing time.
capture = function("audio_capture_task")
uplink = function("audio_uplink_task")
assert capture.count("uplink_frame_admitted(mode, generation, (uint32_t)captured_ms)") == 2
assert uplink.count("uplink_frame_admitted(item->mode, item->generation, item->captured_ms)") == 2
assert "uplink_session_current(item->mode" not in uplink
assert "uplink_timeline_skip(&packet_samples, &next_timestamp_ms, uplink_gap_samples);" in capture
assert capture.index("clean.lost_samples != 0") < capture.index("atomic_exchange(&s_preroll_gap, 0)")
assert "PREROLL_GAP_CAPTURE" not in function("audio_input_task")
assert "starter_aec_report_capture_loss();" in function("audio_input_task")
assert "atomic_store(&s_room_ptt_drain_generation, 0);" in function("stop_media")

code = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "starter_tirtc.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_TIMEOUT 0x107
#define ESP_LOGI(...) ((void)0)
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define AUDIO_TRANSPORT_SAMPLE_RATE_HZ STARTER_TIRTC_AUDIO_SAMPLE_RATE_HZ
static atomic_bool s_ready, s_active, s_speaker_muted, s_microphone_muted;
static atomic_int s_mode;
static atomic_uint s_generation, s_drain_generation, s_room_ptt_generation, s_mute_epoch;
static atomic_uint s_room_ptt_drain_generation, s_room_ptt_release_ms;
static int64_t clock_us;
static int64_t esp_timer_get_time(void) { return clock_us; }
static void at_ms(int64_t ms) { clock_us = ms * 1000; }
''' + define("ROOM_PTT_DRAIN_MS") + diag + gates + r'''
/* Prompt writer boundaries: capture every stereo frame written to the codec. */
''' + define("AUDIO_ENCODED_MAX_BYTES") + define("AUDIO_RX_BYTES") + \
    define("AUDIO_PCM8K_I2S_VALUES_PER_INPUT") + r'''
#define ESP_CODEC_DEV_OK 0
#define OUTPUT_OWNER_PROMPT 5
static atomic_uint s_pcm8k_cancel_sequence;
static void *s_speaker_dev = (void *)1, *s_audio_output_mutex = (void *)2;
static int16_t s_play_stereo[AUDIO_RX_BYTES * AUDIO_PCM8K_I2S_VALUES_PER_INPUT];
static int16_t written[40000 * 4];
static size_t written_values;
static bool take_audio_output_lock(int owner, unsigned ticks) { return owner == OUTPUT_OWNER_PROMPT && ticks == 500; }
static void give_audio_output_lock(void) {}
static esp_err_t audio_output_set_locked(bool enabled) { (void)enabled; return ESP_OK; }
static void vTaskDelay(unsigned ticks) { (void)ticks; }
static int esp_codec_dev_write(void *dev, void *data, size_t bytes)
{
    assert(dev == s_speaker_dev && data == s_play_stereo && bytes <= sizeof(s_play_stereo));
    assert(written_values + bytes / sizeof(int16_t) <= sizeof(written) / sizeof(written[0]));
    memcpy(written + written_values, data, bytes);
    written_values += bytes / sizeof(int16_t);
    return ESP_CODEC_DEV_OK;
}
''' + prompt + r'''
static void start(starter_tirtc_mode_t mode, uint32_t generation)
{
    s_active = true; s_mode = mode; s_generation = generation; s_drain_generation = 0;
}

static void test_ptt_drain(void)
{
    const starter_tirtc_mode_t room = STARTER_TIRTC_ROOM;
    at_ms(100000);
    start(room, 5);
    assert(!uplink_frame_admitted(room, 5, 99990)); /* listening */
    assert(starter_media_room_ptt(5, true));
    unsigned pressed_epoch = s_mute_epoch;
    assert(uplink_frame_admitted(room, 5, 100010));
    at_ms(101000);
    assert(starter_media_room_ptt(5, false));
    /* Release keeps the epoch: speech captured while held keeps flowing. */
    assert(s_mute_epoch == pressed_epoch && s_room_ptt_generation == 0);
    assert(s_room_ptt_drain_generation == 5 && s_room_ptt_release_ms == 101000);
    assert(!uplink_session_current(room, 5) && room_ptt_drain_open(room, 5, NULL));
    assert(uplink_frame_admitted(room, 5, 100700) && uplink_frame_admitted(room, 5, 101000));
    assert(!uplink_frame_admitted(room, 5, 101001) && !uplink_frame_admitted(room, 5, 101400));
    assert(!uplink_frame_admitted(room, 4, 100700));
    assert(!uplink_frame_admitted(STARTER_TIRTC_H5, 5, 100700));
    /* A repeated release (runtime tick) neither restarts nor extends it. */
    at_ms(101300);
    assert(starter_media_room_ptt(5, false));
    assert(s_room_ptt_release_ms == 101000 && s_room_audio_diag.ptt_up == 1);
    at_ms(101000 + ROOM_PTT_DRAIN_MS);
    assert(uplink_frame_admitted(room, 5, 100990));
    at_ms(101000 + ROOM_PTT_DRAIN_MS + 1);
    assert(!uplink_frame_admitted(room, 5, 100990) && !room_ptt_drain_open(room, 5, NULL));

    /* Re-press during a drain: new spurt, drain ended, old frames stale. */
    at_ms(200000);
    assert(starter_media_room_ptt(5, true));
    at_ms(200500);
    assert(starter_media_room_ptt(5, false) && room_ptt_drain_open(room, 5, NULL));
    unsigned epoch = s_mute_epoch;
    at_ms(200550);
    assert(starter_media_room_ptt(5, true));
    assert(s_mute_epoch != epoch && s_room_ptt_drain_generation == 0);
    assert(!room_ptt_drain_open(room, 5, NULL) && uplink_session_current(room, 5));

    /* Stop, generation change and an EOS drain flag all end admission. */
    at_ms(300000);
    assert(starter_media_room_ptt(5, false) && room_ptt_drain_open(room, 5, NULL));
    s_active = false;
    assert(!uplink_frame_admitted(room, 5, 299990));
    s_active = true;
    s_generation = 6;
    assert(!uplink_frame_admitted(room, 5, 299990) && !uplink_frame_admitted(room, 6, 299990));
    s_generation = 5;
    s_drain_generation = 5;
    assert(!uplink_frame_admitted(room, 5, 299990));
    s_drain_generation = 0;
    assert(uplink_frame_admitted(room, 5, 299990));
    /* Room close/teardown (generation 0): no drain, everything invalidated. */
    epoch = s_mute_epoch;
    assert(starter_media_room_ptt(0, false));
    assert(s_mute_epoch == epoch + 1 && s_room_ptt_drain_generation == 0);
    assert(!uplink_frame_admitted(room, 5, 299990));
    /* Mute refuses a press; a released generation never re-arms by itself. */
    s_microphone_muted = true;
    assert(!starter_media_room_ptt(5, true) && s_room_ptt_generation == 0);
    s_microphone_muted = false;
    assert(!starter_media_room_ptt(0, true) && !starter_media_room_ptt(7, true));

    /* esp_timer milliseconds past the 32-bit wrap keep ordering. */
    at_ms(INT64_C(0x100000000) - 100);
    assert(starter_media_room_ptt(5, true));
    assert(starter_media_room_ptt(5, false) && s_room_ptt_release_ms == 0xffffff9cU);
    at_ms(INT64_C(0x100000000) + 200);
    assert(uplink_frame_admitted(room, 5, 0xffffff00U));
    assert(!uplink_frame_admitted(room, 5, 50U));
    at_ms(INT64_C(0x100000000) + 501);
    assert(!uplink_frame_admitted(room, 5, 0xffffff00U));
}

static void test_timeline_skip(void)
{
    size_t samples = 192;
    uint32_t timestamp = 0;
    uplink_timeline_skip(&samples, &timestamp, 0);
    assert(samples == 192 && timestamp == 0);
    /* No timeline yet: the partial pre-gap packet is dropped, nothing moves. */
    uplink_timeline_skip(&samples, &timestamp, 512);
    assert(samples == 0 && timestamp == 0);
    /* Partial packet (12 ms) + 32 ms lost: next packet starts 44 ms later. */
    samples = 192; timestamp = 5000;
    uplink_timeline_skip(&samples, &timestamp, 512);
    assert(samples == 0 && timestamp == 5044);
    uplink_timeline_skip(&samples, &timestamp, 16000);
    assert(samples == 0 && timestamp == 6044);
    samples = 319; timestamp = UINT32_MAX - 10;
    uplink_timeline_skip(&samples, &timestamp, 4096U * 1000U);
    assert(samples == 0 && timestamp == (uint32_t)(UINT32_MAX - 10 + (319U + 4096000U) / 16U));
}

static int16_t pcm[2 * AUDIO_RX_BYTES + 7];
static int16_t tone[8000];

static double bin(const int16_t *stereo, size_t frames, double hz)
{
    double re = 0, im = 0;
    for (size_t m = 0; m < frames; ++m) {
        double phase = 2.0 * M_PI * hz * (double)m / 16000.0;
        re += stereo[m * 2U] * cos(phase);
        im -= stereo[m * 2U] * sin(phase);
    }
    return sqrt(re * re + im * im);
}

static void test_prompt_upsampler(void)
{
    s_ready = true;
    s_active = false; /* prompts play only without an RTC session */
    size_t count = sizeof(pcm) / sizeof(pcm[0]);
    for (size_t n = 0; n < count; ++n) pcm[n] = (int16_t)((int)(n * 37U % 20001U) - 10000);
    pcm[0] = 1234;
    written_values = 0;
    assert(starter_media_play_pcm8k_at_epoch(pcm, count, 0) == ESP_OK);
    assert(written_values == count * AUDIO_PCM8K_I2S_VALUES_PER_INPUT);
    for (size_t n = 0; n < count; ++n) {
        int16_t previous = n ? pcm[n - 1U] : pcm[0];
        int16_t midpoint = (int16_t)(((int32_t)previous + pcm[n]) / 2);
        const int16_t *out = written + n * AUDIO_PCM8K_I2S_VALUES_PER_INPUT;
        /* Midpoint first (between previous and current), then current;
         * both stereo channels equal; continuity across 1500-sample chunks. */
        assert(out[0] == midpoint && out[1] == midpoint);
        assert(out[2] == pcm[n] && out[3] == pcm[n]);
    }
    /* A ramp stays monotonic: real interpolation, not a zig-zag. */
    for (size_t n = 0; n < count; ++n) pcm[n] = (int16_t)(n * 10U);
    written_values = 0;
    assert(starter_media_play_pcm8k_at_epoch(pcm, count, 0) == ESP_OK);
    for (size_t m = 1; m < written_values / 2U; ++m) assert(written[m * 2U] >= written[(m - 1U) * 2U]);
    /* 1 kHz tone: the 7 kHz image is about -28 dB for linear interpolation
     * (the swapped order left it at about -7.6 dB). */
    for (size_t n = 0; n < 8000; ++n) tone[n] = (int16_t)lrint(10000.0 * sin(2.0 * M_PI * 1000.0 * n / 8000.0));
    written_values = 0;
    assert(starter_media_play_pcm8k_at_epoch(tone, 8000, 0) == ESP_OK);
    double signal = bin(written, 16000, 1000.0), image = bin(written, 16000, 7000.0);
    assert(image < signal * 0.06 && signal > 1e7);
    /* Gain and stereo duplication unchanged: the input samples pass through. */
    for (size_t n = 0; n < 8000; ++n) assert(written[n * 4U + 2U] == tone[n]);
}

int main(void)
{
    test_ptt_drain();
    test_timeline_skip();
    test_prompt_upsampler();
    puts("PASS: Room PTT capture-time drain (bound, re-press, stop, generation, close, mute, wrap), "
         "uplink gap timeline, linear 8k->16k prompt interpolation");
}
'''

with tempfile.TemporaryDirectory(prefix="media-uplink-gates-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(code, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined",
                    "-I", str(root / "components/starter_tirtc/include"),
                    str(path / "test.c"), "-o", str(path / "test"), "-lm"], check=True)
    subprocess.run([str(path / "test")], check=True)
