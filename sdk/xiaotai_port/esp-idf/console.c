// SPDX-License-Identifier: MIT
#include "console_internal.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
#define PORT_UART 1
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/queue.h"
static QueueHandle_t s_rx_events;
#define UART CONFIG_ESP_CONSOLE_UART_NUM
#endif
#ifndef PORT_UART
#define PORT_UART 0
#endif

static _Thread_local bool s_bulk;
static _Thread_local UBaseType_t s_priority;
void xiaotai_console_bulk(bool active)
{
    if (active == s_bulk) return;
    s_bulk = active;
    if (active) {
        s_priority = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, tskIDLE_PRIORITY);
    } else vTaskPrioritySet(NULL, s_priority);
}

void console_io_lock(void) { flockfile(stdout); }
void console_io_unlock(void) { funlockfile(stdout); }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || PORT_UART
static TickType_t ticks(uint32_t ms)
{
    const uint64_t rounded = ((uint64_t)ms * configTICK_RATE_HZ + 999U) / 1000U;
    return rounded >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t)rounded;
}
#endif

xiaotai_err_t console_io_open(size_t rx)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t settings = {.tx_buffer_size = 512, .rx_buffer_size = rx};
        const esp_err_t err = usb_serial_jtag_driver_install(&settings);
        if (err != ESP_OK) return err;
    }
    usb_serial_jtag_vfs_use_driver();
    return XIAOTAI_OK;
#elif PORT_UART
    bool installed_here = !uart_is_driver_installed(UART);
    if (installed_here) {
        const uart_config_t settings = {
            .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE, .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t err = uart_driver_install(UART, (int)rx, 0, 16, &s_rx_events, 0);
        if (err != ESP_OK) return err;
        err = uart_param_config(UART, &settings);
        if (err != ESP_OK) {
            (void)uart_driver_delete(UART);
            s_rx_events = NULL;
            return err;
        }
    }
    const esp_err_t err = uart_flush_input(UART);
    if (err != ESP_OK) {
        if (installed_here) {
            (void)uart_driver_delete(UART);
            s_rx_events = NULL;
        }
        return err;
    }
    if (s_rx_events) (void)xQueueReset(s_rx_events);
    uart_vfs_dev_use_driver(UART);
    return XIAOTAI_OK;
#else
    (void)rx;
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
void xiaotai_console_info(xiaotai_console_info_t *info)
{
    if (!info) return;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    *info = (xiaotai_console_info_t){.device = "usb-serial", .present = true};
#elif PORT_UART
    *info = (xiaotai_console_info_t){
        .device = "uart" STRINGIFY(UART), .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE, .present = true,
    };
#else
    *info = (xiaotai_console_info_t){.device = ""};
#endif
}

size_t console_io_read(char *data, size_t size, uint32_t ms)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    const int n = usb_serial_jtag_read_bytes(data, size, ticks(ms));
    return n > 0 ? (size_t)n : 0;
#elif PORT_UART
    /* The IDF UART driver waits per ring item until it fills the request.
     * Only the first byte may wait, then drain the already queued bytes. */
    const size_t first = ms && size ? 1 : size;
    int n = uart_read_bytes(UART, data, first, ticks(ms));
    if (n > 0 && first < size) {
        const int rest = uart_read_bytes(UART, data + n, size - (size_t)n, 0);
        if (rest > 0) n += rest;
    }
    if (s_rx_events) {
        uart_event_t event;
        /* IDF does not expose an exact byte count for overflow. Count confirmed
         * FIFO loss events, not ring-full notifications for stashed bytes. */
        for (unsigned i = 0; i < 16 && xQueueReceive(s_rx_events, &event, 0); ++i)
            if (event.type == UART_FIFO_OVF) console_note_rx_overflow(1);
    }
    return n > 0 ? (size_t)n : 0;
#else
    (void)data; (void)size; (void)ms;
    return 0;
#endif
}

bool console_io_connected(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_is_connected();
#elif PORT_UART
    return true;
#else
    return false;
#endif
}

xiaotai_err_t console_io_flush(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (fflush(stdout) != 0) return XIAOTAI_FAIL;
    return usb_serial_jtag_wait_tx_done(ticks(XIAOTAI_CONSOLE_WRITE_TIMEOUT_MS));
#elif PORT_UART
    if (fflush(stdout) != 0) return XIAOTAI_FAIL;
    return uart_wait_tx_done(UART, ticks(XIAOTAI_CONSOLE_WRITE_TIMEOUT_MS));
#else
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}

xiaotai_err_t console_io_write(const char *data, size_t size, size_t *accepted)
{
    *accepted = 0;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    const TickType_t timeout = ticks(XIAOTAI_CONSOLE_WRITE_TIMEOUT_MS);
    if (!usb_serial_jtag_is_connected()) return XIAOTAI_FAIL;
    int n = usb_serial_jtag_write_bytes(data, size, timeout);
    if (n < 0) return XIAOTAI_FAIL;
    *accepted = (size_t)n;
    if (*accepted != size) return XIAOTAI_ERR_TIMEOUT;
    return usb_serial_jtag_wait_tx_done(timeout);
#elif PORT_UART
    const TickType_t timeout = ticks(XIAOTAI_CONSOLE_WRITE_TIMEOUT_MS);
    while (*accepted < size) {
        const int n = uart_tx_chars(UART, data + *accepted, size - *accepted);
        if (n < 0) return XIAOTAI_FAIL;
        *accepted += (size_t)n;
        const esp_err_t err = uart_wait_tx_done(UART, timeout);
        if (err != ESP_OK) return err;
        if (n == 0) return XIAOTAI_ERR_TIMEOUT;
    }
    return XIAOTAI_OK;
#else
    (void)data; (void)size;
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}
