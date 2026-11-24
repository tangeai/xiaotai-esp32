// SPDX-License-Identifier: MIT
#include "xiaotai/media_codec.h"

#include "xiaotai/media.h"
#include "xiaotai/media_config.h"
#include "xiaotai/port/mem.h"

/* esp_audio_codec links libopus but does not expose its public header. */
extern int opus_packet_get_nb_samples(const unsigned char *packet, int32_t len, int32_t rate_hz);
extern int opus_packet_parse(const unsigned char *packet, int32_t len, unsigned char *toc,
                             const unsigned char *frames[48], int16_t sizes[48], int *payload_offset);

static bool rate_supported(uint32_t rate)
{
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

static bool mono_opus(const xiaotai_media_format_t *format)
{
    return format && format->encoding == XIAOTAI_CODEC_OPUS && format->channels == 1 && rate_supported(format->rate_hz);
}

static uint32_t packet_samples(const uint8_t *data, size_t size, uint32_t rate)
{
    if (!data || !size || size > XIAOTAI_MEDIA_PACKET_BYTES_MAX || !rate_supported(rate)) return 0;
    int16_t sizes[48];
    const int frames = opus_packet_parse(data, (int32_t)size, NULL, NULL, sizes, NULL);
    if (frames <= 0) return 0;
    const int samples = opus_packet_get_nb_samples(data, (int32_t)size, (int32_t)rate);
    return samples > 0 && (uint32_t)samples <= rate * XIAOTAI_MEDIA_PACKET_MS_MAX / 1000U ? (uint32_t)samples : 0;
}

#if XIAOTAI_MEDIA_CODEC_OPUS_DECODER
#include "decoder/impl/esp_opus_dec.h"

typedef struct { void *codec; uint32_t rate; } opus_decoder_t;

static int decoder_match(const xiaotai_media_format_t *format) { return mono_opus(format) ? 1 : 0; }

static void decoder_close(void *opaque)
{
    opus_decoder_t *dec = opaque;
    if (dec->codec) (void)esp_opus_dec_close(dec->codec);
    xiaotai_mem_free(dec);
}

static xiaotai_err_t decoder_open(const xiaotai_media_format_t *format, void **state)
{
    if (!state || !mono_opus(format)) return XIAOTAI_ERR_INVALID_ARG;
    opus_decoder_t *dec = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*dec));
    if (!dec) return XIAOTAI_ERR_NO_MEM;
    dec->rate = format->rate_hz;
    /* The vendor uses frame_duration as a minimum output-buffer size;
     * packet_samples() and the decode buffer determine the actual duration. */
    esp_opus_dec_cfg_t cfg = {.sample_rate = dec->rate, .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_2_5_MS, .self_delimited = false};
    const esp_audio_err_t ret = esp_opus_dec_open(&cfg, sizeof(cfg), &dec->codec);
    if (ret != ESP_AUDIO_ERR_OK) {
        decoder_close(dec);
        return ret == ESP_AUDIO_ERR_MEM_LACK ? XIAOTAI_ERR_NO_MEM : XIAOTAI_FAIL;
    }
    *state = dec;
    return XIAOTAI_OK;
}

static xiaotai_err_t decode(void *opaque, const uint8_t *data, size_t size, int16_t *pcm, size_t capacity,
                            size_t *samples)
{
    opus_decoder_t *dec = opaque;
    if (!dec || !pcm || !samples) return XIAOTAI_ERR_INVALID_ARG;
    *samples = 0;
    const uint32_t expected = packet_samples(data, size, dec->rate);
    if (!expected || capacity < expected) return XIAOTAI_ERR_INVALID_SIZE;
    esp_audio_dec_in_raw_t in = {.buffer = (uint8_t *)data, .len = (uint32_t)size};
    esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)pcm, .len = expected * sizeof(int16_t)};
    esp_audio_dec_info_t info = {0};
    if (esp_opus_dec_decode(dec->codec, &in, &out, &info) != ESP_AUDIO_ERR_OK) return XIAOTAI_FAIL;
    if (in.consumed != size || out.decoded_size != expected * sizeof(int16_t) ||
        info.sample_rate != dec->rate || info.channel != 1U || info.bits_per_sample != 16U)
        return XIAOTAI_ERR_INVALID_SIZE;
    *samples = expected;
    return XIAOTAI_OK;
}

static xiaotai_err_t decoder_reset(void *opaque)
{
    opus_decoder_t *dec = opaque;
    return esp_opus_dec_reset(dec->codec) == ESP_AUDIO_ERR_OK ? XIAOTAI_OK : XIAOTAI_FAIL;
}

const xiaotai_audio_decoder_t media_opus_decoder = {.name = "opus", .encoding = XIAOTAI_CODEC_OPUS,
    .stack_bytes = 8192, .match = decoder_match, .packet_samples = packet_samples, .open = decoder_open,
    .decode = decode, .reset = decoder_reset, .close = decoder_close};
#endif

#if XIAOTAI_MEDIA_CODEC_OPUS_ENCODER
#include "encoder/impl/esp_opus_enc.h"

typedef struct { void *codec; uint32_t rate, samples, max_bytes; } opus_encoder_t;

static esp_opus_enc_frame_duration_t duration(uint8_t ms)
{
    switch (ms) {
        case 5: return ESP_OPUS_ENC_FRAME_DURATION_5_MS;
        case 10: return ESP_OPUS_ENC_FRAME_DURATION_10_MS;
        case 20: return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
        case 40: return ESP_OPUS_ENC_FRAME_DURATION_40_MS;
        case 60: return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        case 80: return ESP_OPUS_ENC_FRAME_DURATION_80_MS;
        case 100: return ESP_OPUS_ENC_FRAME_DURATION_100_MS;
        case 120: return ESP_OPUS_ENC_FRAME_DURATION_120_MS;
        default: return ESP_OPUS_ENC_FRAME_DURATION_ARG;
    }
}

static int encoder_match(const xiaotai_media_format_t *format)
{
    return mono_opus(format) && duration(format->packet_ms) != ESP_OPUS_ENC_FRAME_DURATION_ARG ? 1 : 0;
}

static void encoder_close(void *opaque)
{
    opus_encoder_t *enc = opaque;
    if (enc->codec) esp_opus_enc_close(enc->codec);
    xiaotai_mem_free(enc);
}

static xiaotai_err_t encoder_open(const xiaotai_media_format_t *format, void **state)
{
    if (!state || !encoder_match(format)) return XIAOTAI_ERR_INVALID_ARG;
    const int32_t bitrate = xiaotai_media_param("opus.bitrate", format->rate_hz >= 24000 ? 40000 : 24000);
    const int32_t complexity = xiaotai_media_param("opus.complexity", 0);
    const int32_t low_delay = xiaotai_media_param("opus.low_delay", 0);
    if (complexity < 0 || complexity > 10 || bitrate < 6000 || bitrate > 510000 ||
        (low_delay != 0 && low_delay != 1)) return XIAOTAI_ERR_INVALID_ARG;
    opus_encoder_t *enc = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*enc));
    if (!enc) return XIAOTAI_ERR_NO_MEM;
    enc->rate = format->rate_hz;
    enc->samples = format->rate_hz * format->packet_ms / 1000U;
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = (int)format->rate_hz;
    cfg.channel = 1;
    cfg.bitrate = bitrate;
    cfg.complexity = complexity;
    cfg.frame_duration = duration(format->packet_ms);
    cfg.application_mode = low_delay ? ESP_OPUS_ENC_APPLICATION_LOWDELAY : ESP_OPUS_ENC_APPLICATION_VOIP;
    const esp_audio_err_t ret = esp_opus_enc_open(&cfg, sizeof(cfg), &enc->codec);
    if (ret != ESP_AUDIO_ERR_OK) {
        encoder_close(enc);
        return ret == ESP_AUDIO_ERR_MEM_LACK ? XIAOTAI_ERR_NO_MEM : XIAOTAI_FAIL;
    }
    int input = 0, output = 0;
    if (esp_opus_enc_get_frame_size(enc->codec, &input, &output) != ESP_AUDIO_ERR_OK ||
        input != (int)(enc->samples * sizeof(int16_t)) || output <= 0 || output > (int)XIAOTAI_MEDIA_PACKET_BYTES_MAX) {
        encoder_close(enc);
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    enc->max_bytes = (uint32_t)output;
    *state = enc;
    return XIAOTAI_OK;
}

static xiaotai_err_t encode(void *opaque, const int16_t *pcm, size_t samples, uint8_t *data, size_t capacity,
                            size_t *size)
{
    opus_encoder_t *enc = opaque;
    if (!enc || !pcm || !data || !size) return XIAOTAI_ERR_INVALID_ARG;
    *size = 0;
    if (enc->samples != samples || capacity < enc->max_bytes) return XIAOTAI_ERR_INVALID_SIZE;
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm, .len = (uint32_t)(samples * sizeof(int16_t))};
    esp_audio_enc_out_frame_t out = {.buffer = data, .len = enc->max_bytes};
    if (esp_opus_enc_process(enc->codec, &in, &out) != ESP_AUDIO_ERR_OK) return XIAOTAI_FAIL;
    if (out.encoded_bytes > enc->max_bytes || packet_samples(data, out.encoded_bytes, enc->rate) != samples) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    *size = out.encoded_bytes;
    return XIAOTAI_OK;
}

static xiaotai_err_t encoder_reset(void *opaque)
{
    opus_encoder_t *enc = opaque;
    return esp_opus_enc_reset(enc->codec) == ESP_AUDIO_ERR_OK ? XIAOTAI_OK : XIAOTAI_FAIL;
}

const xiaotai_audio_encoder_t media_opus_encoder = {.name = "opus", .encoding = XIAOTAI_CODEC_OPUS,
    .stack_bytes = 40960, .match = encoder_match, .open = encoder_open, .encode = encode,
    .reset = encoder_reset, .close = encoder_close};
#endif
