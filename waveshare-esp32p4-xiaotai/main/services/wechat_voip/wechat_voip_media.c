#include "wechat_voip_media.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio_alaw_codec.h"
#include "audio_device.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "tirtc_session.h"
#include "tiRTC.h"

static const char *TAG = "wx_voip_media";

#define WECHAT_VOIP_MEDIA_QUEUE_LEN       4
#define WECHAT_VOIP_MEDIA_TASK_STACK      (5 * 1024)
#define WECHAT_VOIP_MEDIA_TASK_PRIORITY   10
#define WECHAT_VOIP_MEDIA_STOP_POLL_MS    10
#define WECHAT_VOIP_MEDIA_REPLACE_STOP_MS 300
#define WECHAT_VOIP_MEDIA_MAX_FRAME_BYTES 640U
#define WECHAT_VOIP_AUDIO_TARGET_RATE_HZ  8000U
#define WECHAT_VOIP_AUDIO_SOURCE_RATE_HZ  16000U
#define WECHAT_VOIP_AUDIO_STREAM_ID       10U

typedef struct {
    uint32_t generation;
    tirtc_conn_t conn;
    tirtc_session_audio_format_t format;
    uint64_t pts_us;
    size_t data_len;
    uint8_t data[WECHAT_VOIP_MEDIA_MAX_FRAME_BYTES];
} wechat_voip_media_packet_t;

typedef struct {
    tirtc_conn_t conn;
    QueueHandle_t queue;
    TaskHandle_t task;
    bool initialized;
    bool running;
    bool uplink_enabled;
    bool worker_busy;
    bool resources_prepared;
    bool local_video_enabled;
    bool remote_video_enabled;
    uint32_t generation;
    wechat_voip_media_lifecycle_t lifecycle;
    void *lifecycle_ctx;
    wechat_voip_media_stats_t stats;
} wechat_voip_media_state_t;

static wechat_voip_media_state_t s_media;
static portMUX_TYPE s_media_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_unsupported_audio_format_logged;
static bool s_audio_downsample_logged;

static void wechat_voip_media_release_resources(void);
static void wechat_voip_media_capture_cb(const uint8_t *data,
                                         size_t data_len,
                                         const audio_format_t *format,
                                         void *ctx);

static bool wechat_voip_media_can_accept_locked(void)
{
    return s_media.running &&
           s_media.uplink_enabled &&
           s_media.conn != NULL &&
           s_media.queue != NULL;
}

static void wechat_voip_media_reset_stats_locked(void)
{
    memset(&s_media.stats, 0, sizeof(s_media.stats));
    s_media.stats.running = s_media.running;
    s_media.stats.uplink_enabled = s_media.uplink_enabled;
    s_media.stats.last_error = ESP_OK;
}

static void wechat_voip_media_note_drop(void)
{
    taskENTER_CRITICAL(&s_media_lock);
    s_media.stats.dropped_frames++;
    taskEXIT_CRITICAL(&s_media_lock);
}

static void wechat_voip_media_drain_queue(QueueHandle_t queue)
{
    wechat_voip_media_packet_t stale = {0};

    while (queue != NULL && xQueueReceive(queue, &stale, 0) == pdTRUE) {
    }
}

static bool wechat_voip_media_format_is_pcma_ready(const tirtc_session_audio_format_t *format)
{
    return format != NULL &&
           format->sample_rate_hz == WECHAT_VOIP_AUDIO_TARGET_RATE_HZ &&
           format->bits_per_sample == 16U &&
           format->channels == 1U;
}

static bool wechat_voip_media_format_is_16k_pcm_mono(const tirtc_session_audio_format_t *format)
{
    return format != NULL &&
           format->sample_rate_hz == WECHAT_VOIP_AUDIO_SOURCE_RATE_HZ &&
           format->bits_per_sample == 16U &&
           format->channels == 1U;
}

static esp_err_t wechat_voip_media_send_pcma_packet(const wechat_voip_media_packet_t *packet,
                                                    audio_alaw_stream_encoder_t *encoder,
                                                    size_t *payload_len_out)
{
    size_t encoded_len = 0;
    uint8_t encoded[WECHAT_VOIP_MEDIA_MAX_FRAME_BYTES / 2U];

    if (payload_len_out != NULL) {
        *payload_len_out = 0;
    }
    ESP_RETURN_ON_FALSE(packet != NULL && packet->conn != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid microphone packet");

    esp_err_t ret = ESP_OK;
    if (wechat_voip_media_format_is_pcma_ready(&packet->format)) {
        ret = audio_alaw_encode_to(packet->data,
                                   packet->data_len,
                                   encoded,
                                   sizeof(encoded),
                                   &encoded_len);
    } else if (wechat_voip_media_format_is_16k_pcm_mono(&packet->format)) {
        ret = audio_alaw_stream_encode_16k_mono_to_8k(encoder,
                                                      packet->data,
                                                      packet->data_len,
                                                      encoded,
                                                      sizeof(encoded),
                                                      &encoded_len);
        if (ret == ESP_OK && !s_audio_downsample_logged) {
            s_audio_downsample_logged = true;
            ESP_LOGI(TAG,
                     "wechat microphone audio encode: %luHz -> %luHz bytes=%u payload=%u",
                     (unsigned long)WECHAT_VOIP_AUDIO_SOURCE_RATE_HZ,
                     (unsigned long)WECHAT_VOIP_AUDIO_TARGET_RATE_HZ,
                     (unsigned)packet->data_len,
                     (unsigned)encoded_len);
        }
    } else {
        if (!s_unsupported_audio_format_logged) {
            s_unsupported_audio_format_logged = true;
            ESP_LOGW(TAG,
                     "unsupported WeChat audio format: %luHz/%ubit/%uch",
                     (unsigned long)packet->format.sample_rate_hz,
                     packet->format.bits_per_sample,
                     packet->format.channels);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    TIRTCFRAMEINFO frame = {
        .stream_id = WECHAT_VOIP_AUDIO_STREAM_ID,
        .media = TIRTC_AUDIO_ALAW,
        .flags = TIRTC_AUDIOSAMPLE_8K16B1C,
        .reserved = 0,
        .ts = (uint32_t)(packet->pts_us / 1000ULL),
        .length = (uint32_t)encoded_len,
    };

    ret = tirtc_session_send_audio_frame(packet->conn, &frame, encoded);
    if (ret == ESP_OK && payload_len_out != NULL) {
        *payload_len_out = encoded_len;
    }
    return ret;
}

static void wechat_voip_media_task(void *ctx)
{
    (void)ctx;
    wechat_voip_media_packet_t packet = {0};
    audio_alaw_stream_encoder_t encoder = {0};
    uint32_t encoder_generation = UINT32_MAX;

    while (true) {
        if (xQueueReceive(s_media.queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bool running = false;
        tirtc_conn_t conn = NULL;
        uint32_t generation = 0;

        taskENTER_CRITICAL(&s_media_lock);
        s_media.worker_busy = true;
        running = s_media.running;
        conn = s_media.conn;
        generation = s_media.generation;
        taskEXIT_CRITICAL(&s_media_lock);

        if (!running || conn == NULL || conn != packet.conn || generation != packet.generation) {
            taskENTER_CRITICAL(&s_media_lock);
            s_media.worker_busy = false;
            taskEXIT_CRITICAL(&s_media_lock);
            continue;
        }

        if (encoder_generation != packet.generation) {
            audio_alaw_stream_encoder_reset(&encoder);
            encoder_generation = packet.generation;
        }

        size_t payload_len = 0;
        esp_err_t ret = wechat_voip_media_send_pcma_packet(&packet, &encoder, &payload_len);

        taskENTER_CRITICAL(&s_media_lock);
        bool log_first_packet = ret == ESP_OK && s_media.stats.tx_frames == 0;
        s_media.stats.last_error = ret;
        if (ret == ESP_OK) {
            s_media.stats.tx_frames++;
            s_media.stats.tx_bytes += payload_len;
        } else {
            s_media.stats.tx_failures++;
        }
        s_media.worker_busy = false;
        taskEXIT_CRITICAL(&s_media_lock);

        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE &&
            ret != ESP_ERR_TIMEOUT && ret != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "send microphone audio failed: %s", esp_err_to_name(ret));
        } else if (log_first_packet) {
            ESP_LOGI(TAG,
                     "wechat audio first packet sent: media=alaw stream=%u payload=%u ts=%lu",
                     (unsigned)WECHAT_VOIP_AUDIO_STREAM_ID,
                     (unsigned)payload_len,
                     (unsigned long)(packet.pts_us / 1000ULL));
        }
    }
}

static esp_err_t wechat_voip_media_init(void)
{
    if (s_media.initialized) {
        return ESP_OK;
    }

    s_media.queue = xQueueCreateWithCaps(WECHAT_VOIP_MEDIA_QUEUE_LEN,
                                         sizeof(wechat_voip_media_packet_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_media.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = microphone_register_observer(wechat_voip_media_capture_cb, NULL);
    if (ret != ESP_OK) {
        vQueueDeleteWithCaps(s_media.queue);
        s_media.queue = NULL;
        return ret;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(wechat_voip_media_task,
                                              "wx_audio_tx",
                                              WECHAT_VOIP_MEDIA_TASK_STACK,
                                              NULL,
                                              WECHAT_VOIP_MEDIA_TASK_PRIORITY,
                                              &s_media.task,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        microphone_unregister_observer(wechat_voip_media_capture_cb, NULL);
        vQueueDeleteWithCaps(s_media.queue);
        s_media.queue = NULL;
        s_media.task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_media.initialized = true;
    return ESP_OK;
}

esp_err_t wechat_voip_media_configure_lifecycle(const wechat_voip_media_lifecycle_t *lifecycle,
                                                void *ctx)
{
    taskENTER_CRITICAL(&s_media_lock);
    if (s_media.running || s_media.resources_prepared) {
        taskEXIT_CRITICAL(&s_media_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (lifecycle != NULL) {
        s_media.lifecycle = *lifecycle;
    } else {
        memset(&s_media.lifecycle, 0, sizeof(s_media.lifecycle));
    }
    s_media.lifecycle_ctx = ctx;
    taskEXIT_CRITICAL(&s_media_lock);
    return ESP_OK;
}

esp_err_t wechat_voip_media_prepare(bool local_video_enabled, bool remote_video_enabled)
{
    wechat_voip_media_lifecycle_t lifecycle = {0};
    void *lifecycle_ctx = NULL;

    taskENTER_CRITICAL(&s_media_lock);
    if (s_media.resources_prepared) {
        bool same_profile = s_media.local_video_enabled == local_video_enabled &&
                            s_media.remote_video_enabled == remote_video_enabled;
        taskEXIT_CRITICAL(&s_media_lock);
        return same_profile ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    lifecycle = s_media.lifecycle;
    lifecycle_ctx = s_media.lifecycle_ctx;
    taskEXIT_CRITICAL(&s_media_lock);

    if (lifecycle.prepare != NULL) {
        esp_err_t ret = lifecycle.prepare(local_video_enabled,
                                          remote_video_enabled,
                                          lifecycle_ctx);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    taskENTER_CRITICAL(&s_media_lock);
    s_media.resources_prepared = true;
    s_media.local_video_enabled = local_video_enabled;
    s_media.remote_video_enabled = remote_video_enabled;
    taskEXIT_CRITICAL(&s_media_lock);
    return ESP_OK;
}

static void wechat_voip_media_release_resources(void)
{
    void (*release)(void *ctx) = NULL;
    void *lifecycle_ctx = NULL;

    taskENTER_CRITICAL(&s_media_lock);
    if (s_media.resources_prepared) {
        s_media.resources_prepared = false;
        s_media.local_video_enabled = false;
        s_media.remote_video_enabled = false;
        release = s_media.lifecycle.release;
        lifecycle_ctx = s_media.lifecycle_ctx;
    }
    taskEXIT_CRITICAL(&s_media_lock);

    if (release != NULL) {
        release(lifecycle_ctx);
    }
}

esp_err_t wechat_voip_media_start(tirtc_conn_t conn)
{
    if (conn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_media_lock);
    bool replacing = s_media.running && s_media.conn != conn;
    taskEXIT_CRITICAL(&s_media_lock);

    if (replacing) {
        esp_err_t stop_ret = wechat_voip_media_stop_wait(NULL, WECHAT_VOIP_MEDIA_REPLACE_STOP_MS);
        if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "replace microphone media timeout: %s", esp_err_to_name(stop_ret));
            return stop_ret;
        }
    }

    taskENTER_CRITICAL(&s_media_lock);
    bool resources_prepared = s_media.resources_prepared;
    taskEXIT_CRITICAL(&s_media_lock);
    ESP_RETURN_ON_FALSE(resources_prepared,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "wechat media resources are not prepared");

    ESP_RETURN_ON_ERROR(wechat_voip_media_init(), TAG, "init microphone media failed");
    ESP_RETURN_ON_ERROR(microphone_prepare_capture_path(), TAG, "prepare microphone path failed");

    esp_err_t speaker_ret = speaker_prepare_playback_path();
    if (speaker_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "speaker path is not ready, keep microphone uplink running: %s",
                 esp_err_to_name(speaker_ret));
    }

    QueueHandle_t queue = s_media.queue;
    wechat_voip_media_drain_queue(queue);

    taskENTER_CRITICAL(&s_media_lock);
    s_media.generation++;
    s_media.conn = conn;
    s_media.running = true;
    s_media.uplink_enabled = true;
    s_unsupported_audio_format_logged = false;
    s_audio_downsample_logged = false;
    wechat_voip_media_reset_stats_locked();
    taskEXIT_CRITICAL(&s_media_lock);

    esp_err_t ret = microphone_set_observer_enabled(wechat_voip_media_capture_cb, NULL, true);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_media_lock);
        s_media.conn = NULL;
        s_media.running = false;
        s_media.uplink_enabled = false;
        wechat_voip_media_reset_stats_locked();
        taskEXIT_CRITICAL(&s_media_lock);
        return ret;
    }

    ESP_LOGI(TAG, "微信 VoIP 真实音频已启动");
    return ESP_OK;
}

void wechat_voip_media_stop(tirtc_conn_t conn)
{
    bool should_stop = false;
    QueueHandle_t queue = NULL;

    taskENTER_CRITICAL(&s_media_lock);
    should_stop = conn == NULL || conn == s_media.conn;
    if (should_stop) {
        s_media.generation++;
        s_media.conn = NULL;
        s_media.running = false;
        s_media.uplink_enabled = false;
        s_media.stats.running = false;
        s_media.stats.uplink_enabled = false;
        queue = s_media.queue;
    }
    taskEXIT_CRITICAL(&s_media_lock);

    if (!should_stop) {
        return;
    }

    (void)microphone_set_observer_enabled(wechat_voip_media_capture_cb, NULL, false);
    wechat_voip_media_drain_queue(queue);
}

esp_err_t wechat_voip_media_stop_wait(tirtc_conn_t conn, uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;
    bool target_running = false;
    bool worker_busy = false;
    bool any_running = false;

    taskENTER_CRITICAL(&s_media_lock);
    bool wrong_connection = conn != NULL && s_media.running && s_media.conn != conn;
    taskEXIT_CRITICAL(&s_media_lock);
    if (wrong_connection) {
        return ESP_ERR_INVALID_STATE;
    }

    wechat_voip_media_stop(conn);

    do {
        taskENTER_CRITICAL(&s_media_lock);
        target_running = s_media.running && (conn == NULL || conn == s_media.conn);
        any_running = s_media.running;
        worker_busy = s_media.worker_busy;
        taskEXIT_CRITICAL(&s_media_lock);
        if (!target_running && !any_running && !worker_busy) {
            wechat_voip_media_release_resources();
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(WECHAT_VOIP_MEDIA_STOP_POLL_MS));
        waited_ms += WECHAT_VOIP_MEDIA_STOP_POLL_MS;
    } while (waited_ms < timeout_ms);

    return (target_running || any_running || worker_busy) ? ESP_ERR_TIMEOUT : ESP_OK;
}

bool wechat_voip_media_is_running(void)
{
    bool running = false;

    taskENTER_CRITICAL(&s_media_lock);
    running = s_media.running;
    taskEXIT_CRITICAL(&s_media_lock);
    return running;
}

esp_err_t wechat_voip_media_set_uplink_enabled(bool enabled)
{
    ESP_RETURN_ON_ERROR(wechat_voip_media_init(), TAG, "init microphone media failed");

    taskENTER_CRITICAL(&s_media_lock);
    if (!s_media.running || s_media.conn == NULL) {
        taskEXIT_CRITICAL(&s_media_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_media.uplink_enabled = enabled;
    s_media.stats.uplink_enabled = enabled;
    taskEXIT_CRITICAL(&s_media_lock);

    return microphone_set_observer_enabled(wechat_voip_media_capture_cb, NULL, enabled);
}

void wechat_voip_media_get_stats(wechat_voip_media_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_media_lock);
    *stats = s_media.stats;
    stats->running = s_media.running;
    stats->uplink_enabled = s_media.uplink_enabled;
    taskEXIT_CRITICAL(&s_media_lock);
}

static void wechat_voip_media_capture_cb(const uint8_t *data,
                                         size_t data_len,
                                         const audio_format_t *format,
                                         void *ctx)
{
    (void)ctx;
    wechat_voip_media_packet_t packet = {0};
    bool accept = false;
    QueueHandle_t queue = NULL;

    if (data == NULL || data_len == 0U || format == NULL) {
        return;
    }
    if (data_len > WECHAT_VOIP_MEDIA_MAX_FRAME_BYTES) {
        wechat_voip_media_note_drop();
        return;
    }

    taskENTER_CRITICAL(&s_media_lock);
    accept = wechat_voip_media_can_accept_locked();
    if (accept) {
        queue = s_media.queue;
        packet.generation = s_media.generation;
        packet.conn = s_media.conn;
        packet.format.sample_rate_hz = format->sample_rate_hz;
        packet.format.channels = format->channels;
        packet.format.bits_per_sample = format->bits_per_sample;
        packet.pts_us = (uint64_t)esp_timer_get_time();
        packet.data_len = data_len;
    }
    taskEXIT_CRITICAL(&s_media_lock);

    if (!accept) {
        return;
    }

    memcpy(packet.data, data, data_len);
    if (queue == NULL) {
        return;
    }

    if (xQueueSend(queue, &packet, 0) == pdTRUE) {
        return;
    }

    wechat_voip_media_packet_t stale = {0};
    if (xQueueReceive(queue, &stale, 0) == pdTRUE) {
        wechat_voip_media_note_drop();
    }
    if (xQueueSend(queue, &packet, 0) != pdTRUE) {
        wechat_voip_media_note_drop();
    }
}
