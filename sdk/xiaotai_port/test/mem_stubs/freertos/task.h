// SPDX-License-Identifier: MIT
#pragma once
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
typedef void *TaskHandle_t;
bool xPortInIsrContext(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t task);
UBaseType_t uxTaskPriorityGet(TaskHandle_t task);
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority);
