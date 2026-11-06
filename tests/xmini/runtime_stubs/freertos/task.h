// SPDX-License-Identifier: MIT
#pragma once
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, unsigned stack, void *context, unsigned priority, TaskHandle_t *task);
TaskHandle_t xTaskCreateStatic(TaskFunction_t function, const char *name, unsigned stack, void *context, unsigned priority, StackType_t *buffer, StaticTask_t *storage);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
