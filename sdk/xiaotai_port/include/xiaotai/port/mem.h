// SPDX-License-Identifier: MIT
#pragma once
/* Memory by purpose. SDK code says what a buffer is for; the port decides where
 * it lives on this chip (PSRAM when the board has it, otherwise internal RAM).
 *
 * Callers, with the tags defined once in xiaotai/framework.h: allocation and
 * release are [any] and never [rt] (they take the heap's lock and may walk its
 * free lists); nothing here may be called from an interrupt handler.
 *
 * Diagnostics. The port is the only owner of the platform's global
 * allocation-failure hook, so every failed allocation in the firmware (SDK,
 * platform libraries, the RTC stack) is counted once, here; nobody else
 * registers one. Watermarks are sampled on request (xiaotai_mem_sample), never
 * on the allocation path. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/port/sys.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Large or long-lived buffers: PSRAM when present, and then never internal
     * RAM. Without external RAM: the chip's spare low-power fast memory (RTC
     * fast memory the platform hands to its heap) first, internal RAM when
     * that is full, so small long-lived blocks leave main RAM to stacks and
     * DMA. The port uses that memory only where it serves every use a BULK
     * buffer may have (data, task stacks, atomic operations; never DMA) and
     * where the platform's configuration gives it to the heap; elsewhere BULK
     * is internal RAM. A BULK pointer is not external (xiaotai_mem_is_external)
     * and stays valid with the flash cache off either way. */
    XIAOTAI_MEM_BULK = 0,
    /* Internal RAM: real-time loops, and stacks of tasks that run while the
     * flash cache may be disabled. */
    XIAOTAI_MEM_INTERNAL,
    /* Internal, DMA-capable RAM. */
    XIAOTAI_MEM_DMA,
} xiaotai_mem_kind_t;

typedef struct {
    size_t total;
    size_t free;
    size_t minimum_free;       /* lowest free since boot, kept by the platform */
    size_t largest_block;      /* largest free block now */
    /* Smallest largest_block seen by xiaotai_mem_sample() and by this call;
     * equals largest_block when nothing was sampled. */
    size_t minimum_largest_block;
    uint32_t samples;          /* xiaotai_mem_sample() calls since boot */
} xiaotai_mem_stats_t;

/* [any] NULL on failure. Freed with xiaotai_mem_free whatever the kind. */
void *xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
/* [any] */
void *xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
/* [any] alignment is a power of two. */
void *xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size);
/* [any] NULL is ignored. */
void xiaotai_mem_free(void *pointer);

/* [any, rt] True when the board has external RAM, so BULK is not taken from
 * internal RAM. */
bool xiaotai_mem_has_external(void);
/* [any, rt] True when pointer lies in external RAM. */
bool xiaotai_mem_is_external(const void *pointer);
/* [any] Statistics of the region that serves this kind (without external RAM,
 * BULK and INTERNAL both count the internal heap, low-power fast memory
 * included). Walks the heap: a diagnostics call, not for periodic use on a
 * real-time path. */
void xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats);
/* [any] Records the largest free block of every kind into the watermarks of
 * xiaotai_mem_stats. Whoever wants watermarks calls it periodically from a
 * background task; it walks the heap. */
void xiaotai_mem_sample(void);

typedef struct {
    size_t free;
    size_t largest_block;
    size_t minimum_free;
} xiaotai_mem_rtc_stats_t;

/* [any] Independent diagnostics for the RTC fast-memory heap, in bytes.
 * This is not an allocation kind. Zero is a valid reading, including when
 * the platform reserves the region instead of handing it to the heap.
 * XIAOTAI_ERR_NOT_SUPPORTED when the port has no such capability. */
xiaotai_err_t xiaotai_mem_rtc_stats(xiaotai_mem_rtc_stats_t *stats);

/* ------------------------------------------------------- allocation failures */

#define XIAOTAI_MEM_NAME_MAX 32U

/* The first failed allocation since boot, and how many failed in all. */
typedef struct {
    /* The port's hook is installed; false when the platform has no hook
     * (failures then read 0) or registering it failed (error). */
    bool installed;
    xiaotai_err_t error;
    uint32_t failures;           /* every failed allocation since boot, saturates */
    bool has_first;              /* the fields below describe the first one */
    int64_t first_at_us;         /* xiaotai_time_us() when it failed */
    size_t requested_size;
    uint32_t platform_caps;      /* the platform's capability bits of the request */
    bool in_isr;                 /* requested from an interrupt handler */
    bool cache_enabled;          /* false: the flash cache was off (task name unknown) */
    /* The allocating function and task as the platform names them, cut to
     * fit (*_truncated); "" when unknown. */
    char function[XIAOTAI_MEM_NAME_MAX];
    char task[16];
    bool function_truncated, task_truncated;
} xiaotai_mem_failure_t;

/* [any] Copy of the record; reading changes nothing (the evidence stays).
 *
 * The hook is the platform's one global allocation-failure observer, and the
 * port owns it: it installs it once from the platform's start-up hook, before
 * any component runs, and nothing else in the firmware registers one. A
 * second installation attempt (a restarted subsystem) keeps the first
 * installation and its result. The hook runs in whatever context the failing
 * allocation ran in (an interrupt, the cache off) and therefore:
 *   - only counts and fills the record, under a lock that masks interrupts on
 *     a single core and a spin lock across cores, held for a bounded copy;
 *   - never allocates, waits, queries the heap, logs or calls back;
 *   - records the first failure only (later ones never overwrite it) and
 *     saturates failures at UINT32_MAX instead of wrapping;
 *   - keeps its code and its record (under 128 bytes) in internal RAM, and
 *     reads the time and the interrupt context only through functions that
 *     stay callable with the cache off. Where the platform's configuration
 *     does not guarantee that (on ESP-IDF: CONFIG_ESP_TIMER_IN_IRAM, and
 *     interrupt context queries not placed in flash), the port does not
 *     install the hook: installed is false and error is
 *     XIAOTAI_ERR_NOT_SUPPORTED, rather than a hook that may fault. */
void xiaotai_mem_failure(xiaotai_mem_failure_t *failure);

/* ------------------------------------------------------------- heap tracing */

/* Diagnostic builds whose platform configuration enables standalone heap
 * tracing: record allocations still outstanding between start and stop.
 * XIAOTAI_ERR_NOT_SUPPORTED in every other build; the record buffer is the
 * platform's (a fixed number of entries chosen in its configuration).
 * [any] start: XIAOTAI_ERR_INVALID_STATE while a trace runs. stop: ends the
 * recording, keeps the records for dump. dump: writes one line per
 * outstanding allocation and a summary line through write; finishes the
 * platform's own bookkeeping so a later start begins clean. */
xiaotai_err_t xiaotai_mem_trace_start(void);
xiaotai_err_t xiaotai_mem_trace_stop(void);
xiaotai_err_t xiaotai_mem_trace_dump(xiaotai_port_write_fn_t write, void *ctx);

#ifdef __cplusplus
}
#endif
