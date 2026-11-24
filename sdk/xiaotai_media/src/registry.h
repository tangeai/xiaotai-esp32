// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media_capture.h"
#include "xiaotai/media_codec.h"

/* All registrations are sealed before hardware descriptions are inspected. */
bool media_registration_closed(void);
const xiaotai_audio_processor_t *media_select_processor(const xiaotai_audio_desc_t *input,
                                                        const xiaotai_audio_desc_t *output);
const xiaotai_audio_encoder_t *media_select_encoder(const xiaotai_media_format_t *format);
const xiaotai_audio_decoder_t *media_select_decoder(const xiaotai_media_format_t *format);
const xiaotai_audio_decoder_t *media_registered_decoder_at(size_t index);

#define MEDIA_SOUND_DECODERS_MAX 8U

static inline xiaotai_media_format_t media_stream_format(xiaotai_codec_t encoding, uint32_t rate_hz)
{
    return (xiaotai_media_format_t){.encoding = encoding, .rate_hz = rate_hz, .channels = 1, .packet_ms = 20};
}

bool media_required_stream_format_at(size_t index, xiaotai_media_format_t *format);
const xiaotai_audio_effect_t *media_effect_at(size_t index);
const xiaotai_media_tap_t *media_tap_at(size_t index, bool *enabled);
void media_registry_reset(void);
