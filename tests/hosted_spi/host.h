#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

enum { EH_HOST_PORT_OK, EH_HOST_PORT_ERR_TIMEOUT, EH_FRAME_OK = 0,
       EH_FRAME_DUMMY = 1, EH_FRAME_BAD = 2, ESP_STA_IF = 0, ESP_HCI_IF = 1,
       EH_HOST_PORT_SPI_GPIO_HANDSHAKE_Pin = 16, EH_HOST_PORT_SPI_GPIO_DATA_READY_Pin = 17,
       EH_HOST_PORT_HS_VAL_ACTIVE = 1, EH_HOST_PORT_DR_VAL_ACTIVE = 1, EH_SPI_MAX_BUF = 64 };
typedef int eh_host_port_err_t;
typedef int eh_frame_result_t;
typedef struct { int port, pin; } eh_host_port_gpio_desc_t;
typedef struct { uint8_t *payload; unsigned payload_len; int if_type; uint16_t seq_num; } interface_buffer_handle_t;
static const char *TAG = "spi";
static char log_line[512];
#define ESP_LOGI(tag, ...) do { (void)(tag); snprintf(log_line, sizeof(log_line), __VA_ARGS__); } while (0)
#define ESP_LOGD(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
static int s_trans_ready_sem, s_tx_q, s_bus_lock;
static bool s_running = true;
static uint8_t s_dr_pending, s_rx_buf[EH_SPI_MAX_BUF], tx_buffer[EH_SPI_MAX_BUF];
static int step = -1, transfers, dequeues, tx_frees, posts, dispatches, hci, locks;
static uint16_t delivered[6];
#ifdef DIAGNOSTICS
static int64_t timer;
static int64_t esp_timer_get_time(void) { timer += step + 1; return timer; }
#endif

static eh_host_port_err_t eh_host_port_sem_wait_ms(int sem, unsigned timeout)
{
    (void)sem;
    assert(timeout == 100);
    ++step;
    if (step == 12) s_running = false;
    if (step == 0) s_dr_pending = 1;
    if (step == 2) s_dr_pending = 1;
    return step == 0 || step == 2 ? EH_HOST_PORT_ERR_TIMEOUT : EH_HOST_PORT_OK;
}
static int eh_host_port_gpio_get(const eh_host_port_gpio_desc_t *pin)
{
    if (pin->pin == EH_HOST_PORT_SPI_GPIO_DATA_READY_Pin) return step == 0 || step == 2;
    assert(pin->pin == EH_HOST_PORT_SPI_GPIO_HANDSHAKE_Pin);
    return step != 0;
}
static int eh_host_port_queue_receive(int queue, uint8_t **data, unsigned timeout)
{
    (void)queue;
    assert(timeout == 0 && step != 0);
    ++dequeues;
    *data = step == 1 ? tx_buffer : NULL;
    return *data ? EH_HOST_PORT_OK : -1;
}
static void eh_host_port_mutex_lock(int lock) { (void)lock; assert(locks++ == 0); }
static void eh_host_port_mutex_unlock(int lock) { (void)lock; assert(--locks == 0); }
static int eh_spi_xfer_once(uint8_t *tx, uint8_t *rx)
{
    assert(locks == 1 && rx == s_rx_buf);
    assert(tx == (step == 1 ? tx_buffer : NULL));
    ++transfers;
    return step == 4 ? -1 : 0;
}
static void eh_host_port_dma_free(void *data) { assert(data == tx_buffer); ++tx_frees; }
static void eh_host_port_sem_post(int sem) { (void)sem; ++posts; }
static eh_frame_result_t eh_frame_decode(uint8_t *data, uint16_t size, interface_buffer_handle_t *frame)
{
    assert(data == s_rx_buf && size == EH_SPI_MAX_BUF && step != 4);
    if (step == 2) return EH_FRAME_DUMMY;
    if (step == 3) return EH_FRAME_BAD;
    frame->payload = data;
    frame->payload_len = step == 10 ? 0 : 1;
    frame->if_type = step == 9 ? ESP_HCI_IF : ESP_STA_IF;
    const uint16_t sequence[] = {0, 65535, 0, 0, 0, 0, 3, 3, 2, 0, 0, 4};
    frame->seq_num = sequence[step];
    return EH_FRAME_OK;
}
static void eh_host_mcu_hci_rx_deliver(uint8_t *data, unsigned size)
{
    assert(data == s_rx_buf && size == 1 && step == 9); ++hci;
}
static void eh_host_mcu_transport_dispatch_frame(interface_buffer_handle_t *frame)
{
    assert(frame->payload == s_rx_buf && frame->payload_len == 1 && dispatches < 6);
    delivered[dispatches++] = frame->seq_num;
}
