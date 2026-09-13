#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
	CAMERA_DRIVER_PIXEL_FORMAT_GRAYSCALE = 0,
	CAMERA_DRIVER_PIXEL_FORMAT_RGB565,
	CAMERA_DRIVER_PIXEL_FORMAT_YUV420_OUYY_EVYY,
} camera_driver_pixel_format_t;

typedef struct {
	const uint8_t *data;
	size_t data_len;
	uint16_t width;
	uint16_t height;
	camera_driver_pixel_format_t pixel_format;
	uint32_t sequence;
	uint32_t stale_frames_dropped;
	uint64_t sensor_timestamp_us;
	void *owner;
} camera_driver_frame_t;

bool camera_driver_is_configured(void);
esp_err_t camera_driver_prepare_video_subsystem(void);
esp_err_t camera_driver_set_stream_target(uint16_t width, uint16_t height, uint8_t fps);
esp_err_t camera_driver_init(void);
esp_err_t camera_driver_acquire(void);
void camera_driver_release_device(void);
esp_err_t camera_driver_capture(camera_driver_frame_t *frame);
void camera_driver_release(camera_driver_frame_t *frame);
esp_err_t camera_driver_deinit(void);
