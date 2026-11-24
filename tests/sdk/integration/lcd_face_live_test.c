// SPDX-License-Identifier: MIT
#include "lcd_face.h"
#include "lcd_internal.h"
#include "lvgl/ui_lvgl_private.h"
#include "ui_test.h"
#include "xiaotai/at.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/theme_lcd320.h"
#include "xiaotai/theme_lcd480.h"
#include "xiaotai_mem_host.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { W = 320, H = 240, FACE_X = 50, FACE_Y = 66, WIDE_W = 1024, WIDE_H = 600, WAIT_MS = 2000,
       FACE_PIXELS = LCD_FACE_WIDTH * LCD_FACE_HEIGHT };
static uint16_t panel[WIDE_W * WIDE_H];
static uint16_t face_before[FACE_PIXELS], face_reference[FACE_PIXELS];
static uint16_t width = W, height = H;
static lv_area_t dirty[8];
static unsigned invalidated, face_deletes;
static bool manual, trace_dirty;
static uint32_t now_ms;
static uint64_t presented_pixels;
static lcd_state_t *kit;
static xiaotai_signal_t shown, pressed;

static const xiaotai_theme_t *stock_theme(void)
{
    return width == W ? &xiaotai_theme_lcd320 : &xiaotai_theme_lcd480;
}

static uint32_t clock_ms(void) { return now_ms; }

static void note_dirty(lv_event_t *e)
{
    if (!trace_dirty) return;
    const lv_area_t *area = lv_event_get_param(e);
    /* LVGL also reflows empty status labels at (0, 0); measure the face. */
    if (area->x2 < FACE_X || area->x1 >= FACE_X + LCD_FACE_WIDTH ||
        area->y2 < FACE_Y || area->y1 >= FACE_Y + LCD_FACE_HEIGHT) return;
    if (invalidated < sizeof(dirty) / sizeof(dirty[0])) dirty[invalidated] = *area;
    ++invalidated;
}

static void note_face_delete(lv_event_t *e)
{
    (void)e;
    ++face_deletes;
}

static xiaotai_err_t fixture_start(const xiaotai_ui_host_t *host, void **out)
{
    assert(manual || !xiaotai_on_owner());
    xiaotai_err_t err = stock_theme()->start(host, out);
    if (err == XIAOTAI_OK) {
        kit = *out;
        lv_display_add_event_cb(xiaotai_ui_host_root(host), note_dirty, LV_EVENT_INVALIDATE_AREA, NULL);
    }
    return err;
}

static void primary(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && args->value == 1);
    xiaotai_signal_give(&pressed);
}

static xiaotai_err_t app_init(void)
{
    static xiaotai_theme_t theme;
    theme = *stock_theme();
    theme.start = fixture_start;
    xiaotai_err_t err = xiaotai_ui_theme_register(&theme);
    return err == XIAOTAI_OK ? xiaotai_intent_handle("test.face", primary, NULL) : err;
}

static xiaotai_err_t present(void *ctx, const xiaotai_rect_t *area, const void *bytes)
{
    (void)ctx;
    assert(manual && area->x + area->width <= width && area->y + area->height <= height);
    presented_pixels += (uint64_t)area->width * area->height;
    for (size_t y = 0; y < area->height; ++y)
        memcpy(&panel[(area->y + y) * width + area->x], (const uint8_t *)bytes + y * area->width * 2U, area->width * 2U);
    return XIAOTAI_OK;
}

static void step(uint32_t timestamp)
{
    now_ms = timestamp;
    ui_driver_step(ui_display_at(0), timestamp);
}

static int setup(void)
{
    xiaotai_display_desc_t desc = ui_fake_desc(width, height, XIAOTAI_PIXEL_RGB565);
    /* For scaled snapshot parity, keep the software scanout and capture
     * clipping equal: LVGL derives its inverse scale step per draw area. */
    if (width != W) desc.render = XIAOTAI_RENDER_FULL;
    desc.transfer_pixels = width * (width == W ? 24U : height);
    desc.touch = true;
    ui_fake_board_set_displays(&desc, 1);
    if (manual) {
        ((xiaotai_display_t *)ui_fake_display(0))->present = present;
        ui_core_set_clock(clock_ms);
        ui_driver_manual(true);
    }
    CHECK(xiaotai_signal_init(&shown) == XIAOTAI_OK && xiaotai_signal_init(&pressed) == XIAOTAI_OK);
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "face-fixture", .init = app_init};
    static const xiaotai_module_t *const modules[] = {&ui};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 1,
        .applications = apps, .application_count = 1};
    CHECK(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    xiaotai_ui_info_t info;
    CHECK(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.ready && !info.owner_timer);
    CHECK(strcmp(info.theme, stock_theme()->name) == 0);
    if (manual) step(0);
    return 0;
}

static void teardown(void)
{
    xiaotai_ui_reset();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_signal_deinit(&shown);
    xiaotai_signal_deinit(&pressed);
}

static void show(void *view)
{
    assert(xiaotai_on_owner());
    xiaotai_view_show(view);
    xiaotai_signal_give(&shown);
}

static const xiaotai_view_action_t action = {.role = XIAOTAI_ROLE_PRIMARY, .intent = "test.face", .value = 1};

static xiaotai_view_t home(void)
{
    return (xiaotai_view_t){.kind = XIAOTAI_VIEW_HOME, .id = "face", .actions = &action, .action_count = 1};
}

typedef struct { unsigned late, slow, direct, pixels, fallback; } counters_t;

static counters_t counters(void)
{
    char line[192];
    counters_t out;
    xiaotai_theme_lcd_report(kit, line, sizeof(line));
    assert(sscanf(line, "face_late=%u face_slow=%u direct=%u direct_pixels=%u fallback=%u",
                  &out.late, &out.slow, &out.direct, &out.pixels, &out.fallback) == 5);
    return out;
}

static bool inside_dirty(int x, int y)
{
    for (unsigned n = 0; n < invalidated && n < sizeof(dirty) / sizeof(dirty[0]); ++n)
        if (x >= dirty[n].x1 && x <= dirty[n].x2 && y >= dirty[n].y1 && y <= dirty[n].y2) return true;
    return false;
}

static uint16_t rgb565(uint32_t rgb) { return lv_color_to_u16(lv_color_hex(rgb)); }

static void copy_face(uint16_t pixels[FACE_PIXELS], const uint16_t *screen)
{
    for (size_t y = 0; y < LCD_FACE_HEIGHT; ++y)
        memcpy(pixels + y * LCD_FACE_WIDTH, screen + (FACE_Y + y) * W + FACE_X,
               LCD_FACE_WIDTH * sizeof(*pixels));
}

static size_t painted_face(const uint16_t pixels[FACE_PIXELS], uint16_t background)
{
    size_t count = 0;
    for (size_t i = 0; i < FACE_PIXELS; ++i) count += pixels[i] != background;
    return count;
}

static int fresh_pose(uint32_t background, uint64_t at)
{
    lcd_face_t *face;
    CHECK(lcd_face_create(background, &face) == XIAOTAI_OK);
    const xiaotai_ui_surface_t surface = {.pixels = (uint8_t *)face_reference, .width = LCD_FACE_WIDTH,
        .height = LCD_FACE_HEIGHT, .stride = LCD_FACE_WIDTH * sizeof(uint16_t), .pixel = XIAOTAI_PIXEL_RGB565};
    lcd_face_rect_t regions[LCD_FACE_REGIONS];
    lcd_face_attach(face, at);
    const size_t count = lcd_face_step(face, at, true, regions);
    const bool rendered = lcd_face_canvas(face, &surface);
    lcd_face_destroy(face);
    CHECK(count == LCD_FACE_REGIONS && rendered);
    CHECK(painted_face(face_reference, rgb565(background)) > 500);
    return 0;
}

static int clean_attach(uint64_t at)
{
    CHECK(fresh_pose(LCD_BACKGROUND, at) == 0);
    copy_face(face_before, panel);
    CHECK(memcmp(face_before, face_reference, sizeof(face_before)) == 0);
    return 0;
}

static int styled_painter(lv_obj_t *image, const void *source)
{
    static uint16_t blue[8 * 8];
    static const lv_image_dsc_t background = {
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = 8, .h = 8, .stride = 16},
        .data_size = sizeof(blue), .data = (const uint8_t *)blue,
    };
    for (size_t i = 0; i < sizeof(blue) / sizeof(blue[0]); ++i) blue[i] = 0x001f;
    const size_t centre = (FACE_Y + LCD_FACE_HEIGHT / 2) * W + FACE_X + LCD_FACE_WIDTH / 2;
    lv_image_set_rotation(image, 0);
    lv_image_set_scale_y(image, LV_SCALE_NONE);
    lv_obj_set_style_radius(image, 0, 0);
    lv_obj_set_style_bg_image_src(image, &background, 0);
    lv_obj_set_style_bg_image_opa(image, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(image, LV_OPA_TRANSP, 0);
    counters_t c = counters();
    step(now_ms + 40);
    CHECK(panel[centre] == 0x001f && counters().direct == c.direct && counters().fallback == c.fallback);

    lv_obj_set_style_image_opa(image, LV_OPA_50, 0);
    step(now_ms + 40);
    xiaotai_ui_capture_t frame;
    CHECK(ui_lvgl_capture(ui_display_at(0), &frame) == XIAOTAI_OK);
    CHECK(frame.size == W * H * sizeof(panel[0]));
    const uint16_t reference = ((const uint16_t *)frame.pixels)[centre];
    const lv_image_dsc_t *canvas = source;
    const size_t image_row = LCD_FACE_HEIGHT / 2;
    const uint16_t *row = (const uint16_t *)(canvas->data + image_row * canvas->header.stride);
    const uint16_t face_pixel = row[LCD_FACE_WIDTH / 2];
    xiaotai_ui_capture_free(&frame);
    CHECK(reference == lv_color_16_16_mix(face_pixel, 0x001f, LV_OPA_50) && panel[centre] == reference);
    CHECK(counters().direct == c.direct && counters().fallback == c.fallback + 1);

    /* Matching source pointers cannot distinguish these two LVGL tasks.
     * Let software draw both; preparing the shared canvas is still necessary. */
    c = counters();
    lv_obj_set_style_bg_image_src(image, source, 0);
    step(now_ms + 40);
    CHECK(counters().direct == c.direct && counters().fallback == c.fallback + 1);
    CHECK(ui_lvgl_capture(ui_display_at(0), &frame) == XIAOTAI_OK);
    CHECK(memcmp(frame.pixels, panel, frame.size) == 0);
    xiaotai_ui_capture_free(&frame);

    /* A distinct styled background cannot claim or disable the painter for
     * the now-opaque foreground. It draws the same pixels as a plain image. */
    c = counters();
    lv_obj_set_style_bg_image_src(image, &background, 0);
    lv_obj_set_style_image_opa(image, LV_OPA_COVER, 0);
    step(now_ms + 40);
    CHECK(counters().direct > c.direct && counters().fallback == c.fallback);
    CHECK(ui_lvgl_capture(ui_display_at(0), &frame) == XIAOTAI_OK);
    CHECK(memcmp(frame.pixels, panel, frame.size) == 0);
    xiaotai_ui_capture_free(&frame);
    return 0;
}

static int preview_geometry(const void *source)
{
    xiaotai_view_t preview = {.kind = XIAOTAI_VIEW_TALK, .id = "preview", .emotion = "laughing",
        .flags = XIAOTAI_VIEW_EMOTION_PREVIEW};
    ui_view_accept(&preview);
    step(now_ms + 1);
    lv_obj_t *image = kit->layers[XIAOTAI_LAYER_PAGE].face;
    CHECK(image && lv_image_get_src(image) == source && !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    lv_area_t preview_area, ordinary_area;
    lv_obj_get_coords(image, &preview_area);
    CHECK(preview_area.y1 == 48 * height / H);
    CHECK(preview_area.x1 == (width - LCD_FACE_WIDTH * height / H) / 2);

    preview.flags = 0;
    ui_view_accept(&preview);
    step(now_ms + 1);
    CHECK(kit->layers[XIAOTAI_LAYER_PAGE].face == image && lv_image_get_src(image) == source);
    lv_obj_get_coords(image, &ordinary_area);
    CHECK(ordinary_area.y1 == (height - LCD_FACE_HEIGHT * height / H) / 2);
    CHECK(preview_area.x1 == ordinary_area.x1 && preview_area.x2 == ordinary_area.x2);
    return 0;
}

static int deterministic(void)
{
    manual = true;
    /* The KV worker lives for the process, not the display. */
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    const int64_t baseline = xiaotai_mem_host_outstanding();
    CHECK(setup() == 0);
    xiaotai_view_t page = home();
    ui_view_accept(&page);
    step(0);
    lv_obj_t *image = kit->layers[XIAOTAI_LAYER_PAGE].face;
    CHECK(image && !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_add_event_cb(image, note_face_delete, LV_EVENT_DELETE, NULL));
    CHECK(clean_attach(0) == 0);
    counters_t c = counters();
    CHECK(c.direct && c.pixels >= LCD_FACE_WIDTH * LCD_FACE_HEIGHT && c.fallback == 0);
    const uint32_t allocs = xiaotai_mem_host_allocations();
    const void *source = lv_image_get_src(image);
    trace_dirty = true;
    step(39);
    CHECK(invalidated == 0 && counters().direct == c.direct);
    step(40);
    trace_dirty = false;
    if (invalidated != LCD_FACE_REGIONS) {
        fprintf(stderr, "face invalidations: %u, direct=%u->%u, tick_due=%u\n",
                invalidated, c.direct, counters().direct, ui_display_at(0)->driver.tick_due);
        for (unsigned i = 0; i < invalidated && i < sizeof(dirty) / sizeof(dirty[0]); ++i)
            fprintf(stderr, "  %d,%d .. %d,%d\n", (int)dirty[i].x1, (int)dirty[i].y1,
                    (int)dirty[i].x2, (int)dirty[i].y2);
    }
    CHECK(invalidated == LCD_FACE_REGIONS);
    CHECK(dirty[0].x2 < dirty[1].x1 && dirty[2].x1 > dirty[0].x1 && dirty[2].x2 < dirty[1].x2);
    for (unsigned y = 0; y < LCD_FACE_HEIGHT; ++y)
        for (unsigned x = 0; x < LCD_FACE_WIDTH; ++x)
            CHECK(face_before[y * LCD_FACE_WIDTH + x] == panel[(FACE_Y + y) * W + FACE_X + x] ||
                  inside_dirty(FACE_X + x, FACE_Y + y));
    step(80);
    step(120);
    CHECK(xiaotai_mem_host_allocations() == allocs);
    c = counters();
    CHECK(c.late == 0 && c.fallback == 0);
    xiaotai_ui_capture_t frame;
    /* The same pose is captured in real LVGL software, not a theme mock. */
    CHECK(ui_lvgl_capture(ui_display_at(0), &frame) == XIAOTAI_OK);
    CHECK(frame.width == W && frame.height == H && frame.size == W * H * sizeof(panel[0]));
    CHECK(memcmp(frame.pixels, panel, frame.size) == 0);
    CHECK(counters().direct == c.direct && counters().fallback == 1);
    xiaotai_ui_capture_free(&frame);

    /* Opacity disqualifies the painter on the live draw path. The new frame
     * must reach the actual framebuffer through the prepared image source. */
    copy_face(face_before, panel);
    lv_obj_set_style_image_opa(image, LV_OPA_50, 0);
    step(160);
    CHECK(counters().direct == c.direct && counters().fallback == 2);
    copy_face(face_reference, panel);
    CHECK(memcmp(face_before, face_reference, sizeof(face_before)) != 0);
    lv_obj_set_style_image_opa(image, LV_OPA_COVER, 0);
    step(200);
    CHECK(counters().direct > c.direct && counters().fallback == 2);

    xiaotai_view_t overlay = {.kind = XIAOTAI_VIEW_NOTICE, .id = "modal", .layer = XIAOTAI_LAYER_OVERLAY};
    ui_view_accept(&overlay);
    step(210);
    CHECK(lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    c = counters();
    step(550);
    CHECK(counters().direct == c.direct && counters().late == c.late);
    ui_view_accept(&(xiaotai_view_t){.flags = XIAOTAI_VIEW_DISMISS, .layer = XIAOTAI_LAYER_OVERLAY, .id = "modal"});
    step(551);
    CHECK(!lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) && counters().direct > c.direct && counters().late == c.late);
    CHECK(xiaotai_ui_sleep(0) == XIAOTAI_OK && !xiaotai_ui_awake(0));
    CHECK(lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    c = counters();
    step(1100);
    CHECK(counters().direct == c.direct && counters().late == c.late);
    CHECK(xiaotai_ui_wake(0) == XIAOTAI_OK);
    step(1101);
    CHECK(face_deletes == 0);
    CHECK(!lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) && counters().direct > c.direct && counters().late == c.late);
    step(1200);
    CHECK(counters().late == c.late + 1);

    ui_view_accept(&overlay);
    step(now_ms + 1);
    CHECK(lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    c = counters();
    const uint64_t before_refresh = presented_pixels;
    CHECK(xiaotai_ui_refresh(0) == XIAOTAI_OK);
    step(now_ms + 1);
    CHECK(face_deletes == 0 && kit->layers[XIAOTAI_LAYER_PAGE].face == image &&
          lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) && counters().direct == c.direct);
    CHECK(presented_pixels - before_refresh >= (uint64_t)W * H);
    ui_view_accept(&(xiaotai_view_t){.flags = XIAOTAI_VIEW_DISMISS, .layer = XIAOTAI_LAYER_OVERLAY, .id = "modal"});
    step(now_ms + 1);
    CHECK(face_deletes == 0 && !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));

    /* A real page identity change received while asleep must still rebuild
     * on wake even after the scene has stopped reporting it as new. */
    CHECK(xiaotai_ui_sleep(0) == XIAOTAI_OK);
    ui_view_accept(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CLOCK, .id = "clock"});
    step(now_ms + 1);
    step(now_ms + 1);
    CHECK(face_deletes == 0);
    CHECK(xiaotai_ui_wake(0) == XIAOTAI_OK);
    step(now_ms + 1);
    CHECK(face_deletes == 1 && !kit->layers[XIAOTAI_LAYER_PAGE].face);
    const uint32_t before_return = xiaotai_mem_host_allocations();
    page.kind = XIAOTAI_VIEW_TALK;
    page.id = "talk";
    ui_view_accept(&page);
    const uint32_t attach_at = now_ms + 1;
    step(attach_at);
    image = kit->layers[XIAOTAI_LAYER_PAGE].face;
    CHECK(image && lv_image_get_src(image) == source);
    CHECK(xiaotai_mem_host_allocations() == before_return);
    CHECK(clean_attach(attach_at) == 0);

    /* Nonuniform scale plus rotation must use the same software pixels
     * on incremental refresh and on a full render of the changed pose. */
    page.phase = XIAOTAI_TALK_SPEAKING;
    page.emotion = "happy";
    ui_view_accept(&page);
    step(attach_at + 1);
    c = counters();
    lv_image_set_pivot(image, LCD_FACE_WIDTH / 2, LCD_FACE_HEIGHT / 2);
    lv_image_set_rotation(image, 150);
    lv_image_set_scale_y(image, LV_SCALE_NONE * 3 / 4);
    step(attach_at + 40);
    CHECK(counters().direct == c.direct && counters().fallback == c.fallback + 1);
    xiaotai_ui_capture_t previous;
    CHECK(ui_lvgl_capture(ui_display_at(0), &previous) == XIAOTAI_OK);
    invalidated = 0;
    trace_dirty = true;
    step(attach_at + 80);
    trace_dirty = false;
    CHECK(counters().direct == c.direct && counters().fallback == c.fallback + 2);
    CHECK(ui_lvgl_capture(ui_display_at(0), &frame) == XIAOTAI_OK);
    CHECK(invalidated == LCD_FACE_REGIONS && previous.size == frame.size);
    /* Both reference frames have the same LVGL clip endpoints. This tests
     * every changed pixel without assuming an incremental clip rounds the
     * inverse step the same way as a full clip. */
    size_t changed = 0;
    for (unsigned i = 0; i < W * H; ++i) {
        if (((const uint16_t *)previous.pixels)[i] == ((const uint16_t *)frame.pixels)[i]) continue;
        CHECK(inside_dirty(i % W, i / W));
        ++changed;
    }
    xiaotai_ui_capture_free(&previous);
    CHECK(changed != 0);
    /* With a full-width sweep, partial panel and software capture have
     * identical clip origins for every row and must match bit for bit. */
    ui_display_t *display = ui_display_at(0);
    ui_lvgl_enter(display);
    lv_display_t *screen = xiaotai_ui_host_root(kit->host);
    lv_obj_invalidate(lv_display_get_screen_active(screen));
    lv_refr_now(screen);
    ui_lvgl_leave(display);
    CHECK(memcmp(frame.pixels, panel, frame.size) == 0);
    CHECK(counters().direct == c.direct && counters().fallback == c.fallback + 2);
    xiaotai_ui_capture_free(&frame);
    CHECK(styled_painter(image, source) == 0);
    CHECK(preview_geometry(source) == 0);

    teardown();
    if (xiaotai_mem_host_outstanding() != baseline)
        fprintf(stderr, "port allocations at close: %lld -> %lld\n",
                (long long)baseline, (long long)xiaotai_mem_host_outstanding());
    CHECK(xiaotai_mem_host_outstanding() == baseline);
    return 0;
}

static int scaled(uint16_t w, uint16_t h)
{
    manual = true;
    width = w;
    height = h;
    CHECK(setup() == 0);
    xiaotai_view_t page = home();
    ui_view_accept(&page);
    step(0);
    lv_obj_t *image = kit->layers[XIAOTAI_LAYER_PAGE].face;
    CHECK(image && !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    const lv_image_dsc_t *source = lv_image_get_src(image);
    CHECK(source->header.cf == LV_COLOR_FORMAT_RGB565 && source->header.w == LCD_FACE_WIDTH &&
          source->header.h == LCD_FACE_HEIGHT);
    CHECK(source->data_size == LCD_FACE_HEIGHT * source->header.stride);
    const unsigned scale = lv_image_get_scale(image);
    CHECK(scale == height * LV_SCALE_NONE / H);
    lv_point_t pivot;
    lv_image_get_pivot(image, &pivot);
    CHECK(pivot.x == 0 && pivot.y == 0);
    lv_area_t area;
    lv_obj_get_coords(image, &area);
    CHECK(area.x1 == (width - LCD_FACE_WIDTH * height / H) / 2 &&
          area.y1 == (height - LCD_FACE_HEIGHT * height / H) / 2);
    /* The stock LCD480 paints the original centre colour in the face canvas
     * over the gradient, and scales that canvas with LVGL software drawing. */
    const uint32_t background = 0x181c21;
    CHECK(panel[0] == rgb565(0x101418));
    CHECK(panel[(size_t)area.y1 * width + area.x1] == rgb565(background));
    CHECK(fresh_pose(background, 0) == 0);
    for (size_t y = 0; y < LCD_FACE_HEIGHT; ++y)
        CHECK(memcmp(source->data + y * source->header.stride,
                     face_reference + y * LCD_FACE_WIDTH, LCD_FACE_WIDTH * sizeof(uint16_t)) == 0);
    unsigned solid = 0;
    for (unsigned y = 2; y < LCD_FACE_HEIGHT - 2; ++y) {
        for (unsigned x = 2; x < LCD_FACE_WIDTH - 2; ++x) {
            const uint16_t color = face_reference[y * LCD_FACE_WIDTH + x];
            if (color == rgb565(background)) continue;
            bool same = true;
            /* Match solid interior pixels, not samples that may interpolate
             * with an authored edge at the fixed-point zoom endpoint. */
            for (unsigned sy = y - 2; sy <= y + 2; ++sy)
                for (unsigned sx = x - 2; sx <= x + 2; ++sx)
                    same = same && face_reference[sy * LCD_FACE_WIDTH + sx] == color;
            if (!same) continue;
            const size_t panel_y = area.y1 + y * scale / LV_SCALE_NONE;
            const size_t panel_x = area.x1 + x * scale / LV_SCALE_NONE;
            if (panel[panel_y * width + panel_x] != color)
                fprintf(stderr, "%ux%u scale=%u face=(%u,%u) panel=(%zu,%zu) RGB565=%04x expected=%04x\n",
                        width, height, scale, x, y, panel_x, panel_y, panel[panel_y * width + panel_x], color);
            CHECK(panel[panel_y * width + panel_x] == color);
            ++solid;
        }
    }
    CHECK(solid > 500);
    CHECK(counters().direct == 0 && counters().fallback == 1);
    xiaotai_ui_capture_t frame;
    CHECK(ui_lvgl_capture(ui_display_at(0), &frame) == XIAOTAI_OK);
    CHECK(frame.width == width && frame.height == height && frame.size == (size_t)width * height * sizeof(panel[0]));
    const uint16_t *snapshot = (const uint16_t *)frame.pixels;
    size_t differences = 0;
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        if (snapshot[i] == panel[i]) continue;
        if (++differences <= 3)
            fprintf(stderr, "%ux%u screenshot pixel=(%zu,%zu) panel=%04x capture=%04x\n",
                    width, height, i % width, i / width, panel[i], snapshot[i]);
    }
    if (differences) fprintf(stderr, "scaled full-frame comparison: %zu different pixels\n", differences);
    CHECK(memcmp(frame.pixels, panel, frame.size) == 0 && counters().fallback == 1);
    xiaotai_ui_capture_free(&frame);
    step(40);
    CHECK(counters().direct == 0 && counters().fallback == 2);
    CHECK(preview_geometry(source) == 0);
    teardown();
    return 0;
}

static int owner(void)
{
    CHECK(setup() == 0);
    xiaotai_view_t page = home();
    CHECK(xiaotai_run(show, &page) == XIAOTAI_OK && xiaotai_signal_take(&shown, WAIT_MS));
    xiaotai_ui_control_t c;
    size_t count = 0;
    const int64_t end = xiaotai_time_us() + WAIT_MS * 1000U;
    do {
        CHECK(xiaotai_ui_controls(0, &c, 1, &count, WAIT_MS) == XIAOTAI_OK);
        if (count != 1) xiaotai_delay_ms(1);
    } while (count != 1 && xiaotai_time_us() < end);
    CHECK(count == 1);
    CHECK(c.x <= FACE_X && c.y == FACE_Y && !strcmp(c.intent, "test.face"));
    CHECK(xiaotai_ui_tap(0, W / 2, H / 2) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&pressed, WAIT_MS));
    xiaotai_ui_capture_t frame;
    CHECK(xiaotai_ui_capture(0, &frame, WAIT_MS) == XIAOTAI_OK);
    CHECK(frame.width == W && frame.height == H && frame.size == W * H * sizeof(panel[0]));
    copy_face(face_before, (const uint16_t *)frame.pixels);
    CHECK(painted_face(face_before, rgb565(LCD_BACKGROUND)) > 500);
    xiaotai_ui_capture_free(&frame);
    CHECK(counters().direct > 0 && counters().fallback > 0);
    teardown();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "owner")) return owner();
    if (argc > 1 && !strcmp(argv[1], "scaled")) return scaled(480, 320);
    if (argc > 1 && !strcmp(argv[1], "wide")) return scaled(WIDE_W, WIDE_H);
    return deterministic();
}
