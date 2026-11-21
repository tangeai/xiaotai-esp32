// SPDX-License-Identifier: MIT
#include "xiaotai/video_backend.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

extern const xiaotai_video_encoder_t video_esp_jpeg_encoder;
#ifdef XIAOTAI_VIDEO_NATIVE_H264
extern const xiaotai_video_encoder_t video_esp_h264_encoder;
extern const xiaotai_video_decoder_t video_esp_h264_decoder;
#endif
#if defined(CONFIG_SOC_JPEG_DECODE_SUPPORTED) && CONFIG_SOC_JPEG_DECODE_SUPPORTED
extern const xiaotai_video_decoder_t video_esp_jpeg_decoder;
#endif
#if defined(SOC_PPA_SUPPORTED) && SOC_PPA_SUPPORTED
extern const xiaotai_video_transform_t video_esp_ppa_transform;
#endif

xiaotai_err_t xiaotai_video_port_init(void)
{
    xiaotai_err_t err = xiaotai_video_encoder_register(&video_esp_jpeg_encoder);
#ifdef XIAOTAI_VIDEO_NATIVE_H264
    if (err == XIAOTAI_OK) err = xiaotai_video_encoder_register(&video_esp_h264_encoder);
    if (err == XIAOTAI_OK) err = xiaotai_video_decoder_register(&video_esp_h264_decoder);
#endif
#if defined(CONFIG_SOC_JPEG_DECODE_SUPPORTED) && CONFIG_SOC_JPEG_DECODE_SUPPORTED
    if (err == XIAOTAI_OK) err = xiaotai_video_decoder_register(&video_esp_jpeg_decoder);
#endif
#if defined(SOC_PPA_SUPPORTED) && SOC_PPA_SUPPORTED
    if (err == XIAOTAI_OK) err = xiaotai_video_transform_register(&video_esp_ppa_transform);
#endif
    return err;
}
