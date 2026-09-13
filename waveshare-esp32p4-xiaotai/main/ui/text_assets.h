#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lvgl.h"

typedef struct {
    const char *text;
    uint8_t size;
    int8_t x_offset;
    int8_t y_offset;
    const lv_img_dsc_t *image;
} ui_text_asset_t;

uint8_t ui_text_asset_normalize_size(uint8_t size);
const ui_text_asset_t *ui_text_asset_find(const char *text, uint8_t size);
bool ui_text_asset_has_cjk(const char *text);
