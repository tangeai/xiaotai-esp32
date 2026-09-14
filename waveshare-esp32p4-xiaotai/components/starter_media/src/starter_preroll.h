#ifndef STARTER_PREROLL_H
#define STARTER_PREROLL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure 8 kHz PCM queue. Caller serializes access; no SDK calls or allocation. */
#define PREROLL_RATE 8000U
#define PREROLL_HISTORY_SAMPLES 12000U
#define PREROLL_CAPACITY_SAMPLES (PREROLL_RATE * 20U)
#define PREROLL_TIMEOUT_MS 19000
typedef struct {
    int16_t *pcm;
    size_t capacity, head, count;
    uint32_t next_token, token, generation;
    int64_t end_ms, expires_ms, timestamp_ms;
    bool failed;
} starter_preroll_t;

void starter_preroll_init(starter_preroll_t *q, int16_t *pcm, size_t capacity);
void starter_preroll_clear(starter_preroll_t *q);
void starter_preroll_append(starter_preroll_t *q, const int16_t *pcm,
                           size_t samples, int64_t end_ms);
uint32_t starter_preroll_prepare(starter_preroll_t *q, int64_t wake_ms, int64_t now_ms);
bool starter_preroll_valid(const starter_preroll_t *q, uint32_t token, int64_t now_ms);
bool starter_preroll_bind(starter_preroll_t *q, uint32_t token,
                         uint32_t generation, int64_t now_ms);
bool starter_preroll_peek(starter_preroll_t *q, uint32_t generation,
                         int16_t *pcm, size_t samples, uint32_t *timestamp_ms);
void starter_preroll_consume(starter_preroll_t *q, size_t samples);
/* Transfer the last partial packet and release replay ownership atomically. */
bool starter_preroll_finish_replay(starter_preroll_t *q, uint32_t generation,
                                  int16_t *tail, size_t packet_samples,
                                  size_t *tail_samples, uint32_t *timestamp_ms);
/* Only setup is all-or-nothing. Active media discontinuities must recover. */
bool starter_preroll_failure_requires_abort(const starter_preroll_t *q,
                                            uint32_t token, int64_t now_ms);
#endif
