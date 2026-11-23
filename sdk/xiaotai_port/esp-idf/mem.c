// SPDX-License-Identifier: MIT
/* xiaotai/port/mem.h on ESP-IDF. BULK is PSRAM when the board has it, and then
 * never silently falls back to internal RAM: internal RAM is the scarce resource
 * and an exhausted PSRAM pool must show up as a failure, not as a starved stack. */
#include "xiaotai/port/mem.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP && defined(__riscv) && !defined(__riscv_atomic)
#define RTC_BULK 1
/* Both supported IDF lines use this no-notify primitive for preferred aligned
 * allocations. A failed RTC attempt must not trigger the firmware's OOM hook. */
extern void *heap_caps_aligned_alloc_base(size_t alignment, size_t size, uint32_t caps);
#endif

static uint32_t caps(xiaotai_mem_kind_t kind)
{
    switch (kind) {
    case XIAOTAI_MEM_BULK:
        return xiaotai_mem_has_external() ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                                          : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    case XIAOTAI_MEM_DMA:
        return MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    case XIAOTAI_MEM_INTERNAL:
    default:
        return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
}

void *xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    const uint32_t fallback = caps(kind);
#if RTC_BULK
    if (kind == XIAOTAI_MEM_BULK && !(fallback & MALLOC_CAP_SPIRAM))
        return heap_caps_malloc_prefer(size, 2, (uint32_t)MALLOC_CAP_RTCRAM, fallback);
#endif
    return heap_caps_malloc(size, fallback);
}

void *xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    const uint32_t fallback = caps(kind);
#if RTC_BULK
    if (kind == XIAOTAI_MEM_BULK && !(fallback & MALLOC_CAP_SPIRAM))
        return heap_caps_calloc_prefer(count, size, 2, (uint32_t)MALLOC_CAP_RTCRAM, fallback);
#endif
    return heap_caps_calloc(count, size, fallback);
}

void *xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size)
{
    const uint32_t fallback = caps(kind);
#if RTC_BULK
    if (kind == XIAOTAI_MEM_BULK && !(fallback & MALLOC_CAP_SPIRAM) &&
        alignment && !(alignment & (alignment - 1))) {
        void *p = heap_caps_aligned_alloc_base(alignment, size, MALLOC_CAP_RTCRAM);
        if (p) return p;
    }
#endif
    return heap_caps_aligned_alloc(alignment, size, fallback);
}

void xiaotai_mem_free(void *pointer) { heap_caps_free(pointer); }

bool xiaotai_mem_has_external(void) { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0; }

bool xiaotai_mem_is_external(const void *pointer) { return pointer && esp_ptr_external_ram(pointer); }

#define KINDS 3U

/* A sampled zero is a valid watermark. */
static size_t s_min_largest[KINDS];
static unsigned s_sampled;
static uint32_t s_samples;
static portMUX_TYPE s_watermark_lock = portMUX_INITIALIZER_UNLOCKED;

/* Lowers the watermark of kind to largest and returns the result. */
static size_t note_largest(xiaotai_mem_kind_t kind, size_t largest)
{
    const unsigned index = (unsigned)kind < KINDS ? (unsigned)kind : (unsigned)XIAOTAI_MEM_INTERNAL;
    portENTER_CRITICAL(&s_watermark_lock);
    if (!(s_sampled & (1U << index)) || largest < s_min_largest[index]) s_min_largest[index] = largest;
    s_sampled |= 1U << index;
    const size_t minimum = s_min_largest[index];
    portEXIT_CRITICAL(&s_watermark_lock);
    return minimum;
}

void xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    if (!stats) return;
    const uint32_t region = caps(kind);
    stats->total = heap_caps_get_total_size(region);
    stats->free = heap_caps_get_free_size(region);
    stats->minimum_free = heap_caps_get_minimum_free_size(region);
    stats->largest_block = heap_caps_get_largest_free_block(region);
    stats->minimum_largest_block = note_largest(kind, stats->largest_block);
    portENTER_CRITICAL(&s_watermark_lock);
    stats->samples = s_samples;
    portEXIT_CRITICAL(&s_watermark_lock);
}

void xiaotai_mem_sample(void)
{
    /* Heap walks happen outside the critical section; only the result is stored there. */
    for (unsigned kind = 0; kind < KINDS; ++kind) {
        (void)note_largest((xiaotai_mem_kind_t)kind, heap_caps_get_largest_free_block(caps((xiaotai_mem_kind_t)kind)));
    }
    portENTER_CRITICAL(&s_watermark_lock);
    if (s_samples != UINT32_MAX) ++s_samples;
    portEXIT_CRITICAL(&s_watermark_lock);
}

xiaotai_err_t xiaotai_mem_rtc_stats(xiaotai_mem_rtc_stats_t *stats)
{
    if (!stats) return XIAOTAI_ERR_INVALID_ARG;
#if defined(CONFIG_SOC_RTC_FAST_MEM_SUPPORTED) && CONFIG_SOC_RTC_FAST_MEM_SUPPORTED && defined(MALLOC_CAP_RTCRAM)
    *stats = (xiaotai_mem_rtc_stats_t){
        .free = heap_caps_get_free_size(MALLOC_CAP_RTCRAM),
        .largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM),
        .minimum_free = heap_caps_get_minimum_free_size(MALLOC_CAP_RTCRAM),
    };
    return XIAOTAI_OK;
#else
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}
