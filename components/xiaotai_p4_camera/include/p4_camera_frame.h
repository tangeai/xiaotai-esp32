#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CAMERA_DRIVER_PIXEL_FORMAT_GRAYSCALE = 0,
    CAMERA_DRIVER_PIXEL_FORMAT_RGB565,
    CAMERA_DRIVER_PIXEL_FORMAT_YUV420_OUYY_EVYY,
} camera_driver_pixel_format_t;

// The P4 CSI YUV420 stream is packed by row pairs (OUYY/EVYY), not I420
// or S3 YUYV422. Width and height are visible pixels. data_len may include
// a board-specific DMA tail. Zero sequence/timestamp can mean unavailable.
typedef struct {
    const uint8_t *data;
    size_t data_len;
    uint16_t width;
    uint16_t height;
    camera_driver_pixel_format_t pixel_format;
    uint32_t sequence;
    uint32_t stale_frames_dropped;
    uint64_t sensor_timestamp_us;
} camera_driver_frame_t;

// After rejecting DQBUF's error flag, use the negotiated image size if
// bytesused was omitted, not the USERPTR capacity. Zero rejects the loan.
static inline size_t p4_camera_payload_size(size_t used, size_t image, size_t capacity,
                                            uint16_t width, uint16_t height)
{
    if (!width || !height || ((width | height) & 1U)) return 0;
    size_t pairs = (size_t)(width / 2U) * height;
    if (pairs > SIZE_MAX / 3U) return 0;
    size_t visible = pairs * 3U;
    size_t size = used ? used : image;
    return size >= visible && size <= capacity ? size : 0;
}
