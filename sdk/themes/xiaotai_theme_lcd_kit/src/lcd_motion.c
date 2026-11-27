// SPDX-License-Identifier: MIT
/* Motion for the colour pages. The UI system draws on demand and does not run
 * LVGL's timer handler, so LVGL animations and style transitions never
 * advance here. The few moving things are stepped from the theme's tick: a
 * page, dialog or notice settling into place, a progress bar reaching its
 * value, the busy ring and the ring around a waiting call. Each touches only
 * its own object, costs nothing while idle and stops with the object. A page
 * settles by position alone: fading a whole page would need an off-screen
 * layer of it, and its first frame would be an empty screen. */
#include "lcd_internal.h"

enum { ENTER_MS = 140, WIDTH_MS = 220, STEP_MS = 16, LOOP_STEP_MS = 60, SLOW_STEP_MS = 40 };

static lcd_move_t *claim(lcd_state_t *s, const lv_obj_t *obj, lcd_move_kind_t kind)
{
    lcd_move_t *spare = NULL;
    for (size_t i = 0; i < LCD_MOVES; ++i) {
        lcd_move_t *move = &s->moves[i];
        if (move->kind == kind && move->obj == obj) return move;
        if (!spare && move->kind == LCD_MOVE_NONE) spare = move;
    }
    return spare;
}

void lcd_move_enter(lcd_state_t *s, lv_obj_t *obj, uint32_t now_ms)
{
    /* Settling redraws the whole page at every step. Where that takes longer
     * than the step it would only hold up what the user does next. */
    if (s->slow) return;
    lcd_move_t *move = claim(s, obj, LCD_MOVE_ENTER);
    if (!obj || !move) return;   /* every slot busy: the object simply appears */
    *move = (lcd_move_t){.obj = obj, .kind = LCD_MOVE_ENTER, .start_ms = now_ms, .duration_ms = ENTER_MS,
                         .from = lcd_dp(s, 10)};
    lv_obj_set_style_translate_y(obj, move->from, 0);
}

void lcd_move_width(lcd_state_t *s, lv_obj_t *obj, int32_t to, uint32_t now_ms)
{
    const int32_t from = lv_obj_get_style_width(obj, 0);
    lcd_move_t *move = claim(s, obj, LCD_MOVE_WIDTH);
    if (!move || from == to || s->slow) {
        lv_obj_set_width(obj, to);
        if (move && move->kind == LCD_MOVE_WIDTH) move->kind = LCD_MOVE_NONE;
        return;
    }
    if (move->kind == LCD_MOVE_WIDTH && move->to == to) return;
    *move = (lcd_move_t){.obj = obj, .kind = LCD_MOVE_WIDTH, .start_ms = now_ms, .duration_ms = WIDTH_MS,
                         .from = from, .to = to};
}

void lcd_move_loop(lcd_state_t *s, lv_obj_t *obj, lcd_move_kind_t kind, uint16_t period_ms)
{
    lcd_move_t *move = claim(s, obj, kind);
    if (!obj || !move || move->kind == kind) return;
    *move = (lcd_move_t){.obj = obj, .kind = kind, .duration_ms = period_ms,
                         .from = lv_obj_get_style_width(obj, 0)};
}

void lcd_move_drop(lcd_state_t *s, const lv_obj_t *obj)
{
    for (size_t i = 0; i < LCD_MOVES; ++i) {
        lcd_move_t *move = &s->moves[i];
        if (move->kind == LCD_MOVE_NONE) continue;
        for (const lv_obj_t *at = move->obj; at; at = lv_obj_get_parent(at)) {
            if (at != obj) continue;
            move->kind = LCD_MOVE_NONE;
            move->obj = NULL;
            break;
        }
    }
}

void lcd_move_settle(lcd_state_t *s)
{
    for (size_t i = 0; i < LCD_MOVES; ++i) {
        lcd_move_t *move = &s->moves[i];
        if (move->kind != LCD_MOVE_ENTER) continue;
        lv_obj_remove_local_style_prop(move->obj, LV_STYLE_TRANSLATE_Y, 0);
        move->kind = LCD_MOVE_NONE;
        move->obj = NULL;
    }
}

uint32_t lcd_move_tick(lcd_state_t *s, uint32_t now_ms)
{
    uint32_t next = 0;
    bool settling = false;
    for (size_t i = 0; i < LCD_MOVES; ++i) settling |= s->moves[i].kind == LCD_MOVE_ENTER && s->moves[i].started;
    /* The frame of the step before took that long. */
    if (settling && s->moved_ms && now_ms - s->moved_ms > SLOW_STEP_MS) {
        s->slow = true;
        lcd_move_settle(s);
    }
    s->moved_ms = 0;
    for (size_t i = 0; i < LCD_MOVES; ++i) {
        lcd_move_t *move = &s->moves[i];
        if (move->kind == LCD_MOVE_NONE) continue;
        s->moved_ms = now_ms;
        if (!move->started) {   /* time runs from the first step after the frame that built the object */
            move->started = true;
            move->start_ms = now_ms;
        }
        const uint32_t elapsed = now_ms - move->start_ms;
        uint32_t wait = STEP_MS;
        if (move->kind == LCD_MOVE_SPIN) {
            lv_arc_set_rotation(move->obj, (int32_t)(elapsed % move->duration_ms) * 360 / move->duration_ms);
            wait = LOOP_STEP_MS;
        } else if (move->kind == LCD_MOVE_PULSE) {
            /* A ring grows from the badge to one and a half times its size and fades out. */
            const int32_t phase = (int32_t)(elapsed % move->duration_ms) * 256 / move->duration_ms;
            const int32_t side = move->from + move->from * phase / 512;
            lv_obj_set_size(move->obj, side, side);
            lv_obj_center(move->obj);
            lv_obj_set_style_border_opa(move->obj, (lv_opa_t)((256 - phase) * 150 / 256), 0);
            wait = LOOP_STEP_MS;
        } else {
            int32_t t = elapsed >= move->duration_ms ? 256 : (int32_t)(elapsed * 256U / move->duration_ms);
            const int32_t rest = 256 - t;
            t = 256 - rest * rest / 256 * rest / 256;   /* ease out */
            if (move->kind == LCD_MOVE_ENTER) {
                lv_obj_set_style_translate_y(move->obj, move->from * (256 - t) / 256, 0);
                if (elapsed >= move->duration_ms) lv_obj_remove_local_style_prop(move->obj, LV_STYLE_TRANSLATE_Y, 0);
            } else {
                lv_obj_set_width(move->obj, move->from + (move->to - move->from) * t / 256);
            }
            if (elapsed >= move->duration_ms) {
                move->kind = LCD_MOVE_NONE;
                move->obj = NULL;
                continue;
            }
        }
        if (!next || wait < next) next = wait;
    }
    return next;
}
