// SPDX-License-Identifier: MIT
// Replaces legacy: tests/lvgl/test_recognition_ui.c::recognition_ui_0
// Replaces legacy: tests/lvgl/test_recognition_ui.c::recognition_ui_1
/* The presenter and renderer are real; use the recognition owner's existing result fixtures. */
#define main recognition_fake_sink_main
int recognition_fake_sink_main(void);
#include "recognition_test.c"
#undef main

#include "lvgl.h"
#include "src/misc/lv_text_private.h"
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/theme_lcd320.h"

enum { WIDTH = 320, HEIGHT = 240, TIMEOUT = 4000, MAX_CONTROLS = 24 };

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_has_class(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *match = find_label(lv_obj_get_child(obj, (int32_t)i), text);
        if (match) return match;
    }
    return NULL;
}

static void assert_font(lv_obj_t *label, const char *text)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    for (uint32_t next = 0; text[next];) {
        const uint32_t code = lv_text_encoded_next(text, &next);
        if (code < 32) continue;
        lv_font_glyph_dsc_t glyph = {0};
        if (!lv_font_get_glyph_dsc(font, &glyph, code, 0) || glyph.is_placeholder) {
            fprintf(stderr, "Missing real LCD glyph U+%04x in %s\n", (unsigned)code, text);
            assert(0);
        }
    }
}

static void wait_label(const char *text, bool check_font)
{
    ui_display_t *display = ui_display_at(0);
    assert(display);
    for (unsigned ms = 0; ms < TIMEOUT; ms += 10) {
        bool present = false;
        ui_lvgl_enter(display);
        if (display->host.root) {
            lv_obj_t *root = lv_display_get_screen_active(display->host.root);
            lv_obj_t *label = find_label(root, text);
            if (label && lv_obj_is_visible(label)) {
                if (check_font) assert_font(label, text);
                present = true;
            }
        }
        ui_lvgl_leave(display);
        if (present) return;
        xiaotai_delay_ms(10);
    }
    fprintf(stderr, "Not present on real LCD: %.70s\n", text);
    assert(0);
}

static bool overlaps(const xiaotai_ui_control_t *a, const xiaotai_ui_control_t *b)
{
    return a->x < b->x + b->width && b->x < a->x + a->width &&
           a->y < b->y + b->height && b->y < a->y + a->height;
}

static void assert_in_screen(const xiaotai_ui_control_t *item)
{
    assert(item->x >= 0 && item->y >= 0 && item->width && item->height);
    assert(item->x + item->width <= WIDTH && item->y + item->height <= HEIGHT);
}

/* The page's buttons: on the screen, apart from each other, large enough. */
static size_t actions(xiaotai_ui_control_t out[MAX_CONTROLS])
{
    xiaotai_ui_control_t actual[MAX_CONTROLS];
    size_t count, kept = 0;
    assert(xiaotai_ui_controls(0, actual, MAX_CONTROLS, &count, TIMEOUT) == XIAOTAI_OK && count <= MAX_CONTROLS);
    for (size_t i = 0; i < count; ++i) {
        if (actual[i].layer != XIAOTAI_LAYER_PAGE || actual[i].element != XIAOTAI_UI_ELEMENT_ACTION) continue;
        assert_in_screen(&actual[i]);
        for (size_t j = 0; j < kept; ++j) assert(!overlaps(&out[j], &actual[i]));
        out[kept++] = actual[i];
    }
    return kept;
}

static xiaotai_ui_control_t labelled(const char *text)
{
    for (unsigned ms = 0; ms < TIMEOUT; ms += 10) {
        xiaotai_ui_control_t found[MAX_CONTROLS];
        const size_t count = actions(found);
        for (size_t i = 0; i < count; ++i)
            if (!strcmp(found[i].text, text) && !(found[i].state & XIAOTAI_STATE_DISABLED)) return found[i];
        xiaotai_delay_ms(10);
    }
    fprintf(stderr, "No button on the LCD: %s\n", text);
    assert(0);
    return (xiaotai_ui_control_t){0};
}

static void wait_overlay(const char *expected)
{
    for (unsigned ms = 0; ms < TIMEOUT; ms += 10) {
        xiaotai_ui_info_t info;
        assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK);
        if (!strcmp(info.overlay, expected) ||
            (!strcmp(expected, "confirm.") && !strncmp(info.overlay, expected, strlen(expected)))) return;
        xiaotai_delay_ms(10);
    }
    fprintf(stderr, "Expected LCD overlay: %s\n", expected);
    assert(0);
}

static size_t overlay_controls(xiaotai_ui_control_t out[MAX_CONTROLS])
{
    size_t count;
    assert(xiaotai_ui_controls(0, out, MAX_CONTROLS, &count, TIMEOUT) == XIAOTAI_OK);
    assert(count <= MAX_CONTROLS);
    size_t overlays = 0;
    for (size_t i = 0; i < count; ++i) {
        if (out[i].layer != XIAOTAI_LAYER_OVERLAY) continue;
        assert_in_screen(&out[i]);
        for (size_t j = 0; j < overlays; ++j) assert(!overlaps(&out[i], &out[j]));
        out[overlays++] = out[i];
    }
    assert(overlays);
    return overlays;
}

static const xiaotai_ui_control_t *control(const xiaotai_ui_control_t *items, size_t count,
                                          uint8_t element, uint16_t index)
{
    for (size_t i = 0; i < count; ++i)
        if (items[i].element == element && items[i].index == index) return &items[i];
    fprintf(stderr, "Missing overlay control element=%u index=%u\n", (unsigned)element, (unsigned)index);
    assert(0);
    return NULL;
}

static void assert_stepper(const xiaotai_ui_control_t *items, size_t count,
                           bool minimum, bool maximum)
{
    const xiaotai_ui_control_t *minus = control(items, count, XIAOTAI_UI_ELEMENT_FIELD, 0);
    const size_t rest = count - (size_t)(minus - items) - 1U;
    const xiaotai_ui_control_t *plus = control(minus + 1, rest, XIAOTAI_UI_ELEMENT_FIELD, 0);
    assert(minus->x < plus->x);
    assert((bool)(minus->state & XIAOTAI_STATE_DISABLED) == minimum);
    assert((bool)(plus->state & XIAOTAI_STATE_DISABLED) == maximum);
}

static void modal_label(const char *text)
{
    wait_label(text, true);
    ui_display_t *display = ui_display_at(0);
    ui_lvgl_enter(display);
    lv_obj_t *root = lv_display_get_screen_active(display->host.root);
    lv_obj_update_layout(root);
    lv_obj_t *label = find_label(root, text);
    assert(label && lv_obj_is_visible(label));
    lv_area_t actual, parent;
    lv_obj_get_coords(label, &actual);
    lv_obj_get_coords(lv_obj_get_parent(label), &parent);
    assert(actual.x1 >= parent.x1 && actual.x2 <= parent.x2 && actual.y1 >= parent.y1 && actual.y2 <= parent.y2);
    ui_lvgl_leave(display);
}

static void tap(const xiaotai_ui_control_t *control)
{
    if (control->state & XIAOTAI_STATE_DISABLED) {
        xiaotai_ui_info_t info;
        assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK);
        fprintf(stderr, "Disabled LCD control page=%s overlay=%s label=%s element=%u index=%u at (%d,%d)\n",
                info.page, info.overlay, control->text, (unsigned)control->element, (unsigned)control->index,
                (int)control->x, (int)control->y);
    }
    assert(!(control->state & XIAOTAI_STATE_DISABLED));
    assert(xiaotai_ui_tap(0, control->x + control->width / 2,
                          control->y + control->height / 2) == XIAOTAI_OK);
}

static void set_recognition_status(void *ctx)
{
    snapshot = *(const xiaotai_recognition_status_t *)ctx;
    result.active = snapshot.phase == RC(RESULT);
}

static void announce_status(xiaotai_recognition_status_t status)
{
    assert(xiaotai_run(set_recognition_status, &status) == XIAOTAI_OK);
    flush();
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
}

static void exercise(void)
{
    xiaotai_ui_control_t modal[MAX_CONTROLS], button;
    policy = (xiaotai_recognition_policy_t){.valid = true, .scheme_count = 1, .monthly_limit = 100};
    announce_status((xiaotai_recognition_status_t){0});
    wait_label("声音识别", true);
    wait_label("这台设备不能用声音识别。\n请联系服务方开通，再刷新。", true);
    policy.allowed = policy.enabled = policy.effective_enabled = true;
    announce_status((xiaotai_recognition_status_t){0});
    button = labelled("时长");
    tap(&button);
    wait_overlay("record");
    modal_label("10 秒");
    const size_t count = overlay_controls(modal);
    assert_stepper(modal, count, false, false);
    const xiaotai_ui_control_t *minus = control(modal, count, XIAOTAI_UI_ELEMENT_FIELD, 0);
    const xiaotai_ui_control_t *plus = control(minus + 1, count - (size_t)(minus - modal) - 1U, XIAOTAI_UI_ELEMENT_FIELD, 0);
    tap(plus);
    modal_label("15 秒");
    tap(control(modal, count, XIAOTAI_UI_ELEMENT_ACTION, 0));
    wait_overlay("");
    button = labelled("开始录音");
    assert(button.height >= 44);
    tap(&button);
    flush();
    for (unsigned ms = 0; ms < TIMEOUT && !requested.count; ms += 10) xiaotai_delay_ms(10);
    assert(requested.count == 1 && requested.action == RC(ACTION_RECORD) && requested.value == 15);
    receipt(1, RC(ACTION_RECORD), XIAOTAI_OK);

    announce_status((xiaotai_recognition_status_t){.phase = RC(RECORDING), .token = 191,
        .decision_serial = 1, .samples = 16000, .target_samples = 120000, .capture_rate_hz = 8000});
    wait_label("正在听，还有 13 秒", true);
    button = labelled("取消");
    tap(&button);
    for (unsigned ms = 0; ms < TIMEOUT && requested.count < 2; ms += 10) xiaotai_delay_ms(10);
    assert(requested.count == 2 && requested.action == RC(ACTION_CANCEL) && requested.value == 191);
    receipt(2, RC(ACTION_CANCEL), XIAOTAI_OK);

    announce_status((xiaotai_recognition_status_t){.phase = RC(ACCEPTED), .decision_serial = 2, .request_id = "fixture"});
    (void)labelled("问一下结果");
    const xiaotai_recognition_status_t done = {.phase = RC(RESULT), .task_state = RC(TASK_SUCCEEDED),
        .decision_serial = 2, .task_revision = 43, .request_id = "fixture"};
    announce_status(done);
    wait_label("结论：可能有异常，请留意", true);
    wait_label("这是一段识别结果。", true);
    button = labelled("详细");
    tap(&button);
    wait_label("收起", true);
    wait_label("这是一段识别结果。\n标签：哭声、婴儿\n次数：3\n持续：是\n依据：\n  音高：高", true);
    button = labelled("完成");
    tap(&button);
    wait_overlay("confirm.");
    modal_label("完成这次识别");
    modal_label("这次的结果不再保留在设备上，之后可以录新的。");
    assert(overlay_controls(modal) == 2);
    assert(!result.leases);
}

int main(void)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = companion_fixture_init};
    static const char *const after_probe[] = {"probe", NULL};
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init,
        .start = xiaotai_engine_ui_start, .depends = after_probe};
    static const char *const after_ui[] = {"ui", NULL};
    static const xiaotai_module_t theme = {.name = "theme", .init = xiaotai_engine_theme_lcd320_init,
        .depends = after_ui};
    static const xiaotai_module_t *const modules[] = {&probe, &ui, &theme};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&app};

    xiaotai_display_desc_t lcd = ui_fake_desc(WIDTH, HEIGHT, XIAOTAI_PIXEL_RGB565);
    lcd.touch = true;
    lcd.transfer_pixels = WIDTH * 16;
    ui_fake_board_set_displays(&lcd, 1);
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    /* Both LCD320 product boards have external RAM; the generic host port
     * otherwise picks the 1536-byte, no-external-memory view limit. */
    const xiaotai_ui_config_t ui_config = {.layer_bytes = UI_LAYER_BYTES_EXTERNAL};
    assert(xiaotai_ui_configure(&ui_config) == XIAOTAI_OK);
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 3,
        .applications = apps, .application_count = 1, .available_needs = xiaotai_hal_needs()};
    assert(xiaotai_signal_init(&synced) == XIAOTAI_OK);
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    assert(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, PAGE, 0);

    exercise();

    xiaotai_ui_reset();
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&synced);
    assert(!result.leases);
    return 0;
}
