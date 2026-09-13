#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"

/*
 * P4 memory ownership:
 * - internal RAM is reserved for realtime control, cache-off code and DMA;
 * - large or long-lived payloads and background stacks are PSRAM-only;
 * - a failed PSRAM allocation must be handled by the owner instead of silently
 *   consuming the internal DMA reserve.
 */
#define APP_MEMORY_CAPS_CONTROL          (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define APP_MEMORY_CAPS_DMA              (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define APP_MEMORY_CAPS_PSRAM            (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define APP_TASK_STACK_CAPS_REALTIME     APP_MEMORY_CAPS_CONTROL
#define APP_TASK_STACK_CAPS_INTERNAL     APP_MEMORY_CAPS_CONTROL
#define APP_TASK_STACK_CAPS_CONTROL      APP_MEMORY_CAPS_CONTROL
#define APP_TASK_STACK_CAPS_BACKGROUND   APP_MEMORY_CAPS_PSRAM
#define APP_QUEUE_CAPS_CONTROL           APP_MEMORY_CAPS_CONTROL
#define APP_QUEUE_CAPS_BACKGROUND        APP_MEMORY_CAPS_PSRAM
#define APP_SYNC_CAPS_CONTROL            APP_MEMORY_CAPS_CONTROL

/* Early internal DMA reserve borrowed only by codec and media bootstrap. */
#define APP_MEMORY_DMA_ESCROW_BYTES       (96U * 1024U)

/*
 * These waterlines protect allocations that cannot fall back to PSRAM.  The
 * warning level leaves enough room to investigate before WHIP, DMA, or codec
 * setup reaches its hard allocation floor; the critical level is deliberately
 * close to the minimum required by those paths.
 */
#define APP_MEMORY_INTERNAL_FREE_WARNING_BYTES       (64U * 1024U)
#define APP_MEMORY_INTERNAL_FREE_CRITICAL_BYTES      (32U * 1024U)
#define APP_MEMORY_INTERNAL_LARGEST_WARNING_BYTES    (24U * 1024U)
#define APP_MEMORY_INTERNAL_LARGEST_CRITICAL_BYTES   (8U * 1024U)
#define APP_MEMORY_DMA_FREE_WARNING_BYTES            (48U * 1024U)
#define APP_MEMORY_DMA_FREE_CRITICAL_BYTES           (24U * 1024U)
#define APP_MEMORY_DMA_LARGEST_WARNING_BYTES         (16U * 1024U)
#define APP_MEMORY_DMA_LARGEST_CRITICAL_BYTES        (8U * 1024U)
#define APP_MEMORY_PSRAM_FREE_WARNING_BYTES          (4U * 1024U * 1024U)
#define APP_MEMORY_PSRAM_FREE_CRITICAL_BYTES         (2U * 1024U * 1024U)
#define APP_MEMORY_PSRAM_LARGEST_WARNING_BYTES       (2U * 1024U * 1024U)
#define APP_MEMORY_PSRAM_LARGEST_CRITICAL_BYTES      (1U * 1024U * 1024U)

typedef enum {
    APP_MEMORY_HEALTH_NORMAL = 0,
    APP_MEMORY_HEALTH_WARNING,
    APP_MEMORY_HEALTH_CRITICAL,
} app_memory_health_t;

typedef struct {
    size_t internal_free;
    size_t internal_largest;
    size_t internal_min_free;
    size_t dma_free;
    size_t dma_largest;
    size_t dma_min_free;
    size_t psram_free;
    size_t psram_largest;
    size_t psram_min_free;
    uint32_t psram_alloc_failures;
} app_memory_snapshot_t;

void *app_memory_alloc_psram(size_t size);
void *app_memory_calloc_psram(size_t count, size_t size);
void *app_memory_aligned_alloc_psram(size_t alignment, size_t size, uint32_t extra_caps);
void *app_memory_aligned_calloc_psram(size_t alignment,
                                      size_t count,
                                      size_t size,
                                      uint32_t extra_caps);
void app_memory_get_snapshot(app_memory_snapshot_t *snapshot);
app_memory_health_t app_memory_classify(const app_memory_snapshot_t *snapshot);
const char *app_memory_health_name(app_memory_health_t health);
