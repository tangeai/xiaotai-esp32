#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define WAKE_WINDOW_SAMPLES 16192U
#define WAKE_HOP_SAMPLES 2560U
#define WAKE_ALIGNMENT_SAMPLES 160U
#define WAKE_RING_SAMPLES (WAKE_WINDOW_SAMPLES + WAKE_ALIGNMENT_SAMPLES - 1U)
typedef enum { WAKE_ACCEPT, WAKE_REJECT_EPOCH, WAKE_REJECT_AGE, WAKE_REJECT_COOLDOWN } wake_verdict_t;
static inline wake_verdict_t wake_result_check(bool enabled, bool gap, uint32_t epoch,
                                               uint32_t current_epoch, int64_t captured_ms,
                                               int64_t now_ms, int64_t cooldown_ms)
{
    if (!enabled || gap || epoch != current_epoch) return WAKE_REJECT_EPOCH;
    if (now_ms < captured_ms || now_ms - captured_ms > 1000) return WAKE_REJECT_AGE;
    if (now_ms < cooldown_ms) return WAKE_REJECT_COOLDOWN;
    return WAKE_ACCEPT;
}
/* Caller serializes access. Latest continuous window, never a frame backlog. */
typedef struct {
    int16_t *pcm;
    size_t write, count, fresh;
    int64_t captured_ms;
    uint32_t epoch;
    uint64_t total_samples, snapshot_end;
} wake_window_t;
static inline void wake_window_reset(wake_window_t *w)
{
    w->write = w->count = w->fresh = 0;
    w->captured_ms = 0;
    w->total_samples = w->snapshot_end = 0;
    ++w->epoch;
}
static inline void wake_window_append(wake_window_t *w, const int16_t *pcm, size_t n, int64_t at)
{
    if (w->captured_ms && (at <= w->captured_ms || at - w->captured_ms > 100))
        wake_window_reset(w);
    for (size_t i = 0; i < n; ++i) {
        w->pcm[w->write++] = pcm[i];
        if (w->write == WAKE_RING_SAMPLES) w->write = 0;
    }
    w->count = w->count + n < WAKE_RING_SAMPLES ? w->count + n : WAKE_RING_SAMPLES;
    w->fresh = w->fresh + n < WAKE_WINDOW_SAMPLES ? w->fresh + n : WAKE_WINDOW_SAMPLES;
    w->captured_ms = at;
    w->total_samples += n;
}
static inline bool wake_window_snapshot(wake_window_t *w, int16_t *out)
{
    if (w->count < WAKE_WINDOW_SAMPLES || w->fresh < WAKE_HOP_SAMPLES) return false;
    /* Keep the 10 ms FFT grid across DMA blocks; retain at most 159 newer
     * samples rather than re-framing the entire overlap. */
    size_t pending = (size_t)((w->total_samples - WAKE_WINDOW_SAMPLES) % WAKE_ALIGNMENT_SAMPLES);
    if (w->count < WAKE_WINDOW_SAMPLES + pending) return false;
    size_t start = (w->write + WAKE_RING_SAMPLES - pending - WAKE_WINDOW_SAMPLES) % WAKE_RING_SAMPLES;
    size_t tail = WAKE_RING_SAMPLES - start;
    if (tail > WAKE_WINDOW_SAMPLES) tail = WAKE_WINDOW_SAMPLES;
    memcpy(out, w->pcm + start, tail * sizeof(*out));
    memcpy(out + tail, w->pcm, (WAKE_WINDOW_SAMPLES - tail) * sizeof(*out));
    w->fresh = 0;
    w->snapshot_end = w->total_samples - pending;
    return true;
}
