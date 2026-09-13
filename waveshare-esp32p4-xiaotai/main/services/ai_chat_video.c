#include "ai_chat_video.h"

#include <stddef.h>

#include "app_memory_policy.h"
#include "camera_video_source.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rtc_media_bridge.h"
#include "tirtc_session.h"

static const char *TAG = "ai_chat_video";

#define AI_CHAT_VIDEO_FAILURE_LOG_INTERVAL_MS 2000U
#define AI_CHAT_VIDEO_KEY_RETRY_INTERVAL_MS   500U

typedef struct {
    tirtc_conn_t conn;
    bool active;
    bool running;
    bool first_key_frame_logged;
    uint32_t queued_frames;
    uint32_t queue_failures;
    TickType_t last_failure_log_tick;
    TickType_t last_key_retry_tick;
} ai_chat_video_state_t;

static SemaphoreHandle_t s_transition_lock;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static ai_chat_video_state_t s_video;

/* Camera enable/disable can block. Serialize transitions outside the frame
 * callback so a late start cannot reopen the camera after session teardown. */
static bool ai_chat_video_state_matches(tirtc_conn_t conn)
{
    bool matches = false;

    taskENTER_CRITICAL(&s_state_lock);
    matches = s_video.active && s_video.conn == conn;
    taskEXIT_CRITICAL(&s_state_lock);
    return matches;
}

static esp_err_t ai_chat_video_submit(const uint8_t *data,
                                      size_t data_len,
                                      uint16_t width,
                                      uint16_t height,
                                      uint64_t pts_us,
                                      uint8_t media,
                                      bool key_frame,
                                      void *ctx)
{
    (void)ctx;

    tirtc_conn_t conn = NULL;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_video.active) {
        conn = s_video.conn;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (conn == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (media != TIRTC_VIDEO_H264) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = tirtc_session_send_external_video_frame(
        conn,
        AI_CHAT_VIDEO_STREAM_ID,
        data,
        data_len,
        width,
        height,
        pts_us,
        media,
        key_frame ? TIRTC_FRAME_FLAG_KEY_FRAME : 0U);

    bool log_first_key_frame = false;
    bool log_failure = false;
    bool request_key_frame = false;
    TickType_t now = xTaskGetTickCount();

    taskENTER_CRITICAL(&s_state_lock);
    if (ret == ESP_OK) {
        s_video.queued_frames++;
        if (key_frame && !s_video.first_key_frame_logged) {
            s_video.first_key_frame_logged = true;
            log_first_key_frame = true;
        }
    } else {
        s_video.queue_failures++;
        if (s_video.last_failure_log_tick == 0 ||
            now - s_video.last_failure_log_tick >=
                pdMS_TO_TICKS(AI_CHAT_VIDEO_FAILURE_LOG_INTERVAL_MS)) {
            s_video.last_failure_log_tick = now;
            log_failure = true;
        }
        if (s_video.last_key_retry_tick == 0 ||
            now - s_video.last_key_retry_tick >=
                pdMS_TO_TICKS(AI_CHAT_VIDEO_KEY_RETRY_INTERVAL_MS)) {
            s_video.last_key_retry_tick = now;
            request_key_frame = true;
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (request_key_frame) {
        camera_video_source_request_stream_start_key_frame();
    }
    if (log_first_key_frame) {
        ESP_LOGI(TAG,
                 "AI Chat video first IDR queued: stream=%u size=%ux%u payload=%u",
                 (unsigned)AI_CHAT_VIDEO_STREAM_ID,
                 (unsigned)width,
                 (unsigned)height,
                 (unsigned)data_len);
    } else if (log_failure) {
        ESP_LOGW(TAG,
                 "AI Chat video queue rejected: ret=%s key=%d payload=%u",
                 esp_err_to_name(ret),
                 key_frame ? 1 : 0,
                 (unsigned)data_len);
    }
    return ret;
}

esp_err_t ai_chat_video_init(void)
{
    if (s_transition_lock != NULL) {
        return ESP_OK;
    }

    s_transition_lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    return s_transition_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ai_chat_video_start(tirtc_conn_t conn,
                              ai_chat_video_session_valid_cb_t session_valid,
                              void *session_ctx)
{
    ESP_RETURN_ON_FALSE(conn != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "AI Chat video connection is null");
    ESP_RETURN_ON_FALSE(session_valid != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "AI Chat video session guard is null");
    ESP_RETURN_ON_ERROR(ai_chat_video_init(),
                        TAG,
                        "initialize AI Chat video failed");

    xSemaphoreTake(s_transition_lock, portMAX_DELAY);
    if (!session_valid(conn, session_ctx)) {
        xSemaphoreGive(s_transition_lock);
        return ESP_ERR_INVALID_STATE;
    }

    bool already_running = false;
    bool route_busy = false;
    taskENTER_CRITICAL(&s_state_lock);
    already_running = s_video.active &&
                      s_video.running &&
                      s_video.conn == conn;
    route_busy = s_video.active && s_video.conn != conn;
    if (!already_running && !route_busy) {
        s_video = (ai_chat_video_state_t) {
            .conn = conn,
            .active = true,
        };
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (already_running) {
        xSemaphoreGive(s_transition_lock);
        return ESP_OK;
    }
    if (route_busy) {
        xSemaphoreGive(s_transition_lock);
        return ESP_ERR_INVALID_STATE;
    }

    bool external_route_active = false;
    bool sink_registered = false;
    esp_err_t ret = tirtc_session_set_external_video_active(
        conn,
        AI_CHAT_VIDEO_STREAM_ID,
        true);
    if (ret == ESP_OK && session_valid(conn, session_ctx)) {
        external_route_active = true;
        ret = rtc_media_bridge_register_external_video_sink(
            ai_chat_video_submit,
            NULL);
    } else if (ret == ESP_OK) {
        external_route_active = true;
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && session_valid(conn, session_ctx)) {
        sink_registered = true;
        camera_video_source_request_stream_start_key_frame();
        ret = camera_video_source_set_enabled(true);
    } else if (ret == ESP_OK) {
        sink_registered = true;
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK &&
        session_valid(conn, session_ctx) &&
        ai_chat_video_state_matches(conn)) {
        camera_video_source_request_stream_start_key_frame();
        taskENTER_CRITICAL(&s_state_lock);
        if (s_video.active && s_video.conn == conn) {
            s_video.running = true;
        } else {
            ret = ESP_ERR_INVALID_STATE;
        }
        taskEXIT_CRITICAL(&s_state_lock);
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_STATE;
    }

    if (ret != ESP_OK) {
        (void)camera_video_source_set_enabled(false);
        if (sink_registered) {
            rtc_media_bridge_unregister_external_video_sink(
                ai_chat_video_submit,
                NULL);
        }
        if (external_route_active) {
            (void)tirtc_session_set_external_video_active(
                conn,
                AI_CHAT_VIDEO_STREAM_ID,
                false);
        }
        taskENTER_CRITICAL(&s_state_lock);
        if (s_video.conn == conn) {
            s_video = (ai_chat_video_state_t) {0};
        }
        taskEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_transition_lock);
        return ret;
    }

    ESP_LOGI(TAG,
             "AI Chat camera video started: stream=%u media=H264",
             (unsigned)AI_CHAT_VIDEO_STREAM_ID);
    xSemaphoreGive(s_transition_lock);
    return ESP_OK;
}

void ai_chat_video_stop(tirtc_conn_t conn)
{
    if (s_transition_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_transition_lock, portMAX_DELAY);

    tirtc_conn_t active_conn = NULL;
    bool should_cleanup = false;
    bool was_running = false;
    uint32_t queued_frames = 0;
    uint32_t queue_failures = 0;

    taskENTER_CRITICAL(&s_state_lock);
    should_cleanup = s_video.active &&
                     (conn == NULL || conn == s_video.conn);
    if (should_cleanup) {
        active_conn = s_video.conn;
        was_running = s_video.running;
        queued_frames = s_video.queued_frames;
        queue_failures = s_video.queue_failures;
        s_video.active = false;
        s_video.running = false;
        s_video.conn = NULL;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (should_cleanup) {
        esp_err_t camera_ret = camera_video_source_set_enabled(false);
        rtc_media_bridge_unregister_external_video_sink(
            ai_chat_video_submit,
            NULL);
        (void)tirtc_session_set_external_video_active(
            active_conn,
            AI_CHAT_VIDEO_STREAM_ID,
            false);

        taskENTER_CRITICAL(&s_state_lock);
        s_video = (ai_chat_video_state_t) {0};
        taskEXIT_CRITICAL(&s_state_lock);

        if (camera_ret != ESP_OK && camera_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG,
                     "stop AI Chat camera failed: %s",
                     esp_err_to_name(camera_ret));
        }
        if (was_running) {
            ESP_LOGI(TAG,
                     "AI Chat camera video stopped: queued=%lu rejected=%lu",
                     (unsigned long)queued_frames,
                     (unsigned long)queue_failures);
        }
    }

    xSemaphoreGive(s_transition_lock);
}

void ai_chat_video_get_stats(ai_chat_video_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    *stats = (ai_chat_video_stats_t) {
        .active = s_video.running,
        .queued_frames = s_video.queued_frames,
        .queue_failures = s_video.queue_failures,
    };
    taskEXIT_CRITICAL(&s_state_lock);
}
