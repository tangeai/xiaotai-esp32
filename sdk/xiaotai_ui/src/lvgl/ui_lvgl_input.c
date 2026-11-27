// SPDX-License-Identifier: MIT
/* Contacts into LVGL's pointer, and what coasts on after a drag. LVGL is fed
 * once for each contact and not otherwise: it takes a read without movement
 * for a finger at rest and forgets the swipe it was counting. Its own coasting
 * runs once a frame, so it would go as fast as the display refreshes; here it
 * follows the clock. */
#include "ui_lvgl_input.h"
#include "src/indev/lv_indev_private.h"

#include <math.h>

/* Units (1/240 of the shorter side) and units per second. */
#define SCROLL_LIMIT 8             /* a drag begins */
#define SWIPE_DISTANCE 40
#define COAST_TAU_MS 325.0f
#define COAST_FROM 90.0f
#define COAST_UNTIL 18.0f
#define COAST_MAX 3200.0f
#define CATCH_SPEED 60.0f          /* a touch that stops a faster list is no tap on it */

static void gone(lv_event_t *event)
{
    ui_coast_t *c = lv_event_get_user_data(event);
    if (c->obj == lv_event_get_target_obj(event)) c->obj = NULL;
}

static int32_t position(const ui_coast_t *c)
{
    return c->vertical ? lv_obj_get_scroll_y(c->obj) : lv_obj_get_scroll_x(c->obj);
}

void ui_coast_stop(ui_coast_t *c)
{
    if (c->obj) (void)lv_obj_remove_event_cb_with_user_data(c->obj, gone, c);
    c->obj = NULL;
}

static void coast_start(ui_lvgl_input_t *in, lv_obj_t *obj, lv_dir_t dir, float speed, uint32_t now)
{
    ui_coast_t *c = &in->coast;
    ui_coast_stop(c);
    if (speed > COAST_MAX * in->unit) speed = COAST_MAX * in->unit;
    if (speed < -COAST_MAX * in->unit) speed = -COAST_MAX * in->unit;
    if (fabsf(speed) < COAST_FROM * in->unit || !lv_obj_add_event_cb(obj, gone, LV_EVENT_DELETE, c)) return;
    *c = (ui_coast_t){.obj = obj, .vertical = dir == LV_DIR_VER, .speed = speed, .at = now};
    c->left_at = position(c);
}

void ui_coast_step(ui_lvgl_input_t *in, uint32_t now)
{
    ui_coast_t *c = &in->coast;
    if (!c->obj || now == c->at) return;
    if (position(c) != c->left_at) {   /* the application moved it: it knows where */
        ui_coast_stop(c);
        return;
    }
    float dt = (float)(now - c->at);
    c->at = now;
    if (dt > 100.0f) dt = 100.0f;   /* a long frame must not turn into a leap */
    const float keep = expf(-dt / COAST_TAU_MS);
    c->rest += c->speed * (COAST_TAU_MS / 1000.0f) * (1.0f - keep);
    c->speed *= keep;
    int32_t step = (int32_t)c->rest;
    c->rest -= (float)step;
    const int32_t room = c->vertical ? (c->speed > 0 ? lv_obj_get_scroll_top(c->obj) : lv_obj_get_scroll_bottom(c->obj))
                                     : (c->speed > 0 ? lv_obj_get_scroll_left(c->obj) : lv_obj_get_scroll_right(c->obj));
    const bool edge = LV_ABS(step) >= room;
    if (edge) step = room <= 0 ? 0 : step > 0 ? room : -room;
    lv_obj_t *const obj = c->obj;
    const bool ends = edge || fabsf(c->speed) < COAST_UNTIL * in->unit;
    if (step) lv_obj_scroll_by(obj, c->vertical ? 0 : step, c->vertical ? step : 0, LV_ANIM_OFF);
    if (ends) ui_coast_stop(c);
    else if (c->obj) c->left_at = position(c);
}

void ui_lvgl_input_open(ui_lvgl_input_t *in, lv_indev_t *indev, uint16_t width, uint16_t height)
{
    const uint16_t side = width < height ? width : height;
    *in = (ui_lvgl_input_t){.indev = indev, .unit = side ? (float)side / 240.0f : 1.0f};
    const float swipe = SWIPE_DISTANCE * in->unit, drag = SCROLL_LIMIT * in->unit;
    lv_indev_set_scroll_limit(indev, (uint8_t)(drag > 255.0f ? 255.0f : drag));
    lv_indev_set_gesture_min_distance(indev, (uint8_t)(swipe > 255.0f ? 255.0f : swipe));
    lv_indev_set_gesture_min_velocity(indev, (uint8_t)(in->unit > 2.0f ? 2 : 1));
}

/* What LVGL would let coast and what it aligns to a snap point itself. */
static bool coasts(lv_obj_t *obj, lv_dir_t dir)
{
    if (!obj || !lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLL_MOMENTUM)) return false;
    return (dir == LV_DIR_VER ? lv_obj_get_scroll_snap_y(obj) : lv_obj_get_scroll_snap_x(obj)) == LV_SCROLL_SNAP_NONE;
}

void ui_lvgl_input_feed(ui_lvgl_input_t *in, const ui_contact_t *contact, uint32_t now)
{
    lv_indev_t *const indev = in->indev;
    const bool pressed = contact->down && !in->contact.down, released = !contact->down && in->contact.down;
    in->contact = *contact;
    bool caught = false;
    lv_obj_t *scrolled = NULL;
    lv_dir_t dir = LV_DIR_NONE;
    if (pressed) {
        caught = in->coast.obj && fabsf(in->coast.speed) > CATCH_SPEED * in->unit;
        ui_coast_stop(&in->coast);
    }
    if (released && coasts(indev->pointer.scroll_obj, (lv_dir_t)indev->pointer.scroll_dir)) {
        scrolled = indev->pointer.scroll_obj;
        dir = (lv_dir_t)indev->pointer.scroll_dir;
        /* Nothing left to throw: LVGL ends the drag here, or pulls back what
         * was dragged past its end. */
        indev->pointer.scroll_throw_vect = (lv_point_t){0};
    }
    lv_indev_read(indev);
    if (caught && indev->pointer.act_obj) {
        indev->pointer.pressed = 0;   /* no click at the end of this contact */
        lv_obj_remove_state(indev->pointer.act_obj, LV_STATE_PRESSED);
    }
    if (scrolled && lv_obj_is_valid(scrolled) && !lv_anim_get(scrolled, NULL))
        coast_start(in, scrolled, dir, dir == LV_DIR_VER ? contact->vy : contact->vx, now);
}
