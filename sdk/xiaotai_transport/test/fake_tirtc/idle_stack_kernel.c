// SPDX-License-Identifier: MIT
#include "freertos/task.h"

extern unsigned kernel_calls;
extern uint32_t applied;
extern int next_rc;
extern TaskFunction_t task_code;
extern const char *task_name;
extern void *task_arg;
extern UBaseType_t task_prio;
extern BaseType_t task_core;
extern TaskHandle_t *task_created;

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t code, const char *name,
                                 uint32_t bytes, void *arg, UBaseType_t priority,
                                 TaskHandle_t *created, BaseType_t core)
{
    ++kernel_calls;
    applied = bytes;
    task_code = code;
    task_name = name;
    task_arg = arg;
    task_prio = priority;
    task_core = core;
    task_created = created;
    if (next_rc == 1 && created) *created = (void *)(uintptr_t)0x42;
    return next_rc;
}
