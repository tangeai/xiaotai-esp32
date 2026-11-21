// SPDX-License-Identifier: MIT
#include "native_spi_shim.h"
#include "spi_lcd_ownership.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum { FRAME_BYTES = 320 * 480 * 2, MAX_BYTES = 32767, CHUNKS = 10 };
static uint8_t frame[FRAME_BYTES];
int GPIO;
struct fake_spi {
    spi_device_interface_config_t cfg;
    spi_transaction_t *queued[20];
    unsigned count, submitted, received, empty_pops, polling, callbacks, removed;
    unsigned dma_fail_at, no_pop_at;
    bool remove_fails, acquired, exists;
    spi_transaction_t wire[20];
};

static struct fake_spi spi;

esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg,
                              spi_device_handle_t *out)
{
    assert(host == 1 && cfg->clock_speed_hz == 80000000 && cfg->mode == 0 && cfg->spics_io_num == 23);
    assert(!spi.exists);
    spi = (struct fake_spi){.cfg = *cfg, .exists = true};
    *out = &spi;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t p)
{
    assert(p == &spi && spi.exists && !spi.count && !spi.acquired);
    if (spi.remove_fails) return ESP_ERR_INVALID_STATE;
    ++spi.removed;
    spi.exists = false;
    return ESP_OK;
}

esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t host, size_t *out)
{
    assert(host == 1);
    *out = MAX_BYTES;
    return ESP_OK;
}

esp_err_t spi_device_acquire_bus(spi_device_handle_t p, uint32_t wait)
{
    assert(p == &spi && spi.exists && !spi.acquired && wait == portMAX_DELAY);
    spi.acquired = true;
    return ESP_OK;
}

void spi_device_release_bus(spi_device_handle_t p)
{
    assert(p == &spi && spi.exists && spi.acquired);
    spi.acquired = false;
}

esp_err_t spi_device_queue_trans(spi_device_handle_t p, spi_transaction_t *t, uint32_t wait)
{
    assert(p == &spi && spi.acquired && wait == portMAX_DELAY);
    assert(spi.count < (unsigned)spi.cfg.queue_size && t->length <= MAX_BYTES * 8);
    spi.queued[spi.count++] = t;
    assert(spi.submitted < sizeof(spi.wire) / sizeof(spi.wire[0]));
    spi.wire[spi.submitted++] = *t;
    return ESP_OK;
}

esp_err_t spi_device_get_trans_result(spi_device_handle_t p, spi_transaction_t **out, uint32_t wait)
{
    assert(p == &spi && spi.exists && wait == portMAX_DELAY);
    if (!spi.count) {
        ++spi.empty_pops;
        return ESP_ERR_TIMEOUT; /* A real empty queue would block forever. */
    }
    const unsigned serial = spi.received + 1;
    if (serial == spi.no_pop_at) {
        spi.no_pop_at = 0;
        return ESP_ERR_TIMEOUT; /* IDF leaves the supplied out pointer untouched. */
    }
    *out = spi.queued[0];
    --spi.count;
    memmove(spi.queued, spi.queued + 1, spi.count * sizeof(spi.queued[0]));
    ++spi.received;
    spi.cfg.pre_cb(*out);
    spi.cfg.post_cb(*out);
    return serial == spi.dma_fail_at ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t p, spi_transaction_t *t)
{
    assert(p == &spi && spi.acquired && !spi.count);
    if (t->tx_buffer && t->length == 8)
        assert(*(const uint8_t *)t->tx_buffer == 0x2c || *(const uint8_t *)t->tx_buffer == 0x01);
    if (t->rx_buffer) *(uint8_t *)t->rx_buffer = 0x29;
    ++spi.polling;
    return ESP_OK;
}

bool esp_ptr_external_ram(const void *p)
{
    return (uintptr_t)p >= (uintptr_t)frame && (uintptr_t)p < (uintptr_t)frame + sizeof(frame);
}

static bool complete(esp_lcd_panel_io_handle_t io, void *event, void *ctx)
{
    assert(io && !event && ctx == &spi);
    ++spi.callbacks;
    return false;
}

static esp_lcd_panel_io_handle_t create(int depth)
{
    const esp_lcd_panel_io_spi_config_t cfg = {
        .cs_gpio_num = 23, .dc_gpio_num = 26, .pclk_hz = 80000000, .spi_mode = 0,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .trans_queue_depth = depth,
        .on_color_trans_done = complete, .user_ctx = &spi, .flags.psram_dma_direct = true,
    };
    esp_lcd_panel_io_handle_t io = NULL;
#ifdef PRISTINE_DRIVER
    assert(esp_lcd_new_panel_io_spi(1, &cfg, &io) == ESP_OK);
#else
    assert(xiaotai_lcd_new_panel_io_spi(1, &cfg, &io) == ESP_OK);
#endif
    return io;
}

static void submit(esp_lcd_panel_io_handle_t io)
{
    assert(io->tx_color(io, 0x2c, frame, sizeof(frame)) == ESP_OK);
    assert(spi.count == CHUNKS && spi.polling == 1 && spi.received == 0 && spi.submitted == CHUNKS);
    size_t offset = 0;
    for (unsigned i = 0; i < CHUNKS; ++i) {
        const spi_transaction_t *wire = &spi.wire[i];
        assert(!!(wire->flags & SPI_TRANS_CS_KEEP_ACTIVE) == (i + 1 < CHUNKS));
        assert(wire->flags & SPI_TRANS_DMA_USE_PSRAM);
        assert(wire->tx_buffer == frame + offset);
        size_t bytes = i + 1 < CHUNKS ? MAX_BYTES : FRAME_BYTES - offset;
        assert(wire->length == bytes * 8);
        offset += bytes;
    }
    assert(offset == FRAME_BYTES);
}

#ifdef PRISTINE_DRIVER
int main(void)
{
    esp_lcd_panel_io_handle_t io = create(20);
    submit(io);
    spi.dma_fail_at = 4;
    assert(io->tx_param(io, -1, NULL, 0) == ESP_ERR_INVALID_STATE && spi.received == 4);
    assert(io->tx_param(io, -1, NULL, 0) == ESP_ERR_TIMEOUT && spi.count == 0 && spi.empty_pops == 1);
    /* The pinned old object cannot delete itself; release fixture memory. */
    free(io);
    return 0;
}
#else
static void popped_failure(unsigned where)
{
    esp_lcd_panel_io_handle_t io = create(20);
    submit(io);
    spi.dma_fail_at = where;
    assert(io->tx_param(io, -1, NULL, 0) == ESP_ERR_INVALID_STATE);
    assert(!spi.count && !spi.empty_pops && spi.received == CHUNKS && spi.callbacks == 1 && spi.polling == 1);
    assert(io->tx_param(io, -1, NULL, 0) == ESP_OK && !spi.empty_pops);
    assert(io->del(io) == ESP_OK && spi.removed == 1);
}

static void no_pop_retains_owner(void)
{
    esp_lcd_panel_io_handle_t io = create(20);
    submit(io);
    spi.no_pop_at = 1;
    assert(io->del(io) == ESP_ERR_TIMEOUT && spi.count == CHUNKS && !spi.received && !spi.removed);
    spi.no_pop_at = 4;
    spi.dma_fail_at = 2;
    assert(io->tx_param(io, -1, NULL, 0) == ESP_ERR_INVALID_STATE); /* first fault even if later no pop */
    assert(spi.count == CHUNKS - 3 && !spi.removed && !spi.empty_pops);
    spi.remove_fails = true;
    assert(io->del(io) == ESP_ERR_INVALID_STATE && !spi.count && !spi.removed && !spi.empty_pops);
    spi.remove_fails = false;
    assert(io->del(io) == ESP_OK && spi.removed == 1);
}

static void queue_full_and_read(void)
{
    esp_lcd_panel_io_handle_t io = create(3);
    spi.dma_fail_at = 1;
    assert(io->tx_color(io, 0x2c, frame, sizeof(frame)) == ESP_ERR_INVALID_STATE);
    assert(spi.submitted == 3 && spi.received == 1 && spi.count == 2 && spi.callbacks == 0);
    spi.dma_fail_at = 2;
    uint8_t read = 0;
    assert(io->rx_param(io, 0x01, &read, 1) == ESP_ERR_INVALID_STATE);
    assert(!spi.count && spi.received == 3 && !spi.empty_pops && spi.polling == 1 && !read);
    assert(io->rx_param(io, 0x01, &read, 1) == ESP_OK && read == 0x29 && spi.polling == 3);
    assert(io->del(io) == ESP_OK && spi.removed == 1);

    io = create(20);
    submit(io);
    spi.dma_fail_at = 3;
    assert(io->tx_color(io, 0x2c, frame, 16) == ESP_ERR_INVALID_STATE);
    assert(!spi.count && spi.polling == 1 && spi.submitted == CHUNKS && !spi.empty_pops);
    assert(io->tx_color(io, 0x2c, frame, 16) == ESP_OK);
    assert(io->del(io) == ESP_OK && spi.removed == 1);
}

int main(void)
{
    popped_failure(1);
    popped_failure(5);
    popped_failure(10);
    no_pop_retains_owner();
    queue_full_and_read();
    return 0;
}
#endif
