// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai/ui_owner_frame.h"

#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
static uint8_t *owner_frame(size_t bytes)
{
    return companion.buffer && !companion.building_view && bytes <= sizeof(companion.buffer->owner_frame)
        ? companion.buffer->owner_frame : NULL;
}

static void ui_reset(void) { xiaotai_ui_owner_frame_forget(owner_frame); }
#endif

#ifdef XIAOTAI_COMPANION_WECHAT_CODE_ASSET
#define WECHAT_CODE_OVERLAY COMPANION_WECHAT_CODE_OVERLAY
#define WECHAT_CODE_CLOSE "companion.wechat_code.close"

extern const xiaotai_ui_image_t xiaotai_companion_wechat_code;

static xiaotai_err_t show_wechat_code(void)
{
    if (!companion.has_display || !xiaotai_ui_image_find(XIAOTAI_APP_COMPANION_IMAGE_WECHAT_CODE))
        return XIAOTAI_ERR_NOT_SUPPORTED;
    static const xiaotai_view_action_t back = {.role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK,
        .intent = WECHAT_CODE_CLOSE};
    const xiaotai_view_t view = {.kind = XIAOTAI_VIEW_CODE, .layer = XIAOTAI_LAYER_OVERLAY,
        .id = WECHAT_CODE_OVERLAY, .image = XIAOTAI_APP_COMPANION_IMAGE_WECHAT_CODE, .title = "微信扫码添加联系人",
        .actions = &back, .action_count = 1};
    companion_settings_code_visible(true);
    xiaotai_view_show(&view);
    return XIAOTAI_OK;
}

static void close_wechat_code(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    if (!companion.status.foreground || !companion.code_visible) return;
    companion_hide_wechat_code();
}

static xiaotai_err_t register_wechat_code(void)
{
    const xiaotai_ui_image_t *const image = &xiaotai_companion_wechat_code;
    if (!image->name || strcmp(image->name, XIAOTAI_APP_COMPANION_IMAGE_WECHAT_CODE) ||
        image->format != XIAOTAI_IMAGE_MONO1 ||
        image->size != ((image->width + 7U) / 8U) * image->height) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t err = xiaotai_ui_image_register(image);
    return err == XIAOTAI_OK ? xiaotai_intent_handle_ex(WECHAT_CODE_CLOSE, close_wechat_code, NULL,
        XIAOTAI_INTENT_LOCAL) : err;
}
#endif

static void ui_event(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    const bool touched = !strcmp(event->name, XIAOTAI_EVENT_UI_INTERACTION) && event->args.value == 0;
    const bool screen = !strcmp(event->name, XIAOTAI_EVENT_UI_SLEEP) && !strcmp(event->args.text, "0");
    if (screen && !event->args.value) companion.woke_us = xiaotai_time_us();
    if (touched && companion.status.foreground) companion_home_interaction();
    else if (touched || (screen && !event->args.value)) companion_home_present();
    if (!companion.status.foreground) return;
    if (screen) {
        companion_settings_screen_changed(!event->args.value);
        if (event->args.value) companion_home_screen_dark();
    }
    if (!strcmp(event->name, XIAOTAI_EVENT_UI_REJECTED) && xiaotai_ui_rejection_current(&event->args))
        companion_refuse(event->args.text, XIAOTAI_ERR_NOT_SUPPORTED);
}

static xiaotai_err_t ui_init(void)
{
    if (!companion.has_display) return XIAOTAI_OK;
    xiaotai_err_t err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_UI_INTERACTION, ui_event, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_UI_SLEEP, ui_event, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_UI_REJECTED, ui_event, NULL);
#ifdef XIAOTAI_COMPANION_WECHAT_CODE_ASSET
    if (err == XIAOTAI_OK) err = register_wechat_code();
#endif
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    if (err == XIAOTAI_OK) err = xiaotai_ui_owner_frame_offer(owner_frame);
#endif
    return err;
}

static bool ui_awake(void) { return xiaotai_ui_awake(0); }
static xiaotai_err_t ui_sleep(void) { return xiaotai_ui_sleep(0); }
static void ui_timeout(uint32_t timeout) { (void)xiaotai_ui_set_sleep_timeout(timeout); }
static void ui_brightness(uint8_t percent) { (void)xiaotai_ui_set_brightness(0, percent); }

static struct {
    companion_local_room_hold_fn_t run;
    companion_local_room_refresh_fn_t refresh;
} local_room_hold;

static xiaotai_err_t ui_room_hold(void *ctx, uint32_t ticket, int32_t value, xiaotai_ui_hold_phase_t phase)
{
    (void)ctx;
    if (phase == XIAOTAI_UI_HOLD_REFRESH)
        return local_room_hold.refresh ? local_room_hold.refresh((uint32_t)value, ticket) : XIAOTAI_ERR_NOT_SUPPORTED;
    return local_room_hold.run(ticket, (uint32_t)value, phase == XIAOTAI_UI_HOLD_BEGIN);
}

static xiaotai_err_t register_room_hold(const char *intent, companion_local_room_hold_fn_t run,
                                         companion_local_room_refresh_fn_t refresh)
{
    const xiaotai_err_t err = xiaotai_ui_local_hold_register(intent, ui_room_hold, NULL);
    if (err == XIAOTAI_OK) {
        local_room_hold.run = run;
        local_room_hold.refresh = refresh;
    }
    return err;
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_ui = {
    .name = "ui", .init = ui_init, .user_activity = xiaotai_ui_activity,
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    .reset = ui_reset,
#endif
    .screen_awake = ui_awake, .screen_sleep = ui_sleep, .sleep_timeout = ui_timeout,
    .brightness = ui_brightness,
    .register_local_room_hold = register_room_hold,
#ifdef XIAOTAI_COMPANION_WECHAT_CODE_ASSET
    .show_wechat_code = show_wechat_code,
#endif
};
