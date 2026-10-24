// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
typedef int BaseType_t;
typedef uint32_t StackType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef int StaticTask_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
#define portBYTE_ALIGNMENT 4
