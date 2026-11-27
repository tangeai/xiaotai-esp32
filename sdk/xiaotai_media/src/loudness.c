// SPDX-License-Identifier: MIT
/* The loudness of the moment (xiaotai_media_loudness), left behind by the
 * audio tasks for whoever draws along with sound. The capture task already
 * measures every clean frame and stores that value. The playout task writes
 * ahead of what is heard, by as much as the driver holds: it files the level
 * of each block under the moment the block will be heard, in a small ring, and
 * a reader takes the entry whose moment has come. Plain stores, no lock, no
 * log, nothing allocated. */
#include "loudness.h"

#include <stdatomic.h>
#include <stdbool.h>
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"

enum {
    STALE_MS = 250,         /* a capture level older than this is no level */
    CELLS = 32,             /* blocks between writing and hearing: 320 ms of the shortest blocks in use (10 ms) */
    LATE_MS = 40,           /* a block that follows this late still follows: the level before it holds */
    /* The cells' 16-bit times come round after 65 s, so old cells must not lie about: nothing is read
     * this long after the last block, and a block after a pause first clears the ring. */
    QUIET_MS = 8000, PAUSE_MS = 1000,
};

static atomic_uint capture_level, capture_at;
/* level << 16 | the low 16 bits of the millisecond the block begins to be heard, in one word so that
 * the two never come apart. */
static atomic_uint cells[CELLS];
static atomic_uint cell_count, cell_at, block_ms;

static unsigned now_ms(int64_t now_us) { return (unsigned)(now_us / 1000); }
/* Not longer ago than that. A reader that was held up after it took the time may even be a moment early. */
static bool recent(unsigned now, unsigned at, int most)
{
    const int age = (int)(now - at);
    return age <= most && age > -PAUSE_MS;
}

void media_loudness_capture(uint32_t rms, int64_t now_us)
{
    atomic_store_explicit(&capture_level, rms > 32768U ? 32768U : rms, memory_order_relaxed);
    atomic_store_explicit(&capture_at, now_ms(now_us), memory_order_relaxed);
}

uint32_t media_loudness_peak(const int16_t *pcm, size_t frames)
{
    uint32_t peak = 0;
    for (size_t i = 0; i < frames; i += 4U) {
        const int32_t sample = pcm[i];
        const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        if (magnitude > peak) peak = magnitude;
    }
    return peak;
}

void media_loudness_playout(uint32_t peak, int64_t heard_us, int64_t duration_us, int64_t now_us)
{
    const unsigned n = atomic_load_explicit(&cell_count, memory_order_relaxed), now = now_ms(now_us);
    const unsigned level = peak > 32768U ? 32768U : peak;
    if (n && now - atomic_load_explicit(&cell_at, memory_order_relaxed) > PAUSE_MS) {
        const unsigned past = (now - QUIET_MS) & 0xFFFFU;
        for (unsigned i = 0; i < CELLS; ++i) atomic_store_explicit(&cells[i], past, memory_order_relaxed);
    }
    atomic_store_explicit(&cells[n % CELLS], level << 16 | (now_ms(heard_us) & 0xFFFFU), memory_order_relaxed);
    atomic_store_explicit(&block_ms, now_ms(duration_us), memory_order_relaxed);
    atomic_store_explicit(&cell_at, now, memory_order_relaxed);
    atomic_store_explicit(&cell_count, n + 1U, memory_order_release);
}

/* The level of the latest block that has begun to be heard at `now`; false when no block is filed for it (nothing
 * written lately, or not that far ahead). */
static bool heard(unsigned now, uint16_t *level)
{
    *level = 0;
    const unsigned written = atomic_load_explicit(&cell_count, memory_order_acquire);
    if (!written || !recent(now, atomic_load_explicit(&cell_at, memory_order_relaxed), QUIET_MS)) return false;
    const unsigned lasts = atomic_load_explicit(&block_ms, memory_order_relaxed) + LATE_MS;
    unsigned best = lasts;
    bool filed = false;
    for (unsigned i = 0; i < CELLS && i < written; ++i) {
        const unsigned cell = atomic_load_explicit(&cells[i], memory_order_relaxed);
        const int age = (int16_t)(uint16_t)(now - (cell & 0xFFFFU));
        if (age >= 0 && (unsigned)age < best) {
            best = (unsigned)age;
            *level = (uint16_t)(cell >> 16);
            filed = true;
        }
    }
    return filed;
}

bool media_loudness_playout_at(uint16_t *level, int64_t at_us) { return heard(now_ms(at_us), level); }

void media_loudness_read(xiaotai_media_loudness_t *out, int64_t now_us)
{
    const unsigned now = now_ms(now_us);
    out->capture = recent(now, atomic_load_explicit(&capture_at, memory_order_relaxed), STALE_MS)
                       ? (uint16_t)atomic_load_explicit(&capture_level, memory_order_relaxed) : 0;
    (void)heard(now, &out->playout);
}

void xiaotai_media_loudness(xiaotai_media_loudness_t *out)
{
    if (out) media_loudness_read(out, xiaotai_time_us());
}

bool xiaotai_media_loudness_ahead(xiaotai_media_loudness_t *out, uint32_t ahead_ms)
{
    if (!out) return false;
    const int64_t now = xiaotai_time_us();
    media_loudness_read(out, now);
    return media_loudness_playout_at(&out->playout, now + (int64_t)ahead_ms * 1000);
}
