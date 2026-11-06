#pragma once
#include <stdint.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
typedef void *QueueHandle_t;
typedef struct { unsigned unused; } StaticQueue_t;
int xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks);
int xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks);
QueueHandle_t xQueueCreateStatic(unsigned count, unsigned size, uint8_t *buffer,
                                StaticQueue_t *storage);
