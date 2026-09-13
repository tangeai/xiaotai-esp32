#include "rtc_media_bridge.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "audio_alaw_codec.h"
#include "audio_device.h"
#include "call_video_renderer.h"
#include "camera_video_source.h"
#include "media_sink.h"
#include "tiRTC.h"

static const char *TAG = "rtc_media_bridge";

#define RTC_MEDIA_BRIDGE_REMOTE_AUDIO_MAX_PAYLOAD 8192U

static tirtc_session_capture_frame_cb_t s_capture_cb;
static void *s_capture_cb_ctx;
static camera_video_source_submit_cb_t s_external_video_sink;
static void *s_external_video_sink_ctx;
static portMUX_TYPE s_bridge_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_remote_audio_first_logged;
static bool s_remote_audio_unsupported_logged;
static bool s_remote_audio_submit_failed_logged;
static bool s_remote_audio_bad_length_logged;

static esp_err_t rtc_media_bridge_submit_local_video(const uint8_t *data,
                                                     size_t data_len,
                                                     uint16_t width,
                                                     uint16_t height,
                                                     uint64_t pts_us,
                                                     uint8_t media,
                                                     bool key_frame,
                                                     void *ctx);

static const char *rtc_media_bridge_audio_media_name(uint8_t media)
{
    switch (media) {
    case TIRTC_AUDIO_PCM:
        return "pcm";
    case TIRTC_AUDIO_ALAW:
        return "alaw";
    case TIRTC_AUDIO_AAC:
        return "aac";
    default:
        return "unknown";
    }
}

static bool rtc_media_bridge_take_log_once(bool *flag)
{
    bool should_log = false;

    taskENTER_CRITICAL(&s_bridge_lock);
    if (flag != NULL && !*flag) {
        *flag = true;
        should_log = true;
    }
    taskEXIT_CRITICAL(&s_bridge_lock);
    return should_log;
}

static void rtc_media_bridge_audio_capture_cb(const uint8_t *data,
                                              size_t data_len,
                                              const audio_format_t *format,
                                              void *ctx)
{
    (void)ctx;

    tirtc_session_capture_frame_cb_t capture_cb = NULL;
    void *capture_cb_ctx = NULL;

    taskENTER_CRITICAL(&s_bridge_lock);
    capture_cb = s_capture_cb;
    capture_cb_ctx = s_capture_cb_ctx;
    taskEXIT_CRITICAL(&s_bridge_lock);

    if (capture_cb == NULL || format == NULL) {
        return;
    }

    tirtc_session_audio_format_t rtc_format = {
        .sample_rate_hz = format->sample_rate_hz,
        .channels = format->channels,
        .bits_per_sample = format->bits_per_sample,
    };

    capture_cb(data, data_len, &rtc_format, capture_cb_ctx);
}

static void rtc_media_bridge_set_capture_frame_cb(tirtc_session_capture_frame_cb_t cb,
                                                  void *cb_ctx,
                                                  void *ctx)
{
    (void)ctx;

    taskENTER_CRITICAL(&s_bridge_lock);
    s_capture_cb = cb;
    s_capture_cb_ctx = cb_ctx;
    taskEXIT_CRITICAL(&s_bridge_lock);
    microphone_set_frame_cb(cb != NULL ? rtc_media_bridge_audio_capture_cb : NULL, NULL);
}

static esp_err_t rtc_media_bridge_init(void *ctx)
{
    (void)ctx;

    ESP_RETURN_ON_ERROR(media_sink_init(), TAG, "media sink init failed");
    return camera_video_source_init(rtc_media_bridge_submit_local_video, NULL);
}

static esp_err_t rtc_media_bridge_set_capture_enabled(bool enabled, void *ctx)
{
    (void)ctx;

    if (enabled) {
        bool callback_ready = false;

        taskENTER_CRITICAL(&s_bridge_lock);
        callback_ready = s_capture_cb != NULL;
        taskEXIT_CRITICAL(&s_bridge_lock);
        ESP_RETURN_ON_FALSE(callback_ready,
                            ESP_ERR_INVALID_STATE,
                            TAG,
                            "local audio capture callback is not configured");

        /* The shared audio device clears its active callback when suspended.
         * Rebind the long-lived TiRTC bridge whenever a new call enables input. */
        microphone_set_frame_cb(rtc_media_bridge_audio_capture_cb, NULL);
    }
    return microphone_set_enabled(enabled);
}

static esp_err_t rtc_media_bridge_set_video_capture_enabled(bool enabled, void *ctx)
{
    (void)ctx;

    if (!enabled) {
        bool external_video_active = false;

        taskENTER_CRITICAL(&s_bridge_lock);
        external_video_active = s_external_video_sink != NULL;
        taskEXIT_CRITICAL(&s_bridge_lock);
        if (external_video_active) {
            return ESP_OK;
        }
    }
    return camera_video_source_set_enabled(enabled);
}

static void rtc_media_bridge_request_video_key_frame(void *ctx)
{
    (void)ctx;

    camera_video_source_request_key_frame();
}

static void rtc_media_bridge_request_video_stream_start_key_frame(void *ctx)
{
    (void)ctx;

    camera_video_source_request_stream_start_key_frame();
}

static esp_err_t rtc_media_bridge_prepare_playback_path(void *ctx)
{
    (void)ctx;

    return speaker_prepare_playback_path();
}

static bool rtc_media_bridge_map_tirtc_audio_format(uint8_t flags, audio_format_t *format)
{
    if (format == NULL) {
        return false;
    }

    switch (flags) {
    case TIRTC_AUDIOSAMPLE_8K16B1C:
        format->sample_rate_hz = 8000;
        format->bits_per_sample = 16;
        format->channels = 1;
        return true;
    case TIRTC_AUDIOSAMPLE_16K16B1C:
        format->sample_rate_hz = 16000;
        format->bits_per_sample = 16;
        format->channels = 1;
        return true;
    case TIRTC_AUDIOSAMPLE_8K16B2C:
        format->sample_rate_hz = 8000;
        format->bits_per_sample = 16;
        format->channels = 2;
        return true;
    case TIRTC_AUDIOSAMPLE_16K16B2C:
        format->sample_rate_hz = 16000;
        format->bits_per_sample = 16;
        format->channels = 2;
        return true;
    default:
        return false;
    }
}

static esp_err_t rtc_media_bridge_submit_remote_audio(uint8_t media,
                                                      uint8_t flags,
                                                      const uint8_t *data,
                                                      size_t data_len,
                                                      uint32_t pts,
                                                      size_t *playback_data_len,
                                                      void *ctx)
{
    (void)ctx;

    audio_format_t format = {0};
    uint8_t *decoded_data = NULL;
    size_t pcm_data_len = data_len;
    esp_err_t submit_ret = ESP_OK;

    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid remote audio packet");
    if (data_len > RTC_MEDIA_BRIDGE_REMOTE_AUDIO_MAX_PAYLOAD) {
        if (rtc_media_bridge_take_log_once(&s_remote_audio_bad_length_logged)) {
            ESP_LOGW(TAG,
                     "remote audio rejected: payload=%u max=%u media=%u(%s) flags=%u",
                     (unsigned)data_len,
                     RTC_MEDIA_BRIDGE_REMOTE_AUDIO_MAX_PAYLOAD,
                     media,
                     rtc_media_bridge_audio_media_name(media),
                     flags);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    if (media == TIRTC_AUDIO_ALAW) {
        if (!rtc_media_bridge_map_tirtc_audio_format(flags, &format)) {
            if (rtc_media_bridge_take_log_once(&s_remote_audio_unsupported_logged)) {
                ESP_LOGW(TAG,
                         "unsupported remote alaw flags: flags=%u payload=%u",
                         flags,
                         (unsigned)data_len);
            }
            return ESP_ERR_NOT_SUPPORTED;
        }

        esp_err_t decode_ret = audio_alaw_decode(data, data_len, &decoded_data, &pcm_data_len);
        if (decode_ret != ESP_OK) {
            if (rtc_media_bridge_take_log_once(&s_remote_audio_unsupported_logged)) {
                ESP_LOGW(TAG,
                         "remote alaw decode failed: ret=%s payload=%u flags=%u",
                         esp_err_to_name(decode_ret),
                         (unsigned)data_len,
                         flags);
            }
            return decode_ret;
        }
    } else if (media == TIRTC_AUDIO_PCM) {
        if (!rtc_media_bridge_map_tirtc_audio_format(flags, &format)) {
            if (rtc_media_bridge_take_log_once(&s_remote_audio_unsupported_logged)) {
                ESP_LOGW(TAG,
                         "unsupported remote pcm flags: flags=%u payload=%u",
                         flags,
                         (unsigned)data_len);
            }
            return ESP_ERR_NOT_SUPPORTED;
        }
    } else {
        if (rtc_media_bridge_take_log_once(&s_remote_audio_unsupported_logged)) {
            ESP_LOGW(TAG,
                     "unsupported remote audio media: media=%u(%s) flags=%u payload=%u",
                     media,
                     rtc_media_bridge_audio_media_name(media),
                     flags,
                     (unsigned)data_len);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (playback_data_len != NULL) {
        *playback_data_len = pcm_data_len;
    }

    if (decoded_data != NULL) {
        submit_ret = media_sink_submit_remote_audio_owned(decoded_data,
                                                          pcm_data_len,
                                                          &format,
                                                          pts);
        if (submit_ret == ESP_OK) {
            decoded_data = NULL;
        }
    } else {
        submit_ret = media_sink_submit_remote_audio(data,
                                                    data_len,
                                                    &format,
                                                    pts);
    }

    if (submit_ret == ESP_OK) {
        if (rtc_media_bridge_take_log_once(&s_remote_audio_first_logged)) {
            ESP_LOGI(TAG,
                     "remote audio accepted: media=%u(%s) flags=%u payload=%u pcm=%u format=%luHz/%ubit/%uch",
                     media,
                     rtc_media_bridge_audio_media_name(media),
                     flags,
                     (unsigned)data_len,
                     (unsigned)pcm_data_len,
                     (unsigned long)format.sample_rate_hz,
                     format.bits_per_sample,
                     format.channels);
        }
    } else if (rtc_media_bridge_take_log_once(&s_remote_audio_submit_failed_logged)) {
        ESP_LOGW(TAG,
                 "remote audio queue failed: ret=%s media=%u(%s) flags=%u payload=%u pcm=%u",
                 esp_err_to_name(submit_ret),
                 media,
                 rtc_media_bridge_audio_media_name(media),
                 flags,
                 (unsigned)data_len,
                 (unsigned)pcm_data_len);
    }

    free(decoded_data);
    return submit_ret;
}

static esp_err_t rtc_media_bridge_submit_remote_video(uint8_t media,
                                                      uint8_t flags,
                                                      const uint8_t *data,
                                                      size_t data_len,
                                                      uint32_t pts,
                                                      void *ctx)
{
    (void)ctx;

    if (media == TIRTC_VIDEO_H264) {
        return call_video_renderer_submit_h264(data,
                                               data_len,
                                               (flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0U,
                                               pts);
    }
    if (media == TIRTC_VIDEO_JPEG) {
        return call_video_renderer_submit_mjpeg(data, data_len, pts);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static bool rtc_media_bridge_remote_video_requires_key_frame(void *ctx)
{
    (void)ctx;

    return call_video_renderer_requires_key_frame();
}

static esp_err_t rtc_media_bridge_submit_local_video(const uint8_t *data,
                                                     size_t data_len,
                                                     uint16_t width,
                                                     uint16_t height,
                                                     uint64_t pts_us,
                                                     uint8_t media,
                                                     bool key_frame,
                                                     void *ctx)
{
    (void)ctx;

    camera_video_source_submit_cb_t external_sink = NULL;
    void *external_ctx = NULL;

    taskENTER_CRITICAL(&s_bridge_lock);
    external_sink = s_external_video_sink;
    external_ctx = s_external_video_sink_ctx;
    taskEXIT_CRITICAL(&s_bridge_lock);
    if (external_sink != NULL) {
        return external_sink(data,
                             data_len,
                             width,
                             height,
                             pts_us,
                             media,
                             key_frame,
                             external_ctx);
    }

    return tirtc_session_send_local_video_frame(data,
                                                data_len,
                                                width,
                                                height,
                                                pts_us,
                                                media,
                                                key_frame ? TIRTC_FRAME_FLAG_KEY_FRAME : 0);
}

static void rtc_media_bridge_flush(void *ctx)
{
    (void)ctx;

    media_sink_flush();
    call_video_renderer_flush();
    taskENTER_CRITICAL(&s_bridge_lock);
    s_remote_audio_first_logged = false;
    s_remote_audio_unsupported_logged = false;
    s_remote_audio_submit_failed_logged = false;
    s_remote_audio_bad_length_logged = false;
    taskEXIT_CRITICAL(&s_bridge_lock);
}

static const tirtc_session_media_ops_t s_rtc_media_bridge_ops = {
    .init = rtc_media_bridge_init,
    .set_capture_frame_cb = rtc_media_bridge_set_capture_frame_cb,
    .set_capture_enabled = rtc_media_bridge_set_capture_enabled,
    .set_video_capture_enabled = rtc_media_bridge_set_video_capture_enabled,
    .request_video_key_frame = rtc_media_bridge_request_video_key_frame,
    .request_video_stream_start_key_frame =
        rtc_media_bridge_request_video_stream_start_key_frame,
    .prepare_playback_path = rtc_media_bridge_prepare_playback_path,
    .submit_remote_audio = rtc_media_bridge_submit_remote_audio,
    .submit_remote_video = rtc_media_bridge_submit_remote_video,
    .remote_video_requires_key_frame =
        rtc_media_bridge_remote_video_requires_key_frame,
    .flush = rtc_media_bridge_flush,
};

const tirtc_session_media_ops_t *rtc_media_bridge_get_ops(void)
{
    return &s_rtc_media_bridge_ops;
}

esp_err_t rtc_media_bridge_register_external_video_sink(camera_video_source_submit_cb_t cb,
                                                        void *ctx)
{
    ESP_RETURN_ON_FALSE(cb != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "external video sink is null");

    esp_err_t ret = ESP_OK;
    taskENTER_CRITICAL(&s_bridge_lock);
    if (s_external_video_sink != NULL &&
        (s_external_video_sink != cb || s_external_video_sink_ctx != ctx)) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_external_video_sink = cb;
        s_external_video_sink_ctx = ctx;
    }
    taskEXIT_CRITICAL(&s_bridge_lock);
    return ret;
}

void rtc_media_bridge_unregister_external_video_sink(camera_video_source_submit_cb_t cb,
                                                     void *ctx)
{
    taskENTER_CRITICAL(&s_bridge_lock);
    if (s_external_video_sink == cb && s_external_video_sink_ctx == ctx) {
        s_external_video_sink = NULL;
        s_external_video_sink_ctx = NULL;
    }
    taskEXIT_CRITICAL(&s_bridge_lock);
}

void *rtc_media_bridge_get_context(void)
{
    return NULL;
}
