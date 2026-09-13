#include "app_ai_chat_config.h"

#include "esp_check.h"
#include "nvs.h"

#include "platform_nvs_async.h"
#include "platform_storage.h"

static const char *TAG = "app_ai_chat_config";

#define APP_AI_CHAT_NVS_NAMESPACE  "ai_chat_ui"
#define APP_AI_CHAT_NVS_KEY_AVATAR "avatar"

static app_ai_chat_config_t s_ai_chat_config = {
    .avatar = APP_AI_CHAT_AVATAR_BUDDY,
};

static uint8_t app_ai_chat_config_normalize_avatar(uint8_t avatar)
{
    return avatar < APP_AI_CHAT_AVATAR_COUNT ? avatar : APP_AI_CHAT_AVATAR_BUDDY;
}

esp_err_t app_ai_chat_config_load(app_ai_chat_config_t *config)
{
    nvs_handle_t nvs_handle = 0;
    uint8_t saved_avatar = APP_AI_CHAT_AVATAR_BUDDY;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid AI Chat config");
    ESP_RETURN_ON_ERROR(platform_storage_init(), TAG, "nvs init failed");

    esp_err_t ret = nvs_open(APP_AI_CHAT_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = nvs_get_u8(nvs_handle, APP_AI_CHAT_NVS_KEY_AVATAR, &saved_avatar);
        nvs_close(nvs_handle);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }

    s_ai_chat_config.avatar = app_ai_chat_config_normalize_avatar(saved_avatar);
    *config = s_ai_chat_config;
    return ESP_OK;
}

uint8_t app_ai_chat_config_get_avatar(void)
{
    return s_ai_chat_config.avatar;
}

esp_err_t app_ai_chat_config_set_avatar(uint8_t avatar)
{
    uint8_t normalized = app_ai_chat_config_normalize_avatar(avatar);

    esp_err_t ret = platform_nvs_async_set_u8_and_wait(APP_AI_CHAT_NVS_NAMESPACE,
                                                        APP_AI_CHAT_NVS_KEY_AVATAR,
                                                        normalized);
    if (ret == ESP_OK) {
        s_ai_chat_config.avatar = normalized;
    }
    return ret;
}
