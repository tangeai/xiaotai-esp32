// SPDX-License-Identifier: MIT
#pragma once

#include "lcd_internal.h"

xiaotai_err_t lcd_face_live_start(lcd_state_t *s);
lv_obj_t *lcd_face_live_build(lcd_state_t *s, lv_obj_t *parent, const xiaotai_view_t *view);
void lcd_face_live_place(lcd_state_t *s, lv_obj_t *image, const xiaotai_view_t *view);
void lcd_face_live_set(lcd_state_t *s, const xiaotai_view_t *view, const xiaotai_ui_scene_t *scene);
uint32_t lcd_face_live_tick(lcd_state_t *s, uint32_t now_ms);
void lcd_face_live_pause(lcd_state_t *s);
void lcd_face_live_detach(lcd_state_t *s);
void lcd_face_live_stop(lcd_state_t *s);
void lcd_face_live_report(const lcd_state_t *s, char *line, size_t capacity);
