// SPDX-License-Identifier: MIT
// Replaces legacy: tests/lvgl/test_door_preferences.c::door_preferences
// Replaces legacy: tests/lvgl/test_door_preferences.c
#include "templates_wide.h"
#include "door_wide_font_check.h"
#include "ui_test.h"
#include "xiaotai/apps/door.h"
#include "xiaotai/port/os.h"
#include "xiaotai/theme_lcd.h"
#include "lvgl.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

enum { WIDTH = 1024, HEIGHT = 600, WAIT_MS = 5000, MAX_EVENTS = 24 };
enum { ROW_TOKEN = 0x40000017, FIRST_CALL = 51, NEXT_CALL = 52, CONFIRM_TOKEN = 913 };
enum { VIDEO_X = 86, VIDEO_Y = 355, VIDEO_COLOR = 0xf800, VIDEO_GENERATION = 103 };
static xiaotai_signal_t owner_done;
static atomic_uint present_count, accepted, video_pixel, video_paints;
static atomic_uint rendered[2];
static uint8_t view_stamp;
static bool video_pending;
static struct { const char *intent; xiaotai_args_t args; } events[MAX_EVENTS];

static const char *const intents[] = {
    XIAOTAI_INTENT_APP_DOOR_MONITOR, XIAOTAI_INTENT_APP_DOOR_DEVICES, XIAOTAI_INTENT_APP_DOOR_OPEN,
    XIAOTAI_INTENT_APP_DOOR_SELECT, XIAOTAI_INTENT_APP_DOOR_BACK, XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM,
    XIAOTAI_INTENT_APP_DOOR_MUTE, XIAOTAI_INTENT_APP_DOOR_VOLUME, XIAOTAI_INTENT_APP_DOOR_VOLUME_SET,
    XIAOTAI_INTENT_SYSTEM_NETWORK, "door.call.accept", "door.call.reject", "door.call.hangup",
};

static void record(const xiaotai_args_t *args, void *ctx)
{
    assert(xiaotai_on_owner());
    const unsigned n = atomic_load(&accepted);
    assert(n < MAX_EVENTS);
    events[n].intent = ctx;
    events[n].args = *args;
    atomic_store(&accepted, n + 1);
}

static xiaotai_err_t panel_present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    (void)self;
    assert(pixels && area->width && area->height && area->x + area->width <= WIDTH && area->y + area->height <= HEIGHT);
    if (VIDEO_X >= area->x && VIDEO_X < area->x + area->width && VIDEO_Y >= area->y && VIDEO_Y < area->y + area->height) {
        const uint8_t *p = (const uint8_t *)pixels + ((VIDEO_Y - area->y) * area->width + VIDEO_X - area->x) * 2U;
        atomic_store(&video_pixel, p[0] | ((unsigned)p[1] << 8U));
    }
    atomic_fetch_add(&present_count, 1);
    return XIAOTAI_OK;
}

static int match(const xiaotai_display_desc_t *d)
{
    return d->pixel == XIAOTAI_PIXEL_RGB565 ? 100 : 0;
}

static xiaotai_err_t start_theme(const xiaotai_ui_host_t *host, void **state)
{
    return xiaotai_theme_lcd_start(host, NULL, state);
}

static void render(void *state, const xiaotai_ui_scene_t *scene)
{
    xiaotai_theme_lcd_render(state, scene);
    atomic_store(&rendered[XIAOTAI_LAYER_PAGE], scene->page.present ? scene->page.view.progress : 0);
    atomic_store(&rendered[XIAOTAI_LAYER_OVERLAY], scene->overlay.present ? scene->overlay.view.progress : 0);
}

static xiaotai_err_t init(void)
{
    static const xiaotai_theme_t theme = {
        .name = "door-test", .backend = XIAOTAI_UI_BACKEND_LVGL, .match = match, .start = start_theme,
        .render = render, .tick = xiaotai_theme_lcd_tick, .sleep = xiaotai_theme_lcd_sleep,
        .controls = xiaotai_theme_lcd_controls, .stop = xiaotai_theme_lcd_stop, .stack_bytes = XIAOTAI_THEME_LCD_STACK_BYTES,
    };
    xiaotai_err_t err = xiaotai_ui_theme_register(&theme);
    if (err == XIAOTAI_OK) err = door_templates_register();
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(intents) / sizeof(*intents); ++i)
        err = xiaotai_intent_handle(intents[i], record, (void *)intents[i]);
    return err;
}

static int setup(void)
{
    xiaotai_display_desc_t d = ui_fake_desc(WIDTH, HEIGHT, XIAOTAI_PIXEL_RGB565);
    d.transfer_pixels = WIDTH * 16U;
    d.touch = true;
    ui_fake_board_set_displays(&d, 1);
    ((xiaotai_display_t *)ui_fake_display(0))->present = panel_present;
    CHECK(xiaotai_signal_init(&owner_done) == XIAOTAI_OK && xiaotai_hal_init() == XIAOTAI_OK);
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "door-test", .init = init};
    static const xiaotai_module_t *const modules[] = {&ui};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t fw = {.modules = modules, .module_count = 1, .applications = apps, .application_count = 1};
    CHECK(xiaotai_framework_start(&fw) == XIAOTAI_OK);
    size_t n = 1;
    CHECK(xiaotai_ui_controls(0, NULL, 0, &n, WAIT_MS) == XIAOTAI_OK && !n);
    return 0;
}

static void show_on_owner(void *ctx)
{
    xiaotai_view_show(ctx);
    xiaotai_signal_give(&owner_done);
}

static void clear_on_owner(void *ctx)
{
    (void)ctx;
    xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, NULL);
    xiaotai_signal_give(&owner_done);
}

static void run_owner(void (*work)(void *), void *ctx)
{
    assert(xiaotai_run(work, ctx) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_done, WAIT_MS));
}

static void stage(const xiaotai_view_t *view)
{
    xiaotai_ui_info_t info;
    xiaotai_view_t staged = *view;
    /* Unlike inspect(), this stamp witnesses the rendered scene, not the
     * newer staged view that may arrive during a periodic frame. */
    assert(staged.layer <= XIAOTAI_LAYER_OVERLAY && view_stamp < 100);
    staged.progress = ++view_stamp; /* ignored by every door template */
    run_owner(show_on_owner, &staged);
    for (unsigned i = 0; i < WAIT_MS; ++i) {
        assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && !info.present_fault && !info.present_errors);
        if (atomic_load(&rendered[staged.layer]) == staged.progress) return;
        xiaotai_delay_ms(1);
    }
    fprintf(stderr, "Failed to render %s; last page=%s, overlay=%s\n", view->id, info.page, info.overlay);
    assert(false);
}

static xiaotai_ui_capture_t capture(void)
{
    xiaotai_ui_capture_t frame;
    assert(xiaotai_ui_capture(0, &frame, WAIT_MS) == XIAOTAI_OK);
    assert(frame.width == WIDTH && frame.height == HEIGHT && frame.pixel == XIAOTAI_PIXEL_RGB565 && frame.size == WIDTH * HEIGHT * 2U);
    return frame;
}

static uint16_t pixel(const xiaotai_ui_capture_t *frame, unsigned x, unsigned y)
{
    const uint8_t *p = &frame->pixels[(y * WIDTH + x) * 2U];
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8U));
}

static unsigned ink(const xiaotai_ui_capture_t *f, unsigned x, unsigned y, unsigned w, unsigned h)
{
    unsigned count = 0;
    for (unsigned dy = 0; dy < h; ++dy) {
        for (unsigned dx = 0; dx < w; ++dx) {
            const uint16_t c = pixel(f, x + dx, y + dy);
            if ((c >> 11U) > 22U && ((c >> 5U) & 63U) > 45U && (c & 31U) > 22U) ++count;
        }
    }
    return count;
}

static uint32_t region_digest(const xiaotai_ui_capture_t *f, unsigned x, unsigned y, unsigned w, unsigned h)
{
    uint32_t hash = 2166136261U;
    for (unsigned dy = 0; dy < h; ++dy) {
        for (unsigned dx = 0; dx < w; ++dx) hash = (hash ^ pixel(f, x + dx, y + dy)) * 16777619U;
    }
    return hash;
}

static xiaotai_ui_control_t control(uint8_t layer, uint8_t element, uint16_t index, const char *intent)
{
    xiaotai_ui_control_t entries[24];
    size_t n = 0;
    assert(xiaotai_ui_controls(0, entries, sizeof(entries) / sizeof(*entries), &n, WAIT_MS) == XIAOTAI_OK);
    assert(n <= sizeof(entries) / sizeof(*entries));
    for (size_t i = 0; i < n; ++i) {
        xiaotai_ui_control_t *c = &entries[i];
        if (c->layer != layer || c->element != element || c->index != index) continue;
        if (strcmp(c->intent, intent)) {
            fprintf(stderr, "Control %s has %s instead (layer %u element %u index %u)\n", intent, c->intent, layer, element, index);
            assert(false);
        }
        assert(c->x >= 0 && c->y >= 0 && c->width && c->height && c->x + c->width <= WIDTH && c->y + c->height <= HEIGHT);
        return *c;
    }
    fprintf(stderr, "Missing control %s (layer %u element %u index %u)\n", intent, layer, element, index);
    assert(false);
    return (xiaotai_ui_control_t){0};
}

static void wait_events(unsigned count, const char *intent)
{
    for (unsigned i = 0; i < WAIT_MS; ++i) {
        if (atomic_load(&accepted) >= count) return;
        xiaotai_delay_ms(1);
    }
    xiaotai_ui_info_t info;
    assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK);
    fprintf(stderr, "%s: intents %u/%u, page=%s overlay=%s, posted=%u rejected=%u ignored=%u\n",
            intent, atomic_load(&accepted), count, info.page, info.overlay, info.posted, info.rejected, info.ignored);
    assert(!"expected intent not dispatched");
}

static bool tap(int16_t x, int16_t y, const char *name, int32_t value)
{
    const unsigned n = atomic_load(&accepted);
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    for (unsigned i = 0; i < 300 && err == XIAOTAI_ERR_INVALID_STATE; ++i) {
        if (i) xiaotai_delay_ms(1);
        err = xiaotai_ui_tap(0, x, y);
    }
    assert(err == XIAOTAI_OK);
    wait_events(n + 1, name);
    return atomic_load(&accepted) == n + 1 && !strcmp(events[n].intent, name) && events[n].args.value == value;
}

static bool tap_control(xiaotai_ui_control_t c, int32_t value)
{
    return tap(c.x + c.width / 2, c.y + c.height / 2, c.intent, value);
}

static int home(void)
{
    const xiaotai_view_indicator_t wifi = {.icon = XIAOTAI_ICON_WIFI, .level = 3, .intent = XIAOTAI_INTENT_SYSTEM_NETWORK};
    const xiaotai_view_action_t actions[] = {
        {.label = "入户门", .intent = XIAOTAI_INTENT_APP_DOOR_DEVICES},
        {.label = "查看门口", .intent = XIAOTAI_INTENT_APP_DOOR_MONITOR},
        {.label = "开门", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN},
    };
    const xiaotai_app_door_home_view_t status = {.state = XIAOTAI_APP_DOOR_STATION_ONLINE};
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_HOME,
        .custom = XIAOTAI_APP_DOOR_TEMPLATE_HOME, .title = "小钛可视门铃", .status = "门外机在线",
        .custom_data = &status, .custom_size = sizeof(status), .actions = actions, .action_count = 3,
        .indicators = &wifi, .indicator_count = 1};
    stage(&v);
    const xiaotai_ui_control_t monitor = control(XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ACTION, 1, actions[1].intent);
    CHECK(monitor.x == 312 && monitor.y == 440 && monitor.width == 176 && monitor.height == 124);
    const xiaotai_ui_control_t network = control(XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_INDICATOR, 0, wifi.intent);
    CHECK(network.x == 948 && network.y == 24);
    xiaotai_ui_capture_t f = capture();
    CHECK(ink(&f, 35, 30, 220, 58) > 50 && ink(&f, 230, 126, 560, 150) > 100);
    CHECK(pixel(&f, 83, 400) != pixel(&f, 938, 400));
    xiaotai_ui_capture_free(&f);
    CHECK(tap_control(monitor, 0) && tap_control(network, 0));
    return 0;
}

static int devices(void)
{
    const xiaotai_view_action_t back = {.label = "返回", .intent = XIAOTAI_INTENT_APP_DOOR_BACK, .role = XIAOTAI_ROLE_BACK};
    const xiaotai_view_item_t rows[] = {
        {.label = "测试门口", .detail = "在线", .intent = XIAOTAI_INTENT_APP_DOOR_SELECT, .value = ROW_TOKEN,
         .state = XIAOTAI_STATE_CHECKED},
        {.label = "入户门", .detail = "离线", .intent = XIAOTAI_INTENT_APP_DOOR_SELECT, .value = ROW_TOKEN + 1},
    };
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_DEVICES, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DEVICES,
        .title = "选择门外机", .items = rows, .item_count = 2, .actions = &back, .action_count = 1};
    stage(&v);
    const xiaotai_ui_control_t row = control(XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ITEM, 0, rows[0].intent);
    CHECK(row.x == 32 && row.y == 112 && row.height == 88 && (row.state & XIAOTAI_STATE_CHECKED));
    xiaotai_ui_capture_t f = capture();
    CHECK(ink(&f, 49, 115, 180, 33) > 30 && pixel(&f, 60, 179) != pixel(&f, 60, 201));
    xiaotai_ui_capture_free(&f);
    CHECK(tap_control(row, ROW_TOKEN));
    return 0;
}

static int monitor(void)
{
    const xiaotai_view_action_t actions[] = {
        {.label = "返回", .intent = XIAOTAI_INTENT_APP_DOOR_BACK, .role = XIAOTAI_ROLE_BACK},
        {.label = "开门", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN},
    };
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_MONITOR, .custom = XIAOTAI_APP_DOOR_TEMPLATE_MONITOR,
        .title = "查看门口", .subtitle = "测试门口", .status = "未连接", .text = "门外机离线", .actions = actions, .action_count = 2};
    stage(&v);
    const xiaotai_ui_control_t open = control(XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ACTION, 1, actions[1].intent);
    CHECK(open.x == 568 && open.y == 466 && open.width == 112 && open.height == 116);
    xiaotai_ui_capture_t f = capture();
    CHECK(ink(&f, 30, 20, 225, 49) > 30 && ink(&f, 370, 244, 278, 48) > 30);
    xiaotai_ui_capture_free(&f);
    CHECK(tap_control(open, 0));
    return 0;
}

static int call(void)
{
    const xiaotai_view_action_t ringing[] = {
        {.label = "拒绝", .intent = "door.call.reject", .value = FIRST_CALL},
        {.label = "接听", .intent = "door.call.accept", .value = FIRST_CALL},
    };
    xiaotai_app_door_call_view_t state = {.phase = XIAOTAI_APP_DOOR_CALL_RINGING};
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_CALL, .custom = XIAOTAI_APP_DOOR_TEMPLATE_CALL,
        .title = "门口有人", .status = "待接听", .custom_data = &state, .custom_size = sizeof(state),
        .actions = ringing, .action_count = 2};
    stage(&v);
    const xiaotai_ui_control_t accept = control(XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ACTION, 1, ringing[1].intent);
    CHECK(accept.x == 568 && accept.y == 466);
    xiaotai_ui_capture_t f = capture();
    /* The accept plate (a solid circle from x 588) stands out from the sheet beside it. */
    CHECK(ink(&f, 30, 20, 248, 49) > 30 && pixel(&f, 600, 492) != pixel(&f, 560, 492) &&
          pixel(&f, 600, 492) == pixel(&f, 650, 492));
    xiaotai_ui_capture_free(&f);
    CHECK(tap_control(accept, FIRST_CALL));

    xiaotai_view_action_t active[] = {
        {.label = "静音", .intent = XIAOTAI_INTENT_APP_DOOR_MUTE, .value = FIRST_CALL},
        {.label = "音量", .intent = XIAOTAI_INTENT_APP_DOOR_VOLUME, .value = FIRST_CALL},
        {.label = "开门", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN, .value = FIRST_CALL},
        {.label = "挂断", .intent = "door.call.hangup", .value = FIRST_CALL},
    };
    state.phase = XIAOTAI_APP_DOOR_CALL_ACTIVE;
    v.title = "正在通话";
    v.status = NULL;
    v.timer_mode = XIAOTAI_TIMER_ELAPSED;
    v.timer_s = 20;
    v.actions = active;
    v.action_count = 4;
    stage(&v);
    const xiaotai_ui_control_t hangup = control(XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ACTION, 3, active[3].intent);
    CHECK(hangup.x == 696 && hangup.y == 466);
    const int16_t x = hangup.x + hangup.width / 2, y = hangup.y + hangup.height / 2;
    const unsigned sent = atomic_load(&accepted);
    /* Require the old button's actual pressed redraw before replacing it. */
    f = capture();
    const uint16_t old = pixel(&f, 705, 474);
    xiaotai_ui_capture_free(&f);
    CHECK(xiaotai_ui_pointer(0, x, y, true) == XIAOTAI_OK);
    bool pressed = false;
    for (unsigned i = 0; !pressed && i < 30; ++i) {
        xiaotai_delay_ms(10);
        f = capture();
        pressed = pixel(&f, 705, 474) != old;
        xiaotai_ui_capture_free(&f);
    }
    CHECK(pressed);
    active[3].value = NEXT_CALL;
    stage(&v);
    CHECK(xiaotai_ui_pointer(0, x, y, false) == XIAOTAI_OK);
    CHECK(tap_control(control(XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ACTION, 3, active[3].intent), NEXT_CALL));
    CHECK(atomic_load(&accepted) == sent + 1);
    return 0;
}

static int dialogs(void)
{
    const char *const confirm = XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM;
    const xiaotai_view_action_t actions[] = {
        {.label = "取消", .intent = confirm, .role = XIAOTAI_ROLE_BACK},
        {.label = "确认开门", .intent = confirm, .role = XIAOTAI_ROLE_PRIMARY, .value = CONFIRM_TOKEN},
    };
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_CUSTOM, .layer = XIAOTAI_LAYER_OVERLAY, .id = XIAOTAI_APP_DOOR_OVERLAY_OPEN,
        .custom = XIAOTAI_APP_DOOR_TEMPLATE_DIALOG, .title = "确认开门？", .subtitle = "入户门 · 门锁",
        .text = "开门后，门口访客将可进入。", .actions = actions, .action_count = 2};
    stage(&v);
    const xiaotai_ui_control_t primary = control(XIAOTAI_LAYER_OVERLAY, XIAOTAI_UI_ELEMENT_ACTION, 1, confirm);
    CHECK(primary.x >= 524 && primary.x <= 526 && primary.y >= 370 && primary.y <= 372 && primary.height == 64);
    xiaotai_ui_capture_t f = capture();
    CHECK(ink(&f, 300, 147, 380, 60) > 40 && pixel(&f, 20, 120) != pixel(&f, 242, 140));
    xiaotai_ui_capture_free(&f);
    /* The original modal swallows backdrop touches; only Cancel or Confirm act. */
    CHECK(xiaotai_ui_tap(0, 33, 33) == XIAOTAI_OK);
    CHECK(tap_control(primary, CONFIRM_TOKEN));
    CHECK(tap_control(control(XIAOTAI_LAYER_OVERLAY, XIAOTAI_UI_ELEMENT_ACTION, 0, confirm), 0));

    const xiaotai_view_action_t back = {.label = "返回", .intent = XIAOTAI_INTENT_APP_DOOR_BACK, .role = XIAOTAI_ROLE_BACK};
    xiaotai_app_door_result_view_t result = {.opened = false};
    v.id = XIAOTAI_APP_DOOR_OVERLAY_RESULT;
    v.title = "暂时无法开门";
    v.text = "门锁尚未配置，未发送开门指令";
    v.subtitle = NULL;
    v.custom_data = &result;
    v.custom_size = sizeof(result);
    v.actions = &back;
    v.action_count = 1;
    stage(&v);
    f = capture();
    CHECK(ink(&f, 375, 218, 274, 70) > 30);
    const uint16_t failed_badge = pixel(&f, 510, 172);
    xiaotai_ui_capture_free(&f);
    result.opened = true;
    v.title = "门已打开";
    v.text = NULL;
    stage(&v);
    f = capture();
    CHECK(failed_badge != pixel(&f, 510, 172));
    xiaotai_ui_capture_free(&f);
    CHECK(tap_control(control(XIAOTAI_LAYER_OVERLAY, XIAOTAI_UI_ELEMENT_ACTION, 0, back.intent), 0));

    xiaotai_view_field_t field = {.intent = XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, .kind = XIAOTAI_FIELD_SLIDER,
        .min = 0, .max = 100, .value = 20, .step = 10, .flags = XIAOTAI_FIELD_LIVE};
    v.id = XIAOTAI_APP_DOOR_OVERLAY_VOLUME;
    v.title = "通话音量";
    v.custom_data = NULL;
    v.custom_size = 0;
    v.fields = &field;
    v.field_count = 1;
    stage(&v);
    const xiaotai_ui_control_t slider = control(XIAOTAI_LAYER_OVERLAY, XIAOTAI_UI_ELEMENT_FIELD, 0, field.intent);
    CHECK(slider.x >= 564 && slider.x <= 566 && slider.y >= 410 && slider.y <= 412 && slider.width == 304);
    f = capture();
    CHECK(ink(&f, 589, 305, 247, 62) > 30);
    const unsigned thumb_x = slider.x, thumb_y = slider.y - 12U, label_x = slider.x + slider.width + 3U;
    const unsigned label_y = slider.y - 16U;
    const uint32_t thumb_before = region_digest(&f, thumb_x, thumb_y, slider.width, 28);
    const uint32_t label_before = region_digest(&f, label_x, label_y, 76, 40);
    xiaotai_ui_capture_free(&f);
    const unsigned n = atomic_load(&accepted);
    CHECK(xiaotai_ui_tap(0, slider.x + 255, slider.y + 2) == XIAOTAI_OK);
    wait_events(n + 2, field.intent);
    CHECK(atomic_load(&accepted) == n + 2 && !strcmp(events[n].intent, field.intent) && !strcmp(events[n + 1].intent, field.intent));
    CHECK(events[n].args.value >= 80 && events[n].args.value <= 90 && events[n].args.value == events[n + 1].args.value);
    CHECK(!strcmp(events[n].args.text, XIAOTAI_ARG_LIVE) && !events[n + 1].args.text[0]);
    field.value = events[n].args.value;
    stage(&v);
    f = capture();
    CHECK(region_digest(&f, thumb_x, thumb_y, slider.width, 28) != thumb_before);
    CHECK(region_digest(&f, label_x, label_y, 76, 40) != label_before);
    xiaotai_ui_capture_free(&f);
    /* The owner re-renders the saved value after refusing a preview. */
    field.value = 20;
    stage(&v);
    f = capture();
    CHECK(region_digest(&f, thumb_x, thumb_y, slider.width, 28) == thumb_before);
    CHECK(region_digest(&f, label_x, label_y, 76, 40) == label_before);
    xiaotai_ui_capture_free(&f);
    CHECK(atomic_load(&accepted) == n + 2);
    CHECK(tap_control(control(XIAOTAI_LAYER_OVERLAY, XIAOTAI_UI_ELEMENT_ACTION, 0, back.intent), 0));
    run_owner(clear_on_owner, NULL);
    return 0;
}

static bool new_video(void *ctx)
{
    (void)ctx;
    if (!video_pending) return false;
    video_pending = false;
    return true;
}

static bool paint_video(void *ctx, const xiaotai_ui_surface_t *s,
                        uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    (void)ctx;
    assert(x == 0 && y == 0 && w == WIDTH && h == HEIGHT && s->pixel == XIAOTAI_PIXEL_RGB565 && !s->swap_bytes);
    atomic_fetch_add(&video_paints, 1);
    if (VIDEO_Y < s->y || VIDEO_Y >= s->y + s->height || VIDEO_X < s->x || VIDEO_X >= s->x + s->width) return false;
    uint8_t *p = s->pixels + (VIDEO_Y - s->y) * s->stride + (VIDEO_X - s->x) * 2U;
    p[0] = (uint8_t)VIDEO_COLOR;
    p[1] = (uint8_t)(VIDEO_COLOR >> 8);
    return true;
}

static void attach_on_owner(void *ctx)
{
    const xiaotai_ui_video_source_t source = {.pending = new_video, .paint = paint_video};
    *(xiaotai_err_t *)ctx = xiaotai_ui_video_set(0, &source, VIDEO_GENERATION);
    xiaotai_signal_give(&owner_done);
}

static void detach_on_owner(void *ctx)
{
    (void)ctx;
    xiaotai_ui_video_clear(0, VIDEO_GENERATION);
    xiaotai_signal_give(&owner_done);
}

static int video(void)
{
    const xiaotai_app_door_call_view_t state = {.phase = XIAOTAI_APP_DOOR_CALL_ACTIVE};
    xiaotai_view_t v = {.kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_CALL,
        .custom = XIAOTAI_APP_DOOR_TEMPLATE_CALL, .title = "正在通话", .custom_data = &state, .custom_size = sizeof(state)};
    stage(&v);
    video_pending = true;
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    run_owner(attach_on_owner, &result);
    CHECK(result == XIAOTAI_OK && !atomic_load(&video_paints));
    v.flags = XIAOTAI_VIEW_VIDEO;
    stage(&v);
    for (unsigned i = 0; i < WAIT_MS && atomic_load(&video_pixel) != VIDEO_COLOR; ++i) xiaotai_delay_ms(1);
    CHECK(atomic_load(&video_pixel) == VIDEO_COLOR && atomic_load(&video_paints) > 0);
    v.flags = 0;
    stage(&v);
    for (unsigned i = 0; i < WAIT_MS && atomic_load(&video_pixel) == VIDEO_COLOR; ++i) xiaotai_delay_ms(1);
    CHECK(atomic_load(&video_pixel) != VIDEO_COLOR);
    const unsigned painted = atomic_load(&video_paints);
    run_owner(detach_on_owner, NULL);
    xiaotai_ui_capture_t f = capture();
    CHECK(pixel(&f, VIDEO_X, VIDEO_Y) != VIDEO_COLOR && atomic_load(&video_paints) == painted);
    xiaotai_ui_capture_free(&f);
    v.id = XIAOTAI_APP_DOOR_PAGE_DEVICES;
    v.custom = XIAOTAI_APP_DOOR_TEMPLATE_DEVICES;
    v.title = "选择门外机";
    v.custom_data = NULL;
    v.custom_size = 0;
    stage(&v);
    run_owner(attach_on_owner, &result);
    CHECK(result == XIAOTAI_ERR_NOT_SUPPORTED);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "fonts")) {
        lv_init();
        door_wide_verify_fonts();
        lv_deinit();
        return 0;
    }
    RUN(setup);
    RUN(home);
    RUN(devices);
    RUN(monitor);
    RUN(dialogs);
    RUN(call);
    RUN(video);
    CHECK(atomic_load(&present_count) > 0);
    xiaotai_framework_reset();
    xiaotai_ui_reset();
    xiaotai_signal_deinit(&owner_done);
    return 0;
}
