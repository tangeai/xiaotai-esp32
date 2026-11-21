// SPDX-License-Identifier: MIT
#include "led_io.h"
#include "driver/rmt_tx.h"
#include "soc/soc_caps.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <string.h>

static char channel, encoder;
static xiaotai_led_io_t *io;
static const uint8_t *borrowed;
static uint8_t sent[3];
static unsigned deletes, transmissions, waits;
static bool fail_init, fail_send, complete;

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *handle)
{
    assert(config->gpio_num == 5 && config->clk_src == RMT_CLK_SRC_DEFAULT && config->resolution_hz == 10000000);
    assert(config->mem_block_symbols == SOC_RMT_MEM_WORDS_PER_CHANNEL && config->trans_queue_depth == 1);
    *handle = &channel;
    return ESP_OK;
}

esp_err_t rmt_new_bytes_encoder(const rmt_bytes_encoder_config_t *config, rmt_encoder_handle_t *handle)
{
    assert(config->bit0.level0 == 1 && config->bit0.duration0 == 3 &&
           !config->bit0.level1 && config->bit0.duration1 == 9);
    assert(config->bit1.level0 == 1 && config->bit1.duration0 == 9 &&
           !config->bit1.level1 && config->bit1.duration1 == 3 && config->flags.msb_first);
    *handle = &encoder;
    return ESP_OK;
}

esp_err_t rmt_enable(rmt_channel_handle_t handle)
{
    assert(handle == &channel);
    return fail_init ? XIAOTAI_FAIL : ESP_OK;
}

esp_err_t rmt_del_encoder(rmt_encoder_handle_t handle)
{
    assert(handle == &encoder && !borrowed && deletes == 0);
    ++deletes;
    return ESP_OK;
}

esp_err_t rmt_del_channel(rmt_channel_handle_t handle)
{
    assert(handle == &channel && !borrowed && deletes == 1);
    ++deletes;
    return ESP_OK;
}

esp_err_t rmt_transmit(rmt_channel_handle_t h, rmt_encoder_handle_t e, const void *bytes, size_t len,
                       const rmt_transmit_config_t *cfg)
{
    assert(h == &channel && e == &encoder && !borrowed && len == sizeof(sent));
    assert(!cfg->loop_count && !cfg->flags.eot_level && cfg->flags.queue_nonblocking);
    ++transmissions;
    if (fail_send) return XIAOTAI_FAIL;
    borrowed = bytes;
    memcpy(sent, bytes, len);
    return ESP_OK;
}

esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t handle, int wait_ms)
{
    static const int expected[] = {10, 0, 0, 10};
    assert(handle == &channel && borrowed && waits < sizeof(expected) / sizeof(expected[0]));
    assert(wait_ms == expected[waits++] && memcmp(borrowed, sent, sizeof(sent)) == 0);
    if (!complete) return ESP_ERR_TIMEOUT;
    borrowed = NULL;
    return ESP_OK;
}

int main(void)
{
    fail_init = true;
    assert(xiaotai_led_io_create(30, &io) == XIAOTAI_ERR_INVALID_ARG && !io);
    assert(xiaotai_led_io_create(5, &io) == XIAOTAI_FAIL && !io && deletes == 2);
    assert(xiaotai_mem_host_outstanding() == 0);
    fail_init = false;
    assert(xiaotai_led_io_create(5, &io) == XIAOTAI_OK && io);
    uint8_t original[] = {1, 2, 3};
    assert(xiaotai_led_io_send(io, original, sizeof(original)) == ESP_ERR_TIMEOUT);
    assert(borrowed && borrowed != original && borrowed[0] == 1);
    original[0] = 4;
    assert(xiaotai_led_io_send(io, original, sizeof(original)) == XIAOTAI_ERR_BUSY);
    assert(transmissions == 1 && borrowed[0] == 1);
    complete = true;
    assert(xiaotai_led_io_send(io, original, sizeof(original)) == ESP_OK);
    assert(transmissions == 2 && !borrowed && sent[0] == 4);
    fail_send = true;
    assert(xiaotai_led_io_send(io, original, sizeof(original)) == XIAOTAI_FAIL);
    assert(transmissions == 3 && !borrowed && waits == 4);
    return 0;
}
