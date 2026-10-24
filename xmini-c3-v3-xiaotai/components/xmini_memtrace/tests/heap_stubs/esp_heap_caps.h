#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void (*esp_alloc_failed_hook_t)(size_t size, uint32_t caps, const char *function);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);
