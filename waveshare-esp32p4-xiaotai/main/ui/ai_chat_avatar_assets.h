#pragma once

#include <stdint.h>

#include "lvgl.h"

#define AI_CHAT_AVATAR_ASSET_WIDTH  96
#define AI_CHAT_AVATAR_ASSET_HEIGHT 96
#define AI_CHAT_AVATAR_ROLE_COUNT   2U

typedef enum {
    AI_CHAT_AVATAR_ROLE_BUDDY = 0,
    AI_CHAT_AVATAR_ROLE_SPROUT = 1,
} ai_chat_avatar_role_t;

typedef enum {
    AI_CHAT_AVATAR_STATE_IDLE = 0,
    AI_CHAT_AVATAR_STATE_LISTENING,
    AI_CHAT_AVATAR_STATE_THINKING,
    AI_CHAT_AVATAR_STATE_SPEAKING,
    AI_CHAT_AVATAR_STATE_RESTING,
    AI_CHAT_AVATAR_STATE_ERROR,
    AI_CHAT_AVATAR_STATE_COUNT,
} ai_chat_avatar_state_t;

const lv_img_dsc_t *ai_chat_avatar_asset_get(uint8_t role, ai_chat_avatar_state_t state);
