#pragma once
#include <stddef.h>

typedef struct {
    size_t lowest_free;
    size_t largest_at_lowest_free;
    size_t lowest_largest;
    unsigned samples;
} memory_watermark_t;

/* Capture free bytes and the largest block at nearly the same instant. This is
 * different from IDF's aggregate per-region historical minimum. */
void memory_probe_sample(void);
void memory_probe_watermark(memory_watermark_t *watermark);
/* Call from the main task; never logs inside an allocator or SDK callback. */
void memory_probe_poll(void);
/* Serial task only. Detailed allocation records require a diagnostic build. */
void memory_probe_snapshot(void);
void memory_probe_init(void);
