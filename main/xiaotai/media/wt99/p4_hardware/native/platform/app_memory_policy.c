#include "app_memory_policy.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_memory_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_psram_alloc_failures;

_Static_assert(APP_MEMORY_INTERNAL_FREE_CRITICAL_BYTES <
                   APP_MEMORY_INTERNAL_FREE_WARNING_BYTES,
               "internal free waterlines must be ordered");
_Static_assert(APP_MEMORY_INTERNAL_LARGEST_CRITICAL_BYTES <
                   APP_MEMORY_INTERNAL_LARGEST_WARNING_BYTES,
               "internal largest-block waterlines must be ordered");
_Static_assert(APP_MEMORY_DMA_FREE_CRITICAL_BYTES <
                   APP_MEMORY_DMA_FREE_WARNING_BYTES,
               "DMA free waterlines must be ordered");
_Static_assert(APP_MEMORY_DMA_LARGEST_CRITICAL_BYTES <
                   APP_MEMORY_DMA_LARGEST_WARNING_BYTES,
               "DMA largest-block waterlines must be ordered");
_Static_assert(APP_MEMORY_PSRAM_FREE_CRITICAL_BYTES <
                   APP_MEMORY_PSRAM_FREE_WARNING_BYTES,
               "PSRAM free waterlines must be ordered");
_Static_assert(APP_MEMORY_PSRAM_LARGEST_CRITICAL_BYTES <
                   APP_MEMORY_PSRAM_LARGEST_WARNING_BYTES,
               "PSRAM largest-block waterlines must be ordered");

static void app_memory_note_psram_failure(void)
{
    taskENTER_CRITICAL(&s_memory_lock);
    s_psram_alloc_failures++;
    taskEXIT_CRITICAL(&s_memory_lock);
}

void *app_memory_alloc_psram(size_t size)
{
    void *ptr = NULL;

    if (size == 0U) {
        return NULL;
    }

    ptr = heap_caps_malloc(size, APP_MEMORY_CAPS_PSRAM);
    if (ptr == NULL) {
        app_memory_note_psram_failure();
    }
    return ptr;
}

void *app_memory_calloc_psram(size_t count, size_t size)
{
    void *ptr = NULL;

    if (count == 0U || size == 0U || count > (SIZE_MAX / size)) {
        return NULL;
    }

    ptr = heap_caps_calloc(count, size, APP_MEMORY_CAPS_PSRAM);
    if (ptr == NULL) {
        app_memory_note_psram_failure();
    }
    return ptr;
}

void *app_memory_aligned_alloc_psram(size_t alignment, size_t size, uint32_t extra_caps)
{
    void *ptr = NULL;

    if (alignment == 0U || size == 0U) {
        return NULL;
    }

    ptr = heap_caps_aligned_alloc(alignment,
                                  size,
                                  APP_MEMORY_CAPS_PSRAM | extra_caps);
    if (ptr == NULL) {
        app_memory_note_psram_failure();
    }
    return ptr;
}

void *app_memory_aligned_calloc_psram(size_t alignment,
                                      size_t count,
                                      size_t size,
                                      uint32_t extra_caps)
{
    void *ptr = NULL;

    if (alignment == 0U || count == 0U || size == 0U || count > (SIZE_MAX / size)) {
        return NULL;
    }

    ptr = heap_caps_aligned_calloc(alignment,
                                   count,
                                   size,
                                   APP_MEMORY_CAPS_PSRAM | extra_caps);
    if (ptr == NULL) {
        app_memory_note_psram_failure();
    }
    return ptr;
}

void app_memory_get_snapshot(app_memory_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->internal_free = heap_caps_get_free_size(APP_MEMORY_CAPS_CONTROL);
    snapshot->internal_largest = heap_caps_get_largest_free_block(APP_MEMORY_CAPS_CONTROL);
    snapshot->internal_min_free = heap_caps_get_minimum_free_size(APP_MEMORY_CAPS_CONTROL);
    snapshot->dma_free = heap_caps_get_free_size(APP_MEMORY_CAPS_DMA);
    snapshot->dma_largest = heap_caps_get_largest_free_block(APP_MEMORY_CAPS_DMA);
    snapshot->dma_min_free = heap_caps_get_minimum_free_size(APP_MEMORY_CAPS_DMA);
    snapshot->psram_free = heap_caps_get_free_size(APP_MEMORY_CAPS_PSRAM);
    snapshot->psram_largest = heap_caps_get_largest_free_block(APP_MEMORY_CAPS_PSRAM);
    snapshot->psram_min_free = heap_caps_get_minimum_free_size(APP_MEMORY_CAPS_PSRAM);

    taskENTER_CRITICAL(&s_memory_lock);
    snapshot->psram_alloc_failures = s_psram_alloc_failures;
    taskEXIT_CRITICAL(&s_memory_lock);
}

app_memory_health_t app_memory_classify(const app_memory_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return APP_MEMORY_HEALTH_CRITICAL;
    }

    if (snapshot->internal_free < APP_MEMORY_INTERNAL_FREE_CRITICAL_BYTES ||
        snapshot->internal_largest < APP_MEMORY_INTERNAL_LARGEST_CRITICAL_BYTES ||
        snapshot->dma_free < APP_MEMORY_DMA_FREE_CRITICAL_BYTES ||
        snapshot->dma_largest < APP_MEMORY_DMA_LARGEST_CRITICAL_BYTES ||
        snapshot->psram_free < APP_MEMORY_PSRAM_FREE_CRITICAL_BYTES ||
        snapshot->psram_largest < APP_MEMORY_PSRAM_LARGEST_CRITICAL_BYTES) {
        return APP_MEMORY_HEALTH_CRITICAL;
    }

    if (snapshot->internal_free < APP_MEMORY_INTERNAL_FREE_WARNING_BYTES ||
        snapshot->internal_largest < APP_MEMORY_INTERNAL_LARGEST_WARNING_BYTES ||
        snapshot->dma_free < APP_MEMORY_DMA_FREE_WARNING_BYTES ||
        snapshot->dma_largest < APP_MEMORY_DMA_LARGEST_WARNING_BYTES ||
        snapshot->psram_free < APP_MEMORY_PSRAM_FREE_WARNING_BYTES ||
        snapshot->psram_largest < APP_MEMORY_PSRAM_LARGEST_WARNING_BYTES) {
        return APP_MEMORY_HEALTH_WARNING;
    }

    return APP_MEMORY_HEALTH_NORMAL;
}

const char *app_memory_health_name(app_memory_health_t health)
{
    switch (health) {
    case APP_MEMORY_HEALTH_NORMAL:
        return "normal";
    case APP_MEMORY_HEALTH_WARNING:
        return "warning";
    case APP_MEMORY_HEALTH_CRITICAL:
        return "critical";
    default:
        return "unknown";
    }
}
