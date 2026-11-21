// SPDX-License-Identifier: MIT
#pragma once

#include "lcd_internal.h"

/* The layer's root owns every LVGL object. Free the form state after deleting
 * that root, so object callbacks cannot outlive their context. */
bool lcd_form_build(lcd_state_t *state, lcd_layer_t *layer, uint8_t index, xiaotai_view_kind_t kind);
void lcd_form_update(lcd_layer_t *layer, const xiaotai_ui_layer_t *view, uint32_t now_ms);
void lcd_form_free(lcd_layer_t *layer);
size_t lcd_form_controls(const lcd_layer_t *layer, xiaotai_ui_control_t *out, size_t capacity);
