#!/usr/bin/env python3
"""Host-only virtual-clock tests for the S3 playout FIFO and output lock.

No serial, network or hardware actions. Requires a C11 host compiler (CC/cc).
"""
import os
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source_dir = project / "components/starter_media/src"
media = (source_dir / "starter_media.c").read_text(encoding="utf-8")

controller_test = r'''
#include "starter_playout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void fifo_and_deadlines(void) {
    starter_playout_t q;
    uint8_t slot;
    starter_playout_init(&q);
    assert(q.target_ms == 200 && STARTER_PLAYOUT_MIN_MS == 60 &&
           STARTER_PLAYOUT_MAX_MS == 500);
    for (unsigned i = 0; i < 10; ++i) {
        assert(starter_playout_push(&q, i, 20000, 1000 + i * 20, 0,
                                    1000000ULL + i * 20000));
        if (i < 9) assert(!starter_playout_pop(&q, 1000000ULL + i * 20000, false, &slot));
    }
    for (unsigned i = 0; i < 10; ++i) {
        assert(starter_playout_pop(&q, 1180000, false, &slot) && slot == i);
        starter_playout_written(&q, 1180000);
    }
    assert(q.count == 0 && q.queued_us == 0 && q.starts == 1);
    starter_playout_idle(&q, 1280000); /* Empty app FIFO may still have DMA audio. */
    assert(!q.buffering);
    starter_playout_idle(&q, 1300000);
    assert(q.buffering);

    /* A short phrase waits for its bounded deadline; a quiet network gap must
     * not bypass the configured 200/500 ms prebuffer after only 60 ms. */
    starter_playout_init(&q);
    assert(starter_playout_push(&q, 7, 20000, 0, 0, 1000000));
    assert(!starter_playout_pop(&q, 1060000, false, &slot));
    assert(!starter_playout_pop(&q, 1199999, false, &slot));
    assert(starter_playout_pop(&q, 1200000, false, &slot) && slot == 7);
    starter_playout_init(&q);
    q.target_ms = 500;
    assert(starter_playout_push(&q, 9, 20000, 0, 0, 1000000));
    assert(!starter_playout_pop(&q, 1499999, false, &slot));
    assert(starter_playout_pop(&q, 1500000, false, &slot) && slot == 9);

    /* Variable 20/40/80 ms packets use audio duration, not packet count. */
    starter_playout_init(&q);
    const uint32_t durations[] = {20000, 40000, 80000, 80000};
    uint32_t timestamp = 0;
    for (unsigned i = 0; i < 4; ++i) {
        assert(starter_playout_push(&q, i, durations[i], timestamp, 0, 1000000ULL + timestamp * 1000));
        timestamp += durations[i] / 1000;
    }
    assert(q.queued_us == 220000);
    for (unsigned i = 0; i < 4; ++i)
        assert(starter_playout_pop(&q, 1140000, false, &slot) && slot == i);
    assert(q.queued_us == 0);
}

static void adaptation_and_timestamps(void) {
    starter_playout_t q;
    uint8_t slot;
    uint64_t now = 1000000;
    uint32_t timestamp = 0;
    starter_playout_init(&q);
    /* Remove entries with force so this test isolates the arrival estimator. */
    for (unsigned i = 0; i < 80; ++i) {
        assert(starter_playout_push(&q, 0, 20000, timestamp, 0, now));
        assert(starter_playout_pop(&q, now, true, &slot));
        timestamp += 20;
        now += 300000;
        assert(q.target_ms <= 500);
    }
    assert(q.target_ms == 500 && q.late_bursts > 0);
    for (unsigned i = 0; i < 100; ++i) {
        assert(starter_playout_push(&q, 0, 20000, timestamp, 0, now));
        assert(starter_playout_pop(&q, now, true, &slot));
        timestamp += 20; now += 20000;
    }
    assert(q.target_ms == 500); /* Two stable seconds must not collapse it. */
    for (unsigned i = 0; i < 14000; ++i) {
        assert(starter_playout_push(&q, 0, 20000, timestamp, 0, now));
        assert(starter_playout_pop(&q, now, true, &slot));
        timestamp += 20; now += 20000;
        assert(q.target_ms >= 60 && q.target_ms <= 500);
    }
    assert(q.target_ms == 60);

    starter_playout_init(&q);
    assert(starter_playout_push(&q, 0, 20000, UINT32_MAX - 9U, 0, 1000000));
    assert(starter_playout_push(&q, 1, 20000, 10, 0, 1020000));
    assert(q.timestamp_breaks == 0 && q.jitter_us == 0);
    assert(starter_playout_push(&q, 2, 20000, 800, 0, 5000000));
    assert(q.timestamp_breaks == 1 && q.target_ms == 200); /* Normal phrase gap. */
    assert(starter_playout_push(&q, 3, 20000, 800, 0, 5020000));
    assert(q.untimed_pairs == 1 && q.late_bursts == 0);
    for (unsigned i = 0; i < 4; ++i)
        assert(starter_playout_pop(&q, 5020000, true, &slot) && slot == i);
    assert(!starter_playout_pop(&q, 5020000, true, &slot));
}

static void bounded_pool(void) {
    starter_playout_t q;
    uint8_t slot;
    starter_playout_init(&q);
    q.target_ms = 500;
    assert(!starter_playout_push(&q, 0, 0, 0, 0, 1000000));
    for (unsigned i = 0; i < STARTER_PLAYOUT_CAPACITY; ++i)
        assert(starter_playout_push(&q, i, 1000, i, 0, 1000000ULL + i * 1000));
    assert(!starter_playout_push(&q, 99, 1000, 32, 0, 1032000));
    /* Tiny packets must not deadlock a full fixed pool below the time target. */
    for (unsigned i = 0; i < STARTER_PLAYOUT_CAPACITY; ++i)
        assert(starter_playout_pop(&q, 1032000, false, &slot) && slot == i);
    assert(q.count == 0 && q.queued_us == 0);
    starter_playout_init(&q);
    assert(q.target_ms == 200 && q.late_bursts == 0);
}
static void pcm_reblocking(void) {
    starter_playout_pcm_t pcm;
    starter_playout_pcm_init(&pcm);
    int16_t packet[1500];
    uint32_t input_hash = STARTER_PLAYOUT_HASH_INITIAL;
    uint32_t output_hash = STARTER_PLAYOUT_HASH_INITIAL;
    unsigned produced = 0, consumed = 0, completed = 0;
    const unsigned lengths[] = {160, 320, 640, 1, 79, 37, 1500, 13, 7};
    for (unsigned n = 0; n < 9000; ++n) {
        size_t length = lengths[n % 9];
        for (unsigned i = 0; i < length; ++i)
            packet[i] = (int16_t)((int)((produced + i) % 30001) - 15000);
        input_hash = starter_playout_pcm_hash(input_hash, packet, length);
        produced += length;
        size_t offset = 0;
        while (offset < length) {
            size_t take = starter_playout_pcm_append(&pcm, packet + offset, length - offset);
            assert(take > 0 && pcm.count <= 160);
            offset += take;
            if (pcm.count == 160) {
                for (unsigned i = 0; i < pcm.count; ++i)
                    assert(pcm.samples[i] == (int16_t)((int)((consumed + i) % 30001) - 15000));
                output_hash = starter_playout_pcm_hash(output_hash, pcm.samples, pcm.count);
                consumed += pcm.count;
                completed += starter_playout_pcm_commit(&pcm, true);
            }
        }
    }
    if (pcm.count) {
        output_hash = starter_playout_pcm_hash(output_hash, pcm.samples, pcm.count);
        consumed += pcm.count;
        completed += starter_playout_pcm_commit(&pcm, true);
    }
    assert(consumed == produced && completed == 9000 && input_hash == output_hash);
    assert(!starter_playout_tail_ready(1000000, 999999));
    assert(!starter_playout_tail_ready(1000000, 1059999));
    assert(starter_playout_tail_ready(1000000, 1060000));

    /* A failed first chunk must not count that packet as played when its
     * second chunk succeeds. A following independent packet still succeeds. */
    assert(starter_playout_pcm_append(&pcm, packet, 200) == 160);
    assert(starter_playout_pcm_commit(&pcm, false) == 0);
    assert(starter_playout_pcm_append(&pcm, packet + 160, 40) == 40);
    assert(starter_playout_pcm_append(&pcm, packet, 120) == 120);
    assert(starter_playout_pcm_commit(&pcm, true) == 1);
    for (unsigned i = 0; i < 160; ++i)
        assert(starter_playout_pcm_append(&pcm, packet, 1) == 1);
    assert(starter_playout_pcm_commit(&pcm, true) == 160);
    starter_playout_pcm_append(&pcm, packet, 200);
    starter_playout_pcm_commit(&pcm, false);
    starter_playout_pcm_init(&pcm); /* Mute/stop/new epoch clears partial packet. */
    assert(pcm.count == 0 && !pcm.failed_packet);
    assert(starter_playout_pcm_append(&pcm, packet, 7) == 7);
    assert(starter_playout_pcm_commit(&pcm, true) == 1);
    puts("PASS: PCM reblocking/9000 variable packets/order/hash/tail/failure/reset");
}
static void bounded_rate_control(void) {
    starter_playout_t q;
    uint8_t slot;
    starter_playout_init(&q);
    for (unsigned i = 0; i < 9; ++i) {
        assert(starter_playout_push(&q, 0, 20000, i * 20, 0, 1000000ULL + i * 20000));
        assert(starter_playout_pop(&q, 1000000ULL + i * 20000, true, &slot));
    }
    assert(q.timed_run == 8);
    q.buffering = false;
    q.rate_start_us = 1000000;
    q.last_received_us = 4000000;
    q.queued_us = 150000;
    assert(starter_playout_quantum(&q, 0, 4000000) == 160);
    q.queued_us = 139999;
    assert(starter_playout_quantum(&q, 0, 4000000) == 159);
    q.queued_us = 170000;
    assert(starter_playout_quantum(&q, 0, 4000000) == 159);
    q.queued_us = 180000;
    assert(starter_playout_quantum(&q, 0, 4000000) == 160);
    q.queued_us = 260001;
    assert(starter_playout_quantum(&q, 0, 4000000) == 161);
    q.queued_us = 230000;
    assert(starter_playout_quantum(&q, 0, 4000000) == 161);
    q.queued_us = 220000;
    assert(starter_playout_quantum(&q, 0, 4000000) == 160);
    q.queued_us = 1000;
    starter_playout_output_written(&q, 4000000, 160);
    assert(starter_playout_quantum(&q, 1200, 4000000) == 160); /* PCM + DMA estimate. */
    q.queued_us = 100000;
    q.target_ms = 60;
    assert(starter_playout_quantum(&q, 0, 4000000) == 160); /* Do not drain DMA to 60 ms. */
    assert(q.target_ms == 60); /* Rate floor must not change prebuffer policy. */
    q.target_ms = 200;
    q.queued_us = 0;
    assert(starter_playout_quantum(&q, 160, 4000000) == 159);
    q.timed_run = 7;
    assert(starter_playout_quantum(&q, 160, 4000000) == 160);
    q.timed_run = 8;
    q.rate_start_us = 3000000;
    assert(starter_playout_quantum(&q, 160, 4000000) == 160); /* Warmup. */
    q.rate_start_us = 1000000;
    assert(starter_playout_quantum(&q, 160, 4200001) == 160); /* Stale arrival. */
    q.buffering = true;
    assert(starter_playout_quantum(&q, 160, 4000000) == 160);

    /* Constant/reset stamps disable rate, but remain valid FIFO audio. */
    assert(starter_playout_push(&q, 0, 20000, 160, 0, 4020000));
    assert(q.timed_run == 0 && q.untimed_pairs == 1);
    assert(starter_playout_pop(&q, 4020000, true, &slot));
    assert(starter_playout_push(&q, 0, 20000, 0, 0, 4040000));
    assert(q.timed_run == 0 && q.timestamp_breaks == 1);
    starter_playout_init(&q);
    for (unsigned i = 0; i < 10; ++i) starter_playout_output_written(&q, 1000000, 160);
    assert(starter_playout_output_estimate(&q, 1000000) == 90000);
    assert(starter_playout_output_estimate(&q, 1030000) == 60000);
    assert(starter_playout_output_estimate(&q, 1100000) == 0);
    assert(starter_playout_output_estimate(&q, 999999) == 0);
    puts("PASS: rate limits/hysteresis/timestamp confidence/warmup/DMA estimate");
}

static void render_and_fade(void) {
    starter_playout_pcm_t pcm;
    int16_t source[161], output[162];
    uint16_t fade = 0;
    for (unsigned n = 159; n <= 161; ++n) {
        starter_playout_pcm_init(&pcm);
        pcm.limit = n;
        for (unsigned i = 0; i < n; ++i) source[i] = (int16_t)((int)i * 200 - 16000);
        assert(starter_playout_pcm_append(&pcm, source, n) == n);
        output[160] = 1111; output[161] = 2222;
        assert(starter_playout_pcm_render(&pcm, output, 160, false, &fade) == 160);
        assert(output[160] == 1111 && output[161] == 2222);
        assert(output[0] == source[0] && output[159] == source[n - 1]);
        for (unsigned i = 1; i < 160; ++i) assert(output[i] >= output[i - 1]);
        if (n == 160) assert(memcmp(source, output, 320) == 0);
        assert(memcmp(source, pcm.samples, n * sizeof(*source)) == 0);
        assert(starter_playout_pcm_commit(&pcm, true) == 1);
    }
    for (unsigned n = 1; n <= 160; ++n) {
        starter_playout_pcm_init(&pcm); pcm.limit = 161;
        assert(starter_playout_pcm_append(&pcm, source, n) == n);
        assert(starter_playout_pcm_render(&pcm, output, 160, false, &fade) == 0);
        assert(starter_playout_pcm_render(&pcm, output, n - 1, true, &fade) == 0);
        assert(starter_playout_pcm_render(&pcm, output, 160, true, &fade) == n);
        assert(memcmp(source, output, n * sizeof(*source)) == 0); /* Exact short tail. */
    }
    starter_playout_pcm_init(&pcm);
    for (unsigned i = 0; i < 161; ++i) source[i] = 16000;
    fade = STARTER_PLAYOUT_FADE_SAMPLES;
    assert(starter_playout_pcm_append(&pcm, source, 7) == 7);
    assert(starter_playout_pcm_render(&pcm, output, 160, true, &fade) == 7);
    assert(output[0] == 400 && output[6] == 2800 && fade == 33);
    assert(starter_playout_pcm_commit(&pcm, true) == 1);
    assert(starter_playout_pcm_append(&pcm, source, 160) == 160);
    assert(starter_playout_pcm_render(&pcm, output, 160, false, &fade) == 160);
    assert(output[0] == 3200 && output[32] == 16000 && output[159] == 16000 && fade == 0);
    assert(starter_playout_pcm_commit(&pcm, true) == 1);
    assert(starter_playout_pcm_append(&pcm, source, 160) == 160);
    assert(starter_playout_pcm_render(&pcm, output, 160, false, &fade) == 160);
    assert(memcmp(source, output, 320) == 0); /* No per-packet fade. */
    puts("PASS: render 159/160/161 bounds/endpoints/identity/tails/start-only fade");
}

static void rate_source_conservation(void) {
    starter_playout_pcm_t pcm;
    starter_playout_pcm_init(&pcm);
    int16_t packet[640], output[160];
    unsigned produced = 0, consumed = 0, completed = 0, chunks = 0;
    uint32_t in_hash = STARTER_PLAYOUT_HASH_INITIAL, out_hash = STARTER_PLAYOUT_HASH_INITIAL;
    uint16_t fade = STARTER_PLAYOUT_FADE_SAMPLES;
    const unsigned lengths[] = {160, 1, 320, 79, 640};
    for (unsigned n = 0; n < 9000; ++n) {
        size_t length = lengths[n % 5], offset = 0;
        for (unsigned i = 0; i < length; ++i)
            packet[i] = (int16_t)((int)((produced + i) % 30001) - 15000);
        in_hash = starter_playout_pcm_hash(in_hash, packet, length);
        produced += length;
        while (offset < length) {
            if (pcm.count == 0) pcm.limit = 159 + chunks % 3;
            size_t take = starter_playout_pcm_append(&pcm, packet + offset, length - offset);
            assert(take > 0 && pcm.count <= pcm.limit);
            offset += take;
            if (pcm.count == pcm.limit) {
                assert(starter_playout_pcm_render(&pcm, output, 160, false, &fade) == 160);
                out_hash = starter_playout_pcm_hash(out_hash, pcm.samples, pcm.count);
                consumed += pcm.count;
                completed += starter_playout_pcm_commit(&pcm, true);
                ++chunks;
            }
        }
    }
    if (pcm.count) {
        assert(starter_playout_pcm_render(&pcm, output, 160, true, &fade) == pcm.count);
        out_hash = starter_playout_pcm_hash(out_hash, pcm.samples, pcm.count);
        consumed += pcm.count;
        completed += starter_playout_pcm_commit(&pcm, true);
    }
    assert(produced == consumed && in_hash == out_hash && completed == 9000);
    puts("PASS: variable-rate 9000 packet source conservation and packet boundaries");
}
int main(void) {
    fifo_and_deadlines(); adaptation_and_timestamps(); bounded_pool();
    pcm_reblocking();
    bounded_rate_control(); render_and_fade(); rate_source_conservation();
    puts("PASS: playout duration/FIFO/deadlines/adaptation/timestamps/capacity");
}
'''

# Exercise the actual render function, not a second implementation of it.
decode_function = media[media.index("static size_t decode_audio_item("):
                        media.index("static bool play_audio_chunk(")]
decode_test = r'''
#include "starter_playout.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#define ESP_AUDIO_ERR_OK 0
typedef int esp_audio_err_t;
typedef struct { int mode; uint32_t generation, epoch;
    struct { unsigned length; } frame; uint8_t payload[1500]; } audio_rx_item_t;
typedef struct { uint8_t *buffer; size_t len; } esp_audio_dec_in_raw_t;
typedef struct { uint8_t *buffer; size_t len, decoded_size; } esp_audio_dec_out_frame_t;
typedef struct { int unused; } esp_audio_dec_info_t;
static struct { atomic_uint_fast32_t epoch, stale, decode_max_us,
    decoded_samples, pcm_in_hash; } s_playout_diag;
static atomic_uint_fast32_t s_audio_decoded, s_audio_decode_failed;
static int16_t s_decode_pcm[1500];
static void *s_g711_decoder = (void *)1;
static int64_t now_us;
static unsigned calls, response;
static int64_t esp_timer_get_time(void) { return now_us; }
static bool same_session(int mode, uint32_t gen) { return mode == 1 && gen == 7; }
static void update_max_counter(atomic_uint_fast32_t *value, uint32_t next) {
    if (atomic_load(value) < next) atomic_store(value, next);
}
static esp_audio_err_t esp_g711a_dec_decode(void *decoder, esp_audio_dec_in_raw_t *in,
                    esp_audio_dec_out_frame_t *out, esp_audio_dec_info_t *info) {
    assert(decoder == s_g711_decoder && in->len <= 1500 && out->len == 3000);
    (void)info; ++calls; now_us += 100;
    for (unsigned i = 0; i < in->len; ++i) ((int16_t *)out->buffer)[i] = i;
    out->decoded_size = response == 1 ? 3002 : response == 2 ? 0 :
                        response == 3 ? in->len * 2 - 1 : in->len * 2;
    return response == 4 ? -1 : 0;
}
''' + decode_function + r'''
int main(void) {
    audio_rx_item_t item = {.mode=1, .generation=7, .epoch=1, .frame={640}};
    s_playout_diag.epoch = 1;
    s_playout_diag.pcm_in_hash = STARTER_PLAYOUT_HASH_INITIAL;
    assert(decode_audio_item(&item) == 640);
    assert(s_audio_decoded == 1 && s_playout_diag.decoded_samples == 640);
    assert(s_playout_diag.pcm_in_hash == starter_playout_pcm_hash(
        STARTER_PLAYOUT_HASH_INITIAL, s_decode_pcm, 640));
    for (response = 1; response <= 4; ++response) assert(!decode_audio_item(&item));
    assert(s_audio_decoded == 1 && s_audio_decode_failed == 4);
    assert(s_playout_diag.decoded_samples == 640 && s_playout_diag.decode_max_us == 100);
    item.epoch = 0;
    assert(!decode_audio_item(&item) && calls == 5 && s_playout_diag.stale == 1);
    assert(!decode_audio_item(NULL) && calls == 5 && s_playout_diag.stale == 2);
    puts("PASS: decoder size/error/epoch/sample/hash diagnostics");
}
'''
lock_functions = media[media.index("static void update_max_counter("):
                       media.index("static bool same_session(")]
play_function = media[media.index("static bool play_audio_chunk("):
                      media.index("static void release_audio_rx_slot(")]
lock_test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define AUDIO_RX_BYTES 1500U
#define AUDIO_PACKET_SAMPLES 160U
#define AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT 4U
#define AUDIO_OUTPUT_LOCK_WAIT_MS 500U
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define ESP_AUDIO_ERR_OK 0
#define ESP_CODEC_DEV_OK 0
typedef unsigned TickType_t;
typedef int starter_tirtc_mode_t;
typedef enum { OUTPUT_OWNER_NONE, OUTPUT_OWNER_RX, OUTPUT_OWNER_VOLUME,
    OUTPUT_OWNER_MUTE, OUTPUT_OWNER_LCD, OUTPUT_OWNER_PROMPT,
    OUTPUT_OWNER_START, OUTPUT_OWNER_STOP } audio_output_owner_t;
static struct { atomic_uint_fast32_t epoch, owner, lock_max_us, lock_timeouts,
    last_lock_owner, stale, write_max_us, slow_writes; } s_playout_diag;
static atomic_uint_fast32_t s_audio_dropped, s_audio_write_failed, s_audio_playback_blocked;
static atomic_bool s_speaker_muted;
static int16_t input_pcm[160], s_play_stereo[6000], s_playback_previous;
static uint32_t s_playback_resampler_generation, s_playback_resampler_epoch;
static void *s_audio_output_mutex = (void *)1, *s_speaker_dev = (void *)3;
static bool s_amp_enabled = true, held, fail_lock, fail_write, change_epoch;
static unsigned writes, lock_delay, write_delay, expected_samples = 160;
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static bool same_session(int mode, uint32_t generation) { return mode == 1 && generation == 7; }
static int xSemaphoreTake(void *mutex, TickType_t timeout) {
    assert(mutex == s_audio_output_mutex && timeout == 500 && !held);
    /* Before we own the lock, prompt PCM must remain untouched. */
    for (unsigned i = 0; i < 640; ++i) assert(s_play_stereo[i] == -777);
    now_us += lock_delay;
    if (change_epoch) ++s_playout_diag.epoch;
    if (fail_lock) return 0;
    held = true; return 1;
}
static void xSemaphoreGive(void *mutex) { assert(mutex == s_audio_output_mutex && held); held = false; }
static int esp_codec_dev_write(void *dev, void *pcm, size_t bytes) {
    assert(held && dev == s_speaker_dev && pcm == s_play_stereo && bytes == expected_samples * 8);
    for (unsigned i = 0; i < expected_samples; ++i) {
        int16_t current = 100 + i * 2, midpoint = i == 0 ? 100 : current - 1;
        assert(s_play_stereo[i * 4] == midpoint && s_play_stereo[i * 4 + 1] == midpoint);
        assert(s_play_stereo[i * 4 + 2] == current && s_play_stereo[i * 4 + 3] == current);
    }
    ++writes; now_us += write_delay; return fail_write ? -1 : 0;
}
''' + lock_functions + play_function + r'''
static void prepare(void) {
    held = fail_lock = fail_write = change_epoch = false;
    s_playout_diag.epoch = 1; s_playout_diag.owner = OUTPUT_OWNER_PROMPT;
    s_playback_resampler_generation = 0; lock_delay = write_delay = 0;
    s_playback_resampler_epoch = 0; expected_samples = 160;
    for (unsigned i = 0; i < 6000; ++i) s_play_stereo[i] = -777;
    for (unsigned i = 0; i < 160; ++i) input_pcm[i] = 100 + i * 2;
}
int main(void) {
    prepare(); fail_lock = true; lock_delay = 500000;
    assert(!play_audio_chunk(1, 7, 1, input_pcm, 160, false) && writes == 0 && !held);
    assert(s_playout_diag.lock_timeouts == 1 && s_audio_dropped == 1);
    assert(s_playout_diag.last_lock_owner == OUTPUT_OWNER_PROMPT);
    prepare(); write_delay = 20000;
    assert(play_audio_chunk(1, 7, 1, input_pcm, 160, false) && writes == 1 && !held);
    assert(s_playout_diag.slow_writes == 0);
    prepare(); change_epoch = true;
    assert(!play_audio_chunk(1, 7, 1, input_pcm, 160, false) && writes == 1 && !held && s_audio_playback_blocked == 1);
    prepare(); fail_write = true; write_delay = 50000;
    assert(!play_audio_chunk(1, 7, 1, input_pcm, 160, false) && writes == 2 && !held && s_audio_write_failed == 1);
    assert(s_playout_diag.slow_writes == 1 && s_playout_diag.write_max_us == 50000);
    prepare();
    assert(!play_audio_chunk(1, 7, 0, input_pcm, 160, false) && writes == 2);
    assert(!play_audio_chunk(1, 7, 1, input_pcm, 161, false) && writes == 2);
    expected_samples = 7;
    assert(play_audio_chunk(1, 7, 1, input_pcm, 7, false) && writes == 3);
    prepare();
    s_playback_resampler_generation = 7; s_playback_resampler_epoch = 1;
    s_playback_previous = 30000; /* A pre-gap endpoint must not bypass a restart fade. */
    assert(play_audio_chunk(1, 7, 1, input_pcm, 160, true) && writes == 4);
    puts("PASS: output PCM ownership/timeout/epoch/write diagnostics");
}
'''

with tempfile.TemporaryDirectory(prefix="xiaotai-playout-") as folder:
    root = Path(folder)
    for name, code, extras in (
        ("controller", controller_test, [str(source_dir / "starter_playout.c")]),
        ("decoder", decode_test, [str(source_dir / "starter_playout.c")]),
        ("lock", lock_test, []),
    ):
        src = root / (name + ".c")
        exe = root / (name + (".exe" if os.name == "nt" else ""))
        src.write_text(code, encoding="utf-8")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(source_dir), str(src), *extras, "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
