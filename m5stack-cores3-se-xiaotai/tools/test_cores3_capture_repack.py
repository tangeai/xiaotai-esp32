"""Compare the real MR capture producer with a two-pass PCM/statistics reference.

MIC1 + reference feed the AFE; MIC2 is still measured. Loss before an accepted
block (full pool, failed read) is carried in-band on that block's first stamp.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
media = root / "components/starter_media"
source = (media / "src/starter_aec.c").read_text(encoding="utf-8")
header = (media / "src/starter_aec.h").read_text(encoding="utf-8")
constants = re.search(r"enum \{.*?\n\};", header, re.S)[0]
types = source[source.index("typedef struct {"):source.index("typedef struct {\n    const esp_afe")]
producer = source[source.index("esp_err_t starter_aec_submit_capture("):
                  source.index("esp_err_t starter_aec_fetch(")]
lost_add = source[source.index("static uint32_t lost_add("):source.index("static int16_t *allocate_samples(")]
assert "void starter_aec_report_capture_loss(void)" in producer
defines = "".join(re.search(r"^#define " + name + r" [^\n]*\n", source, re.M)[0]
                  for name in ("AFE_INPUT_SLOTS", "AFE_FEED_CHANNELS", "AFE_LOST_SAMPLES_MAX"))
assert "#define AFE_FEED_CHANNELS 2U" in defines
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "starter_signal_level.h"
#include "starter_log.h"
void starter_log_record(starter_log_topic_t topic, const uint32_t values[STARTER_LOG_VALUES]) {
    assert(topic == STARTER_LOG_AFE_FULL && values[0] != 0);
}
#define AFE_MAX_SAMPLES 1024U
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_FAIL 4
#define ESP_LOGE(...) ((void)0)
#define pdTRUE 1
typedef int esp_err_t;
'''
stubs = r'''
_Static_assert(AFE_MAX_SAMPLES <= INT32_MAX / 32768U, "PCM sum range");
static struct {
    void *handle;
    size_t feed_samples, fetch_samples, chunks_per_feed;
    int16_t *tdm;
    afe_input_slot_t input[AFE_INPUT_SLOTS];
    void *free_slots, *pending_slots;
    uint32_t capture_lost;
    atomic_bool failed;
    atomic_uint input_full;
} s_aec;
static unsigned receives, sends, returns, failures;
static bool free_full, pending_full;
static uint8_t selected;
static size_t starter_aec_capture_bytes(void) {
    return s_aec.feed_samples * STARTER_AEC_CAPTURE_DMA_CHANNELS * sizeof(int16_t);
}
static int xQueueReceive(void *q, uint8_t *slot, unsigned wait) {
    assert(q == s_aec.free_slots && wait == 0);
    ++receives;
    if (free_full) return 0;
    *slot = selected;
    return pdTRUE;
}
static int xQueueSend(void *q, const uint8_t *slot, unsigned wait) {
    assert(*slot == selected && wait == 0);
    if (q == s_aec.free_slots) { ++returns; return pdTRUE; }
    assert(q == s_aec.pending_slots);
    ++sends;
    return pending_full ? 0 : pdTRUE;
}
static void fail_pipeline(const char *stage, int result) {
    assert(strcmp(stage, "input queue") == 0 && result == -1);
    ++failures;
    atomic_store(&s_aec.failed, true);
}
'''
tests = r'''
static int16_t raw[AFE_MAX_SAMPLES * 4U], original[AFE_MAX_SAMPLES * 4U];
static int16_t output[AFE_MAX_SAMPLES * 3U + 32U];
static int16_t expected[AFE_MAX_SAMPLES * AFE_FEED_CHANNELS];
static int16_t measured[AFE_MAX_SAMPLES * 3U];
static afe_frame_stamp_t expected_stamp[2];
static uint32_t random_state = 0x6841d2f3;
static uint32_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
static void reference(int64_t time_ms, uint32_t epoch, uint32_t mute, uint32_t lost) {
    for (size_t i = 0; i < s_aec.feed_samples; ++i) {
        /* Wire order M1/R/M2/N: the AFE gets M1/R, levels cover M1/M2/R. */
        expected[i * 2U] = raw[i * 4U];
        expected[i * 2U + 1U] = raw[i * 4U + 1U];
        measured[i * 3U] = raw[i * 4U];
        measured[i * 3U + 1U] = raw[i * 4U + 2U];
        measured[i * 3U + 2U] = raw[i * 4U + 1U];
    }
    memset(expected_stamp, 0, sizeof(expected_stamp));
    for (size_t f = 0; f < s_aec.chunks_per_feed; ++f) {
        afe_frame_stamp_t *stamp = &expected_stamp[f];
        stamp->captured_ms = time_ms - (int64_t)((s_aec.chunks_per_feed - f - 1U) *
            s_aec.fetch_samples * 1000U / STARTER_AEC_SAMPLE_RATE_HZ);
        stamp->capture_epoch = epoch;
        stamp->mute_epoch = mute;
        stamp->lost_samples = f == 0 ? lost : 0;
        int64_t sum[3] = {0};
        uint64_t square[3] = {0};
        uint32_t peak[3] = {0};
        for (size_t i = 0; i < s_aec.fetch_samples; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                int32_t value = measured[(f * s_aec.fetch_samples + i) * 3U + c];
                uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
                sum[c] += value;
                square[c] += (int64_t)value * value;
                if (magnitude > peak[c]) peak[c] = magnitude;
                if (magnitude >= 32700U) {
                    if (c == 2) ++stamp->reference_clipped;
                    else ++stamp->mic_clipped;
                }
            }
        }
        for (size_t c = 0; c < 3; ++c) {
            starter_signal_level_t *level = c < 2 ? &stamp->mic[c] : &stamp->reference;
            level->peak = peak[c];
            level->dc = (int32_t)(sum[c] / (int64_t)s_aec.fetch_samples);
            level->rms = starter_signal_isqrt(square[c] / s_aec.fetch_samples);
        }
    }
}
static void reset(size_t feed, size_t fetch) {
    memset(&s_aec, 0, sizeof(s_aec));
    s_aec.handle = (void *)1;
    s_aec.free_slots = (void *)2;
    s_aec.pending_slots = (void *)3;
    s_aec.tdm = raw;
    s_aec.feed_samples = feed;
    s_aec.fetch_samples = fetch;
    s_aec.chunks_per_feed = feed / fetch;
    selected = (uint8_t)(next_random() % AFE_INPUT_SLOTS);
    s_aec.input[selected].pcm = output + 16;
    memset(output, 0x5a, sizeof(output));
    receives = sends = returns = failures = 0;
    free_full = pending_full = false;
}
int main(void) {
    const size_t sizes[][2] = {{1024,512}, {512,512}, {512,256}, {1024,1024}};
    unsigned comparisons = 0;
    for (unsigned size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size) {
        for (unsigned pattern = 0; pattern < 68; ++pattern) {
            reset(sizes[size][0], sizes[size][1]);
            for (size_t i = 0; i < s_aec.feed_samples * 4U; ++i) {
                switch (pattern) {
                    case 0: raw[i] = INT16_MIN; break;
                    case 1: raw[i] = INT16_MAX; break;
                    case 2: raw[i] = i & 1U ? INT16_MIN : INT16_MAX; break;
                    case 3: raw[i] = (int16_t)((int)(i % 7U) - 3); break;
                    default: raw[i] = (int16_t)next_random(); break;
                }
            }
            memcpy(original, raw, sizeof(raw));
            int64_t time_ms = pattern & 1U ? INT64_C(9123456789) : 3;
            uint32_t epoch = pattern & 1U ? UINT32_MAX : 0;
            uint32_t mute = next_random();
            uint32_t lost = pattern % 3U == 0 ? (uint32_t)s_aec.feed_samples * (pattern % 5U) : 0;
            s_aec.capture_lost = lost;
            reference(time_ms, epoch, mute, lost);
            assert(starter_aec_submit_capture(starter_aec_capture_bytes(), time_ms, epoch, mute) == ESP_OK);
            assert(receives == 1 && sends == 1 && returns == 0 && failures == 0);
            assert(s_aec.capture_lost == 0);
            assert(memcmp(original, raw, sizeof(raw)) == 0);
            assert(memcmp(output + 16, expected,
                          s_aec.feed_samples * AFE_FEED_CHANNELS * sizeof(int16_t)) == 0);
            assert(memcmp(s_aec.input[selected].stamp, expected_stamp,
                          s_aec.chunks_per_feed * sizeof(afe_frame_stamp_t)) == 0);
            for (size_t i = 0; i < 16; ++i) assert(output[i] == 0x5a5a);
            for (size_t i = 16 + s_aec.feed_samples * AFE_FEED_CHANNELS;
                 i < sizeof(output)/sizeof(output[0]); ++i)
                assert(output[i] == 0x5a5a);
            ++comparisons;
        }
    }
    reset(1024, 512);
    assert(starter_aec_submit_capture(0, 0, 0, 0) == ESP_ERR_INVALID_ARG);
    assert(receives == 0 && sends == 0);
    s_aec.handle = NULL;
    assert(starter_aec_submit_capture(starter_aec_capture_bytes(), 0, 0, 0) == ESP_ERR_INVALID_ARG);
    s_aec.handle = (void *)1;
    atomic_store(&s_aec.failed, true);
    assert(starter_aec_submit_capture(starter_aec_capture_bytes(), 0, 0, 0) == ESP_ERR_INVALID_STATE);
    assert(receives == 0 && sends == 0);
    atomic_store(&s_aec.failed, false);
    free_full = true;
    for (unsigned i = 0; i < 101; ++i)
        assert(starter_aec_submit_capture(starter_aec_capture_bytes(), 0, 0, 0) == ESP_ERR_NO_MEM);
    assert(atomic_load(&s_aec.input_full) == 101 && sends == 0 && returns == 0);
    for (size_t i = 0; i < sizeof(output)/sizeof(output[0]); ++i) assert(output[i] == 0x5a5a);
    assert(!atomic_load(&s_aec.failed));
    /* Rejected blocks and failed reads become one in-band gap on the next
     * accepted block's first stamp, then the carry restarts from zero. */
    assert(s_aec.capture_lost == 101U * 1024U);
    starter_aec_report_capture_loss();
    starter_aec_report_capture_loss();
    assert(s_aec.capture_lost == 103U * 1024U);
    free_full = false;
    assert(starter_aec_submit_capture(starter_aec_capture_bytes(), 77, 5, 6) == ESP_OK);
    assert(s_aec.input[selected].stamp[0].lost_samples == 103U * 1024U);
    assert(s_aec.input[selected].stamp[1].lost_samples == 0);
    assert(s_aec.input[selected].stamp[0].captured_ms == 45);
    assert(s_aec.input[selected].stamp[1].captured_ms == 77);
    assert(s_aec.capture_lost == 0);
    assert(starter_aec_submit_capture(starter_aec_capture_bytes(), 109, 5, 6) == ESP_OK);
    assert(s_aec.input[selected].stamp[0].lost_samples == 0);
    s_aec.handle = NULL;
    starter_aec_report_capture_loss();
    assert(s_aec.capture_lost == 0);
    reset(1024, 512);
    pending_full = true;
    starter_aec_report_capture_loss();
    assert(starter_aec_submit_capture(starter_aec_capture_bytes(), 0, 0, 0) == ESP_FAIL);
    assert(receives == 1 && sends == 1 && returns == 1 && failures == 1);
    assert(atomic_load(&s_aec.failed) && s_aec.capture_lost == 1024U);
    printf("PASS: %u bit-exact MR PCM/stamp comparisons; MIC2 levels, in-band loss carry, "
           "clipping, overflow, epochs, guards and queue failures\n", comparisons);
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-capture-repack-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + defines + constants + types + stubs + lost_add + producer + tests,
                                 encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(media / "include"),
                    "-I", str(root / "components/starter_log/include"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
