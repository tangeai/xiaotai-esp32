#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#define WAKE_AUDIO_QUEUE_BLOCKS 32U
#define WAKE_AUDIO_BLOCK_SAMPLES 1024U

typedef struct {
    int64_t captured_ms;
    uint32_t epoch;
    size_t samples;
    int16_t pcm[WAKE_AUDIO_BLOCK_SAMPLES];
} wake_audio_block_t;

/* One capture producer, one inference consumer. A published slot is immutable
 * until the consumer releases it. Both PCM and indices live in allocated PSRAM.
 * No retry, mutex, allocation or model work is permitted on the producer. */
typedef struct {
    atomic_uint write;
    atomic_uint read;
    wake_audio_block_t blocks[WAKE_AUDIO_QUEUE_BLOCKS];
} wake_audio_queue_t;

static inline void wake_audio_queue_init(wake_audio_queue_t *q)
{
    atomic_init(&q->write, 0);
    atomic_init(&q->read, 0);
}

static inline bool wake_audio_queue_push(wake_audio_queue_t *q, const int16_t *pcm,
                                        size_t samples, int64_t at, uint32_t epoch)
{
    if (!pcm || !samples || samples > WAKE_AUDIO_BLOCK_SAMPLES) return false;
    unsigned write = atomic_load_explicit(&q->write, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&q->read, memory_order_acquire);
    if (write - read >= WAKE_AUDIO_QUEUE_BLOCKS) return false;
    wake_audio_block_t *b = &q->blocks[write % WAKE_AUDIO_QUEUE_BLOCKS];
    b->captured_ms = at;
    b->epoch = epoch;
    b->samples = samples;
    memcpy(b->pcm, pcm, samples * sizeof(*pcm));
    atomic_store_explicit(&q->write, write + 1U, memory_order_release);
    return true;
}

static inline const wake_audio_block_t *wake_audio_queue_peek(wake_audio_queue_t *q)
{
    unsigned read = atomic_load_explicit(&q->read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&q->write, memory_order_acquire);
    return read == write ? NULL : &q->blocks[read % WAKE_AUDIO_QUEUE_BLOCKS];
}

static inline void wake_audio_queue_release(wake_audio_queue_t *q)
{
    unsigned read = atomic_load_explicit(&q->read, memory_order_relaxed);
    atomic_store_explicit(&q->read, read + 1U, memory_order_release);
}
