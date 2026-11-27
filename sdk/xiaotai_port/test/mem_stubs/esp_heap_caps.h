// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define MALLOC_CAP_SPIRAM (1U << 0)
#define MALLOC_CAP_INTERNAL (1U << 1)
#define MALLOC_CAP_8BIT (1U << 2)
#define MALLOC_CAP_DMA (1U << 3)
#define MALLOC_CAP_RTCRAM (1U << 4)

typedef void (*esp_alloc_failed_hook_t)(size_t, uint32_t, const char *);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t cb);
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void *heap_caps_malloc_prefer(size_t size, size_t tries, ...);
void *heap_caps_calloc_prefer(size_t n, size_t size, size_t tries, ...);
void heap_caps_free(void *pointer);
