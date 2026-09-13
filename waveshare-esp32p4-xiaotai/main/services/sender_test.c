#include "sender_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "device_video_profile.h"
#include "virtual_audio_source.h"
#include "virtual_video_source.h"
#include "network.h"
#include "tirtc_session.h"
#include "tiRTC.h"

void tirtc_session_refresh_media_policy(void);

static const char *TAG = "sender_test";

#define SENDER_TEST_TASK_STACK            (12 * 1024)
#define SENDER_TEST_TASK_PRIORITY         10
#define SENDER_TEST_TASK_CORE             1
#define SENDER_TEST_INVALID_STREAM_ID     0xFFU
#define SENDER_TEST_WAIT_POLL_MS          5
#define SENDER_TEST_AUDIO_WAIT_LOG_US     2000000ULL
#define SENDER_TEST_AUDIO_CATCH_UP_BURST  12U
#define SENDER_TEST_AUDIO_JANK_THRESHOLD_US 30000ULL

typedef struct {
    virtual_audio_source_t source;
    device_video_sender_stats_t stats;
    uint64_t next_send_at_us;
    uint64_t media_pts_us;
    uint64_t last_sent_media_pts_us;
    uint32_t jank_samples;
    uint32_t jank_count;
    bool jank_window_active;
    uint32_t pending_packet_duration_us;
    int packet_pending;
    int packet_enqueued;
    uint8_t pending_send_failures;
    int first_packet_read_logged;
    int first_packet_sent_logged;
    uint64_t last_wait_log_us;
} sender_test_audio_session_t;

static portMUX_TYPE s_sender_test_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_sender_test_task;
static bool s_sender_test_initialized;
static bool s_sender_test_running;
static bool s_sender_test_spiffs_ready;
static bool s_sender_test_restart_requested;
static bool s_sender_test_stop_requested;
static bool s_sender_test_force_video_restart;
static bool s_sender_test_force_audio_restart;
static sender_test_mode_t s_sender_test_requested_mode = SENDER_TEST_MODE_NONE;
static char s_sender_test_status[SENDER_TEST_STATUS_MAX] = "Idle";

static void sender_test_task_entry(void *ctx);

static void sender_test_log_audio_jank_summary(sender_test_audio_session_t *session,
                                                        const char *reason)
{
    if (session == NULL || !session->jank_window_active) {
        return;
    }

    if (session->jank_count > 0U && session->jank_samples > 0U) {
        ESP_LOGW(TAG,
                 "sender test audio jank: rate=%.2f count=%lu samples=%lu threshold_us=%lu frames=%llu bytes=%llu reason=%s",
                 (double)session->jank_count * 100.0 / (double)session->jank_samples,
                 (unsigned long)session->jank_count,
                 (unsigned long)session->jank_samples,
                 (unsigned long)SENDER_TEST_AUDIO_JANK_THRESHOLD_US,
                 (unsigned long long)session->stats.frames_sent,
                 (unsigned long long)session->stats.bytes_sent,
                 reason != NULL ? reason : "unknown");
    } else if (session->jank_samples > 0U) {
        ESP_LOGD(TAG,
                 "sender test audio jank clear: samples=%lu threshold_us=%lu frames=%llu bytes=%llu reason=%s",
                 (unsigned long)session->jank_samples,
                 (unsigned long)SENDER_TEST_AUDIO_JANK_THRESHOLD_US,
                 (unsigned long long)session->stats.frames_sent,
                 (unsigned long long)session->stats.bytes_sent,
                 reason != NULL ? reason : "unknown");
    }

    session->jank_window_active = false;
    session->jank_samples = 0U;
    session->jank_count = 0U;
    session->last_sent_media_pts_us = 0U;
}

static void sender_test_log_heap(const char *stage)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGW(TAG,
             "%s: heap internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage != NULL ? stage : "sender_test",
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)psram_free,
             (unsigned)psram_largest);
}

static BaseType_t sender_test_create_task(TaskHandle_t *task_handle)
{
#if CONFIG_FREERTOS_UNICORE
    return xTaskCreateWithCaps(sender_test_task_entry,
                               "sender_test",
                               SENDER_TEST_TASK_STACK,
                               NULL,
                               SENDER_TEST_TASK_PRIORITY,
                               task_handle,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(sender_test_task_entry,
                                                         "sender_test",
                                                         SENDER_TEST_TASK_STACK,
                                                         NULL,
                                                         SENDER_TEST_TASK_PRIORITY,
                                                         task_handle,
                                                         SENDER_TEST_TASK_CORE,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (task_ok != pdPASS) {
        task_ok = xTaskCreateWithCaps(sender_test_task_entry,
                                      "sender_test",
                                      SENDER_TEST_TASK_STACK,
                                      NULL,
                                      SENDER_TEST_TASK_PRIORITY,
                                      task_handle,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return task_ok;
#endif
}

static const char *sender_test_mode_label(sender_test_mode_t mode)
{
    switch (mode) {
    case SENDER_TEST_MODE_AUDIO:
        return "Audio";
    case SENDER_TEST_MODE_VIDEO:
        return "Video";
    default:
        return "Sender";
    }
}

static const char *sender_test_mode_input_path(sender_test_mode_t mode)
{
    switch (mode) {
    case SENDER_TEST_MODE_VIDEO:
        return VIRTUAL_VIDEO_SOURCE_DEFAULT_PATH;
    default:
        return "";
    }
}

static uint64_t sender_test_now_us(void)
{
    struct timeval tv = {0};

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static bool sender_test_should_log_audio_wait(sender_test_audio_session_t *session, uint64_t now_us)
{
    if (session == NULL) {
        return false;
    }
    if (session->last_wait_log_us == 0U || now_us - session->last_wait_log_us >= SENDER_TEST_AUDIO_WAIT_LOG_US) {
        session->last_wait_log_us = now_us;
        return true;
    }
    return false;
}

static int sender_test_audio_sleep_until(uint64_t target_us)
{
    uint64_t now_us = sender_test_now_us();
    uint64_t wait_us = 0U;

    if (target_us == 0U || now_us >= target_us) {
        return 0;
    }

    wait_us = target_us - now_us;

    if (wait_us >= 2000ULL) {
        uint32_t coarse_ms = (uint32_t)((wait_us - 1000ULL) / 1000ULL);
        if (coarse_ms > 0U) {
            return (int)coarse_ms;
        }
    }

    esp_rom_delay_us((uint32_t)wait_us);
    return 0;
}

static void sender_test_audio_reset_timing(sender_test_audio_session_t *session)
{
    if (session == NULL) {
        return;
    }

    session->next_send_at_us = 0U;
    session->media_pts_us = 0U;
    session->packet_pending = 0;
    session->packet_enqueued = 0;
    session->pending_send_failures = 0;
}

static void sender_test_set_status_locked(const char *text)
{
    strlcpy(s_sender_test_status, text != NULL ? text : "", sizeof(s_sender_test_status));
}

static void sender_test_set_status(const char *fmt, ...)
{
    char text[SENDER_TEST_STATUS_MAX] = {0};
    va_list args;

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    taskENTER_CRITICAL(&s_sender_test_lock);
    sender_test_set_status_locked(text);
    taskEXIT_CRITICAL(&s_sender_test_lock);
}

static bool sender_test_restart_pending(void)
{
    bool restart_requested = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    restart_requested = s_sender_test_restart_requested || s_sender_test_stop_requested;
    taskEXIT_CRITICAL(&s_sender_test_lock);
    return restart_requested;
}

static bool sender_test_consume_video_restart_request(void)
{
    bool restart_requested = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    restart_requested = s_sender_test_force_video_restart;
    s_sender_test_force_video_restart = false;
    taskEXIT_CRITICAL(&s_sender_test_lock);
    return restart_requested;
}

static bool sender_test_consume_audio_restart_request(void)
{
    bool restart_requested = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    restart_requested = s_sender_test_force_audio_restart;
    s_sender_test_force_audio_restart = false;
    taskEXIT_CRITICAL(&s_sender_test_lock);
    return restart_requested;
}

static bool sender_test_input_exists(const char *path)
{
    FILE *file = NULL;

    if (path == NULL || path[0] == '\0') {
        return false;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }

    fclose(file);
    return true;
}

static void sender_test_update_ready_status_locked(bool has_video, bool has_audio)
{
    if (has_video && has_audio) {
        sender_test_set_status_locked("Ready: /spiffs/send_video.h264 + virtual audio");
    } else if (!has_video) {
        sender_test_set_status_locked("Missing: /spiffs/send_video.h264");
    } else {
        sender_test_set_status_locked("Virtual audio unavailable");
    }
}

static esp_err_t sender_test_mount_spiffs(void)
{
    bool has_video = false;
    bool has_audio = false;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount spiffs failed: %s", esp_err_to_name(ret));
        taskENTER_CRITICAL(&s_sender_test_lock);
        s_sender_test_spiffs_ready = false;
        sender_test_set_status_locked("SPIFFS mount failed");
        taskEXIT_CRITICAL(&s_sender_test_lock);
        return ret;
    }

    has_video = sender_test_input_exists(VIRTUAL_VIDEO_SOURCE_DEFAULT_PATH);
    has_audio = true;

    taskENTER_CRITICAL(&s_sender_test_lock);
    s_sender_test_spiffs_ready = true;
    sender_test_update_ready_status_locked(has_video, has_audio);
    taskEXIT_CRITICAL(&s_sender_test_lock);
    return ESP_OK;
}

static esp_err_t sender_test_validate_mode(sender_test_mode_t mode)
{
    const char *input_path = sender_test_mode_input_path(mode);

    if (mode != SENDER_TEST_MODE_VIDEO && mode != SENDER_TEST_MODE_AUDIO) {
        sender_test_set_status("Invalid test mode");
        return ESP_ERR_INVALID_ARG;
    }

    if (!network_is_connected()) {
        sender_test_set_status("Connect WiFi first");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_sender_test_spiffs_ready) {
        esp_err_t mount_ret = sender_test_mount_spiffs();
        if (mount_ret != ESP_OK) {
            return mount_ret;
        }
    }

    if (mode == SENDER_TEST_MODE_VIDEO && !sender_test_input_exists(input_path)) {
        sender_test_set_status("Missing: %s", input_path);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static void sender_test_audio_reset_source(sender_test_audio_session_t *session)
{
    if (session == NULL) {
        return;
    }

    virtual_audio_source_reset(&session->source);
    session->packet_pending = 0;
    session->pending_packet_duration_us = 0U;
    sender_test_audio_reset_timing(session);
}

static int sender_test_audio_reopen_source(sender_test_audio_session_t *session)
{
    if (session == NULL) {
        return DEVICE_VIDEO_ERR_INVALID_ARG;
    }

    virtual_audio_source_close(&session->source);

    int rc = virtual_audio_source_open(&session->source);
    if (rc != DEVICE_VIDEO_OK) {
        device_video_stats_mark_error(&session->stats, rc, "audio_reopen_virtual");
        return rc;
    }

    session->packet_pending = 0;
    session->pending_packet_duration_us = 0U;
    sender_test_audio_reset_timing(session);
    return DEVICE_VIDEO_OK;
}

static int sender_test_run_video(device_video_sender_stats_t *stats)
{
    device_video_config_t config = {0};
    device_video_h264_file_t source = {0};
    bool restart_from_head = false;
    uint64_t next_frame_at_us = 0U;
    int rc = DEVICE_VIDEO_OK;

    if (stats == NULL) {
        return DEVICE_VIDEO_ERR_INVALID_ARG;
    }

    device_video_stats_init(stats);
    virtual_video_source_prepare_config(&config);

    rc = device_video_source_file_open(config.input_path, &source);
    if (rc != DEVICE_VIDEO_OK) {
        device_video_stats_mark_error(stats, rc, "video_open_file");
        return rc;
    }

    ESP_LOGI(TAG,
             "sender test video feeder started: input=%s fps=%d interval_us=%u loop=%d",
             config.input_path,
             config.fps,
             (unsigned)config.frame_interval_us,
             config.loop);

    while (!sender_test_restart_pending()) {
        tirtc_session_stats_t rtc_stats = {0};
        const uint8_t *data_ptr = NULL;
        size_t data_len = 0U;
        int is_key_frame = 0;
        uint64_t now_us = sender_test_now_us();

        tirtc_session_get_stats(&rtc_stats);
        if (!rtc_stats.sdk_started) {
            sender_test_set_status("Video ready, waiting RTC");
            restart_from_head = true;
            next_frame_at_us = 0U;
            device_video_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }
        if (!rtc_stats.active_connection || !rtc_stats.call_active) {
            sender_test_set_status("Video ready, waiting connection");
            restart_from_head = true;
            next_frame_at_us = 0U;
            device_video_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }
        if (!rtc_stats.local_video_send_enabled) {
            sender_test_set_status("Video ready, waiting video enable");
            restart_from_head = true;
            next_frame_at_us = 0U;
            device_video_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }

        if (sender_test_consume_video_restart_request()) {
            restart_from_head = true;
            next_frame_at_us = 0U;
        }

        if (restart_from_head) {
            device_video_source_file_reset(&source);
            next_frame_at_us = 0U;
            restart_from_head = false;
        }

        if (config.frame_interval_us > 0U) {
            if (next_frame_at_us == 0U) {
                next_frame_at_us = now_us;
            }
            if (now_us < next_frame_at_us) {
                uint64_t wait_us = next_frame_at_us - now_us;
                device_video_sleep_ms((int)((wait_us + 999U) / 1000U));
                continue;
            }
        }

        rc = device_video_source_file_next_frame(&source, &data_ptr, &data_len, &is_key_frame);
        if (rc == DEVICE_VIDEO_ERR_EOF && config.loop) {
            device_video_source_file_reset(&source);
            next_frame_at_us = 0U;
            continue;
        }
        if (rc != DEVICE_VIDEO_OK) {
            device_video_stats_mark_error(stats, rc, "video_next_frame");
            break;
        }

        uint64_t frame_pts_us = next_frame_at_us != 0U ? next_frame_at_us : now_us;

        TIRTCFRAMEINFO frame_info = {
            .stream_id = 0,
            .media = TIRTC_VIDEO_H264,
            .flags = is_key_frame ? TIRTC_FRAME_FLAG_KEY_FRAME : 0,
            .reserved = 0,
            .ts = (uint32_t)(frame_pts_us / 1000ULL),
            .length = (uint32_t)data_len,
        };
        esp_err_t send_ret = tirtc_session_send_test_video_frame(&frame_info, data_ptr);
        if (send_ret == ESP_ERR_INVALID_STATE) {
            sender_test_set_status("Video ready, waiting RTC gate");
            restart_from_head = true;
            next_frame_at_us = 0U;
            device_video_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }
        if (send_ret == ESP_ERR_TIMEOUT) {
            device_video_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }
        if (send_ret != ESP_OK) {
            stats->send_failures++;
            device_video_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }

        if (stats->frames_sent == 0U) {
            sender_test_set_status("Video uploading...");
        }
        stats->frames_sent++;
        stats->bytes_sent += data_len;
        if (config.frame_interval_us > 0U) {
            next_frame_at_us = frame_pts_us + (uint64_t)config.frame_interval_us;
        } else {
            next_frame_at_us = 0U;
        }
    }

    stats->ended_at_ms = device_video_now_ms();
    device_video_source_file_close(&source);
    return rc;
}

static int sender_test_audio_send_one_packet(sender_test_audio_session_t *session)
{
    const uint8_t *data_ptr = NULL;
    size_t data_len = 0U;
    const tirtc_session_audio_format_t *format = NULL;
    uint64_t now_us = 0U;
    uint64_t scheduled_send_at_us = 0U;
    int rc = DEVICE_VIDEO_OK;
    tirtc_session_stats_t rtc_stats = {0};

    if (session == NULL) {
        return DEVICE_VIDEO_ERR_INVALID_ARG;
    }

    now_us = sender_test_now_us();
    if (sender_test_consume_audio_restart_request()) {
        session->packet_pending = 0;
        session->packet_enqueued = 0;
        session->pending_send_failures = 0;
        session->first_packet_sent_logged = 0;
        sender_test_audio_reset_source(session);
        now_us = sender_test_now_us();
    }

    if (session->packet_pending && session->next_send_at_us == 0U) {
        session->next_send_at_us = now_us;
    }

    if (session->packet_pending && now_us < session->next_send_at_us) {
        return DEVICE_VIDEO_OK;
    }

    if (!session->packet_pending) {
        rc = virtual_audio_source_next_packet(&session->source,
                                              &data_ptr,
                                              &data_len,
                                              &format,
                                              &session->pending_packet_duration_us);
        if (rc == DEVICE_VIDEO_ERR_IO) {
            if (sender_test_should_log_audio_wait(session, now_us)) {
                ESP_LOGW(TAG, "sender test virtual audio source error: reopen and retry");
            }
            return sender_test_audio_reopen_source(session);
        }
        if (rc != DEVICE_VIDEO_OK) {
            device_video_stats_mark_error(&session->stats, rc, "audio_next_packet");
            return rc;
        }
        session->first_packet_read_logged = 1;
        session->packet_pending = 1;
        if (session->next_send_at_us == 0U) {
            session->next_send_at_us = now_us;
        }
        if (session->media_pts_us == 0U) {
            session->media_pts_us = session->next_send_at_us;
        }
    }

    if (now_us < session->next_send_at_us) {
        return DEVICE_VIDEO_OK;
    }

    scheduled_send_at_us = session->next_send_at_us;
    data_ptr = session->source.packet_buffer;
    data_len = session->source.packet_length;
    format = format != NULL ? format : &session->source.format;

    esp_err_t send_ret = tirtc_session_send_test_audio_pcm_frame(data_ptr,
                                                                 data_len,
                                                                 format,
                                                                 session->media_pts_us);
    if (send_ret == ESP_ERR_INVALID_STATE) {
        tirtc_session_get_stats(&rtc_stats);
        if (sender_test_should_log_audio_wait(session, now_us)) {
            ESP_LOGD(TAG,
                     "sender test audio waiting: ret=%s len=%u stream=%u active=%d call=%d sdk=%d tx_audio=%lu tx_fail=%lu last=%s",
                     esp_err_to_name(send_ret),
                     (unsigned)session->source.packet_length,
                     (unsigned)rtc_stats.local_audio_stream_id,
                     rtc_stats.active_connection,
                     rtc_stats.call_active,
                     rtc_stats.sdk_started,
                     (unsigned long)rtc_stats.tx_audio_frames,
                     (unsigned long)rtc_stats.tx_failures,
                     rtc_stats.last_event);
        }
        if (!rtc_stats.sdk_started) {
            sender_test_set_status("Audio ready, waiting RTC");
        } else if (!rtc_stats.active_connection || !rtc_stats.call_active) {
            sender_test_set_status("Audio ready, waiting connection");
            session->next_send_at_us = 0U;
        } else {
            sender_test_set_status("Audio ready, waiting media");
            if (!session->first_packet_sent_logged) {
                session->packet_pending = 0;
                session->packet_enqueued = 0;
                session->pending_send_failures = 0;
                session->next_send_at_us = now_us;
                session->media_pts_us = now_us;
            } else {
                session->next_send_at_us = scheduled_send_at_us;
            }
        }
        return DEVICE_VIDEO_OK;
    }
    if (send_ret == ESP_ERR_TIMEOUT) {
        if (sender_test_should_log_audio_wait(session, now_us)) {
            ESP_LOGW(TAG, "sender test audio queue full: len=%u", (unsigned)data_len);
        }
        if (!session->first_packet_sent_logged) {
            session->packet_pending = 0;
            session->packet_enqueued = 0;
            session->pending_send_failures = 0;
            session->next_send_at_us = now_us;
            session->media_pts_us = now_us;
        } else {
            session->next_send_at_us = scheduled_send_at_us;
        }
        return DEVICE_VIDEO_OK;
    }
    if (send_ret != ESP_OK) {
        if (sender_test_should_log_audio_wait(session, now_us)) {
            ESP_LOGW(TAG,
                     "sender test audio enqueue failed: ret=%s len=%u",
                     esp_err_to_name(send_ret),
                     (unsigned)data_len);
        }
        session->stats.send_failures++;
        if (!session->first_packet_sent_logged) {
            session->packet_pending = 0;
            session->packet_enqueued = 0;
            session->pending_send_failures = 0;
            session->next_send_at_us = now_us;
            session->media_pts_us = now_us;
        } else {
            session->next_send_at_us = scheduled_send_at_us;
        }
        return DEVICE_VIDEO_OK;
    }
    if (!session->first_packet_sent_logged) {
        session->first_packet_sent_logged = 1;
        session->jank_window_active = true;
        sender_test_set_status("Audio uploading...");
    } else {
        uint64_t interval_us = session->media_pts_us - session->last_sent_media_pts_us;
        session->jank_samples++;
        if (interval_us >= SENDER_TEST_AUDIO_JANK_THRESHOLD_US) {
            session->jank_count++;
        }
    }

    session->last_sent_media_pts_us = session->media_pts_us;

    session->stats.frames_sent++;
    session->stats.bytes_sent += data_len;
    session->packet_pending = 0;
    session->packet_enqueued = 0;
    session->pending_send_failures = 0;
    session->next_send_at_us = scheduled_send_at_us + session->pending_packet_duration_us;
    session->media_pts_us += session->pending_packet_duration_us;
    return DEVICE_VIDEO_OK;
}

static int sender_test_run_audio(device_video_sender_stats_t *stats)
{
    sender_test_audio_session_t session = {0};
    int rc = DEVICE_VIDEO_OK;

    if (stats == NULL) {
        return DEVICE_VIDEO_ERR_INVALID_ARG;
    }

    device_video_stats_init(&session.stats);
    rc = virtual_audio_source_open(&session.source);
    if (rc != DEVICE_VIDEO_OK) {
        device_video_stats_mark_error(&session.stats, rc, "audio_open_virtual");
        *stats = session.stats;
        return rc;
    }

    sender_test_set_status("Audio ready, waiting connection");

    while (!sender_test_restart_pending()) {
        tirtc_session_stats_t rtc_stats = {0};
        bool packet_sent = false;
        uint32_t burst_count = 0U;

        tirtc_session_get_stats(&rtc_stats);
        if (!rtc_stats.sdk_started) {
            sender_test_log_audio_jank_summary(&session, "rtc-stopped");
            sender_test_set_status("Audio ready, waiting RTC");
            sender_test_audio_reset_timing(&session);
            device_video_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }
        if (!rtc_stats.active_connection || !rtc_stats.call_active) {
            sender_test_log_audio_jank_summary(&session, "disconnect");
            sender_test_set_status("Audio ready, waiting connection");
            sender_test_audio_reset_timing(&session);
            device_video_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }

        do {
            uint32_t frames_before = session.stats.frames_sent;

            rc = sender_test_audio_send_one_packet(&session);
            if (rc != DEVICE_VIDEO_OK) {
                if (session.stats.last_error_code == 0) {
                    device_video_stats_mark_error(&session.stats, rc, "audio_session_run");
                }
                break;
            }

            packet_sent = session.stats.frames_sent != frames_before;
            if (!packet_sent) {
                break;
            }

            burst_count++;
        } while (burst_count < SENDER_TEST_AUDIO_CATCH_UP_BURST &&
                 !sender_test_restart_pending() &&
                 session.next_send_at_us != 0U &&
                 sender_test_now_us() >= session.next_send_at_us);

        if (rc != DEVICE_VIDEO_OK) {
            break;
        }

        if (packet_sent) {
            int wait_ms = sender_test_audio_sleep_until(session.next_send_at_us);
            if (wait_ms > 0) {
                device_video_sleep_ms(wait_ms);
            }
        } else {
            device_video_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
        }
    }

    session.stats.ended_at_ms = device_video_now_ms();
    virtual_audio_source_close(&session.source);
    sender_test_log_audio_jank_summary(&session, "exit");
    *stats = session.stats;
    return rc;
}

static void sender_test_finalize_run(sender_test_mode_t mode,
                                              int rc,
                                              const device_video_sender_stats_t *stats,
                                              bool restart_requested,
                                              bool stop_requested)
{
    bool refresh_policy = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    if (!restart_requested) {
        s_sender_test_running = false;
        s_sender_test_task = NULL;
        s_sender_test_requested_mode = SENDER_TEST_MODE_NONE;
        s_sender_test_stop_requested = false;
        refresh_policy = true;
        if (stop_requested) {
            sender_test_set_status_locked("Stopped");
        } else if (rc == DEVICE_VIDEO_OK) {
            sender_test_set_status_locked(mode == SENDER_TEST_MODE_AUDIO ? "Audio exited" : "Video exited");
        } else if (stats != NULL) {
            snprintf(s_sender_test_status,
                     sizeof(s_sender_test_status),
                     "%s failed %s (%d)",
                     sender_test_mode_label(mode),
                     stats->last_error_stage[0] != '\0' ? stats->last_error_stage : "unknown",
                     stats->last_error_code != 0 ? stats->last_error_code : rc);
        }
    }
    taskEXIT_CRITICAL(&s_sender_test_lock);

    if (refresh_policy) {
        tirtc_session_refresh_media_policy();
    }
}

static void sender_test_task_entry(void *ctx)
{
    (void)ctx;

    while (true) {
        sender_test_mode_t mode = SENDER_TEST_MODE_NONE;
        device_video_sender_stats_t stats = {0};
        int rc = DEVICE_VIDEO_OK;
        bool restart_requested = false;
        bool stop_requested = false;

        taskENTER_CRITICAL(&s_sender_test_lock);
        mode = s_sender_test_requested_mode;
        s_sender_test_restart_requested = false;
        s_sender_test_force_video_restart = false;
        s_sender_test_force_audio_restart = false;
        taskEXIT_CRITICAL(&s_sender_test_lock);

        if (!s_sender_test_initialized) {
            esp_err_t init_ret = sender_test_init();
            if (init_ret != ESP_OK) {
                sender_test_set_status("Sender init failed (%s)", esp_err_to_name(init_ret));
                sender_test_finalize_run(mode, DEVICE_VIDEO_ERR_IO, &stats, false, false);
                break;
            }
        }

        if (sender_test_validate_mode(mode) != ESP_OK) {
            sender_test_finalize_run(mode, DEVICE_VIDEO_ERR_INVALID_ARG, &stats, false, false);
            break;
        }

        if (mode == SENDER_TEST_MODE_VIDEO) {
            sender_test_set_status("Video ready, waiting connection");
            rc = sender_test_run_video(&stats);
        } else {
            sender_test_set_status("Audio ready, waiting connection");
            rc = sender_test_run_audio(&stats);
        }

        taskENTER_CRITICAL(&s_sender_test_lock);
        restart_requested = s_sender_test_restart_requested;
        stop_requested = s_sender_test_stop_requested;
        taskEXIT_CRITICAL(&s_sender_test_lock);
        if (restart_requested && !stop_requested) {
            sender_test_finalize_run(mode, rc, &stats, true, false);
            continue;
        }

        sender_test_finalize_run(mode, rc, &stats, false, stop_requested);
        break;
    }

    vTaskDeleteWithCaps(NULL);
}

esp_err_t sender_test_init(void)
{
    if (s_sender_test_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = sender_test_mount_spiffs();
    if (ret != ESP_OK) {
        return ret;
    }

    s_sender_test_initialized = true;
    return ESP_OK;
}

esp_err_t sender_test_start(sender_test_mode_t mode)
{
    BaseType_t task_ok = pdFAIL;
    bool should_restart = false;

    if (mode != SENDER_TEST_MODE_VIDEO && mode != SENDER_TEST_MODE_AUDIO) {
        sender_test_set_status("Invalid test mode");
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_sender_test_lock);
    s_sender_test_requested_mode = mode;
    s_sender_test_stop_requested = false;
    if (s_sender_test_task != NULL) {
        s_sender_test_running = true;
        s_sender_test_restart_requested = true;
        should_restart = true;
        sender_test_set_status_locked(mode == SENDER_TEST_MODE_AUDIO
                                               ? "Restarting audio source..."
                                               : "Restarting video source...");
    } else {
        s_sender_test_running = true;
        s_sender_test_restart_requested = false;
        sender_test_set_status_locked("Launching test source...");
    }
    taskEXIT_CRITICAL(&s_sender_test_lock);

    if (should_restart) {
        return ESP_OK;
    }

    task_ok = sender_test_create_task(&s_sender_test_task);
    if (task_ok != pdPASS) {
        sender_test_log_heap("sender task alloc failed");
        taskENTER_CRITICAL(&s_sender_test_lock);
        s_sender_test_running = false;
        s_sender_test_task = NULL;
        sender_test_set_status_locked("Sender task alloc failed");
        taskEXIT_CRITICAL(&s_sender_test_lock);
        return ESP_ERR_NO_MEM;
    }

    tirtc_session_refresh_media_policy();

    return ESP_OK;
}

void sender_test_stop(void)
{
    bool refresh_policy = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    if (s_sender_test_task != NULL || s_sender_test_running) {
        s_sender_test_stop_requested = true;
        s_sender_test_restart_requested = false;
        s_sender_test_force_video_restart = false;
        s_sender_test_force_audio_restart = false;
        s_sender_test_running = false;
        s_sender_test_requested_mode = SENDER_TEST_MODE_NONE;
        sender_test_set_status_locked("Stopping...");
        refresh_policy = true;
    } else {
        s_sender_test_stop_requested = false;
        s_sender_test_requested_mode = SENDER_TEST_MODE_NONE;
    }
    taskEXIT_CRITICAL(&s_sender_test_lock);

    if (refresh_policy) {
        tirtc_session_refresh_media_policy();
    }
}

bool sender_test_is_mode_active(sender_test_mode_t mode)
{
    bool active = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    active = s_sender_test_running && s_sender_test_requested_mode == mode;
    taskEXIT_CRITICAL(&s_sender_test_lock);

    return active;
}

void sender_test_request_video_restart(void)
{
    taskENTER_CRITICAL(&s_sender_test_lock);
    if (s_sender_test_running && s_sender_test_requested_mode == SENDER_TEST_MODE_VIDEO) {
        s_sender_test_force_video_restart = true;
    }
    taskEXIT_CRITICAL(&s_sender_test_lock);
}

void sender_test_request_audio_restart(void)
{
    taskENTER_CRITICAL(&s_sender_test_lock);
    if (s_sender_test_running && s_sender_test_requested_mode == SENDER_TEST_MODE_AUDIO) {
        s_sender_test_force_audio_restart = true;
    }
    taskEXIT_CRITICAL(&s_sender_test_lock);
}

void sender_test_get_snapshot(sender_test_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    taskENTER_CRITICAL(&s_sender_test_lock);
    snapshot->running = s_sender_test_running;
    snapshot->spiffs_ready = s_sender_test_spiffs_ready;
    strlcpy(snapshot->status, s_sender_test_status, sizeof(snapshot->status));
    taskEXIT_CRITICAL(&s_sender_test_lock);
}
