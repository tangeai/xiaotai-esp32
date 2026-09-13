#include "audio_alaw_codec.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "app_memory_policy.h"

static const char *TAG = "audio_alaw_codec";

/*
 * Keep the 16 kHz -> 8 kHz decimator continuous across 20 ms packets. A
 * packet-local filter restarts its edge state fifty times per second, which
 * produces periodic transients and aliases high-frequency input into PCMA.
 */
#define AUDIO_ALAW_DECIMATOR_Q15_SHIFT 15U

static const int16_t s_audio_alaw_decimator_q15[AUDIO_ALAW_DECIMATOR_TAPS] = {
    -9, 65, 33, -132, -112, 261, 305, -438,
    -691, 635, 1429, -813, -3041, 937, 10266, 15378,
    10266, 937, -3041, -813, 1429, 635, -691, -438,
    305, 261, -112, -132, 33, 65, -9,
};

static int16_t audio_alaw_decimator_push(audio_alaw_stream_encoder_t *encoder,
                                         int16_t sample)
{
    if (!encoder->initialized) {
        for (size_t index = 0; index < AUDIO_ALAW_DECIMATOR_TAPS; ++index) {
            encoder->delay[index] = sample;
        }
        encoder->write_index = 0U;
        encoder->phase = 0U;
        encoder->initialized = true;
    }

    encoder->delay[encoder->write_index] = sample;

    int64_t accumulator = 0;
    size_t delay_index = encoder->write_index;
    for (size_t tap = 0; tap < AUDIO_ALAW_DECIMATOR_TAPS; ++tap) {
        accumulator += (int64_t)encoder->delay[delay_index] *
                       s_audio_alaw_decimator_q15[tap];
        delay_index = delay_index == 0U ? AUDIO_ALAW_DECIMATOR_TAPS - 1U : delay_index - 1U;
    }

    encoder->write_index++;
    if (encoder->write_index >= AUDIO_ALAW_DECIMATOR_TAPS) {
        encoder->write_index = 0U;
    }

    accumulator += (int64_t)1 << (AUDIO_ALAW_DECIMATOR_Q15_SHIFT - 1U);
    accumulator >>= AUDIO_ALAW_DECIMATOR_Q15_SHIFT;
    if (accumulator > INT16_MAX) {
        return INT16_MAX;
    }
    if (accumulator < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)accumulator;
}

static void *audio_alaw_alloc(size_t size)
{
    return app_memory_alloc_psram(size);
}

static uint8_t audio_alaw_linear_to_alaw(int16_t sample)
{
    static const int16_t segment_end[8] = {
        0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF,
    };
    uint8_t mask = 0xD5;
    int32_t scaled = sample >= 0 ?
                         (int32_t)sample / 8 :
                         -(((int32_t)(-sample) + 7) / 8);
    int32_t magnitude = scaled;
    uint8_t segment = 0;

    if (magnitude < 0) {
        mask = 0x55;
        magnitude = -magnitude - 1;
    }

    while (segment < 8 && magnitude > segment_end[segment]) {
        ++segment;
    }

    if (segment >= 8) {
        return (uint8_t)(0x7F ^ mask);
    }

    uint8_t alaw = (uint8_t)(segment << 4);
    if (segment < 2) {
        alaw |= (uint8_t)((magnitude >> 1) & 0x0F);
    } else {
        alaw |= (uint8_t)((magnitude >> segment) & 0x0F);
    }

    return (uint8_t)(alaw ^ mask);
}

static int16_t audio_alaw_to_linear(uint8_t sample)
{
    sample ^= 0x55;

    int16_t value = (int16_t)((sample & 0x0F) << 4);
    uint8_t segment = (uint8_t)((sample & 0x70) >> 4);

    switch (segment) {
    case 0:
        value += 8;
        break;
    case 1:
        value += 0x108;
        break;
    default:
        value += 0x108;
        value = (int16_t)(value << (segment - 1));
        break;
    }

    return (sample & 0x80U) != 0U ? value : (int16_t)-value;
}

static void audio_alaw_log_alloc_failed(const char *operation, size_t size)
{
    ESP_LOGE(TAG,
             "%s alloc failed: size=%u internal_largest=%u psram_largest=%u",
             operation,
             (unsigned)size,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

esp_err_t audio_alaw_encode(const uint8_t *pcm_data,
                            size_t pcm_data_len,
                            uint8_t **encoded_data,
                            size_t *encoded_data_len)
{
    if (pcm_data == NULL || encoded_data == NULL || encoded_data_len == NULL ||
        pcm_data_len == 0 || (pcm_data_len & 0x1U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t sample_count = pcm_data_len / sizeof(int16_t);
    uint8_t *encoded = audio_alaw_alloc(sample_count);
    if (encoded == NULL) {
        audio_alaw_log_alloc_failed("alaw encode", sample_count);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = audio_alaw_encode_to(pcm_data, pcm_data_len, encoded, sample_count, &sample_count);
    if (ret != ESP_OK) {
        free(encoded);
        return ret;
    }

    *encoded_data = encoded;
    *encoded_data_len = sample_count;
    return ESP_OK;
}

esp_err_t audio_alaw_encode_to(const uint8_t *pcm_data,
                               size_t pcm_data_len,
                               uint8_t *encoded_data,
                               size_t encoded_data_size,
                               size_t *encoded_data_len)
{
    if (encoded_data_len != NULL) {
        *encoded_data_len = 0;
    }
    if (pcm_data == NULL || encoded_data == NULL || encoded_data_len == NULL ||
        pcm_data_len == 0 || (pcm_data_len & 0x1U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t sample_count = pcm_data_len / sizeof(int16_t);
    if (encoded_data_size < sample_count) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int16_t *pcm_samples = (const int16_t *)pcm_data;
    for (size_t index = 0; index < sample_count; ++index) {
        encoded_data[index] = audio_alaw_linear_to_alaw(pcm_samples[index]);
    }

    *encoded_data_len = sample_count;
    return ESP_OK;
}

esp_err_t audio_alaw_encode_16k_mono_to_8k(const uint8_t *pcm_data,
                                           size_t pcm_data_len,
                                           uint8_t *encoded_data,
                                           size_t encoded_capacity,
                                           size_t *encoded_data_len)
{
    audio_alaw_stream_encoder_t encoder = {0};
    return audio_alaw_stream_encode_16k_mono_to_8k(&encoder,
                                                   pcm_data,
                                                   pcm_data_len,
                                                   encoded_data,
                                                   encoded_capacity,
                                                   encoded_data_len);
}

void audio_alaw_stream_encoder_reset(audio_alaw_stream_encoder_t *encoder)
{
    if (encoder != NULL) {
        memset(encoder, 0, sizeof(*encoder));
    }
}

esp_err_t audio_alaw_stream_encode_16k_mono_to_8k(audio_alaw_stream_encoder_t *encoder,
                                                  const uint8_t *pcm_data,
                                                  size_t pcm_data_len,
                                                  uint8_t *encoded_data,
                                                  size_t encoded_capacity,
                                                  size_t *encoded_data_len)
{
    if (encoded_data_len != NULL) {
        *encoded_data_len = 0;
    }
    if (encoder == NULL || pcm_data == NULL || encoded_data == NULL || encoded_data_len == NULL ||
        pcm_data_len == 0 || (pcm_data_len & 0x3U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t input_sample_count = pcm_data_len / sizeof(int16_t);
    const size_t output_sample_count = input_sample_count / 2U;
    if (encoded_capacity < output_sample_count) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int16_t *pcm_samples = (const int16_t *)pcm_data;
    size_t output_index = 0U;
    for (size_t input_index = 0; input_index < input_sample_count; ++input_index) {
        int16_t filtered = audio_alaw_decimator_push(encoder, pcm_samples[input_index]);
        if (encoder->phase == 0U) {
            encoded_data[output_index++] = audio_alaw_linear_to_alaw(filtered);
        }
        encoder->phase ^= 1U;
    }

    *encoded_data_len = output_index;
    return ESP_OK;
}

size_t audio_alaw_decoded_pcm_size(size_t alaw_data_len)
{
    if (alaw_data_len > (SIZE_MAX / sizeof(int16_t))) {
        return 0;
    }
    return alaw_data_len * sizeof(int16_t);
}

esp_err_t audio_alaw_decode_to_pcm(const uint8_t *alaw_data,
                                   size_t alaw_data_len,
                                   uint8_t *pcm_data,
                                   size_t pcm_data_len)
{
    if (alaw_data == NULL || pcm_data == NULL || alaw_data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t decoded_len = audio_alaw_decoded_pcm_size(alaw_data_len);
    if (decoded_len == 0 || pcm_data_len < decoded_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    int16_t *decoded = (int16_t *)pcm_data;
    for (size_t index = 0; index < alaw_data_len; ++index) {
        decoded[index] = audio_alaw_to_linear(alaw_data[index]);
    }
    return ESP_OK;
}

esp_err_t audio_alaw_decode(const uint8_t *alaw_data,
                            size_t alaw_data_len,
                            uint8_t **pcm_data,
                            size_t *pcm_data_len)
{
    if (alaw_data == NULL || pcm_data == NULL || pcm_data_len == NULL || alaw_data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t decoded_len = audio_alaw_decoded_pcm_size(alaw_data_len);
    if (decoded_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *decoded = audio_alaw_alloc(decoded_len);
    if (decoded == NULL) {
        audio_alaw_log_alloc_failed("alaw decode", decoded_len);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = audio_alaw_decode_to_pcm(alaw_data, alaw_data_len, decoded, decoded_len);
    if (ret != ESP_OK) {
        free(decoded);
        return ret;
    }

    *pcm_data = decoded;
    *pcm_data_len = decoded_len;
    return ESP_OK;
}
