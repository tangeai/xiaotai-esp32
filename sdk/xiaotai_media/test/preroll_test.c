// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_preroll
// Replaces legacy: tests/regression/test_voice_reliability.py::test_preroll_handoff
#include <assert.h>
#include "preroll.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"

enum { RATE = 8000, BLOCK = 160, BLOCK_US = 20000 };
static uint32_t lose_lock = UINT32_MAX;

bool __real_xiaotai_mutex_lock(xiaotai_mutex_t *lock, uint32_t timeout_ms);
bool __wrap_xiaotai_mutex_lock(xiaotai_mutex_t *lock, uint32_t timeout_ms);

bool __wrap_xiaotai_mutex_lock(xiaotai_mutex_t *lock, uint32_t timeout_ms)
{
    if (lose_lock == timeout_ms) {
        lose_lock = UINT32_MAX;
        return false;
    }
    return __real_xiaotai_mutex_lock(lock, timeout_ms);
}

static int16_t sample(size_t index) { return (int16_t)(index % 31013U); }

static xiaotai_err_t append(media_preroll_t *history, size_t start, int64_t end_us, uint32_t lost)
{
    int16_t block[BLOCK];
    for (size_t n = 0; n < BLOCK; ++n) block[n] = sample(start + n);
    return media_preroll_append(history, block, BLOCK, end_us, lost);
}

static void note_loss(uint32_t token, void *ctx) { *(uint32_t *)ctx = token; }

static void check_startup_allocation(void)
{
    xiaotai_mem_host_clear();
    media_preroll_t *history = NULL;
    assert(media_preroll_create(0, 20000, &history) == XIAOTAI_ERR_INVALID_ARG && !history);
    assert(xiaotai_mem_host_allocations() == 0);
    for (uint32_t failure = 1; failure <= 2; ++failure) {
        xiaotai_mem_host_fail_nth(failure);
        assert(media_preroll_create(16000, 20000, &history) == XIAOTAI_ERR_NO_MEM && !history);
        assert(xiaotai_mem_host_outstanding() == 0);
    }
    assert(media_preroll_create(16000, 20000, &history) == XIAOTAI_OK);
    assert(media_preroll_capacity_ms(history) == 20000);
    assert(xiaotai_mem_host_outstanding() == 2);
    media_preroll_destroy(history);
    assert(xiaotai_mem_host_outstanding() == 0);
}

static void check_hold_and_wrap(void)
{
    media_preroll_t *history;
    assert(media_preroll_create(RATE, 4205, &history) == XIAOTAI_OK);
    assert(media_preroll_capacity_ms(history) == 4205);

    int64_t end = 2000000;
    size_t source_sample = 0;
    xiaotai_media_preroll_t request = {.wake_us = end, .keep_before_ms = 800, .ttl_ms = 19000};
    uint32_t token = 42;
    assert(media_preroll_hold(NULL, &request, end + 1, &token) == XIAOTAI_ERR_NOT_SUPPORTED && token == 0);
    assert(media_preroll_capacity_ms(NULL) == 0);
    assert(media_preroll_hold(history, &request, end + 1, &token) == XIAOTAI_ERR_INVALID_STATE && token == 0);

    for (unsigned block = 0; block < 90U; ++block) {
        end += BLOCK_US;
        assert(append(history, source_sample, end, 0) == XIAOTAI_OK);
        source_sample += BLOCK;
    }
    /* Match the captured timeline rather than pretending wall time is capture time. */
    request.wake_us = end - 1000001;
    assert(media_preroll_hold(history, &request, end, &token) == XIAOTAI_ERR_INVALID_STATE);
    request.wake_us = end + 1;
    assert(media_preroll_hold(history, &request, end, &token) == XIAOTAI_ERR_INVALID_STATE);
    assert(media_preroll_hold(history, &request, end + 2, &token) == XIAOTAI_ERR_INVALID_STATE);
    request.wake_us = end - 400000;
    assert(media_preroll_hold(history, &request, end - 1, &token) == XIAOTAI_ERR_INVALID_STATE);
    lose_lock = 20;
    assert(media_preroll_hold(history, &request, end, &token) == XIAOTAI_ERR_TIMEOUT && token == 0);
    assert(lose_lock == UINT32_MAX);
    assert(media_preroll_hold(history, &request, end + BLOCK_US, &token) == XIAOTAI_OK && token);
    const uint32_t allocations = xiaotai_mem_host_allocations();
    uint32_t another = 10;
    assert(media_preroll_hold(history, &request, end + BLOCK_US, &another) == XIAOTAI_ERR_INVALID_STATE && !another);

    /* Three seconds before bind, even though idle history is limited to 1.5 s. */
    for (unsigned block = 0; block < 150U; ++block) {
        end += BLOCK_US;
        assert(append(history, source_sample, end, 0) == XIAOTAI_OK);
        source_sample += BLOCK;
    }
    assert(media_preroll_bind(history, token + 1U, 9, end) == XIAOTAI_ERR_INVALID_STATE);
    assert(media_preroll_bind(history, token, 9, end) == XIAOTAI_OK);
    assert(media_preroll_bind(history, token, 9, end) == XIAOTAI_ERR_INVALID_STATE);
    media_preroll_release(history, token); /* A bound token belongs to replay. */

    int16_t actual[BLOCK], tail[17];
    for (size_t n = 0; n < 17; ++n) tail[n] = sample(source_sample + n);
    assert(media_preroll_append(history, tail, 17, end + 2125, 0) == XIAOTAI_OK);
    size_t written = 3;
    int64_t stamp = 0;
    assert(media_preroll_finish(history, 9, actual, BLOCK, &written, &stamp) == XIAOTAI_ERR_INVALID_STATE);
    assert(written == 0);
    assert(media_preroll_read(history, 8, actual, BLOCK, &stamp) == XIAOTAI_ERR_INVALID_STATE);
    lose_lock = 2;
    assert(media_preroll_read(history, 9, actual, BLOCK, &stamp) == XIAOTAI_ERR_TIMEOUT);
    for (unsigned block = 0; block < 210U; ++block) {
        assert(media_preroll_read(history, 9, actual, BLOCK, &stamp) == XIAOTAI_OK);
        assert(stamp == 2600000 + (int64_t)block * BLOCK_US);
        for (size_t n = 0; n < BLOCK; ++n) assert(actual[n] == sample((30U + block) * BLOCK + n));
    }
    assert(media_preroll_finish(history, 9, actual, BLOCK, &written, &stamp) == XIAOTAI_OK);
    assert(written == 17 && stamp == end);
    for (size_t n = 0; n < written; ++n) assert(actual[n] == tail[n]);
    media_preroll_loss_t event = {0};
    assert(!media_preroll_take_loss(history, end + 2125, &event));
    assert(media_preroll_read(history, 9, actual, BLOCK, &stamp) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_mem_host_allocations() == allocations);
    media_preroll_destroy(history);
}

static void check_loss_and_release(void)
{
    media_preroll_t *history;
    assert(media_preroll_create(RATE, 40, &history) == XIAOTAI_OK);
    uint32_t notified = 0, token = 0;
    xiaotai_media_preroll_t request = {
        .wake_us = 2000000, .keep_before_ms = 20, .ttl_ms = 30, .lost = note_loss, .ctx = &notified,
    };
    media_preroll_loss_t event = {0};
    assert(append(history, 0, request.wake_us, 0) == XIAOTAI_OK);
    assert(media_preroll_hold(history, &request, request.wake_us, &token) == XIAOTAI_OK);
    assert(!media_preroll_take_loss(history, request.wake_us + 30000, &event));
    assert(media_preroll_take_loss(history, request.wake_us + 30001, &event));
    assert(event.token == token && !event.generation && event.lost == note_loss && event.ctx == &notified);
    event.lost(event.token, event.ctx);
    assert(notified == token && !media_preroll_take_loss(history, request.wake_us + 30002, &event));

    request.wake_us = 3000000;
    assert(append(history, 0, request.wake_us, 0) == XIAOTAI_OK);
    assert(media_preroll_hold(history, &request, request.wake_us, &token) == XIAOTAI_OK);
    lose_lock = 0;
    media_preroll_release(history, token);
    media_preroll_release(history, token + 1U);
    assert(media_preroll_bind(history, token, 7, request.wake_us) == XIAOTAI_ERR_INVALID_STATE);
    assert(!media_preroll_take_loss(history, request.wake_us, &event));

    request.wake_us = 4000000;
    request.ttl_ms = 19000;
    assert(append(history, 0, request.wake_us, 0) == XIAOTAI_OK);
    assert(media_preroll_hold(history, &request, request.wake_us, &token) == XIAOTAI_OK);
    assert(append(history, 160, request.wake_us + BLOCK_US, 0) == XIAOTAI_OK);
    assert(append(history, 320, request.wake_us + 2 * BLOCK_US, 0) == XIAOTAI_OK);
    assert(media_preroll_hold(history, &request, request.wake_us + 2 * BLOCK_US, &(uint32_t){0}) ==
           XIAOTAI_ERR_INVALID_STATE); /* Borrowed notification cannot be overwritten. */
    assert(media_preroll_take_loss(history, request.wake_us + 2 * BLOCK_US, &event) && event.token == token);
    assert(!media_preroll_take_loss(history, request.wake_us + 2 * BLOCK_US, &event));

    request.wake_us += 2 * BLOCK_US;
    assert(media_preroll_hold(history, &request, request.wake_us, &token) == XIAOTAI_OK);
    assert(media_preroll_bind(history, token, 8, request.wake_us) == XIAOTAI_OK);
    assert(append(history, 480, request.wake_us + BLOCK_US, 1) == XIAOTAI_OK);
    assert(media_preroll_take_loss(history, request.wake_us + BLOCK_US, &event));
    assert(event.token == token && event.generation == 8); /* Caller uses the session event, not lost(). */

    request.wake_us += BLOCK_US;
    assert(media_preroll_hold(history, &request, request.wake_us, &token) == XIAOTAI_OK);
    lose_lock = 2;
    assert(append(history, 640, request.wake_us + BLOCK_US, 0) == XIAOTAI_ERR_TIMEOUT);
    assert(media_preroll_take_loss(history, request.wake_us + BLOCK_US, &event) && event.token == token);
    media_preroll_destroy(history);
}

typedef struct { bool accept; size_t first; int64_t time_us; } replay_t;

static bool offer_packet(const int16_t *pcm, size_t samples, int64_t time_us, void *ctx)
{
    const replay_t *expect = ctx;
    assert(samples == BLOCK && time_us == expect->time_us);
    for (size_t n = 0; n < samples; ++n) assert(pcm[n] == sample(expect->first + n));
    return expect->accept;
}

static void check_armed_close_and_rejected_offer(void)
{
    media_preroll_t *history;
    assert(media_preroll_create(RATE, 120, &history) == XIAOTAI_OK);
    const int64_t wake = 4000000;
    uint32_t token = 0;
    const xiaotai_media_preroll_t request = {.wake_us = wake, .keep_before_ms = 40, .ttl_ms = 1000};
    assert(append(history, 0, wake - BLOCK_US, 0) == XIAOTAI_OK);
    assert(append(history, BLOCK, wake, 0) == XIAOTAI_OK);
    assert(media_preroll_hold(history, &request, wake, &token) == XIAOTAI_OK);
    assert(media_preroll_validate(history, token, wake) == XIAOTAI_OK);
    media_preroll_close_binding(history, 4U);
    assert(media_preroll_token(history) == token);
    assert(media_preroll_bind(history, token, 5U, wake + BLOCK_US) == XIAOTAI_OK);
    media_preroll_close_binding(history, 4U); /* A stale generation cannot revoke this binding. */
    assert(media_preroll_token(history) == token);

    int16_t block[BLOCK];
    replay_t expect = {.time_us = wake - 2 * BLOCK_US};
    assert(media_preroll_replay(history, 5U, block, BLOCK, offer_packet, &expect) == XIAOTAI_ERR_NO_MEM);
    xiaotai_media_status_t status = {0};
    media_preroll_status(history, &status);
    assert(status.preroll_token == token && status.preroll_bound && status.preroll_ms == 40U);
    expect.accept = true;
    assert(media_preroll_replay(history, 5U, block, BLOCK, offer_packet, &expect) == XIAOTAI_OK);
    expect = (replay_t){.accept = true, .first = BLOCK, .time_us = wake - BLOCK_US};
    assert(media_preroll_replay(history, 5U, block, BLOCK, offer_packet, &expect) == XIAOTAI_OK);
    assert(media_preroll_replay(history, 5U, block, BLOCK, offer_packet, &expect) == XIAOTAI_ERR_INVALID_STATE);

    lose_lock = 0;
    media_preroll_discard(history, true);
    assert(!media_preroll_token(history));
    bool pending;
    assert(media_preroll_poll_loss(history, wake + BLOCK_US, &pending) == XIAOTAI_OK && pending);
    media_preroll_loss_t loss;
    assert(media_preroll_take_loss(history, wake + BLOCK_US, &loss) && loss.generation == 5U && loss.token == token);
    assert(media_preroll_poll_loss(history, wake + BLOCK_US, &pending) == XIAOTAI_OK && !pending);
    xiaotai_media_stats_t stats = {0};
    media_preroll_stats(history, &stats);
    assert(stats.preroll_tokens == 1U && stats.preroll_lost == 1U);
    media_preroll_destroy(history);
}

static void check_microphone_epoch(void)
{
    media_preroll_t *history;
    assert(media_preroll_create(RATE, 40, &history) == XIAOTAI_OK);
    int16_t pcm[BLOCK] = {0};
    const int64_t end = 3000000;
    assert(media_preroll_append_granted(history, pcm, BLOCK, end, 0, 0) == XIAOTAI_OK);
    media_preroll_mic_transition(history, 1, true);
    assert(media_preroll_append_granted(history, pcm, BLOCK, end + BLOCK_US, 0, 1) == XIAOTAI_ERR_INVALID_STATE);
    media_preroll_mic_transition(history, 2, false);
    assert(media_preroll_append_granted(history, pcm, BLOCK, end + BLOCK_US, 0, 0) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_media_status_t status = {0};
    media_preroll_status(history, &status);
    assert(status.preroll_ms == 0);
    assert(media_preroll_append_granted(history, pcm, BLOCK, end + BLOCK_US, 0, 2) == XIAOTAI_OK);
    media_preroll_status(history, &status);
    assert(status.preroll_ms == 20);
    media_preroll_destroy(history);
}

static void check_s3_history_window(void)
{
    enum { SAMPLES = 320, CAPTURE_RATE = 16000, WAKE_BLOCK = 100, CAPTURE_BLOCKS = 200 };
    media_preroll_t *history;
    assert(media_preroll_create(CAPTURE_RATE, 20000, &history) == XIAOTAI_OK);
    uint32_t token = 0;
    int16_t pcm[SAMPLES];
    int64_t end = 0;
    for (unsigned block = 0; block < CAPTURE_BLOCKS; ++block) {
        for (size_t n = 0; n < SAMPLES; ++n) pcm[n] = sample(block * SAMPLES + n);
        end += BLOCK_US;
        assert(media_preroll_append(history, pcm, SAMPLES, end, 0) == XIAOTAI_OK);
        if (block + 1U != WAKE_BLOCK) continue;
        xiaotai_media_status_t status = {0};
        media_preroll_status(history, &status);
        assert(status.preroll_ms == 1500U);
        const xiaotai_media_preroll_t hold = {.wake_us = end, .keep_before_ms = 800, .ttl_ms = 19000};
        assert(media_preroll_hold(history, &hold, end, &token) == XIAOTAI_OK && token);
        media_preroll_status(history, &status);
        assert(status.preroll_ms == 800U);
    }
    assert(media_preroll_bind(history, token, 7, end) == XIAOTAI_OK);
    /* 800 ms before the 2 s wake point: first sample 19,200, then no gap. */
    int64_t stamp = 0;
    for (unsigned block = 60; block < CAPTURE_BLOCKS; ++block) {
        assert(media_preroll_read(history, 7, pcm, SAMPLES, &stamp) == XIAOTAI_OK);
        assert(stamp == (int64_t)block * BLOCK_US);
        for (size_t n = 0; n < SAMPLES; ++n) assert(pcm[n] == sample(block * SAMPLES + n));
    }
    assert(media_preroll_read(history, 7, pcm, SAMPLES, &stamp) == XIAOTAI_ERR_INVALID_STATE);
    size_t tail = 1;
    assert(media_preroll_finish(history, 7, pcm, SAMPLES, &tail, &stamp) == XIAOTAI_OK);
    assert(tail == 0 && stamp == end && !media_preroll_token(history));

    xiaotai_media_preroll_t hold = {.keep_before_ms = 800, .ttl_ms = 19000};
    hold.wake_us = end += BLOCK_US;
    assert(media_preroll_append(history, pcm, SAMPLES, end, 0) == XIAOTAI_OK);
    uint32_t next = 0;
    assert(media_preroll_hold(history, &hold, end, &next) == XIAOTAI_OK && next != token);
    assert(media_preroll_validate(history, next, end + 19000000) == XIAOTAI_OK);
    assert(media_preroll_bind(history, next, 9, end + 19001000) == XIAOTAI_ERR_INVALID_STATE);
    media_preroll_loss_t loss;
    end += 19001000;
    assert(media_preroll_take_loss(history, end, &loss) && loss.token == next);

    hold.wake_us = end += BLOCK_US;
    assert(media_preroll_append(history, pcm, SAMPLES, end, 0) == XIAOTAI_OK);
    assert(media_preroll_hold(history, &hold, end, &token) == XIAOTAI_OK && token != next);
    end += 220000; /* A real timestamp gap invalidates the pending wake. */
    assert(media_preroll_append(history, pcm, SAMPLES, end, 0) == XIAOTAI_OK);
    assert(media_preroll_take_loss(history, end, &loss) && loss.token == token);
    media_preroll_destroy(history);
}

static void check_s3_handoff(void)
{
    enum { CHUNK = 512, PACKET = 320, TAIL = CHUNK - PACKET };
    media_preroll_t *history;
    assert(media_preroll_create(16000, 20000, &history) == XIAOTAI_OK);
    int16_t pcm[CHUNK], out[PACKET];
    for (size_t i = 0; i < CHUNK; ++i) pcm[i] = (int16_t)i;
    xiaotai_media_preroll_t hold = {.wake_us = 1000000, .keep_before_ms = 800, .ttl_ms = 19000};
    uint32_t token = 0;
    assert(media_preroll_append(history, pcm, CHUNK, hold.wake_us, 0) == XIAOTAI_OK);
    assert(media_preroll_hold(history, &hold, hold.wake_us, &token) == XIAOTAI_OK && token);
    assert(media_preroll_bind(history, token, 1, hold.wake_us) == XIAOTAI_OK);

    size_t count = 99;
    int64_t stamp = 0;
    assert(media_preroll_finish(history, 1, out, PACKET, &count, &stamp) == XIAOTAI_ERR_INVALID_STATE);
    assert(count == 0 && media_preroll_token(history) == token);
    assert(media_preroll_read(history, 1, out, PACKET, &stamp) == XIAOTAI_OK && stamp == 968000);
    for (size_t i = 0; i < PACKET; ++i) assert(out[i] == pcm[i]);
    assert(media_preroll_finish(history, 2, out, PACKET, &count, &stamp) == XIAOTAI_ERR_INVALID_STATE);
    assert(count == 0 && media_preroll_token(history) == token);
    assert(media_preroll_finish(history, 1, out, PACKET, &count, &stamp) == XIAOTAI_OK);
    assert(count == TAIL && stamp == 988000 && !media_preroll_token(history));
    for (size_t i = 0; i < TAIL; ++i) assert(out[i] == pcm[i + PACKET]);
    assert(media_preroll_read(history, 1, out, PACKET, &stamp) == XIAOTAI_ERR_INVALID_STATE);

    for (unsigned phase = 0; phase < 2; ++phase) {
        const uint32_t generation = phase ? 3 : 0;
        hold.wake_us = (int64_t)(phase + 2U) * 1000000;
        assert(media_preroll_append(history, pcm, CHUNK, hold.wake_us, 0) == XIAOTAI_OK);
        assert(media_preroll_hold(history, &hold, hold.wake_us, &token) == XIAOTAI_OK && token);
        if (generation) assert(media_preroll_bind(history, token, generation, hold.wake_us) == XIAOTAI_OK);
        const int64_t after_gap = hold.wake_us + 200000;
        assert(media_preroll_append(history, pcm, CHUNK, after_gap, 0) == XIAOTAI_OK);
        media_preroll_loss_t lost;
        assert(media_preroll_take_loss(history, after_gap, &lost));
        assert(lost.token == token && lost.generation == generation && !media_preroll_token(history));
        assert(!media_preroll_take_loss(history, after_gap, &lost));
    }
    media_preroll_destroy(history);
}

int main(void)
{
    check_startup_allocation();
    check_hold_and_wrap();
    check_loss_and_release();
    check_armed_close_and_rejected_offer();
    check_microphone_epoch();
    check_s3_history_window();
    check_s3_handoff();
    assert(xiaotai_mem_host_outstanding() == 0);
    return 0;
}
