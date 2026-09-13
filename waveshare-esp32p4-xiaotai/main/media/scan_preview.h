#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SCAN_PREVIEW_PIXEL_FORMAT_GRAYSCALE = 0,
    SCAN_PREVIEW_PIXEL_FORMAT_RGB565,
    SCAN_PREVIEW_PIXEL_FORMAT_YUV420_OUYY_EVYY,
} scan_preview_pixel_format_t;

/* The frame is borrowed from the camera and remains valid only while the
 * callback is running. Consumers must render or copy it synchronously. */
typedef struct {
    const uint8_t *data;
    size_t data_len;
    uint16_t width;
    uint16_t height;
    scan_preview_pixel_format_t pixel_format;
} scan_preview_frame_t;

typedef void (*scan_preview_cb_t)(const scan_preview_frame_t *frame, void *ctx);
