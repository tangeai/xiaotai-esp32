// SPDX-License-Identifier: MIT
#include "ui_test.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/theme_lcd.h"
#include "xiaotai_mem_host.h"
#include "lvgl.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

enum { WIDTH = 320, HEIGHT = 240, WAIT = 2000, MAX_ROWS = 40 };

typedef struct {
    const char *text;
    int set_scroll;
    int scroll, height;
    uint32_t memory, blocks;
    int64_t port_allocations;
    unsigned pages;
    uintptr_t widget;
    bool shaped, qr_on, spinner, scrolling, in_scroller, visible;
} probe_t;

static xiaotai_signal_t owner_done, tapped;
static _Atomic(probe_t *) requested;
static lv_obj_t *page;
static unsigned page_sequence;
static atomic_uint taps;
static atomic_int value;
static const uint8_t red[] = {0, 0xf8, 0, 0xf8, 0, 0xf8, 0, 0xf8};
static const xiaotai_ui_image_t asset = {.name = "registered-test-code", .width = 2, .height = 2,
    .format = XIAOTAI_IMAGE_RGB565, .size = sizeof(red), .data = red};
/* A checkerboard of 8 x 8 modules, each two pixels wide: what a code to scan looks like. */
static const uint8_t modules[] = {0xcc, 0xcc, 0xcc, 0xcc, 0x33, 0x33, 0x33, 0x33, 0xcc, 0xcc, 0xcc, 0xcc, 0x33, 0x33, 0x33, 0x33,
                                  0xcc, 0xcc, 0xcc, 0xcc, 0x33, 0x33, 0x33, 0x33, 0xcc, 0xcc, 0xcc, 0xcc, 0x33, 0x33, 0x33, 0x33};
static const xiaotai_ui_image_t mono = {.name = "registered-mono-code", .width = 16, .height = 16,
    .format = XIAOTAI_IMAGE_MONO1, .size = sizeof(modules), .data = modules};

LV_FONT_DECLARE(lcd_font_num_48);

static lv_obj_t *find(lv_obj_t *obj, const char *text, bool scroll)
{
    if (scroll && lv_obj_check_type(obj, &lv_obj_class) && lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) return obj;
    if (text && lv_obj_has_class(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *found = find(lv_obj_get_child(obj, (int32_t)i), text, scroll);
        if (found) return found;
    }
    return NULL;
}

static bool shown_class(lv_obj_t *obj, const lv_obj_class_t *type)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_has_class(obj, type)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (shown_class(lv_obj_get_child(obj, (int32_t)i), type)) return true;
    return false;
}

static void page_built(void *unused, const xiaotai_ui_host_t *host, void *background, const xiaotai_ui_layer_t *layer)
{
    (void)unused;
    (void)host;
    (void)layer;
    assert(!xiaotai_on_owner());
    page = lv_obj_get_parent(background);
    ++page_sequence;
}

static size_t controls(void *state, xiaotai_ui_control_t *out, size_t capacity)
{
    /* Run introspection under the display owner's existing LVGL lock. The
     * render-time count-only callback is not an interrogation request. */
    probe_t *p = out ? atomic_exchange(&requested, NULL) : NULL;
    if (p) {
        assert(!xiaotai_on_owner() && page);
        lv_obj_t *scroller = find(page, NULL, true);
        if (scroller && p->set_scroll >= 0) lv_obj_scroll_to_y(scroller, p->set_scroll, LV_ANIM_OFF);
        p->scroll = scroller ? lv_obj_get_scroll_y(scroller) : -1;
        p->scrolling = scroller && lv_obj_is_scrolling(scroller);
        p->pages = page_sequence;
        lv_obj_t *label = find(page, p->text, false);
        p->widget = (uintptr_t)label;
        if (label) {
            p->height = lv_obj_get_height(label);
            p->in_scroller = scroller && lv_obj_get_parent(label) == scroller;
            p->visible = lv_obj_is_visible(label);
        }
        p->shaped = label && lv_obj_get_style_text_font(label, 0) == &lcd_font_num_48;
        p->spinner = shown_class(page, &lv_arc_class);
#if LV_USE_QRCODE
        p->qr_on = shown_class(page, &lv_qrcode_class);
#endif
        lv_mem_monitor_t stats;
        lv_mem_monitor(&stats);
        p->memory = (uint32_t)stats.free_size;
        p->blocks = (uint32_t)stats.used_cnt;
        p->port_allocations = xiaotai_mem_host_outstanding();
    }
    return xiaotai_theme_lcd_controls(state, out, capacity);
}

static int score(const xiaotai_display_desc_t *d) { return d->pixel == XIAOTAI_PIXEL_RGB565 ? 100 : 0; }

static xiaotai_err_t start(const xiaotai_ui_host_t *host, void **state)
{
    static const xiaotai_theme_lcd_variant_t v = {.page_built = page_built};
    return xiaotai_theme_lcd_start(host, &v, state);
}

static void pressed(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner());
    atomic_store(&value, args->value);
    atomic_fetch_add(&taps, 1U);
    xiaotai_signal_give(&tapped);
}

static xiaotai_err_t app_init(void)
{
    static const xiaotai_theme_t t = {.name = "pages-fixture", .backend = XIAOTAI_UI_BACKEND_LVGL,
        .match = score, .start = start, .render = xiaotai_theme_lcd_render, .tick = xiaotai_theme_lcd_tick,
        .sleep = xiaotai_theme_lcd_sleep, .stop = xiaotai_theme_lcd_stop, .controls = controls,
        .stack_bytes = XIAOTAI_THEME_LCD_STACK_BYTES};
    xiaotai_err_t err = xiaotai_ui_theme_register(&t);
    if (err == XIAOTAI_OK) err = xiaotai_ui_image_register(&asset);
    if (err == XIAOTAI_OK) err = xiaotai_ui_image_register(&mono);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle("fixture.op", pressed, NULL);
    return err;
}

static void owner_show(void *view)
{
    xiaotai_view_show(view);
    xiaotai_signal_give(&owner_done);
}

static void show(xiaotai_view_t *view)
{
    xiaotai_ui_info_t info;
    assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK);
    const uint32_t before = info.frames;
    assert(xiaotai_run(owner_show, view) == XIAOTAI_OK && xiaotai_signal_take(&owner_done, WAIT));
    for (unsigned retry = 0; retry < WAIT; ++retry) {
        assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK);
        if (info.frames > before) return;
        xiaotai_delay_ms(1);
    }
    assert(!"new view not rendered");
}

static size_t read_controls(xiaotai_ui_control_t *out, size_t cap, probe_t *p)
{
    size_t count;
    if (p) atomic_store(&requested, p);
    assert(xiaotai_ui_controls(0, out, cap, &count, WAIT) == XIAOTAI_OK);
    assert(!atomic_load(&requested));
    return count;
}

static probe_t probe(const char *text, int scroll)
{
    probe_t p = {.text = text, .set_scroll = scroll};
    xiaotai_ui_control_t first;
    (void)read_controls(&first, 1, &p);
    return p;
}

static xiaotai_ui_control_t control(unsigned element, unsigned index)
{
    xiaotai_ui_control_t rows[MAX_ROWS] = {0};
    const size_t n = read_controls(rows, MAX_ROWS, NULL);
    assert(n <= MAX_ROWS);
    for (size_t i = 0; i < n; ++i) if (rows[i].element == element && rows[i].index == index) return rows[i];
    return (xiaotai_ui_control_t){0};
}

static uint16_t pixel(const xiaotai_ui_capture_t *frame, unsigned x, unsigned y)
{
    const uint8_t *p = &frame->pixels[(y * WIDTH + x) * 2U];
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8U));
}

static xiaotai_ui_capture_t capture(void)
{
    xiaotai_ui_capture_t out;
    assert(xiaotai_ui_capture(0, &out, WAIT) == XIAOTAI_OK);
    assert(out.width == WIDTH && out.height == HEIGHT && out.pixel == XIAOTAI_PIXEL_RGB565);
    return out;
}

static void tap(xiaotai_ui_control_t c)
{
    assert(c.width && c.height && xiaotai_ui_tap(0, c.x + c.width / 2, c.y + c.height / 2) == XIAOTAI_OK);
}

static int list_case(void)
{
    xiaotai_view_item_t items[12] = {{.label = "Group"},
        {.label = "First", .icon = XIAOTAI_ICON_PHONE, .value = 21, .intent = "fixture.op"},
        {.label = "Selected", .intent = "fixture.op", .state = XIAOTAI_STATE_CHECKED},
        {.label = "Busy", .intent = "fixture.op", .state = XIAOTAI_STATE_BUSY}};
    for (size_t i = 4; i < 12; ++i) items[i] = (xiaotai_view_item_t){.label = "Other", .intent = "fixture.op"};
    const xiaotai_view_action_t acts[] = {{.role = XIAOTAI_ROLE_BACK, .intent = "fixture.op"},
        {.icon = XIAOTAI_ICON_REFRESH, .intent = "fixture.op"}};
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_LIST, .id = "list", .title = "Contacts", .flags = XIAOTAI_VIEW_GRID,
        .items = items, .item_count = 12, .actions = acts, .action_count = 2};
    show(&v);
    const xiaotai_ui_control_t one = control(XIAOTAI_UI_ELEMENT_ITEM, 1), two = control(XIAOTAI_UI_ELEMENT_ITEM, 2);
    const xiaotai_ui_control_t three = control(XIAOTAI_UI_ELEMENT_ITEM, 3), four = control(XIAOTAI_UI_ELEMENT_ITEM, 4);
    CHECK(one.width && one.x < two.x && two.x < three.x && one.y == three.y && four.y > three.y);
    CHECK(two.state == XIAOTAI_STATE_CHECKED && three.state == XIAOTAI_STATE_BUSY);
    CHECK(!control(XIAOTAI_UI_ELEMENT_ITEM, 0).width);
    CHECK(control(XIAOTAI_UI_ELEMENT_ACTION, 0).y < 30 && control(XIAOTAI_UI_ELEMENT_ACTION, 1).y < 30);
    tap(one);
    CHECK(xiaotai_signal_take(&tapped, WAIT) && atomic_load(&value) == 21);
    tap(three);
    CHECK(!xiaotai_signal_take(&tapped, 155));

    v.flags = 0;
    show(&v);
    CHECK(xiaotai_ui_swipe(0, 250, 177, 250, 65, 180) == XIAOTAI_OK);
    bool seen = false, settled = false;
    for (unsigned i = 0; !settled && i < WAIT / 10; ++i) {
        const probe_t current = probe(NULL, -1);
        seen |= current.scroll > 24;
        settled = seen && !current.scrolling;
        xiaotai_delay_ms(10);
    }
    CHECK(settled);
    const probe_t before = probe("First", 132);
    CHECK(before.scroll == 132 && before.widget);
    items[1].detail = "Changed details";
    show(&v);
    const probe_t after = probe("First", -1);
    if (after.scroll != before.scroll || after.pages != before.pages || after.widget != before.widget)
        fprintf(stderr, "LIST update: y=%d/%d; builds=%u/%u; widget same=%d\n", before.scroll, after.scroll,
                before.pages, after.pages, before.widget == after.widget);
    CHECK(after.scroll == before.scroll && after.pages == before.pages && after.widget == before.widget);
    return 0;
}

/* Every module of the checkerboard, in a square of `cell` pixels a module. */
static bool checkerboard(const xiaotai_ui_capture_t *frame, unsigned left, unsigned top, unsigned cell)
{
    for (unsigned y = 0; y < 8 * cell; ++y) for (unsigned x = 0; x < 8 * cell; ++x)
        if (pixel(frame, left + x, top + y) != ((x / cell + y / cell) % 2 ? 0xffff : 0)) return false;
    return true;
}

static int code_case(void)
{
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_CODE, .id = "code", .title = "Bind", .text = "235689", .subtitle = "App code",
        .timer_mode = XIAOTAI_TIMER_REMAINING, .timer_s = 62};
    show(&v);
    const probe_t baseline = probe("235689", -1);
    CHECK(baseline.widget && baseline.shaped && probe("01:02", -1).widget);

    v.flags = XIAOTAI_VIEW_QR;
    v.text = "http://example.test/local/fixture";
    show(&v);
    CHECK(probe(NULL, -1).qr_on);
    xiaotai_ui_capture_t frame = capture();
    unsigned black = 0, white = 0;
    uint32_t first = 2166136261U;
    for (unsigned y = 50; y < 145; ++y) for (unsigned x = 110; x < 210; ++x) {
        const uint16_t rgb = pixel(&frame, x, y);
        black += rgb == 0;
        white += rgb == 0xffff;
        first = (first ^ rgb) * 16777619U;
    }
    CHECK(black > 100 && white > 100);
    xiaotai_ui_capture_free(&frame);

    v.image = asset.name;
    v.text = "http://example.test/local/updated";
    show(&v);
    frame = capture();
    CHECK(pixel(&frame, 160, 90) == 0xf800 && !probe(NULL, -1).qr_on);
    xiaotai_ui_capture_free(&frame);

    v.image = "not-registered";
    show(&v);
    CHECK(probe(NULL, -1).qr_on);
    frame = capture();
    uint32_t second = 2166136261U;
    for (unsigned y = 50; y < 145; ++y) for (unsigned x = 110; x < 210; ++x)
        second = (second ^ pixel(&frame, x, y)) * 16777619U;
    CHECK(second != first);
    xiaotai_ui_capture_free(&frame);

    v.flags = 0;
    v.text = "975431";
    show(&v);
    const probe_t after = probe("975431", -1);
    CHECK(after.pages == baseline.pages && after.widget == baseline.widget && after.shaped);
    bool counted = false;
    for (unsigned i = 0; !counted && i < WAIT / 10; ++i) {
        counted = probe("01:01", -1).widget != 0;
        xiaotai_delay_ms(10);
    }
    CHECK(counted && probe("975431", -1).widget == baseline.widget);

    /* A one-bit picture is drawn dark on white, on a page and in a dialog,
     * and its pixels go with it. */
    v.timer_mode = XIAOTAI_TIMER_NONE;
    show(&v);
    const int64_t plain = probe("975431", -1).port_allocations;
    v.image = mono.name;
    show(&v);
    CHECK(!probe("975431", -1).visible);
    frame = capture();
    CHECK(checkerboard(&frame, 96, 44, 16));
    xiaotai_ui_capture_free(&frame);
    v.image = NULL;
    show(&v);
    CHECK(probe("975431", -1).visible && probe(NULL, -1).port_allocations == plain);
    static const xiaotai_view_action_t back = {.role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK, .intent = "fixture.op"};
    xiaotai_view_t dialog = {.kind = XIAOTAI_VIEW_CODE, .layer = XIAOTAI_LAYER_OVERLAY, .id = "scan", .title = "Scan",
        .image = mono.name, .actions = &back, .action_count = 1};
    /* The second round ends with what the first one left: the layer stays, the pixels do not. */
    int64_t left = 0;
    for (int round = 0; round < 2; ++round) {
        dialog.flags = 0;
        show(&dialog);
        (void)probe(NULL, -1);
        frame = capture();
        CHECK(checkerboard(&frame, 73, 49, 22));   /* inside the panel's one-pixel border */
        xiaotai_ui_capture_free(&frame);
        dialog.flags = XIAOTAI_VIEW_DISMISS;
        show(&dialog);
        const int64_t now = probe("975431", -1).port_allocations;
        CHECK(probe("975431", -1).visible && (!round || now == left));
        left = now;
    }
    return 0;
}

static int call_case(void)
{
    xiaotai_view_action_t a[] = {{.label = "Answer", .intent = "fixture.op", .value = 9, .style = XIAOTAI_STYLE_POSITIVE},
        {.label = "Reject", .intent = "fixture.op", .style = XIAOTAI_STYLE_DESTRUCTIVE}};
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_CALL, .id = "call", .title = "Call", .subtitle = "Peer", .text = "Ringing",
        .timer_mode = XIAOTAI_TIMER_ELAPSED, .timer_s = 7, .actions = a, .action_count = 2};
    show(&v);
    const probe_t before = probe("Peer", -1);
    CHECK(before.widget && probe("00:07", -1).widget);
    xiaotai_ui_control_t one = control(XIAOTAI_UI_ELEMENT_ACTION, 0), two = control(XIAOTAI_UI_ELEMENT_ACTION, 1);
    CHECK(one.width >= 90 && two.width == one.width && two.x > one.x && two.y == one.y);
    xiaotai_ui_capture_t frame = capture();
    CHECK(pixel(&frame, one.x + 10, one.y + 5) == lv_color_to_u16(lv_color_hex(0x1fa672)));   /* LCD_POSITIVE */
    CHECK(pixel(&frame, two.x + 10, two.y + 5) == lv_color_to_u16(lv_color_hex(0xdc434d)));   /* LCD_DESTRUCTIVE */
    xiaotai_ui_capture_free(&frame);
    tap(one);
    CHECK(xiaotai_signal_take(&tapped, WAIT) && atomic_load(&value) == 9);

    a[0].state = XIAOTAI_STATE_BUSY;
    a[0].label = "Cancel";
    v.text = "Connecting";
    v.flags = XIAOTAI_VIEW_VIDEO;
    v.action_count = 1;
    show(&v);
    one = control(XIAOTAI_UI_ELEMENT_ACTION, 0);
    CHECK(one.width >= 90 && one.state == XIAOTAI_STATE_BUSY && !control(XIAOTAI_UI_ELEMENT_ACTION, 1).width);
    tap(one);
    CHECK(!xiaotai_signal_take(&tapped, 155) && atomic_load(&taps) == 1);
    const probe_t after = probe("Peer", -1);
    CHECK(after.pages == before.pages && after.widget == before.widget);
    return 0;
}

static int notice_case(void)
{
#define PARA "A detailed line of the recognition result that wraps on the device.\n"
    const char *text = PARA PARA PARA PARA PARA PARA PARA PARA PARA PARA;
    const char *updated = PARA PARA PARA PARA PARA PARA PARA PARA PARA "Accepted result";
#undef PARA
    const char *intro = "Speech note\nReview suggested";
    xiaotai_view_action_t tabs[] = {{.label = "Text", .intent = "fixture.op", .state = XIAOTAI_STATE_CHECKED},
        {.label = "Details", .intent = "fixture.op"}};
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_NOTICE, .id = "notice", .title = "Result", .text = text,
        .subtitle = intro, .status = "Updating",
        .actions = tabs, .action_count = 2};
    show(&v);
    const probe_t heading = probe(intro, -1);
    CHECK(heading.widget && heading.height >= 36 && heading.in_scroller && heading.visible);
    const probe_t before = probe(text, 95);
    CHECK(before.widget && before.scroll == 95 && control(XIAOTAI_UI_ELEMENT_ACTION, 0).y < 80);
    const probe_t status = probe("Updating", -1);
    CHECK(status.widget && status.visible && !status.in_scroller && !probe(intro, -1).visible);
    v.text = updated;
    v.status = "Updated";
    show(&v);
    const probe_t after = probe(updated, -1);
    CHECK(after.widget == before.widget && after.scroll == before.scroll && after.pages == before.pages);
    const probe_t current_status = probe("Updated", -1);
    CHECK(current_status.widget == status.widget && current_status.visible && !current_status.in_scroller);
    tabs[0].state = 0;
    tabs[1].state = XIAOTAI_STATE_CHECKED;
    show(&v);
    CHECK(probe(updated, -1).scroll == 0);

    /* Match the next steady state, rather than comparing it to a page that
     * was built with different text and then edited/retabbed in place. */
    v.id = "next-notice";
    show(&v);
    v.id = "notice";
    show(&v);
    const probe_t warm = probe(updated, -1);
    for (unsigned i = 0; i < 100; ++i) {
        v.id = i % 2 ? "notice" : "next-notice";
        show(&v);
    }
    const probe_t rebuilt = probe(updated, -1);
    if (rebuilt.memory != warm.memory || rebuilt.pages != warm.pages + 100)
        fprintf(stderr, "NOTICE lifecycle: builds=%u/%u; free=%u/%u; blocks=%u/%u\n", warm.pages, rebuilt.pages,
                warm.memory, rebuilt.memory, warm.blocks, rebuilt.blocks);
    CHECK(rebuilt.pages == warm.pages + 100 && rebuilt.memory == warm.memory && rebuilt.blocks == warm.blocks &&
          rebuilt.port_allocations == warm.port_allocations);

    v = (xiaotai_view_t){.kind = XIAOTAI_VIEW_PROGRESS, .id = "progress", .title = "Update", .text = "Working",
        .flags = XIAOTAI_VIEW_PERCENT, .progress = 60};
    show(&v);
    CHECK(probe("60%", -1).widget);
    v.flags = 0;
    show(&v);
    CHECK(probe("Working", -1).spinner && !probe("60%", -1).widget);   /* busy: the ring turns */
    return 0;
}

int main(void)
{
    xiaotai_display_desc_t d = ui_fake_desc(WIDTH, HEIGHT, XIAOTAI_PIXEL_RGB565);
    d.touch = true;
    d.transfer_pixels = WIDTH * 16;
    ui_fake_board_set_displays(&d, 1);
    CHECK(xiaotai_hal_init() == XIAOTAI_OK && xiaotai_signal_init(&owner_done) == XIAOTAI_OK &&
          xiaotai_signal_init(&tapped) == XIAOTAI_OK);
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "fixture", .init = app_init};
    static const xiaotai_module_t *const modules[] = {&ui};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 1, .applications = apps, .application_count = 1};
    CHECK(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    CHECK(read_controls(NULL, 0, NULL) == 0);
    const int result = !strcmp(LCD_PAGES_CASE, "list") ? list_case() : !strcmp(LCD_PAGES_CASE, "code") ? code_case() :
        !strcmp(LCD_PAGES_CASE, "call") ? call_case() : notice_case();
    xiaotai_ui_reset();
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&tapped);
    xiaotai_signal_deinit(&owner_done);
    return result;
}
