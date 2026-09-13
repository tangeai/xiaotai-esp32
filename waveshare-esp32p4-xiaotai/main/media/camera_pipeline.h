#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef esp_err_t (*camera_pipeline_video_cb_t)(const uint8_t *data,
                                               size_t data_len,
                                               uint16_t width,
                                               uint16_t height,
                                               uint64_t pts_us,
                                               uint8_t media,
                                               bool key_frame,
                                               void *ctx);

typedef struct {
    bool running;
    bool rtc_enabled;
    uint16_t width;
    uint16_t height;
    uint8_t target_fps;
    uint32_t configured_bitrate_bps;
    uint32_t measured_fps_x10;
    uint32_t measured_bitrate_kbps;
    uint32_t avg_payload_bytes;
    uint32_t dropped_frames;
    uint32_t capture_failures;
    uint32_t encode_failures;
    bool direct_input;
} camera_pipeline_metrics_t;

esp_err_t camera_pipeline_init(void);
esp_err_t camera_pipeline_prewarm_h264(void);
esp_err_t camera_pipeline_prewarm_call_scaler(void);
void camera_pipeline_on_rtc_video_config_changed(void);
void camera_pipeline_request_key_frame(void);
void camera_pipeline_request_stream_start_key_frame(void);
esp_err_t camera_pipeline_set_rtc_video_sink(camera_pipeline_video_cb_t cb, void *ctx);
esp_err_t camera_pipeline_set_rtc_video_enabled(bool enabled);
bool camera_pipeline_is_running(void);
bool camera_pipeline_is_rtc_video_active(void);
void camera_pipeline_get_metrics(camera_pipeline_metrics_t *metrics);
