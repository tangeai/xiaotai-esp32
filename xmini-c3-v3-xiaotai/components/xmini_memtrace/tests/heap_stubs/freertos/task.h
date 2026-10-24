#pragma once
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
char *pcTaskGetName(TaskHandle_t task);
