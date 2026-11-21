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
enum { ACQUIRE_TIMEOUT = 1, DETAIL_NO_MEM = 2, EMOJI_SUMMARY = 4 };

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

static void actions(xiaotai_ui_control_t out[7])
{
    xiaotai_ui_control_t actual[MAX_CONTROLS];
    size_t count;
    assert(xiaotai_ui_controls(0, actual, MAX_CONTROLS, &count, TIMEOUT) == XIAOTAI_OK);
    assert(count <= MAX_CONTROLS);
    unsigned mask = 0;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_ui_control_t *item = &actual[i];
        if (item->layer != XIAOTAI_LAYER_PAGE || item->element != XIAOTAI_UI_ELEMENT_ACTION) continue;
        assert(item->index < 7 && !(mask & (1U << item->index)));
        assert_in_screen(item);
        out[item->index] = *item;
        mask |= 1U << item->index;
    }
    assert(mask == 0x7f);
    for (unsigned i = 0; i < 7; ++i) for (unsigned j = i + 1; j < 7; ++j) {
        if (overlaps(&out[i], &out[j])) fprintf(stderr, "Visible actions overlap: %u/%u\n", i, j);
        assert(!overlaps(&out[i], &out[j]));
    }
}

static void wait_page_action_enabled(xiaotai_ui_control_t out[7], unsigned index)
{
    /* Owner barriers do not drain the display's coalesced same-page updates. */
    const int64_t deadline = xiaotai_time_us() + TIMEOUT * 1000;
    do {
        actions(out);
        if (!(out[index].state & XIAOTAI_STATE_DISABLED)) return;
        xiaotai_delay_ms(10);
    } while (xiaotai_time_us() < deadline);
    fprintf(stderr, "LCD action did not re-enable: %s\n", out[index].text);
    assert(0);
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

static void set_post_error(void *ctx)
{
    post_error = *(const xiaotai_err_t *)ctx;
}

static void inject_post_error(xiaotai_err_t error)
{
    assert(xiaotai_run(set_post_error, &error) == XIAOTAI_OK);
    flush();
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

static void set_result_read_faults(void *ctx)
{
    const unsigned mask = *(const unsigned *)ctx;
    timeout_result = (mask & ACQUIRE_TIMEOUT) != 0;
    fail_detail = (mask & DETAIL_NO_MEM) != 0;
    long_summary = (mask & EMOJI_SUMMARY) != 0;
}

static void result_read_faults(unsigned mask)
{
    assert(xiaotai_run(set_result_read_faults, &mask) == XIAOTAI_OK);
    flush();
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
}

static uint32_t frame_hash(void)
{
    xiaotai_ui_capture_t frame;
    assert(xiaotai_ui_capture(0, &frame, TIMEOUT) == XIAOTAI_OK);
    assert(frame.width == WIDTH && frame.height == HEIGHT && frame.pixel == XIAOTAI_PIXEL_RGB565 &&
           frame.size == WIDTH * HEIGHT * 2U);
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < frame.size; ++i) hash = (hash ^ frame.pixels[i]) * 16777619U;
    xiaotai_ui_capture_free(&frame);
    return hash;
}

static void exercise_modals(void)
{
    const char *const seconds = "companion.recognition.seconds";
    xiaotai_ui_control_t page[7], modal[MAX_CONTROLS];
    announce_status((xiaotai_recognition_status_t){0});
    wait_label("等待结果", true);
    actions(page);
    tap(&page[2]);
    wait_overlay("record");
    modal_label("录音将上传到识别服务\n可能消耗账户额度");
    modal_label("10 秒");
    size_t count = overlay_controls(modal);
    assert(count == 4);
    assert_stepper(modal, count, false, false);
    const xiaotai_ui_control_t *minus = control(modal, count, XIAOTAI_UI_ELEMENT_FIELD, 0);
    const xiaotai_ui_control_t *plus = control(minus + 1, count - (size_t)(minus - modal) - 1U,
                                               XIAOTAI_UI_ELEMENT_FIELD, 0);
    assert(minus->x < plus->x);
    tap(plus);
    modal_label("11 秒");
    tap(minus);
    modal_label("10 秒");
    send(seconds, NULL, 1);
    modal_label("1 秒");
    count = overlay_controls(modal);
    assert_stepper(modal, count, true, false);
    send(seconds, NULL, 60);
    modal_label("60 秒");
    count = overlay_controls(modal);
    assert_stepper(modal, count, false, true);
    send(seconds, NULL, 61);
    modal_label("60 秒");
    send(seconds, NULL, 10);
    modal_label("10 秒");
    count = overlay_controls(modal);
    inject_post_error(XIAOTAI_ERR_NO_MEM);
    tap(control(modal, count, XIAOTAI_UI_ELEMENT_ACTION, 1));
    wait_label("操作未提交，请检查状态", true);
    wait_overlay("record");
    modal_label("10 秒");
    flush();
    assert(requested.count == 1 && requested.action == RC(ACTION_RECORD) && requested.value == 10);
    inject_post_error(XIAOTAI_OK);
    tap(control(modal, count, XIAOTAI_UI_ELEMENT_ACTION, 1));
    wait_overlay("");
    flush();
    assert(requested.count == 2 && requested.action == RC(ACTION_RECORD) && requested.value == 10);
    receipt(1, RC(ACTION_RECORD), XIAOTAI_ERR_INVALID_STATE);
    wait_overlay("");

    announce_status((xiaotai_recognition_status_t){.phase = RC(RECORDING), .token = 191,
        .decision_serial = 1, .samples = 16000, .target_samples = 80000, .capture_rate_hz = 8000});
    wait_label("正在录音 2 / 10 秒", true);
    actions(page);
    assert(page[2].state & XIAOTAI_STATE_DISABLED);
    tap(&page[4]);
    wait_overlay("task");
    modal_label("停止");
    count = overlay_controls(modal);
    tap(control(modal, count, XIAOTAI_UI_ELEMENT_ITEM, 0));
    wait_overlay("");
    flush();
    assert(requested.count == 3 && requested.action == RC(ACTION_CANCEL) && requested.value == 191);
    receipt(2, RC(ACTION_CANCEL), XIAOTAI_OK);

    xiaotai_recognition_status_t done = {.phase = RC(RESULT), .task_state = RC(TASK_SUCCEEDED),
        .decision_serial = 2, .task_revision = 43, .request_id = "fixture"};
    announce_status(done);
    wait_label("任务完成", true);
    actions(page);
    tap(&page[4]);
    wait_overlay("task");
    count = overlay_controls(modal);
    assert(control(modal, count, XIAOTAI_UI_ELEMENT_ITEM, 0)->state & XIAOTAI_STATE_DISABLED);
    tap(control(modal, count, XIAOTAI_UI_ELEMENT_ITEM, 3));
    wait_overlay("confirm.");
    modal_label("归档任务");
    modal_label("归档后可开始新的录音\n本机不保留这次结果");
    count = overlay_controls(modal);
    assert(count == 2);
    ++done.task_revision;
    announce_status(done);
    wait_overlay("confirm.");
    tap(control(modal, count, XIAOTAI_UI_ELEMENT_ACTION, 0));
    wait_overlay("");
    flush();
    assert(requested.count == 4 && requested.action == RC(ACTION_ARCHIVE) && requested.revision == 43);
    assert(!strcmp(requested.request, "fixture") && !result.leases);
    receipt(3, RC(ACTION_ARCHIVE), XIAOTAI_ERR_INVALID_STATE);

    wait_page_action_enabled(page, 4);
    tap(&page[4]);
    wait_overlay("task");
    count = overlay_controls(modal);
    tap(control(modal, count, XIAOTAI_UI_ELEMENT_ITEM, 2));
    wait_overlay("confirm.");
    modal_label("确认略过当前这一条结果？");
    count = overlay_controls(modal);
    tap(control(modal, count, XIAOTAI_UI_ELEMENT_ACTION, 0));
    wait_overlay("");
    flush();
    assert(requested.count == 5 && requested.action == RC(ACTION_DISMISS));
    assert(!strcmp(requested.request, "fixture") && requested.item[0] && !result.leases);
    receipt(4, RC(ACTION_DISMISS), XIAOTAI_OK);

    announce_status((xiaotai_recognition_status_t){.decision_serial = 4});
    wait_label("等待结果", true);
    /* Exercise release on page exit repeatedly without 100 synthetic gestures. */
    for (unsigned i = 0; i < 100; ++i) {
        send(COMMAND, "record", 0);
        wait_overlay("record");
        send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
        wait_overlay("");
        assert(!result.leases && requested.count == 5);
        send(XIAOTAI_INTENT_APP_COMPANION_SHOW, PAGE, 0);
    }
    fprintf(stdout, "Real lcd320 consent, task, archive/skip and 100 modal exits passed\n");
}

static void exercise_results(void)
{
    xiaotai_ui_control_t slots[7];
    wait_label("类型 1 / 8", true);
    wait_label("任务完成", true);
    char long_text[1600] = "分析成功 / 语音失败\n模型提示异常，待确认\n";
    const size_t prefix = strlen(long_text);
    for (unsigned i = 0; i < 500; ++i) memcpy(long_text + prefix + i * 3U, "声", 3U);
    wait_label(long_text, false);
    actions(slots);
    assert(slots[0].state & XIAOTAI_STATE_DISABLED);
    const uint32_t summary = frame_hash();
    tap(&slots[3]);
    wait_label("详情 · 第 1 页", true);
    static char expected[511];
    memset(expected, 'a', 510);
    wait_label(expected, false);
    actions(slots);
    const uint32_t first = frame_hash();
    assert(summary != first && (slots[0].state & XIAOTAI_STATE_DISABLED));
    tap(&slots[1]);
    wait_label("详情 · 第 2 页", true);
    wait_label("中字尾", true);
    actions(slots);
    const uint32_t second = frame_hash();
    assert(second != first && (slots[1].state & XIAOTAI_STATE_DISABLED));
    result_read_faults(ACQUIRE_TIMEOUT);
    wait_label("详情 · 第 2 页", true);
    wait_label("中字尾", true);
    assert(!result.leases);
    result_read_faults(0);
    wait_page_action_enabled(slots, 0);
    tap(&slots[0]);
    wait_label("详情 · 第 1 页", true);
    wait_label(expected, false);
    actions(slots);
    tap(&slots[3]);
    wait_label("类型 1 / 8", true);
    result_read_faults(DETAIL_NO_MEM);
    actions(slots);
    tap(&slots[3]);
    wait_label("详细结果暂不可用", true);
    wait_label("类型 1 / 8", true);
    assert(!result.leases);
    result_read_faults(0);
    for (unsigned i = 2; i <= 8; ++i) {
        char title[32];
        snprintf(title, sizeof(title), "类型 %u / 8", i);
        actions(slots);
        tap(&slots[1]);
        wait_label(title, true);
        if (i == 2) {
            wait_label("分析成功 / 语音已就绪\n本条返回语音", true);
            actions(slots);
            assert(slots[3].state & XIAOTAI_STATE_DISABLED);
        } else if (i == 3) {
            wait_label("分析失败 / 仅文字\n尚无文字结果", true);
        }
    }
    actions(slots);
    assert(slots[1].state & XIAOTAI_STATE_DISABLED);
    xiaotai_recognition_status_t held = snapshot;
    ++held.task_revision;
    announce_status(held);
    wait_label("类型 8 / 8", true);
    assert(!result.leases);
    result_read_faults(EMOJI_SUMMARY);
    static char emoji[2001];
    for (size_t i = 0; i < 2000U; i += 4U) memcpy(emoji + i, "\xf0\x9f\x8e\xa7", 4U);
    wait_label(emoji, false);
    actions(slots);
    tap(&slots[3]);
    wait_label("详情 · 第 1 页", true);
    result_read_faults(ACQUIRE_TIMEOUT);
    held.task_revision = 0;
    held.request_id[0] = '\0';
    announce_status(held);
    wait_label("暂无识别结果", true);
    assert(!result.leases);
    announce_status((xiaotai_recognition_status_t){0});
    result_read_faults(0);
    fprintf(stdout, "Real lcd320 RGB565 summary=%08x detail1=%08x detail2=%08x, 8 items and timeout/retirement\n",
            summary, first, second);
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
    result.active = true;
    snapshot.phase = RC(RESULT);
    snapshot.task_state = RC(TASK_SUCCEEDED);
    snapshot.task_revision = 42;
    strcpy(snapshot.request_id, "fixture");
    legacy_items = true;
    assert(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, PAGE, 0);

    exercise_results();
    exercise_modals();

    xiaotai_ui_reset();
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&synced);
    assert(!result.leases);
    return 0;
}
