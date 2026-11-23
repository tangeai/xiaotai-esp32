// SPDX-License-Identifier: MIT
#include "registry.h"

#include <stdatomic.h>
#include "g711.h"
#include "internal.h"
#include "portable_audio.h"
#include "resample.h"

#define PROCESSORS_MAX 8U
#define CODECS_MAX 8U
#define EFFECTS_MAX 12U
#define TAPS_MAX 8U

static const xiaotai_audio_processor_t *s_processors[PROCESSORS_MAX];
static const xiaotai_audio_effect_t *s_effects[EFFECTS_MAX];
static const xiaotai_media_tap_t *s_taps[TAPS_MAX];
static const xiaotai_audio_encoder_t *s_encoders[CODECS_MAX];
static const xiaotai_audio_decoder_t *s_decoders[CODECS_MAX];
static size_t s_processor_count, s_effect_count, s_encoder_count, s_decoder_count;
static struct { xiaotai_codec_t encoding; uint32_t rate_hz; } s_stream_formats[MEDIA_SOUND_DECODERS_MAX];
static size_t s_stream_format_count;
static atomic_size_t s_tap_count;
static atomic_bool s_tap_enabled[TAPS_MAX];

xiaotai_err_t xiaotai_media_processor_register(const xiaotai_audio_processor_t *processor)
{
    if (!processor || !processor->name || !processor->match || !processor->open || !processor->feed ||
        !processor->fetch || !processor->close) return XIAOTAI_ERR_INVALID_ARG;
    if (media_registration_closed()) return XIAOTAI_ERR_INVALID_STATE;
    if (s_processor_count == PROCESSORS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_processors[s_processor_count++] = processor;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_effect_register(const xiaotai_audio_effect_t *effect)
{
    if (!effect || !effect->name || !effect->open || !effect->process || !effect->close ||
        (effect->stage != XIAOTAI_EFFECT_CAPTURE && effect->stage != XIAOTAI_EFFECT_UPLINK)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (media_registration_closed()) return XIAOTAI_ERR_INVALID_STATE;
    if (s_effect_count == EFFECTS_MAX) return XIAOTAI_ERR_NO_MEM;
    /* Stable insertion, grouped by stage. Earlier registrations win ties. */
    size_t at = s_effect_count;
    while (at > 0) {
        const xiaotai_audio_effect_t *before = s_effects[at - 1U];
        if (before->stage < effect->stage || (before->stage == effect->stage && before->order <= effect->order)) break;
        s_effects[at--] = before;
    }
    s_effects[at] = effect;
    ++s_effect_count;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_tap_register(const xiaotai_media_tap_t *tap)
{
    if (!tap || !tap->name || !tap->pcm) return XIAOTAI_ERR_INVALID_ARG;
    if (media_registration_closed()) return XIAOTAI_ERR_INVALID_STATE;
    const size_t count = atomic_load_explicit(&s_tap_count, memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        if (s_taps[i] == tap) return XIAOTAI_OK;
    }
    if (count == TAPS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_taps[count] = tap;
    atomic_store_explicit(&s_tap_enabled[count], false, memory_order_relaxed);
    atomic_store_explicit(&s_tap_count, count + 1U, memory_order_release);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_tap_enable(const xiaotai_media_tap_t *tap, bool enabled)
{
    const size_t count = atomic_load_explicit(&s_tap_count, memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (s_taps[i] == tap) {
            atomic_store_explicit(&s_tap_enabled[i], enabled, memory_order_release);
            media_session_wake_playout();
            return XIAOTAI_OK;
        }
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_media_encoder_register(const xiaotai_audio_encoder_t *encoder)
{
    if (!encoder || !encoder->name || !XIAOTAI_CODEC_IS_AUDIO(encoder->encoding) || !encoder->match ||
        !encoder->open || !encoder->encode || !encoder->close) return XIAOTAI_ERR_INVALID_ARG;
    if (media_registration_closed()) return XIAOTAI_ERR_INVALID_STATE;
    if (s_encoder_count == CODECS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_encoders[s_encoder_count++] = encoder;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_decoder_register(const xiaotai_audio_decoder_t *decoder)
{
    if (!decoder || !decoder->name || !XIAOTAI_CODEC_IS_AUDIO(decoder->encoding) || !decoder->match ||
        !decoder->packet_samples || !decoder->open || !decoder->decode || !decoder->close) return XIAOTAI_ERR_INVALID_ARG;
    if (media_registration_closed()) return XIAOTAI_ERR_INVALID_STATE;
    if (s_decoder_count == CODECS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_decoders[s_decoder_count++] = decoder;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_require_stream_format(xiaotai_codec_t encoding, uint32_t rate_hz)
{
    if (!XIAOTAI_CODEC_IS_AUDIO(encoding) || rate_hz < XIAOTAI_MEDIA_RATE_MIN_HZ ||
        rate_hz > XIAOTAI_MEDIA_RATE_MAX_HZ) return XIAOTAI_ERR_INVALID_ARG;
    if (media_registration_closed()) return XIAOTAI_ERR_INVALID_STATE;
    if (encoding == XIAOTAI_CODEC_PCM16 || encoding == XIAOTAI_CODEC_G711A) return XIAOTAI_OK;
    for (size_t i = 0; i < s_stream_format_count; ++i) {
        if (s_stream_formats[i].encoding == encoding && s_stream_formats[i].rate_hz == rate_hz)
            return XIAOTAI_OK;
    }
    if (s_stream_format_count == MEDIA_SOUND_DECODERS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_stream_formats[s_stream_format_count].encoding = encoding;
    s_stream_formats[s_stream_format_count++].rate_hz = rate_hz;
    return XIAOTAI_OK;
}

bool media_required_stream_format_at(size_t index, xiaotai_media_format_t *format)
{
    if (index >= s_stream_format_count) return false;
    *format = media_stream_format(s_stream_formats[index].encoding, s_stream_formats[index].rate_hz);
    return true;
}

const xiaotai_audio_processor_t *media_select_processor(const xiaotai_audio_desc_t *input,
                                                        const xiaotai_audio_desc_t *output)
{
    if (!input) return NULL;
    const xiaotai_audio_processor_t *selected = NULL;
    int best = 0;
    for (size_t i = 0; i < s_processor_count; ++i) {
        const int score = s_processors[i]->match(input, output);
        if (score > best) {
            selected = s_processors[i];
            best = score;
        }
    }
    if (selected) return selected;
    if (media_echo_gate.match(input, output) > 0) return &media_echo_gate;
    if (media_passthrough.match(input, output) > 0) return &media_passthrough;
    return NULL;
}

const xiaotai_audio_encoder_t *media_select_encoder(const xiaotai_media_format_t *format)
{
    int best = xiaotai_media_g711a_encoder.match(format);
    const xiaotai_audio_encoder_t *selected = best > 0 ? &xiaotai_media_g711a_encoder : NULL;
    for (size_t i = 0; i < s_encoder_count; ++i) {
        if (s_encoders[i]->encoding != format->encoding) continue;
        const int score = s_encoders[i]->match(format);
        if (score > best) {
            selected = s_encoders[i];
            best = score;
        }
    }
    return selected;
}

const xiaotai_audio_decoder_t *media_select_decoder(const xiaotai_media_format_t *format)
{
    int best = xiaotai_media_g711a_decoder.match(format);
    const xiaotai_audio_decoder_t *selected = best > 0 ? &xiaotai_media_g711a_decoder : NULL;
    for (size_t i = 0; i < s_decoder_count; ++i) {
        if (s_decoders[i]->encoding != format->encoding) continue;
        const int score = s_decoders[i]->match(format);
        if (score > best) {
            selected = s_decoders[i];
            best = score;
        }
    }
    return selected;
}

const xiaotai_audio_decoder_t *media_registered_decoder_at(size_t index)
{
    return index < s_decoder_count ? s_decoders[index] : NULL;
}

const xiaotai_audio_effect_t *media_effect_at(size_t index)
{
    return index < s_effect_count ? s_effects[index] : NULL;
}

const xiaotai_media_tap_t *media_tap_at(size_t index, bool *enabled)
{
    if (index >= atomic_load_explicit(&s_tap_count, memory_order_acquire)) return NULL;
    if (enabled) *enabled = atomic_load_explicit(&s_tap_enabled[index], memory_order_acquire);
    return s_taps[index];
}

void media_registry_reset(void)
{
    s_processor_count = s_effect_count = s_encoder_count = s_decoder_count = s_stream_format_count = 0;
    atomic_store_explicit(&s_tap_count, 0, memory_order_relaxed);
}
