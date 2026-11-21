// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_ui.c::ui
// Replaces legacy: tests/regression/test_p4_room_ptt_feedback.py
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/port/clock.h"
#include "xiaotai_clock_host.h"

#include <string.h>

static uint32_t now;
static xiaotai_signal_t done;
#define OWNER_QUEUE_DEPTH 32U
static ui_display_t *display;
static const xiaotai_ui_host_t *host;
static struct {
    const char *name;
    xiaotai_args_t args;
} messages[128];
static size_t received;
static bool bind_local_hold;
static struct {
    xiaotai_ui_hold_phase_t phase;
    uint32_t ticket;
    int32_t value;
} local_edges[16];
static size_t local_count;
static xiaotai_err_t local_begin_result;

#define PAGE XIAOTAI_LAYER_PAGE
#define OVERLAY XIAOTAI_LAYER_OVERLAY
#define ACTION XIAOTAI_UI_ELEMENT_ACTION
#define ITEM XIAOTAI_UI_ELEMENT_ITEM
#define FIELD XIAOTAI_UI_ELEMENT_FIELD
#define INPUT(layer, what, index, how) xiaotai_ui_host_input(host, layer, what, index, XIAOTAI_UI_INPUT_##how, 0, NULL)

static uint32_t clock_ms(void) { return now; }
static void barrier_job(void *ctx) { xiaotai_signal_give(ctx); }
static void noop_job(void *ctx) { (void)ctx; }
static void block_owner(void *ctx)
{
    xiaotai_signal_give(&done);
    (void)xiaotai_signal_take(ctx, 5000U);
}
static bool barrier(void)
{
    return xiaotai_run(barrier_job, &done) == XIAOTAI_OK && xiaotai_signal_take(&done, 5000U);
}

static void capture(const xiaotai_args_t *args, void *ctx)
{
    if (received < sizeof(messages) / sizeof(messages[0])) {
        messages[received].name = ctx;
        messages[received++].args = *args;
    }
}

static void capture_event(const xiaotai_event_t *event, void *ctx) { capture(&event->args, ctx); }

static const char *const intents[] = {"pick", "update", "primary", "arrow", "hold-a", "hold-b", "long",
                                       "overlay.back", "aux", "slide"};
static const char *const events[] = {XIAOTAI_EVENT_VIEW_CHANGED, XIAOTAI_EVENT_UI_INTERACTION,
                                     XIAOTAI_EVENT_UI_REJECTED};

static xiaotai_err_t trace_local_hold(void *ctx, uint32_t ticket, int32_t value, xiaotai_ui_hold_phase_t phase)
{
    xiaotai_ui_info_t info;
    if (ctx != &local_count || local_count >= sizeof(local_edges) / sizeof(*local_edges) ||
        xiaotai_ui_inspect(0, &info) != XIAOTAI_OK) return XIAOTAI_ERR_INVALID_STATE;
    local_edges[local_count].phase = phase;
    local_edges[local_count].ticket = ticket;
    local_edges[local_count++].value = value;
    return phase == XIAOTAI_UI_HOLD_BEGIN ? local_begin_result : XIAOTAI_OK;
}

static xiaotai_err_t app_init(void)
{
    if (bind_local_hold) {
        const xiaotai_err_t err = xiaotai_ui_local_hold_register("hold-a", trace_local_hold, &local_count);
        if (err != XIAOTAI_OK) return err;
        if (xiaotai_ui_local_hold_register("hold-b", trace_local_hold, NULL) != XIAOTAI_ERR_INVALID_STATE)
            return XIAOTAI_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < sizeof(intents) / sizeof(intents[0]); ++i) {
        const xiaotai_err_t err = xiaotai_intent_handle(intents[i], capture, (void *)intents[i]);
        if (err != XIAOTAI_OK) return err;
    }
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        const xiaotai_err_t err = xiaotai_event_subscribe(events[i], capture_event, (void *)events[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t theme_init(void)
{
    const ui_fake_theme_config_t config = {.name = "test", .backend = XIAOTAI_UI_BACKEND_FRAME,
                                           .width = 128, .height = 64, .pixel = XIAOTAI_PIXEL_MONO1, .exact = 10};
    return xiaotai_ui_theme_register(ui_fake_theme(0, &config));
}

static int setup(uint32_t swipe_guard)
{
    const xiaotai_ui_config_t cfg = {.swipe_tap_guard_ms = swipe_guard};
    xiaotai_display_desc_t panel = ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1);
    panel.touch = bind_local_hold;
    const xiaotai_display_desc_t board[] = {panel, panel};
    ui_fake_board_set_displays(board, bind_local_hold ? 2U : 1U);
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&done) == XIAOTAI_OK);
    ui_core_set_clock(clock_ms);
    ui_driver_manual(true);
    CHECK(xiaotai_ui_configure(&cfg) == XIAOTAI_OK);
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const char *const deps[] = {"ui", NULL};
    static const xiaotai_module_t theme = {.name = "theme_test", .init = theme_init, .depends = deps};
    static const xiaotai_application_t app = {.name = "test", .init = app_init};
    static const xiaotai_module_t *modules[] = {&ui, &theme};
    static const xiaotai_application_t *apps[] = {&app};
    const xiaotai_framework_config_t fw = {.modules = modules, .module_count = 2U,
                                           .applications = apps, .application_count = 1U,
                                           .queue_depth = OWNER_QUEUE_DEPTH};
    CHECK(xiaotai_framework_start(&fw) == XIAOTAI_OK);
    CHECK(barrier());
    received = 0U;
    display = ui_display_at(0U);
    host = ui_fake_theme_host(0U, 0U);
    CHECK(display && host && xiaotai_intent_exists(XIAOTAI_INTENT_UI_CONFIRM));
    return 0;
}

static void teardown(void)
{
    xiaotai_framework_reset();
    xiaotai_ui_reset();
    xiaotai_signal_deinit(&done);
}

static int count(const char *name)
{
    int n = 0;
    for (size_t i = 0; i < received; ++i) if (strcmp(messages[i].name, name) == 0) ++n;
    return n;
}

static bool has(const char *name, int32_t value, const char *text)
{
    for (size_t n = 0; n < received; ++n) {
        if (!strcmp(messages[n].name, name) && messages[n].args.value == value &&
            !strcmp(messages[n].args.text, text)) return true;
    }
    return false;
}

static bool key(const char *intent)
{
    if (xiaotai_intent_post(intent, NULL) != XIAOTAI_OK || !barrier()) return false;
    (void)ui_scene_step(display, now);
    return barrier();
}

static void show(const xiaotai_view_t *view)
{
    ui_view_accept(view);
    (void)ui_scene_step(display, now);
}

static int focus(void)
{
    xiaotai_view_item_t items[] = {{.label = "One", .intent = "pick", .value = 1},
                                   {.label = "Two", .intent = "pick", .value = 2},
                                   {.label = "Three", .intent = "pick", .value = 3}};
    xiaotai_view_t page = {.kind = XIAOTAI_VIEW_LIST, .id = "contacts", .items = items, .item_count = 3, .focus = 2};
    show(&page);
    CHECK(barrier() && has(XIAOTAI_EVENT_VIEW_CHANGED, 0, "contacts"));
    CHECK(display->scene.page.focus == 1U);
    CHECK(xiaotai_intent_post_external(XIAOTAI_INTENT_UI_CONFIRM, NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    page.focus = 0;
    page.text = "Updated caption";
    items[0].detail = "Updated row";
    show(&page);
    CHECK(barrier() && count(XIAOTAI_EVENT_VIEW_CHANGED) == 1);
    CHECK(display->scene.page.focus == 1U);
    items[1].value = 22;
    show(&page);
    CHECK(barrier() && count(XIAOTAI_EVENT_VIEW_CHANGED) == 2);
    CHECK(key(XIAOTAI_INTENT_UI_NEXT) && display->scene.page.focus == 2U);
    CHECK(key(XIAOTAI_INTENT_UI_NEXT) && display->scene.page.focus == 0U);
    CHECK(key(XIAOTAI_INTENT_UI_PREVIOUS) && display->scene.page.focus == 2U);
    page.item_count = 2U;
    show(&page);
    CHECK(barrier() && display->scene.page.focus == 1U && count(XIAOTAI_EVENT_VIEW_CHANGED) == 3);
    CHECK(key(XIAOTAI_INTENT_UI_CONFIRM) && has("pick", 22, ""));
    INPUT(PAGE, ITEM, 0, TAP);
    CHECK(barrier() && has("pick", 1, "") && display->scene.page.focus == 0U);
    CHECK(count(XIAOTAI_EVENT_UI_INTERACTION) == 1);
    now += 250;
    items[0].state = XIAOTAI_STATE_BUSY;
    show(&page);
    CHECK(key(XIAOTAI_INTENT_UI_CONFIRM));
    INPUT(PAGE, ITEM, 99, TAP);
    items[1].intent = "missing";
    show(&page);
    INPUT(PAGE, ITEM, 1, TAP);
    CHECK(barrier() && has(XIAOTAI_EVENT_UI_REJECTED, 0, "missing"));
    CHECK(count("pick") == 2 && count(XIAOTAI_EVENT_UI_INTERACTION) == 2);
    const xiaotai_view_action_t back = {.role = XIAOTAI_ROLE_BACK, .intent = "overlay.back"};
    const xiaotai_view_t dialog = {.kind = XIAOTAI_VIEW_NOTICE, .layer = OVERLAY, .actions = &back, .action_count = 1};
    show(&dialog);
    CHECK(key(XIAOTAI_INTENT_UI_BACK) && count("overlay.back") == 1);
    INPUT(PAGE, ITEM, 1, TAP);
    xiaotai_ui_info_t info;
    CHECK(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.ignored == 4U);

    page.id = "settings";
    page.focus = 2U;
    page.flags = XIAOTAI_VIEW_FOCUS_ON_ENTRY;
    ui_view_accept(&page);
    page.status = "Refresh before the first paint";
    show(&page);
    CHECK(display->scene.page.focus == 1U);
    CHECK(key(XIAOTAI_INTENT_UI_NEXT) && display->scene.page.focus == 0U);
    page.status = "Refresh after the user moved";
    show(&page);
    CHECK(display->scene.page.focus == 0U);
    page.flags = 0U;
    show(&page);
    CHECK(display->scene.page.focus == 1U);
    return 0;
}

static int digits(void)
{
    const xiaotai_view_field_t fields[] = {
        {.kind = XIAOTAI_FIELD_DIGITS, .intent = "update", .text = "90", .max = 2},
        {.kind = XIAOTAI_FIELD_DIGITS, .intent = "update", .text = "4", .max = 1},
        {.kind = XIAOTAI_FIELD_SWITCH, .intent = "update", .value = 1},
        {.kind = XIAOTAI_FIELD_STEPPER, .intent = "update", .value = 8, .min = 3, .max = 9, .step = 2},
        {.kind = XIAOTAI_FIELD_SLIDER, .intent = "slide", .min = 0, .max = 100, .flags = XIAOTAI_FIELD_LIVE},
    };
    const xiaotai_view_action_t ok = {.intent = "primary", .role = XIAOTAI_ROLE_PRIMARY};
    xiaotai_view_t page = {.kind = XIAOTAI_VIEW_FORM, .id = "form", .fields = fields, .field_count = 5,
                           .actions = &ok, .action_count = 1};
    show(&page);
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_UI_NEXT, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_UI_NEXT, NULL) == XIAOTAI_OK);
    CHECK(barrier());
    (void)ui_scene_step(display, now);
    CHECK(barrier() && has("update", 0, "00") && has("update", 0, "10"));
    CHECK(key(XIAOTAI_INTENT_UI_CONFIRM) && display->scene.page.cursor == 1U);
    page.text = "caption";
    show(&page);
    CHECK(display->scene.page.cursor == 1U);
    CHECK(key(XIAOTAI_INTENT_UI_PREVIOUS) && has("update", 0, "99"));
    CHECK(key(XIAOTAI_INTENT_UI_CONFIRM) && display->scene.page.focus == 1U && display->scene.page.cursor == 0U);
    CHECK(key(XIAOTAI_INTENT_UI_CONFIRM) && count("primary") == 1);
    page.focus = 3;
    show(&page);
    CHECK(key(XIAOTAI_INTENT_UI_CONFIRM) && has("update", 0, ""));
    page.focus = 4;
    show(&page);
    CHECK(key(XIAOTAI_INTENT_UI_CONFIRM) && has("update", 3, ""));
    xiaotai_ui_host_input(host, PAGE, FIELD, 4, XIAOTAI_UI_INPUT_CHANGE, 35, NULL);
    xiaotai_ui_host_input(host, PAGE, FIELD, 4, XIAOTAI_UI_INPUT_COMMIT, 40, NULL);
    xiaotai_ui_host_input(host, PAGE, FIELD, 4, XIAOTAI_UI_INPUT_COMMIT, 101, NULL);
    xiaotai_ui_host_input(host, PAGE, FIELD, 0, XIAOTAI_UI_INPUT_COMMIT, 0, "9");
    CHECK(barrier() && count("slide") == 2 && has("slide", 35, XIAOTAI_ARG_LIVE) && has("slide", 40, ""));
    return 0;
}

static int hold(void)
{
    xiaotai_view_action_t actions[] = {{.intent = "hold-a", .trigger = XIAOTAI_TRIGGER_HOLD, .value = 7},
                                      {.intent = "long", .trigger = XIAOTAI_TRIGGER_LONG_PRESS}};
    xiaotai_view_t page = {.kind = XIAOTAI_VIEW_TALK, .id = "room", .actions = actions, .action_count = 2};
    now = UINT32_MAX - 150U;
    show(&page);
    INPUT(PAGE, ACTION, 0, PRESS);
    INPUT(PAGE, ACTION, 0, PRESS);
    CHECK(barrier() && count("hold-a") == 1 && has("hold-a", 7, XIAOTAI_ARG_PRESS));
    const uint32_t base = now;
    uint32_t due = 0U;
    CHECK(ui_scene_next_due(display, now, &due) && due == base + 250U);
    now = base + 249U;
    (void)ui_scene_step(display, now);
    CHECK(barrier() && count("hold-a") == 1);
    now = base + 800U;
    (void)ui_scene_step(display, now);
    CHECK(barrier() && count("hold-a") == 2 && ui_scene_next_due(display, now, &due) && due == now + 250U);
    actions[0].state = XIAOTAI_STATE_DISABLED;
    actions[0].intent = "hold-b";
    show(&page);
    INPUT(PAGE, ACTION, 0, RELEASE);
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(barrier() && count("hold-a") == 3 && count("hold-b") == 0 && has("hold-a", 7, XIAOTAI_ARG_RELEASE));
    actions[0].state = 0;
    show(&page);
    INPUT(PAGE, ACTION, 0, PRESS);
    ui_input_cancel_press(display);
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(barrier() && count("hold-b") == 2 && has("hold-b", 7, XIAOTAI_ARG_RELEASE));
    INPUT(PAGE, ACTION, 1, PRESS);
    INPUT(PAGE, ACTION, 1, RELEASE);
    INPUT(PAGE, ACTION, 1, TAP);
    INPUT(PAGE, ACTION, 1, PRESS);
    INPUT(PAGE, ACTION, 1, LONG_PRESS);
    INPUT(PAGE, ACTION, 1, LONG_PRESS);
    INPUT(PAGE, ACTION, 1, RELEASE);
    CHECK(barrier() && count("long") == 1);

    INPUT(PAGE, ACTION, 0, PRESS);
    CHECK(barrier() && count("hold-b") == 3);
    xiaotai_signal_t unblock;
    CHECK(xiaotai_signal_init(&unblock) == XIAOTAI_OK);
    CHECK(xiaotai_run(block_owner, &unblock) == XIAOTAI_OK && xiaotai_signal_take(&done, 5000U));
    for (unsigned n = 1; n < OWNER_QUEUE_DEPTH; ++n) CHECK(xiaotai_run(noop_job, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_run(barrier_job, &done) == XIAOTAI_OK);
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(display->press.release_pending && ui_scene_next_due(display, now, &due) && due == now + 10U);
    xiaotai_signal_give(&unblock);
    CHECK(xiaotai_signal_take(&done, 5000U));
    xiaotai_signal_deinit(&unblock);
    now += 10U;
    (void)ui_scene_step(display, now);
    CHECK(barrier() && count("hold-b") == 4 && !display->press.release_pending);
    now += 500U;
    (void)ui_scene_step(display, now);
    CHECK(barrier() && count("hold-b") == 4);
    return 0;
}

static bool local_pair(size_t n, int32_t value)
{
    return local_count == n + 2U && local_edges[n].phase == XIAOTAI_UI_HOLD_BEGIN &&
           local_edges[n + 1U].phase == XIAOTAI_UI_HOLD_END && local_edges[n].ticket != 0U &&
           local_edges[n].ticket == local_edges[n + 1U].ticket && local_edges[n].value == value &&
           local_edges[n + 1U].value == value;
}

static int local_hold(void)
{
    xiaotai_view_action_t action = {.intent = "hold-a", .trigger = XIAOTAI_TRIGGER_HOLD, .value = 7};
    xiaotai_view_t page = {.kind = XIAOTAI_VIEW_TALK, .id = "room", .actions = &action, .action_count = 1};
    show(&page);
    CHECK(barrier());
    CHECK(xiaotai_ui_local_hold_register("hold-b", trace_local_hold, NULL) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_t unblock;
    CHECK(xiaotai_signal_init(&unblock) == XIAOTAI_OK);
    CHECK(xiaotai_run(block_owner, &unblock) == XIAOTAI_OK && xiaotai_signal_take(&done, 5000U));
    for (unsigned n = 1; n < OWNER_QUEUE_DEPTH; ++n) CHECK(xiaotai_run(noop_job, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_run(barrier_job, &done) == XIAOTAI_OK);
    CHECK(xiaotai_run(noop_job, NULL) == XIAOTAI_ERR_NO_MEM);
    INPUT(PAGE, ACTION, 0, PRESS);
    INPUT(PAGE, ACTION, 0, PRESS);
    now += 2000U;
    (void)ui_scene_step(display, now);
    const bool no_repeat = local_count == 1U && display->press.hold_ticket != 0U;
    INPUT(PAGE, ACTION, 1, RELEASE);  /* the original hit target has moved */
    INPUT(PAGE, ACTION, 0, RELEASE);
    const bool full_queue_release = local_pair(0U, 7) && !display->press.release_pending &&
                                    xiaotai_run(noop_job, NULL) == XIAOTAI_ERR_NO_MEM;
    xiaotai_signal_give(&unblock);
    CHECK(xiaotai_signal_take(&done, 5000U));
    xiaotai_signal_deinit(&unblock);
    CHECK(no_repeat && full_queue_release && count("hold-a") == 0);

    INPUT(PAGE, ACTION, 0, PRESS);
    page.text = "Unrelated view content";
    show(&page);
    CHECK(local_count == 3U);
    action.value = 21;
    show(&page);
    CHECK(local_pair(2U, 7) && local_edges[0].ticket != local_edges[2].ticket);
    INPUT(PAGE, ACTION, 0, PRESS);
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(local_count == 4U);
    INPUT(PAGE, ACTION, 0, PRESS);
    const xiaotai_view_t overlay = {.kind = XIAOTAI_VIEW_NOTICE, .layer = OVERLAY, .id = "dialog"};
    show(&overlay);
    CHECK(local_pair(4U, 21) && local_edges[2].ticket != local_edges[4].ticket);
    INPUT(PAGE, ACTION, 0, RELEASE);
    const xiaotai_view_t dismiss = {.layer = OVERLAY, .flags = XIAOTAI_VIEW_DISMISS, .id = "dialog"};
    show(&dismiss);
    CHECK(local_count == 6U);

    local_begin_result = XIAOTAI_ERR_BUSY;
    INPUT(PAGE, ACTION, 0, PRESS);
    now += 2000U;
    (void)ui_scene_step(display, now);
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(local_count == 7U && local_edges[6].phase == XIAOTAI_UI_HOLD_BEGIN &&
          local_edges[6].ticket != local_edges[4].ticket);
    CHECK(barrier() && count(XIAOTAI_EVENT_UI_REJECTED) == 0 && count("hold-a") == 0);

    const xiaotai_ui_host_t *other = ui_fake_theme_host(0U, 1U);
    CHECK(other && ui_display_at(1U));
    page.display = 1;
    ui_view_accept(&page);
    (void)ui_scene_step(ui_display_at(1U), now);
    xiaotai_ui_host_input(other, PAGE, ACTION, 0, XIAOTAI_UI_INPUT_PRESS, 0, NULL);
    xiaotai_ui_host_input(other, PAGE, ACTION, 0, XIAOTAI_UI_INPUT_RELEASE, 0, NULL);
    CHECK(barrier() && count("hold-a") == 2 && has("hold-a", 21, XIAOTAI_ARG_PRESS) &&
          has("hold-a", 21, XIAOTAI_ARG_RELEASE) && local_count == 7U);

    page.display = 0;
    action.intent = "hold-b";
    show(&page);
    INPUT(PAGE, ACTION, 0, PRESS);
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(barrier() && count("hold-b") == 2 && local_count == 7U);
    action.intent = "hold-a";
    show(&page);
    local_begin_result = XIAOTAI_OK;
    INPUT(PAGE, ACTION, 0, PRESS);
    /* The HAL reports the finger is up before any widget reports RELEASED. */
    display->pointer.down = true;
    ui_pointer_step(display, now, true);
    CHECK(local_pair(7U, 21));
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(local_count == 9U);
    INPUT(PAGE, ACTION, 0, PRESS);
    xiaotai_ui_reset();
    CHECK(local_pair(9U, 21) && local_edges[7].ticket != local_edges[9].ticket);
    xiaotai_ui_reset();
    CHECK(local_count == 11U && count("hold-a") == 2 &&
          xiaotai_ui_local_hold_register("hold-a", trace_local_hold, &local_count) == XIAOTAI_ERR_INVALID_STATE);
    return 0;
}

static void tap(size_t index)
{
    INPUT(PAGE, ACTION, index, PRESS);
    INPUT(PAGE, ACTION, index, RELEASE);
    INPUT(PAGE, ACTION, index, TAP);
}

static int swipe(void)
{
    const xiaotai_view_action_t actions[] = {{.intent = "primary", .role = XIAOTAI_ROLE_PRIMARY},
        {.intent = "arrow", .trigger = XIAOTAI_TRIGGER_SWIPE_LEFT, .icon = "next"}};
    xiaotai_view_t page = {.kind = XIAOTAI_VIEW_HOME, .id = "home", .actions = actions, .action_count = 2};
    now = 1000;
    show(&page);
    INPUT(PAGE, ACTION, 0, PRESS);
    INPUT(PAGE, ACTION, 0, SWIPE_LEFT);
    INPUT(PAGE, ACTION, 0, SWIPE_LEFT);
    INPUT(PAGE, ACTION, 0, TAP);
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(barrier() && count("arrow") == 1 && count("primary") == 0);
    now = 1349;
    page.id = "other-home";
    show(&page);
    xiaotai_ui_info_t before, after;
    CHECK(xiaotai_ui_inspect(0, &before) == XIAOTAI_OK);
    tap(0);
    CHECK(xiaotai_ui_inspect(0, &after) == XIAOTAI_OK && after.ignored == before.ignored + 1U);
    tap(1);
    CHECK(barrier() && count("arrow") == 2 && count("primary") == 0);
    INPUT(PAGE, ACTION, 0, PRESS);
    INPUT(PAGE, ACTION, 0, SWIPE_LEFT);
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(barrier() && count("arrow") == 2);
    now = 1350;
    tap(0);
    CHECK(barrier() && count("primary") == 1);
    INPUT(PAGE, ACTION, 0, PRESS);
    INPUT(PAGE, ACTION, 0, SWIPE_LEFT);
    INPUT(PAGE, ACTION, 0, RELEASE);
    CHECK(barrier() && count("arrow") == 3);
    g_ui_config.swipe_tap_guard_ms = 0;
    now += 1U;
    tap(0);
    CHECK(barrier() && count("primary") == 2);
    xiaotai_view_t overlay = {.kind = XIAOTAI_VIEW_NOTICE, .layer = OVERLAY};
    show(&overlay);
    now += 500U;
    INPUT(PAGE, ACTION, 0, SWIPE_LEFT);
    CHECK(barrier() && count("arrow") == 3);
    return 0;
}

static int timer(void)
{
    xiaotai_view_t page = {.kind = XIAOTAI_VIEW_CODE, .id = "code", .timer_mode = XIAOTAI_TIMER_REMAINING, .timer_s = 2};
    const uint32_t start = UINT32_MAX - 300U;
    now = start;
    ui_view_accept(&page);
    now = start + 800U;
    CHECK(ui_scene_step(display, now) && display->scene.page.timer_s == 2);
    uint32_t due = 0;
    CHECK(ui_scene_next_due(display, now, &due) && due == start + 1000U);
    now = start + 1000U;
    CHECK(ui_scene_step(display, now) && display->scene.page.timer_s == 1 &&
          display->scene.page.changed == XIAOTAI_CHANGED_TIMER);
    page.text = "still same timer";
    now = start + 1400U;
    show(&page);
    CHECK(display->scene.page.timer_s == 1);
    now = start + 2000U;
    CHECK(ui_scene_step(display, now) && display->scene.page.timer_s == 0 && display->scene.page.timer_expired);
    ++now;
    CHECK(!ui_scene_step(display, now) && !ui_scene_next_due(display, now, &due));
    page.timer_mode = XIAOTAI_TIMER_ELAPSED;
    page.timer_s = 11;
    show(&page);
    now += 3040U;
    CHECK(ui_scene_step(display, now) && display->scene.page.timer_s == 14 && !display->scene.page.timer_expired);
    CHECK(barrier() && count(XIAOTAI_EVENT_VIEW_CHANGED) == 1);
    const char *const servers[] = {"clock.invalid"};
    const xiaotai_clock_config_t config = {.servers = servers, .server_count = 1U, .timezone = "CST-8"};
    CHECK(xiaotai_clock_init(&config) == XIAOTAI_OK);
    const xiaotai_view_t clock = {.kind = XIAOTAI_VIEW_CLOCK, .id = "clock"};
    show(&clock);
    CHECK(!display->scene.clock.valid && ui_scene_next_due(display, now, &due) && due == now + 1000U);
    const int64_t unix_ms = INT64_C(1704067198123);
    CHECK(xiaotai_clock_set_unix_ms(unix_ms) == XIAOTAI_OK);
    xiaotai_clock_host_set_wall_ms(unix_ms);
    now = due;
    CHECK(ui_scene_step(display, now) && display->scene.clock_changed && display->scene.clock.valid);
    CHECK(display->scene.clock.day == 1 && display->scene.clock.hour == 7 && display->scene.clock.minute == 59);
    CHECK(ui_scene_next_due(display, now, &due) && due == now + 877U);
    xiaotai_clock_host_advance_ms(877U);
    now = due;
    CHECK(!ui_scene_step(display, now) && display->scene.clock.second == 59);
    xiaotai_clock_host_advance_ms(1000U);
    now += 1000U;
    CHECK(ui_scene_step(display, now) && display->scene.clock.hour == 8 && display->scene.clock.minute == 0);
    xiaotai_clock_host_reset();
    return 0;
}

static int stale_press(void)
{
    xiaotai_view_item_t items[] = {{.intent = "pick", .value = 1}, {.intent = "pick", .value = 2}};
    xiaotai_view_t page = {.kind = XIAOTAI_VIEW_LIST, .id = "old", .items = items, .item_count = 2};
    show(&page);
    INPUT(PAGE, ITEM, 0, PRESS);
    items[1].detail = "changed without replacing the pressed item";
    show(&page);
    INPUT(PAGE, ITEM, 0, RELEASE);
    INPUT(PAGE, ITEM, 0, TAP);
    CHECK(barrier() && count("pick") == 1 && has("pick", 1, ""));
    INPUT(PAGE, ITEM, 0, PRESS);
    items[0].value = 11;
    show(&page);
    INPUT(PAGE, ITEM, 0, RELEASE);
    INPUT(PAGE, ITEM, 0, TAP);
    CHECK(barrier() && count("pick") == 1);
    INPUT(PAGE, ITEM, 0, PRESS);
    INPUT(PAGE, ITEM, 0, RELEASE);
    INPUT(PAGE, ITEM, 0, TAP);
    CHECK(barrier() && count("pick") == 2 && has("pick", 11, ""));
    INPUT(PAGE, ITEM, 0, PRESS);
    page.id = "new";
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_UI_CONFIRM, NULL) == XIAOTAI_OK && barrier());
    show(&page);
    INPUT(PAGE, ITEM, 0, RELEASE);
    INPUT(PAGE, ITEM, 0, TAP);
    CHECK(barrier() && count("pick") == 2 && has(XIAOTAI_EVENT_VIEW_CHANGED, 0, "new"));
    for (size_t i = 0; i < UI_NAV_DEPTH + 1U; ++i) CHECK(xiaotai_intent_post(XIAOTAI_INTENT_UI_NEXT, NULL) == XIAOTAI_OK);
    CHECK(barrier() && display->nav_count == UI_NAV_DEPTH);
    (void)ui_scene_step(display, now);
    xiaotai_ui_info_t info;
    CHECK(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.ignored == 4);
    return 0;
}

int main(void)
{
    const struct { const char *name; int (*run)(void); } cases[] = {
        {"focus", focus}, {"digits", digits}, {"hold", hold}, {"swipe", swipe}, {"timer", timer},
        {"stale_press", stale_press}, {"local_hold", local_hold},
    };
    for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
        if (!strcmp(cases[n].name, UI_TEST_CASE)) {
            bind_local_hold = !strcmp(UI_TEST_CASE, "local_hold");
            CHECK(setup(!strcmp(UI_TEST_CASE, "swipe") ? 350U : 0U) == 0);
            const int result = cases[n].run();
            teardown();
            return result;
        }
    }
    CHECK(false);
    return 1;
}
