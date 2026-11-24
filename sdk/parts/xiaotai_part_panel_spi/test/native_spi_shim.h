// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef int gpio_num_t;
typedef int esp_lcd_spi_bus_handle_t;
typedef int spi_host_device_t;
typedef struct fake_spi *spi_device_handle_t;
enum { ESP_OK = 0, ESP_ERR_NO_MEM = 0x101, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
       ESP_ERR_NOT_SUPPORTED = 0x106, ESP_ERR_TIMEOUT };
enum {
    SPI_DEVICE_HALFDUPLEX = 1, SPI_DEVICE_TXBIT_LSBFIRST = 2, SPI_DEVICE_3WIRE = 4, SPI_DEVICE_POSITIVE_CS = 8,
    SPI_TRANS_CS_KEEP_ACTIVE = 16, SPI_TRANS_DMA_USE_PSRAM = 32, SPI_TRANS_MULTILINE_CMD = 64,
    SPI_TRANS_MULTILINE_ADDR = 128, SPI_TRANS_MODE_OCT = 256, SPI_TRANS_MODE_QIO = 512,
};

#define portMAX_DELAY UINT32_MAX
#define IRAM_ATTR
#define PIN_FUNC_GPIO 0
#define __containerof(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_GOTO_ON_FALSE(c, e, label, ...) do { if (!(c)) { ret = (e); goto label; } } while (0)
#define ESP_GOTO_ON_ERROR(status, label, ...) do { if ((status) != ESP_OK) goto label; } while (0)
#define ESP_RETURN_ON_ERROR(status, ...) do { const esp_err_t err_ = (status); if (err_ != ESP_OK) return err_; } while (0)

typedef struct spi_transaction_t {
    unsigned flags;
    size_t length, rxlength;
    const void *tx_buffer;
    void *rx_buffer, *user;
} spi_transaction_t;

typedef struct {
    unsigned flags;
    int clock_speed_hz, mode, spics_io_num, queue_size;
    void (*pre_cb)(spi_transaction_t *);
    void (*post_cb)(spi_transaction_t *);
    uint8_t cs_ena_pretrans, cs_ena_posttrans;
} spi_device_interface_config_t;

typedef struct esp_lcd_panel_io_t esp_lcd_panel_io_t;
typedef esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t, void *, void *);
typedef struct { esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done; } esp_lcd_panel_io_callbacks_t;

struct esp_lcd_panel_io_t {
    esp_err_t (*del)(esp_lcd_panel_io_t *);
    esp_err_t (*rx_param)(esp_lcd_panel_io_t *, int, void *, size_t);
    esp_err_t (*tx_param)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*tx_color)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*register_event_callbacks)(esp_lcd_panel_io_handle_t, const esp_lcd_panel_io_callbacks_t *, void *);
};

typedef struct {
    int pclk_hz, spi_mode, cs_gpio_num, dc_gpio_num, lcd_cmd_bits, lcd_param_bits, trans_queue_depth;
    uint8_t cs_ena_pretrans, cs_ena_posttrans;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
    struct {
        unsigned lsb_first : 1, sio_mode : 1, cs_high_active : 1, dc_high_on_cmd : 1,
                 dc_low_on_data : 1, dc_low_on_param : 1, octal_mode : 1, quad_mode : 1, psram_dma_direct : 1;
    } flags;
} esp_lcd_panel_io_spi_config_t;

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t, const esp_lcd_panel_io_spi_config_t *,
                                   esp_lcd_panel_io_handle_t *);
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t *, spi_device_handle_t *);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t, size_t *);
esp_err_t spi_device_acquire_bus(spi_device_handle_t, uint32_t);
void spi_device_release_bus(spi_device_handle_t);
esp_err_t spi_device_queue_trans(spi_device_handle_t, spi_transaction_t *, uint32_t);
esp_err_t spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t **, uint32_t);
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t *);
bool esp_ptr_external_ram(const void *);

extern int GPIO;
static inline void gpio_set_level(int pin, int level) { (void)pin; (void)level; }
static inline void gpio_func_sel(int pin, int func) { (void)pin; (void)func; }
static inline void gpio_output_enable(int pin) { (void)pin; }
static inline void gpio_output_disable(int pin) { (void)pin; }
static inline void gpio_ll_set_level(void *gpio, int pin, int level) { (void)gpio; (void)pin; (void)level; }
static inline void gpio_ll_output_enable(void *gpio, int pin) { (void)gpio; (void)pin; }
static inline void gpio_ll_output_disable(void *gpio, int pin) { (void)gpio; (void)pin; }
static inline void lcd_com_reverse_buffer_bytes(uint8_t *src, int first, int last)
{
    while (first < last) {
        const uint8_t value = src[first];
        src[first++] = src[last];
        src[last--] = value;
    }
}
