#pragma once
#include "FreeRTOS.h"
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
