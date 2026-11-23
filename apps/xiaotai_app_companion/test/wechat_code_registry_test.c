// SPDX-License-Identifier: MIT
#include "fakes.h"
#include "ui_internal.h"
#include "ui_test.h"

#include <assert.h>
#include <string.h>

extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_ui;
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {
    &xiaotai_app_companion_with_ui, NULL,
};

#define OPEN "fixture.wechat_code.open"
static xiaotai_signal_t fence;
static xiaotai_err_t result;

static void fence_done(void *ctx) { xiaotai_signal_give(ctx); }
static void flush(void)
{
    assert(xiaotai_run(fence_done, &fence) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&fence, 5000));
}

static void open_code(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    result = companion_show_wechat_code();
}

static xiaotai_err_t app_init(void)
{
    const xiaotai_err_t err = xiaotai_app_companion_init();
    return err == XIAOTAI_OK ? xiaotai_intent_handle_ex(OPEN, open_code, NULL, XIAOTAI_INTENT_LOCAL) : err;
}

static xiaotai_err_t theme_init(void)
{
    const ui_fake_theme_config_t theme = {.name = "test", .backend = XIAOTAI_UI_BACKEND_FRAME,
        .width = 240, .height = 240, .pixel = XIAOTAI_PIXEL_RGB565, .exact = 10};
    return xiaotai_ui_theme_register(ui_fake_theme(0, &theme));
}

static void press_back(ui_display_t *display)
{
    assert(xiaotai_intent_post(XIAOTAI_INTENT_UI_BACK, NULL) == XIAOTAI_OK);
    flush();
    (void)ui_scene_step(display, 0);
    flush();
    (void)ui_scene_step(display, 0);
}

int main(void)
{
    xiaotai_display_desc_t board = ui_fake_desc(240, 240, XIAOTAI_PIXEL_RGB565);
    board.touch = true;
    ui_fake_board_set_displays(&board, 1);
    ui_driver_manual(true);
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const char *const deps[] = {"ui", NULL};
    static const xiaotai_module_t theme = {.name = "theme", .init = theme_init, .depends = deps};
    static const xiaotai_module_t *const modules[] = {&ui, &theme};
    static const xiaotai_application_t app = {.name = "companion", .init = app_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t fw = {.modules = modules, .module_count = 2, .applications = apps,
        .application_count = 1, .available_needs = xiaotai_hal_needs()};
    assert(xiaotai_framework_start(&fw) == XIAOTAI_OK);
    flush();
    ui_display_t *display = ui_display_at(0);
    assert(display);
    (void)ui_scene_step(display, 0);
    assert(display->scene.page.present && !strcmp(display->scene.page.view.id, "home"));

    const xiaotai_ui_image_t *image = xiaotai_ui_image_find(XIAOTAI_APP_COMPANION_IMAGE_WECHAT_CODE);
    assert(companion_show_wechat_code() == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_intent_post(OPEN, NULL) == XIAOTAI_OK);
    flush();
    (void)ui_scene_step(display, 0);
#ifdef XIAOTAI_COMPANION_WECHAT_CODE_ASSET
    extern const xiaotai_ui_image_t xiaotai_companion_wechat_code;
    assert(image == &xiaotai_companion_wechat_code);
    assert(image->format == XIAOTAI_IMAGE_MONO1);
#ifdef COMPANION_TEST_ORIGINAL_CODE
    assert(image->width == 185 && image->height == 185 && image->size == 4440);
#else
    assert(image->width == 9 && image->height == 2 && image->size == 4);
    assert(!memcmp(image->data, (const uint8_t[]){0xaa, 0x80, 0x55, 0x00}, image->size));
#endif
    assert(result == XIAOTAI_OK && companion.code_visible && display->scene.overlay.present);
    const xiaotai_view_t *overlay = &display->scene.overlay.view;
    assert(!strcmp(overlay->id, "wechat.code") && overlay->kind == XIAOTAI_VIEW_CODE);
    assert(!strcmp(overlay->image, image->name) && !strcmp(overlay->title, "微信扫码添加联系人"));
    assert(!(overlay->flags & XIAOTAI_VIEW_QR) && !overlay->text);
    assert(overlay->action_count == 1 && overlay->actions[0].role == XIAOTAI_ROLE_BACK);
    assert(xiaotai_intent_post_external(overlay->actions[0].intent, NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    press_back(display);
    assert(!display->scene.overlay.present && !companion.code_visible);
    assert(display->scene.page.present && !strcmp(display->scene.page.view.id, "home"));
    assert(xiaotai_intent_post(OPEN, NULL) == XIAOTAI_OK);
    flush();
    (void)ui_scene_step(display, 0);
    assert(display->scene.overlay.present);
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_HOME, &(xiaotai_args_t){.value = 1}) == XIAOTAI_OK);
    flush();
    (void)ui_scene_step(display, 0);
    assert(!display->scene.overlay.present && !companion.code_visible);
    assert(display->scene.page.view.kind == XIAOTAI_VIEW_CLOCK);
#else
    assert(result == XIAOTAI_ERR_NOT_SUPPORTED && !image);
    assert(!xiaotai_intent_exists("companion.wechat_code.close") && !display->scene.overlay.present);
    press_back(display);
#endif
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_ui_reset();
    xiaotai_signal_deinit(&fence);
    return 0;
}
