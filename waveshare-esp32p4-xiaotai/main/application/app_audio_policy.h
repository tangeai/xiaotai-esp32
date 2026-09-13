#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_AUDIO_SOURCE_RTC_MEDIA = 1U << 0,
    APP_AUDIO_SOURCE_AI_CHAT_MEDIA = 1U << 1,
} app_audio_source_t;

esp_err_t app_audio_policy_init(void);
/* Prepared keeps the adaptive playout controller warm before media arrives. */
esp_err_t app_audio_policy_set_source_prepared(app_audio_source_t source, bool prepared);
/* Active enables full-duplex AEC while at least one realtime source is live. */
esp_err_t app_audio_policy_set_source_active(app_audio_source_t source, bool active);
