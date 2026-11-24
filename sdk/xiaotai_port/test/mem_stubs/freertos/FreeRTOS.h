// SPDX-License-Identifier: MIT
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
typedef atomic_int portMUX_TYPE;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 100U
#define tskIDLE_PRIORITY 0U
#define portMAX_DELAY UINT32_MAX
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) do { while (atomic_exchange_explicit((mux), 1, memory_order_acquire)) {} } while (0)
#define portEXIT_CRITICAL(mux) atomic_store_explicit((mux), 0, memory_order_release)
#define portENTER_CRITICAL_SAFE portENTER_CRITICAL
#define portEXIT_CRITICAL_SAFE portEXIT_CRITICAL
