#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 1
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 2
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 4
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 8
esp_err_t esp_cache_msync(void *addr, size_t size, int flags);
size_t esp_cache_get_line_size_by_addr(const void *addr);
