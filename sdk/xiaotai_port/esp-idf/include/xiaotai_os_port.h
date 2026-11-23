// SPDX-License-Identifier: MIT
#pragma once
/* ESP-IDF object types behind xiaotai/port/os.h. Kernel state must stay in
 * internal RAM: keep these objects in static storage or XIAOTAI_MEM_INTERNAL. */
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    TaskHandle_t handle;
} xiaotai_task_t;

typedef struct {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
} xiaotai_mutex_t;

typedef struct {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
} xiaotai_signal_t;

typedef struct {
    StaticQueue_t storage;
    QueueHandle_t handle;
    uint8_t *items;
} xiaotai_queue_t;

typedef struct {
    StaticEventGroup_t storage;
    EventGroupHandle_t handle;
} xiaotai_events_t;

typedef struct {
    portMUX_TYPE mux;
} xiaotai_critical_t;
