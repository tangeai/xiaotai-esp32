#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XIAOTAI_CAMERA_UNKNOWN = 0,
    XIAOTAI_CAMERA_YUYV422,
} xiaotai_camera_format_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t width;
    size_t height;
    int64_t captured_us;
    xiaotai_camera_format_t format;
} xiaotai_camera_frame_t;

// Enable sensor power and wait for board-specific settling before returning.
// One worker owns all calls. After powering on, recheck admission before open.
// After an open attempt, close before powering down or opening another mode.
// Do not power down while a frame is borrowed.
esp_err_t xiaotai_board_camera_set_power(bool enabled);
esp_err_t xiaotai_board_camera_open(unsigned width, unsigned height);
esp_err_t xiaotai_board_camera_close(void);

// ESP_ERR_NOT_FOUND means an empty queue, with no blocking driver read attempted.
// On success this exact descriptor holds the only read-only loan, until release.
// Return it before encoding output is queued, reconfiguring, or closing.
esp_err_t xiaotai_board_camera_borrow(xiaotai_camera_frame_t *frame);
esp_err_t xiaotai_board_camera_release(xiaotai_camera_frame_t *frame);

#ifdef __cplusplus
}
#endif
