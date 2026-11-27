// SPDX-License-Identifier: MIT
#include "xiaotai/port/mem.h"
#include "xiaotai/port_config.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_private/cache_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static DRAM_ATTR xiaotai_mem_failure_t s_first = {.error = XIAOTAI_ERR_INVALID_STATE};
static DRAM_ATTR portMUX_TYPE s_failure_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR const char *s_function;
_Static_assert(sizeof(s_first) + sizeof(s_function) + sizeof(s_failure_lock) + sizeof(bool) < 128,
               "Allocation-failure observer storage must stay bounded");

static bool copy_name(char *out, size_t capacity, const char *name)
{
    size_t count = 0;
    if (name) while (count + 1 < capacity && name[count]) {
        out[count] = name[count];
        ++count;
    }
    out[count] = '\0';
    return name && name[count];
}

#if defined(XIAOTAI_PORT_NATIVE_STARTUP) || defined(XIAOTAI_MEM_DIAG_FIXTURE)
static bool s_install_attempted;

#if CONFIG_ESP_TIMER_IN_IRAM && !CONFIG_FREERTOS_PLACE_ISR_FUNCTIONS_INTO_FLASH
static void IRAM_ATTR allocation_failed(size_t size, uint32_t caps, const char *function)
{
    portENTER_CRITICAL_SAFE(&s_failure_lock);
    if (s_first.failures != UINT32_MAX) ++s_first.failures;
    if (!s_first.has_first) {
        s_first.first_at_us = esp_timer_get_time();
        s_first.requested_size = size;
        s_first.platform_caps = caps;
        s_first.in_isr = xPortInIsrContext();
        s_first.cache_enabled = spi_flash_cache_enabled();
        /* The allocator's function string is static and may be in flash. */
        s_function = function;
        if (!s_first.in_isr && s_first.cache_enabled) {
            TaskHandle_t task = xTaskGetCurrentTaskHandle();
            s_first.task_truncated = copy_name(s_first.task, sizeof(s_first.task), task ? pcTaskGetName(task) : NULL);
        }
        s_first.has_first = true;
    }
    portEXIT_CRITICAL_SAFE(&s_failure_lock);
}
#endif

/* This is called only during single-threaded boot. */
static xiaotai_err_t install_observer(void)
{
    if (s_install_attempted) return s_first.error;
    s_install_attempted = true;
#if CONFIG_ESP_TIMER_IN_IRAM && !CONFIG_FREERTOS_PLACE_ISR_FUNCTIONS_INTO_FLASH
    xiaotai_err_t err = heap_caps_register_failed_alloc_callback(allocation_failed);
#else
    xiaotai_err_t err = XIAOTAI_ERR_NOT_SUPPORTED;
#endif
    portENTER_CRITICAL_SAFE(&s_failure_lock);
    s_first.error = err;
    s_first.installed = err == XIAOTAI_OK;
    portEXIT_CRITICAL_SAFE(&s_failure_lock);
    return err;
}
#endif

/* BI-4 enables this in the product build when it removes all legacy hooks. */
#if defined(XIAOTAI_PORT_NATIVE_STARTUP)
#include "esp_private/startup_internal.h"
ESP_SYSTEM_INIT_FN(xiaotai_mem_diag, SECONDARY, BIT(0), 210)
{
    (void)install_observer();
    /* BI-4 establishes ownership; IDF cannot detect a conflicting registration. */
    return ESP_OK;
}
#endif

void xiaotai_mem_failure(xiaotai_mem_failure_t *failure)
{
    if (!failure) return;
    portENTER_CRITICAL_SAFE(&s_failure_lock);
    *failure = s_first;
    const char *function = s_function;
    portEXIT_CRITICAL_SAFE(&s_failure_lock);
    failure->function_truncated = copy_name(failure->function, sizeof(failure->function), function);
}

/* The supported IDF lines do not handle failure of the optional hash table
 * allocation. The standalone array mode has a bounded, checked allocation. */
#if CONFIG_HEAP_TRACING_STANDALONE && !CONFIG_HEAP_TRACE_HASH_MAP
#include "esp_heap_trace.h"
#include <stdio.h>

typedef enum { EMPTY, STARTING, RECORDING, STOPPING, STOPPED, DUMPING } trace_state_t;
static trace_state_t s_trace_state;
static portMUX_TYPE s_trace_lock = portMUX_INITIALIZER_UNLOCKED;
static heap_trace_record_t *s_records;
static heap_trace_record_t s_inactive;

static bool begin_trace_op(trace_state_t from, trace_state_t from_also, trace_state_t next)
{
    portENTER_CRITICAL(&s_trace_lock);
    bool ok = s_trace_state == from || s_trace_state == from_also;
    if (ok) s_trace_state = next;
    portEXIT_CRITICAL(&s_trace_lock);
    return ok;
}

static void set_trace_state(trace_state_t state)
{
    portENTER_CRITICAL(&s_trace_lock);
    s_trace_state = state;
    portEXIT_CRITICAL(&s_trace_lock);
}

static esp_err_t discard_records(void)
{
    /* Detach the old storage first: supported IDF lines reject NULL/0. */
    esp_err_t err = heap_trace_init_standalone(&s_inactive, 1);
    if (err == ESP_OK) {
        xiaotai_mem_free(s_records);
        s_records = NULL;
    }
    return err;
}

xiaotai_err_t xiaotai_mem_trace_start(void)
{
    if (!begin_trace_op(EMPTY, STOPPED, STARTING)) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_records) {
        heap_trace_record_t *records = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, XIAOTAI_PORT_HEAP_TRACE_RECORDS,
                                                         sizeof(*records));
        if (!records) {
            set_trace_state(EMPTY);
            return XIAOTAI_ERR_NO_MEM;
        }
        esp_err_t err = heap_trace_init_standalone(records, XIAOTAI_PORT_HEAP_TRACE_RECORDS);
        if (err != ESP_OK) {
            xiaotai_mem_free(records);
            set_trace_state(EMPTY);
            return err;
        }
        s_records = records;
    }
    esp_err_t err = heap_trace_start(HEAP_TRACE_LEAKS);
    if (err != ESP_OK) (void)discard_records();
    set_trace_state(err == ESP_OK ? RECORDING : s_records ? STOPPED : EMPTY);
    return err;
}

xiaotai_err_t xiaotai_mem_trace_stop(void)
{
    if (!begin_trace_op(RECORDING, RECORDING, STOPPING)) return XIAOTAI_ERR_INVALID_STATE;
    esp_err_t err = heap_trace_stop();
    set_trace_state(err == ESP_OK ? STOPPED : RECORDING);
    return err;
}

xiaotai_err_t xiaotai_mem_trace_dump(xiaotai_port_write_fn_t write, void *ctx)
{
    if (!write) return XIAOTAI_ERR_INVALID_ARG;
    if (!begin_trace_op(STOPPED, STOPPED, DUMPING)) return XIAOTAI_ERR_INVALID_STATE;
    heap_trace_summary_t summary;
    esp_err_t err = heap_trace_summary(&summary);
    char line[96 + CONFIG_HEAP_TRACING_STACK_DEPTH * (8 + 2 * sizeof(void *))];
    for (size_t i = 0; err == ESP_OK && i < summary.count; ++i) {
        heap_trace_record_t rec;
        err = heap_trace_get(i, &rec);
        if (err != ESP_OK) break;
        size_t used = (size_t)snprintf(line, sizeof(line), "MEM_BLOCK ptr=%p size=%zu", rec.address, rec.size);
#if CONFIG_HEAP_TRACING_STACK_DEPTH > 0
        for (size_t j = 0; j < CONFIG_HEAP_TRACING_STACK_DEPTH && used < sizeof(line); ++j)
            used += (size_t)snprintf(line + used, sizeof(line) - used, " pc=%p", rec.alloced_by[j]);
#else
        (void)used;
#endif
        write(ctx, line);
    }
    if (err == ESP_OK) {
        (void)snprintf(line, sizeof(line), "MEM_TRACE count=%zu capacity=%zu high=%zu overflow=%u",
                       (size_t)summary.count, (size_t)summary.capacity, (size_t)summary.high_water_mark,
                       (unsigned)summary.has_overflowed);
        write(ctx, line);
        err = discard_records();
    }
    set_trace_state(s_records ? STOPPED : EMPTY);
    return err;
}
#else
xiaotai_err_t xiaotai_mem_trace_start(void) { return XIAOTAI_ERR_NOT_SUPPORTED; }
xiaotai_err_t xiaotai_mem_trace_stop(void) { return XIAOTAI_ERR_NOT_SUPPORTED; }
xiaotai_err_t xiaotai_mem_trace_dump(xiaotai_port_write_fn_t write, void *ctx)
{
    (void)write;
    (void)ctx;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
#endif
