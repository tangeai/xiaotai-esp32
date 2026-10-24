// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdTRUE 1
#define pdFALSE 0
#define BIT0 1U
#define BIT1 2U
#define BIT2 4U
#define pdMS_TO_TICKS(ms) (ms)
