// SPDX-License-Identifier: MIT
/* IDF may return a completed descriptor and report a DMA error in the same
 * call. The descriptor, not the status, determines who owns it. */
static esp_err_t xiaotai_spi_reap(esp_lcd_panel_io_spi_t *io, spi_transaction_t **completed)
{
    *completed = NULL;
    const esp_err_t status = spi_device_get_trans_result(io->spi_dev, completed, portMAX_DELAY);
    if (*completed) {
        io->num_trans_inflight--;
        return status;
    }
    return status == ESP_OK ? ESP_ERR_INVALID_STATE : status;
}

static esp_err_t xiaotai_spi_drain(esp_lcd_panel_io_spi_t *io)
{
    esp_err_t first = ESP_OK;
    while (io->num_trans_inflight) {
        spi_transaction_t *completed;
        const esp_err_t status = xiaotai_spi_reap(io, &completed);
        if (first == ESP_OK && status != ESP_OK) first = status;
        if (!completed) break;
    }
    return first;
}
