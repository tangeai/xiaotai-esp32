// SPDX-License-Identifier: MIT
#include "portable_audio.h"

#include "xiaotai/media_codec.h"
#include "xiaotai/media_config.h"

extern const xiaotai_audio_processor_t media_aec_dual;
extern const xiaotai_audio_processor_t media_aec_single_frontend;
extern const xiaotai_audio_processor_t media_aec_single;
extern const xiaotai_audio_effect_t media_agc, media_boost;
extern const xiaotai_audio_encoder_t media_opus_encoder;
extern const xiaotai_audio_decoder_t media_opus_decoder;

xiaotai_err_t media_platform_register(void)
{
    xiaotai_err_t err = XIAOTAI_OK;
#if XIAOTAI_MEDIA_PROCESSOR_AEC_DUAL
    err = xiaotai_media_processor_register(&media_aec_dual);
#endif
#if XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND
    if (err == XIAOTAI_OK) err = xiaotai_media_processor_register(&media_aec_single_frontend);
#endif
#if XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE
    if (err == XIAOTAI_OK) err = xiaotai_media_processor_register(&media_aec_single);
#endif
#if XIAOTAI_MEDIA_EFFECT_AGC
    if (err == XIAOTAI_OK) err = xiaotai_media_effect_register(&media_agc);
    if (err == XIAOTAI_OK) err = xiaotai_media_effect_register(&media_boost);
#endif
#if XIAOTAI_MEDIA_CODEC_OPUS_ENCODER
    if (err == XIAOTAI_OK) err = xiaotai_media_encoder_register(&media_opus_encoder);
#endif
#if XIAOTAI_MEDIA_CODEC_OPUS_DECODER
    if (err == XIAOTAI_OK) err = xiaotai_media_decoder_register(&media_opus_decoder);
#endif
    return err;
}
