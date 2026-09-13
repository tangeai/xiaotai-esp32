#pragma once

#include "esp_err.h"

#include "ai_chat.h"

typedef struct {
    char device_id[AI_CHAT_DEVICE_ID_MAX];
    char role_id[AI_CHAT_ROLE_ID_MAX];
    char peer_id[AI_CHAT_PEER_ID_MAX];
    char token[AI_CHAT_TOKEN_MAX];
} ai_chat_join_info_t;

esp_err_t ai_chat_token_request_join(const ai_chat_config_t *config, ai_chat_join_info_t *join_info);
esp_err_t ai_chat_token_prefetch_join(const ai_chat_config_t *config);
void ai_chat_token_invalidate_cache(void);
