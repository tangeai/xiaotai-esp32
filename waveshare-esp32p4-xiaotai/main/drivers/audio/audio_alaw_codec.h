#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_ALAW_DECIMATOR_TAPS 31U

typedef struct {
    int16_t delay[AUDIO_ALAW_DECIMATOR_TAPS];
    uint8_t write_index;
    uint8_t phase;
    bool initialized;
} audio_alaw_stream_encoder_t;

esp_err_t audio_alaw_encode(const uint8_t *pcm_data,
                            size_t pcm_data_len,
                            uint8_t **encoded_data,
                            size_t *encoded_data_len);
esp_err_t audio_alaw_encode_to(const uint8_t *pcm_data,
                               size_t pcm_data_len,
                               uint8_t *encoded_data,
                               size_t encoded_data_size,
                               size_t *encoded_data_len);
esp_err_t audio_alaw_encode_16k_mono_to_8k(const uint8_t *pcm_data,
                                           size_t pcm_data_len,
                                           uint8_t *encoded_data,
                                           size_t encoded_capacity,
                                           size_t *encoded_data_len);
void audio_alaw_stream_encoder_reset(audio_alaw_stream_encoder_t *encoder);
esp_err_t audio_alaw_stream_encode_16k_mono_to_8k(audio_alaw_stream_encoder_t *encoder,
                                                  const uint8_t *pcm_data,
                                                  size_t pcm_data_len,
                                                  uint8_t *encoded_data,
                                                  size_t encoded_capacity,
                                                  size_t *encoded_data_len);
size_t audio_alaw_decoded_pcm_size(size_t alaw_data_len);
esp_err_t audio_alaw_decode_to_pcm(const uint8_t *alaw_data,
                                   size_t alaw_data_len,
                                   uint8_t *pcm_data,
                                   size_t pcm_data_len);
esp_err_t audio_alaw_decode(const uint8_t *alaw_data,
                            size_t alaw_data_len,
                            uint8_t **pcm_data,
                            size_t *pcm_data_len);
