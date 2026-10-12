#ifndef STARTER_RX_TRACE_H
#define STARTER_RX_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define STARTER_RX_TRACE_CAPACITY 128U

typedef enum {
    STARTER_RX_TRACE_ACCEPTED,
    STARTER_RX_TRACE_NO_FREE_SLOT,
    STARTER_RX_TRACE_BAD_SLOT,
    STARTER_RX_TRACE_READY_FULL,
    STARTER_RX_TRACE_PLAYOUT_FULL,
} starter_rx_trace_result_t;

/* Queue/stage fields are independently sampled hints. The result records the
 * actual admission decision; timestamps are monotonic low 32 bits. */
typedef struct {
    uint32_t at_us, timestamp_ms, generation, epoch;
    uint32_t result, format, length, queues;
    uint32_t queued_ms, pending_samples, decoded, played;
    uint32_t sink_stage, sink_at_us, owner, buffering_count;
} starter_rx_trace_entry_t;
_Static_assert(sizeof(starter_rx_trace_entry_t) == 64U, "RX trace entry budget changed");

typedef struct {
    starter_rx_trace_entry_t entries[STARTER_RX_TRACE_CAPACITY];
    atomic_uint active, busy, frozen, skipped, missed_faults, max_us;
    unsigned next, count;
} starter_rx_trace_t;
_Static_assert(sizeof(starter_rx_trace_t) == 8224U, "RX trace PSRAM budget changed");

/* Observation never waits. Contention skips only a record, not media. Once a
 * failure is recorded, release/acquire freezes a chronological prehistory. */
static inline bool starter_rx_trace_record(starter_rx_trace_t *trace,
                                           const starter_rx_trace_entry_t *entry)
{
    if (!atomic_load_explicit(&trace->active, memory_order_relaxed)) return false;
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&trace->busy, &expected, 1U,
                                                memory_order_acquire, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&trace->skipped, 1U, memory_order_relaxed);
        if (entry->result != STARTER_RX_TRACE_ACCEPTED)
            atomic_fetch_add_explicit(&trace->missed_faults, 1U, memory_order_relaxed);
        return false;
    }
    bool written = atomic_load_explicit(&trace->active, memory_order_relaxed) != 0;
    if (written) {
        trace->entries[trace->next] = *entry;
        trace->next = (trace->next + 1U) % STARTER_RX_TRACE_CAPACITY;
        if (trace->count < STARTER_RX_TRACE_CAPACITY) ++trace->count;
        if (entry->result != STARTER_RX_TRACE_ACCEPTED) {
            atomic_store_explicit(&trace->active, 0U, memory_order_relaxed);
            atomic_store_explicit(&trace->frozen, 1U, memory_order_release);
        }
    }
    atomic_store_explicit(&trace->busy, 0U, memory_order_release);
    return written;
}

/* Caller owns the control channel; re-arm only with the media session stopped.
 * Payload storage need not be cleared: count bounds all access after reset. */
static inline bool starter_rx_trace_arm(starter_rx_trace_t *trace, bool enabled)
{
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&trace->busy, &expected, 1U,
                                                memory_order_acquire, memory_order_relaxed)) return false;
    atomic_store_explicit(&trace->active, 0U, memory_order_relaxed);
    if (enabled) {
        trace->next = trace->count = 0;
        atomic_store(&trace->skipped, 0U);
        atomic_store(&trace->missed_faults, 0U);
        atomic_store(&trace->max_us, 0U);
        atomic_store_explicit(&trace->frozen, 0U, memory_order_relaxed);
        atomic_store_explicit(&trace->active, 1U, memory_order_release);
    } else {
        atomic_store_explicit(&trace->frozen, 1U, memory_order_release);
    }
    atomic_store_explicit(&trace->busy, 0U, memory_order_release);
    return true;
}

static inline unsigned starter_rx_trace_count(const starter_rx_trace_t *trace)
{
    return atomic_load_explicit(&trace->frozen, memory_order_acquire) ? trace->count : 0U;
}

static inline bool starter_rx_trace_read(const starter_rx_trace_t *trace,
                                          unsigned index, starter_rx_trace_entry_t *out)
{
    unsigned count = starter_rx_trace_count(trace);
    if (!out || index >= count) return false;
    unsigned head = (trace->next + STARTER_RX_TRACE_CAPACITY - count) % STARTER_RX_TRACE_CAPACITY;
    *out = trace->entries[(head + index) % STARTER_RX_TRACE_CAPACITY];
    return true;
}

#endif
