#include "memory_probe.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>

static portMUX_TYPE s_watermark_lock = portMUX_INITIALIZER_UNLOCKED;
static memory_watermark_t s_watermark = {
    .lowest_free = SIZE_MAX,
    .largest_at_lowest_free = SIZE_MAX,
    .lowest_largest = SIZE_MAX,
};

void memory_probe_sample(void)
{
    size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    portENTER_CRITICAL(&s_watermark_lock);
    if (free < s_watermark.lowest_free) {
        s_watermark.lowest_free = free;
        s_watermark.largest_at_lowest_free = largest;
    }
    if (largest < s_watermark.lowest_largest)
        s_watermark.lowest_largest = largest;
    ++s_watermark.samples;
    portEXIT_CRITICAL(&s_watermark_lock);
}

void memory_probe_watermark(memory_watermark_t *watermark)
{
    if (!watermark)
        return;
    portENTER_CRITICAL(&s_watermark_lock);
    *watermark = s_watermark;
    portEXIT_CRITICAL(&s_watermark_lock);
}

#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#include <stdlib.h>
static heap_trace_record_t *s_trace;
static heap_trace_record_t s_disabled_record;

void memory_probe_init(void)
{
    const size_t records = 640;
    s_trace = calloc(records, sizeof(*s_trace));
    if (!s_trace) {
        ESP_LOGE("memory", "trace buffer allocation failed");
        return;
    }
    ESP_ERROR_CHECK(heap_trace_init_standalone(s_trace, records));
    ESP_LOGI("memory", "diagnostic trace buffer=%uB; released by memory command",
             (unsigned)(records * sizeof(*s_trace)));
    ESP_ERROR_CHECK(heap_trace_start(HEAP_TRACE_LEAKS));
}
#else
void memory_probe_init(void) {}
#endif

void memory_probe_snapshot(void)
{
    memory_probe_sample();
#if CONFIG_HEAP_TRACING_STANDALONE
    if (s_trace)
        ESP_ERROR_CHECK(heap_trace_stop());
#endif
    multi_heap_info_t heap;
    heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI("memory", "snapshot: free=%u largest=%u min_free=%u allocated=%u "
             "allocated_blocks=%u free_blocks=%u integrity=%d",
             (unsigned)heap.total_free_bytes, (unsigned)heap.largest_free_block,
             (unsigned)heap.minimum_free_bytes, (unsigned)heap.total_allocated_bytes,
             (unsigned)heap.allocated_blocks, (unsigned)heap.free_blocks,
             heap_caps_check_integrity_all(false));
    memory_watermark_t watermark;
    memory_probe_watermark(&watermark);
    ESP_LOGI("memory",
             "sampled watermark: lowest_free=%u largest_at_lowest_free=%u "
             "lowest_largest=%u samples=%u",
             (unsigned)watermark.lowest_free, (unsigned)watermark.largest_at_lowest_free,
             (unsigned)watermark.lowest_largest, watermark.samples);
#if CONFIG_HEAP_TRACING_STANDALONE
    if (s_trace) {
        heap_trace_summary_t summary;
        ESP_ERROR_CHECK(heap_trace_summary(&summary));
        ESP_LOGI("memory", "trace count=%u capacity=%u high=%u overflow=%u",
                 (unsigned)summary.count, (unsigned)summary.capacity,
                 (unsigned)summary.high_water_mark, (unsigned)summary.has_overflowed);
        for (size_t i = 0; i < summary.count; ++i) {
            heap_trace_record_t r;
            ESP_ERROR_CHECK(heap_trace_get(i, &r));
            printf("MEM_BLOCK ptr=%p size=%u", r.address, (unsigned)r.size);
            for (size_t j = 0; j < CONFIG_HEAP_TRACING_STACK_DEPTH; ++j)
                printf(" pc=%p", r.alloced_by[j]);
            printf("\n");
        }
        /* IDF 5.5.5 rejects NULL/0 despite the header's teardown example. Keep
         * tracing stopped and detach the large buffer before freeing it. */
        ESP_ERROR_CHECK(heap_trace_init_standalone(&s_disabled_record, 1));
        free(s_trace);
        s_trace = NULL;
        ESP_LOGI("memory", "trace buffer released; diagnostic code overhead remains");
    }
#endif
}
