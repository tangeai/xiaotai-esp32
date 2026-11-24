// SPDX-License-Identifier: MIT
#pragma once

#include "lcd_internal.h"

/* The clock page of a desk (CLOCK with XIAOTAI_VIEW_CALENDAR): the time, what
 * day it is and what comes next. A display with room for it shows the month
 * beside the time, a smaller one the week below it. */
typedef struct lcd_clock lcd_clock_t;

/* Children of `root`, drawn inside `area` (display pixels). NULL without memory. */
lcd_clock_t *lcd_clock_open(lcd_state_t *s, lv_obj_t *root);
/* Takes the time and what the view says comes next; true when the page has to
 * be placed (again): on the first one, on a new day, when the cards change. */
bool lcd_clock_update(lcd_clock_t *c, const xiaotai_view_t *view, const xiaotai_ui_scene_t *scene);
/* `phone`: the page has a call shortcut, which the clock page finds room for.
 * Returns where (an empty box without one). */
lcd_box_t lcd_clock_place(lcd_clock_t *c, const xiaotai_view_t *view, lcd_box_t area, bool phone);
uint32_t lcd_clock_tick(lcd_clock_t *c, uint32_t now_ms);
/* Call after the layer's root has been deleted. */
void lcd_clock_free(lcd_clock_t *c);
