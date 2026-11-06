#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The selected VB6824 firmware emits one fixed 40-byte Opus packet per 20 ms.
 * Keep a little diagnostic headroom, but do not reserve generic RTC-frame
 * capacity in every UART queue item. */
#define VB6824_OPUS_PACKET_BYTES 40U
#define VB6824_OPUS_MAX_BYTES 64U

typedef struct {
    uart_port_t uart_port;
    gpio_num_t tx_gpio;
    gpio_num_t rx_gpio;
    gpio_num_t reset_gpio;
    uint32_t baud_rate;
} vb6824_config_t;

typedef struct {
    uint32_t uart_bytes_received;
    uint32_t frames_received;
    uint32_t opus_received;
    uint32_t opus_dropped;
    uint32_t control_received;
    uint32_t unknown_commands;
    uint32_t checksum_errors;
    uint32_t framing_errors;
    uint32_t uart_errors;
    uint32_t pcm_frames_sent;
    uint32_t write_errors;
} vb6824_stats_t;

typedef void (*vb6824_control_callback_t)(const uint8_t *data, size_t length,
                                          void *user_data);

esp_err_t vb6824_init(const vb6824_config_t *config);
void vb6824_deinit(void);
void vb6824_set_control_callback(vb6824_control_callback_t callback,
                                 void *user_data);

void vb6824_set_input_enabled(bool enabled);
bool vb6824_input_enabled(void);
esp_err_t vb6824_read_opus(uint8_t *packet, size_t capacity, size_t *length,
                           uint32_t timeout_ms);

esp_err_t vb6824_write_pcm16(const int16_t *samples, size_t sample_count);
esp_err_t vb6824_set_volume(uint8_t percent);
void vb6824_flush(void);
vb6824_stats_t vb6824_stats(void);

#ifdef __cplusplus
}
#endif
