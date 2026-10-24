#pragma once
#include <stdbool.h>
#include <stdint.h>
#define IRAM_ATTR
#define pdMS_TO_TICKS(ms) (ms)
#define USB_SERIAL_JTAG_INTR_SOF 1U
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
typedef int esp_err_t;
#define ESP_SYSTEM_INIT_FN(name, ...) int name(void)
#define USB_SERIAL_JTAG_SOF_DIAG_SAMPLES 16
#define USB_SERIAL_JTAG_SOF_SAMPLE_OBSERVED 1U
#define USB_SERIAL_JTAG_SOF_SAMPLE_CLEARED 2U
#define USB_SERIAL_JTAG_SOF_SAMPLE_RAW_AFTER 4U
typedef struct {
    uint64_t timestamp_us;
    uint32_t disconnect_count;
    uint32_t sof_history;
    uint32_t since_sof_us;
    uint32_t missing_ticks;
    uint32_t delta_us[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
    uint16_t frame_index[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
    uint8_t sample_flags[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
    uint8_t frame_index_valid;
} usb_serial_jtag_sof_diagnostic_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portENTER_CRITICAL_ISR(lock) ((void)(lock))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
uint32_t usb_serial_jtag_ll_get_intraw_mask(void);
void usb_serial_jtag_ll_clr_intsts_mask(uint32_t mask);
int esp_register_freertos_tick_hook(void (*hook)(void));
