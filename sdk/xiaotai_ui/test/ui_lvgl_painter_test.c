// SPDX-License-Identifier: MIT
// Replaces legacy: tests/lvgl/test_draw_hook.c::draw_hook
#include "ui_test.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui_theme.h"
#include "lvgl.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

static xiaotai_signal_t shown;
static const xiaotai_ui_host_t *host;
static lv_obj_t *image;
static atomic_uint calls, direct_calls;
static atomic_bool draw_direct, match_source;
static int16_t image_x = -2, image_y = 8;
static const struct { const char *id; int16_t x, y; } clip_cases[] = {
    {"clip_inside", 3, 4}, {"clip_lower_left", -2, 22}, {"clip_upper_right", 30, -1},
};
static const uint16_t red[] = {0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800,
                               0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800};
static const lv_image_dsc_t tile = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
    .w = 4, .h = 4, .stride = 8}, .data_size = sizeof(red), .data = (const uint8_t *)red};

static bool paint(void *ctx, const xiaotai_ui_surface_t *target, int16_t origin_x, int16_t origin_y)
{
    assert(!xiaotai_on_owner() && ctx == &calls && target->pixel == XIAOTAI_PIXEL_RGB565);
    assert(target->x < 32 && target->y < 24 && target->width && target->height);
    assert(target->x + target->width <= 32 && target->y + target->height <= 24);
    assert(origin_x == image_x && origin_y == image_y && !target->swap_bytes);
    atomic_fetch_add(&calls, 1U);
    if (!atomic_load(&draw_direct)) return false;
    atomic_fetch_add(&direct_calls, 1U);
    const uint16_t color = atomic_load(&match_source) ? red[0] : 0x07e0;
    for (unsigned y = 0; y < target->height; ++y) {
        for (unsigned x = 0; x < target->width; ++x) {
            uint8_t *p = target->pixels + y * target->stride + x * 2U;
            p[0] = (uint8_t)color;
            p[1] = (uint8_t)(color >> 8U);
        }
    }
    return true;
}

static int match(const xiaotai_display_desc_t *desc) { return desc->width == 32 ? 100 : 0; }

static xiaotai_err_t start(const xiaotai_ui_host_t *h, void **state)
{
    assert(!xiaotai_on_owner() && xiaotai_ui_host_root(h));
    host = h;
    lv_obj_t *root = lv_display_get_screen_active(xiaotai_ui_host_root(h));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    image = lv_image_create(root);
    lv_image_set_src(image, &tile);
    lv_obj_set_pos(image, image_x, image_y);
    assert(xiaotai_ui_host_painter(host, NULL, paint, &calls) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_err_t result = xiaotai_ui_host_painter(host, image, paint, &calls);
    if (result == XIAOTAI_OK) assert(xiaotai_ui_host_painter(host, image, paint, &calls) == XIAOTAI_OK);
    *state = root;
    return result;
}

static void render(void *state, const xiaotai_ui_scene_t *scene)
{
    if (!scene->page.present || !scene->page.view.id) return;
    const char *id = scene->page.view.id;
    for (size_t i = 0; i < sizeof(clip_cases) / sizeof(*clip_cases); ++i) {
        if (strcmp(id, clip_cases[i].id)) continue;
        image_x = clip_cases[i].x;
        image_y = clip_cases[i].y;
        lv_obj_set_pos(image, image_x, image_y);
        return;
    }
    if (!strcmp(id, "rotate")) lv_image_set_rotation(image, 900);
    else if (!strcmp(id, "slots") && image) {
        lv_image_set_rotation(image, 0);
        lv_obj_t *other[XIAOTAI_UI_PAINTERS_MAX];
        for (size_t i = 0; i < XIAOTAI_UI_PAINTERS_MAX; ++i) other[i] = lv_image_create(state);
        for (size_t i = 0; i + 1U < XIAOTAI_UI_PAINTERS_MAX; ++i)
            assert(xiaotai_ui_host_painter(host, other[i], paint, &calls) == XIAOTAI_OK);
        assert(xiaotai_ui_host_painter(host, other[XIAOTAI_UI_PAINTERS_MAX - 1U], paint, &calls) == XIAOTAI_ERR_NO_MEM);
        lv_obj_delete(image);
        image = NULL;
        assert(xiaotai_ui_host_painter(host, other[XIAOTAI_UI_PAINTERS_MAX - 1U], paint, &calls) == XIAOTAI_OK);
        for (size_t i = 0; i < XIAOTAI_UI_PAINTERS_MAX; ++i) lv_obj_delete(other[i]);
    }
}

static xiaotai_err_t theme(void)
{
    static const xiaotai_theme_t d = {.name = "ui-painter", .backend = XIAOTAI_UI_BACKEND_LVGL,
        .match = match, .start = start, .render = render, .stack_bytes = 8192};
    return xiaotai_ui_theme_register(&d);
}

static void show(void *ctx)
{
    const char *id = ctx;
    const xiaotai_view_t view = {.id = id, .kind = XIAOTAI_VIEW_HOME};
    xiaotai_view_show(&view);
    xiaotai_signal_give(&shown);
}

static xiaotai_ui_capture_t capture(const char *id)
{
    assert(xiaotai_run(show, (void *)id) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&shown, 3000));
    xiaotai_ui_capture_t shot;
    assert(xiaotai_ui_capture(0, &shot, 2000) == XIAOTAI_OK);
    assert(shot.width == 32 && shot.height == 24 && shot.pixel == XIAOTAI_PIXEL_RGB565 && shot.size == 32U * 24U * 2U);
    return shot;
}

static unsigned pixel(const xiaotai_ui_capture_t *shot, unsigned x, unsigned y)
{
    const uint8_t *p = shot->pixels + (y * shot->width + x) * 2U;
    return p[0] | ((unsigned)p[1] << 8U);
}

static void compare_clips(void)
{
    atomic_store(&match_source, true);
    for (size_t i = 0; i < sizeof(clip_cases) / sizeof(*clip_cases); ++i) {
        atomic_store(&draw_direct, false);
        const unsigned before = atomic_load(&calls);
        xiaotai_ui_capture_t fallback = capture(clip_cases[i].id);
        assert(atomic_load(&calls) > before);
        bool nonblank = false;
        for (unsigned y = 0; y < fallback.height; ++y)
            for (unsigned x = 0; x < fallback.width; ++x)
                nonblank |= pixel(&fallback, x, y) == red[0];
        assert(nonblank);

        atomic_store(&draw_direct, true);
        const unsigned direct_before = atomic_load(&direct_calls);
        xiaotai_ui_capture_t direct = capture(clip_cases[i].id);
        assert(atomic_load(&direct_calls) > direct_before);
        assert(direct.size == fallback.size && !memcmp(direct.pixels, fallback.pixels, direct.size));
        xiaotai_ui_capture_free(&direct);
        xiaotai_ui_capture_free(&fallback);
    }
}

int main(void)
{
    xiaotai_display_desc_t display = ui_fake_desc(32, 24, XIAOTAI_PIXEL_RGB565);
    display.render = XIAOTAI_RENDER_FULL;
    ui_fake_board_set_displays(&display, 1);
    assert(xiaotai_hal_init() == XIAOTAI_OK && xiaotai_signal_init(&shown) == XIAOTAI_OK);
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "paint", .init = theme};
    static const xiaotai_module_t *const modules[] = {&ui};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1, .applications = apps,
        .application_count = 1};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    xiaotai_ui_capture_t shot = capture("fallback");
    assert(pixel(&shot, 0, 8) == 0xf800 && pixel(&shot, 1, 11) == 0xf800 && pixel(&shot, 2, 8) == 0);
    xiaotai_ui_capture_free(&shot);
    assert(atomic_load(&calls) > 0);
    atomic_store(&draw_direct, true);
    shot = capture("direct");
    assert(pixel(&shot, 0, 8) == 0x07e0 && pixel(&shot, 1, 11) == 0x07e0 && pixel(&shot, 2, 8) == 0);
    xiaotai_ui_capture_free(&shot);
    compare_clips();
    const unsigned before = atomic_load(&calls);
    shot = capture("rotate");
    assert(atomic_load(&calls) == before);
    xiaotai_ui_capture_free(&shot);
    shot = capture("slots");
    xiaotai_ui_capture_free(&shot);
    xiaotai_ui_reset();
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&shown);
    return 0;
}
