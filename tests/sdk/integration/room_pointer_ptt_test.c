// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_room_ptt_feedback.py
#include "audio_fakes.h"
#include "fake_link.h"
#include "fakes.h"
#include "lvgl.h"
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/modules/room.h"
#include "xiaotai/theme_lcd.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t room_pointer_test_advance(unsigned advance);
void room_pointer_test_drain(void);
void room_pointer_test_join(void);

const xiaotai_module_t room_pointer_test_ui = {
    .name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start,
};

static int lcd_match(const xiaotai_display_desc_t *desc) { return desc->width == 320 ? 100 : 0; }

static xiaotai_err_t lcd_start(const xiaotai_ui_host_t *host, void **out)
{
    return xiaotai_theme_lcd_start(host, NULL, out);
}

void room_pointer_test_prepare(void);
void room_pointer_test_prepare(void)
{
    xiaotai_display_desc_t desc = ui_fake_desc(320, 240, XIAOTAI_PIXEL_RGB565);
    desc.touch = true;
    ui_fake_board_set_displays(&desc, 1);
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    ui_driver_manual(true);
}

xiaotai_err_t room_pointer_test_init(void);
xiaotai_err_t room_pointer_test_init(void)
{
    static const xiaotai_theme_t theme = {.name = "room-fixture", .backend = XIAOTAI_UI_BACKEND_LVGL,
        .match = lcd_match, .start = lcd_start, .render = xiaotai_theme_lcd_render,
        .tick = xiaotai_theme_lcd_tick, .sleep = xiaotai_theme_lcd_sleep, .controls = xiaotai_theme_lcd_controls,
        .stop = xiaotai_theme_lcd_stop, .stack_bytes = XIAOTAI_THEME_LCD_STACK_BYTES};
    const xiaotai_err_t err = xiaotai_ui_theme_register(&theme);
    return err == XIAOTAI_OK ? companion_fixture_init() : err;
}

void room_pointer_test_reset(void);
void room_pointer_test_reset(void) { xiaotai_ui_reset(); }

static lv_obj_t *find_text(lv_obj_t *root, const char *label)
{
    if (lv_obj_has_class(root, &lv_label_class) && !strcmp(lv_label_get_text(root), label))
        return lv_obj_get_parent(root);
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find_text(lv_obj_get_child(root, (int32_t)i), label);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *render_ptt(const char *label, unsigned advance)
{
    const uint32_t now = room_pointer_test_advance(advance);
    ui_display_t *display = ui_display_at(0);
    ui_driver_step(display, now);
    return find_text(lv_display_get_screen_active(display->host.root), label);
}

void room_pointer_test_run(void);
void room_pointer_test_run(void)
{
    lv_obj_t *button = render_ptt("按下讲话", 20);
    assert(button && lv_obj_send_event(button, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(button, LV_EVENT_PRESS_LOST, NULL) == LV_RESULT_OK);
    room_pointer_test_drain();
    assert(fixture.toasts == 1 && !strcmp(fixture.toast, "尚未就绪或麦克风已关闭") && !fake_link.gate_close_calls);
    room_pointer_test_join();
    xiaotai_room_status_t status;
    xiaotai_room_status(&status);
    assert(status.phase == XIAOTAI_ROOM_LISTENING && status.generation);
    button = render_ptt("按下讲话", 20);
    assert(button);
    const unsigned open = fake_link.gate_open_calls;
    const unsigned close = fake_link.gate_close_calls;

    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_ROOM,
                               &(xiaotai_args_t){.text = "leave"}) == XIAOTAI_OK);
    assert(render_ptt("按下讲话", 20) == button);
    assert(ui_display_at(0)->scene.overlay.present &&
           !strcmp(fixture.overlay_title, "退出房间") && !strcmp(fixture.overlay_body, "退出后需要重新加入"));
    assert(lv_obj_send_event(button, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(button, LV_EVENT_PRESS_LOST, NULL) == LV_RESULT_OK);
    room_pointer_test_drain();
    assert(fixture.toasts == 1 && !fake_link.gate_open && fake_link.gate_open_calls == open &&
           fake_link.gate_close_calls == close);
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_CONFIRM,
                               &(xiaotai_args_t){.value = 0}) == XIAOTAI_OK);
    assert(render_ptt("按下讲话", 20) == button && !ui_display_at(0)->scene.overlay.present);
    puts("room leave confirmation blocks underlay PTT press and lost release");

    audio_fixture.status.mic_muted = true;
    assert(lv_obj_send_event(button, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(button, LV_EVENT_PRESS_LOST, NULL) == LV_RESULT_OK);
    room_pointer_test_drain();
    assert(fixture.toasts == 2 && !strcmp(fixture.toast, "尚未就绪或麦克风已关闭") &&
           !fake_link.gate_open && fake_link.gate_open_calls == open && fake_link.gate_close_calls == close);
    audio_fixture.status.mic_muted = false;
    assert(lv_obj_send_event(button, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK && fake_link.gate_open);
    assert(render_ptt("松开结束讲话", 749) == button && fake_link.gate_open);
    assert(lv_obj_send_event(button, LV_EVENT_PRESSING, NULL) == LV_RESULT_OK);
    assert(render_ptt("松开结束讲话", 750) == button && fake_link.gate_open && fake_link.gate_close_calls == close);
    assert(render_ptt("按下讲话", 1) == button && !fake_link.gate_open && fake_link.gate_close_calls == close + 1U);
    assert(lv_obj_send_event(button, LV_EVENT_PRESSING, NULL) == LV_RESULT_OK && !fake_link.gate_open);
    assert(lv_obj_send_event(button, LV_EVENT_RELEASED, NULL) == LV_RESULT_OK && fake_link.gate_close_calls == close + 1U);
    assert(lv_obj_send_event(button, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK && fake_link.gate_open);
    lv_obj_delete(button);
    assert(!fake_link.gate_open && fake_link.gate_close_calls == close + 2U);
    room_pointer_test_drain();
    printf("LCD -> owner room -> media gate: notices=%u, opens=%u, closes=%u\n", fixture.toasts,
           fake_link.gate_open_calls, fake_link.gate_close_calls - close);
}
