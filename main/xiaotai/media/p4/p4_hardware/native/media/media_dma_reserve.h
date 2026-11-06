#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool reserved;
    size_t configured_bytes;
    size_t reserved_bytes;
    uint32_t release_count;
    uint32_t reclaim_count;
    uint32_t reserve_fail_count;
} media_dma_reserve_snapshot_t;

esp_err_t media_dma_reserve_init(void);
void media_dma_reserve_release(const char *reason);
esp_err_t media_dma_reserve_reclaim(const char *reason);
bool media_dma_reserve_is_reserved(void);
void media_dma_reserve_get_snapshot(media_dma_reserve_snapshot_t *snapshot);
