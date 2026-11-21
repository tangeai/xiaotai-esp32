#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT 1U
#define MALLOC_CAP_SPIRAM 2U
#define MALLOC_CAP_INTERNAL 4U
typedef struct {
    size_t total_free_bytes, total_allocated_bytes, largest_free_block, minimum_free_bytes;
    size_t allocated_blocks, free_blocks;
} multi_heap_info_t;
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps);
bool heap_caps_check_integrity(uint32_t caps, bool print_errors);
