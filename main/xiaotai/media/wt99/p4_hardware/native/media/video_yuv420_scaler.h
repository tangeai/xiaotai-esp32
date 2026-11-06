#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct video_yuv420_scaler *video_yuv420_scaler_handle_t;

typedef struct {
    uint16_t input_width;
    uint16_t input_height;
    uint16_t output_width;
    uint16_t output_height;
    bool rotate_ccw90;
} video_yuv420_scaler_config_t;

esp_err_t video_yuv420_scaler_create(const video_yuv420_scaler_config_t *config,
                                     video_yuv420_scaler_handle_t *out_handle);

bool video_yuv420_scaler_matches(video_yuv420_scaler_handle_t handle,
                                 const video_yuv420_scaler_config_t *config);

/* Run one full-size transaction so PPA/DMA setup is paid before a live call. */
esp_err_t video_yuv420_scaler_warmup(video_yuv420_scaler_handle_t handle);

esp_err_t video_yuv420_scaler_process(video_yuv420_scaler_handle_t handle,
                                      const uint8_t *input,
                                      size_t input_len,
                                      const uint8_t **output,
                                      size_t *output_len);

void video_yuv420_scaler_destroy(video_yuv420_scaler_handle_t handle);

#ifdef __cplusplus
}
#endif
