#include "product_audio.h"

#include <stdatomic.h>
#include <string.h>

#include "decoder/impl/esp_opus_dec.h"
#include "esp_audio_types.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rtc_adapter.h"
#include "vb6824.h"

#define AUDIO_OUTPUT_SAMPLES 160U
#define AUDIO_DECODE_QUEUE_DEPTH 6U
#define AUDIO_DECODE_MAX_BYTES 512U
#define AUDIO_DECODE_TASK_STACK_BYTES 14336U
#define AUDIO_TX_BACKLOG_BYTES 320U
#define AUDIO_PROMPT_WAIT_MS 30000U
#define AUDIO_PLAYOUT_PREBUFFER_PACKETS 3U
#define AUDIO_PLAYOUT_PREBUFFER_WAIT_MS 60U
#define AUDIO_PLAYOUT_LATE_RESET_MS 15U
#define AUDIO_RING_INCOMING_GAP_MS 560U
#define AUDIO_RING_OUTGOING_GAP_MS 760U
#define AUDIO_RING_STOP_POLL_MS 20U

typedef enum {
    DECODE_ITEM_REMOTE_AUDIO,
    DECODE_ITEM_PROMPT_CODE,
    DECODE_ITEM_PROMPT_SUCCESS,
    DECODE_ITEM_CUE,
    DECODE_ITEM_RING,
} decode_item_kind_t;

typedef struct {
    uint32_t epoch;
    uint16_t length;
    uint8_t kind;
    union {
        uint8_t data[AUDIO_DECODE_MAX_BYTES];
        char code[7];
        uint8_t cue;
        uint8_t ring;
    } payload;
} decode_item_t;

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
} prompt_asset_t;

#define DECLARE_PROMPT_ASSET(name)                                             \
    extern const uint8_t s_##name##_start[]                                   \
        asm("_binary_" #name "_opuspkt_start");                              \
    extern const uint8_t s_##name##_end[]                                     \
        asm("_binary_" #name "_opuspkt_end")
#define PROMPT_ASSET(name)                                                     \
    { .start = s_##name##_start, .end = s_##name##_end }

DECLARE_PROMPT_ASSET(activation);
DECLARE_PROMPT_ASSET(wifi_provisioning);
DECLARE_PROMPT_ASSET(success);
DECLARE_PROMPT_ASSET(popup);
DECLARE_PROMPT_ASSET(exclamation);
DECLARE_PROMPT_ASSET(call_ended);
DECLARE_PROMPT_ASSET(ring_incoming);
DECLARE_PROMPT_ASSET(ring_outgoing);
DECLARE_PROMPT_ASSET(digit_0);
DECLARE_PROMPT_ASSET(digit_1);
DECLARE_PROMPT_ASSET(digit_2);
DECLARE_PROMPT_ASSET(digit_3);
DECLARE_PROMPT_ASSET(digit_4);
DECLARE_PROMPT_ASSET(digit_5);
DECLARE_PROMPT_ASSET(digit_6);
DECLARE_PROMPT_ASSET(digit_7);
DECLARE_PROMPT_ASSET(digit_8);
DECLARE_PROMPT_ASSET(digit_9);

static const prompt_asset_t s_activation_prompt = PROMPT_ASSET(activation);
static const prompt_asset_t s_wifi_provisioning_prompt =
    PROMPT_ASSET(wifi_provisioning);
static const prompt_asset_t s_success_prompt = PROMPT_ASSET(success);
static const prompt_asset_t s_popup_prompt = PROMPT_ASSET(popup);
static const prompt_asset_t s_exclamation_prompt = PROMPT_ASSET(exclamation);
static const prompt_asset_t s_call_ended_prompt = PROMPT_ASSET(call_ended);
static const prompt_asset_t s_ring_incoming_prompt =
    PROMPT_ASSET(ring_incoming);
static const prompt_asset_t s_ring_outgoing_prompt =
    PROMPT_ASSET(ring_outgoing);
static const prompt_asset_t s_digit_prompts[] = {
    PROMPT_ASSET(digit_0), PROMPT_ASSET(digit_1), PROMPT_ASSET(digit_2),
    PROMPT_ASSET(digit_3), PROMPT_ASSET(digit_4), PROMPT_ASSET(digit_5),
    PROMPT_ASSET(digit_6), PROMPT_ASSET(digit_7), PROMPT_ASSET(digit_8),
    PROMPT_ASSET(digit_9),
};

static const char *TAG = "product_audio";
static atomic_bool s_initialized;
static atomic_bool s_session_active;
static atomic_bool s_subscribed;
static atomic_bool s_decoder_reset;
static atomic_bool s_output_configured;
static atomic_uint s_epoch;
static atomic_uintptr_t s_connection;
static atomic_uchar s_stream;
static atomic_uchar s_ring;

static StaticQueue_t s_decode_queue_storage;
/* Encoded packets are copied by the CPU and never used as DMA buffers. */
static RTC_DATA_ATTR uint8_t
    s_decode_queue_buffer[AUDIO_DECODE_QUEUE_DEPTH * sizeof(decode_item_t)];
static QueueHandle_t s_decode_queue;

static StaticTask_t s_decode_task_storage;
static StackType_t s_decode_task_stack[AUDIO_DECODE_TASK_STACK_BYTES / sizeof(StackType_t)];

static void *s_decoder;
static StaticSemaphore_t s_decoder_mutex_storage;
static SemaphoreHandle_t s_decoder_mutex;
static StaticSemaphore_t s_prompt_request_mutex_storage;
static SemaphoreHandle_t s_prompt_request_mutex;
static atomic_uint s_prompt_completed_epoch;
static atomic_int s_prompt_result;
static product_audio_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static product_voice_command_callback_t s_voice_callback;
static void *s_voice_user_data;
static TickType_t s_stream_prompt_next;
static bool s_stream_prompt_paced;
static uint8_t s_speaker_volume;

static void status_increment(uint32_t *field)
{
    portENTER_CRITICAL(&s_status_lock);
    ++*field;
    portEXIT_CRITICAL(&s_status_lock);
}

static bool write_pcm_block(uint32_t epoch, const int16_t *samples,
                            TickType_t *next, bool *paced)
{
    if (epoch != atomic_load_explicit(&s_epoch, memory_order_acquire)) {
        return false;
    }
    if (!*paced) {
        if (!atomic_load_explicit(&s_output_configured,
                                  memory_order_acquire)) {
            esp_err_t volume_err = vb6824_set_volume(s_speaker_volume);
            if (volume_err != ESP_OK) {
                ESP_LOGE(TAG, "speaker volume apply failed: %s",
                         esp_err_to_name(volume_err));
                return false;
            }
            atomic_store_explicit(&s_output_configured, true,
                                  memory_order_release);
            ESP_LOGI(TAG, "speaker volume applied before playback: %u",
                     s_speaker_volume);
        }
        *next = xTaskGetTickCount();
        *paced = true;
    }
    if (vb6824_write_pcm16(samples, AUDIO_OUTPUT_SAMPLES) != ESP_OK) {
        status_increment(&s_status.pcm_blocks_dropped);
        return false;
    }
    status_increment(&s_status.pcm_blocks_played);
    vTaskDelayUntil(next, pdMS_TO_TICKS(10));
    return true;
}

static bool playout_clock_late(TickType_t now, TickType_t next)
{
    return (int32_t)(now - next) >
           (int32_t)pdMS_TO_TICKS(AUDIO_PLAYOUT_LATE_RESET_MS);
}

static void prebuffer_remote_audio(uint32_t epoch)
{
    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(AUDIO_PLAYOUT_PREBUFFER_WAIT_MS);
    while (uxQueueMessagesWaiting(s_decode_queue) <
               AUDIO_PLAYOUT_PREBUFFER_PACKETS - 1U &&
           epoch == atomic_load_explicit(&s_epoch, memory_order_acquire) &&
           atomic_load_explicit(&s_session_active, memory_order_acquire) &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t play_prompt_asset(uint32_t prompt_id,
                                   const prompt_asset_t *asset,
                                   TickType_t *next, bool *paced)
{
    if (asset == NULL || asset->start >= asset->end) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_decoder_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    const uint8_t *cursor = asset->start;
    int16_t pcm[320];
    if (esp_opus_dec_reset(s_decoder) != ESP_AUDIO_ERR_OK) {
        err = ESP_FAIL;
        goto done;
    }
    while (cursor < asset->end) {
        size_t remaining = (size_t)(asset->end - cursor);
        if (remaining < 2U) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        size_t length = (size_t)cursor[0] | ((size_t)cursor[1] << 8U);
        cursor += 2;
        remaining -= 2U;
        if (length == 0U || length > AUDIO_DECODE_MAX_BYTES ||
            length > remaining) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (prompt_id != atomic_load_explicit(&s_epoch,
                                               memory_order_acquire)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        esp_audio_dec_in_raw_t input = {
            .buffer = (uint8_t *)cursor,
            .len = length,
        };
        esp_audio_dec_out_frame_t output = {
            .buffer = (uint8_t *)pcm,
            .len = sizeof(pcm),
        };
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t result =
            esp_opus_dec_decode(s_decoder, &input, &output, &info);
        if (result != ESP_AUDIO_ERR_OK || output.decoded_size != sizeof(pcm) ||
            info.sample_rate != 16000U || info.channel != 1U ||
            info.bits_per_sample != 16U) {
            err = ESP_FAIL;
            break;
        }
        if (!write_pcm_block(prompt_id, pcm, next, paced) ||
            !write_pcm_block(prompt_id, pcm + AUDIO_OUTPUT_SAMPLES,
                             next, paced)) {
            err = prompt_id == atomic_load_explicit(&s_epoch,
                                                     memory_order_acquire)
                      ? ESP_FAIL
                      : ESP_ERR_INVALID_STATE;
            break;
        }
        cursor += length;
    }

done:
    xSemaphoreGive(s_decoder_mutex);
    return err;
}

static bool ring_requested(uint32_t epoch, product_audio_ring_t ring)
{
    return epoch == atomic_load_explicit(&s_epoch, memory_order_acquire) &&
           ring == (product_audio_ring_t)atomic_load_explicit(
                       &s_ring, memory_order_acquire) &&
           !atomic_load_explicit(&s_session_active, memory_order_acquire);
}

static void wait_ring_gap(uint32_t epoch, product_audio_ring_t ring,
                          uint32_t gap_ms)
{
    uint32_t waited_ms = 0;
    while (waited_ms < gap_ms && ring_requested(epoch, ring)) {
        uint32_t delay_ms = gap_ms - waited_ms;
        if (delay_ms > AUDIO_RING_STOP_POLL_MS) {
            delay_ms = AUDIO_RING_STOP_POLL_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        waited_ms += delay_ms;
    }
}

static void play_ring(const decode_item_t *item)
{
    product_audio_ring_t ring = (product_audio_ring_t)item->payload.ring;
    const prompt_asset_t *asset = ring == PRODUCT_AUDIO_RING_INCOMING
                                      ? &s_ring_incoming_prompt
                                      : &s_ring_outgoing_prompt;
    uint32_t gap_ms = ring == PRODUCT_AUDIO_RING_INCOMING
                          ? AUDIO_RING_INCOMING_GAP_MS
                          : AUDIO_RING_OUTGOING_GAP_MS;

    while (ring_requested(item->epoch, ring)) {
        TickType_t next = xTaskGetTickCount();
        bool paced = false;
        esp_err_t err = play_prompt_asset(item->epoch, asset, &next, &paced);
        if (err != ESP_OK) {
            if (err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "%s ringtone playback failed: %s",
                         ring == PRODUCT_AUDIO_RING_INCOMING ? "incoming"
                                                             : "outgoing",
                         esp_err_to_name(err));
            }
            break;
        }
        wait_ring_gap(item->epoch, ring, gap_ms);
    }
}

static void decode_task(void *argument)
{
    (void)argument;
    decode_item_t item;
    int16_t pcm[320];
    TickType_t next = 0;
    bool paced = false;
    uint32_t remote_epoch = 0;
    for (;;) {
        if (xQueueReceive(s_decode_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.kind == DECODE_ITEM_RING) {
            if (item.epoch ==
                atomic_load_explicit(&s_epoch, memory_order_acquire)) {
                play_ring(&item);
            }
            continue;
        }
        if (item.kind == DECODE_ITEM_PROMPT_CODE ||
            item.kind == DECODE_ITEM_PROMPT_SUCCESS ||
            item.kind == DECODE_ITEM_CUE) {
            TickType_t prompt_next = xTaskGetTickCount();
            bool prompt_paced = false;
            esp_err_t prompt_result = ESP_OK;
            if (item.epoch !=
                atomic_load_explicit(&s_epoch, memory_order_acquire)) {
                prompt_result = ESP_ERR_INVALID_STATE;
            } else if (item.kind == DECODE_ITEM_PROMPT_SUCCESS) {
                prompt_result = play_prompt_asset(
                    item.epoch, &s_success_prompt, &prompt_next,
                    &prompt_paced);
            } else if (item.kind == DECODE_ITEM_CUE) {
                const prompt_asset_t *asset = NULL;
                switch ((product_audio_cue_t)item.payload.cue) {
                case PRODUCT_AUDIO_CUE_TAP:
                    asset = &s_popup_prompt;
                    break;
                case PRODUCT_AUDIO_CUE_CALL_ENDED:
                    asset = &s_call_ended_prompt;
                    break;
                case PRODUCT_AUDIO_CUE_WARNING:
                    asset = &s_exclamation_prompt;
                    break;
                case PRODUCT_AUDIO_CUE_WIFI_PROVISIONING:
                    asset = &s_wifi_provisioning_prompt;
                    break;
                }
                prompt_result = play_prompt_asset(
                    item.epoch, asset, &prompt_next, &prompt_paced);
            } else {
                prompt_result = play_prompt_asset(
                    item.epoch, &s_activation_prompt, &prompt_next,
                    &prompt_paced);
                for (size_t i = 0; prompt_result == ESP_OK && i < 6U; ++i) {
                    unsigned digit = (unsigned)(item.payload.code[i] - '0');
                    prompt_result = play_prompt_asset(
                        item.epoch, &s_digit_prompts[digit], &prompt_next,
                        &prompt_paced);
                }
            }
            atomic_store_explicit(&s_prompt_result, prompt_result,
                                  memory_order_relaxed);
            atomic_store_explicit(&s_prompt_completed_epoch, item.epoch,
                                  memory_order_release);
            continue;
        }
        if (item.epoch != atomic_load_explicit(&s_epoch, memory_order_acquire)) {
            paced = false;
            remote_epoch = 0;
            continue;
        }
        if (remote_epoch != item.epoch) {
            remote_epoch = item.epoch;
            paced = false;
            prebuffer_remote_audio(item.epoch);
        } else if (paced &&
                   playout_clock_late(xTaskGetTickCount(), next)) {
            /* The vendor reference keeps PCM on a continuous 10 ms clock.
             * Rebuild a small queue cushion after a real starvation instead
             * of replaying a late packet burst or resetting cadence for every
             * normally empty queue boundary. */
            paced = false;
            status_increment(&s_status.playout_resyncs);
            prebuffer_remote_audio(item.epoch);
        }
        if (xSemaphoreTake(s_decoder_mutex, portMAX_DELAY) != pdTRUE) {
            status_increment(&s_status.rx_decode_failed);
            paced = false;
            continue;
        }
        if (atomic_exchange_explicit(&s_decoder_reset, false,
                                     memory_order_acq_rel)) {
            (void)esp_opus_dec_reset(s_decoder);
        }
        esp_audio_dec_in_raw_t input = {
            .buffer = item.payload.data,
            .len = item.length,
        };
        esp_audio_dec_out_frame_t output = {
            .buffer = (uint8_t *)pcm,
            .len = sizeof(pcm),
        };
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t result = esp_opus_dec_decode(s_decoder, &input,
                                                      &output, &info);
        xSemaphoreGive(s_decoder_mutex);
        if (result != ESP_AUDIO_ERR_OK || output.decoded_size != sizeof(pcm) ||
            info.sample_rate != 16000U || info.channel != 1U ||
            info.bits_per_sample != 16U) {
            status_increment(&s_status.rx_decode_failed);
            paced = false;
            continue;
        }
        if (!write_pcm_block(item.epoch, pcm, &next, &paced) ||
            !write_pcm_block(item.epoch, pcm + AUDIO_OUTPUT_SAMPLES, &next,
                             &paced)) {
            paced = false;
        }
    }
}

static esp_err_t submit_prompt_work(uint32_t prompt_id,
                                    decode_item_kind_t kind,
                                    const char *six_digit_code)
{
    if (atomic_load(&s_session_active) ||
        prompt_id != atomic_load(&s_epoch)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_prompt_request_mutex, pdMS_TO_TICKS(1000)) !=
        pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    decode_item_t item = {
        .epoch = prompt_id,
        .kind = kind,
    };
    if (six_digit_code != NULL) {
        memcpy(item.payload.code, six_digit_code, 6U);
        item.payload.code[6] = '\0';
    }

    esp_err_t err = ESP_OK;
    if (xQueueSend(s_decode_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        err = ESP_ERR_NO_MEM;
    } else {
        int64_t deadline = esp_timer_get_time() +
                           (int64_t)AUDIO_PROMPT_WAIT_MS * 1000;
        while (atomic_load_explicit(&s_prompt_completed_epoch,
                                    memory_order_acquire) != prompt_id) {
            if (prompt_id != atomic_load(&s_epoch)) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            if (esp_timer_get_time() >= deadline) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (err == ESP_OK) {
            err = atomic_load_explicit(&s_prompt_result,
                                       memory_order_relaxed);
        }
    }
    xSemaphoreGive(s_prompt_request_mutex);
    return err;
}

static void voice_control(const uint8_t *data, size_t length, void *user_data)
{
    (void)user_data;
    if (s_voice_callback == NULL || data == NULL || length == 0) {
        return;
    }
    char command[65];
    size_t copy = length < sizeof(command) - 1 ? length : sizeof(command) - 1;
    memcpy(command, data, copy);
    command[copy] = '\0';
    s_voice_callback(command, s_voice_user_data);
}

esp_err_t product_audio_init(const product_audio_config_t *config)
{
    if (config == NULL || config->speaker_volume > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_decoder_mutex = xSemaphoreCreateMutexStatic(&s_decoder_mutex_storage);
    s_prompt_request_mutex =
        xSemaphoreCreateMutexStatic(&s_prompt_request_mutex_storage);
    if (s_decoder_mutex == NULL || s_prompt_request_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_decode_queue = xQueueCreateStatic(AUDIO_DECODE_QUEUE_DEPTH,
                                        sizeof(decode_item_t),
                                        s_decode_queue_buffer,
                                        &s_decode_queue_storage);
    if (s_decode_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    vb6824_config_t vb = {
        .uart_port = config->uart_port,
        .tx_gpio = config->tx_gpio,
        .rx_gpio = config->rx_gpio,
        .reset_gpio = config->reset_gpio,
        .baud_rate = config->baud_rate,
    };
    esp_err_t err = vb6824_init(&vb);
    if (err != ESP_OK) {
        return err;
    }
    vb6824_set_control_callback(voice_control, NULL);
    /* The reference VB6824 integration applies volume on the first playback
     * write. The DSP may not accept control commands immediately after UART
     * setup, so defer this command until audio is actually sent. */
    s_speaker_volume = config->speaker_volume;
    atomic_store(&s_output_configured, false);
    esp_opus_dec_cfg_t decoder = ESP_OPUS_DEC_CONFIG_DEFAULT();
    decoder.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    decoder.channel = ESP_AUDIO_MONO;
    decoder.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    decoder.self_delimited = false;
    if (esp_opus_dec_open(&decoder, sizeof(decoder), &s_decoder) !=
        ESP_AUDIO_ERR_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateStatic(decode_task, "opus_dec", AUDIO_DECODE_TASK_STACK_BYTES,
                          NULL, 7, s_decode_task_stack,
                          &s_decode_task_storage) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    atomic_store(&s_epoch, 1U);
    atomic_store(&s_ring, PRODUCT_AUDIO_RING_NONE);
    atomic_store(&s_initialized, true);
    ESP_LOGI(TAG,
             "ready: capture=Opus/16000/mono/20ms playback=PCM16/16000/mono volume=%u",
             config->speaker_volume);
    return ESP_OK;
}

void product_audio_set_voice_command_callback(
    product_voice_command_callback_t callback, void *user_data)
{
    s_voice_user_data = user_data;
    s_voice_callback = callback;
}

void product_audio_session_begin(tirtc_conn_t connection)
{
    product_audio_ring_t previous_ring =
        (product_audio_ring_t)atomic_exchange_explicit(
            &s_ring, PRODUCT_AUDIO_RING_NONE, memory_order_acq_rel);
    if (previous_ring != PRODUCT_AUDIO_RING_NONE) {
        ESP_LOGI(TAG, "call ringtone stopped: media session begin");
    }
    uint32_t epoch = atomic_fetch_add(&s_epoch, 1U) + 1U;
    (void)epoch;
    xQueueReset(s_decode_queue);
    atomic_store(&s_decoder_reset, true);
    atomic_store(&s_connection, (uintptr_t)connection);
    atomic_store(&s_stream, 10U);
    atomic_store(&s_subscribed, true);
    atomic_store(&s_session_active, true);
    atomic_store(&s_output_configured, false);
    vb6824_flush();
    vb6824_set_input_enabled(true);
    portENTER_CRITICAL(&s_status_lock);
    s_status.session_active = true;
    s_status.uplink_subscribed = true;
    s_status.tx_packets = s_status.tx_bytes = s_status.tx_dropped = 0;
    s_status.tx_failed = s_status.rx_packets = s_status.rx_dropped = 0;
    s_status.rx_decode_failed = s_status.pcm_blocks_played = 0;
    s_status.pcm_blocks_dropped = s_status.playout_resyncs = 0;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "session start: TX=VB6824 Opus, RX=Opus->PCM->VB6824");
}

void product_audio_session_end(void)
{
    bool active = atomic_exchange(&s_session_active, false);
    atomic_store_explicit(&s_ring, PRODUCT_AUDIO_RING_NONE,
                          memory_order_release);
    atomic_store(&s_connection, (uintptr_t)NULL);
    atomic_store(&s_subscribed, false);
    vb6824_set_input_enabled(false);
    atomic_fetch_add(&s_epoch, 1U);
    xQueueReset(s_decode_queue);
    atomic_store(&s_decoder_reset, true);
    portENTER_CRITICAL(&s_status_lock);
    s_status.session_active = false;
    s_status.uplink_subscribed = false;
    portEXIT_CRITICAL(&s_status_lock);
    if (active) {
        product_audio_log_status();
        ESP_LOGI(TAG, "session stopped");
    }
}

void product_audio_set_subscribed(uint8_t stream, bool enabled)
{
    if (enabled) {
        atomic_store(&s_stream, stream);
    }
    atomic_store(&s_subscribed, enabled);
    portENTER_CRITICAL(&s_status_lock);
    s_status.uplink_subscribed = enabled;
    portEXIT_CRITICAL(&s_status_lock);
}

void product_audio_tick(void)
{
    if (!atomic_load(&s_session_active) || !atomic_load(&s_subscribed)) {
        return;
    }
    uint8_t packet[VB6824_OPUS_MAX_BYTES];
    size_t length = 0;
    esp_err_t read = vb6824_read_opus(packet, sizeof(packet), &length, 0);
    if (read == ESP_ERR_TIMEOUT) {
        return;
    }
    if (read != ESP_OK || length == 0) {
        status_increment(&s_status.tx_dropped);
        return;
    }
    tirtc_conn_t connection = (tirtc_conn_t)atomic_load(&s_connection);
    if (connection == NULL || rtc_send_buffer_used(connection) >
                                  AUDIO_TX_BACKLOG_BYTES) {
        status_increment(&s_status.tx_dropped);
        return;
    }
    TIRTCFRAMEINFO frame = {
        .stream_id = atomic_load(&s_stream),
        .media = TIRTC_AUDIO_OPUS,
        .flags = TIRTC_AUDIOSAMPLE_16K16B1C,
        .ts = (uint32_t)(esp_timer_get_time() / 1000),
        .length = (uint32_t)length,
    };
    int result = rtc_send_audio(connection, &frame, packet);
    if (result > 0) {
        bool first;
        portENTER_CRITICAL(&s_status_lock);
        first = s_status.tx_packets == 0;
        ++s_status.tx_packets;
        s_status.tx_bytes += (uint32_t)length;
        portEXIT_CRITICAL(&s_status_lock);
        if (first) {
            ESP_LOGI(TAG, "TX first: codec=Opus stream=%u bytes=%u",
                     frame.stream_id, (unsigned)length);
        }
    } else {
        status_increment(&s_status.tx_failed);
    }
}

void product_audio_receive(tirtc_conn_t connection,
                           const TIRTCFRAMEINFO *frame, const void *data)
{
    if (frame == NULL || data == NULL ||
        connection != (tirtc_conn_t)atomic_load(&s_connection) ||
        !atomic_load(&s_session_active)) {
        return;
    }
    if (frame->media != TIRTC_AUDIO_OPUS ||
        frame->flags != TIRTC_AUDIOSAMPLE_16K16B1C || frame->length == 0 ||
        frame->length > AUDIO_DECODE_MAX_BYTES) {
        status_increment(&s_status.rx_dropped);
        return;
    }
    decode_item_t item = {
        .epoch = atomic_load(&s_epoch),
        .length = (uint16_t)frame->length,
        .kind = DECODE_ITEM_REMOTE_AUDIO,
    };
    memcpy(item.payload.data, data, frame->length);
    if (xQueueSend(s_decode_queue, &item, 0) != pdTRUE) {
        decode_item_t stale;
        (void)xQueueReceive(s_decode_queue, &stale, 0);
        if (xQueueSend(s_decode_queue, &item, 0) != pdTRUE) {
            status_increment(&s_status.rx_dropped);
            return;
        }
        status_increment(&s_status.rx_dropped);
    }
    bool first;
    portENTER_CRITICAL(&s_status_lock);
    first = s_status.rx_packets == 0;
    ++s_status.rx_packets;
    portEXIT_CRITICAL(&s_status_lock);
    if (first) {
        ESP_LOGI(TAG, "RX first: codec=Opus stream=%u bytes=%lu",
                 frame->stream_id, (unsigned long)frame->length);
    }
}

product_audio_status_t product_audio_status(void)
{
    portENTER_CRITICAL(&s_status_lock);
    product_audio_status_t copy = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return copy;
}

void product_audio_log_status(void)
{
    product_audio_status_t status = product_audio_status();
    vb6824_stats_t vb = vb6824_stats();
    ESP_LOGI(TAG,
             "stats: tx=%lu/%luB drop=%lu fail=%lu rx=%lu drop=%lu decode_fail=%lu pcm=%lu/%lu resync=%lu vb_uart=%luB frames=%lu opus=%lu ctrl=%lu unknown=%lu bad=%lu/%lu uart_err=%lu write_err=%lu",
             (unsigned long)status.tx_packets,
             (unsigned long)status.tx_bytes,
             (unsigned long)status.tx_dropped,
             (unsigned long)status.tx_failed,
             (unsigned long)status.rx_packets,
             (unsigned long)status.rx_dropped,
             (unsigned long)status.rx_decode_failed,
             (unsigned long)status.pcm_blocks_played,
             (unsigned long)status.pcm_blocks_dropped,
             (unsigned long)status.playout_resyncs,
             (unsigned long)vb.uart_bytes_received,
             (unsigned long)vb.frames_received,
             (unsigned long)vb.opus_received,
             (unsigned long)vb.control_received,
             (unsigned long)vb.unknown_commands,
             (unsigned long)vb.checksum_errors,
             (unsigned long)vb.framing_errors,
             (unsigned long)vb.uart_errors,
             (unsigned long)vb.write_errors);
}

uint32_t product_audio_prompt_begin(void)
{
    atomic_store_explicit(&s_ring, PRODUCT_AUDIO_RING_NONE,
                          memory_order_release);
    uint32_t prompt_id = atomic_fetch_add(&s_epoch, 1U) + 1U;
    xQueueReset(s_decode_queue);
    atomic_store(&s_decoder_reset, true);
    s_stream_prompt_next = xTaskGetTickCount();
    s_stream_prompt_paced = false;
    atomic_store(&s_output_configured, false);
    ESP_LOGI(TAG, "binding prompt start: id=%lu", (unsigned long)prompt_id);
    return prompt_id;
}

esp_err_t product_audio_prompt_play_code(uint32_t prompt_id,
                                         const char *six_digit_code)
{
    if (six_digit_code == NULL || strlen(six_digit_code) != 6U) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < 6U; ++i) {
        if (six_digit_code[i] < '0' || six_digit_code[i] > '9') {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return submit_prompt_work(prompt_id, DECODE_ITEM_PROMPT_CODE,
                              six_digit_code);
}

esp_err_t product_audio_prompt_write_pcm8(uint32_t prompt_id,
                                          const int16_t *samples,
                                          size_t sample_count)
{
    if (samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int16_t output[AUDIO_OUTPUT_SAMPLES];
    size_t offset = 0;
    while (offset < sample_count) {
        if (prompt_id != atomic_load(&s_epoch)) {
            return ESP_ERR_INVALID_STATE;
        }
        size_t count = sample_count - offset;
        if (count > AUDIO_OUTPUT_SAMPLES / 2U) {
            count = AUDIO_OUTPUT_SAMPLES / 2U;
        }
        for (size_t i = 0; i < count; ++i) {
            output[i * 2] = samples[offset + i];
            output[i * 2 + 1] = samples[offset + i];
        }
        if (count < AUDIO_OUTPUT_SAMPLES / 2U) {
            memset(output + count * 2, 0,
                   (AUDIO_OUTPUT_SAMPLES - count * 2) * sizeof(output[0]));
        }
        if (!write_pcm_block(prompt_id, output, &s_stream_prompt_next,
                             &s_stream_prompt_paced)) {
            return prompt_id == atomic_load(&s_epoch) ? ESP_FAIL
                                                       : ESP_ERR_INVALID_STATE;
        }
        offset += count;
    }
    return ESP_OK;
}

esp_err_t product_audio_prompt_end(uint32_t prompt_id, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (prompt_id != atomic_load(&s_epoch)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stream_prompt_paced = false;
    ESP_LOGI(TAG, "binding prompt done: id=%lu", (unsigned long)prompt_id);
    return ESP_OK;
}

void product_audio_prompt_cancel(uint32_t prompt_id)
{
    if (prompt_id == atomic_load(&s_epoch)) {
        atomic_fetch_add(&s_epoch, 1U);
        s_stream_prompt_paced = false;
        ESP_LOGI(TAG, "binding prompt cancelled: id=%lu",
                 (unsigned long)prompt_id);
    }
}

esp_err_t product_audio_play_confirmation(void)
{
    if (atomic_load(&s_session_active)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t prompt_id = product_audio_prompt_begin();
    esp_err_t err = submit_prompt_work(prompt_id, DECODE_ITEM_PROMPT_SUCCESS,
                                       NULL);
    if (err == ESP_OK) {
        err = product_audio_prompt_end(prompt_id, 5000U);
    } else {
        product_audio_prompt_cancel(prompt_id);
    }
    return err;
}

esp_err_t product_audio_play_cue(product_audio_cue_t cue)
{
    if ((unsigned)cue > PRODUCT_AUDIO_CUE_WIFI_PROVISIONING) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire) ||
        atomic_load_explicit(&s_session_active, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    decode_item_t item = {
        .epoch = atomic_load_explicit(&s_epoch, memory_order_acquire),
        .kind = DECODE_ITEM_CUE,
    };
    item.payload.cue = (uint8_t)cue;
    return xQueueSend(s_decode_queue, &item, 0) == pdTRUE ? ESP_OK
                                                          : ESP_ERR_NO_MEM;
}

esp_err_t product_audio_set_ring(product_audio_ring_t ring)
{
    if ((unsigned)ring > PRODUCT_AUDIO_RING_OUTGOING) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ring != PRODUCT_AUDIO_RING_NONE &&
        atomic_load_explicit(&s_session_active, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    product_audio_ring_t previous =
        (product_audio_ring_t)atomic_exchange_explicit(
            &s_ring, ring, memory_order_acq_rel);
    if (previous == ring) {
        return ESP_OK;
    }

    uint32_t epoch = atomic_fetch_add_explicit(&s_epoch, 1U,
                                                memory_order_acq_rel) + 1U;
    xQueueReset(s_decode_queue);
    atomic_store_explicit(&s_decoder_reset, true, memory_order_release);
    atomic_store_explicit(&s_output_configured, false, memory_order_release);

    if (ring == PRODUCT_AUDIO_RING_NONE) {
        ESP_LOGI(TAG, "call ringtone stopped");
        return ESP_OK;
    }

    decode_item_t item = {
        .epoch = epoch,
        .kind = DECODE_ITEM_RING,
    };
    item.payload.ring = (uint8_t)ring;
    if (xQueueSend(s_decode_queue, &item, 0) != pdTRUE) {
        atomic_store_explicit(&s_ring, PRODUCT_AUDIO_RING_NONE,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "call ringtone started: %s",
             ring == PRODUCT_AUDIO_RING_INCOMING ? "incoming" : "outgoing");
    return ESP_OK;
}
