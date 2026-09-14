#include "starter_preroll.h"
#include <string.h>

void starter_preroll_init(starter_preroll_t *q, int16_t *pcm, size_t capacity)
{
    *q = (starter_preroll_t){.pcm = pcm, .capacity = capacity};
}

void starter_preroll_clear(starter_preroll_t *q)
{
    q->head = q->count = 0;
    q->token = q->generation = 0;
    q->end_ms = q->expires_ms = q->timestamp_ms = 0;
    q->failed = false;
}

void starter_preroll_append(starter_preroll_t *q, const int16_t *pcm,
                           size_t samples, int64_t end_ms)
{
    if (q->failed || q->capacity == 0 || samples == 0) return;
    /* An input discontinuity invalidates a pending utterance, never splices it. */
    int64_t duration_ms = (int64_t)samples / 8;
    if (q->end_ms != 0 &&
        (end_ms <= q->end_ms || end_ms - q->end_ms > duration_ms + 100)) {
        if (q->token != 0) { q->failed = true; return; }
        starter_preroll_clear(q);
    }
    if (q->token != 0 &&
        ((q->generation == 0 && end_ms > q->expires_ms) ||
         samples > q->capacity - q->count)) {
        q->failed = true;
        return;
    }
    size_t limit = q->token ? q->capacity :
                   (q->capacity < PREROLL_HISTORY_SAMPLES ? q->capacity : PREROLL_HISTORY_SAMPLES);
    for (size_t i = 0; i < samples; ++i) {
        if (q->count == limit) {
            q->head = (q->head + 1U) % q->capacity;
            --q->count;
        }
        q->pcm[(q->head + q->count) % q->capacity] = pcm[i];
        ++q->count;
    }
    q->end_ms = end_ms;
}

uint32_t starter_preroll_prepare(starter_preroll_t *q, int64_t wake_ms, int64_t now_ms)
{
    if (q->token != 0 || q->failed || q->count == 0 || wake_ms <= 0 ||
        now_ms < wake_ms || now_ms - wake_ms > 1000 ||
        now_ms - q->end_ms > 150) return 0;
    /* Retain 800 ms before the detection frame, including a suffix that may
     * have begun before MultiNet reported DETECTED. Idle history is never sent
     * by a manual AI start. */
    int64_t keep_ms = q->end_ms - wake_ms + 800;
    if (keep_ms <= 0) return 0;
    size_t keep = (size_t)keep_ms * 8U;
    if (keep < q->count) {
        size_t drop = q->count - keep;
        q->head = (q->head + drop) % q->capacity;
        q->count = keep;
    }
    if (++q->next_token == 0) ++q->next_token;
    q->token = q->next_token;
    q->timestamp_ms = q->end_ms - (int64_t)q->count / 8;
    q->expires_ms = now_ms + PREROLL_TIMEOUT_MS;
    return q->token;
}

bool starter_preroll_valid(const starter_preroll_t *q, uint32_t token, int64_t now_ms)
{
    return token != 0 && q->token == token && !q->failed &&
           (q->generation != 0 || now_ms <= q->expires_ms);
}

bool starter_preroll_bind(starter_preroll_t *q, uint32_t token,
                         uint32_t generation, int64_t now_ms)
{
    if (generation == 0 || !starter_preroll_valid(q, token, now_ms) ||
        q->generation != 0) return false;
    q->generation = generation;
    return true;
}

bool starter_preroll_peek(starter_preroll_t *q, uint32_t generation,
                         int16_t *pcm, size_t samples, uint32_t *timestamp_ms)
{
    if (generation == 0 || q->generation != generation || q->failed ||
        samples > q->count) return false;
    for (size_t i = 0; i < samples; ++i) pcm[i] = q->pcm[(q->head + i) % q->capacity];
    *timestamp_ms = (uint32_t)q->timestamp_ms;
    return true;
}

void starter_preroll_consume(starter_preroll_t *q, size_t samples)
{
    if (samples > q->count) return;
    q->head = (q->head + samples) % q->capacity;
    q->count -= samples;
    q->timestamp_ms += (int64_t)samples / 8;
}

bool starter_preroll_finish_replay(starter_preroll_t *q, uint32_t generation,
                                  int16_t *tail, size_t packet_samples,
                                  size_t *tail_samples, uint32_t *timestamp_ms)
{
    if (!tail || !tail_samples || !timestamp_ms || !q->token || !generation ||
        q->generation != generation || q->failed || q->count >= packet_samples) return false;
    *tail_samples = q->count;
    *timestamp_ms = (uint32_t)q->timestamp_ms;
    for (size_t i = 0; i < q->count; ++i) tail[i] = q->pcm[(q->head + i) % q->capacity];
    starter_preroll_clear(q);
    return true;
}

bool starter_preroll_failure_requires_abort(const starter_preroll_t *q,
                                            uint32_t token, int64_t now_ms)
{
    return token != 0 && q->generation == 0 && !starter_preroll_valid(q, token, now_ms);
}
