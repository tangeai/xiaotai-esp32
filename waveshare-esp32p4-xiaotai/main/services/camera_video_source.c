#include "camera_video_source.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "camera_pipeline.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static camera_video_source_submit_cb_t s_submit_cb;
static void *s_submit_ctx;

static esp_err_t camera_video_source_submit_bridge(const uint8_t *data,
                                                   size_t data_len,
                                                   uint16_t width,
                                                   uint16_t height,
                                                   uint64_t pts_us,
                                                   uint8_t media,
                                                   bool key_frame,
                                                   void *ctx)
{
    (void)ctx;

    camera_video_source_submit_cb_t cb = NULL;
    void *submit_ctx = NULL;

    taskENTER_CRITICAL(&s_lock);
    cb = s_submit_cb;
    submit_ctx = s_submit_ctx;
    taskEXIT_CRITICAL(&s_lock);

    if (cb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return cb(data, data_len, width, height, pts_us, media, key_frame, submit_ctx);
}

esp_err_t camera_video_source_init(camera_video_source_submit_cb_t cb, void *ctx)
{
    taskENTER_CRITICAL(&s_lock);
    s_submit_cb = cb;
    s_submit_ctx = ctx;
    taskEXIT_CRITICAL(&s_lock);

    esp_err_t ret = camera_pipeline_init();
    if (ret != ESP_OK) {
        return ret;
    }
    return camera_pipeline_set_rtc_video_sink(camera_video_source_submit_bridge, NULL);
}

esp_err_t camera_video_source_set_enabled(bool enabled)
{
    return camera_pipeline_set_rtc_video_enabled(enabled);
}

void camera_video_source_request_key_frame(void)
{
    camera_pipeline_request_key_frame();
}

void camera_video_source_request_stream_start_key_frame(void)
{
    camera_pipeline_request_stream_start_key_frame();
}

bool camera_video_source_is_running(void)
{
    return camera_pipeline_is_rtc_video_active();
}
