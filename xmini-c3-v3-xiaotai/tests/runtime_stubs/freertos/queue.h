// SPDX-License-Identifier: MIT
#pragma once
#include "FreeRTOS.h"
typedef struct { unsigned capacity, size, count, head; uint8_t *buffer; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(unsigned capacity, unsigned size, uint8_t *buffer, StaticQueue_t *storage);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
unsigned uxQueueMessagesWaiting(QueueHandle_t queue);
