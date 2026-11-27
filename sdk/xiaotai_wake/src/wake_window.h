// SPDX-License-Identifier: MIT
#pragma once
/* The latest contiguous window of audio and the checks a score passes before
 * it becomes a detection. Pure C; the caller serializes access.
 *
 * The window keeps only the newest run of contiguous audio, never a backlog of
 * windows: when scoring is slower than the stride, the steps in between are
 * skipped. A snapshot ends on the model's grid (grid_samples) counted from the
 * start of the contiguous run, so consecutive windows of one run share their
 * feature frames and the model may reuse them; up to grid_samples - 1 newer
 * samples wait for the next snapshot. grid_samples 1 is the unaligned window
 * (the newest window_samples, the ring exactly one window long). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Policy constants: the same for every model and board. */
#define WAKE_FRAME_GAP_MAX_US 100000      /* a later frame further away than this starts a new run */
#define WAKE_RESULT_AGE_MAX_US 1000000    /* a window older than this when scored is not a detection */

typedef struct {
    int16_t *pcm;                 /* ring of capacity samples */
    uint32_t capacity;            /* window + grid - 1 */
    uint32_t window, grid, stride;
    size_t write, count, fresh;   /* next write position, samples held, samples since the last snapshot */
    int64_t captured_us;          /* capture time of the newest sample's frame end; 0: empty since reset */
    uint32_t generation;          /* changes at every reset, never 0: the run the window holds */
    uint64_t total_samples;       /* samples appended since the reset */
    uint64_t snapshot_end;        /* total_samples position where the last snapshot ended */
} wake_window_t;

/* Ring capacity for a model: the window plus the samples that may wait for
 * the next grid position. */
static inline uint32_t wake_window_capacity(uint32_t window, uint32_t grid)
{
    return window + grid - 1U;
}

static inline void wake_window_reset(wake_window_t *w)
{
    w->write = w->count = w->fresh = 0;
    w->captured_us = 0;
    w->total_samples = w->snapshot_end = 0;
    if (++w->generation == 0) w->generation = 1;
}

/* pcm holds wake_window_capacity(window, grid) samples. */
static inline void wake_window_init(wake_window_t *w, int16_t *pcm, uint32_t window, uint32_t grid, uint32_t stride)
{
    memset(w, 0, sizeof(*w));
    w->pcm = pcm;
    w->window = window;
    w->grid = grid;
    w->stride = stride;
    w->capacity = wake_window_capacity(window, grid);
    wake_window_reset(w);
}

/* Appends one frame whose end was captured at captured_us. A frame that does
 * not move time forward, or comes more than WAKE_FRAME_GAP_MAX_US after the
 * previous one, starts a new run first (the generation changes). */
static inline void wake_window_append(wake_window_t *w, const int16_t *pcm, size_t n, int64_t captured_us)
{
    if (w->captured_us && (captured_us <= w->captured_us || captured_us - w->captured_us > WAKE_FRAME_GAP_MAX_US)) {
        wake_window_reset(w);
    }
    size_t done = 0;
    while (done < n) {
        size_t room = w->capacity - w->write;
        size_t chunk = n - done < room ? n - done : room;
        memcpy(w->pcm + w->write, pcm + done, chunk * sizeof(*pcm));
        w->write += chunk;
        if (w->write == w->capacity) w->write = 0;
        done += chunk;
    }
    w->count = w->count + n < w->capacity ? w->count + n : w->capacity;
    w->fresh = w->fresh + n < w->window ? w->fresh + n : w->window;
    w->captured_us = captured_us;
    w->total_samples += n;
}

/* Copies the newest window that ends on the grid into out (window samples)
 * when a full window is held and at least stride samples arrived since the
 * last snapshot. */
static inline bool wake_window_snapshot(wake_window_t *w, int16_t *out)
{
    if (w->count < w->window || w->fresh < w->stride) return false;
    size_t pending = (size_t)((w->total_samples - w->window) % w->grid);
    if (w->count < w->window + pending) return false;
    size_t start = (w->write + w->capacity - pending - w->window) % w->capacity;
    size_t tail = w->capacity - start;
    if (tail > w->window) tail = w->window;
    memcpy(out, w->pcm + start, tail * sizeof(*out));
    memcpy(out + tail, w->pcm, (w->window - tail) * sizeof(*out));
    w->fresh = 0;
    w->snapshot_end = w->total_samples - pending;
    return true;
}

/* Capture time of the last snapshot's end: the newest frame's time less the
 * samples left after the grid position. */
static inline int64_t wake_window_snapshot_us(const wake_window_t *w, uint32_t rate_hz)
{
    return w->captured_us - (int64_t)((w->total_samples - w->snapshot_end) * 1000000U / rate_hz);
}

typedef enum {
    WAKE_ACCEPT = 0,
    WAKE_REJECT_STREAM,     /* not listening, a gap, or the input or the window changed meanwhile */
    WAKE_REJECT_AGE,        /* the window ends in the future or is older than the age limit */
    WAKE_REJECT_COOLDOWN,   /* inside the cooldown after a detection */
} wake_verdict_t;

/* The checks after a score reached the threshold, in this order. changed: a
 * gap was flagged or the input generation moved since the window was taken. */
static inline wake_verdict_t wake_verdict(bool listening, bool changed, uint32_t generation,
                                          uint32_t current_generation, int64_t captured_us, int64_t now_us,
                                          int64_t cooldown_until_us)
{
    if (!listening || changed || generation != current_generation) return WAKE_REJECT_STREAM;
    if (now_us < captured_us || now_us - captured_us > WAKE_RESULT_AGE_MAX_US) return WAKE_REJECT_AGE;
    if (now_us < cooldown_until_us) return WAKE_REJECT_COOLDOWN;
    return WAKE_ACCEPT;
}
