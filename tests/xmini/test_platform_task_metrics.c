// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include "demo_platform.h"
#include "platform_task_metrics.h"

static unsigned suspended, samples, bytes;
static int worker;
static TaskHandle_t handle;
static bool alive, exit_on_resume;

void vTaskSuspendAll(void) { assert(!suspended); suspended = 1; }
BaseType_t xTaskResumeAll(void)
{
    assert(suspended == 1); suspended = 0;
    if (exit_on_resume) alive = false;
    return 0;
}
TaskHandle_t platform_metrics_worker_handle(void)
{ assert(suspended == 1); return alive ? handle : NULL; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{ assert(suspended == 1 && alive && task == &worker); ++samples; return bytes; }

int main(void)
{
    unsigned value = 99;
    assert(!platform_stack_watermark(&value) && value == 0 && !samples && !suspended);
    assert(!platform_stack_watermark(NULL) && !samples && !suspended);
    alive = true; // Creation publication has not supplied a handle yet.
    assert(!platform_stack_watermark(&value) && value == 0 && !samples);
    handle = &worker; bytes = 2345;
    assert(platform_stack_watermark(&value) && value == 2345 && samples == 1 && !suspended);
    bytes = 0;
    assert(platform_stack_watermark(&value) && value == 0 && samples == 2);
    alive = false; // Fatal identity error self-deleted; retained s_task is not live.
    assert(!platform_stack_watermark(&value) && value == 0 && samples == 2);
    alive = true; bytes = 1234; exit_on_resume = true;
    assert(platform_stack_watermark(&value) && value == 1234 && samples == 3 && !alive);
    assert(!platform_stack_watermark(&value) && value == 0 && samples == 3);
    return 0;
}
