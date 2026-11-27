// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_wake_window
/* The latest-window rules (src/wake_window.h): snapshots on the model's grid
 * and their content, the stride between snapshots, the newest-window
 * semantics, the unaligned case (grid 1), the window-end timestamp,
 * continuity resets, and the four-step verdict after a hit.
 *
 * Ports tests/regression/test_voice_reliability.py test_wake_window with the
 * same frame sizes and schedule (512 samples every 32 ms), in microseconds. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wake_window.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#define WINDOW 16192U
#define GRID 160U
#define STRIDE 2560U
#define RATE 16000U

static int16_t s_ring[WINDOW + GRID - 1U], s_ring_flat[WINDOW];
static int16_t s_out[WINDOW], s_out_flat[WINDOW];

/* Sample n of the test signal. */
static int16_t sample(uint64_t n) { return (int16_t)(n % 30000U); }

static void fill(int16_t *pcm, size_t n, uint64_t first)
{
    for (size_t i = 0; i < n; ++i) pcm[i] = sample(first + i);
}

static int test_ported(void)
{
    wake_window_t w;
    int16_t pcm[512];
    CHECK(wake_window_capacity(WINDOW, GRID) == 16351U);
    wake_window_init(&w, s_ring, WINDOW, GRID, STRIDE);
    CHECK(w.generation == 1U && w.capacity == 16351U);
    CHECK(!wake_window_snapshot(&w, s_out));
    uint64_t total = 0;
    int64_t at = 1000000;
    for (int frame = 0; frame < 80; ++frame) {
        fill(pcm, 512, total);
        wake_window_append(&w, pcm, 512, at);
        total += 512;
        at += 32000;
        if (frame == 30) CHECK(!wake_window_snapshot(&w, s_out));
    }
    CHECK(wake_window_snapshot(&w, s_out));
    /* The window ends on the 10 ms grid; up to 159 newer samples wait. */
    uint64_t pending = (total - WINDOW) % GRID, end = total - pending;
    CHECK(pending == 128U && end == 40832U);
    CHECK(w.snapshot_end == end);
    for (size_t i = 0; i < WINDOW; ++i) CHECK(s_out[i] == sample(end - WINDOW + i));
    /* Window end time: the newest frame's end less the 128 waiting samples (8 ms). */
    CHECK(wake_window_snapshot_us(&w, RATE) == at - 32000 - 8000);
    CHECK(!wake_window_snapshot(&w, s_out));
    /* 2048 new samples are not a stride; the fifth frame (2560) is. */
    for (int i = 0; i < 4; ++i) {
        fill(pcm, 512, total);
        wake_window_append(&w, pcm, 512, at);
        total += 512;
        at += 32000;
    }
    CHECK(!wake_window_snapshot(&w, s_out));
    fill(pcm, 512, total);
    wake_window_append(&w, pcm, 512, at);
    total += 512;
    at += 32000;
    CHECK(wake_window_snapshot(&w, s_out));
    pending = (total - WINDOW) % GRID;
    CHECK(w.snapshot_end == total - pending);
    for (size_t i = 0; i < WINDOW; ++i) CHECK(s_out[i] == sample(total - pending - WINDOW + i));

    uint32_t generation = w.generation;
    wake_window_reset(&w);
    CHECK(w.generation != generation && w.generation != 0U && !wake_window_snapshot(&w, s_out));
    CHECK(w.count == 0 && w.total_samples == 0 && w.captured_us == 0);

    /* Continuity: a frame 200 ms later, or earlier, or at the same time,
     * starts over with that frame alone; exactly 100 ms later continues. */
    wake_window_append(&w, pcm, 512, 5000000);
    generation = w.generation;
    wake_window_append(&w, pcm, 512, 5200000);
    CHECK(w.generation != generation && w.count == 512 && w.total_samples == 512);
    generation = w.generation;
    wake_window_append(&w, pcm, 512, 5100000);
    CHECK(w.generation != generation && w.count == 512);
    generation = w.generation;
    wake_window_append(&w, pcm, 512, 5100000);
    CHECK(w.generation != generation && w.count == 512);
    generation = w.generation;
    wake_window_append(&w, pcm, 512, 5200000);
    CHECK(w.generation == generation && w.count == 1024);
    wake_window_append(&w, pcm, 512, 5300001);
    CHECK(w.generation != generation && w.count == 512);
    return 0;
}

static int test_verdict(void)
{
    /* listening, changed, generation, current, captured, now, cooldown end */
    CHECK(wake_verdict(true, false, 1, 2, 1000000, 1100000, 0) == WAKE_REJECT_STREAM);
    CHECK(wake_verdict(false, false, 1, 1, 1000000, 1100000, 0) == WAKE_REJECT_STREAM);
    CHECK(wake_verdict(true, true, 1, 1, 1000000, 1100000, 0) == WAKE_REJECT_STREAM);
    CHECK(wake_verdict(true, false, 1, 1, 1000000, 2000001, 0) == WAKE_REJECT_AGE);
    CHECK(wake_verdict(true, false, 1, 1, 1000000, 2000000, 0) == WAKE_ACCEPT);
    CHECK(wake_verdict(true, false, 1, 1, 1000000, 999999, 0) == WAKE_REJECT_AGE);
    CHECK(wake_verdict(true, false, 1, 1, 1000000, 1100000, 1100001) == WAKE_REJECT_COOLDOWN);
    CHECK(wake_verdict(true, false, 1, 1, 1000000, 1100000, 1100000) == WAKE_ACCEPT);
    /* The order: a stream problem wins over age, age over cooldown. */
    CHECK(wake_verdict(false, true, 1, 2, 1000000, 9000000, 9999999) == WAKE_REJECT_STREAM);
    CHECK(wake_verdict(true, false, 1, 1, 1000000, 9000000, 9999999) == WAKE_REJECT_AGE);
    return 0;
}

/* Grid 1 is the unaligned window: a ring of exactly one window, snapshots of
 * the newest window_samples once the ring is full, no waiting samples. */
static int test_unaligned(void)
{
    wake_window_t w;
    int16_t pcm[480];
    CHECK(wake_window_capacity(WINDOW, 1) == WINDOW);
    wake_window_init(&w, s_ring_flat, WINDOW, 1, STRIDE);
    uint64_t total = 0;
    int64_t at = 1000000;
    while (total + 480 < WINDOW) {
        fill(pcm, 480, total);
        wake_window_append(&w, pcm, 480, at);
        total += 480;
        at += 30000;
        CHECK(!wake_window_snapshot(&w, s_out_flat));
    }
    fill(pcm, 480, total);
    wake_window_append(&w, pcm, 480, at);
    total += 480;
    CHECK(wake_window_snapshot(&w, s_out_flat));
    CHECK(w.snapshot_end == total);
    CHECK(wake_window_snapshot_us(&w, RATE) == at);
    for (size_t i = 0; i < WINDOW; ++i) CHECK(s_out_flat[i] == sample(total - WINDOW + i));
    return 0;
}

/* At the same grid position the aligned and the unaligned window hold the
 * same audio: the aligned one is the unaligned one taken when the run had
 * reached that position. */
static int test_same_grid_position(void)
{
    wake_window_t aligned, flat;
    int16_t pcm[1024];
    wake_window_init(&aligned, s_ring, WINDOW, GRID, STRIDE);
    wake_window_init(&flat, s_ring_flat, WINDOW, 1, STRIDE);
    uint64_t total = 0;
    int64_t at = 1000000;
    /* Odd frame sizes, the largest a block holds included. */
    const size_t sizes[] = {1024, 1000, 333, 512, 1024, 7};
    size_t next = 0;
    while (total < 3U * WINDOW) {
        size_t n = sizes[next++ % (sizeof(sizes) / sizeof(sizes[0]))];
        fill(pcm, n, total);
        wake_window_append(&aligned, pcm, n, at);
        total += n;
        at += (int64_t)n * 1000000 / RATE + 1;
    }
    CHECK(wake_window_snapshot(&aligned, s_out));
    uint64_t end = aligned.snapshot_end;
    CHECK((end - WINDOW) % GRID == 0 && total - end < GRID);
    /* The same run fed to the unaligned window up to that position. */
    for (uint64_t done = 0; done < end;) {
        size_t n = end - done < 1024U ? (size_t)(end - done) : 1024U;
        fill(pcm, n, done);
        wake_window_append(&flat, pcm, n, 1000000 + (int64_t)done);
        done += n;
    }
    CHECK(wake_window_snapshot(&flat, s_out_flat));
    CHECK(flat.snapshot_end == end);
    CHECK(!memcmp(s_out, s_out_flat, sizeof(s_out)));
    return 0;
}

/* Newest-window semantics: when nobody took a snapshot for many strides,
 * the next one is the newest window, and only one is available. */
static int test_newest_only(void)
{
    wake_window_t w;
    int16_t pcm[1024];
    wake_window_init(&w, s_ring, WINDOW, GRID, STRIDE);
    uint64_t total = 0;
    int64_t at = 1000000;
    while (total < 10U * WINDOW) {
        fill(pcm, 1024, total);
        wake_window_append(&w, pcm, 1024, at);
        total += 1024;
        at += 64000;
    }
    CHECK(w.count == w.capacity && w.fresh == WINDOW);
    CHECK(wake_window_snapshot(&w, s_out));
    uint64_t pending = (total - WINDOW) % GRID;
    CHECK(w.snapshot_end == total - pending);
    for (size_t i = 0; i < WINDOW; ++i) CHECK(s_out[i] == sample(total - pending - WINDOW + i));
    CHECK(!wake_window_snapshot(&w, s_out));
    return 0;
}

int main(void)
{
    if (test_ported() || test_verdict() || test_unaligned() || test_same_grid_position() || test_newest_only()) {
        return 1;
    }
    puts("wake_window: ok");
    return 0;
}
