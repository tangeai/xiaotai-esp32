#pragma once
#include <stddef.h>
typedef struct {
    size_t lowest_free;
    size_t largest_at_lowest_free;
    size_t lowest_largest;
    unsigned samples;
} memory_watermark_t;
static inline void memory_probe_sample(void) {}
static inline void memory_probe_watermark(memory_watermark_t *watermark)
{
    if (watermark)
        *watermark = (memory_watermark_t){0};
}
