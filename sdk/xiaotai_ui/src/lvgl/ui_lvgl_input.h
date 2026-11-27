// SPDX-License-Identifier: MIT
#pragma once

#include "lvgl.h"
#include "ui_touch.h"

/* A list that coasts on after the finger left. */
typedef struct {
    lv_obj_t *obj;              /* NULL: nothing coasts */
    bool vertical;
    float speed, rest;          /* px/s; the part of a pixel not scrolled yet */
    uint32_t at;
    int32_t left_at;            /* where the last step left it: anything else moved it since */
} ui_coast_t;

typedef struct {
    lv_indev_t *indev;
    ui_contact_t contact;       /* what LVGL reads */
    ui_coast_t coast;
    float unit;
} ui_lvgl_input_t;

/* Called with the LVGL lock held. */
void ui_lvgl_input_open(ui_lvgl_input_t *in, lv_indev_t *indev, uint16_t width, uint16_t height);
void ui_lvgl_input_feed(ui_lvgl_input_t *in, const ui_contact_t *contact, uint32_t now);
void ui_coast_step(ui_lvgl_input_t *in, uint32_t now);
void ui_coast_stop(ui_coast_t *coast);
