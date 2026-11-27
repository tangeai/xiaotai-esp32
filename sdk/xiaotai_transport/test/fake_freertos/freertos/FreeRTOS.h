// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#define portBYTE_ALIGNMENT 16
#define CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY 1
typedef uint8_t StackType_t;
typedef struct { uint8_t data[84]; } StaticSemaphore_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
