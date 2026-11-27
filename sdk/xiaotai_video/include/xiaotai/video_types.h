// SPDX-License-Identifier: MIT
#pragma once
/* Video engine: value types shared by its API (xiaotai/video.h), its backends
 * (xiaotai/video_backend.h) and whatever carries video for it (a link,
 * xiaotai/video.h). Nothing here names a codec library, a chip or a board. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/codec.h"
#include "xiaotai/hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ codecs */

/* Encoded video on the network: the video values of the device's one codec
 * vocabulary (xiaotai/codec.h), shared with links and parts. A new codec is
 * one more value there and one more encoder or decoder backend. The values
 * are never stored or sent on the wire, so the move to the shared numbering
 * (JPEG 3, H.264 4) changes nothing outside the build. */
typedef xiaotai_codec_t xiaotai_video_codec_t;
#define XIAOTAI_VIDEO_CODEC_NONE XIAOTAI_CODEC_NONE
#define XIAOTAI_VIDEO_CODEC_JPEG XIAOTAI_CODEC_JPEG   /* one complete JPEG per frame (FF D8 ... FF D9); every frame is a key frame */
#define XIAOTAI_VIDEO_CODEC_H264 XIAOTAI_CODEC_H264   /* one Annex-B access unit per frame; key frame: IDR with SPS and PPS */

/* Bit of a codec in a codec mask (capabilities); codec values stay below 32. */
#define XIAOTAI_VIDEO_CODEC_BIT(codec) (1u << (unsigned)(codec))

/* ------------------------------------------------------------------ pixels */

/* Layout of an uncompressed picture. The two 4:2:0 layouts are different byte
 * streams and are never interchangeable without a transform. New layouts are
 * added at the end. */
typedef enum {
    XIAOTAI_VIDEO_PIXEL_NONE = 0,
    /* Packed 4:2:2, bytes Y0 U Y1 V; 2 bytes per pixel. */
    XIAOTAI_VIDEO_PIXEL_YUYV422,
    /* 4:2:0 packed by row pairs: the first row of a pair is U Y Y, the second
     * V Y Y; 1.5 bytes per pixel. What a CSI pipeline delivers and a hardware
     * H.264 encoder reads. */
    XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED,
    /* Planar 4:2:0: the Y plane, then U, then V; 1.5 bytes per pixel. */
    XIAOTAI_VIDEO_PIXEL_I420,
    /* 16-bit RGB in the CPU's byte order. */
    XIAOTAI_VIDEO_PIXEL_RGB565,
    /* 16-bit RGB, high byte first (panels fed over a byte-wide bus). */
    XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED,
    /* 24-bit RGB, 3 bytes per pixel: blue at the lowest address, then green,
     * then red (a 0xRRGGBB value stored little-endian in 3 bytes). What a
     * display whose descriptor says XIAOTAI_PIXEL_RGB888 holds. */
    XIAOTAI_VIDEO_PIXEL_RGB888,
} xiaotai_video_pixel_t;

/* Bytes of a tightly packed picture of `rows` rows. 0 for an unknown layout,
 * an empty picture, or odd dimensions in a 4:2:0 layout (which has none). */
static inline size_t xiaotai_video_image_bytes(xiaotai_video_pixel_t pixel, uint16_t width, uint16_t rows)
{
    const size_t pixels = (size_t)width * (size_t)rows;
    switch (pixel) {
    case XIAOTAI_VIDEO_PIXEL_YUYV422:
    case XIAOTAI_VIDEO_PIXEL_RGB565:
    case XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED:
        return pixels * 2u;
    case XIAOTAI_VIDEO_PIXEL_RGB888:
        return pixels * 3u;
    case XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED:
    case XIAOTAI_VIDEO_PIXEL_I420:
        return ((width | rows) & 1u) ? 0u : pixels / 2u * 3u;
    case XIAOTAI_VIDEO_PIXEL_NONE:
    default:
        return 0u;
    }
}

/* ---------------------------------------------------------------- geometry */

/* Quarter turns, clockwise as the viewer sees the picture. The values equal
 * the quarter-turn counts used by the part descriptors (xiaotai/hal.h). */
typedef enum {
    XIAOTAI_VIDEO_ROTATE_0 = 0,
    XIAOTAI_VIDEO_ROTATE_90,
    XIAOTAI_VIDEO_ROTATE_180,
    XIAOTAI_VIDEO_ROTATE_270,
} xiaotai_video_rotation_t;

/* How a picture is placed in an area of another aspect ratio. Both keep the
 * picture's proportions. */
typedef enum {
    XIAOTAI_VIDEO_FIT_CONTAIN = 0, /* the whole picture, centred; the rest of the area is black. Without an
                                    * accelerator a picture less than a third too small is not enlarged. */
    XIAOTAI_VIDEO_FIT_COVER,       /* the area is filled; the picture's edges are cropped */
} xiaotai_video_fit_t;

/* A rectangle inside a picture or a frame buffer, in that picture's own pixels:
 * the crop and place of a transform, the picture inside a finished frame.
 * Never a position on a display (that is xiaotai_video_area_t), and not the
 * panel-native rectangle of xiaotai_rect_t either. */
typedef struct {
    uint16_t x, y, width, height;
} xiaotai_video_rect_t;

/* A rectangle on a display, in display pixels: the logical frame of the
 * display part (xiaotai_display_desc_t width x height, after its rotation),
 * the frame the UI draws in. It differs from the panel's native frame of
 * xiaotai_rect_t (present()) whenever the descriptor's rotation is not 0, so
 * the two are never interchangeable. The one logical display rectangle of
 * xiaotai/hal.h under the engine's name; xiaotai_ui_rect_t (xiaotai/ui.h) is
 * the same type, so a video source passes rectangles between the UI and this
 * engine without converting. */
typedef xiaotai_display_area_t xiaotai_video_area_t;

/* ---------------------------------------------------------------- pictures */

/* An uncompressed picture. The memory behind data belongs to whoever produced
 * the image; each function that takes or returns one states how long it stays
 * valid. */
typedef struct {
    xiaotai_video_pixel_t pixel;
    uint16_t width, height;   /* visible picture */
    /* Rows present in data, >= height. A camera whose height is not a multiple
     * of 16 repeats its last row pair up to the next multiple, because encoders
     * read whole macroblocks; rows beyond height are never shown. */
    uint16_t rows;
    /* Pixels from the start of one row to the start of the next, >= width; 0
     * means width. Only the RGB and YUYV422 layouts may be wider than their
     * visible part (hardware decoders pad rows); 4:2:0 pictures are tight. */
    uint16_t stride;
    /* true: luma 0..255 (BT.601 full range); false: 16..235 (BT.601 limited). */
    bool full_range;
    const uint8_t *data;
    size_t size;              /* bytes at data, at least the picture's rows at its stride */
    int64_t captured_us;      /* xiaotai_time_us() when the picture was complete; 0: unknown */
} xiaotai_video_image_t;

/* One encoded frame, in either direction. data is valid only during the call
 * that passes the packet; whoever needs it longer copies it. */
typedef struct {
    xiaotai_video_codec_t codec;
    bool key_frame;           /* decodable without an earlier frame */
    uint16_t width, height;   /* picture size; 0 when the producer does not know it (received frames) */
    uint32_t timestamp_ms;    /* uplink: capture time (xiaotai_time_us() / 1000); downlink: the sender's media timestamp */
    const uint8_t *data;
    size_t size;
} xiaotai_video_packet_t;

#ifdef __cplusplus
}
#endif
