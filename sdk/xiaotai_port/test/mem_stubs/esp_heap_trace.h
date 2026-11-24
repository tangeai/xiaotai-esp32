// SPDX-License-Identifier: MIT
#pragma once
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include <stdbool.h>

typedef enum { HEAP_TRACE_LEAKS } heap_trace_mode_t;
typedef struct {
    void *address;
    size_t size;
    void *alloced_by[CONFIG_HEAP_TRACING_STACK_DEPTH];
} heap_trace_record_t;
typedef struct {
    size_t count, capacity, high_water_mark;
    bool has_overflowed;
} heap_trace_summary_t;

esp_err_t heap_trace_init_standalone(heap_trace_record_t *buffer, size_t capacity);
esp_err_t heap_trace_start(heap_trace_mode_t mode);
esp_err_t heap_trace_stop(void);
esp_err_t heap_trace_summary(heap_trace_summary_t *summary);
esp_err_t heap_trace_get(size_t index, heap_trace_record_t *record);
