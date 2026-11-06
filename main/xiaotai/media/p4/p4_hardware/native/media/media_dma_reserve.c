#include "media_dma_reserve.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "app_memory_policy.h"

static const char *TAG = "media_dma";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_escrow;
static size_t s_escrow_size;
static uint32_t s_release_count;
static uint32_t s_reclaim_count;
static uint32_t s_reserve_fail_count;

static void media_dma_reserve_log(const char *action,
                                  const char *reason,
                                  size_t bytes,
                                  esp_err_t ret)
{
    ESP_LOGI(TAG,
             "dma escrow: action=%s reason=%s ret=%s bytes=%u configured=%u reserved=%d internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u releases=%lu reclaims=%lu fail=%lu",
             action != NULL ? action : "unknown",
             reason != NULL ? reason : "unknown",
             esp_err_to_name(ret),
             (unsigned)bytes,
             (unsigned)APP_MEMORY_DMA_ESCROW_BYTES,
             s_escrow != NULL ? 1 : 0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned long)s_release_count,
             (unsigned long)s_reclaim_count,
             (unsigned long)s_reserve_fail_count);
}

esp_err_t media_dma_reserve_init(void)
{
    return media_dma_reserve_reclaim("init");
}

void media_dma_reserve_release(const char *reason)
{
    uint8_t *ptr = NULL;
    size_t bytes = 0;

    taskENTER_CRITICAL(&s_lock);
    ptr = s_escrow;
    bytes = s_escrow_size;
    s_escrow = NULL;
    s_escrow_size = 0;
    if (ptr != NULL) {
        s_release_count++;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (ptr == NULL) {
        media_dma_reserve_log("release-skip", reason, 0, ESP_ERR_NOT_FOUND);
        return;
    }

    heap_caps_free(ptr);
    media_dma_reserve_log("release", reason, bytes, ESP_OK);
}

esp_err_t media_dma_reserve_reclaim(const char *reason)
{
    uint8_t *ptr = NULL;
    size_t bytes = APP_MEMORY_DMA_ESCROW_BYTES;

    if (bytes == 0U) {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_lock);
    bool already_reserved = s_escrow != NULL;
    taskEXIT_CRITICAL(&s_lock);
    if (already_reserved) {
        media_dma_reserve_log("reclaim-skip", reason, bytes, ESP_OK);
        return ESP_OK;
    }

    ptr = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        taskENTER_CRITICAL(&s_lock);
        s_reserve_fail_count++;
        taskEXIT_CRITICAL(&s_lock);
        media_dma_reserve_log("reclaim-failed", reason, bytes, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_escrow == NULL) {
        s_escrow = ptr;
        s_escrow_size = bytes;
        s_reclaim_count++;
        ptr = NULL;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (ptr != NULL) {
        heap_caps_free(ptr);
        media_dma_reserve_log("reclaim-race", reason, bytes, ESP_OK);
        return ESP_OK;
    }

    media_dma_reserve_log("reclaim", reason, bytes, ESP_OK);
    return ESP_OK;
}

bool media_dma_reserve_is_reserved(void)
{
    bool reserved = false;

    taskENTER_CRITICAL(&s_lock);
    reserved = s_escrow != NULL;
    taskEXIT_CRITICAL(&s_lock);
    return reserved;
}

void media_dma_reserve_get_snapshot(media_dma_reserve_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    *snapshot = (media_dma_reserve_snapshot_t) {
        .reserved = s_escrow != NULL,
        .configured_bytes = APP_MEMORY_DMA_ESCROW_BYTES,
        .reserved_bytes = s_escrow_size,
        .release_count = s_release_count,
        .reclaim_count = s_reclaim_count,
        .reserve_fail_count = s_reserve_fail_count,
    };
    taskEXIT_CRITICAL(&s_lock);
}
