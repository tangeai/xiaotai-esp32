#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XMINI_MEMTRACE_SLOTS 192
#define XMINI_MEMTRACE_FAILURES 8
typedef struct {
    uintptr_t address, caller;
    size_t size;
} xmini_memtrace_record_t;
typedef struct {
    uintptr_t caller;
    size_t size, free_bytes, largest_block;
    uint32_t sequence;
    uint8_t operation; /* 0 malloc, 1 calloc, 2 realloc */
} xmini_memtrace_failure_t;
typedef struct {
    bool compiled, running;
    uint32_t sequence, allocations, frees, failures, lost;
    size_t live_bytes, peak_bytes;
    unsigned live_slots, peak_slots;
} xmini_memtrace_status_t;

/* Diagnostic build: -D XMINI_MEMTRACE=ON. Call before the first SDK allocation
 * for full SDK accounting, or interpret a later start as a delta window only.
 * Start never resets live ownership; Stop still tracks frees of known blocks. */
bool xmini_memtrace_start(void);
void xmini_memtrace_stop(void);
void xmini_memtrace_status(xmini_memtrace_status_t *status);
bool xmini_memtrace_record(unsigned slot, xmini_memtrace_record_t *record);
bool xmini_memtrace_failure(unsigned slot, xmini_memtrace_failure_t *failure);
