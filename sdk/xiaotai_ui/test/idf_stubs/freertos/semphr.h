#pragma once
#include <stdbool.h>
#include "FreeRTOS.h"
typedef struct { bool given; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
