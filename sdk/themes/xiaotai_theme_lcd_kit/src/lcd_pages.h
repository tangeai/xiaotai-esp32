// SPDX-License-Identifier: MIT
#pragma once

#include "lcd_internal.h"

typedef struct lcd_pages lcd_pages_t;

bool lcd_pages_matches(xiaotai_view_kind_t kind);
lcd_pages_t *lcd_pages_open(lcd_state_t *s, lcd_layer_t *layer, xiaotai_view_kind_t kind, uint8_t index);
void lcd_pages_update(lcd_pages_t *pages, const xiaotai_ui_layer_t *layer, uint32_t now_ms);
uint32_t lcd_pages_tick(lcd_pages_t *pages, uint32_t now_ms);
size_t lcd_pages_controls(lcd_pages_t *pages, xiaotai_ui_control_t *out, size_t capacity);
/* Call after the layer's root and its event callbacks have been deleted. */
void lcd_pages_free(lcd_pages_t *pages);
