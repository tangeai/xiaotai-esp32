// SPDX-License-Identifier: MIT
// Replaces legacy: tests/native/cores3/test_cores3_capture_repack.py
#include "level.h"
#include "processors/frontend_pairing.h"
#include "product_test.h"
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"
#include "xiaotai/xiaotai.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RATE = 16000U, LANES = 4U, MAX_FEED = 1024U };
static int16_t raw[MAX_FEED * LANES], original[MAX_FEED * LANES], mono[MAX_FEED];
static uint32_t random_state = UINT32_C(0x6841d2f3);
static _Thread_local xiaotai_queue_t *free_queue, *ready_queue;
static _Thread_local unsigned queue_init_index;
static _Thread_local bool inspect_queues, reject_pending;

typedef struct {
    media_frontend_pairing_t *pairing;
    unsigned feed, fetch, fetched, feed_calls;
} replay_t;

xiaotai_err_t __real_xiaotai_start(const xiaotai_config_t *config);
xiaotai_err_t __wrap_xiaotai_start(const xiaotai_config_t *config);
xiaotai_err_t __real_xiaotai_queue_init(xiaotai_queue_t *, size_t, size_t, xiaotai_mem_kind_t);
xiaotai_err_t __wrap_xiaotai_queue_init(xiaotai_queue_t *, size_t, size_t, xiaotai_mem_kind_t);
bool __real_xiaotai_queue_send(xiaotai_queue_t *, const void *, uint32_t);
bool __wrap_xiaotai_queue_send(xiaotai_queue_t *, const void *, uint32_t);

xiaotai_err_t __wrap_xiaotai_queue_init(xiaotai_queue_t *queue, size_t depth, size_t item_size,
                                       xiaotai_mem_kind_t storage)
{
    if (inspect_queues) {
        if (queue_init_index == 0) free_queue = queue;
        if (queue_init_index == 1) ready_queue = queue;
        ++queue_init_index;
    }
    return __real_xiaotai_queue_init(queue, depth, item_size, storage);
}

bool __wrap_xiaotai_queue_send(xiaotai_queue_t *queue, const void *item, uint32_t timeout_ms)
{
    if (reject_pending && queue == ready_queue) {
        reject_pending = false;
        assert(!timeout_ms);
        return false;
    }
    return __real_xiaotai_queue_send(queue, item, timeout_ms);
}

static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void load_vector(unsigned feed, unsigned pattern)
{
    (void)next_random(); /* The legacy producer fixture chooses a pool slot first. */
    for (size_t i = 0; i < (size_t)feed * LANES; ++i) {
        switch (pattern) {
            case 0: raw[i] = INT16_MIN; break;
            case 1: raw[i] = INT16_MAX; break;
            case 2: raw[i] = i & 1U ? INT16_MIN : INT16_MAX; break;
            case 3: raw[i] = (int16_t)((int)(i % 7U) - 3); break;
            default: raw[i] = (int16_t)next_random(); break;
        }
    }
    (void)next_random(); /* Consume the legacy mute-epoch draw without moving it into the AFE. */
    memcpy(original, raw, sizeof(raw));
}

static int feed_mr(void *opaque, const int16_t *mr)
{
    replay_t *r = opaque;
    for (unsigned i = 0; i < r->feed; ++i) {
        assert(mr[2U * i] == original[LANES * i] && mr[2U * i + 1U] == original[LANES * i + 1U]);
        mono[i] = mr[2U * i];
    }
    r->fetched = 0;
    ++r->feed_calls;
    return (int)(r->feed * 2U * sizeof(*mr));
}

static xiaotai_err_t fetch_mr(void *opaque, const int16_t **pcm, size_t *count, uint32_t timeout_ms)
{
    replay_t *r = opaque;
    assert(timeout_ms == 200U);
    if (r->fetched == r->feed) return XIAOTAI_ERR_TIMEOUT;
    *pcm = mono + r->fetched;
    *count = r->fetch;
    r->fetched += r->fetch;
    return XIAOTAI_OK;
}

static void open_mr(replay_t *r, unsigned feed, unsigned fetch, const xiaotai_audio_desc_t *input)
{
    *r = (replay_t){.feed = feed, .fetch = fetch, .fetched = feed};
    const int32_t slots = xiaotai_media_param("aec.input_slots", 0);
    const int32_t ring = xiaotai_media_param("aec.ring_frames", 17);
    const int32_t wait = xiaotai_media_param("aec.stamp_wait_ms", 20);
    const bool strict = xiaotai_media_param("aec.strict_feed", 0) != 0;
    const bool count_monitor = xiaotai_media_param("aec.count_monitor_clipping", 0) != 0;
    assert(!slots && ring == 17 && wait == 20 && !strict);
    const media_frontend_pairing_config_t cfg = {.input = input, .microphones = 1,
        .feed_samples = (uint16_t)feed, .fetch_samples = (uint16_t)fetch,
        .input_slots = (uint8_t)(4096U / feed), .ring_frames = (uint8_t)ring,
        .stamp_wait_ms = (uint8_t)wait, .strict_feed = strict, .count_monitor_clipping = count_monitor};
    const media_frontend_ops_t ops = {.feed = feed_mr, .fetch = fetch_mr, .ctx = r};
    queue_init_index = 0;
    inspect_queues = true;
    assert(media_frontend_pairing_open(&cfg, &ops, &r->pairing) == XIAOTAI_OK);
    inspect_queues = false;
    assert(queue_init_index == 3 && free_queue && ready_queue);
}

static unsigned measure(const int16_t *pcm, unsigned count)
{
    uint64_t square = 0;
    int64_t sum = 0;
    uint32_t peak = 0;
    unsigned clipped = 0;
    for (unsigned i = 0; i < count; ++i) {
        const int32_t sample = pcm[i * LANES];
        const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        sum += sample;
        square += (int64_t)sample * sample;
        if (magnitude > peak) peak = magnitude;
        clipped += magnitude >= 32700U;
    }
    const xiaotai_media_level_t got = xiaotai_media_level_measure(pcm, count, LANES);
    assert(got.dc == sum / count && got.peak == peak && got.rms == (uint32_t)sqrt((double)(square / count)));
    return clipped;
}

static void compare_vectors(const xiaotai_audio_desc_t *input)
{
    const unsigned sizes[][2] = {{1024, 512}, {512, 512}, {512, 256}, {1024, 1024}};
    unsigned comparisons = 0, changed_clip_vectors = 0;
    for (size_t shape = 0; shape < sizeof(sizes) / sizeof(*sizes); ++shape) {
        const unsigned feed = sizes[shape][0], fetch = sizes[shape][1], chunks = feed / fetch;
        replay_t r;
        open_mr(&r, feed, fetch, input);
        for (unsigned pattern = 0; pattern < 68; ++pattern) {
            load_vector(feed, pattern);
            const int64_t stamp = (pattern & 1U ? INT64_C(9123456789) : 3) * 1000;
            const uint32_t loss = pattern % 3U == 0 ? feed * (pattern % 5U) : 0;
            xiaotai_processor_stats_t before, after;
            media_frontend_pairing_stats(r.pairing, &before);
            assert(media_frontend_pairing_feed(r.pairing, raw, feed * LANES * sizeof(*raw), stamp, loss) == XIAOTAI_OK);
            assert(media_frontend_pairing_feed_next(r.pairing, 0) == XIAOTAI_OK && r.feed_calls == pattern + 1U);
            unsigned clipped_primary = 0, clipped_reference = 0, clipped_monitor = 0;
            for (unsigned c = 0; c < chunks; ++c) {
                xiaotai_processor_frame_t frame;
                assert(media_frontend_pairing_fetch(r.pairing, &frame, 200) == XIAOTAI_OK);
                assert(frame.samples == fetch && frame.size == fetch * sizeof(int16_t) &&
                       frame.encoding == XIAOTAI_CODEC_PCM16 && frame.lost_samples == (c ? 0 : loss));
                assert(frame.captured_us == stamp - (int64_t)(chunks - c - 1U) * fetch * 1000000U / RATE);
                const int16_t *start = original + c * fetch * LANES;
                for (unsigned i = 0; i < fetch; ++i) assert(((const int16_t *)frame.data)[i] == start[i * LANES]);
                clipped_primary += measure(start, fetch);
                clipped_reference += measure(start + 1, fetch);
                clipped_monitor += measure(start + 2, fetch);
            }
            media_frontend_pairing_stats(r.pairing, &after);
            const uint32_t mic_clips = after.clipped_mic - before.clipped_mic;
            const unsigned legacy_clips = clipped_primary + clipped_monitor;
            assert(mic_clips == clipped_primary || mic_clips == legacy_clips);
            assert(after.clipped_reference - before.clipped_reference == clipped_reference);
            assert(!memcmp(original, raw, sizeof(raw)));
            if (mic_clips != legacy_clips) {
                if (!changed_clip_vectors) {
                    printf("CoreS3 %u/%u pattern=%u mic_clipped: old MIC1+MIC2=%u, new mic=%u, ref=%u\n",
                           feed, fetch, pattern, legacy_clips, mic_clips, clipped_reference);
                }
                ++changed_clip_vectors;
            }
            ++comparisons;
        }
        media_frontend_pairing_close(r.pairing);
    }
    assert(comparisons == 272);
    printf("PASS: CoreS3 %u legacy MR/PCM/stamp/level vectors; monitor clipping differs on %u\n",
           comparisons, changed_clip_vectors);
    fflush(stdout);
    if (getenv("XIAOTAI_HOST_MR_REQUIRE_OLD_CLIP") && changed_clip_vectors) {
        fputs("FAIL: strict legacy MIC1+MIC2 clipping parity\n", stderr);
        exit(EXIT_FAILURE);
    }
}

static void queue_boundaries(const xiaotai_audio_desc_t *input)
{
    replay_t r;
    open_mr(&r, 1024, 512, input);
    assert(media_frontend_pairing_feed(r.pairing, raw, 0, 0, 0) == XIAOTAI_ERR_INVALID_SIZE);
    assert(!xiaotai_queue_count(ready_queue) && xiaotai_queue_count(free_queue) == 4);
    for (unsigned i = 0; i < 4; ++i)
        assert(media_frontend_pairing_feed(r.pairing, raw, sizeof(raw), 0, 0) == XIAOTAI_OK);
    for (unsigned i = 0; i < 101; ++i)
        assert(media_frontend_pairing_feed(r.pairing, raw, sizeof(raw), 0, 0) == XIAOTAI_ERR_NO_MEM);
    assert(!memcmp(original, raw, sizeof(raw)) && xiaotai_queue_count(ready_queue) == 4 && !r.feed_calls);
    for (unsigned i = 0; i < 4; ++i) {
        assert(media_frontend_pairing_feed_next(r.pairing, 0) == XIAOTAI_OK);
        for (unsigned c = 0; c < 2; ++c) {
            xiaotai_processor_frame_t frame;
            assert(media_frontend_pairing_fetch(r.pairing, &frame, 200) == XIAOTAI_OK && !frame.lost_samples);
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        assert(media_frontend_pairing_feed(r.pairing, raw, sizeof(raw), 77000 + i * 32000,
                                           i ? 0 : 2U * r.feed) == XIAOTAI_OK);
        assert(media_frontend_pairing_feed_next(r.pairing, 0) == XIAOTAI_OK);
        for (unsigned c = 0; c < 2; ++c) {
            xiaotai_processor_frame_t frame;
            assert(media_frontend_pairing_fetch(r.pairing, &frame, 200) == XIAOTAI_OK);
            assert(frame.captured_us == 45000 + (i + c) * 32000);
            assert(frame.lost_samples == (!i && !c ? 103U * r.feed : 0));
        }
    }
    xiaotai_processor_stats_t stats;
    media_frontend_pairing_stats(r.pairing, &stats);
    assert(stats.input_full == 101 && stats.lost_samples == 103U * r.feed && !stats.failed);
    reject_pending = true;
    assert(media_frontend_pairing_feed(r.pairing, raw, sizeof(raw), 0, r.feed) == XIAOTAI_ERR_INVALID_STATE);
    assert(!reject_pending && xiaotai_queue_count(free_queue) == 4 && !xiaotai_queue_count(ready_queue));
    assert(media_frontend_pairing_feed(r.pairing, raw, sizeof(raw), 0, 0) == XIAOTAI_ERR_INVALID_STATE);
    media_frontend_pairing_stats(r.pairing, &stats);
    assert(stats.failed && stats.errors == 1 && r.feed_calls == 6U);
    media_frontend_pairing_close(r.pairing);
    puts("PASS: CoreS3 rejected 101 full-pool feeds; 103-block carry, guard and pending fault preserved");
}

xiaotai_err_t __wrap_xiaotai_start(const xiaotai_config_t *config)
{
    const xiaotai_err_t result = __real_xiaotai_start(config);
    if (result != XIAOTAI_OK || !getenv("XIAOTAI_HOST_MR_LEGACY")) return result;
    const xiaotai_audio_input_t *in = xiaotai_hal_audio_input(0);
    const xiaotai_channel_role_t roles[] = {XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE,
                                           XIAOTAI_CHANNEL_MONITOR, XIAOTAI_CHANNEL_UNUSED};
    assert(in && in->desc.encoding == XIAOTAI_CODEC_PCM16 && in->desc.rate_hz == RATE &&
           in->desc.channels == LANES && in->info.block_frames == 256);
    assert(!memcmp(in->desc.roles, roles, sizeof(roles)));
    compare_vectors(&in->desc);
    queue_boundaries(&in->desc);
    return result;
}
