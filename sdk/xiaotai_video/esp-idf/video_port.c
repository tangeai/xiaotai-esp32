// SPDX-License-Identifier: MIT
#include "xiaotai/video_backend.h"
#include "sdkconfig.h"

/* The component's CMakeLists.txt defines what the chip has; the build options
 * (xiaotai/video_config.h) say what of it is used. */
#if defined(XIAOTAI_VIDEO_JPEG_DECODER) && XIAOTAI_VIDEO_ACCELERATORS
#define JPEG_HARDWARE 1
#else
#define JPEG_HARDWARE 0
#endif
#if defined(XIAOTAI_VIDEO_PIXEL_ACCELERATOR) && XIAOTAI_VIDEO_ACCELERATORS
#define PIXEL_HARDWARE 1
#else
#define PIXEL_HARDWARE 0
#endif

#if XIAOTAI_VIDEO_JPEG_ENCODER
extern const xiaotai_video_encoder_t video_esp_jpeg_encoder;
#endif
#ifdef XIAOTAI_VIDEO_H264_ENCODER
extern const xiaotai_video_encoder_t video_esp_h264_encoder;
#endif
#ifdef XIAOTAI_VIDEO_H264_DECODER
extern const xiaotai_video_decoder_t video_esp_h264_decoder;
#endif
#if JPEG_HARDWARE
extern const xiaotai_video_decoder_t video_esp_jpeg_decoder;
#endif
#if XIAOTAI_VIDEO_JPEG_SOFTWARE_DECODER
extern const xiaotai_video_decoder_t video_esp_jpeg_software_decoder;
#endif
#if PIXEL_HARDWARE
extern const xiaotai_video_transform_t video_esp_ppa_transform;
#endif

xiaotai_err_t xiaotai_video_port_init(void)
{
    xiaotai_err_t err = XIAOTAI_OK;
#if XIAOTAI_VIDEO_JPEG_ENCODER
    if (err == XIAOTAI_OK) err = xiaotai_video_encoder_register(&video_esp_jpeg_encoder);
#endif
#ifdef XIAOTAI_VIDEO_H264_ENCODER
    if (err == XIAOTAI_OK) err = xiaotai_video_encoder_register(&video_esp_h264_encoder);
#endif
#ifdef XIAOTAI_VIDEO_H264_DECODER
    if (err == XIAOTAI_OK) err = xiaotai_video_decoder_register(&video_esp_h264_decoder);
#endif
#if JPEG_HARDWARE
    if (err == XIAOTAI_OK) err = xiaotai_video_decoder_register(&video_esp_jpeg_decoder);
#endif
#if XIAOTAI_VIDEO_JPEG_SOFTWARE_DECODER
    if (err == XIAOTAI_OK) err = xiaotai_video_decoder_register(&video_esp_jpeg_software_decoder);
#endif
#if PIXEL_HARDWARE
    if (err == XIAOTAI_OK) err = xiaotai_video_transform_register(&video_esp_ppa_transform);
#endif
    return err;
}
