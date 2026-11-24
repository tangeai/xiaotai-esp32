// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_hardware_boundaries.py
#include "ui_test.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"
#include "xiaotai/theme_lcd.h"
#include "lvgl.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#ifdef LCD_TEST_REAL_THEME
#include "xiaotai/port/sys.h"
#include "xiaotai/theme_lcd320.h"
#endif

enum { WAIT_MS = 2000, ACTIONS = 5, PRIMARY_VALUE = 42 };
static xiaotai_signal_t owner_done, primary;
static atomic_uint calls;

static const xiaotai_view_action_t buttons[ACTIONS] = {
    {.role = XIAOTAI_ROLE_PRIMARY, .intent = "lcd.test.primary", .value = PRIMARY_VALUE},
    {.icon = XIAOTAI_ICON_MENU, .intent = "lcd.test.menu"},
    {.icon = XIAOTAI_ICON_PREVIOUS, .intent = "lcd.test.previous"},
    {.icon = XIAOTAI_ICON_NEXT, .intent = "lcd.test.next"},
    {.icon = XIAOTAI_ICON_PHONE, .intent = "lcd.test.phone"},
};

static void handle_primary(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && args->value == PRIMARY_VALUE);
    atomic_fetch_add(&calls, 1U);
    xiaotai_signal_give(&primary);
}

#ifndef LCD_TEST_REAL_THEME
static atomic_uint custom_opened, custom_updated, custom_closed;

static int fixture_match(const xiaotai_display_desc_t *d) { return d->pixel == XIAOTAI_PIXEL_RGB565 ? 100 : 0; }

static xiaotai_err_t fixture_start(const xiaotai_ui_host_t *host, void **state)
{
    assert(!xiaotai_on_owner());
    /* Exercise the kit at larger sizes before their production variant lands. */
    return xiaotai_theme_lcd_start(host, NULL, state);
}

static const xiaotai_theme_t fixture_theme = {
    .name = "kit-fixture", .backend = XIAOTAI_UI_BACKEND_LVGL, .match = fixture_match,
    .start = fixture_start, .render = xiaotai_theme_lcd_render, .tick = xiaotai_theme_lcd_tick,
    .sleep = xiaotai_theme_lcd_sleep, .controls = xiaotai_theme_lcd_controls,
    .stop = xiaotai_theme_lcd_stop, .stack_bytes = XIAOTAI_THEME_LCD_STACK_BYTES,
};

static xiaotai_err_t custom_open(const xiaotai_ui_host_t *host, const xiaotai_ui_canvas_t *canvas,
                                 const xiaotai_ui_layer_t *layer, void **state)
{
    assert(!xiaotai_on_owner() && xiaotai_ui_host_index(host) == 2);
    assert(canvas->backend == XIAOTAI_UI_BACKEND_LVGL && canvas->width == 1024 && canvas->height == 600);
    assert(layer->view.layer == XIAOTAI_LAYER_OVERLAY);
    lv_obj_t *marker = lv_obj_create(canvas->root);
    lv_obj_remove_style_all(marker);
    lv_obj_set_pos(marker, canvas->width - 20, canvas->height - 20);
    lv_obj_set_size(marker, 12, 12);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
    *state = marker;
    atomic_fetch_add(&custom_opened, 1U);
    return XIAOTAI_OK;
}

static void custom_update(void *state, const xiaotai_ui_canvas_t *canvas, const xiaotai_ui_layer_t *layer,
                          const xiaotai_ui_scene_t *scene)
{
    (void)scene;
    assert(!xiaotai_on_owner() && lv_obj_get_parent(state) == canvas->root);
    const uint32_t color = !strcmp(layer->view.text, "second") ? 0x0000ff : 0xff00ff;
    lv_obj_set_style_bg_color(state, lv_color_hex(color), 0);
    atomic_fetch_add(&custom_updated, 1U);
}

static void custom_close(void *state)
{
    assert(!xiaotai_on_owner() && lv_obj_is_valid(state));
    atomic_fetch_add(&custom_closed, 1U);
}

static const xiaotai_ui_template_t overlay_template = {
    .name = "scale.overlay", .backend = XIAOTAI_UI_BACKEND_LVGL, .match = fixture_match,
    .open = custom_open, .update = custom_update, .close = custom_close,
};
#endif

static xiaotai_err_t app_init(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(buttons[0].intent, handle_primary, NULL);
    if (err != XIAOTAI_OK) return err;
#ifdef LCD_TEST_REAL_THEME
    return xiaotai_engine_theme_lcd320_init();
#else
    err = xiaotai_ui_theme_register(&fixture_theme);
    return err == XIAOTAI_OK ? xiaotai_ui_template_register(&overlay_template) : err;
#endif
}

static void show_on_owner(void *view)
{
    assert(xiaotai_on_owner());
    xiaotai_view_show(view);
    xiaotai_signal_give(&owner_done);
}

static void stage(xiaotai_view_t *view)
{
    xiaotai_ui_info_t info;
    assert(xiaotai_ui_inspect(view->display, &info) == XIAOTAI_OK);
    const uint32_t before = info.frames;
    assert(xiaotai_run(show_on_owner, view) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_done, WAIT_MS));
    /* The owner accepts a view before its display has rendered it. Capture
     * intentionally sees the rendered scene, so wait for this update. */
    for (unsigned elapsed = 0; elapsed < WAIT_MS; ++elapsed) {
        assert(xiaotai_ui_inspect(view->display, &info) == XIAOTAI_OK);
        if (info.frames != before) return;
        xiaotai_delay_ms(1);
    }
    assert(!"display did not render the staged view");
}

static void show(uint8_t index, const char *title, const char *text, xiaotai_speaker_t speaker)
{
    xiaotai_view_t view = {.kind = XIAOTAI_VIEW_HOME, .id = "home", .display = index,
        .title = title, .text = text, .speaker = speaker, .actions = buttons, .action_count = ACTIONS};
    stage(&view);
}

static xiaotai_ui_capture_t capture(uint8_t index, unsigned width, unsigned height)
{
    xiaotai_ui_capture_t frame;
    assert(xiaotai_ui_capture(index, &frame, WAIT_MS) == XIAOTAI_OK);
    assert(frame.width == width && frame.height == height && frame.pixel == XIAOTAI_PIXEL_RGB565 &&
           frame.size == width * height * 2U);
    return frame;
}

static int setup(const uint16_t (*dimensions)[2], size_t size)
{
    xiaotai_display_desc_t descs[UI_FAKE_DISPLAYS_MAX];
    CHECK(size <= UI_FAKE_DISPLAYS_MAX);
    for (size_t i = 0; i < size; ++i) {
        descs[i] = ui_fake_desc(dimensions[i][0], dimensions[i][1], XIAOTAI_PIXEL_RGB565);
        descs[i].transfer_pixels = dimensions[i][0] * 16U;
        descs[i].touch = true;
    }
    ui_fake_board_set_displays(descs, size);
    CHECK(xiaotai_signal_init(&owner_done) == XIAOTAI_OK && xiaotai_signal_init(&primary) == XIAOTAI_OK);
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "lcd-fixture", .init = app_init};
    static const xiaotai_module_t *const modules[] = {&ui};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 1, .applications = apps,
        .application_count = 1};
    CHECK(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    for (uint8_t i = 0; i < size; ++i) {
        xiaotai_ui_info_t info;
        CHECK(xiaotai_ui_inspect(i, &info) == XIAOTAI_OK && info.ready && !info.owner_timer);
#ifdef LCD_TEST_REAL_THEME
        CHECK(!strcmp(info.theme, XIAOTAI_THEME_LCD320_NAME));
#endif
        /* Consume the initial empty display step before staging the first
         * view, so its redraw cannot satisfy the new-view frame wait. */
        size_t count;
        CHECK(xiaotai_ui_controls(i, NULL, 0, &count, WAIT_MS) == XIAOTAI_OK && !count);
    }
    return 0;
}

static int expect_controls(uint8_t index, unsigned width, unsigned height)
{
    static const unsigned base[][4] = {{282, 3, 34, 28}, {0, 94, 36, 52}, {284, 94, 36, 52}, {266, 136, 46, 46}};
    xiaotai_ui_control_t controls[ACTIONS] = {0};
    size_t count = 0;
    const xiaotai_err_t err = xiaotai_ui_controls(index, controls, ACTIONS, &count, WAIT_MS);
    if (err != XIAOTAI_OK || count != ACTIONS) {
        xiaotai_ui_info_t info;
        assert(xiaotai_ui_inspect(index, &info) == XIAOTAI_OK);
        fprintf(stderr, "display %u (%ux%u) controls: result=%d count=%zu\n", index, width, height, err, count);
        fprintf(stderr, "page=%s theme=%s frames=%u dropped=%u shown=%u truncated=%u\n",
                info.page, info.theme, info.frames, info.dropped, info.shown, info.truncated);
    }
    CHECK(err == XIAOTAI_OK && count == ACTIONS);
    unsigned found = 0;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_ui_control_t *c = &controls[i];
        CHECK(c->index < ACTIONS && !(found & (1U << c->index)) && !strcmp(c->intent, buttons[c->index].intent));
        CHECK(c->layer == XIAOTAI_LAYER_PAGE && c->element == XIAOTAI_UI_ELEMENT_ACTION && c->state == 0);
        found |= 1U << c->index;
        if (c->index == 0) {
            const unsigned w = 224U * height / 240U, h = 108U * height / 240U;
            CHECK(c->x == (int)((width - w) / 2U) && c->y == (int)((height - h) / 2U));
            CHECK(c->width == w && c->height == h);
        } else {
            const unsigned *rect = base[c->index - 1U];
            unsigned x = rect[0] * width / 320U, y = rect[1] * height / 240U;
            unsigned w = rect[2] * width / 320U, h = rect[3] * height / 240U;
            if (c->index == ACTIONS - 1) {   /* the call shortcut is a circle, three text lines at most */
                unsigned side = w < h ? w : h;
                if (side > 60U) side = 60U;
                x += w - side;
                y += h - side;
                w = h = side;
            }
            CHECK(c->x == (int)x && c->y == (int)y && c->width == w && c->height == h);
        }
    }
    return 0;
}

static uint16_t rgb565(uint32_t rgb) { return lv_color_to_u16(lv_color_hex(rgb)); }

static uint16_t pixel_at(const xiaotai_ui_capture_t *f, unsigned x, unsigned y)
{
    const uint8_t *p = &f->pixels[(y * f->width + x) * 2U];
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8U));
}

typedef struct { unsigned left, top, right, bottom, count; } ink_t;

static ink_t ink(const xiaotai_ui_capture_t *f, unsigned x1, unsigned y1, unsigned x2, unsigned y2, uint32_t rgb)
{
    ink_t box = {.left = x2, .top = y2};
    const uint16_t color = rgb565(rgb);
    for (unsigned y = y1; y < y2; ++y) for (unsigned x = x1; x < x2; ++x) {
        if (pixel_at(f, x, y) != color) continue;
        if (x < box.left) box.left = x;
        if (x > box.right) box.right = x;
        if (y < box.top) box.top = y;
        if (y > box.bottom) box.bottom = y;
        ++box.count;
    }
    return box;
}

static int expect_caption_labels(uint8_t i, unsigned w, unsigned h)
{
    const unsigned bottom = h - 48U;
    show(i, "", "answer", XIAOTAI_SPEAKER_ASSISTANT);
    xiaotai_ui_capture_t a = capture(i, w, h);
    const ink_t bare = ink(&a, 4, bottom, w - 4U, h, 0xb5dfff);
    xiaotai_ui_capture_free(&a);

    show(i, "", "AI：answer", XIAOTAI_SPEAKER_ASSISTANT);
    a = capture(i, w, h);
    const ink_t ai = ink(&a, 4, bottom, w - 4U, h, 0xb5dfff);
    xiaotai_ui_capture_free(&a);
    show(i, "", "你：answer", XIAOTAI_SPEAKER_USER);
    a = capture(i, w, h);
    const ink_t user = ink(&a, 4, bottom, w - 4U, h, 0xbce8cf);
    xiaotai_ui_capture_free(&a);
    CHECK(bare.count > 20 && ai.count > bare.count && user.count > bare.count);
    /* The prefix is drawn in the same line: two proportional Latin letters or
     * one CJK character, then a full-width colon (about 34-36 px at 18 px). */
    CHECK(ai.right >= bare.right + 28U && ai.right <= bare.right + 40U);
    CHECK(user.right >= bare.right + 28U && user.right <= bare.right + 40U);

    /* The older caption line can scroll away; the last two lines stay put. */
    show(i, "", "AI：first\nKEEP\nTAIL", XIAOTAI_SPEAKER_ASSISTANT);
    a = capture(i, w, h);
    show(i, "", "first\nKEEP\nTAIL", XIAOTAI_SPEAKER_ASSISTANT);
    xiaotai_ui_capture_t b = capture(i, w, h);
    const size_t offset = (size_t)w * bottom * 2U;
    const bool same = memcmp(a.pixels + offset, b.pixels + offset, a.size - offset) == 0;
    xiaotai_ui_capture_free(&b);
    xiaotai_ui_capture_free(&a);
    CHECK(same);
    return 0;
}

#ifdef LCD_TEST_REAL_THEME
static int exercise(void)
{
    static const uint16_t dims[][2] = {{320, 240}};
    CHECK(setup(dims, 1) == 0);
    /* The body shows only the lowest two lines. Altering earlier rows must
     * not change any pixel in or around the caption. */
    show(0, "\xe4\xbd\xa0\xe5\xa5\xbd", "AAAA\nBBBB\nMMMM\nWWWW", XIAOTAI_SPEAKER_ASSISTANT);
    CHECK(expect_controls(0, 320, 240) == 0);
    xiaotai_ui_capture_t a = capture(0, 320, 240);
    /* The call shortcut: a filled green circle. */
    CHECK(pixel_at(&a, 289, 140) == rgb565(0x1fa672) && pixel_at(&a, 267, 137) != rgb565(0x1fa672));
    const ink_t ai = ink(&a, 4, 185, 275, 240, 0xb5dfff);
    CHECK(ai.count > 20 && ai.top >= 190 && ai.bottom > 225 && ai.bottom - ai.top > 20);
    CHECK(ink(&a, 4, 150, 275, 190, 0xb5dfff).count == 0);
    show(0, "\xe6\x82\xa8\xe5\xa5\xbd", "ZZZZ\nTTTT\nMMMM\nWWWW", XIAOTAI_SPEAKER_ASSISTANT);
    xiaotai_ui_capture_t b = capture(0, 320, 240);
    for (unsigned y = 185; y < 240; ++y) for (unsigned x = 4; x < 275; ++x)
        CHECK(pixel_at(&a, x, y) == pixel_at(&b, x, y));
    /* Actual unequal Chinese glyph rasters, not two identical fallback boxes. */
    unsigned changed = 0;
    for (unsigned y = 7; y < 27; ++y) for (unsigned x = 40; x < 58; ++x)
        changed += pixel_at(&a, x, y) != pixel_at(&b, x, y);
    CHECK(changed > 12 && ink(&b, 40, 7, 76, 27, 0xf2f5f7).count > 15);
    xiaotai_ui_capture_free(&a);
    xiaotai_ui_capture_free(&b);

    show(0, "\xe4\xbd\xa0\xe5\xa5\xbd", "ZZZZ\nTTTT\nMMMM\nWWWW", XIAOTAI_SPEAKER_USER);
    a = capture(0, 320, 240);
    const ink_t user = ink(&a, 4, 185, 275, 240, 0xbce8cf);
    CHECK(user.count > 20 && user.top == ai.top && user.bottom == ai.bottom);
    CHECK(ink(&a, 4, 185, 275, 240, 0xb5dfff).count == 0);
    xiaotai_ui_capture_free(&a);

    CHECK(expect_caption_labels(0, 320, 240) == 0);
    CHECK(xiaotai_ui_tap(0, 160, 120) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&primary, WAIT_MS));
    CHECK(!xiaotai_signal_take(&primary, 60));
    CHECK(atomic_load(&calls) == 1);
    return 0;
}
#else
static int exercise_native_template(void)
{
    xiaotai_view_t overlay = {.kind = XIAOTAI_VIEW_CUSTOM, .layer = XIAOTAI_LAYER_OVERLAY,
        .display = 2, .id = "native", .custom = "scale.overlay", .text = "first"};
    stage(&overlay);
    xiaotai_ui_capture_t frame = capture(2, 1024, 600);
    CHECK(pixel_at(&frame, 1009, 585) == rgb565(0xff00ff) && atomic_load(&custom_opened) == 1);
    xiaotai_ui_capture_free(&frame);

    overlay.text = "second";
    stage(&overlay);
    frame = capture(2, 1024, 600);
    CHECK(pixel_at(&frame, 1009, 585) == rgb565(0x0000ff));
    CHECK(atomic_load(&custom_opened) == 1 && atomic_load(&custom_updated) >= 2 && atomic_load(&custom_closed) == 0);
    xiaotai_ui_capture_free(&frame);

    overlay.flags = XIAOTAI_VIEW_DISMISS;
    stage(&overlay);
    frame = capture(2, 1024, 600);
    CHECK(pixel_at(&frame, 1009, 585) != rgb565(0x0000ff) && atomic_load(&custom_closed) == 1);
    xiaotai_ui_capture_free(&frame);
    return 0;
}

static int exercise(void)
{
    static const uint16_t dims[][2] = {{320, 240}, {480, 320}, {1024, 600}};
    CHECK(setup(dims, sizeof(dims) / sizeof(dims[0])) == 0);
    ink_t title_baseline = {0};
    for (uint8_t i = 0; i < sizeof(dims) / sizeof(dims[0]); ++i) {
        const unsigned w = dims[i][0], h = dims[i][1];
        show(i, "\xe4\xbd\xa0\xe5\xa5\xbd", "Welcome", XIAOTAI_SPEAKER_NONE);
        CHECK(expect_controls(i, w, h) == 0);
        xiaotai_ui_capture_t frame = capture(i, w, h);
        /* After the brand mark: margin, a line-high mark, a gap; centred in the bar. */
        const unsigned x = 12U * w / 320U + 28U, y = 3U * h / 240U + (28U * h / 240U - 20U) / 2U;
        ink_t title = ink(&frame, x, y, x + 36, y + 20, 0xf2f5f7);
        xiaotai_ui_capture_free(&frame);
        CHECK(title.count > 15);
        title.left -= x;
        title.right -= x;
        title.top -= y;
        title.bottom -= y;
        if (i == 0) title_baseline = title;
        else CHECK(title.count == title_baseline.count && title.left == title_baseline.left &&
                   title.top == title_baseline.top && title.right == title_baseline.right &&
                   title.bottom == title_baseline.bottom);
        CHECK(expect_caption_labels(i, w, h) == 0);
    }
    return exercise_native_template();
}
#endif

int main(void)
{
    const int result = exercise();
    xiaotai_ui_reset();
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&primary);
    xiaotai_signal_deinit(&owner_done);
    return result;
}
