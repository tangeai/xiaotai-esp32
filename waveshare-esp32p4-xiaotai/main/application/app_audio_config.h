#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t speaker_volume_percent;
    uint8_t capture_gain_percent;
} app_audio_config_t;

esp_err_t app_audio_config_load(app_audio_config_t *config);
esp_err_t app_audio_config_save_speaker_volume(uint8_t percent);
esp_err_t app_audio_config_save_capture_gain(uint8_t percent);
