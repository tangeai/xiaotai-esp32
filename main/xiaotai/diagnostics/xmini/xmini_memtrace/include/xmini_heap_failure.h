#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define XMINI_HEAP_FAILURE_FUNCTION_CAPACITY 32
#define XMINI_HEAP_FAILURE_TASK_CAPACITY 16

typedef struct {
    int64_t first_at_us;
    size_t requested_size;
    uintptr_t task_handle;
    uint32_t caps, failures;
    esp_err_t registration_error;
    char function[XMINI_HEAP_FAILURE_FUNCTION_CAPACITY];
    char task[XMINI_HEAP_FAILURE_TASK_CAPACITY];
    bool installed, has_first, function_truncated, task_truncated;
    bool in_isr, cache_enabled, task_available;
} xmini_heap_failure_status_t;

/* Startup-only registration, before board/network/codec/SDK initialization.
 * IDF provides no callback getter/chaining API: the application must own this
 * single hook. Earlier boot allocations are outside this observation window.
 * Repeated calls return the original result and never reset evidence. */
esp_err_t xmini_heap_failure_init(void);
/* Normal cache-enabled control task only. Context flags describe the first
 * failure, not this snapshot. Task metadata is owned only when that failure
 * occurred in a cache-enabled task; an ISR's interrupted task is not claimed. */
void xmini_heap_failure_status(xmini_heap_failure_status_t *status);
