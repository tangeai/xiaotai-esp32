#include "vb6824.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "vb6824_wire.h"

#define VB_FRAME_MAX_PAYLOAD 512U
#define VB_PARSER_CAPACITY (VB_FRAME_MAX_PAYLOAD + VB6824_WIRE_OVERHEAD + 64U)
#define VB_RX_QUEUE_DEPTH 4U
#define VB_RX_TASK_STACK_BYTES 3584U
#define VB_UART_RX_BUFFER_BYTES 2048U
#define VB_UART_TX_BUFFER_BYTES 1024U

#define VB_CMD_AUDIO_FROM_DSP 0x2080U
#define VB_CMD_CONTROL_FROM_DSP 0x0180U
#define VB_CMD_PCM_TO_DSP 0x2081U
#define VB_CMD_VOLUME_TO_DSP 0x0203U

typedef struct {
    uint16_t length;
    uint8_t data[VB6824_OPUS_MAX_BYTES];
} opus_item_t;

static const char *TAG = "vb6824";
static vb6824_config_t s_config;
static atomic_bool s_initialized;
static atomic_bool s_running;
static atomic_bool s_input_enabled;
static vb6824_control_callback_t s_control_callback;
static void *s_control_user_data;

static StaticQueue_t s_opus_queue_storage;
static RTC_DATA_ATTR uint8_t
    s_opus_queue_buffer[VB_RX_QUEUE_DEPTH * sizeof(opus_item_t)];
static QueueHandle_t s_opus_queue;
static StaticSemaphore_t s_tx_mutex_storage;
static SemaphoreHandle_t s_tx_mutex;
static StaticTask_t s_rx_task_storage;
static StackType_t s_rx_task_stack[VB_RX_TASK_STACK_BYTES / sizeof(StackType_t)];
static TaskHandle_t s_rx_task;

/* These framing buffers are CPU-only; keep DMA-capable SRAM available to Wi-Fi. */
static RTC_DATA_ATTR uint8_t s_parser[VB_PARSER_CAPACITY];
static size_t s_parser_length;
static RTC_DATA_ATTR uint8_t
    s_tx_frame[VB_FRAME_MAX_PAYLOAD + VB6824_WIRE_OVERHEAD];
static vb6824_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_first_uart_rx_logged;

static void stats_add(uint32_t *field)
{
    portENTER_CRITICAL(&s_stats_lock);
    ++*field;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stats_add_value(uint32_t *field, uint32_t value)
{
    portENTER_CRITICAL(&s_stats_lock);
    *field += value;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void drop_prefix(size_t bytes)
{
    if (bytes >= s_parser_length) {
        s_parser_length = 0;
        return;
    }
    memmove(s_parser, s_parser + bytes, s_parser_length - bytes);
    s_parser_length -= bytes;
}

static void dispatch_frame(uint16_t command, const uint8_t *payload,
                           size_t length)
{
    stats_add(&s_stats.frames_received);
    if (command == VB_CMD_AUDIO_FROM_DSP) {
        if (!atomic_load_explicit(&s_input_enabled, memory_order_acquire)) {
            return;
        }
        if (length != VB6824_OPUS_PACKET_BYTES) {
            stats_add(&s_stats.framing_errors);
            return;
        }
        opus_item_t item = {.length = (uint16_t)length};
        memcpy(item.data, payload, length);
        if (xQueueSend(s_opus_queue, &item, 0) != pdTRUE) {
            opus_item_t discarded;
            (void)xQueueReceive(s_opus_queue, &discarded, 0);
            if (xQueueSend(s_opus_queue, &item, 0) != pdTRUE) {
                stats_add(&s_stats.opus_dropped);
                return;
            }
            stats_add(&s_stats.opus_dropped);
        }
        stats_add(&s_stats.opus_received);
        return;
    }
    if (command == VB_CMD_CONTROL_FROM_DSP) {
        stats_add(&s_stats.control_received);
        if (s_control_callback != NULL) {
            s_control_callback(payload, length, s_control_user_data);
        }
        return;
    }
    stats_add(&s_stats.unknown_commands);
}

static void parse_pending(void)
{
    for (;;) {
        if (s_parser_length < VB6824_WIRE_OVERHEAD) {
            return;
        }
        size_t header = 0;
        while (header + 1 < s_parser_length &&
               (s_parser[header] != VB6824_WIRE_HEADER_0 ||
                s_parser[header + 1] != VB6824_WIRE_HEADER_1)) {
            ++header;
        }
        if (header > 0) {
            stats_add(&s_stats.framing_errors);
            drop_prefix(header);
        }
        if (s_parser_length < VB6824_WIRE_OVERHEAD) {
            return;
        }
        uint16_t command = 0;
        size_t payload_length = 0;
        size_t frame_length = 0;
        vb6824_wire_result_t result = vb6824_wire_inspect(
            s_parser, s_parser_length, VB_FRAME_MAX_PAYLOAD, &command,
            &payload_length, &frame_length);
        if (result == VB6824_WIRE_NEED_MORE) {
            return;
        }
        if (result == VB6824_WIRE_BAD_CHECKSUM) {
            stats_add(&s_stats.checksum_errors);
            drop_prefix(1);
            continue;
        }
        if (result != VB6824_WIRE_COMPLETE) {
            stats_add(&s_stats.framing_errors);
            drop_prefix(1);
            continue;
        }
        dispatch_frame(command, &s_parser[6], payload_length);
        drop_prefix(frame_length);
    }
}

static void parser_append(const uint8_t *data, size_t length)
{
    while (length > 0) {
        size_t space = sizeof(s_parser) - s_parser_length;
        if (space == 0) {
            stats_add(&s_stats.framing_errors);
            drop_prefix(1);
            space = sizeof(s_parser) - s_parser_length;
        }
        size_t copy = length < space ? length : space;
        memcpy(s_parser + s_parser_length, data, copy);
        s_parser_length += copy;
        data += copy;
        length -= copy;
        parse_pending();
    }
}

static void rx_task(void *argument)
{
    (void)argument;
    uint8_t buffer[256];
    ESP_LOGI(TAG, "RX task ready: uart=%d baud=%lu", s_config.uart_port,
             (unsigned long)s_config.baud_rate);
    while (atomic_load_explicit(&s_running, memory_order_acquire)) {
        int length = uart_read_bytes(s_config.uart_port, buffer, sizeof(buffer),
                                     pdMS_TO_TICKS(20));
        if (length > 0) {
            stats_add_value(&s_stats.uart_bytes_received, (uint32_t)length);
            if (!s_first_uart_rx_logged) {
                s_first_uart_rx_logged = true;
                size_t preview = (size_t)length < 16U ? (size_t)length : 16U;
                ESP_LOGI(TAG, "UART RX first: bytes=%d preview=%u", length,
                         (unsigned)preview);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, preview, ESP_LOG_INFO);
            }
            parser_append(buffer, (size_t)length);
        } else if (length < 0) {
            stats_add(&s_stats.uart_errors);
        }
    }
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t write_frame(uint16_t command, const void *payload,
                             size_t payload_length)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((payload_length > 0 && payload == NULL) ||
        payload_length > VB_FRAME_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    size_t frame_length = vb6824_wire_encode(
        command, payload, payload_length, s_tx_frame, sizeof(s_tx_frame));
    if (frame_length == 0) {
        xSemaphoreGive(s_tx_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    int written = uart_write_bytes(s_config.uart_port, s_tx_frame, frame_length);
    xSemaphoreGive(s_tx_mutex);
    if (written != (int)frame_length) {
        stats_add(&s_stats.write_errors);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t vb6824_init(const vb6824_config_t *config)
{
    if (config == NULL || config->uart_port < UART_NUM_0 ||
        config->uart_port >= UART_NUM_MAX || config->tx_gpio < 0 ||
        config->rx_gpio < 0 || config->baud_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_config = *config;
    memset(&s_stats, 0, sizeof(s_stats));
    s_first_uart_rx_logged = false;
    s_parser_length = 0;
    s_opus_queue = xQueueCreateStatic(VB_RX_QUEUE_DEPTH, sizeof(opus_item_t),
                                      s_opus_queue_buffer, &s_opus_queue_storage);
    s_tx_mutex = xSemaphoreCreateMutexStatic(&s_tx_mutex_storage);
    if (s_opus_queue == NULL || s_tx_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (config->reset_gpio >= 0) {
        gpio_config_t reset = {
            .pin_bit_mask = 1ULL << config->reset_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&reset), TAG, "configure reset pin");
        gpio_set_level(config->reset_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(config->reset_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    const uart_config_t uart = {
        .baud_rate = (int)config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(config->uart_port, &uart);
    if (err == ESP_OK) {
        err = uart_set_pin(config->uart_port, config->tx_gpio, config->rx_gpio,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = uart_driver_install(config->uart_port, VB_UART_RX_BUFFER_BYTES,
                                  VB_UART_TX_BUFFER_BYTES, 0, NULL, 0);
    }
    if (err != ESP_OK) {
        return err;
    }
    atomic_store_explicit(&s_input_enabled, false, memory_order_release);
    atomic_store_explicit(&s_running, true, memory_order_release);
    atomic_store_explicit(&s_initialized, true, memory_order_release);
    s_rx_task = xTaskCreateStatic(rx_task, "vb6824_rx", VB_RX_TASK_STACK_BYTES,
                                  NULL, 9, s_rx_task_stack, &s_rx_task_storage);
    if (s_rx_task == NULL) {
        atomic_store(&s_initialized, false);
        atomic_store(&s_running, false);
        uart_driver_delete(config->uart_port);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready: uart=%d tx=%d rx=%d reset=%d baud=%lu",
             config->uart_port, config->tx_gpio, config->rx_gpio,
             config->reset_gpio, (unsigned long)config->baud_rate);
    return ESP_OK;
}

void vb6824_deinit(void)
{
    if (!atomic_exchange(&s_initialized, false)) {
        return;
    }
    atomic_store(&s_running, false);
    for (unsigned i = 0; i < 20 && s_rx_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    (void)uart_driver_delete(s_config.uart_port);
    s_opus_queue = NULL;
    s_tx_mutex = NULL;
}

void vb6824_set_control_callback(vb6824_control_callback_t callback,
                                 void *user_data)
{
    s_control_user_data = user_data;
    s_control_callback = callback;
}

void vb6824_set_input_enabled(bool enabled)
{
    atomic_store_explicit(&s_input_enabled, enabled, memory_order_release);
    if (!enabled && s_opus_queue != NULL) {
        xQueueReset(s_opus_queue);
    }
}

bool vb6824_input_enabled(void)
{
    return atomic_load_explicit(&s_input_enabled, memory_order_acquire);
}

esp_err_t vb6824_read_opus(uint8_t *packet, size_t capacity, size_t *length,
                           uint32_t timeout_ms)
{
    if (packet == NULL || length == NULL || s_opus_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    opus_item_t item;
    if (xQueueReceive(s_opus_queue, &item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (item.length > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(packet, item.data, item.length);
    *length = item.length;
    return ESP_OK;
}

esp_err_t vb6824_write_pcm16(const int16_t *samples, size_t sample_count)
{
    if (sample_count == 0 || sample_count > 160U) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = write_frame(VB_CMD_PCM_TO_DSP, samples,
                                sample_count * sizeof(*samples));
    if (err == ESP_OK) {
        stats_add(&s_stats.pcm_frames_sent);
    }
    return err;
}

esp_err_t vb6824_set_volume(uint8_t percent)
{
    if (percent > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t level = (uint8_t)(((uint32_t)percent * 31U + 50U) / 100U);
    return write_frame(VB_CMD_VOLUME_TO_DSP, &level, sizeof(level));
}

void vb6824_flush(void)
{
    /* UART RX and parser state belong exclusively to rx_task. Flushing the
     * hardware UART or rewriting s_parser_length from the VoIP control task
     * can wait on the driver's RX lock while rx_task is blocked in
     * uart_read_bytes(), stalling the complete call state machine. Frames are
     * already discarded while input is disabled, so clearing the delivered
     * Opus queue is the only session-boundary operation required here. */
    if (s_opus_queue != NULL) {
        xQueueReset(s_opus_queue);
    }
}

vb6824_stats_t vb6824_stats(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    vb6824_stats_t copy = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
    return copy;
}
