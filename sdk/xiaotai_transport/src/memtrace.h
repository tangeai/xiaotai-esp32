// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

typedef struct { uintptr_t address, caller; size_t bytes; } xt_memtrace_record_t;
typedef struct {
    size_t bytes, free_bytes, largest;
    uintptr_t caller;
    uint32_t sequence;
    unsigned operation;
} xt_memtrace_failure_t;
typedef struct {
    size_t live, peak;
    uint32_t sequence, allocations, frees, failures, lost, slots, peak_slots;
    bool running;
} xt_memtrace_status_t;

/* Start/stop is owned by the engine lifecycle. Already tracked allocations
 * still retire while stopped. An in-flight realloc owns its original slot. */
xiaotai_err_t xiaotai_transport_memtrace_init(void);
void xt_memtrace_stop(void);
void xt_memtrace_alloc(uintptr_t address, size_t bytes, uintptr_t caller);
void xt_memtrace_free(uintptr_t address);
unsigned xt_memtrace_realloc_begin(uintptr_t address);
void xt_memtrace_realloc_end(unsigned ticket, uintptr_t address, size_t bytes, uintptr_t caller);
bool xt_memtrace_running(void);
void xt_memtrace_failed(size_t bytes, uintptr_t caller, unsigned operation, size_t free_bytes, size_t largest);
void xt_memtrace_status(xt_memtrace_status_t *out);
bool xt_memtrace_record(unsigned slot, xt_memtrace_record_t *out);
bool xt_memtrace_failure(unsigned slot, xt_memtrace_failure_t *out);
