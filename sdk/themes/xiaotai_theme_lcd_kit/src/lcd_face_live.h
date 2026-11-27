// SPDX-License-Identifier: MIT
#pragma once

#include "lcd_internal.h"

xiaotai_err_t lcd_face_live_start(lcd_state_t *s);
lv_obj_t *lcd_face_live_build(lcd_state_t *s, lv_obj_t *parent, const xiaotai_view_t *view);
/* The same in a box of a page that is about something else, at a calmer pace:
 * the sound of a call comes first. NULL where an actor draws the emotion: the
 * page then shows what it shows without one. */
lv_obj_t *lcd_face_live_build_in(lcd_state_t *s, lv_obj_t *parent, lcd_box_t box);
/* Whether the press that ends now stroked the face: no tap. */
bool lcd_face_live_stroked(const lcd_state_t *s);
void lcd_face_live_place(lcd_state_t *s, lv_obj_t *image, const xiaotai_view_t *view);
void lcd_face_live_set(lcd_state_t *s, const xiaotai_view_t *view, const xiaotai_ui_scene_t *scene);
uint32_t lcd_face_live_tick(lcd_state_t *s, uint32_t now_ms);
void lcd_face_live_pause(lcd_state_t *s);
void lcd_face_live_sleep(lcd_state_t *s, bool asleep);
/* An actor draws the emotion on this display (a figure, larger than the face it stands in for). */
bool lcd_face_live_acted(const lcd_state_t *s);
/* Where a tap on the emotion lands, in display pixels. */
lcd_box_t lcd_face_live_hit(lcd_state_t *s, const xiaotai_view_t *view);
void lcd_face_live_detach(lcd_state_t *s);
void lcd_face_live_stop(lcd_state_t *s);
void lcd_face_live_report(const lcd_state_t *s, char *line, size_t capacity);
