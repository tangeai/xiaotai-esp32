// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_afe_pipeline_counters
// Replaces legacy: tests/native/cores3/test_cores3_afe_continuity.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "level.h"
#include "processors/frontend_pairing.h"
#include "xiaotai/port/os.h"

enum { FEED = 8, FETCH = 4, CHANNELS = 4, QUEUE = 8 };

typedef struct {
    media_frontend_pairing_t *pairing;
    int16_t packed[FEED * 3], pcm[QUEUE][FETCH];
    unsigned head, count;
    uint8_t microphones;
    int feed_ret;
    bool early, short_frame;
    uint32_t early_wait;
    uint32_t feed_duration_us;
    xiaotai_err_t early_result;
    int64_t early_stamp, early_duration;
} frontend_t;

static int64_t s_fixture_now_us;

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    return s_fixture_now_us ? s_fixture_now_us : __real_xiaotai_time_us();
}

static int ring_feed(void *opaque, const int16_t *pcm)
{
    frontend_t *f = opaque;
    const size_t channels = f->microphones + 1U;
    memcpy(f->packed, pcm, FEED * channels * sizeof(*pcm));
    if (s_fixture_now_us) s_fixture_now_us += f->feed_duration_us;
    if (f->feed_ret <= 0) return f->feed_ret;
    for (unsigned block = 0; block < FEED / FETCH; ++block) {
        assert(f->count < QUEUE);
        const unsigned index = (f->head + f->count++) % QUEUE;
        for (size_t i = 0; i < FETCH; ++i) f->pcm[index][i] = pcm[(block * FETCH + i) * channels];
    }
    if (f->early) {
        xiaotai_processor_frame_t frame;
        const int64_t start = xiaotai_time_us();
        f->early_result = media_frontend_pairing_fetch(f->pairing, &frame, f->early_wait);
        f->early_duration = xiaotai_time_us() - start;
        if (f->early_result == XIAOTAI_OK) f->early_stamp = frame.captured_us;
    }
    return f->feed_ret;
}

static xiaotai_err_t ring_fetch(void *opaque, const int16_t **pcm, size_t *samples, uint32_t timeout_ms)
{
    (void)timeout_ms;
    frontend_t *f = opaque;
    if (!f->count) return XIAOTAI_ERR_TIMEOUT;
    --f->count;
    *pcm = f->pcm[f->head];
    *samples = f->short_frame ? FETCH - 1 : FETCH;
    f->head = (f->head + 1U) % QUEUE;
    return XIAOTAI_OK;
}

static void start(frontend_t *f, bool strict, uint8_t microphones, bool count_monitor)
{
    memset(f, 0, sizeof(*f));
    f->microphones = microphones;
    f->feed_ret = FEED;
    const xiaotai_audio_desc_t in = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = CHANNELS,
        .roles = {XIAOTAI_CHANNEL_UNUSED, XIAOTAI_CHANNEL_REFERENCE, XIAOTAI_CHANNEL_MIC,
            microphones == 1 ? XIAOTAI_CHANNEL_MONITOR : XIAOTAI_CHANNEL_MIC}};
    uint8_t slots[3];
    assert(media_frontend_pairing_slots(&in, microphones, slots));
    assert(slots[0] == 2 && slots[microphones] == 1);
    assert(!media_frontend_pairing_slots(&in, microphones == 1 ? 2 : 1, NULL));
    if (microphones == 2) assert(slots[1] == 3);

    const media_frontend_pairing_config_t cfg = {.input = &in, .feed_samples = FEED, .fetch_samples = FETCH,
        .microphones = microphones, .input_slots = 2, .ring_frames = strict ? 12 : 17,
        .stamp_wait_ms = strict ? 0 : 20, .strict_feed = strict, .count_monitor_clipping = count_monitor};
    const media_frontend_ops_t port = {.feed = ring_feed, .fetch = ring_fetch, .ctx = f};
    assert(media_frontend_pairing_open(&cfg, &port, &f->pairing) == XIAOTAI_OK);
}

static xiaotai_err_t feed(frontend_t *f, int64_t end_us, uint32_t lost)
{
    int16_t tdm[CHANNELS * FEED];
    for (int i = 0; i < FEED; ++i) {
        tdm[CHANNELS * i] = 32767;
        tdm[CHANNELS * i + 1] = i == 0 ? -32768 : (int16_t)(-900 - i);
        tdm[CHANNELS * i + 2] = (int16_t)(100 + i);
        tdm[CHANNELS * i + 3] = i == 0 ? 32700 : (int16_t)(300 + i);
    }
    const xiaotai_media_level_t mic = xiaotai_media_level_measure(tdm + 2, FEED, CHANNELS);
    const xiaotai_media_level_t reference = xiaotai_media_level_measure(tdm + 1, FEED, CHANNELS);
    assert(mic.peak == 107 && mic.dc == 103 && reference.peak == 32768 && reference.dc == -4887);
    return media_frontend_pairing_feed(f->pairing, tdm, sizeof(tdm), end_us, lost);
}

static void fetch(frontend_t *f, int64_t end_us, uint32_t lost, bool second)
{
    xiaotai_processor_frame_t frame;
    assert(media_frontend_pairing_fetch(f->pairing, &frame, 30) == XIAOTAI_OK);
    assert(frame.captured_us == end_us && frame.lost_samples == lost);
    assert(frame.size == FETCH * sizeof(int16_t) && frame.samples == FETCH && frame.encoding == XIAOTAI_CODEC_PCM16);
    const int16_t *pcm = frame.data;
    assert(pcm[0] == (second ? 104 : 100) && pcm[3] == (second ? 107 : 103));
}

static void recoverable(uint8_t microphones)
{
    frontend_t f;
    start(&f, false, microphones, false);
    assert(feed(&f, 1000, 5) == XIAOTAI_OK);
    assert(feed(&f, 2000, 7) == XIAOTAI_OK);
    assert(feed(&f, 3000, 3) == XIAOTAI_ERR_NO_MEM);
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_OK);
    assert(f.packed[0] == 100 && f.packed[microphones] == -32768);
    if (microphones == 2) assert(f.packed[1] == 32700);
    fetch(&f, 750, 5, false);
    fetch(&f, 1000, 0, true);
    f.feed_ret = 0;
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_ERR_NO_MEM);
    assert(feed(&f, 4000, 1) == XIAOTAI_OK);
    f.feed_ret = FEED;
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_OK);
    fetch(&f, 3750, 27, false);
    fetch(&f, 4000, 0, true);
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_processor_stats_t counters;
    media_frontend_pairing_stats(f.pairing, &counters);
    assert(!counters.failed && !counters.errors && counters.input_full == 2);
    assert(counters.feeds == 2 && counters.frames == 4 && counters.lost_samples == 32);
    assert(counters.clipped_reference == 3 && counters.clipped_mic == (microphones == 2 ? 3U : 0U));

    /* PCM outruns its metadata. Wait ends after 20 ms, then the stamp for
       that lost frame is ignored on the next successful fetch. */
    assert(feed(&f, 5000, 9) == XIAOTAI_OK);
    f.early = true;
    f.early_wait = 1000;
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_OK);
    assert(f.early_result == XIAOTAI_ERR_TIMEOUT && f.early_duration >= 17000 && f.early_duration < 600000);
    f.early = false;
    fetch(&f, 5000, FETCH + 9, true);

    assert(feed(&f, 6000, 0) == XIAOTAI_OK);
    f.feed_ret = -1;
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_ERR_INVALID_STATE);
    assert(feed(&f, 7000, 0) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_processor_frame_t frame;
    assert(media_frontend_pairing_fetch(f.pairing, &frame, 0) == XIAOTAI_ERR_INVALID_STATE);
    media_frontend_pairing_stats(f.pairing, &counters);
    assert(counters.failed && counters.errors == 1 && counters.fetch_timeouts == 1 && counters.lost_samples == 45);
    media_frontend_pairing_close(f.pairing);
}

static void strict_legacy(void)
{
    frontend_t f;
    start(&f, true, 2, false);
    assert(feed(&f, 1000, 0) == XIAOTAI_OK);
    f.early = true;
    f.early_wait = 0;
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_OK);
    assert(f.early_result == XIAOTAI_OK && f.early_stamp == 750);
    f.early = false;
    fetch(&f, 1000, 0, true);
    assert(feed(&f, 2000, 0) == XIAOTAI_OK);
    f.feed_ret = 0;
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_ERR_INVALID_STATE);
    assert(feed(&f, 3000, 0) == XIAOTAI_ERR_INVALID_STATE);
    media_frontend_pairing_close(f.pairing);

    start(&f, false, 1, false);
    assert(feed(&f, 1000, 0) == XIAOTAI_OK);
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_OK);
    f.short_frame = true;
    xiaotai_processor_frame_t frame;
    assert(media_frontend_pairing_fetch(f.pairing, &frame, 0) == XIAOTAI_ERR_INVALID_STATE);
    media_frontend_pairing_close(f.pairing);
}

static void lifetime_counters(void)
{
    frontend_t f;
    start(&f, true, 2, false);
    s_fixture_now_us = 1000000;
    f.feed_duration_us = 16000;
    assert(feed(&f, 1000, 0) == XIAOTAI_OK);
    assert(feed(&f, 2000, 0) == XIAOTAI_OK);
    for (unsigned lost = 0; lost < 4; ++lost)
        assert(feed(&f, 3000, 0) == XIAOTAI_ERR_NO_MEM);
    for (unsigned i = 0; i < 17; ++i) {
        const int64_t stamp = (int64_t)(i + 1) * 1000;
        if (i >= 2) assert(feed(&f, stamp, 0) == XIAOTAI_OK);
        assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_OK);
        fetch(&f, stamp - 250, i == 2 ? 4 * FEED : 0, false);
        fetch(&f, stamp, 0, true);
    }
    xiaotai_processor_frame_t frame;
    for (unsigned i = 0; i < 7; ++i)
        assert(media_frontend_pairing_fetch(f.pairing, &frame, 0) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_processor_stats_t before;
    media_frontend_pairing_stats(f.pairing, &before);
    assert(before.feeds == 17 && before.frames == 34 && before.input_full == 4);
    assert(before.fetch_timeouts == 7 && before.process_max_us == 16000 && !before.failed);
    assert(before.deadline_misses == 17);

    f.feed_duration_us = 63125;
    f.feed_ret = 0;
    assert(feed(&f, 19000, 0) == XIAOTAI_OK);
    assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_processor_stats_t after;
    media_frontend_pairing_stats(f.pairing, &after);
    assert(after.feeds == 17 && after.frames == 34 && after.input_full == 4);
    assert(after.fetch_timeouts == 7 && after.failed && after.errors == 1);
    assert(after.process_max_us == 63125); /* A failed feed is still a timed attempt. */
    assert(after.deadline_misses == before.deadline_misses);
    assert(before.process_max_us == 16000 && !before.failed);
    s_fixture_now_us = 0;
    media_frontend_pairing_close(f.pairing);
}

static void monitor_clipping(void)
{
    for (unsigned count = 0; count <= 1; ++count) {
        frontend_t f;
        start(&f, false, 1, count != 0);
        assert(feed(&f, 1000, 0) == XIAOTAI_OK);
        assert(media_frontend_pairing_feed_next(f.pairing, 0) == XIAOTAI_OK);
        for (unsigned i = 0; i < FEED; ++i) {
            assert(f.packed[2U * i] == (int16_t)(100U + i));
            assert(f.packed[2U * i + 1U] == (i ? (int16_t)(-900 - (int)i) : INT16_MIN));
        }
        fetch(&f, 750, 0, false);
        fetch(&f, 1000, 0, true);
        xiaotai_processor_stats_t stats;
        media_frontend_pairing_stats(f.pairing, &stats);
        assert(stats.clipped_mic == count && stats.clipped_reference == 1 && stats.feeds == 1);
        media_frontend_pairing_close(f.pairing);
    }
}

enum { CORES3_RATE = 16000, CORES3_CHANNELS = 4, LEGACY_RING = 6, LEGACY_FETCH_MAX = 512 };
typedef struct {
    media_frontend_pairing_t *pairing;
    int16_t pcm[LEGACY_RING][LEGACY_FETCH_MAX];
    unsigned feed, fetch, head, count, deferred;
    bool force, no_pcm;
    int forced;
} legacy_afe_t;

static xiaotai_err_t legacy_fetch(void *ctx, const int16_t **pcm, size_t *samples, uint32_t timeout_ms)
{
    assert(timeout_ms == 200U);
    legacy_afe_t *afe = ctx;
    if (!afe->count) return XIAOTAI_ERR_TIMEOUT;
    *pcm = afe->pcm[afe->head];
    *samples = afe->fetch;
    afe->head = (afe->head + 1U) % LEGACY_RING;
    --afe->count;
    return XIAOTAI_OK;
}

static int legacy_afe_feed(void *ctx, const int16_t *mr)
{
    legacy_afe_t *afe = ctx;
    if (afe->force) return afe->forced;
    const unsigned chunks = afe->feed / afe->fetch;
    if (afe->count + chunks > 3U * chunks) return 0;
    if (!afe->no_pcm) {
        for (unsigned c = 0; c < chunks; ++c) {
            int16_t *frame = afe->pcm[(afe->head + afe->count++) % LEGACY_RING];
            for (size_t i = 0; i < afe->fetch; ++i) {
                const size_t at = c * afe->fetch + i;
                assert(mr[2U * at + 1U] == -1);
                frame[i] = mr[2U * at];
            }
        }
    }
    /* Vendor PCM becomes fetchable before the feed owner publishes stamps. */
    for (unsigned i = 0; i < afe->deferred; ++i) {
        xiaotai_processor_frame_t frame;
        assert(media_frontend_pairing_fetch(afe->pairing, &frame, 200U) == XIAOTAI_ERR_TIMEOUT);
    }
    return (int)(afe->feed * 2U * sizeof(*mr));
}

static const xiaotai_audio_desc_t cores3_input = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = CORES3_RATE,
    .channels = CORES3_CHANNELS,
    .roles = {XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE, XIAOTAI_CHANNEL_MONITOR, XIAOTAI_CHANNEL_UNUSED}};

static void legacy_open(legacy_afe_t *afe, unsigned feed, unsigned fetch)
{
    memset(afe, 0, sizeof(*afe));
    afe->feed = feed;
    afe->fetch = fetch;
    const media_frontend_pairing_config_t cfg = {.input = &cores3_input, .microphones = 1,
        .feed_samples = (uint16_t)feed, .fetch_samples = (uint16_t)fetch, .input_slots = (uint8_t)(4096U / feed),
        .ring_frames = 17, .stamp_wait_ms = 20, .strict_feed = false};
    const media_frontend_ops_t port = {.feed = legacy_afe_feed, .fetch = legacy_fetch, .ctx = afe};
    assert(media_frontend_pairing_open(&cfg, &port, &afe->pairing) == XIAOTAI_OK);
}

static int64_t legacy_end_us(const legacy_afe_t *afe, unsigned block)
{
    return INT64_C(1000000) + (int64_t)block * afe->feed * 1000000U / CORES3_RATE;
}

static xiaotai_err_t legacy_submit(legacy_afe_t *afe, unsigned block, uint32_t lost)
{
    int16_t input[1024U * CORES3_CHANNELS];
    assert(afe->feed <= 1024U);
    for (unsigned at = 0; at < afe->feed; ++at) {
        input[CORES3_CHANNELS * at] = (int16_t)(block * 4U + at / afe->fetch);
        input[CORES3_CHANNELS * at + 1U] = -1;
        input[CORES3_CHANNELS * at + 2U] = 200;
        input[CORES3_CHANNELS * at + 3U] = 300;
    }
    assert(media_frontend_pairing_feed(afe->pairing, input, afe->feed * CORES3_CHANNELS * sizeof(*input),
                                       legacy_end_us(afe, block), lost) == XIAOTAI_OK);
    return media_frontend_pairing_feed_next(afe->pairing, 0);
}

static void legacy_read_block(legacy_afe_t *afe, unsigned block, uint32_t lost)
{
    const unsigned chunks = afe->feed / afe->fetch;
    for (unsigned c = 0; c < chunks; ++c) {
        xiaotai_processor_frame_t frame;
        assert(media_frontend_pairing_fetch(afe->pairing, &frame, 200U) == XIAOTAI_OK);
        const int64_t elapsed = (int64_t)(chunks - c - 1U) * afe->fetch * 1000000U / CORES3_RATE;
        assert(frame.captured_us == legacy_end_us(afe, block) - elapsed);
        assert(frame.lost_samples == (c ? 0U : lost));
        assert(frame.samples == afe->fetch && frame.size == afe->fetch * sizeof(int16_t) &&
               frame.encoding == XIAOTAI_CODEC_PCM16);
        for (unsigned i = 0; i < frame.samples; ++i)
            assert(((const int16_t *)frame.data)[i] == (int16_t)(block * 4U + c));
    }
}

static void legacy_continuity(unsigned feed, unsigned fetch)
{
    legacy_afe_t afe;
    legacy_open(&afe, feed, fetch);
    const unsigned chunks = feed / fetch;
    for (unsigned b = 1; b <= 3; ++b) assert(legacy_submit(&afe, b, 0) == XIAOTAI_OK);
    for (unsigned b = 1; b <= 3; ++b) legacy_read_block(&afe, b, 0);
    xiaotai_processor_frame_t frame;
    assert(media_frontend_pairing_fetch(afe.pairing, &frame, 200U) == XIAOTAI_ERR_TIMEOUT);
    for (unsigned b = 4; b <= 6; ++b) assert(legacy_submit(&afe, b, 0) == XIAOTAI_OK);
    assert(legacy_submit(&afe, 7, 100) == XIAOTAI_ERR_NO_MEM);
    assert(legacy_submit(&afe, 8, 0) == XIAOTAI_ERR_NO_MEM);
    for (unsigned b = 4; b <= 6; ++b) legacy_read_block(&afe, b, 0);
    assert(legacy_submit(&afe, 9, 7) == XIAOTAI_OK);
    legacy_read_block(&afe, 9, 7U + 100U + 2U * feed);

    afe.deferred = chunks;
    assert(legacy_submit(&afe, 10, 3) == XIAOTAI_OK);
    afe.deferred = 0;
    assert(legacy_submit(&afe, 11, 0) == XIAOTAI_OK);
    legacy_read_block(&afe, 11, 3U + feed);
    xiaotai_processor_stats_t stats;
    media_frontend_pairing_stats(afe.pairing, &stats);
    assert(stats.feeds == 9 && stats.frames == 8U * chunks && stats.fetch_timeouts == chunks + 1U);
    assert(stats.input_full == 2 && stats.lost_samples == 110U + 3U * feed && !stats.failed && !stats.errors);
    afe.force = true;
    assert(legacy_submit(&afe, 14, 0) == XIAOTAI_ERR_NO_MEM);
    afe.forced = -3;
    assert(legacy_submit(&afe, 15, 0) == XIAOTAI_ERR_INVALID_STATE);
    assert(media_frontend_pairing_fetch(afe.pairing, &frame, 200U) == XIAOTAI_ERR_INVALID_STATE);
    media_frontend_pairing_stats(afe.pairing, &stats);
    assert(stats.failed && stats.errors == 1U);
    media_frontend_pairing_close(afe.pairing);

    legacy_open(&afe, feed, fetch);
    /* Only the stub violates the contract: it claims a successful write
     * without exposing PCM to its consumer, letting the real stamp queue fill. */
    afe.no_pcm = true;
    xiaotai_err_t result = XIAOTAI_OK;
    for (unsigned b = 1; b <= 64 && result == XIAOTAI_OK; ++b) result = legacy_submit(&afe, b, 0);
    assert(result == XIAOTAI_ERR_INVALID_STATE);
    assert(media_frontend_pairing_fetch(afe.pairing, &frame, 200U) == XIAOTAI_ERR_INVALID_STATE);
    media_frontend_pairing_stats(afe.pairing, &stats);
    assert(stats.failed && stats.errors == 1U);
    media_frontend_pairing_close(afe.pairing);
    printf("PASS: CoreS3 %u/%u PCM+stamp pairing, full/late loss, negative feed and stamp overflow\n", feed, fetch);
}

int main(void)
{
    recoverable(1);
    recoverable(2);
    strict_legacy();
    lifetime_counters();
    monitor_clipping();
    legacy_continuity(512, 512);
    legacy_continuity(1024, 512);
    legacy_continuity(512, 256);
    return 0;
}
