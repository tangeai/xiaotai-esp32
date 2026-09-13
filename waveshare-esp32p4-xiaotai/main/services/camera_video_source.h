#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef esp_err_t (*camera_video_source_submit_cb_t)(const uint8_t *data,
                                                     size_t data_len,
                                                     uint16_t width,
                                                     uint16_t height,
                                                     uint64_t pts_us,
                                                     uint8_t media,
                                                     bool key_frame,
                                                     void *ctx);

esp_err_t camera_video_source_init(camera_video_source_submit_cb_t cb, void *ctx);
esp_err_t camera_video_source_set_enabled(bool enabled);
void camera_video_source_request_key_frame(void);
void camera_video_source_request_stream_start_key_frame(void);
bool camera_video_source_is_running(void);
