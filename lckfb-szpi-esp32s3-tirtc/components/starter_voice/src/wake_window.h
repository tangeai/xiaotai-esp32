#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define WAKE_WINDOW_SAMPLES 16192U
#define WAKE_HOP_SAMPLES 2560U
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
} wake_window_t;
static inline void wake_window_reset(wake_window_t *w)
{
    w->write = w->count = w->fresh = 0;
    w->captured_ms = 0;
    ++w->epoch;
}
static inline void wake_window_append(wake_window_t *w, const int16_t *pcm, size_t n, int64_t at)
{
    if (w->captured_ms && (at <= w->captured_ms || at - w->captured_ms > 100))
        wake_window_reset(w);
    for (size_t i = 0; i < n; ++i) {
        w->pcm[w->write++] = pcm[i];
        if (w->write == WAKE_WINDOW_SAMPLES) w->write = 0;
    }
    w->count = w->count + n < WAKE_WINDOW_SAMPLES ? w->count + n : WAKE_WINDOW_SAMPLES;
    w->fresh = w->fresh + n < WAKE_WINDOW_SAMPLES ? w->fresh + n : WAKE_WINDOW_SAMPLES;
    w->captured_ms = at;
}
static inline bool wake_window_snapshot(wake_window_t *w, int16_t *out)
{
    if (w->count != WAKE_WINDOW_SAMPLES || w->fresh < WAKE_HOP_SAMPLES) return false;
    size_t tail = WAKE_WINDOW_SAMPLES - w->write;
    memcpy(out, w->pcm + w->write, tail * sizeof(*out));
    memcpy(out + tail, w->pcm, w->write * sizeof(*out));
    w->fresh = 0;
    return true;
}
