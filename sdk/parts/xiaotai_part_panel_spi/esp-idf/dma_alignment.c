// SPDX-License-Identifier: MIT
#include "xiaotai/parts/panel_spi.h"

#ifdef XIAOTAI_SPI_EXTERNAL_DMA
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#endif

xiaotai_err_t xiaotai_panel_spi_external_dma_alignment(uint8_t *alignment)
{
    if (!alignment) return XIAOTAI_ERR_INVALID_ARG;
    *alignment = 0;
#ifdef XIAOTAI_SPI_EXTERNAL_DMA
    size_t bytes = 0;
    const xiaotai_err_t err = esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &bytes);
    if (err != XIAOTAI_OK) return err;
    if (!bytes || bytes > UINT8_MAX || (bytes & (bytes - 1U))) return XIAOTAI_ERR_INVALID_SIZE;
    *alignment = (uint8_t)bytes;
    return XIAOTAI_OK;
#else
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}
