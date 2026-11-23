// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_binding_prompt_cancel.py
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#include "clip_playout.h"
#include "resample.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

#define WAIT_MS 3000U

static int16_t played[256];
static size_t length, writes, flush_queries;
static atomic_uint completions;
static uint32_t ids[5];
static xiaotai_err_t results[5];
static bool write_fails;
static xiaotai_signal_t seen, ready, release;

static xiaotai_err_t write_pcm(void *ctx, void *data, size_t bytes)
{
    assert(!xiaotai_on_owner() && ctx == played && bytes % 4U == 0 && bytes <= 16U && bytes);
    ++writes;
    if (write_fails) return XIAOTAI_ERR_TIMEOUT;
    int16_t *pcm = data;
    for (size_t i = 0; i < bytes / 4U; ++i) {
        assert(pcm[2 * i] == pcm[2 * i + 1] && length < sizeof(played) / sizeof(played[0]));
        played[length++] = pcm[2 * i];
    }
    memset(data, 0x9a, bytes);
    return XIAOTAI_OK;
}

static size_t flush_frames(void *ctx)
{
    assert(ctx == played && !xiaotai_on_owner());
    ++flush_queries;
    return 5U;
}

static void done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == played);
    const unsigned at = atomic_load(&completions);
    assert(at < sizeof(ids) / sizeof(ids[0]));
    ids[at] = id;
    results[at] = result;
    atomic_store(&completions, at + 1U);
    xiaotai_signal_give(&seen);
}

static void give(void *ctx) { xiaotai_signal_give(ctx); }
static void hold(void *ctx)
{
    xiaotai_signal_give(ctx);
    assert(xiaotai_signal_take(&release, WAIT_MS));
}

static uint32_t submit(media_clip_player_t *p, const xiaotai_media_play_t *play)
{
    media_clip_request_t request;
    uint32_t id = 0;
    assert(media_clip_request_prepare(p, play, &request) == XIAOTAI_OK);
    assert(media_clip_player_submit(p, &request, &id) == XIAOTAI_OK && id);
    return id;
}

static void finish(media_clip_player_t *p, uint32_t id)
{
    for (size_t i = 0; i < 128 && media_clip_player_id(p); ++i) {
        assert(media_clip_player_id(p) == id);
        assert(media_clip_player_step(p, id, XIAOTAI_OK) == 0);
    }
    assert(!media_clip_player_id(p));
}

static void wait_for(unsigned count, uint32_t id, xiaotai_err_t result)
{
    while (atomic_load(&completions) < count) assert(xiaotai_signal_take(&seen, WAIT_MS));
    assert(ids[count - 1U] == id && results[count - 1U] == result);
}

int main(void)
{
    assert(xiaotai_signal_init(&ready) == XIAOTAI_OK && xiaotai_signal_init(&seen) == XIAOTAI_OK &&
           xiaotai_signal_init(&release) == XIAOTAI_OK);
    const xiaotai_framework_config_t framework = {.queue_depth = 2};
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);

    static const uint8_t first[] = {100, 0, 200, 0, 44, 1};
    static const uint8_t second[] = {144, 1, 244, 1};
    static const xiaotai_sound_t named = {.name = "test.named", .encoding = XIAOTAI_CODEC_PCM16,
        .rate_hz = 8000, .channels = 1, .data = first, .size = sizeof(first)};
    static const xiaotai_sound_pack_t pack = {.name = "test", .sounds = &named, .count = 1};
    static const xiaotai_sound_t borrowed = {.encoding = XIAOTAI_CODEC_PCM16,
        .rate_hz = 8000, .channels = 1, .data = second, .size = sizeof(second)};
    assert(xiaotai_sounds_register(&pack) == XIAOTAI_OK);
    static const xiaotai_audio_output_t out = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 8000, .channels = 2},
        .info = {.max_write_frames = 4, .flags = XIAOTAI_AUDIO_OUTPUT_PACED},
        .write = write_pcm, .flush_frames = flush_frames, .self = played,
    };
    media_clip_player_t *player;
    assert(media_clip_player_create(&out, &player) == XIAOTAI_OK);
    assert(media_clip_player_decoder_stack_bytes(player) == 0);
    xiaotai_media_play_t request = {.clips = {{.name = "test.named"}, {.sound = &borrowed}},
        .clip_count = 2, .done = done, .ctx = played, .repeat = 1, .gap_ms = 1};
    const uint32_t multi = submit(player, &request);
    finish(player, multi);
    wait_for(1, multi, XIAOTAI_OK);
    static const int16_t expected[] = {100, 200, 300, 400, 500, 0, 0, 0, 0, 0, 0, 0, 0,
                                       100, 200, 300, 400, 500, 0, 0, 0, 0, 0};
    assert(length == sizeof(expected) / sizeof(expected[0]) && flush_queries == 1);
    assert(memcmp(played, expected, sizeof(expected)) == 0);

    request.clip_count = 1;
    request.repeat = 0;
    request.priority = XIAOTAI_PLAY_URGENT;
    const uint32_t urgent = submit(player, &request);
    media_clip_request_t lower;
    request.priority = XIAOTAI_PLAY_NORMAL;
    assert(media_clip_request_prepare(player, &request, &lower) == XIAOTAI_OK);
    assert(media_clip_player_submit(player, &lower, NULL) == XIAOTAI_ERR_BUSY);
    request.priority = XIAOTAI_PLAY_CUE;
    assert(media_clip_request_prepare(player, &request, &lower) == XIAOTAI_OK);
    assert(media_clip_player_submit(player, &lower, NULL) == XIAOTAI_ERR_BUSY);
    const size_t before = writes;
    assert(media_clip_player_step(player, urgent, XIAOTAI_OK) == 0 && writes == before + 1U);

    assert(xiaotai_run(hold, &ready) == XIAOTAI_OK && xiaotai_signal_take(&ready, WAIT_MS));
    assert(xiaotai_run(give, &ready) == XIAOTAI_OK && xiaotai_run(give, &ready) == XIAOTAI_OK);
    media_clip_player_cancel(player, urgent, XIAOTAI_ERR_INVALID_STATE);
    assert(!media_clip_player_id(player));
    const uint32_t replacement = submit(player, &request);
    assert(replacement != urgent && media_clip_player_step(player, urgent, XIAOTAI_OK) != 0);
    assert(writes == before + 1U && atomic_load(&completions) == 1 && media_clip_player_needs_work(player));
    xiaotai_signal_give(&release);
    assert(xiaotai_signal_take(&ready, WAIT_MS));
    finish(player, replacement);
    for (unsigned tries = 0; atomic_load(&completions) < 3 && tries < 10; ++tries) {
        (void)media_clip_player_step(player, 0, XIAOTAI_OK);
        (void)xiaotai_signal_take(&seen, 20);
    }
    wait_for(2, urgent, XIAOTAI_ERR_INVALID_STATE);
    wait_for(3, replacement, XIAOTAI_OK);
    assert(writes == before + 3U && flush_queries == 3);

    write_fails = true;
    const uint32_t failed = submit(player, &request);
    finish(player, failed);
    wait_for(4, failed, XIAOTAI_FAIL);
    write_fails = false;

    uint8_t samples[320];
    for (unsigned i = 0; i < 160; ++i) {
        const uint16_t value = (uint16_t)(i * 997U);
        samples[2 * i] = (uint8_t)value;
        samples[2 * i + 1U] = (uint8_t)(value >> 8);
    }
    const xiaotai_sound_t wide = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 1,
                                  .data = samples, .size = sizeof(samples)};
    request.clips[0] = (xiaotai_media_clip_t){.sound = &wide};
    size_t baseline = length;
    const uint32_t wide_id = submit(player, &request);
    finish(player, wide_id);
    wait_for(5, wide_id, XIAOTAI_OK);
    int16_t reference[160];
    xiaotai_media_rate_t rate;
    size_t consumed;
    assert(xiaotai_media_rate_open(&rate, 16000, 1, 8000) == XIAOTAI_OK);
    size_t n = xiaotai_media_rate_process(&rate, samples, 160, &consumed, reference, 160);
    assert(consumed == 160);
    n += xiaotai_media_rate_finish(&rate, reference + n, 160U - n);
    assert(length == baseline + n + 5 && memcmp(played + baseline, reference, n * sizeof(reference[0])) == 0);
    for (size_t i = 0; i < 5; ++i) assert(played[baseline + n + i] == 0);

    media_clip_player_destroy(player);
    xiaotai_framework_reset();
    xiaotai_sounds_reset();
    xiaotai_signal_deinit(&release);
    xiaotai_signal_deinit(&seen);
    xiaotai_signal_deinit(&ready);
    return 0;
}
