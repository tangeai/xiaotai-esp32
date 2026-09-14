#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"

/*
 * TiRTC owns the TGTRP worker lifecycle.  A device-to-device call exercises
 * the worker's TLS, signalling and media paths together, which exceeds the
 * small default external-RAM stack in the prebuilt SDK.  Enforce the budget at
 * the SDK boundary so application state machines do not carry an SDK detail.
 */
#define TIRTC_RTC_THREAD_MIN_STACK_BYTES (24 * 1024)

typedef void (*tirtc_thread_routine_t)(void *arg);

extern int __real_freertos_ThreadCreateWithStackSize(void *thread_handle,
                                                      tirtc_thread_routine_t routine,
                                                      void *arg,
                                                      int stack_size,
                                                      const char *name);

int __wrap_freertos_ThreadCreateWithStackSize(void *thread_handle,
                                              tirtc_thread_routine_t routine,
                                              void *arg,
                                              int stack_size,
                                              const char *name)
{
    int effective_stack_size = stack_size;
    int trace_thread = name != NULL && strcmp(name, "rtc_thread") == 0;

    if (trace_thread &&
        effective_stack_size < TIRTC_RTC_THREAD_MIN_STACK_BYTES) {
        effective_stack_size = TIRTC_RTC_THREAD_MIN_STACK_BYTES;
    }
    if (trace_thread) {
        ESP_LOGI("tirtc_task_stack",
                 "CONN rtc_thread begin: name=%s requested=%d effective=%d",
                 name,
                 stack_size,
                 effective_stack_size);
    }

    uint32_t started_ms = trace_thread ? (uint32_t)(esp_timer_get_time() / 1000) : 0U;
    int rc = __real_freertos_ThreadCreateWithStackSize(thread_handle,
                                                    routine,
                                                    arg,
                                                    effective_stack_size,
                                                    name);
    if (trace_thread) {
        ESP_LOGI("tirtc_task_stack", "CONN rtc_thread end: create_ms=%lu rc=%d",
                 (unsigned long)((uint32_t)(esp_timer_get_time() / 1000) - started_ms), rc);
    }
    return rc;
}
