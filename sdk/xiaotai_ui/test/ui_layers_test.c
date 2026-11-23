// SPDX-License-Identifier: MIT
/* View slots and layers (src/ui_core.c) at exact times: a new page identity
 * removes the overlay and keeps the toast, which runs out its own duration;
 * a new overlay identity removes the toast; dismissal by id and without;
 * toast and timed-dialog durations (default 2400 ms, the product's own, a
 * new toast restarting it, an updated dialog keeping its deadline, the
 * millisecond counter wrapping); update against new view and the change bits
 * the theme receives when several views coalesce before a take; views kept
 * between init and start, dropped and counted for a display without a theme;
 * buffers from the memory port (the overlay's on first use, all of them
 * released by reset and on every failed init); and the framework's view
 * sink delivering an application's views to the slots. */
#include <string.h>
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/framework.h"
#include "xiaotai_mem_host.h"

static uint32_t s_now;
static uint32_t test_clock(void) { return s_now; }

static xiaotai_ui_scene_t s_scene;
static ui_take_t s_take[UI_LAYER_COUNT];

static const ui_fake_theme_config_t MONO = {.name = "mono", .backend = XIAOTAI_UI_BACKEND_FRAME, .width = 128,
                                            .height = 64, .pixel = XIAOTAI_PIXEL_MONO1_VLSB, .exact = 100};

static void boards_one_display(void)
{
    const xiaotai_display_desc_t desc = ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    ui_fake_board_set_displays(&desc, 1);
}

static int setup(const xiaotai_ui_config_t *config, bool start)
{
    xiaotai_ui_reset();
    ui_core_set_clock(test_clock);
    if (config) CHECK(xiaotai_ui_configure(config) == XIAOTAI_OK);
    CHECK(xiaotai_engine_ui_init() == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO)) == XIAOTAI_OK);
    if (start) xiaotai_engine_ui_start();
    return 0;
}

static bool take(void)
{
    memset(&s_scene, 0, sizeof(s_scene));
    return ui_layers_take(ui_display_at(0), s_now, &s_scene, s_take);
}

static void show(xiaotai_view_layer_t layer, xiaotai_view_kind_t kind, const char *id, const char *text,
                 uint32_t duration_ms)
{
    xiaotai_view_t view;
    memset(&view, 0, sizeof(view));
    view.kind = kind;
    view.layer = (uint8_t)layer;
    view.id = id;
    view.text = text;
    view.duration_ms = duration_ms;
    ui_view_accept(&view);
}

static void page(const char *id, const char *text) { show(XIAOTAI_LAYER_PAGE, XIAOTAI_VIEW_LIST, id, text, 0); }
static void overlay(const char *id, const char *text, uint32_t duration_ms)
{
    show(XIAOTAI_LAYER_OVERLAY, XIAOTAI_VIEW_NOTICE, id, text, duration_ms);
}
/* What xiaotai_view_toast() builds. */
static void toast(const char *text, uint32_t duration_ms)
{
    show(XIAOTAI_LAYER_TOAST, XIAOTAI_VIEW_NOTICE, NULL, text, duration_ms);
}
/* What xiaotai_view_dismiss() builds. */
static void dismiss(xiaotai_view_layer_t layer, const char *id)
{
    xiaotai_view_t view;
    memset(&view, 0, sizeof(view));
    view.kind = XIAOTAI_VIEW_NOTICE;
    view.layer = (uint8_t)layer;
    view.id = id;
    view.flags = XIAOTAI_VIEW_DISMISS;
    ui_view_accept(&view);
}

static bool text_is(const xiaotai_ui_layer_t *layer, const char *text)
{
    return layer->present && layer->view.text && strcmp(layer->view.text, text) == 0;
}
static bool id_is(const xiaotai_ui_layer_t *layer, const char *id)
{
    return layer->present && layer->view.id && strcmp(layer->view.id, id) == 0;
}

static xiaotai_ui_info_t info(uint8_t display)
{
    xiaotai_ui_info_t out;
    memset(&out, 0, sizeof(out));
    (void)xiaotai_ui_inspect(display, &out);
    return out;
}

/* ------------------------------------------------------------------ tests */

static int test_page_change_keeps_toast(void)
{
    CHECK(setup(NULL, true) == 0);
    s_now = 1000;
    page("home", "idle");
    overlay("confirm", "leave?", 0);
    toast("saved", 0);
    CHECK(take());
    CHECK(id_is(&s_scene.page, "home") && s_scene.page.changed == UI_CHANGED_ALL);
    CHECK(id_is(&s_scene.overlay, "confirm") && text_is(&s_scene.toast, "saved"));
    CHECK(s_take[XIAOTAI_LAYER_PAGE].shown && s_take[XIAOTAI_LAYER_TOAST].shown);

    /* Submit, toast, then a page change driven by state: the notice stays. */
    s_now = 1100;
    page("contacts", NULL);
    CHECK(take());
    CHECK(id_is(&s_scene.page, "contacts") && (s_scene.page.changed & XIAOTAI_CHANGED_IDENTITY));
    CHECK(!s_scene.overlay.present && s_scene.overlay.changed == UI_CHANGED_ALL);
    CHECK(text_is(&s_scene.toast, "saved") && s_scene.toast.changed == 0U && !s_take[XIAOTAI_LAYER_TOAST].shown);

    /* It runs out 2400 ms after it was shown, not after the page change. */
    s_now = 3399;
    CHECK(!take());
    CHECK(text_is(&s_scene.toast, "saved"));
    s_now = 3400;
    CHECK(take());
    CHECK(!s_scene.toast.present && s_scene.toast.changed == UI_CHANGED_ALL);
    CHECK(id_is(&s_scene.page, "contacts") && s_scene.page.changed == 0U);
    s_now = 3401;
    CHECK(!take());
    CHECK(!s_scene.toast.present && s_scene.toast.changed == 0U);

    xiaotai_ui_info_t i = info(0);
    CHECK(i.shown == 4U && i.updated == 0U && i.dropped == 0U && strcmp(i.page, "contacts") == 0);
    CHECK(i.page_kind == XIAOTAI_VIEW_LIST && i.overlay[0] == '\0');
    return 0;
}

static int test_overlay_removes_toast(void)
{
    CHECK(setup(NULL, true) == 0);
    s_now = 0;
    page("home", NULL);
    toast("hello", 0);
    overlay("menu", "a", 0);
    CHECK(take());
    CHECK(id_is(&s_scene.overlay, "menu") && !s_scene.toast.present);

    /* A toast shown above the dialog stays above it. */
    toast("again", 0);
    CHECK(take());
    CHECK(text_is(&s_scene.toast, "again"));

    /* Updating the dialog is not a new dialog: the toast stays. */
    overlay("menu", "b", 0);
    CHECK(take());
    CHECK(text_is(&s_scene.overlay, "b") && s_scene.overlay.changed == XIAOTAI_CHANGED_TEXT);
    CHECK(text_is(&s_scene.toast, "again") && s_scene.toast.changed == 0U);

    /* Another dialog is. */
    overlay("volume", NULL, 0);
    CHECK(take());
    CHECK(id_is(&s_scene.overlay, "volume") && !s_scene.toast.present);
    CHECK(strcmp(info(0).overlay, "volume") == 0);
    return 0;
}

static int test_updates_and_coalescing(void)
{
    CHECK(setup(NULL, true) == 0);
    page("call", "dialling");
    CHECK(take() && s_scene.page.changed == UI_CHANGED_ALL);

    /* The same identity again is an update with only what differs. */
    page("call", "talking");
    CHECK(take());
    CHECK(s_scene.page.changed == XIAOTAI_CHANGED_TEXT && text_is(&s_scene.page, "talking"));
    CHECK(info(0).shown == 1U && info(0).updated == 1U);

    /* Equal content: no bit, but the view did arrive (a focus request of an
     * unchanged view still applies). */
    page("call", "talking");
    CHECK(!take());
    CHECK(s_scene.page.changed == 0U && s_take[XIAOTAI_LAYER_PAGE].shown && s_scene.page.present);
    CHECK(!take());
    CHECK(!s_take[XIAOTAI_LAYER_PAGE].shown && s_scene.page.present);

    /* A, then B, then A before the theme drew: the identity change is kept. */
    page("menu", NULL);
    page("call", "talking");
    CHECK(take());
    CHECK(s_scene.page.changed == UI_CHANGED_ALL && id_is(&s_scene.page, "call"));
    CHECK(info(0).shown == 3U);

    /* Content that changed and changed back before the take: nothing to redraw. */
    page("call", "held");
    page("call", "talking");
    CHECK(!take());
    CHECK(s_scene.page.changed == 0U && s_take[XIAOTAI_LAYER_PAGE].shown);
    page("call", "held");
    page("call", "ended");
    CHECK(take() && s_scene.page.changed == XIAOTAI_CHANGED_TEXT && text_is(&s_scene.page, "ended"));

    /* The caller's memory is free again as soon as show returned. */
    char buffer[16];
    strcpy(buffer, "first");
    page("call", buffer);
    strcpy(buffer, "second");
    CHECK(take() && text_is(&s_scene.page, "first"));
    return 0;
}

static int test_dismiss(void)
{
    CHECK(setup(NULL, true) == 0);
    page("p", NULL);
    overlay("dlg", NULL, 0);
    toast("t", 0);
    CHECK(take());
    const uint32_t shown = info(0).shown;

    /* With an id only that view goes. */
    dismiss(XIAOTAI_LAYER_OVERLAY, "other");
    CHECK(!take() && id_is(&s_scene.overlay, "dlg"));
    dismiss(XIAOTAI_LAYER_OVERLAY, "dlg");
    CHECK(take() && !s_scene.overlay.present && s_scene.overlay.changed == UI_CHANGED_ALL);
    CHECK(text_is(&s_scene.toast, "t") && id_is(&s_scene.page, "p"));
    dismiss(XIAOTAI_LAYER_TOAST, NULL);
    CHECK(take() && !s_scene.toast.present);
    dismiss(XIAOTAI_LAYER_OVERLAY, NULL);   /* nothing there */
    CHECK(!take());
    CHECK(info(0).shown == shown && info(0).updated == 0U);

    /* Without its page the dialog goes too; the display is blank. */
    overlay("dlg2", NULL, 0);
    CHECK(take() && id_is(&s_scene.overlay, "dlg2"));
    dismiss(XIAOTAI_LAYER_PAGE, NULL);
    CHECK(take() && !s_scene.page.present && !s_scene.overlay.present);
    CHECK(info(0).page[0] == '\0');

    /* A view dismissed before it was drawn never reaches the theme. */
    page("q", NULL);
    dismiss(XIAOTAI_LAYER_PAGE, "q");
    CHECK(!take() && !s_scene.page.present);
    return 0;
}

static int test_durations(void)
{
    CHECK(setup(NULL, true) == 0);
    uint32_t due = 0;
    s_now = 1000;
    page("home", NULL);
    /* The product's own duration (a 1500 ms "not done" banner). */
    toast("not done", 1500);
    CHECK(ui_layers_next_due(ui_display_at(0), &due) && due == 2500U);
    CHECK(take() && text_is(&s_scene.toast, "not done"));
    s_now = 2499;
    CHECK(!take() && s_scene.toast.present);
    s_now = 2500;
    CHECK(take() && !s_scene.toast.present);
    CHECK(!ui_layers_next_due(ui_display_at(0), &due));

    /* A new toast replaces the old one and restarts the default duration. */
    s_now = 3000;
    toast("one", 0);
    CHECK(take());
    s_now = 4000;
    toast("two", 0);
    CHECK(take() && text_is(&s_scene.toast, "two") && s_scene.toast.changed == XIAOTAI_CHANGED_TEXT);
    s_now = 6399;
    CHECK(!take() && text_is(&s_scene.toast, "two"));
    s_now = 6400;
    CHECK(take() && !s_scene.toast.present);

    /* A timed dialog: an update with the same duration keeps the deadline. */
    s_now = 10000;
    overlay("code", "123", 3000);
    CHECK(take());
    s_now = 11000;
    overlay("code", "456", 3000);
    CHECK(take() && text_is(&s_scene.overlay, "456"));
    s_now = 12999;
    CHECK(!take() && s_scene.overlay.present);
    s_now = 13000;
    CHECK(take() && !s_scene.overlay.present);

    /* A changed duration runs from the update. */
    s_now = 20000;
    overlay("code", "a", 3000);
    s_now = 21000;
    overlay("code", "b", 5000);
    CHECK(take());
    s_now = 25999;
    CHECK(!take() && s_scene.overlay.present);
    s_now = 26000;
    CHECK(take() && !s_scene.overlay.present);

    /* The earliest of a toast and a dialog is what the UI task waits for. */
    s_now = 30000;
    overlay("code", "c", 9000);
    toast("x", 0);
    CHECK(ui_layers_next_due(ui_display_at(0), &due) && due == 32400U);

    /* A dialog without a duration stays. */
    overlay("confirm", NULL, 0);
    CHECK(take());
    s_now = 30000U + 100000000U;
    CHECK(!take() && id_is(&s_scene.overlay, "confirm"));

    /* Deadlines across the wrap of the millisecond counter. */
    s_now = 0xFFFFFC00U;
    toast("wrap", 0);
    CHECK(take());
    s_now = 0xFFFFFC00U + 2399U;   /* 0x0000055F after the wrap */
    CHECK(!take() && s_scene.toast.present);
    s_now = 0xFFFFFC00U + 2400U;
    CHECK(take() && !s_scene.toast.present);

    /* The public duration is uint32_t. A duration above INT32_MAX must not
     * be treated as already elapsed, including through a clock wrap. */
    const uint32_t start = 10000U, duration = UINT32_MAX - 7U;
    s_now = start;
    overlay("later", NULL, UINT32_MAX);
    toast("long", duration);
    CHECK(ui_layers_next_due(ui_display_at(0), &due) && due == start + duration);
    CHECK(take() && s_scene.toast.present && s_scene.overlay.present);
    s_now = start + duration - 1U;
    CHECK(!take() && s_scene.toast.present && s_scene.overlay.present);
    s_now = start + duration;
    CHECK(take() && !s_scene.toast.present && s_scene.overlay.present);
    s_now = start + UINT32_MAX;
    CHECK(take() && !s_scene.overlay.present);
    return 0;
}

static int test_before_start_and_without_theme(void)
{
    xiaotai_display_desc_t descs[2] = {ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB),
                                       ui_fake_desc(320, 240, XIAOTAI_PIXEL_RGB565)};
    ui_fake_board_set_displays(descs, 2);
    CHECK(setup(NULL, false) == 0);
    page("boot", "starting");
    xiaotai_view_t second = {.kind = XIAOTAI_VIEW_HOME, .display = 1};
    ui_view_accept(&second);
    CHECK(!info(0).ready && strcmp(info(0).page, "boot") == 0 && strcmp(info(1).page, "home") == 0);
    CHECK(ui_fake_display_opens(0) == 0U && ui_fake_theme_starts(0) == 0U);

    xiaotai_engine_ui_start();
    xiaotai_ui_info_t first = info(0);
    CHECK(first.ready && strcmp(first.theme, "mono") == 0 && first.width == 128 && first.height == 64);
    CHECK(first.pixel == XIAOTAI_PIXEL_MONO1_VLSB);
    CHECK(ui_fake_display_opens(0) == 1U && ui_fake_theme_starts(0) == 1U);
    /* The view shown before start is drawn after it. */
    CHECK(take() && id_is(&s_scene.page, "boot") && text_is(&s_scene.page, "starting"));

    /* No theme draws 320x240 colour here: the display stays closed and its
     * views are counted, not kept. */
    CHECK(ui_fake_display_opens(1) == 0U && !info(1).ready && info(1).theme[0] == '\0');
    ui_view_accept(&second);
    CHECK(info(1).dropped == 1U && info(1).page[0] == '\0');
    CHECK(g_ui.displays_closed == 1U);

    /* A display the board does not have: counted on the first display. */
    xiaotai_view_t missing = {.kind = XIAOTAI_VIEW_HOME, .display = 7};
    ui_view_accept(&missing);
    CHECK(info(0).dropped == 1U);
    xiaotai_ui_info_t out;
    CHECK(xiaotai_ui_inspect(2, &out) == XIAOTAI_ERR_NOT_FOUND);

    /* A layer number beyond the toast is no layer. */
    xiaotai_view_t bad = {.kind = XIAOTAI_VIEW_HOME, .layer = 3};
    ui_view_accept(&bad);
    CHECK(info(0).dropped == 2U);

    /* Reset stops the theme on the displays it runs on. */
    xiaotai_ui_reset();
    CHECK(ui_fake_theme_stops(0) == 1U);
    boards_one_display();
    return 0;
}

static int test_memory(void)
{
    xiaotai_ui_reset();
    xiaotai_mem_host_clear();
    CHECK(setup(NULL, true) == 0);
    const uint32_t at_start = xiaotai_mem_host_allocations();
    page("p", NULL);
    toast("t", 0);
    CHECK(xiaotai_mem_host_allocations() == at_start);
    /* The dialog's buffers on first use only. */
    overlay("a", NULL, 0);
    CHECK(xiaotai_mem_host_allocations() == at_start + 1U);
    overlay("b", NULL, 0);
    dismiss(XIAOTAI_LAYER_OVERLAY, NULL);
    overlay("c", NULL, 0);
    CHECK(xiaotai_mem_host_allocations() == at_start + 1U);
    xiaotai_ui_reset();
    CHECK(xiaotai_mem_host_outstanding() == 0);

    /* No memory for the dialog: dropped and counted, and the next dialog
     * takes its buffers. */
    CHECK(setup(NULL, true) == 0);
    xiaotai_mem_host_fail_nth(1);
    overlay("a", NULL, 0);
    CHECK(!xiaotai_mem_host_fail_pending());
    CHECK(info(0).dropped == 1U && info(0).overlay[0] == '\0');
    overlay("a", NULL, 0);
    CHECK(take() && id_is(&s_scene.overlay, "a"));

    /* Every allocation of init failing in turn: an error, nothing kept. */
    uint32_t failures = 0;
    for (uint32_t n = 1; n < 64; ++n) {
        xiaotai_ui_reset();
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(n);
        xiaotai_err_t err = xiaotai_engine_ui_init();
        if (err == XIAOTAI_OK) {
            CHECK(!xiaotai_mem_host_fail_pending() || n > xiaotai_mem_host_allocations());
            break;
        }
        CHECK(err == XIAOTAI_ERR_NO_MEM && xiaotai_mem_host_outstanding() == 0);
        ++failures;
    }
    /* Three registries, the display table, the page and the toast. */
    CHECK(failures == 6U);
    xiaotai_ui_reset();
    CHECK(xiaotai_mem_host_outstanding() == 0);
    xiaotai_mem_host_fail_nth(0);

    /* The product's layer size bounds what a page keeps. */
    static char long_text[1500];
    memset(long_text, 'z', sizeof(long_text) - 1U);
    const xiaotai_ui_config_t config = {.layer_bytes = 1024};
    CHECK(setup(&config, true) == 0);
    CHECK(ui_display_at(0)->slots[XIAOTAI_LAYER_PAGE].capacity == 1024U);
    CHECK(ui_display_at(0)->slots[XIAOTAI_LAYER_TOAST].capacity == XIAOTAI_UI_TOAST_BYTES);
    page("p", long_text);
    toast(long_text, 0);
    CHECK(take());
    CHECK(strlen(s_scene.page.view.text) == 1024U - 2U - 1U);   /* the id "p" first */
    CHECK(strlen(s_scene.toast.view.text) == XIAOTAI_UI_TOAST_BYTES - 1U);
    CHECK(info(0).truncated == 2U);

    /* Without a product value: 1536 bytes on a board without external RAM. */
    CHECK(setup(NULL, false) == 0);
    CHECK(ui_display_at(0)->slots[XIAOTAI_LAYER_PAGE].capacity ==
          (xiaotai_mem_has_external() ? UI_LAYER_BYTES_EXTERNAL : UI_LAYER_BYTES_INTERNAL));
    return 0;
}

/* ------------------------------------------------- through the framework */

static xiaotai_err_t theme_module_init(void) { return xiaotai_ui_theme_register(ui_fake_theme(0, &MONO)); }

static xiaotai_err_t app_init(void)
{
    const xiaotai_view_t view = {.kind = XIAOTAI_VIEW_HOME, .id = "home", .title = "XiaoTai"};
    xiaotai_view_show(&view);
    return XIAOTAI_OK;
}

static void app_start(void) { xiaotai_view_toast("ready", 0); }

static const char *const THEME_DEPENDS[] = {"ui", NULL};
static const xiaotai_module_t UI_ENGINE = {.name = "ui", .init = xiaotai_engine_ui_init,
                                           .start = xiaotai_engine_ui_start};
static const xiaotai_module_t THEME = {.name = "theme_mono", .depends = THEME_DEPENDS, .init = theme_module_init};
static const xiaotai_application_t APP = {.name = "companion", .init = app_init, .start = app_start};

static int test_framework_sink(void)
{
    xiaotai_ui_reset();
    const xiaotai_module_t *modules[] = {&THEME, &UI_ENGINE};
    const xiaotai_application_t *applications[] = {&APP};
    xiaotai_framework_config_t config = {.modules = modules, .module_count = 2, .applications = applications,
                                         .application_count = 1};
    s_now = 50000;
    ui_driver_manual(true);
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    xiaotai_ui_info_t i = info(0);
    CHECK(i.ready && strcmp(i.theme, "mono") == 0 && strcmp(i.page, "home") == 0 && i.shown == 2U);
    CHECK(take() && id_is(&s_scene.page, "home") && text_is(&s_scene.toast, "ready"));
    CHECK(strcmp(s_scene.page.view.title, "XiaoTai") == 0);
    /* The theme was started on display 0 with that display's host. */
    const xiaotai_ui_host_t *host = ui_fake_theme_host(0, 0);
    CHECK(host && xiaotai_ui_host_index(host) == 0 && xiaotai_ui_host_display(host) == &ui_fake_display(0)->desc);
    CHECK(xiaotai_ui_host_root(host) == NULL);
    xiaotai_framework_reset();
    xiaotai_ui_reset();
    ui_driver_manual(false);
    return 0;
}

static int test_odd_layer_alignment(void)
{
    const xiaotai_ui_config_t config = {.layer_bytes = 1537};
    CHECK(setup(&config, true) == 0);
    const xiaotai_view_action_t action = {.intent = "menu", .label = "Menu"};
    uint64_t payload = UINT64_C(0x123456789abcdef0);
    const xiaotai_view_t page = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = "home", .custom = "home",
        .actions = &action, .action_count = 1, .custom_data = &payload, .custom_size = sizeof(payload),
    };
    xiaotai_view_t dialog = page;
    dialog.layer = XIAOTAI_LAYER_OVERLAY;
    dialog.id = "dialog";
    for (unsigned i = 0; i < 2; ++i) {
        ++payload;
        ui_view_accept(&page);
        ui_view_accept(&dialog);
        CHECK(take());
        const xiaotai_view_t *const views[] = {&s_scene.page.view, &s_scene.overlay.view};
        for (size_t n = 0; n < sizeof(views) / sizeof(views[0]); ++n) {
            const xiaotai_view_t *view = views[n];
            CHECK(view->action_count == 1 && view->custom_size == sizeof(payload));
            CHECK((uintptr_t)view->actions % _Alignof(xiaotai_view_action_t) == 0);
            CHECK((uintptr_t)view->custom_data % _Alignof(max_align_t) == 0);
            CHECK(*(const uint64_t *)view->custom_data == payload);
            CHECK(strcmp(view->actions[0].label, "Menu") == 0);
        }
    }
    CHECK(ui_display_at(0)->slots[XIAOTAI_LAYER_PAGE].capacity == config.layer_bytes);
    CHECK(ui_display_at(0)->slots[XIAOTAI_LAYER_OVERLAY].capacity == config.layer_bytes);
    return 0;
}

int main(void)
{
    boards_one_display();
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    RUN(test_page_change_keeps_toast);
    RUN(test_overlay_removes_toast);
    RUN(test_updates_and_coalescing);
    RUN(test_dismiss);
    RUN(test_durations);
    RUN(test_before_start_and_without_theme);
    RUN(test_memory);
    RUN(test_framework_sink);
    RUN(test_odd_layer_alignment);
    xiaotai_ui_reset();
    return 0;
}
