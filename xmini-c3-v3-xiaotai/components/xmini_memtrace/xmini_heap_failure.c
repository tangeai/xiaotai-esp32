#include "xmini_heap_failure.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_private/cache_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_FREERTOS_UNICORE
#error "The bounded interrupt-mask observer is for the single-core Xmini C3 only"
#endif
#if !CONFIG_ESP_TIMER_IN_IRAM || CONFIG_FREERTOS_PLACE_ISR_FUNCTIONS_INTO_FLASH
#error "The heap observer requires the timer and ISR context/mask queries in IRAM"
#endif

static DRAM_ATTR xmini_heap_failure_status_t s_status = {.registration_error = ESP_ERR_INVALID_STATE};
static DRAM_ATTR const char *s_function;
static DRAM_ATTR bool s_initialized;
_Static_assert(sizeof(s_status) + sizeof(s_function) + sizeof(s_initialized) < 128, "Heap observer RAM budget");

static bool copy_name(char *destination, size_t capacity, const char *source)
{
    size_t used = 0;
    if (source) {
        while (used + 1 < capacity && source[used]) {
            destination[used] = source[used];
            ++used;
        }
    }
    destination[used] = '\0';
    return source && source[used] != '\0';
}

static void IRAM_ATTR allocation_failed(size_t requested_size, uint32_t caps, const char *function)
{
    /* C3 has one core. Saving/restoring its interrupt mask bounds publication
     * without a mutex, spin wait, allocation, heap query or logging callback. */
    UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
    if (s_status.failures != UINT32_MAX)
        ++s_status.failures;
    if (!s_status.has_first) {
        s_status.first_at_us = esp_timer_get_time();
        s_status.requested_size = requested_size;
        s_status.caps = caps;
        /* IDF 5.5.5 supplies static __func__ arrays, potentially in DROM. */
        s_function = function;
        s_status.in_isr = xPortInIsrContext() != 0;
        s_status.cache_enabled = spi_flash_cache_enabled();
        if (!s_status.in_isr && s_status.cache_enabled) {
            TaskHandle_t task = xTaskGetCurrentTaskHandle();
            s_status.task_available = task != NULL;
            s_status.task_handle = (uintptr_t)task;
            s_status.task_truncated = copy_name(s_status.task, sizeof(s_status.task), task ? pcTaskGetName(task) : NULL);
        }
        s_status.has_first = true;
    }
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

esp_err_t xmini_heap_failure_init(void)
{
    if (!s_initialized) {
        s_initialized = true;
        esp_err_t error = heap_caps_register_failed_alloc_callback(allocation_failed);
        UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
        s_status.registration_error = error;
        s_status.installed = error == ESP_OK;
        portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
    }
    return s_status.registration_error;
}

void xmini_heap_failure_status(xmini_heap_failure_status_t *status)
{
    if (!status)
        return;
    UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
    *status = s_status;
    const char *function = s_function;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
    /* This API runs in a normal cached task. The immutable function pointer is
     * image-static; unlike a task name, it cannot expire before this copy. */
    status->function_truncated = copy_name(status->function, sizeof(status->function), function);
}
