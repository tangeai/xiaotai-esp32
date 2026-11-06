// SPDX-License-Identifier: MIT
#include "demo_platform.h"
#include "platform_task_metrics.h"

bool platform_stack_watermark(unsigned *bytes)
{
    if (!bytes) return false;
    *bytes = 0;
    // C3 is single-core: a fatal worker cannot self-delete during this sample.
    vTaskSuspendAll();
    TaskHandle_t task = platform_metrics_worker_handle();
    if (task) *bytes = (unsigned)uxTaskGetStackHighWaterMark(task);
    (void)xTaskResumeAll();
    return task != NULL;
}
