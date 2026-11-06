#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
size_t heap_caps_get_free_size(unsigned capabilities);
size_t heap_caps_get_largest_free_block(unsigned capabilities);
