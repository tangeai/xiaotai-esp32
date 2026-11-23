// SPDX-License-Identifier: MIT
#include "led_io.h"
#include "xiaotai/port/mem.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "soc/soc_caps.h"
#include <string.h>

struct xiaotai_led_io {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    bool in_flight;
    uint8_t frame[XT_LED_BYTES_MAX];
};

xiaotai_err_t xiaotai_led_io_create(int pin, xiaotai_led_io_t **io)
{
    *io = NULL;
    if (pin < 0 || pin >= GPIO_NUM_MAX || !GPIO_IS_VALID_OUTPUT_GPIO(pin)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_led_io_t *rmt = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*rmt));
    if (!rmt) return XIAOTAI_ERR_NO_MEM;
    const rmt_tx_channel_config_t channel = {.gpio_num = pin, .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000U, .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL, .trans_queue_depth = 1};
    const rmt_bytes_encoder_config_t encoder = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3}, .flags.msb_first = 1};
    esp_err_t err = rmt_new_tx_channel(&channel, &rmt->channel);
    if (err == ESP_OK) err = rmt_new_bytes_encoder(&encoder, &rmt->encoder);
    if (err == ESP_OK) err = rmt_enable(rmt->channel);
    if (err != ESP_OK) {
        /* Construction has submitted no transaction. Successful parts and
         * their backing frames live until reset, as the HAL contract says. */
        if (rmt->encoder) (void)rmt_del_encoder(rmt->encoder);
        if (rmt->channel) (void)rmt_del_channel(rmt->channel);
        xiaotai_mem_free(rmt);
        return err;
    }
    *io = rmt;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_led_io_send(xiaotai_led_io_t *rmt, const uint8_t *data, size_t bytes)
{
    if (!rmt || !data || !bytes || bytes > sizeof(rmt->frame)) return XIAOTAI_ERR_INVALID_ARG;
    if (rmt->in_flight) {
        /* Timeout does not cancel an accepted frame. Do not touch the
         * borrowed bytes until the previous transaction actually retires. */
        const esp_err_t pending = rmt_tx_wait_all_done(rmt->channel, 0);
        if (pending != ESP_OK) return pending == ESP_ERR_TIMEOUT ? XIAOTAI_ERR_BUSY : pending;
        rmt->in_flight = false;
    }
    memcpy(rmt->frame, data, bytes);
    const rmt_transmit_config_t config = {.loop_count = 0, .flags = {.eot_level = 0, .queue_nonblocking = 1}};
    esp_err_t err = rmt_transmit(rmt->channel, rmt->encoder, rmt->frame, bytes, &config);
    if (err != ESP_OK) return err;
    rmt->in_flight = true;
    err = rmt_tx_wait_all_done(rmt->channel, XT_LED_WAIT_MS);
    if (err == ESP_OK) rmt->in_flight = false;
    return err;
}
