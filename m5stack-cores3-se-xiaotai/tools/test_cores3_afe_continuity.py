#!/usr/bin/env python3
"""Run the real AFE feed step and fetch pairing against a fake ring-buffer AFE.

Ring-full back-pressure (feed returns 0) must be counted and dropped without
failing the pipeline; every delivered frame keeps its own stamp, and samples
lost before it are reported in-band on that frame. No device or ESP-IDF.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
media = root / "components/starter_media"
source = (media / "src/starter_aec.c").read_text(encoding="utf-8")


def function(name):
    match = re.search(r"^(?:static )?[A-Za-z_][^;{}\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    assert match, f"production function missing: {name}"
    depth, cursor = 1, match.end()
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[match.start():cursor]


defines = "".join(re.search(r"^#define " + name + r" [^\n]*\n", source, re.M)[0]
                  for name in ("AFE_INPUT_SLOTS", "AFE_FEED_CHANNELS", "AFE_STAMP_WAIT_MS",
                               "AFE_LOST_SAMPLES_MAX"))
types = source[source.index("typedef struct {\n    int64_t captured_ms;"):
               source.index("typedef struct {\n    const esp_afe")]
context = source[source.index("typedef struct {\n    const esp_afe"):
                 source.index("} starter_aec_context_t;") + len("} starter_aec_context_t;")]
worker = function("afe_feed_worker")
assert "afe_feed_block(&s_aec.input[slot], &dropped_samples)" in worker
assert "fail_pipeline" not in worker
feed_block = function("afe_feed_block")
assert "if (ret < 0)" in feed_block and "fail_pipeline(\"feed\", ret)" in feed_block
code = (function("fail_pipeline") + function("lost_add") + feed_block + function("starter_aec_fetch") +
        function("starter_aec_get_stats"))

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "starter_aec.h"
static const char *last_failure;
static int last_rc;
#define ESP_LOGE(tag, format, stage, rc) ((void)(format), last_failure = (stage), last_rc = (rc))
#define pdTRUE 1
#define pdFALSE 0
#define CONFIG_XIAOTAI_CAPTURE_AGC 1
#define STARTER_AGC_DELAY_MS 10U
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct { int16_t *data; int data_size; int trigger_channel_id; int ret_value; } afe_fetch_result_t;
typedef struct esp_afe_sr_data_t esp_afe_sr_data_t;
typedef struct {
    int (*feed)(esp_afe_sr_data_t *afe, const int16_t *in);
    afe_fetch_result_t *(*fetch_with_delay)(esp_afe_sr_data_t *afe, TickType_t ticks);
} esp_afe_sr_iface_t;
'''

harness = r'''
static starter_aec_context_t s_aec;
static int64_t clock_us = 1000000;
static int64_t esp_timer_get_time(void) { return clock_us += 37; }

/* Stamp FIFO; a deferred publish models the feed owner being preempted
 * between writing PCM to the ring and pushing the stamp. */
#define STAMP_STORAGE 64U
static afe_frame_stamp_t fifo[STAMP_STORAGE], deferred[8];
static unsigned fifo_head, fifo_count, fifo_capacity, deferred_count;
static bool defer_stamps;
static BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    assert(queue == s_aec.stamps && wait == 0);
    if (defer_stamps) {
        assert(deferred_count < 8);
        deferred[deferred_count++] = *(const afe_frame_stamp_t *)item;
        return pdTRUE;
    }
    if (fifo_count == fifo_capacity) return pdFALSE;
    fifo[(fifo_head + fifo_count++) % STAMP_STORAGE] = *(const afe_frame_stamp_t *)item;
    return pdTRUE;
}
static BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    assert(queue == s_aec.stamps && wait == pdMS_TO_TICKS(AFE_STAMP_WAIT_MS));
    if (fifo_count == 0) return pdFALSE;
    *(afe_frame_stamp_t *)item = fifo[fifo_head];
    fifo_head = (fifo_head + 1U) % STAMP_STORAGE;
    --fifo_count;
    return pdTRUE;
}
static void publish_deferred(void)
{
    defer_stamps = false;
    for (unsigned n = 0; n < deferred_count; ++n) assert(xQueueSend(s_aec.stamps, &deferred[n], 0));
    deferred_count = 0;
}

/* Fake AFE: a ring of fetch frames. Ring full: AEC "ran", nothing written,
 * feed returns 0, as in esp_afe_sr_1mic/2mic afe_feed. */
static int16_t ring[16][1024], fetched[1024];
static unsigned ring_head, ring_count, ring_capacity, feed_calls;
static int forced_ret;
static bool force_ret;
static afe_fetch_result_t fetch_result;
static int fake_feed(esp_afe_sr_data_t *afe, const int16_t *in)
{
    assert(afe == (esp_afe_sr_data_t *)&s_aec);
    ++feed_calls;
    if (force_ret) return forced_ret;
    if (ring_count + s_aec.chunks_per_feed > ring_capacity) return 0;
    for (size_t c = 0; c < s_aec.chunks_per_feed; ++c) {
        int16_t *frame = ring[(ring_head + ring_count++) % 16U];
        for (size_t i = 0; i < s_aec.fetch_samples; ++i)
            frame[i] = in[(c * s_aec.fetch_samples + i) * AFE_FEED_CHANNELS];
    }
    return (int)(s_aec.feed_samples * AFE_FEED_CHANNELS * sizeof(int16_t));
}
static afe_fetch_result_t *fake_fetch(esp_afe_sr_data_t *afe, TickType_t ticks)
{
    assert(afe == (esp_afe_sr_data_t *)&s_aec && ticks == pdMS_TO_TICKS(200));
    if (ring_count == 0) {
        fetch_result = (afe_fetch_result_t){.ret_value = -1};
        return &fetch_result;
    }
    memcpy(fetched, ring[ring_head], s_aec.fetch_samples * sizeof(int16_t));
    ring_head = (ring_head + 1U) % 16U;
    --ring_count;
    fetch_result = (afe_fetch_result_t){.data = fetched,
        .data_size = (int)(s_aec.fetch_samples * sizeof(int16_t)), .ret_value = 0};
    return &fetch_result;
}
static const esp_afe_sr_iface_t iface = {.feed = fake_feed, .fetch_with_delay = fake_fetch};

static unsigned agc_discards;
static bool agc_fail;
static void starter_agc_discard_pending(void) { ++agc_discards; }
static esp_err_t starter_agc_process(const int16_t *clean, int16_t *uplink, size_t samples)
{
    if (agc_fail) return ESP_FAIL;
    memcpy(uplink, clean, samples * sizeof(int16_t));
    return ESP_OK;
}
'''

tests = r'''
static int16_t pcm[1024 * AFE_FEED_CHANNELS], clean[1024], uplink[1024];
static uint32_t dropped;

static void reset(size_t feed, size_t fetch)
{
    memset(&s_aec, 0, sizeof(s_aec));
    s_aec.iface = &iface;
    s_aec.handle = (esp_afe_sr_data_t *)&s_aec;
    s_aec.stamps = (QueueHandle_t)&fifo;
    s_aec.feed_samples = feed;
    s_aec.fetch_samples = fetch;
    s_aec.chunks_per_feed = feed / fetch;
    s_aec.clean = clean;
    s_aec.uplink = uplink;
    fifo_head = fifo_count = deferred_count = ring_head = ring_count = feed_calls = 0;
    fifo_capacity = 32;
    ring_capacity = 3U * (unsigned)s_aec.chunks_per_feed;
    defer_stamps = force_ret = agc_fail = false;
    agc_discards = dropped = 0;
}

/* Block k chunk c carries marker k*4+c in its PCM and its captured_ms. */
static void feed_block(unsigned k, uint32_t lost_in)
{
    afe_input_slot_t slot = {.pcm = pcm};
    for (size_t c = 0; c < s_aec.chunks_per_feed; ++c) {
        slot.stamp[c] = (afe_frame_stamp_t){.captured_ms = k * 4 + c, .mute_epoch = 7,
                                            .lost_samples = c == 0 ? lost_in : 0};
        for (size_t i = 0; i < s_aec.fetch_samples; ++i) {
            pcm[(c * s_aec.fetch_samples + i) * AFE_FEED_CHANNELS] = (int16_t)(k * 4 + c);
            pcm[(c * s_aec.fetch_samples + i) * AFE_FEED_CHANNELS + 1U] = (int16_t)-1;
        }
    }
    afe_feed_block(&slot, &dropped);
}

static void expect_frame(unsigned k, unsigned c, uint32_t lost)
{
    starter_aec_output_t out;
    unsigned discards = agc_discards;
    assert(starter_aec_fetch(&out) == ESP_OK);
    assert(out.captured_ms == (int64_t)(k * 4 + c) && out.mute_epoch == 7);
    assert(out.pcm_16k[0] == (int16_t)out.captured_ms && out.transport_pcm[0] == out.pcm_16k[0]);
    assert(out.pcm_16k[s_aec.fetch_samples - 1U] == (int16_t)out.captured_ms);
    assert(out.lost_samples == lost && out.samples_16k == s_aec.fetch_samples);
    assert(agc_discards == discards + (lost != 0));
}

static void expect_error(esp_err_t err)
{
    starter_aec_output_t out;
    assert(starter_aec_fetch(&out) == err);
}

static void run(size_t feed, size_t fetch)
{
    reset(feed, fetch);
    const uint32_t f = (uint32_t)fetch, b = (uint32_t)feed, chunks = b / f;
    /* Continuous: one stamp per frame, no gap. */
    for (unsigned k = 1; k <= 3; ++k) feed_block(k, 0);
    for (unsigned k = 1; k <= 3; ++k)
        for (unsigned c = 0; c < chunks; ++c) expect_frame(k, c, 0);
    expect_error(ESP_ERR_TIMEOUT); /* empty ring is a fetch timeout, not a gap */
    starter_aec_stats_t stats;
    starter_aec_get_stats(&stats);
    assert(stats.feed_frames == 3 && stats.fetch_frames == 3 * chunks && stats.fetch_timeouts == 1);
    assert(stats.feed_ring_full == 0 && stats.stamp_timeouts == 0 && !stats.failed);

    /* Ring full twice: dropped, counted, never fatal; input-side loss of a
     * dropped block is carried too. The next fed block reports everything. */
    for (unsigned k = 4; k <= 6; ++k) feed_block(k, 0);
    feed_block(7, 100);
    feed_block(8, 0);
    starter_aec_get_stats(&stats);
    assert(stats.feed_ring_full == 2 && stats.feed_frames == 6 && !stats.failed);
    assert(dropped == 100 + 2 * b && fifo_count == 3 * chunks);
    for (unsigned k = 4; k <= 6; ++k)
        for (unsigned c = 0; c < chunks; ++c) expect_frame(k, c, 0);
    feed_block(9, 7);
    assert(dropped == 0);
    expect_frame(9, 0, 7 + 100 + 2 * b);
    for (unsigned c = 1; c < chunks; ++c) expect_frame(9, c, 0);

    /* Stamp published after its PCM became fetchable: the frame is dropped as
     * a gap and its late stamp skipped, so pairing never shifts. */
    defer_stamps = true;
    feed_block(10, 3);
    for (unsigned c = 0; c < chunks; ++c) expect_error(ESP_ERR_TIMEOUT);
    publish_deferred();
    feed_block(11, 0);
    expect_frame(11, 0, chunks * f + 3);
    for (unsigned c = 1; c < chunks; ++c) expect_frame(11, c, 0);
    starter_aec_get_stats(&stats);
    assert(stats.stamp_timeouts == chunks && s_aec.stamps_owed == 0 && !stats.failed);
    assert(last_failure == NULL);

    /* AGC failure drops one delivered frame; the next frame reports it. */
    feed_block(12, 0);
    agc_fail = true;
    expect_error(ESP_FAIL);
    agc_fail = false;
    for (unsigned c = 1; c < chunks; ++c) expect_frame(12, c, f);
    feed_block(13, 0);
    expect_frame(13, 0, chunks == 1 ? f : 0);
    for (unsigned c = 1; c < chunks; ++c) expect_frame(13, c, 0);
    assert(fifo_count == 0 && ring_count == 0);

    /* Only a negative feed or a full stamp queue is a true invariant error. */
    unsigned stamps = fifo_count;
    force_ret = true; forced_ret = 0;
    feed_block(14, 0);
    assert(!atomic_load(&s_aec.failed) && fifo_count == stamps);
    forced_ret = -3;
    feed_block(15, 0);
    assert(atomic_load(&s_aec.failed) && fifo_count == stamps);
    assert(!strcmp(last_failure, "feed") && last_rc == -3);
    expect_error(ESP_ERR_INVALID_STATE);
    reset(feed, fetch);
    fifo_capacity = 0;
    feed_block(16, 0);
    assert(atomic_load(&s_aec.failed) && !strcmp(last_failure, "timestamp queue"));
    last_failure = NULL;
}

int main(void)
{
    run(512, 512);   /* MR: one 32 ms fetch frame per feed */
    run(1024, 512);  /* two stamps per feed block */
    run(512, 256);
    puts("PASS: real AFE feed step + fetch pairing; ring-full drop/count, in-band gaps, "
         "late-stamp skip, AGC-failure carry, invariant errors only on ret<0/stamp overflow");
}
'''

with tempfile.TemporaryDirectory(prefix="cores3-afe-continuity-") as tmp:
    path = Path(tmp)
    (path / "esp_err.h").write_text(
        "typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n"
        "#define ESP_ERR_INVALID_ARG 0x102\n#define ESP_ERR_INVALID_STATE 0x103\n"
        "#define ESP_ERR_INVALID_SIZE 0x104\n#define ESP_ERR_TIMEOUT 0x107\n")
    (path / "test.c").write_text(prefix + defines + types + context + harness + code + tests,
                                 encoding="utf-8")
    # data_size is int in ESP-SR; IDF builds C without -Wsign-compare as well.
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-sign-compare", "-fsanitize=address,undefined",
                    "-I", str(path), "-I", str(media / "src"), "-I", str(media / "include"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
