// SPDX-License-Identifier: MIT
/* The capture-to-engine hand-off (src/wake_queue.h): refusals, capacity of 32
 * blocks, first-in first-out with timestamps and generations, a published
 * block unchanged until released, index wrap-around, the size the engine
 * allocates, and one producer and one consumer on two threads never seeing a
 * torn or reordered block. */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wake_queue.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

static wake_queue_t s_queue;

static int test_basics(void)
{
    int16_t pcm[WAKE_QUEUE_BLOCK_SAMPLES + 1];
    for (size_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); ++i) pcm[i] = (int16_t)i;
    wake_queue_init(&s_queue);
    CHECK(wake_queue_peek(&s_queue) == NULL);
    /* Refused: nothing, too much. */
    CHECK(!wake_queue_push(&s_queue, NULL, 10, 1, 1));
    CHECK(!wake_queue_push(&s_queue, pcm, 0, 1, 1));
    CHECK(!wake_queue_push(&s_queue, pcm, WAKE_QUEUE_BLOCK_SAMPLES + 1, 1, 1));
    CHECK(wake_queue_peek(&s_queue) == NULL);
    /* 32 blocks fit, the 33rd is refused and changes nothing. */
    for (uint32_t i = 0; i < WAKE_QUEUE_BLOCKS; ++i) {
        CHECK(wake_queue_push(&s_queue, pcm + i, i == 0 ? WAKE_QUEUE_BLOCK_SAMPLES : 1U + i, 1000 + i, 7U + i));
    }
    CHECK(!wake_queue_push(&s_queue, pcm, 1, 5000, 99));
    for (uint32_t i = 0; i < WAKE_QUEUE_BLOCKS; ++i) {
        const wake_block_t *block = wake_queue_peek(&s_queue);
        CHECK(block != NULL);
        CHECK(block->captured_us == 1000 + (int64_t)i && block->generation == 7U + i);
        CHECK(block->samples == (i == 0 ? WAKE_QUEUE_BLOCK_SAMPLES : 1U + i));
        CHECK(block->pcm[0] == (int16_t)i && block->pcm[block->samples - 1] == (int16_t)(i + block->samples - 1));
        /* Peeking again returns the same block until it is released. */
        CHECK(wake_queue_peek(&s_queue) == block);
        if (i == 0) {
            /* A full ring refuses the producer while the block is held. */
            CHECK(!wake_queue_push(&s_queue, pcm, 1, 6000, 100));
            CHECK(block->captured_us == 1000 && block->pcm[5] == 5);
        }
        wake_queue_release(&s_queue);
        if (i == 0) CHECK(wake_queue_push(&s_queue, pcm, 3, 7000, 200));
    }
    const wake_block_t *last = wake_queue_peek(&s_queue);
    CHECK(last && last->captured_us == 7000 && last->generation == 200U && last->samples == 3);
    wake_queue_release(&s_queue);
    CHECK(wake_queue_peek(&s_queue) == NULL);
    return 0;
}

/* The indices are free-running unsigned counters: they wrap at 2^32 and the
 * ring keeps working. */
static int test_wrap(void)
{
    int16_t pcm[4] = {1, 2, 3, 4};
    wake_queue_init(&s_queue);
    atomic_store(&s_queue.write, UINT32_MAX - 3U);
    atomic_store(&s_queue.read, UINT32_MAX - 3U);
    for (int round = 0; round < 3; ++round) {
        for (uint32_t i = 0; i < WAKE_QUEUE_BLOCKS; ++i) CHECK(wake_queue_push(&s_queue, pcm, 4, 10 + i, i));
        CHECK(!wake_queue_push(&s_queue, pcm, 4, 99, 99));
        for (uint32_t i = 0; i < WAKE_QUEUE_BLOCKS; ++i) {
            const wake_block_t *block = wake_queue_peek(&s_queue);
            CHECK(block && block->captured_us == 10 + (int64_t)i && block->generation == i);
            wake_queue_release(&s_queue);
        }
        CHECK(wake_queue_peek(&s_queue) == NULL);
    }
    return 0;
}

/* What the engine allocates: 32 blocks of header and 1024 samples, and the
 * two indices; about one second of 32 ms frames. */
static int test_size(void)
{
    CHECK(sizeof(wake_block_t) == 16U + 2U * WAKE_QUEUE_BLOCK_SAMPLES);
    CHECK(sizeof(wake_queue_t) == 8U + WAKE_QUEUE_BLOCKS * sizeof(wake_block_t));
    CHECK(sizeof(wake_queue_t) == 66056U);
    return 0;
}

#define STRESS_FRAMES 200000U

static atomic_uint s_refused;

/* Frame k carries k in its timestamp and generation and k + i in sample i. */
static void *producer(void *arg)
{
    (void)arg;
    int16_t pcm[WAKE_QUEUE_BLOCK_SAMPLES];
    for (uint32_t k = 0; k < STRESS_FRAMES; ++k) {
        size_t n = 1U + k % WAKE_QUEUE_BLOCK_SAMPLES;
        for (size_t i = 0; i < n; ++i) pcm[i] = (int16_t)(k + i);
        while (!wake_queue_push(&s_queue, pcm, n, (int64_t)k + 1, k)) {
            atomic_fetch_add(&s_refused, 1U);
            sched_yield();
        }
    }
    return NULL;
}

static int test_threads(void)
{
    wake_queue_init(&s_queue);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, producer, NULL) == 0);
    uint32_t expected = 0;
    while (expected < STRESS_FRAMES) {
        const wake_block_t *block = wake_queue_peek(&s_queue);
        if (!block) {
            sched_yield();
            continue;
        }
        CHECK(block->generation == expected && block->captured_us == (int64_t)expected + 1);
        CHECK(block->samples == 1U + expected % WAKE_QUEUE_BLOCK_SAMPLES);
        for (size_t i = 0; i < block->samples; ++i) CHECK(block->pcm[i] == (int16_t)(expected + i));
        wake_queue_release(&s_queue);
        ++expected;
    }
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(wake_queue_peek(&s_queue) == NULL);
    printf("wake_queue: %u frames, producer refused %u times\n", STRESS_FRAMES, atomic_load(&s_refused));
    return 0;
}

int main(void)
{
    if (test_basics() || test_wrap() || test_size() || test_threads()) return 1;
    puts("wake_queue: ok");
    return 0;
}
