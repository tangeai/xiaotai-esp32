#pragma once
#include "FreeRTOS.h"
typedef struct {
    unsigned capacity, size, head, count;
    uint8_t *data;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t size, uint8_t *storage,
                                  StaticQueue_t *queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks);
BaseType_t xQueueReset(QueueHandle_t queue);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
