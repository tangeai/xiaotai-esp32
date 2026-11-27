// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "../esp-idf/console.c"
#include <assert.h>
#include <string.h>

static unsigned priority = 6;
UBaseType_t uxTaskPriorityGet(TaskHandle_t task) { assert(!task); return priority; }
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t next) { assert(!task); priority = next; }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || PORT_UART
static bool installed, vfs, stall;
static unsigned enables, writes, waits, received_ticks;
#endif

#if PORT_UART
static bool fail_params;
static unsigned deletes, flushes, resets, rx_events, loss, nonblocking_reads;
bool uart_is_driver_installed(int port) { assert(port == 1); return installed; }
esp_err_t uart_driver_install(int port, int rx, int tx, int events, QueueHandle_t *queue, int flags)
{
    assert(port == 1 && rx == 512 && !tx && events == 16 && !flags);
    ++enables;
    installed = true;
    *queue = &rx_events;
    return XIAOTAI_OK;
}
esp_err_t uart_driver_delete(int port)
{
    assert(port == 1 && installed);
    installed = false;
    ++deletes;
    return XIAOTAI_OK;
}
esp_err_t uart_param_config(int port, const uart_config_t *cfg)
{
    assert(port == 1 && installed && cfg->baud_rate == 921600 && cfg->data_bits == UART_DATA_8_BITS);
    assert(cfg->parity == UART_PARITY_DISABLE && cfg->stop_bits == UART_STOP_BITS_1 &&
           cfg->flow_ctrl == UART_HW_FLOWCTRL_DISABLE && cfg->source_clk == UART_SCLK_DEFAULT);
    return fail_params ? XIAOTAI_FAIL : XIAOTAI_OK;
}
esp_err_t uart_flush_input(int port) { assert(port == 1 && installed); ++flushes; return XIAOTAI_OK; }
int uart_read_bytes(int port, void *data, uint32_t size, uint32_t wait)
{
    assert(port == 1 && installed && size > 0);
    if (!wait) {
        assert(size == 69);
        ++nonblocking_reads;
        return 0;
    }
    assert(size == 1);
    *(char *)data = 'R';
    received_ticks = wait;
    return 1;
}
int uart_tx_chars(int port, const char *data, uint32_t size)
{
    assert(port == 1 && data && size && installed);
    ++writes;
    return stall ? 0 : size >= 32 ? 32 : (int)size;
}
esp_err_t uart_wait_tx_done(int port, unsigned wait)
{
    assert(port == 1 && installed && wait == 50);
    ++waits;
    return stall ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
}
void uart_vfs_dev_use_driver(int port) { assert(port == 1 && installed); vfs = true; }
int xQueueReset(QueueHandle_t queue) { assert(queue == &rx_events); ++resets; return 1; }
int xQueueReceive(QueueHandle_t queue, void *out, unsigned wait)
{
    assert(queue == &rx_events && !wait);
    if (!rx_events) return 0;
    ((uart_event_t *)out)->type = rx_events-- == 2 ? UART_BUFFER_FULL : UART_FIFO_OVF;
    return 1;
}
void console_note_rx_overflow(size_t count) { loss += count; }
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static bool attached;
bool usb_serial_jtag_is_driver_installed(void) { return installed; }
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config)
{
    assert(config->rx_buffer_size == 512 && config->tx_buffer_size >= CONSOLE_TX_CHUNK);
    ++enables;
    installed = true;
    return XIAOTAI_OK;
}
bool usb_serial_jtag_is_connected(void) { return attached; }
int usb_serial_jtag_read_bytes(void *data, uint32_t size, uint32_t wait)
{
    assert(installed && size);
    *(char *)data = 'R';
    received_ticks = wait;
    return 1;
}
int usb_serial_jtag_write_bytes(const void *data, size_t size, uint32_t timeout)
{
    assert(installed && attached && data && size <= CONSOLE_TX_CHUNK && timeout == 50);
    ++writes;
    return stall ? (int)size - 1 : (int)size;
}
esp_err_t usb_serial_jtag_wait_tx_done(uint32_t timeout)
{
    assert(installed && timeout == 50);
    ++waits;
    return XIAOTAI_OK;
}
void usb_serial_jtag_vfs_use_driver(void) { assert(installed); vfs = true; }
#endif

int main(void)
{
    xiaotai_console_info_t info;
    xiaotai_console_info(&info);
    size_t sent = 88;
    char data[70] = {0};
#if PORT_UART
    assert(info.present && info.baud_rate == 921600 && !strcmp(info.device, "uart1"));
    fail_params = true;
    assert(console_io_open(512) == XIAOTAI_FAIL && !installed && !vfs && deletes == 1);
    fail_params = false;
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    assert(info.present && !info.baud_rate && !strcmp(info.device, "usb-serial"));
#else
    assert(!info.present && !info.device[0] && !console_io_connected());
    assert(console_io_open(512) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(console_io_read(data, sizeof(data), 1) == 0);
    assert(console_io_write(data, sizeof(data), &sent) == XIAOTAI_ERR_NOT_SUPPORTED && !sent);
#endif
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || PORT_UART
    assert(console_io_open(512) == XIAOTAI_OK && installed && vfs);
    const unsigned prior = enables;
    assert(console_io_open(512) == XIAOTAI_OK && enables == prior);
#if PORT_UART
    assert(flushes == 2 && resets == 2);
    rx_events = 2;
#else
    assert(!console_io_connected() && console_io_write(data, sizeof(data), &sent) == XIAOTAI_FAIL && !sent);
    attached = true;
#endif
    assert(console_io_connected());
    assert(console_io_read(data, sizeof(data), 1) == 1 && data[0] == 'R' && received_ticks == 1);
    assert(console_io_flush() == XIAOTAI_OK && waits == 1);
    assert(console_io_write(data, sizeof(data), &sent) == XIAOTAI_OK && sent == sizeof(data));
#if PORT_UART
    assert(writes == 3 && waits == 4 && loss == 1 && nonblocking_reads == 1);
#else
    assert(writes == 1 && waits == 2);
#endif
    stall = true;
    assert(console_io_write(data, sizeof(data), &sent) == XIAOTAI_ERR_TIMEOUT);
#if PORT_UART
    assert(!sent);
#else
    assert(sent == sizeof(data) - 1);
#endif
#endif
    xiaotai_console_bulk(true);
    assert(priority == tskIDLE_PRIORITY);
    xiaotai_console_bulk(false);
    assert(priority == 6);
}
