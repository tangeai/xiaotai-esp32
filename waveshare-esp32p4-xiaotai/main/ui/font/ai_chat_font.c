#include "ai_chat_font.h"

LV_FONT_DECLARE(lv_font_cn_14);

esp_err_t ai_chat_font_init(void)
{
    return ESP_OK;
}

esp_err_t ai_chat_font_prepare_external_assets(void)
{
    return ESP_OK;
}

esp_err_t ai_chat_font_attach_external_assets(void)
{
    return ESP_OK;
}

const lv_font_t *ai_chat_font_get_fallback(void)
{
    return &lv_font_cn_14;
}

const lv_font_t *ai_chat_font_get_current(void)
{
    return &lv_font_cn_14;
}

const lv_font_t *ai_chat_font_get(void)
{
    return &lv_font_cn_14;
}

bool ai_chat_font_uses_external_assets(void)
{
    return ai_chat_font_is_ready();
}

bool ai_chat_font_is_ready(void)
{
    return true;
}
