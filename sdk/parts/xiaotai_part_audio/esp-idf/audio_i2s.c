// SPDX-License-Identifier: MIT
#include "audio_i2s_policy.h"
#include "codec_native.h"
#include "xiaotai/port/os.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "soc/soc_caps.h"
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#endif
#ifndef SOC_I2S_NUM
#include "hal/i2s_ll.h"
#endif
#include <stdio.h>
#include <string.h>

enum { IO_TIMEOUT_MS = 1000, MAX_DMA_BYTES = 4092 };

struct xt_audio_io {
    i2s_chan_handle_t rx, tx;
    xt_audio_overflow_fn overflow;
    xt_audio_rx_block_fn receive;
    void *ctx;
    uint8_t rx_channels, rx_slots, tx_slots, dma_descriptors;
    uint16_t dma_frames, mclk_multiple;
    uint8_t *rx_work, *tx_work;
};

static unsigned std_slot_width(const xiaotai_i2s_config_t *c, bool duplex)
{
    return duplex && c->rx_slots > 2 ? c->rx_slots * 16U / 2U : 16U;
}

xiaotai_err_t xt_audio_io_describe(xiaotai_i2s_config_t *c, uint8_t rx, uint8_t tx)
{
#ifdef SOC_I2S_NUM
    const unsigned ports = SOC_I2S_NUM;
#else
    const unsigned ports = I2S_LL_GET(INST_NUM);
#endif
    const bool pdm_rx = rx && c->rx_mode == XIAOTAI_I2S_PDM;
    const bool pdm_tx = tx && c->tx_mode == XIAOTAI_I2S_PDM;
    if ((unsigned)c->port >= ports || ((pdm_rx || pdm_tx) && (c->port != I2S_NUM_0 || (rx && tx))))
        return XIAOTAI_ERR_NOT_SUPPORTED;
#if !SOC_I2S_SUPPORTS_PDM_RX || !SOC_I2S_SUPPORTS_PDM2PCM
    if (pdm_rx) return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
#if !SOC_I2S_SUPPORTS_PDM_TX || !SOC_I2S_SUPPORTS_PCM2PDM
    if (pdm_tx) return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
#if !SOC_I2S_SUPPORTS_TDM
    if (rx && c->rx_slots > 2) return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
    const unsigned width = std_slot_width(c, rx && tx);
    if (width != 16U && width != 24U && width != 32U) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!pdm_rx && !pdm_tx) {
        const unsigned mclk = c->mclk_multiple ? c->mclk_multiple : width == 24U ? 384U : 256U;
        const unsigned wire_bits = (rx ? c->rx_slots : c->tx_slots) * 16U;
        if (mclk % wire_bits != 0) return XIAOTAI_ERR_NOT_SUPPORTED;
        c->mclk_multiple = (uint16_t)mclk;
    }
    const i2s_chan_config_t defaults = I2S_CHANNEL_DEFAULT_CONFIG(c->port, I2S_ROLE_MASTER);
    if (!c->dma_descriptors) c->dma_descriptors = defaults.dma_desc_num;
    const unsigned rx_bytes = rx ? c->rx_slots * sizeof(int16_t) : 0;
    const unsigned tx_bytes = tx ? c->tx_slots * sizeof(int16_t) : 0;
    const unsigned widest = rx_bytes > tx_bytes ? rx_bytes : tx_bytes;
    unsigned frames = c->dma_frames ? c->dma_frames : defaults.dma_frame_num;
    if (!widest || c->dma_descriptors < 2 || frames > MAX_DMA_BYTES / widest)
        return XIAOTAI_ERR_INVALID_ARG;
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
    /* The IDF driver would otherwise round each descriptor independently.
     * The capture ring and HAL report one frame count for both directions. */
    const uint32_t line = cache_hal_get_cache_line_size(CACHE_LL_LEVEL_INT_MEM, CACHE_TYPE_DATA);
    if (!line) return XIAOTAI_ERR_INVALID_STATE;
    while ((frames * rx_bytes) % line || (frames * tx_bytes) % line) {
        if (c->dma_frames || ++frames > MAX_DMA_BYTES / widest) return XIAOTAI_ERR_INVALID_ARG;
    }
#endif
    c->dma_frames = (uint16_t)frames;
    return XIAOTAI_OK;
}

static xiaotai_err_t std_init(i2s_chan_handle_t ch, const xiaotai_i2s_config_t *c, bool tx, unsigned width)
{
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(c->rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = c->mclk_pin, .bclk = c->bclk_pin, .ws = c->ws_pin,
                     .din = tx ? I2S_GPIO_UNUSED : c->din_pin, .dout = tx ? c->dout_pin : I2S_GPIO_UNUSED},
    };
    /* Paired STD TX and TDM RX share BCLK. Narrow PCM samples remain 16-bit
     * even when a stereo TX slot must be widened to match the RX frame. */
    cfg.slot_cfg.slot_bit_width = (i2s_slot_bit_width_t)width;
    cfg.slot_cfg.ws_width = width;
    if (c->mclk_multiple) cfg.clk_cfg.mclk_multiple = c->mclk_multiple;
    return i2s_channel_init_std_mode(ch, &cfg);
}

static xiaotai_err_t rx_init(i2s_chan_handle_t ch, const xiaotai_i2s_config_t *c)
{
    if (c->rx_mode == XIAOTAI_I2S_SLOTS && c->rx_slots == 2) return std_init(ch, c, false, 16);
#if SOC_I2S_SUPPORTS_PDM_RX && SOC_I2S_SUPPORTS_PDM2PCM
    if (c->rx_mode == XIAOTAI_I2S_PDM) {
        const i2s_slot_mode_t mode = c->rx_slots == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
        i2s_pdm_rx_config_t cfg = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(c->rate_hz),
            .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, mode),
            .gpio_cfg = {.clk = c->ws_pin, .din = c->din_pin},
        };
        if (c->mclk_multiple) cfg.clk_cfg.mclk_multiple = c->mclk_multiple;
        return i2s_channel_init_pdm_rx_mode(ch, &cfg);
    }
#endif
#if SOC_I2S_SUPPORTS_TDM
    if (c->rx_mode == XIAOTAI_I2S_SLOTS) {
        i2s_tdm_config_t cfg = {
            .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(c->rate_hz),
            .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                           (1U << c->rx_slots) - 1U),
            .gpio_cfg = {.mclk = c->mclk_pin, .bclk = c->bclk_pin, .ws = c->ws_pin,
                         .din = c->din_pin, .dout = I2S_GPIO_UNUSED},
        };
        cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
        cfg.slot_cfg.total_slot = c->rx_slots;
        cfg.slot_cfg.ws_width = c->rx_slots * 8U;
        cfg.slot_cfg.left_align = true;
        if (c->mclk_multiple) cfg.clk_cfg.mclk_multiple = c->mclk_multiple;
        return i2s_channel_init_tdm_mode(ch, &cfg);
    }
#endif
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t tx_init(i2s_chan_handle_t ch, const xiaotai_i2s_config_t *c, bool duplex)
{
    if (c->tx_mode == XIAOTAI_I2S_SLOTS) return std_init(ch, c, true, std_slot_width(c, duplex));
#if SOC_I2S_SUPPORTS_PDM_TX && SOC_I2S_SUPPORTS_PCM2PDM
    const i2s_slot_mode_t mode = c->tx_slots == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    i2s_pdm_tx_config_t cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(c->rate_hz),
        .slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, mode),
        .gpio_cfg = {.clk = c->ws_pin, .dout = c->dout_pin},
    };
    if (c->mclk_multiple) cfg.clk_cfg.mclk_multiple = c->mclk_multiple;
    return i2s_channel_init_pdm_tx_mode(ch, &cfg);
#else
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}

static bool on_recv(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel;
    xt_audio_io_t *io = arg;
    io->receive(io->ctx, event->dma_buf, event->size);
    return false;  /* The port's ISR signal requests a yield when it wakes a task. */
}

static bool on_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel;
    (void)event;
    xt_audio_io_t *io = arg;
    io->overflow(io->ctx);
    return false;
}

uintptr_t xt_audio_io_reader_id(void) { return (uintptr_t)xTaskGetCurrentTaskHandle(); }

uint8_t xt_audio_io_wire_slots(const xt_audio_io_t *io, xt_audio_direction_t direction)
{
    return direction == XT_AUDIO_INPUT ? io->rx_slots : io->tx_slots;
}

uint16_t xt_audio_io_mclk(const xt_audio_io_t *io) { return io->mclk_multiple; }

unsigned xt_audio_io_bit_clocks(const xt_audio_io_t *io)
{
    return (io->rx_slots > io->tx_slots ? io->rx_slots : io->tx_slots) * 16U;
}

/* Query the real channel state: this is the same idempotency rule as the
 * pinned vendor data-layer patch, without a second owner reformatting I2S. */
static esp_err_t enable(i2s_chan_handle_t channel, bool value)
{
    i2s_chan_info_t info;
    const esp_err_t err = i2s_channel_get_info(channel, &info);
    if (err != ESP_OK || info.is_enabled == value) return err;
    return value ? i2s_channel_enable(channel) : i2s_channel_disable(channel);
}

xiaotai_err_t xt_audio_io_start(const xiaotai_i2s_config_t *c, uint8_t rx, uint8_t tx,
                                xt_audio_overflow_fn overflow, xt_audio_rx_block_fn receive,
                                void *ctx, xt_audio_io_t **out)
{
    *out = NULL;
    xt_audio_io_t *io = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*io));
    if (!io) return XIAOTAI_ERR_NO_MEM;
    *out = io;
    io->rx_channels = rx;
    io->rx_slots = rx ? c->rx_slots : 0;
    io->tx_slots = tx ? c->tx_slots : 0;
    io->dma_frames = c->dma_frames;
    io->mclk_multiple = c->mclk_multiple;
    io->dma_descriptors = c->dma_descriptors;
    io->overflow = overflow;
    io->receive = receive;
    io->ctx = ctx;
    if (rx && !receive && rx != io->rx_slots) {
        io->rx_work = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (size_t)io->rx_slots * io->dma_frames * sizeof(int16_t));
        if (!io->rx_work) return XIAOTAI_ERR_NO_MEM;
    }
    if (tx && tx != io->tx_slots) {
        io->tx_work = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (size_t)io->tx_slots * io->dma_frames * sizeof(int16_t));
        if (!io->tx_work) return XIAOTAI_ERR_NO_MEM;
    }
    i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(c->port, I2S_ROLE_MASTER);
    config.auto_clear_after_cb = true;
    config.dma_desc_num = c->dma_descriptors;
    config.dma_frame_num = c->dma_frames;
    esp_err_t err = i2s_new_channel(&config, tx ? &io->tx : NULL, rx ? &io->rx : NULL);
    if (err == ESP_OK && tx) err = tx_init(io->tx, c, rx != 0);
    if (err == ESP_OK && rx) err = rx_init(io->rx, c);
    if (err == ESP_OK && rx && (receive || overflow)) {
        const i2s_event_callbacks_t events = {.on_recv = receive ? on_recv : NULL,
                                              .on_recv_q_ovf = overflow ? on_overflow : NULL};
        err = i2s_channel_register_event_callback(io->rx, &events, io);
    }
    if (err == ESP_OK && tx) err = enable(io->tx, true);
    if (err == ESP_OK && rx) err = enable(io->rx, true);
    return err;
}

xiaotai_err_t xt_audio_io_pause(xt_audio_io_t *io)
{
    const esp_err_t rx = io->rx ? enable(io->rx, false) : ESP_OK;
    const esp_err_t tx = io->tx ? enable(io->tx, false) : ESP_OK;
    return rx != ESP_OK ? rx : tx;
}

static esp_err_t clear_tx(i2s_chan_handle_t channel)
{
    i2s_chan_info_t info;
    const esp_err_t err = i2s_channel_get_info(channel, &info);
    if (err != ESP_OK) return err;
    if (info.is_enabled || !info.total_dma_buf_size) return ESP_ERR_INVALID_STATE;

    /* Disable resets the write cursor, not stale samples left by cancellation. */
    static const uint8_t silence[64] = {0};
    size_t left = info.total_dma_buf_size;
    while (left) {
        const size_t chunk = left < sizeof(silence) ? left : sizeof(silence);
        size_t loaded = 0;
        const esp_err_t result = i2s_channel_preload_data(channel, silence, chunk, &loaded);
        if (result != ESP_OK) return result;
        if (loaded != chunk) return ESP_ERR_INVALID_STATE;
        left -= loaded;
    }
    return ESP_OK;
}

xiaotai_err_t xt_audio_io_resume(xt_audio_io_t *io)
{
    esp_err_t err = io->tx ? clear_tx(io->tx) : ESP_OK;
    if (err == ESP_OK && io->tx) err = enable(io->tx, true);
    if (err == ESP_OK && io->rx) err = enable(io->rx, true);
    return err;
}

static esp_err_t stop(i2s_chan_handle_t *channel)
{
    if (!*channel) return ESP_OK;
    esp_err_t err = enable(*channel, false);
    if (err == ESP_OK) err = i2s_del_channel(*channel);
    if (err == ESP_OK) *channel = NULL;
    return err;
}

xiaotai_err_t xt_audio_io_stop(xt_audio_io_t *io)
{
    const esp_err_t rx = stop(&io->rx);
    const esp_err_t tx = stop(&io->tx);
    if (rx != ESP_OK || tx != ESP_OK) return rx != ESP_OK ? rx : tx;
    xiaotai_mem_free(io->rx_work);
    xiaotai_mem_free(io->tx_work);
    xiaotai_mem_free(io);
    return XIAOTAI_OK;
}

xiaotai_err_t xt_audio_io_discard_input(xt_audio_io_t *io)
{
    uint8_t scratch[128];
    if (!io->rx || io->receive) return XIAOTAI_ERR_INVALID_STATE;
    const size_t frame_bytes = (size_t)io->rx_slots * sizeof(int16_t);
    const size_t max = sizeof(scratch) / frame_bytes * frame_bytes;
    size_t pending = (size_t)(io->dma_descriptors + 1U) * io->dma_frames * frame_bytes;
    while (pending) {
        size_t moved;
        const size_t chunk = pending < max ? pending : max;
        const esp_err_t err = i2s_channel_read(io->rx, scratch, chunk, &moved, 0);
        if (err == ESP_ERR_TIMEOUT) return XIAOTAI_OK; /* A poll consumed all ready frames. */
        if (err != ESP_OK) return err;
        if (moved != chunk) return XIAOTAI_FAIL;
        pending -= moved;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xt_audio_io_read(xt_audio_io_t *io, void *data, size_t capacity, size_t *size)
{
    *size = 0;
    if (!io->rx || io->receive) return XIAOTAI_ERR_INVALID_STATE;
    if (!io->rx_work) return i2s_channel_read(io->rx, data, capacity, size, IO_TIMEOUT_MS);
    const size_t raw = (size_t)io->rx_slots * sizeof(int16_t);
    const size_t logical = (size_t)io->rx_channels * sizeof(int16_t);
    while (*size < capacity) {
        size_t n = (capacity - *size) / logical, moved;
        if (n > io->dma_frames) n = io->dma_frames;
        const esp_err_t err = i2s_channel_read(io->rx, io->rx_work, n * raw, &moved, IO_TIMEOUT_MS);
        if (err != ESP_OK) return err;
        if (moved != n * raw) return XIAOTAI_FAIL;
        for (size_t f = 0; f < n; ++f)
            memcpy((uint8_t *)data + *size + f * logical, io->rx_work + f * raw, logical);
        *size += n * logical;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xt_audio_io_write(xt_audio_io_t *io, const void *data, size_t size, size_t *written)
{
    *written = 0;
    if (!io->tx) return XIAOTAI_ERR_INVALID_STATE;
    if (!io->tx_work) return i2s_channel_write(io->tx, data, size, written, IO_TIMEOUT_MS);
    const size_t frame = (size_t)io->tx_slots * sizeof(int16_t);
    while (*written < size) {
        size_t n = (size - *written) / sizeof(int16_t), moved;
        if (n > io->dma_frames) n = io->dma_frames;
        for (size_t f = 0; f < n; ++f) {
            const uint8_t *sample = (const uint8_t *)data + *written + f * sizeof(int16_t);
            memcpy(io->tx_work + f * frame, sample, sizeof(int16_t));
            memcpy(io->tx_work + f * frame + sizeof(int16_t), sample, sizeof(int16_t));
        }
        const esp_err_t err = i2s_channel_write(io->tx, io->tx_work, n * frame, &moved, IO_TIMEOUT_MS);
        if (err != ESP_OK) return err;
        if (moved != n * frame) return XIAOTAI_FAIL;
        *written += n * sizeof(int16_t);
    }
    return XIAOTAI_OK;
}

void xt_audio_io_report(const xiaotai_i2s_config_t *c, bool running, xiaotai_hal_write_fn_t write, void *ctx)
{
    char line[96];
    if (running) (void)snprintf(line, sizeof(line), "i2s=%d running", c->port);
    else (void)snprintf(line, sizeof(line), "i2s=%d stopped mclk=%d bclk=%d ws=%d din=%d dout=%d", c->port,
        c->mclk_pin < 0 ? -1 : gpio_get_level(c->mclk_pin), c->bclk_pin < 0 ? -1 : gpio_get_level(c->bclk_pin),
        c->ws_pin < 0 ? -1 : gpio_get_level(c->ws_pin), c->din_pin < 0 ? -1 : gpio_get_level(c->din_pin),
        c->dout_pin < 0 ? -1 : gpio_get_level(c->dout_pin));
    write(ctx, line);
}
