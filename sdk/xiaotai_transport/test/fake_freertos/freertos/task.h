// SPDX-License-Identifier: MIT
#pragma once
#include "freertos/FreeRTOS.h"

TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t code, const char *name,
                                 uint32_t bytes, void *arg, UBaseType_t priority,
                                 TaskHandle_t *created, BaseType_t core);
