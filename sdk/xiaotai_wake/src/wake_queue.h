// SPDX-License-Identifier: MIT
#pragma once
/* Hand-off of clean capture audio from the media engine's capture task (the
 * only producer, inside the tap callback) to the engine task (the only
 * consumer).
 *
 * A lock-free ring of WAKE_QUEUE_BLOCKS blocks. A published block is immutable
 * until the consumer releases it. The producer never waits, locks, allocates,
 * logs or retries: a full ring refuses the frame and the caller counts it.
 * The consumer never resets the indices while the producer may write; frames
 * that must not be scored any more are told apart by their input generation
 * instead (the caller drops blocks of an old generation).
 *
 * WAKE_QUEUE_BLOCKS blocks are about one second of audio at the frame sizes
 * the capture processors deliver (32 ms), so a scoring that takes a few
 * strides never costs audio. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WAKE_QUEUE_BLOCKS 32U
/* Largest frame a block holds: the largest clean frame a processor returns
 * (XIAOTAI_PROCESSOR_SAMPLES_MAX, checked where both headers meet). */
#define WAKE_QUEUE_BLOCK_SAMPLES 1024U

typedef struct {
    int64_t captured_us;   /* capture time of the frame's end */
    uint32_t generation;   /* input generation when the frame was queued */
    uint32_t samples;
    int16_t pcm[WAKE_QUEUE_BLOCK_SAMPLES];
} wake_block_t;

typedef struct {
    atomic_uint write;     /* written by the producer only */
    atomic_uint read;      /* written by the consumer only */
    wake_block_t blocks[WAKE_QUEUE_BLOCKS];
} wake_queue_t;

static inline void wake_queue_init(wake_queue_t *queue)
{
    atomic_init(&queue->write, 0U);
    atomic_init(&queue->read, 0U);
}

/* [rt, producer] False when the frame is not queued: no samples, more than a
 * block holds, or the ring is full. */
static inline bool wake_queue_push(wake_queue_t *queue, const int16_t *pcm, size_t samples, int64_t captured_us,
                                   uint32_t generation)
{
    if (!pcm || samples == 0 || samples > WAKE_QUEUE_BLOCK_SAMPLES) return false;
    unsigned write = atomic_load_explicit(&queue->write, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&queue->read, memory_order_acquire);
    if (write - read >= WAKE_QUEUE_BLOCKS) return false;
    wake_block_t *block = &queue->blocks[write % WAKE_QUEUE_BLOCKS];
    block->captured_us = captured_us;
    block->generation = generation;
    block->samples = (uint32_t)samples;
    memcpy(block->pcm, pcm, samples * sizeof(*pcm));
    atomic_store_explicit(&queue->write, write + 1U, memory_order_release);
    return true;
}

/* [consumer] The oldest published block, or NULL when the ring is empty. It
 * stays valid and unchanged until wake_queue_release. */
static inline const wake_block_t *wake_queue_peek(wake_queue_t *queue)
{
    unsigned read = atomic_load_explicit(&queue->read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&queue->write, memory_order_acquire);
    return read == write ? NULL : &queue->blocks[read % WAKE_QUEUE_BLOCKS];
}

/* [consumer] Gives the block returned by the last peek back to the producer. */
static inline void wake_queue_release(wake_queue_t *queue)
{
    unsigned read = atomic_load_explicit(&queue->read, memory_order_relaxed);
    atomic_store_explicit(&queue->read, read + 1U, memory_order_release);
}
