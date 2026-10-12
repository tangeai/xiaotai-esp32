#ifndef PRODUCT_SLOW_LOG_H
#define PRODUCT_SLOW_LOG_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

/* One LVGL writer, one idle ringtone-worker reader. Every field is atomic;
 * an overlapping read is deferred, never retried while holding a UI lock. */
typedef struct {
    atomic_uint sequence, count, max_us, at_ms, elapsed_us, detail[7];
} product_slow_log_t;

typedef struct {
    uint32_t count, max_us, at_ms, elapsed_us, detail[7];
} product_slow_log_snapshot_t;

static inline void product_slow_log_record(product_slow_log_t *log,
                                           uint32_t at_ms, uint32_t elapsed_us,
                                           const uint32_t detail[7])
{
    atomic_fetch_add(&log->sequence, 1U);
    atomic_fetch_add(&log->count, 1U);
    if (elapsed_us > atomic_load(&log->max_us))
        atomic_store(&log->max_us, elapsed_us);
    atomic_store(&log->at_ms, at_ms);
    atomic_store(&log->elapsed_us, elapsed_us);
    for (unsigned i = 0; i < 7U; ++i)
        atomic_store(&log->detail[i], detail[i]);
    atomic_fetch_add(&log->sequence, 1U);
}

static inline bool product_slow_log_read(const product_slow_log_t *log,
                                         product_slow_log_snapshot_t *out)
{
    unsigned sequence = atomic_load(&log->sequence);
    if (sequence & 1U) return false;
    out->count = atomic_load(&log->count);
    out->max_us = atomic_load(&log->max_us);
    out->at_ms = atomic_load(&log->at_ms);
    out->elapsed_us = atomic_load(&log->elapsed_us);
    for (unsigned i = 0; i < 7U; ++i)
        out->detail[i] = atomic_load(&log->detail[i]);
    return sequence == atomic_load(&log->sequence);
}

#endif
