// SPDX-License-Identifier: MIT
/* xiaotai/port/os.h on ESP-IDF FreeRTOS. Thin wrappers: no extra tasks or heap. */
#include "xiaotai/port/os.h"

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"

/* Rounds up, and any non-zero time is at least one tick: pdMS_TO_TICKS rounds
 * down, which below 1 kHz turns a 1 ms yield into vTaskDelay(0) and starves
 * every lower-priority task (xiaotai/port/os.h). */
static TickType_t ticks(uint32_t ms)
{
    if (ms == XIAOTAI_WAIT_FOREVER) return portMAX_DELAY;
    if (ms == 0) return 0;
    const uint64_t rounded = ((uint64_t)ms * configTICK_RATE_HZ + 999U) / 1000U;
    if (rounded >= (uint64_t)portMAX_DELAY) return portMAX_DELAY - 1;
    return rounded ? (TickType_t)rounded : 1;
}

int64_t xiaotai_time_us(void) { return esp_timer_get_time(); }

void xiaotai_delay_ms(uint32_t ms) { vTaskDelay(ticks(ms)); }

bool xiaotai_in_isr(void) { return xPortInIsrContext(); }

xiaotai_err_t xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (!config || !config->entry || !config->name || config->stack_bytes == 0 || !task ||
        config->priority >= configMAX_PRIORITIES) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    const BaseType_t core = config->core == XIAOTAI_CORE_ANY || config->core >= portNUM_PROCESSORS
                                ? tskNO_AFFINITY
                                : (BaseType_t)config->core;
    if (config->stack == XIAOTAI_MEM_BULK && xiaotai_mem_has_external()) {
        return xTaskCreatePinnedToCoreWithCaps(config->entry, config->name, config->stack_bytes, config->arg,
                                               config->priority, &task->handle, core,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS
                   ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
    }
    /* IDF's dynamic task allocator keeps the TCB and stack internal. Using
     * it here also lets self-deletion succeed without creating a cleanup task. */
    return xTaskCreatePinnedToCore(config->entry, config->name, config->stack_bytes, config->arg,
                                   config->priority, &task->handle, core) == pdPASS
               ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

void xiaotai_task_exit(void)
{
    if (esp_ptr_external_ram(esp_cpu_get_sp())) vTaskDeleteWithCaps(NULL);
    else vTaskDelete(NULL);
    for (;;) {
    }
}

bool xiaotai_task_is_current(const xiaotai_task_t *task)
{
    return task && task->handle && task->handle == xTaskGetCurrentTaskHandle();
}

xiaotai_err_t xiaotai_task_borrow_current(xiaotai_task_t *task)
{
    if (!task) return XIAOTAI_ERR_INVALID_ARG;
    if (xiaotai_in_isr()) return XIAOTAI_ERR_INVALID_STATE;
    task->handle = xTaskGetCurrentTaskHandle();
    return task->handle ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t xiaotai_task_set_current_priority(unsigned priority, unsigned *previous)
{
    if (priority >= configMAX_PRIORITIES) return XIAOTAI_ERR_INVALID_ARG;
    if (xiaotai_in_isr()) return XIAOTAI_ERR_INVALID_STATE;
    if (previous) *previous = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, (UBaseType_t)priority);
    return XIAOTAI_OK;
}

uint32_t xiaotai_task_current_stack_free(void)
{
#if INCLUDE_uxTaskGetStackHighWaterMark
    return (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
#elif INCLUDE_uxTaskGetStackHighWaterMark2
    return (uint32_t)uxTaskGetStackHighWaterMark2(NULL) * sizeof(StackType_t);
#else
    return 0;
#endif
}

xiaotai_err_t xiaotai_require_internal_stack(const char *operation)
{
    /* Match the cache-freeze contract; the current CPU SP also covers main
     * and system tasks that were not created by xiaotai_task_create(). */
    const void *sp = (const void *)esp_cpu_get_sp();
    if (esp_ptr_in_dram(sp)) return XIAOTAI_OK;
#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
    if (esp_ptr_in_rtc_dram_fast(sp)) return XIAOTAI_OK;
#endif
    ESP_LOGE("xiaotai_os", "cache-unsafe task stack: task=%s operation=%s sp=%p",
             pcTaskGetName(NULL), operation ? operation : "?", sp);
    return XIAOTAI_ERR_INVALID_STATE;
}

void xiaotai_task_notify(xiaotai_task_t *task)
{
    if (task && task->handle) xTaskNotifyGive(task->handle);
}

bool xiaotai_task_wait_notify(uint32_t timeout_ms) { return ulTaskNotifyTake(pdTRUE, ticks(timeout_ms)) > 0; }

xiaotai_err_t xiaotai_mutex_init(xiaotai_mutex_t *mutex)
{
    if (!mutex) return XIAOTAI_ERR_INVALID_ARG;
    mutex->handle = xSemaphoreCreateMutexStatic(&mutex->storage);
    return mutex->handle ? XIAOTAI_OK : XIAOTAI_FAIL;
}

bool xiaotai_mutex_lock(xiaotai_mutex_t *mutex, uint32_t timeout_ms)
{
    return xSemaphoreTake(mutex->handle, ticks(timeout_ms)) == pdTRUE;
}

void xiaotai_mutex_unlock(xiaotai_mutex_t *mutex) { xSemaphoreGive(mutex->handle); }

void xiaotai_mutex_deinit(xiaotai_mutex_t *mutex)
{
    if (mutex && mutex->handle) vSemaphoreDelete(mutex->handle);
    if (mutex) mutex->handle = NULL;
}

xiaotai_err_t xiaotai_signal_init(xiaotai_signal_t *signal)
{
    if (!signal) return XIAOTAI_ERR_INVALID_ARG;
    signal->handle = xSemaphoreCreateBinaryStatic(&signal->storage);
    return signal->handle ? XIAOTAI_OK : XIAOTAI_FAIL;
}

void xiaotai_signal_give(xiaotai_signal_t *signal) { xSemaphoreGive(signal->handle); }

void xiaotai_signal_give_from_isr(xiaotai_signal_t *signal)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(signal->handle, &woken);
    portYIELD_FROM_ISR(woken);
}

bool xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout_ms)
{
    return xSemaphoreTake(signal->handle, ticks(timeout_ms)) == pdTRUE;
}

void xiaotai_signal_deinit(xiaotai_signal_t *signal)
{
    if (signal && signal->handle) vSemaphoreDelete(signal->handle);
    if (signal) signal->handle = NULL;
}

xiaotai_err_t xiaotai_queue_init(xiaotai_queue_t *queue, size_t depth, size_t item_size,
                                 xiaotai_mem_kind_t storage)
{
    if (!queue || depth == 0 || item_size == 0 || depth > SIZE_MAX / item_size) return XIAOTAI_ERR_INVALID_ARG;
    queue->items = xiaotai_mem_alloc(storage, depth * item_size);
    if (!queue->items) return XIAOTAI_ERR_NO_MEM;
    queue->handle = xQueueCreateStatic(depth, item_size, queue->items, &queue->storage);
    if (!queue->handle) {
        xiaotai_mem_free(queue->items);
        queue->items = NULL;
        return XIAOTAI_FAIL;
    }
    return XIAOTAI_OK;
}

bool xiaotai_queue_send(xiaotai_queue_t *queue, const void *item, uint32_t timeout_ms)
{
    return xQueueSend(queue->handle, item, ticks(timeout_ms)) == pdTRUE;
}

void xiaotai_queue_overwrite(xiaotai_queue_t *queue, const void *item) { xQueueOverwrite(queue->handle, item); }

bool xiaotai_queue_receive(xiaotai_queue_t *queue, void *item, uint32_t timeout_ms)
{
    return xQueueReceive(queue->handle, item, ticks(timeout_ms)) == pdTRUE;
}

size_t xiaotai_queue_count(xiaotai_queue_t *queue) { return uxQueueMessagesWaiting(queue->handle); }

void xiaotai_queue_reset(xiaotai_queue_t *queue) { xQueueReset(queue->handle); }

void xiaotai_queue_deinit(xiaotai_queue_t *queue)
{
    if (!queue) return;
    if (queue->handle) vQueueDelete(queue->handle);
    xiaotai_mem_free(queue->items);
    queue->handle = NULL;
    queue->items = NULL;
}

xiaotai_err_t xiaotai_events_init(xiaotai_events_t *events)
{
    if (!events) return XIAOTAI_ERR_INVALID_ARG;
    events->handle = xEventGroupCreateStatic(&events->storage);
    return events->handle ? XIAOTAI_OK : XIAOTAI_FAIL;
}

void xiaotai_events_set(xiaotai_events_t *events, uint32_t bits) { xEventGroupSetBits(events->handle, bits); }

void xiaotai_events_clear(xiaotai_events_t *events, uint32_t bits) { xEventGroupClearBits(events->handle, bits); }

uint32_t xiaotai_events_get(xiaotai_events_t *events) { return xEventGroupGetBits(events->handle); }

uint32_t xiaotai_events_wait(xiaotai_events_t *events, uint32_t bits, bool all, bool clear, uint32_t timeout_ms)
{
    return xEventGroupWaitBits(events->handle, bits, clear ? pdTRUE : pdFALSE, all ? pdTRUE : pdFALSE,
                               ticks(timeout_ms));
}

void xiaotai_events_deinit(xiaotai_events_t *events)
{
    if (events && events->handle) vEventGroupDelete(events->handle);
    if (events) events->handle = NULL;
}

void xiaotai_critical_init(xiaotai_critical_t *lock) { portMUX_INITIALIZE(&lock->mux); }

void xiaotai_critical_enter(xiaotai_critical_t *lock) { portENTER_CRITICAL_SAFE(&lock->mux); }

void xiaotai_critical_exit(xiaotai_critical_t *lock) { portEXIT_CRITICAL_SAFE(&lock->mux); }
