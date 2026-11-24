// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/heapcap_status_fixture.c
// Replaces legacy: tests/regression/test_voice_reliability.py::test_external_connect_memory
#include "../esp-idf/mem.c"
#include <assert.h>
#include <stdarg.h>

static bool psram, rtc_space;
static size_t internal_largest, dma_largest, external_largest;
static xiaotai_mem_rtc_stats_t rtc_reading;
static unsigned rtc_queries[3];
static uint32_t selected, prefer_first, prefer_second;
static unsigned int_value, rtc_value, ps_value;

#define INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define DMA (INTERNAL | MALLOC_CAP_DMA)
#define PS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

bool esp_ptr_external_ram(const void *p) { return p == &ps_value; }
size_t heap_caps_get_total_size(uint32_t region)
{
    if (region == MALLOC_CAP_SPIRAM) return psram ? 2048 : 0;
    return 1024;
}
size_t heap_caps_get_free_size(uint32_t region)
{
    if (region != MALLOC_CAP_RTCRAM) return heap_caps_get_largest_free_block(region) * 2;
    ++rtc_queries[0];
    return rtc_reading.free;
}
size_t heap_caps_get_minimum_free_size(uint32_t region)
{
    if (region != MALLOC_CAP_RTCRAM) return heap_caps_get_largest_free_block(region);
    ++rtc_queries[1];
    return rtc_reading.minimum_free;
}
size_t heap_caps_get_largest_free_block(uint32_t region)
{
    if (region == MALLOC_CAP_RTCRAM) {
        ++rtc_queries[2];
        return rtc_reading.largest_block;
    }
    return region == PS ? external_largest : region == DMA ? dma_largest : internal_largest;
}
void *heap_caps_malloc(size_t size, uint32_t region)
{
    assert(size);
    selected = region;
    if (region == MALLOC_CAP_RTCRAM) return rtc_space ? &rtc_value : NULL;
    return region == PS ? &ps_value : &int_value;
}
void *heap_caps_calloc(size_t n, size_t size, uint32_t region) { return heap_caps_malloc(n * size, region); }
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t region)
{
    assert(alignment == 32);
    return heap_caps_malloc(size, region);
}
void *heap_caps_aligned_alloc_base(size_t alignment, size_t size, uint32_t region)
{
    assert(region == MALLOC_CAP_RTCRAM);
    prefer_first = region;
    return heap_caps_aligned_alloc(alignment, size, region);
}
void *heap_caps_malloc_prefer(size_t size, size_t tries, ...)
{
    assert(tries == 2);
    va_list ap;
    va_start(ap, tries);
    prefer_first = va_arg(ap, uint32_t);
    prefer_second = va_arg(ap, uint32_t);
    va_end(ap);
    return heap_caps_malloc(size, rtc_space ? prefer_first : prefer_second);
}
void *heap_caps_calloc_prefer(size_t n, size_t size, size_t tries, ...)
{
    assert(tries == 2);
    va_list ap;
    va_start(ap, tries);
    uint32_t first = va_arg(ap, uint32_t), second = va_arg(ap, uint32_t);
    va_end(ap);
    return heap_caps_malloc_prefer(n * size, tries, first, second);
}
void heap_caps_free(void *p) { assert(p == &int_value || p == &rtc_value || p == &ps_value); }

int main(void)
{
    rtc_space = true;
#if RTC_BULK
    assert(xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 16) == &rtc_value && selected == MALLOC_CAP_RTCRAM);
    assert(prefer_first == MALLOC_CAP_RTCRAM && prefer_second == INTERNAL);
    assert(xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, 16) == &rtc_value);
    assert(xiaotai_mem_aligned(XIAOTAI_MEM_BULK, 32, 16) == &rtc_value);
#else
    assert(xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 16) == &int_value && selected == INTERNAL);
    assert(prefer_first == 0);
#endif
    rtc_space = false;
    assert(xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 16) == &int_value && selected == INTERNAL);
    assert(xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 2, 8) == &int_value && selected == INTERNAL);
    assert(xiaotai_mem_aligned(XIAOTAI_MEM_BULK, 32, 16) == &int_value && selected == INTERNAL);
    psram = true;
    prefer_first = 0;
    assert(xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 16) == &ps_value && selected == PS && !prefer_first);
    assert(xiaotai_mem_aligned(XIAOTAI_MEM_BULK, 32, 16) == &ps_value && selected == PS && !prefer_first);
    assert(xiaotai_mem_alloc(XIAOTAI_MEM_DMA, 8) == &int_value && selected == DMA);
    assert(xiaotai_mem_is_external(&ps_value) && !xiaotai_mem_is_external(&rtc_value));

    xiaotai_mem_stats_t st;
    external_largest = 100;
    internal_largest = 20;
    dma_largest = 7;
    xiaotai_mem_sample();
    xiaotai_mem_stats(XIAOTAI_MEM_BULK, &st);
    assert(st.samples == 1 && st.minimum_largest_block == 100);
    external_largest = 0;
    dma_largest = 5;
    xiaotai_mem_sample();
    external_largest = 56;
    dma_largest = 6;
    xiaotai_mem_stats(XIAOTAI_MEM_BULK, &st);
    assert(st.samples == 2 && st.largest_block == 56 && st.minimum_largest_block == 0);
    xiaotai_mem_stats(XIAOTAI_MEM_DMA, &st);
    assert(st.samples == 2 && st.largest_block == 6 && st.minimum_largest_block == 5);
    xiaotai_mem_stats(XIAOTAI_MEM_INTERNAL, &st);
    assert(st.samples == 2 && st.minimum_largest_block == 20);
    assert(!rtc_queries[0] && !rtc_queries[1] && !rtc_queries[2]);
    assert(xiaotai_mem_rtc_stats(NULL) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_mem_rtc_stats_t rtc = {.free = 7, .largest_block = 6, .minimum_free = 5};
#ifdef CONFIG_SOC_RTC_FAST_MEM_SUPPORTED
    const xiaotai_mem_rtc_stats_t vectors[] = {{3100, 3070, 3030}, {0}, {UINT32_MAX, UINT32_MAX, UINT32_MAX}};
    for (size_t i = 0; i < sizeof(vectors) / sizeof(*vectors); ++i) {
        rtc_reading = vectors[i];
        assert(xiaotai_mem_rtc_stats(&rtc) == XIAOTAI_OK);
        assert(rtc.free == rtc_reading.free && rtc.largest_block == rtc_reading.largest_block &&
               rtc.minimum_free == rtc_reading.minimum_free);
        assert(rtc_queries[0] == i + 1 && rtc_queries[1] == i + 1 && rtc_queries[2] == i + 1);
    }
#else
    assert(xiaotai_mem_rtc_stats(&rtc) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(rtc.free == 7 && rtc.largest_block == 6 && rtc.minimum_free == 5);
    assert(!rtc_queries[0] && !rtc_queries[1] && !rtc_queries[2]);
#endif
    xiaotai_mem_stats(XIAOTAI_MEM_INTERNAL, &st);
    assert(st.samples == 2 && st.minimum_largest_block == 20);
}
