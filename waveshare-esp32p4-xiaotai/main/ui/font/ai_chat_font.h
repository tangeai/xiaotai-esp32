#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

esp_err_t ai_chat_font_init(void);
/* Compatibility hooks. The current font is compiled const data, so these are no-op ready checks. */
esp_err_t ai_chat_font_prepare_external_assets(void);
esp_err_t ai_chat_font_attach_external_assets(void);
const lv_font_t *ai_chat_font_get_fallback(void);
const lv_font_t *ai_chat_font_get_current(void);
const lv_font_t *ai_chat_font_get(void);
bool ai_chat_font_is_ready(void);
bool ai_chat_font_uses_external_assets(void);
