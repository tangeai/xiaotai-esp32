// SPDX-License-Identifier: MIT
#pragma once
#include "xmini_audio.h"
#include "xmini_settings.h"

static inline esp_err_t xmini_request_audio(xmini_audio_request_t request)
{
    if (request.kind == XMINI_AUDIO_VOLUME || request.kind == XMINI_AUDIO_GAIN) {
        xmini_audio_status_t audio = xmini_audio_status();
        if (!audio.ready) return ESP_ERR_INVALID_STATE;
        int volume = request.kind == XMINI_AUDIO_VOLUME ? request.value : audio.volume;
        int gain = request.kind == XMINI_AUDIO_GAIN ? request.value : audio.gain_db;
        // Admission owns the preference; a live audio failure remains an error.
        esp_err_t error = xmini_settings_schedule(volume, gain);
        if (error) return error;
    }
    return xmini_audio_request(request);
}
