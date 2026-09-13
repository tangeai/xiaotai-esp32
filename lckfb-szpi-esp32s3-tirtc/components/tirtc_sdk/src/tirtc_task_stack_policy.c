#include <string.h>

#include "esp_log.h"

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

    if (name != NULL && strcmp(name, "rtc_thread") == 0 &&
        effective_stack_size < TIRTC_RTC_THREAD_MIN_STACK_BYTES) {
        effective_stack_size = TIRTC_RTC_THREAD_MIN_STACK_BYTES;
        ESP_LOGI("tirtc_task_stack",
                 "TiRTC task stack adjusted: name=%s requested=%d effective=%d",
                 name,
                 stack_size,
                 effective_stack_size);
    }

    return __real_freertos_ThreadCreateWithStackSize(thread_handle,
                                                      routine,
                                                      arg,
                                                      effective_stack_size,
                                                      name);
}
