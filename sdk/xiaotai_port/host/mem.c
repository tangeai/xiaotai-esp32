// SPDX-License-Identifier: MIT
/* xiaotai/port/mem.h on the host: one heap, no external RAM or native heap
 * tracing. Allocation counts, failure injection and an opt-in product ledger
 * are in xiaotai_mem_host.h. */
#include "xiaotai/port/mem.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include "mem_ledger.h"

static _Atomic uint32_t s_allocations;
static _Atomic int64_t s_outstanding;
/* Allocations left before the injected failure; 0: none pending. */
static _Atomic uint32_t s_fail_countdown;
static pthread_mutex_t s_failure_lock = PTHREAD_MUTEX_INITIALIZER;
static xiaotai_mem_failure_t s_failure = {.installed = true};

static struct {
    const void *key;
    xiaotai_mem_host_ledger_entry_t entry;
} s_ledger[XIAOTAI_MEM_HOST_LEDGER_CAPACITY];
static _Atomic bool s_ledger_enabled;
static pthread_mutex_t s_ledger_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_ledger_overflow;

static void ledger_add(const void *key, size_t bytes, xiaotai_mem_kind_t kind, uintptr_t site,
                       const char *task_name, bool fixture)
{
    if (!key || !atomic_load_explicit(&s_ledger_enabled, memory_order_acquire)) return;
    pthread_mutex_lock(&s_ledger_lock);
    size_t i;
    for (i = 0; i < XIAOTAI_MEM_HOST_LEDGER_CAPACITY && s_ledger[i].key; ++i) {
    }
    if (i == XIAOTAI_MEM_HOST_LEDGER_CAPACITY) s_ledger_overflow = true;
    else {
        s_ledger[i].key = key;
        s_ledger[i].entry = (xiaotai_mem_host_ledger_entry_t){
            .bytes = bytes, .site = site, .kind = kind, .task = task_name != NULL, .fixture = fixture,
        };
        if (task_name)
            strncpy(s_ledger[i].entry.name, task_name, sizeof(s_ledger[i].entry.name) - 1);
    }
    pthread_mutex_unlock(&s_ledger_lock);
}

static void ledger_remove(const void *key, bool task)
{
    if (!key || !atomic_load_explicit(&s_ledger_enabled, memory_order_acquire)) return;
    pthread_mutex_lock(&s_ledger_lock);
    for (size_t i = 0; i < XIAOTAI_MEM_HOST_LEDGER_CAPACITY; ++i) {
        if (s_ledger[i].key == key && s_ledger[i].entry.task == task) {
            s_ledger[i].key = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&s_ledger_lock);
}

void xiaotai_mem_host_ledger_start(void)
{
    atomic_store_explicit(&s_ledger_enabled, true, memory_order_release);
}

bool xiaotai_mem_host_ledger_snapshot(xiaotai_mem_host_ledger_entry_t *entries, size_t capacity,
                                    size_t *count)
{
    if (!count || (capacity && !entries) ||
        !atomic_load_explicit(&s_ledger_enabled, memory_order_acquire)) return false;
    size_t used = 0;
    pthread_mutex_lock(&s_ledger_lock);
    for (size_t i = 0; i < XIAOTAI_MEM_HOST_LEDGER_CAPACITY; ++i) {
        if (!s_ledger[i].key) continue;
        if (used < capacity) entries[used] = s_ledger[i].entry;
        ++used;
    }
    const bool complete = !s_ledger_overflow && used <= capacity;
    pthread_mutex_unlock(&s_ledger_lock);
    *count = used;
    return complete;
}

void xiaotai_mem_host_task_open(const void *task, const char *name, xiaotai_mem_kind_t kind,
                                size_t bytes, uintptr_t site)
{
    ledger_add(task, bytes, kind, site, name, false);
}

void xiaotai_mem_host_task_close(const void *task)
{
    ledger_remove(task, true);
}

void xiaotai_mem_host_buffer_site(const void *buffer, uintptr_t site)
{
    if (!buffer || !atomic_load_explicit(&s_ledger_enabled, memory_order_acquire)) return;
    pthread_mutex_lock(&s_ledger_lock);
    for (size_t i = 0; i < XIAOTAI_MEM_HOST_LEDGER_CAPACITY; ++i) {
        if (s_ledger[i].key == buffer && !s_ledger[i].entry.task) {
            s_ledger[i].entry.site = site;
            break;
        }
    }
    pthread_mutex_unlock(&s_ledger_lock);
}

static void record_failure(size_t size, const char *function)
{
    pthread_mutex_lock(&s_failure_lock);
    if (s_failure.failures != UINT32_MAX) ++s_failure.failures;
    if (!s_failure.has_first) {
        s_failure.has_first = true;
        s_failure.first_at_us = xiaotai_time_us();
        s_failure.requested_size = size;
        s_failure.cache_enabled = true;
        strncpy(s_failure.function, function, sizeof(s_failure.function) - 1);
    }
    pthread_mutex_unlock(&s_failure_lock);
}

/* Counts the attempt; true when this one is the injected failure. */
static bool injected(size_t size, const char *function)
{
    atomic_fetch_add(&s_allocations, 1U);
    uint32_t left = atomic_load(&s_fail_countdown);
    while (left && !atomic_compare_exchange_weak(&s_fail_countdown, &left, left - 1U)) {
    }
    if (left != 1U) return false;
    record_failure(size, function);
    return true;
}

static void *counted(void *pointer, xiaotai_mem_kind_t kind, size_t bytes, uintptr_t site, bool fixture)
{
    if (pointer) {
        atomic_fetch_add(&s_outstanding, 1);
        ledger_add(pointer, bytes, kind, site, NULL, fixture);
    }
    return pointer;
}

static void *allocate(xiaotai_mem_kind_t kind, size_t size, uintptr_t site, bool fixture)
{
    if (injected(size, "xiaotai_mem_alloc")) return NULL;
    return counted(size ? malloc(size) : NULL, kind, size, site, fixture);
}

static void *zeroed(xiaotai_mem_kind_t kind, size_t count, size_t size, uintptr_t site, bool fixture)
{
    if (injected(count * size, "xiaotai_mem_calloc")) return NULL;
    return counted(count && size ? calloc(count, size) : NULL, kind, count * size, site, fixture);
}

void *xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    return allocate(kind, size, XIAOTAI_LEDGER_CALLER, false);
}

void *xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    return zeroed(kind, count, size, XIAOTAI_LEDGER_CALLER, false);
}

void *xiaotai_mem_host_fixture_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    return allocate(kind, size, XIAOTAI_LEDGER_CALLER, true);
}

void *xiaotai_mem_host_fixture_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    return zeroed(kind, count, size, XIAOTAI_LEDGER_CALLER, true);
}

void *xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size)
{
    if (injected(size, "xiaotai_mem_aligned")) return NULL;
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0 || size == 0) return NULL;
    void *pointer = NULL;
    return counted(posix_memalign(&pointer, alignment, size) == 0 ? pointer : NULL,
                   kind, size, XIAOTAI_LEDGER_CALLER, false);
}

void xiaotai_mem_free(void *pointer)
{
    if (!pointer) return;
    atomic_fetch_sub(&s_outstanding, 1);
    ledger_remove(pointer, false);
    free(pointer);
}

bool xiaotai_mem_has_external(void) { return false; }

bool xiaotai_mem_is_external(const void *pointer)
{
    (void)pointer;
    return false;
}

static _Atomic uint32_t s_samples;

void xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    (void)kind;
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->samples = s_samples;
}

void xiaotai_mem_sample(void)
{
    if (s_samples != UINT32_MAX) ++s_samples;
}

xiaotai_err_t xiaotai_mem_rtc_stats(xiaotai_mem_rtc_stats_t *stats)
{
    return stats ? XIAOTAI_ERR_NOT_SUPPORTED : XIAOTAI_ERR_INVALID_ARG;
}

void xiaotai_mem_failure(xiaotai_mem_failure_t *failure)
{
    if (!failure) return;
    pthread_mutex_lock(&s_failure_lock);
    *failure = s_failure;
    pthread_mutex_unlock(&s_failure_lock);
}

xiaotai_err_t xiaotai_mem_trace_start(void) { return XIAOTAI_ERR_NOT_SUPPORTED; }
xiaotai_err_t xiaotai_mem_trace_stop(void) { return XIAOTAI_ERR_NOT_SUPPORTED; }

xiaotai_err_t xiaotai_mem_trace_dump(xiaotai_port_write_fn_t write, void *ctx)
{
    (void)write;
    (void)ctx;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

/* ------------------------------------------------------------ test controls */

void xiaotai_mem_host_fail_nth(uint32_t n) { atomic_store(&s_fail_countdown, n); }

int xiaotai_mem_host_fail_pending(void) { return atomic_load(&s_fail_countdown) != 0; }

uint32_t xiaotai_mem_host_allocations(void) { return atomic_load(&s_allocations); }

int64_t xiaotai_mem_host_outstanding(void) { return atomic_load(&s_outstanding); }

void xiaotai_mem_host_clear(void)
{
    atomic_store(&s_fail_countdown, 0U);
    atomic_store(&s_allocations, 0U);
    atomic_store(&s_outstanding, 0);
    pthread_mutex_lock(&s_failure_lock);
    memset(&s_failure, 0, sizeof(s_failure));
    s_failure.installed = true;
    pthread_mutex_unlock(&s_failure_lock);
}
