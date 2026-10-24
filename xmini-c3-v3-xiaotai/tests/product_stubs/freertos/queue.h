// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"
typedef struct { unsigned item_size, count; uint8_t *buffer; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(unsigned length, unsigned size, uint8_t *buffer, StaticQueue_t *storage);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
