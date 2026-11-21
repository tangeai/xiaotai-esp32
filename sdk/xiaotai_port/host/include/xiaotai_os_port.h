// SPDX-License-Identifier: MIT
#pragma once
/* Host (POSIX threads) object types behind xiaotai/port/os.h. */
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct xiaotai_host_task;

typedef struct {
    struct xiaotai_host_task *task;
} xiaotai_task_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool locked;
    pthread_t owner;
} xiaotai_mutex_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool given;
} xiaotai_signal_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    uint8_t *items;
    size_t depth, item_size, head, count;
} xiaotai_queue_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    uint32_t bits;
} xiaotai_events_t;

typedef struct {
    pthread_mutex_t mutex;
} xiaotai_critical_t;
