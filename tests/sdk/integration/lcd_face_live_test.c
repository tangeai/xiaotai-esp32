// SPDX-License-Identifier: MIT
#include "lcd_face.h"
#include "lcd_internal.h"
#include "lvgl/ui_lvgl_private.h"
#include "src/misc/lv_area_private.h"
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

enum { W = 320, H = 240, FACE_X = 40, FACE_Y = 58, WIDE_W = 1024, WIDE_H = 600, WAIT_MS = 2000,
       FACE_PIXELS = 600 * 310 };
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

/* A display with an actor: what it is asked for, and when. */
static bool with_actor;
static struct { unsigned builds, detaches, places, sets, shown, hidden; lv_obj_t *widget; } acted;

static int actor_match(const xiaotai_display_desc_t *display)
{
    (void)display;
    return 1;
}

static xiaotai_err_t actor_start(const xiaotai_ui_host_t *host, void **state)
{
    (void)host;
    *state = &acted;
    return XIAOTAI_OK;
}

static void actor_stop(void *state) { (void)state; }

static void *actor_build(void *state, void *parent, const xiaotai_theme_lcd_stage_t *stage)
{
    (void)state;
    assert(stage->width == width && stage->height == height);
    ++acted.builds;
    return acted.widget = lv_obj_create(parent);
}

static void actor_place(void *state, const xiaotai_theme_lcd_stage_t *stage)
{
    (void)state;
    (void)stage;
    ++acted.places;
}

static void actor_detach(void *state)
{
    (void)state;
    ++acted.detaches;
}

static void actor_set(void *state, const xiaotai_view_t *view, uint32_t at)
{
    (void)state;
    (void)view;
    (void)at;
    ++acted.sets;
}

static uint32_t actor_tick(void *state, uint32_t at, bool visible)
{
    (void)state;
    (void)at;
    if (visible) ++acted.shown;
    else ++acted.hidden;
    return visible ? 24U : 0U;
}

static xiaotai_ui_rect_t actor_hit(void *state)
{
    (void)state;
    return (xiaotai_ui_rect_t){0, 0, 10, 10};
}

static const xiaotai_theme_lcd_actor_t actor = {.name = "test", .match = actor_match, .start = actor_start, .stop = actor_stop,
    .build = actor_build, .place = actor_place, .detach = actor_detach, .set = actor_set, .tick = actor_tick, .hit = actor_hit};

static const xiaotai_theme_t *stock_theme(void)
{
    return width == W ? &xiaotai_theme_lcd320 : &xiaotai_theme_lcd480;
}

static uint32_t clock_ms(void) { return now_ms; }

static void note_dirty(lv_event_t *e)
{
    if (!trace_dirty) return;
    const lv_area_t *area = lv_event_get_param(e);
    /* What a refresh draws; a painter shown at once is not among it. Measure the face only. */
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
        lv_display_add_event_cb(xiaotai_ui_host_root(host), note_dirty, LV_EVENT_FLUSH_START, NULL);
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
    if (with_actor) assert(xiaotai_theme_lcd_actor_register(&actor) == XIAOTAI_OK);
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
    if (width == WIDE_W) desc.render = XIAOTAI_RENDER_FULL;     /* as a panel that scans out its frame buffer */
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
    xiaotai_theme_lcd_actor_reset();
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
    char line[224];
    counters_t out;
    xiaotai_theme_lcd_report(kit, line, sizeof(line));
    unsigned frames, step_us, paint_us;
    assert(sscanf(line, "face_frames=%u face_late=%u face_slow=%u face_step_us=%u face_us=%u direct=%u direct_pixels=%u fallback=%u",
                  &frames, &out.late, &out.slow, &step_us, &paint_us, &out.direct, &out.pixels, &out.fallback) == 8);
    return out;
}

static uint16_t rgb565(uint32_t rgb) { return lv_color_to_u16(lv_color_hex(rgb)); }

/* The face as it is shown on this display. */
static uint16_t face_w, face_h, face_x, face_y;
static void face_geometry(void)
{
    face_w = (uint16_t)(LCD_FACE_WIDTH * height / H);
    face_h = (uint16_t)(LCD_FACE_HEIGHT * height / H);
    face_x = (uint16_t)((width - face_w) / 2);
    face_y = (uint16_t)((height - face_h) / 2);
}

static void copy_face(uint16_t *pixels, const uint16_t *screen)
{
    for (size_t y = 0; y < face_h; ++y)
        memcpy(pixels + y * face_w, screen + (size_t)(face_y + y) * width + face_x, face_w * sizeof(*pixels));
}

static size_t painted_face(const uint16_t *pixels, uint16_t background)
{
    size_t count = 0;
    for (size_t i = 0; i < (size_t)face_w * face_h; ++i) count += pixels[i] != background;
    return count;
}

static int fresh_pose(uint32_t background, uint64_t at)
{
    lcd_face_t *face;
    CHECK(lcd_face_create(background, &face) == XIAOTAI_OK && lcd_face_size(face, face_w, face_h) == XIAOTAI_OK);
    const xiaotai_ui_surface_t surface = {.pixels = (uint8_t *)face_reference, .width = face_w,
        .height = face_h, .stride = face_w * sizeof(uint16_t), .pixel = XIAOTAI_PIXEL_RGB565};
    lcd_face_rect_t regions[LCD_FACE_REGIONS];
    lcd_face_attach(face, at);
    const size_t count = lcd_face_step(face, at, true, regions);
    const bool rendered = lcd_face_canvas(face, &surface);
    lcd_face_destroy(face);
    CHECK(count == 1 && rendered);
    CHECK(painted_face(face_reference, rgb565(background)) > 500);
    return 0;
}

static int clean_attach(uint32_t background, uint64_t at)
{
    CHECK(fresh_pose(background, at) == 0);
    copy_face(face_before, panel);
    CHECK(memcmp(face_before, face_reference, (size_t)face_w * face_h * sizeof(uint16_t)) == 0);
    return 0;
}

/* What the panel shows is what a full refresh would show. */
static int as_refreshed(void)
{
    xiaotai_ui_capture_t frame;
    CHECK(ui_lvgl_capture(ui_display_at(0), &frame) == XIAOTAI_OK);
    CHECK(frame.width == width && frame.height == height && frame.size == (size_t)width * height * sizeof(panel[0]));
    const uint16_t *snapshot = (const uint16_t *)frame.pixels;
    size_t differences = 0;
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        if (snapshot[i] == panel[i]) continue;
        if (++differences <= 3)
            fprintf(stderr, "%ux%u pixel=(%zu,%zu) panel=%04x refresh=%04x\n", width, height, i % width, i / width, panel[i], snapshot[i]);
    }
    xiaotai_ui_capture_free(&frame);
    CHECK(differences == 0);
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
    CHECK(preview_area.y1 == 48 * height / H && preview_area.x1 == face_x);
    preview.flags = 0;
    ui_view_accept(&preview);
    step(now_ms + 1);
    CHECK(kit->layers[XIAOTAI_LAYER_PAGE].face == image && lv_image_get_src(image) == source);
    lv_obj_get_coords(image, &ordinary_area);
    CHECK(ordinary_area.y1 == face_y);
    CHECK(preview_area.x1 == ordinary_area.x1 && preview_area.x2 == ordinary_area.x2);
    return 0;
}

/* Something drawn over the face is kept: those areas go through a refresh, the others do not. */
static int covered_face(lv_obj_t *image)
{
    ui_display_t *display = ui_display_at(0);
    ui_lvgl_enter(display);
    lv_obj_t *label = lv_label_create(lv_obj_get_parent(image));
    lv_label_set_text(label, "words");
    lv_obj_set_pos(label, face_x + 30, face_y + 50);
    ui_lvgl_leave(display);
    step(now_ms + 20);
    lv_area_t words;
    lv_obj_get_coords(label, &words);
    invalidated = 0;
    trace_dirty = true;
    for (unsigned k = 0; k < 20; ++k) step(now_ms + 20);
    trace_dirty = false;
    CHECK(invalidated > 0);
    for (unsigned n = 0; n < invalidated && n < sizeof(dirty) / sizeof(dirty[0]); ++n) {
        lv_area_t shared;
        CHECK(lv_area_intersect(&shared, &dirty[n], &words));
    }
    CHECK(as_refreshed() == 0);
    ui_lvgl_enter(display);
    lv_obj_delete(label);
    ui_lvgl_leave(display);
    step(now_ms + 20);
    return 0;
}

static int deterministic(void)
{
    manual = true;
    face_geometry();
    CHECK(face_x == FACE_X && face_y == FACE_Y);
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
    CHECK(clean_attach(LCD_BACKGROUND, 0) == 0);
    counters_t c = counters();
    CHECK(c.direct && c.pixels >= LCD_FACE_WIDTH * LCD_FACE_HEIGHT && c.fallback == 0);
    const void *source = lv_image_get_src(image);
    trace_dirty = true;
    step(18);
    CHECK(invalidated == 0 && counters().direct == c.direct);
    for (uint32_t t = 20; t <= 400; t += 20) step(t);
    trace_dirty = false;
    /* Nothing is over the face: it went to the panel past LVGL, and shows what LVGL would. */
    CHECK(invalidated == 0 && counters().direct > c.direct + 3);
    c = counters();
    CHECK(c.late == 0 && c.fallback == 0);
    CHECK(as_refreshed() == 0);
    CHECK(counters().direct == c.direct && counters().fallback == 1);
    const uint32_t allocs = xiaotai_mem_host_allocations();
    step(420);
    step(440);
    CHECK(xiaotai_mem_host_allocations() == allocs);
    CHECK(covered_face(image) == 0);

    /* Opacity disqualifies the painter on the live draw path. The new frame
     * must reach the actual framebuffer through the prepared image source. */
    c = counters();
    copy_face(face_before, panel);
    lv_obj_set_style_image_opa(image, LV_OPA_50, 0);
    step(now_ms + 20);
    CHECK(counters().direct == c.direct && counters().fallback > c.fallback);
    copy_face(face_reference, panel);
    CHECK(memcmp(face_before, face_reference, (size_t)face_w * face_h * sizeof(uint16_t)) != 0);
    lv_obj_set_style_image_opa(image, LV_OPA_COVER, 0);
    step(now_ms + 20);
    c = counters();
    CHECK(as_refreshed() == 0);

    xiaotai_view_t overlay = {.kind = XIAOTAI_VIEW_NOTICE, .id = "modal", .layer = XIAOTAI_LAYER_OVERLAY};
    ui_view_accept(&overlay);
    step(now_ms + 10);
    CHECK(lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    c = counters();
    step(now_ms + 340);
    CHECK(counters().direct == c.direct && counters().late == c.late);
    ui_view_accept(&(xiaotai_view_t){.flags = XIAOTAI_VIEW_DISMISS, .layer = XIAOTAI_LAYER_OVERLAY, .id = "modal"});
    step(now_ms + 1);
    CHECK(!lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) && counters().direct > c.direct && counters().late == c.late);
    CHECK(xiaotai_ui_sleep(0) == XIAOTAI_OK && !xiaotai_ui_awake(0));
    CHECK(lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    c = counters();
    step(now_ms + 550);
    CHECK(counters().direct == c.direct && counters().late == c.late);
    CHECK(xiaotai_ui_wake(0) == XIAOTAI_OK);
    step(now_ms + 1);
    CHECK(face_deletes == 0);
    CHECK(!lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) && counters().direct > c.direct && counters().late == c.late);
    step(now_ms + 100);
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
    CHECK(clean_attach(LCD_BACKGROUND, attach_at) == 0);
    page.phase = XIAOTAI_TALK_SPEAKING;
    page.emotion = "happy";
    ui_view_accept(&page);
    for (unsigned k = 0; k < 60; ++k) step(now_ms + 20);
    CHECK(as_refreshed() == 0);
    CHECK(preview_geometry(source) == 0);

    teardown();
    if (xiaotai_mem_host_outstanding() != baseline)
        fprintf(stderr, "port allocations at close: %lld -> %lld\n",
                (long long)baseline, (long long)xiaotai_mem_host_outstanding());
    CHECK(xiaotai_mem_host_outstanding() == baseline);
    return 0;
}

/* A larger display gets more pixels of the face, not larger ones. */
static int scaled(uint16_t w, uint16_t h)
{
    manual = true;
    width = w;
    height = h;
    face_geometry();
    CHECK(setup() == 0);
    xiaotai_view_t page = home();
    ui_view_accept(&page);
    step(0);
    lv_obj_t *image = kit->layers[XIAOTAI_LAYER_PAGE].face;
    CHECK(image && !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    const lv_image_dsc_t *source = lv_image_get_src(image);
    CHECK(source->header.cf == LV_COLOR_FORMAT_RGB565 && source->header.w == face_w && source->header.h == face_h);
    CHECK(lv_image_get_scale(image) == LV_SCALE_NONE);
    lv_area_t area;
    lv_obj_get_coords(image, &area);
    CHECK(area.x1 == face_x && area.y1 == face_y && lv_area_get_width(&area) == face_w && lv_area_get_height(&area) == face_h);
    /* The stock LCD480 paints the original centre colour behind the face, over the gradient. */
    const uint32_t background = 0x181c21;
    CHECK(panel[0] == rgb565(0x101418));
    CHECK(panel[(size_t)area.y1 * width + area.x1] == rgb565(background));
    CHECK(clean_attach(background, 0) == 0);
    CHECK(counters().direct > 0 && counters().fallback == 0);
    CHECK(as_refreshed() == 0);
    CHECK(counters().fallback == 1);
    for (uint32_t t = 20; t <= 600; t += 20) step(t);
    CHECK(as_refreshed() == 0);
    CHECK(preview_geometry(source) == 0);
    teardown();
    return 0;
}

static int owner(void)
{
    face_geometry();
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
    CHECK(c.x <= FACE_X + 8 && !strcmp(c.intent, "test.face"));
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

/* The actor is all there is of the expression: nothing as large as a face stands beside it or over it. */
static int stands_alone(const lv_obj_t *widget)
{
    const lv_obj_t *root = lv_obj_get_parent(widget);
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        const lv_obj_t *child = lv_obj_get_child(root, (int32_t)i);
        lv_area_t area;
        lv_obj_get_coords(child, &area);
        if (child == widget || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) || lv_area_get_width(&area) < 200 || lv_area_get_height(&area) < 100) continue;
        CHECK(lv_obj_get_style_bg_opa(child, LV_PART_MAIN) == LV_OPA_TRANSP && lv_obj_get_child_count(child) == 0);
    }
    return 0;
}

/* The actor is on the page of the expression and on no other: a page that is about something else neither
 * builds it nor has it move, nor shows a face that nobody draws. */
static int acting(void)
{
    manual = with_actor = true;
    CHECK(setup() == 0);
    uint32_t at = 100;
    static const xiaotai_view_action_t hang_up = {.role = XIAOTAI_ROLE_BACK, .intent = "test.face", .icon = XIAOTAI_ICON_HANGUP,
                                                  .label = "end"};
    const xiaotai_view_t pages[] = {
        {.kind = XIAOTAI_VIEW_CALL, .id = "ringing", .title = "call", .subtitle = "peer", .text = "calling",
         .emotion = XIAOTAI_EMOTION_RINGING, .actions = &hang_up, .action_count = 1},
        {.kind = XIAOTAI_VIEW_CALL, .id = "talking", .title = "call", .subtitle = "peer", .text = "on the line",
         .emotion = XIAOTAI_EMOTION_NEUTRAL, .phase = XIAOTAI_TALK_SPEAKING, .actions = &hang_up, .action_count = 1},
        {.kind = XIAOTAI_VIEW_LIST, .id = "list", .title = "list"},
        {.kind = XIAOTAI_VIEW_NOTICE, .id = "notice", .title = "notice", .text = "text"},
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        xiaotai_view_t page = home();
        page.emotion = "happy";
        CHECK(xiaotai_run(show, &page) == XIAOTAI_OK && xiaotai_signal_take(&shown, WAIT_MS));
        const unsigned seen = acted.shown;
        for (int n = 0; n < 5; ++n) step(at += 24);
        CHECK(acted.builds == i + 1 && acted.detaches == i && acted.shown >= seen + 5 && acted.sets > 0);
        CHECK(kit->layers[XIAOTAI_LAYER_PAGE].face == acted.widget && stands_alone(acted.widget) == 0);

        page = pages[i];
        CHECK(xiaotai_run(show, &page) == XIAOTAI_OK && xiaotai_signal_take(&shown, WAIT_MS));
        step(at += 24);
        CHECK(acted.builds == i + 1 && acted.detaches == i + 1 && !kit->layers[XIAOTAI_LAYER_PAGE].face);
        const unsigned still = acted.shown, sets = acted.sets;
        for (int n = 0; n < 20; ++n) step(at += 24);
        CHECK(acted.shown == still && acted.sets == sets);
    }
    teardown();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "owner")) return owner();
    if (argc > 1 && !strcmp(argv[1], "acting")) return acting();
    if (argc > 1 && !strcmp(argv[1], "scaled")) return scaled(480, 320);
    if (argc > 1 && !strcmp(argv[1], "wide")) return scaled(WIDE_W, WIDE_H);
    return deterministic();
}
