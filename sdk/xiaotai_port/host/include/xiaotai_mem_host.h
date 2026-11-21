// SPDX-License-Identifier: MIT
#pragma once
/* Test controls for the host memory port: allocation counts and failure
 * injection, so a test can walk every allocation of a start-up path, fail
 * each in turn and check that the code under test cleans up.
 *
 * Only the port's allocators (xiaotai_mem_alloc, _calloc, _aligned) count and
 * fail; the C library's own allocations do not. Every thread's allocations
 * count, so a test fails the n-th allocation of a path only while nothing
 * else allocates through the port (start the path from a quiet state). */
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The n-th port allocation from now fails (1: the next one), once, as a real
 * failure would: NULL is returned and xiaotai_mem_failure() records it.
 * 0 cancels a pending injection. */
void xiaotai_mem_host_fail_nth(uint32_t n);
/* True while an injected failure has not happened yet. */
int xiaotai_mem_host_fail_pending(void);
/* Port allocations attempted since the last clear, failed ones included. */
uint32_t xiaotai_mem_host_allocations(void);
/* Port allocations since the last clear minus non-NULL xiaotai_mem_free calls
 * since then: 0 after a path that frees what it allocated (negative when it
 * frees memory allocated before the clear). */
int64_t xiaotai_mem_host_outstanding(void);
/* Zeroes the counts, cancels an injection and forgets the failure record. */
void xiaotai_mem_host_clear(void);

#define XIAOTAI_MEM_HOST_LEDGER_CAPACITY 1024U

typedef struct {
    size_t bytes;
    uintptr_t site;  /* Caller return PC; resolve site-1 in the host executable. */
    xiaotai_mem_kind_t kind;
    bool task;
    bool fixture;    /* Allocation for a host simulation with no target counterpart. */
    char name[XIAOTAI_MEM_NAME_MAX]; /* Task name; empty for a buffer. */
} xiaotai_mem_host_ledger_entry_t;

/* Enable live requested-byte accounting before starting the product. Takes
 * effect once per process; earlier allocations and the native pthread runtime
 * are outside the ledger. Independent of xiaotai_mem_host_clear(). */
void xiaotai_mem_host_ledger_start(void);
/* When a host adapter delegates to the real allocator, preserve the original
 * caller in the live ledger. This does not change the allocation or its count. */
void xiaotai_mem_host_buffer_site(const void *buffer, uintptr_t site);
/* One consistent snapshot. Overflow is sticky; a too-small output buffer
 * also returns false. On either error the output must not be used as proof. */
bool xiaotai_mem_host_ledger_snapshot(xiaotai_mem_host_ledger_entry_t *entries,
                                    size_t capacity, size_t *count);

#ifdef __cplusplus
}
#endif
