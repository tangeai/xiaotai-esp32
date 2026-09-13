#pragma once

#include <stdint.h>

#include "esp_err.h"

#define APP_AI_CHAT_AVATAR_BUDDY  0U
#define APP_AI_CHAT_AVATAR_SPROUT 1U
#define APP_AI_CHAT_AVATAR_COUNT  2U

typedef struct {
    uint8_t avatar;
} app_ai_chat_config_t;

esp_err_t app_ai_chat_config_load(app_ai_chat_config_t *config);
uint8_t app_ai_chat_config_get_avatar(void);
esp_err_t app_ai_chat_config_set_avatar(uint8_t avatar);
