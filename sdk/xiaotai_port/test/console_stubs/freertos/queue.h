// SPDX-License-Identifier: MIT
#pragma once
typedef void *QueueHandle_t;
int xQueueReceive(QueueHandle_t queue, void *out, unsigned wait);
int xQueueReset(QueueHandle_t queue);
