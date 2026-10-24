// SPDX-License-Identifier: MIT
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Only the metrics sampler calls this while the C3 scheduler is suspended. */
TaskHandle_t platform_metrics_worker_handle(void);
