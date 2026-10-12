/* CoreS3-SE power sequence follows M5Unified / M5GFX (MIT).
 * Audio codecs are owned by esp_codec_dev, not a second I2C/I2S driver. */
#include "starter_board.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_I2S_ISR_IRAM_SAFE
#error "CoreS3-SE observation pools are PSRAM; I2S callbacks require cache enabled"
#endif

enum { DMA_SAMPLES = 256, HISTORY = 16 };
typedef struct { int16_t pcm[DMA_SAMPLES * 4]; } capture_block_t;
static capture_block_t *s_capture;
static uint32_t s_write, s_read;
static size_t s_offset;
static TaskHandle_t s_reader;
static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static starter_board_audio_stats_t s_stats;
static i2c_master_dev_handle_t s_axp, s_aw;
static StaticSemaphore_t s_control_storage;
static SemaphoreHandle_t s_control;
static bool s_power_ready;

static esp_err_t write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(dev, data, sizeof(data), 100);
}

static esp_err_t update_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                            uint8_t mask, uint8_t value)
{
    uint8_t current;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &current, 1, 100);
    if (err == ESP_OK) err = write_reg(dev, reg, (current & ~mask) | (value & mask));
    return err;
}

static esp_err_t attach(i2c_master_bus_handle_t bus, uint8_t address,
                         i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address, .scl_speed_hz = 400000};
    return i2c_master_bus_add_device(bus, &cfg, out);
}

esp_err_t starter_board_init(i2c_master_bus_handle_t bus)
{
    if (s_power_ready) return ESP_OK;
    if (!bus) return ESP_ERR_INVALID_ARG;
    esp_err_t err = attach(bus, 0x34, &s_axp);
    if (err == ESP_OK) err = attach(bus, 0x58, &s_aw);
    if (err != ESP_OK) {
        if (s_axp) { i2c_master_bus_rm_device(s_axp); s_axp = NULL; }
        return err;
    }
    s_control = xSemaphoreCreateMutexStatic(&s_control_storage);
    /* Keep USB as a device: USB_OTG_EN and BUS_OUT_EN remain off. */
    static const uint8_t aw[][2] = {{0x02,0x05},{0x03,0x83},{0x04,0x18},
        {0x05,0x0c},{0x11,0x10},{0x12,0xff},{0x13,0xff}};
    static const uint8_t axp[][2] = {{0x92,13},{0x93,28},{0x95,28},
        {0x99,23},{0x90,0xab},{0x27,0x00},{0x69,0x11},{0x10,0x30},{0x30,0x0f}};
    for (size_t i=0; err == ESP_OK && i<sizeof(aw)/sizeof(aw[0]); ++i)
        err = write_reg(s_aw, aw[i][0], aw[i][1]);
    for (size_t i=0; err == ESP_OK && i<sizeof(axp)/sizeof(axp[0]); ++i)
        err = write_reg(s_axp, axp[i][0], axp[i][1]);
    if (err == ESP_OK) err = starter_board_lcd_reset();
    if (err == ESP_OK) s_power_ready = true;
    else {
        i2c_master_bus_rm_device(s_axp);
        i2c_master_bus_rm_device(s_aw);
        s_axp = s_aw = NULL;
        s_control = NULL;
    }
    return err;
}

esp_err_t starter_board_power_key(uint8_t *press)
{
    if (!press || !s_power_ready) return ESP_ERR_INVALID_STATE;
    const uint8_t reg = 0x49;
    uint8_t value = 0;
    xSemaphoreTake(s_control, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit_receive(s_axp, &reg, 1, &value, 1, 100);
    value &= 0x0c;
    if (err == ESP_OK && value) err = write_reg(s_axp, reg, value);
    xSemaphoreGive(s_control);
    *press = err == ESP_OK ? value >> 2 : 0;
    return err;
}

esp_err_t starter_board_lcd_reset(void)
{
    if (!s_control) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control, portMAX_DELAY);
    esp_err_t err = update_reg(s_aw, 0x03, 2, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (err == ESP_OK) err = update_reg(s_aw, 0x03, 2, 2);
    vTaskDelay(pdMS_TO_TICKS(120));
    xSemaphoreGive(s_control);
    return err;
}

esp_err_t starter_board_backlight(bool enabled)
{
    if (!s_control) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control, portMAX_DELAY);
    esp_err_t err = update_reg(s_axp, 0x90, 0x80, enabled ? 0x80 : 0);
    xSemaphoreGive(s_control);
    return err;
}

static bool rx_received(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel; (void)arg;
    portENTER_CRITICAL_ISR(&s_audio_lock);
    if (event->size != sizeof(s_capture[0].pcm)) {
        ++s_stats.invalid_dma_size;
        portEXIT_CRITICAL_ISR(&s_audio_lock);
        return false;
    }
    if (s_write - s_read < HISTORY) {
        capture_block_t *block = &s_capture[s_write % HISTORY];
        portEXIT_CRITICAL_ISR(&s_audio_lock);
        /* The single I2S ISR publishes the slot only after the copy is complete. */
        memcpy(block->pcm, event->dma_buf, sizeof(block->pcm));
        portENTER_CRITICAL_ISR(&s_audio_lock);
        ++s_write;
        ++s_stats.captured;
    } else ++s_stats.overflow;
    TaskHandle_t reader = s_reader;
    portEXIT_CRITICAL_ISR(&s_audio_lock);
    BaseType_t wake = pdFALSE;
    if (reader) vTaskNotifyGiveFromISR(reader, &wake);
    return wake == pdTRUE;
}

esp_err_t starter_board_audio_observe(i2s_chan_handle_t rx)
{
    if (s_capture) return ESP_ERR_INVALID_STATE;
    s_capture = heap_caps_calloc(HISTORY, sizeof(*s_capture), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_capture) return ESP_ERR_NO_MEM;
    i2s_event_callbacks_t rx_cb = {.on_recv = rx_received};
    esp_err_t err = i2s_channel_register_event_callback(rx, &rx_cb, NULL);
    if (err != ESP_OK) {
        heap_caps_free(s_capture);
        s_capture = NULL;
    }
    return err;
}

esp_err_t starter_board_capture(int16_t *out, size_t bytes)
{
    if (!out || bytes % 8 || !s_capture) return ESP_ERR_INVALID_ARG;
    size_t written = 0, count = bytes / 8;
    while (written < count) {
        portENTER_CRITICAL(&s_audio_lock);
        TaskHandle_t reader = xTaskGetCurrentTaskHandle();
        if (s_reader && s_reader != reader) {
            portEXIT_CRITICAL(&s_audio_lock);
            return ESP_ERR_INVALID_STATE;
        }
        s_reader = reader;
        if (s_write == s_read) {
            portEXIT_CRITICAL(&s_audio_lock);
            if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))) return ESP_ERR_TIMEOUT;
            continue;
        }
        capture_block_t *block = &s_capture[s_read % HISTORY];
        size_t offset = s_offset;
        size_t n = DMA_SAMPLES - offset;
        if (n > count-written) n = count-written;
        /* Retain s_read until all accesses finish, so the ISR cannot reuse this slot. */
        portEXIT_CRITICAL(&s_audio_lock);
        int16_t *samples = out + written * 4;
        memcpy(samples, block->pcm + offset * 4, n * 8);
        uint32_t peaks[4] = {0}, nonzero[4] = {0};
        for (size_t i=0; i<n; ++i) {
            for (unsigned channel = 0; channel < 4; ++channel) {
                int value = samples[i * 4 + channel];
                unsigned peak = value < 0 ? (unsigned)-value : (unsigned)value;
                if (peak > peaks[channel]) peaks[channel] = peak;
                if (value) ++nonzero[channel];
            }
        }
        portENTER_CRITICAL(&s_audio_lock);
        for (unsigned channel = 0; channel < 4; ++channel) {
            if (peaks[channel] > s_stats.peak[channel]) s_stats.peak[channel] = peaks[channel];
            s_stats.nonzero[channel] += nonzero[channel];
        }
        s_offset += n;
        if (s_offset == DMA_SAMPLES) { s_offset=0; ++s_read; }
        portEXIT_CRITICAL(&s_audio_lock);
        written += n;
    }
    return ESP_OK;
}

starter_board_audio_stats_t starter_board_audio_stats(void)
{
    portENTER_CRITICAL(&s_audio_lock);
    starter_board_audio_stats_t result = s_stats;
    portEXIT_CRITICAL(&s_audio_lock);
    return result;
}

esp_err_t starter_board_debug_power(void)
{
    if (!s_power_ready) return ESP_ERR_INVALID_STATE;
    const uint8_t axp_regs[] = {0x80, 0x82, 0x90, 0x92, 0x93, 0x99};
    const uint8_t aw_regs[] = {0x02, 0x03, 0x04, 0x05, 0x11};
    const uint8_t *regs[] = {axp_regs, aw_regs};
    const size_t count[] = {sizeof(axp_regs), sizeof(aw_regs)};
    const i2c_master_dev_handle_t dev[] = {s_axp, s_aw};
    xSemaphoreTake(s_control, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    for (unsigned d = 0; d < 2 && err == ESP_OK; ++d) {
        for (size_t i = 0; i < count[d] && err == ESP_OK; ++i) {
            uint8_t value = 0;
            err = i2c_master_transmit_receive(dev[d], &regs[d][i], 1, &value, 1, 100);
            if (err == ESP_OK) printf("+HW:%s,%02x=%02x\n", d ? "AW" : "AXP", regs[d][i], value);
        }
    }
    xSemaphoreGive(s_control);
    return err;
}
