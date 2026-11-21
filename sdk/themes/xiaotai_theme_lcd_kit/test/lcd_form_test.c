// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_room_ptt_feedback.py
/* Replaces legacy: tests/lvgl/test_key_draw.c::key_draw */
// Replaces legacy: tests/lvgl/test_preferences_choice.c::preferences_choice_0
// Replaces legacy: tests/lvgl/test_preferences_choice.c::preferences_choice_1
// Replaces legacy: tests/lvgl/test_preferences_choice.c::preferences_choice_2
// Replaces legacy: tests/lvgl/test_preferences_choice.c::preferences_choice_3
#include "lvgl.h"
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/theme_lcd.h"

#include <string.h>

enum { CONTROLS = 32, MAX_POSTS = 64 };
typedef struct {
    const char *intent;
    xiaotai_args_t args;
} posted_t;

static uint32_t now;
static size_t posts, flushes;
static uint16_t pixels[320 * 240];
static posted_t post[MAX_POSTS];
static xiaotai_signal_t drained;
static bool fixture_ptt, accept_ptt;
static uint32_t ptt_ticket, ptt_generation, ptt_at;
static unsigned ptt_begin, ptt_refresh, ptt_end;

static uint32_t test_clock(void) { return now; }

static void handle(const xiaotai_args_t *args, void *ctx)
{
    if (posts < MAX_POSTS) post[posts++] = (posted_t){.intent = ctx, .args = *args};
}

static void signal_drain(void *unused)
{
    (void)unused;
    xiaotai_signal_give(&drained);
}

static int drain(void)
{
    CHECK(xiaotai_run(signal_drain, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&drained, 5000U));
    CHECK(posts < MAX_POSTS);
    return 0;
}

static int score(const xiaotai_display_desc_t *desc)
{
    return desc->width == 320 && desc->height == 240 && desc->pixel == XIAOTAI_PIXEL_RGB565 ? 100 : 0;
}

static xiaotai_err_t start(const xiaotai_ui_host_t *host, void **out)
{
    return xiaotai_theme_lcd_start(host, NULL, out);
}

static const xiaotai_theme_t theme = {
    .name = "fixture", .backend = XIAOTAI_UI_BACKEND_LVGL, .match = score, .start = start,
    .render = xiaotai_theme_lcd_render, .tick = xiaotai_theme_lcd_tick, .sleep = xiaotai_theme_lcd_sleep,
    .controls = xiaotai_theme_lcd_controls, .stop = xiaotai_theme_lcd_stop,
    .stack_bytes = XIAOTAI_THEME_LCD_STACK_BYTES,
};

static xiaotai_err_t capture_ptt(void *ctx, uint32_t ticket, int32_t value, xiaotai_ui_hold_phase_t phase)
{
    (void)ctx;
    CHECK(ticket && !xiaotai_on_owner());
    if (phase == XIAOTAI_UI_HOLD_BEGIN) {
        ++ptt_begin;
        if (!accept_ptt || !value) return XIAOTAI_ERR_INVALID_STATE;
        CHECK(!ptt_ticket);
        ptt_ticket = ticket;
        ptt_generation = (uint32_t)value;
    } else {
        CHECK(ptt_ticket == ticket && ptt_generation == (uint32_t)value);
        if (phase == XIAOTAI_UI_HOLD_END) {
            ++ptt_end;
            ptt_ticket = 0;
            return XIAOTAI_OK;
        }
        ++ptt_refresh;
        CHECK(phase == XIAOTAI_UI_HOLD_REFRESH);
        if (now - ptt_at > 750U) return XIAOTAI_ERR_INVALID_STATE;
    }
    ptt_at = now;
    return XIAOTAI_OK;
}

static xiaotai_err_t register_fixture(void)
{
    xiaotai_err_t err = xiaotai_ui_theme_register(&theme);
    if (err == XIAOTAI_OK && fixture_ptt)
        err = xiaotai_ui_local_hold_register("room.fixture.hold", capture_ptt, NULL);
    static const char *const names[] = {"media.volume", "media.speaker", "settings.choice", "settings.slider",
        "settings.extra", "form.code", "form.password", "form.submit", "form.back", "overlay.back", "room.fixture.hold"};
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(names) / sizeof(names[0]); ++i)
        err = xiaotai_intent_handle(names[i], handle, (void *)names[i]);
    return err;
}

static xiaotai_err_t present(void *ctx, const xiaotai_rect_t *area, const void *data)
{
    (void)ctx;
    if (!data || area->x + area->width > 320 || area->y + area->height > 240)
        return XIAOTAI_ERR_INVALID_ARG;
    for (size_t row = 0; row < area->height; ++row)
        memcpy(pixels + (area->y + row) * 320 + area->x,
               (const uint16_t *)data + row * area->width, area->width * sizeof(uint16_t));
    ++flushes;
    return XIAOTAI_OK;
}

static int setup(bool manual)
{
    xiaotai_display_desc_t desc = ui_fake_desc(320, 240, XIAOTAI_PIXEL_RGB565);
    desc.touch = true;
    ui_fake_board_set_displays(&desc, 1);
    ((xiaotai_display_t *)ui_fake_display(0))->present = present;
    CHECK(xiaotai_hal_init() == XIAOTAI_OK && xiaotai_signal_init(&drained) == XIAOTAI_OK);
    ui_core_set_clock(manual ? test_clock : NULL);
    ui_driver_manual(manual);
    static const xiaotai_module_t engine = {.name = "ui", .init = xiaotai_engine_ui_init,
                                            .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "fixture", .init = register_fixture};
    static const xiaotai_module_t *const modules[] = {&engine};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1,
                                               .applications = apps, .application_count = 1};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK && drain() == 0);
    return 0;
}

static void step(uint32_t elapsed)
{
    now += elapsed;
    ui_driver_step(ui_display_at(0), now);
}

static void show(const xiaotai_view_t *view)
{
    ui_view_accept(view);
    step(20);
}

static size_t controls(xiaotai_ui_control_t out[CONTROLS])
{
    ui_display_t *display = ui_display_at(0);
    ui_lvgl_enter(display);
    const size_t count = theme.controls(display->theme_state, out, CONTROLS);
    ui_lvgl_leave(display);
    return count < CONTROLS ? count : CONTROLS;
}

static const xiaotai_ui_control_t *find(const xiaotai_ui_control_t *controls, size_t count,
                                        uint8_t layer, uint8_t element, uint16_t index, size_t copy)
{
    for (size_t i = 0; i < count; ++i)
        if (controls[i].layer == layer && controls[i].element == element && controls[i].index == index && !copy--)
            return &controls[i];
    return NULL;
}

static int tap(int x, int y)
{
    CHECK(xiaotai_ui_tap(0, x, y) == XIAOTAI_OK);
    step(18);
    step(130);
    CHECK(drain() == 0);
    return 0;
}

static int touch(const xiaotai_ui_control_t *control)
{
    CHECK(control && control->width && control->height);
    return tap(control->x + control->width / 2, control->y + control->height / 2);
}

static lv_obj_t *with_class(lv_obj_t *parent, const lv_obj_class_t *kind, size_t *skip)
{
    if (lv_obj_has_class(parent, kind)) {
        if (!*skip) return parent;
        --*skip;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = with_class(lv_obj_get_child(parent, (int32_t)i), kind, skip);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *widget(const lv_obj_class_t *kind, size_t skip)
{
    return with_class(lv_display_get_screen_active(ui_display_at(0)->host.root), kind, &skip);
}

static int form_case(void)
{
    static const char *const options[] = {"1 分钟", "5 分钟", "永不"};
    xiaotai_view_field_t fields[] = {
        {.kind = XIAOTAI_FIELD_STEPPER, .label = "音量", .intent = "media.volume", .min = 0, .max = 10, .step = 3},
        {.kind = XIAOTAI_FIELD_SWITCH, .label = "扬声器", .intent = "media.speaker", .value = 1},
        {.kind = XIAOTAI_FIELD_CHOICE, .label = "自动息屏", .intent = "settings.choice", .options = options, .option_count = 3},
        {.kind = XIAOTAI_FIELD_SLIDER, .label = "音量", .intent = "settings.slider", .min = 0, .max = 10,
         .value = 2, .flags = XIAOTAI_FIELD_LIVE},
        {.kind = XIAOTAI_FIELD_TEXT, .label = "状态", .text = "已准备"},
    };
    static const xiaotai_view_item_t entry = {.label = "网络信息", .intent = "settings.extra", .value = 11};
    xiaotai_view_t view = {.kind = XIAOTAI_VIEW_FORM, .id = "form", .title = "设置", .fields = fields,
                           .field_count = 5, .items = &entry, .item_count = 1};
    show(&view);
    CHECK(flushes > 0);
    xiaotai_ui_control_t reported[CONTROLS];
    size_t n = controls(reported);
    const xiaotai_ui_control_t *minus = find(reported, n, 0, XIAOTAI_UI_ELEMENT_FIELD, 0, 0);
    const xiaotai_ui_control_t *plus = find(reported, n, 0, XIAOTAI_UI_ELEMENT_FIELD, 0, 1);
    CHECK(minus && plus && (minus->state & XIAOTAI_STATE_DISABLED) && !(plus->state & XIAOTAI_STATE_DISABLED));
    CHECK(touch(minus) == 0 && posts == 0);
    CHECK(touch(plus) == 0 && posts == 1 && !strcmp(post[0].intent, "media.volume") && post[0].args.value == 3);
    lv_obj_t *sw = widget(&lv_switch_class, 0);
    lv_obj_t *dd = widget(&lv_dropdown_class, 0);
    lv_obj_t *slider = widget(&lv_slider_class, 0);
    CHECK(sw && dd && slider);
    fields[0].value = 9;
    show(&view);
    CHECK(widget(&lv_switch_class, 0) == sw && widget(&lv_dropdown_class, 0) == dd);
    CHECK(touch(plus) == 0 && posts == 2 && post[1].args.value == 10); /* clamp, never wrap on pointer */
    fields[0].value = 10;
    show(&view);
    n = controls(reported);
    plus = find(reported, n, 0, XIAOTAI_UI_ELEMENT_FIELD, 0, 1);
    CHECK(plus && plus->state & XIAOTAI_STATE_DISABLED);
    CHECK(touch(plus) == 0 && posts == 2);

    CHECK(touch(find(reported, n, 0, XIAOTAI_UI_ELEMENT_FIELD, 1, 0)) == 0 && posts == 3);
    CHECK(!strcmp(post[2].intent, "media.speaker") && post[2].args.value == 0);
    CHECK(lv_obj_has_state(sw, LV_STATE_CHECKED)); /* owner did not accept; no optimistic state left */

    /* Actual pinned dropdown event: a refusal can show an identical view. */
    lv_dropdown_set_selected(dd, 2);
    CHECK(lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK && drain() == 0);
    CHECK(posts == 4 && !strcmp(post[3].intent, "settings.choice") && post[3].args.value == 2);
    CHECK(lv_dropdown_get_selected(dd) == 0);
    show(&view);
    CHECK(drain() == 0 && posts == 4 && widget(&lv_dropdown_class, 0) == dd);
    fields[2].value = 2;
    show(&view);
    CHECK(lv_dropdown_get_selected(dd) == 2);

    const xiaotai_ui_control_t *track = find(reported, n, 0, XIAOTAI_UI_ELEMENT_FIELD, 3, 0);
    CHECK(track && track->x >= 0 && track->y >= 0 && track->y < 230);
    const int x = track->x + track->width / 5, y = track->y + track->height / 2;
    CHECK(xiaotai_ui_pointer(0, x, y, true) == XIAOTAI_OK); step(18);
    CHECK(xiaotai_ui_pointer(0, track->x + track->width * 4 / 5, y, true) == XIAOTAI_OK); step(18);
    CHECK(xiaotai_ui_pointer(0, track->x + track->width * 4 / 5, y, false) == XIAOTAI_OK); step(18);
    CHECK(drain() == 0 && posts >= 6);
    for (size_t i = 4; i + 1U < posts; ++i)
        CHECK(!strcmp(post[i].intent, "settings.slider") && !strcmp(post[i].args.text, XIAOTAI_ARG_LIVE));
    CHECK(!strcmp(post[posts - 1].intent, "settings.slider") && !post[posts - 1].args.text[0]);
    CHECK(post[posts - 1].args.value > 2 && lv_slider_get_value(slider) == 2);

    lv_obj_t *scroll = lv_obj_get_parent(lv_obj_get_parent(sw));
    lv_obj_scroll_to_y(scroll, 115, LV_ANIM_OFF);
    step(18);
    const int32_t scrolled = lv_obj_get_scroll_y(scroll);
    CHECK(scrolled > 70);
    fields[0].value = 4;
    show(&view);
    CHECK(lv_obj_get_scroll_y(scroll) == scrolled && widget(&lv_switch_class, 0) == sw);
    n = controls(reported);
    const size_t before = posts;
    CHECK(touch(find(reported, n, 0, XIAOTAI_UI_ELEMENT_ITEM, 0, 0)) == 0);
    CHECK(posts == before + 1U && !strcmp(post[before].intent, "settings.extra") && post[before].args.value == 11);
    return 0;
}

static int tap_choice_option(lv_obj_t *dropdown, size_t index)
{
    CHECK(lv_dropdown_is_open(dropdown));
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    CHECK(list);
    lv_obj_update_layout(list);
    lv_obj_t *label = lv_obj_get_child(list, 0);
    CHECK(label && lv_obj_has_class(label, &lv_label_class));
    lv_area_t popup, text;
    lv_obj_get_coords(list, &popup);
    lv_obj_get_coords(label, &text);
    const int32_t font_height = lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));
    const int32_t line_height = font_height + lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    CHECK(line_height > 0);
    const int x = (popup.x1 + popup.x2) / 2;
    const int row = (int32_t)index * line_height + font_height / 2;
    int y = text.y1 + row;
    if (y < popup.y1 || y > popup.y2 || y < 0 || y >= 240) {
        const int top = popup.y1 > 0 ? popup.y1 : 0;
        const int bottom = popup.y2 < 239 ? popup.y2 : 239;
        const int center = (top + bottom) / 2;
        lv_obj_scroll_to_y(list, lv_obj_get_scroll_y(list) + y - center, LV_ANIM_OFF);
        lv_obj_update_layout(list);
        lv_obj_get_coords(label, &text);
        y = text.y1 + row;
    }
    CHECK(x >= 0 && x < 320 && y >= 0 && y < 240 && y >= popup.y1 && y <= popup.y2);
    return tap(x, y);
}

static lv_indev_state_t choice_key_state = LV_INDEV_STATE_RELEASED;

static void choice_read_key(lv_indev_t *unused, lv_indev_data_t *data)
{
    (void)unused;
    data->key = LV_KEY_ENTER;
    data->state = choice_key_state;
}

static void choice_press_enter(lv_indev_t *keypad)
{
    choice_key_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(keypad);
    choice_key_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(keypad);
}

static int choice_enter_no_reconfirm(lv_obj_t *dropdown, size_t before)
{
    lv_group_t *group = lv_group_create();
    lv_indev_t *keypad = lv_indev_create();
    CHECK(group && keypad);
    lv_group_add_obj(group, dropdown);
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(keypad, ui_display_at(0)->host.root);
    lv_indev_set_mode(keypad, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(keypad, choice_read_key);
    lv_indev_set_group(keypad, group);

    choice_press_enter(keypad);
    CHECK(drain() == 0 && lv_dropdown_is_open(dropdown) && posts == before);
    choice_press_enter(keypad);
    CHECK(drain() == 0 && !lv_dropdown_is_open(dropdown) && posts == before);
    lv_indev_delete(keypad);
    lv_group_delete(group);
    return 0;
}

static int check_choice(const char *id, const char *label, const char *const *options,
                        size_t count, uint32_t selected, bool confirm)
{
    const xiaotai_view_field_t field = {.kind = XIAOTAI_FIELD_CHOICE, .label = label, .intent = "settings.choice",
        .value = (int32_t)selected, .options = options, .option_count = count,
        .flags = confirm ? XIAOTAI_FIELD_CONFIRM_CURRENT : 0};
    const xiaotai_view_t form = {.kind = XIAOTAI_VIEW_FORM, .id = id, .title = "设置",
        .fields = &field, .field_count = 1};
    show(&form);
    lv_obj_t *dropdown = widget(&lv_dropdown_class, 0);
    CHECK(dropdown && lv_dropdown_get_selected(dropdown) == selected);
    xiaotai_ui_control_t reported[CONTROLS];
    const size_t n = controls(reported);
    const xiaotai_ui_control_t *button = find(reported, n, 0, XIAOTAI_UI_ELEMENT_FIELD, 0, 0);
    CHECK(button && button->width && button->height);

    const uint32_t different = (selected + 1U) % count;
    const size_t before = posts;
    lv_dropdown_set_selected(dropdown, different);
    CHECK(drain() == 0 && posts == before);
    lv_dropdown_set_selected(dropdown, selected);
    uint32_t injected_id = selected;
    CHECK(lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, &injected_id) == LV_RESULT_OK);
    CHECK(drain() == 0 && posts == before);

    CHECK(touch(button) == 0 && lv_dropdown_is_open(dropdown) && posts == before);
    CHECK(lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, &injected_id) == LV_RESULT_OK);
    CHECK(drain() == 0 && lv_dropdown_is_open(dropdown) && posts == before);
    CHECK(touch(button) == 0 && !lv_dropdown_is_open(dropdown) && posts == before);
    CHECK(!confirm || choice_enter_no_reconfirm(dropdown, before) == 0);

    CHECK(touch(button) == 0 && lv_dropdown_is_open(dropdown) && posts == before);
    CHECK(tap_choice_option(dropdown, selected) == 0 && !lv_dropdown_is_open(dropdown));
    CHECK(posts == before + (confirm ? 1U : 0U));
    if (confirm) {
        CHECK(!strcmp(post[before].intent, field.intent) && post[before].args.value == (int32_t)selected);
        CHECK(!strcmp(post[before].args.text, XIAOTAI_ARG_CHOICE_RECONFIRM));
    }
    CHECK(lv_dropdown_get_selected(dropdown) == selected);

    const size_t after_equal = posts;
    CHECK(touch(button) == 0 && lv_dropdown_is_open(dropdown) && posts == after_equal);
    CHECK(tap_choice_option(dropdown, different) == 0 && !lv_dropdown_is_open(dropdown));
    CHECK(posts == after_equal + 1U && !strcmp(post[after_equal].intent, field.intent));
    CHECK(post[after_equal].args.value == (int32_t)different && !post[after_equal].args.text[0]);
    CHECK(lv_dropdown_get_selected(dropdown) == selected); /* View owner has not accepted the change yet. */
    return 0;
}

static int choice_confirm_case(void)
{
    static const char *const voice[] = {"女声", "男声"};
    static const char *const sleep[] = {"1 分钟", "5 分钟", "10 分钟", "30 分钟", "永不"};
    CHECK(check_choice("choice_ack", "回应声", voice, 2, 0, true) == 0);
    CHECK(check_choice("choice_sleep", "自动息屏", sleep, 5, 1, true) == 0);
    CHECK(check_choice("choice_default", "自动息屏", sleep, 5, 1, false) == 0);
    return 0;
}

static void observe_choice_value(lv_event_t *event)
{
    unsigned *const events = lv_event_get_user_data(event);
    ++*events;
}

static int option_matrix(const char *id, const char *const *options, size_t count)
{
    xiaotai_view_field_t field = {.kind = XIAOTAI_FIELD_CHOICE, .intent = "settings.choice", .label = id,
        .options = options, .option_count = count, .flags = XIAOTAI_FIELD_CONFIRM_CURRENT};
    const xiaotai_view_t form = {.kind = XIAOTAI_VIEW_FORM, .id = id, .fields = &field, .field_count = 1};
    show(&form);
    lv_obj_t *dropdown = widget(&lv_dropdown_class, 0);
    CHECK(dropdown);
    unsigned events = 0;
    lv_event_dsc_t *const observer = lv_obj_add_event_cb(dropdown, observe_choice_value, LV_EVENT_VALUE_CHANGED, &events);
    const size_t first_post = posts;
    for (uint32_t old = 0; old < count; ++old) {
        for (uint32_t next = 0; next < count; ++next) {
            field.value = (int32_t)old;
            show(&form);
            CHECK(drain() == 0 && widget(&lv_dropdown_class, 0) == dropdown);
            CHECK(lv_dropdown_get_selected(dropdown) == old);
            xiaotai_ui_control_t reported[CONTROLS];
            const size_t n = controls(reported);
            const xiaotai_ui_control_t *button = find(reported, n, 0, XIAOTAI_UI_ELEMENT_FIELD, 0, 0);
            const size_t before = posts;
            const unsigned before_events = events;
            CHECK(button && touch(button) == 0 && lv_dropdown_is_open(dropdown));
            CHECK(posts == before && events == before_events);
            CHECK(tap_choice_option(dropdown, next) == 0 && !lv_dropdown_is_open(dropdown));
            CHECK(posts == before + 1U && events == before_events + 1U);
            CHECK(post[before].args.value == (int32_t)next && !strcmp(post[before].intent, field.intent));
            CHECK(!strcmp(post[before].args.text, next == old ? XIAOTAI_ARG_CHOICE_RECONFIRM : ""));

            /* A refused owner keeps the old view; an accepted view may then
             * select the new row. Neither update is a second user event. */
            CHECK(lv_dropdown_get_selected(dropdown) == old);
            show(&form);
            CHECK(lv_dropdown_get_selected(dropdown) == old);
            field.value = (int32_t)next;
            show(&form);
            CHECK(lv_dropdown_get_selected(dropdown) == next);
            CHECK(drain() == 0 && posts == before + 1U && events == before_events + 1U);
        }
    }
    CHECK(events == count * count && posts - first_post == count * count);
    CHECK(lv_obj_remove_event_dsc(dropdown, observer));
    printf("choice_matrix field=%s pairs=%u value_events=%u owner_posts=%zu recursive=0\n",
           id, (unsigned)(count * count), events, posts - first_post);
    return 0;
}

static int choice_matrix_case(void)
{
    static const char *const voice[] = {"女声", "男声"};
    static const char *const sleep[] = {"1 分钟", "5 分钟", "10 分钟", "30 分钟", "永不"};
    CHECK(option_matrix("ack", voice, sizeof(voice) / sizeof(*voice)) == 0);
    CHECK(option_matrix("sleep", sleep, sizeof(sleep) / sizeof(*sleep)) == 0);
    return 0;
}

static int key(unsigned number)
{
    /* Current canvas is 320x240 with old fixed 4x3 hitboxes. */
    return tap(42 + (int)(number % 4) * 77, 106 + (int)(number / 4) * 52);
}

typedef struct { unsigned drawn; bool backspace_font; } keypad_draw_t;

static void inspect_key_draw(lv_event_t *event)
{
    const lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(lv_event_get_draw_task(event));
    if (!label || !label->text || label->base.obj != lv_event_get_current_target_obj(event)) return;
    keypad_draw_t *draw = lv_event_get_user_data(event);
    const char *text = label->text;
    if (text[0] >= '0' && text[0] <= '9' && !text[1]) draw->drawn |= 1U << (text[0] - '0');
    else if (!strcmp(text, LV_SYMBOL_BACKSPACE)) {
        draw->drawn |= 1U << 10;
        draw->backspace_font = label->font == &lv_font_montserrat_14;
    } else if (!strcmp(text, "确认")) draw->drawn |= 1U << 11;
}

static int keypad_draw(void)
{
    keypad_draw_t draw = {0};
    lv_obj_t *keys[12];
    size_t count = 0;
    /* Select the built key labels, excluding the title, fields and back icon. */
    for (size_t i = 0;; ++i) {
        lv_obj_t *label = widget(&lv_label_class, i);
        if (!label) break;
        lv_obj_t *parent = lv_obj_get_parent(label);
        if (!parent || lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t area;
        lv_obj_get_coords(parent, &area);
        if (area.y1 < 84 || lv_area_get_width(&area) != 73 || lv_area_get_height(&area) != 48) continue;
        CHECK(count < sizeof(keys) / sizeof(keys[0]));
        keys[count++] = label;
    }
    CHECK(count == sizeof(keys) / sizeof(keys[0]));
    const size_t before = flushes;
    for (size_t i = 0; i < count; ++i) {
        lv_obj_add_flag(keys[i], LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(keys[i], inspect_key_draw, LV_EVENT_DRAW_TASK_ADDED, &draw);
        lv_obj_invalidate(keys[i]);
    }
    step(20);
    for (size_t i = 0; i < count; ++i) (void)lv_obj_remove_event_cb_with_user_data(keys[i], inspect_key_draw, &draw);
    CHECK(draw.drawn == 0xfffU && draw.backspace_font && flushes > before);
    return 0;
}

static int keypad_case(void)
{
    xiaotai_view_field_t fields[] = {
        {.kind = XIAOTAI_FIELD_DIGITS, .label = "六位房间号", .intent = "form.code", .text = "123456", .max = 6},
        {.kind = XIAOTAI_FIELD_DIGITS, .label = "密码（可选）", .intent = "form.password", .text = "", .max = 4,
         .flags = XIAOTAI_FIELD_MASKED | XIAOTAI_FIELD_ALLOW_EMPTY},
    };
    const xiaotai_view_action_t actions[] = {
        {.role = XIAOTAI_ROLE_PRIMARY, .label = "确认", .intent = "form.submit"},
        {.role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK, .intent = "form.back"},
    };
    xiaotai_view_t page = {.kind = XIAOTAI_VIEW_FORM, .id = "keypad", .title = "加入房间",
                           .fields = fields, .field_count = 2, .actions = actions, .action_count = 2};
    show(&page);
    xiaotai_ui_control_t reported[CONTROLS];
    size_t n = controls(reported);
    unsigned keys = 0, submit = 0;
    for (size_t i = 0; i < n; ++i) {
        keys += reported[i].element == XIAOTAI_UI_ELEMENT_OTHER && reported[i].y >= 84;
        submit += reported[i].element == XIAOTAI_UI_ELEMENT_ACTION && reported[i].index == 0 && reported[i].y >= 84;
    }
    CHECK(keys == 11 && submit == 1);
    const xiaotai_ui_control_t *backspace = find(reported, n, 0, XIAOTAI_UI_ELEMENT_OTHER, 10, 0);
    CHECK(backspace && !strcmp(backspace->text, LV_SYMBOL_BACKSPACE));
    CHECK(keypad_draw() == 0);
    CHECK(lv_font_get_glyph_width(&lv_font_montserrat_14, 0xf55a, 0) > 0);
    unsigned ink = 0;
    for (int dy = (int)backspace->height / 2 - 8; dy < (int)backspace->height / 2 + 8; ++dy)
        for (int dx = (int)backspace->width / 2 - 10; dx < (int)backspace->width / 2 + 10; ++dx)
            if (pixels[(backspace->y + dy) * 320 + backspace->x + dx] & 0x8000U) ++ink;
    CHECK(ink >= 8); /* a symbol actually reached the RGB565 flush, not only metadata */
    const xiaotai_ui_control_t *password = find(reported, n, 0, XIAOTAI_UI_ELEMENT_FIELD, 1, 0);
    CHECK(touch(password) == 0 && posts == 0);
    lv_obj_t *password_widget = widget(&lv_textarea_class, 1);
    CHECK(password_widget && lv_textarea_get_password_mode(password_widget));
    lv_textarea_add_char(password_widget, 'X');
    CHECK(!*lv_textarea_get_text(password_widget));
    xiaotai_ui_host_input(&ui_display_at(0)->host, XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_FIELD, 1,
                          XIAOTAI_UI_INPUT_COMMIT, 0, "12X4");
    CHECK(drain() == 0 && posts == 0);
    for (unsigned i = 0; i < 3; ++i) CHECK(key(i) == 0);
    CHECK(key(11) == 0 && posts == 0); /* partial text never reaches the room owner */
    CHECK(key(3) == 0 && posts == 1);
    CHECK(!strcmp(post[0].intent, "form.password") && !strcmp(post[0].args.text, "1234"));
    fields[1].text = "1234";
    show(&page);
    CHECK(key(11) == 0 && posts == 2 && !strcmp(post[1].intent, "form.submit"));
    for (unsigned i = 0; i < 4; ++i) CHECK(key(10) == 0);
    CHECK(posts == 3 && !strcmp(post[2].intent, "form.password") && !post[2].args.text[0]);
    CHECK(key(11) == 0 && posts == 4 && !strcmp(post[3].intent, "form.submit"));

    fields[1].flags = XIAOTAI_FIELD_MASKED;
    fields[1].text = "";
    page.id = "required_password";
    show(&page);
    CHECK(key(11) == 0 && posts == 4);
    xiaotai_ui_host_input(&ui_display_at(0)->host, XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_FIELD, 1,
                          XIAOTAI_UI_INPUT_COMMIT, 0, "");
    CHECK(drain() == 0 && posts == 4);
    return 0;
}

static int overlay_case(void)
{
    const xiaotai_view_field_t field = {.kind = XIAOTAI_FIELD_STEPPER, .label = "音量",
        .intent = "media.volume", .min = 0, .max = 10, .value = 5};
    const xiaotai_view_t form = {.kind = XIAOTAI_VIEW_FORM, .id = "underlay", .title = "设置",
                                 .fields = &field, .field_count = 1};
    show(&form);
    xiaotai_ui_control_t rows[CONTROLS];
    size_t n = controls(rows);
    const xiaotai_ui_control_t *under = find(rows, n, 0, XIAOTAI_UI_ELEMENT_FIELD, 0, 1);
    CHECK(under);
    const int x = under->x + under->width / 2, y = under->y + under->height / 2;
    const xiaotai_view_action_t back = {.label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = "overlay.back"};
    const xiaotai_view_t modal = {.kind = XIAOTAI_VIEW_FORM, .layer = XIAOTAI_LAYER_OVERLAY, .id = "confirm",
                                 .title = "确认", .text = "确认操作", .actions = &back, .action_count = 1};
    show(&modal);
    n = controls(rows);
    CHECK(n && rows[0].layer == XIAOTAI_LAYER_OVERLAY);
    CHECK(tap(x, y) == 0 && posts == 0); /* card shields the live underlay stepper */
    CHECK(tap(4, 4) == 0 && posts == 1 && !strcmp(post[0].intent, "overlay.back"));
    const xiaotai_view_t close = {.kind = XIAOTAI_VIEW_FORM, .layer = XIAOTAI_LAYER_OVERLAY,
                                 .id = "confirm", .flags = XIAOTAI_VIEW_DISMISS};
    show(&close);
    CHECK(tap(x, y) == 0 && posts == 2 && !strcmp(post[1].intent, "media.volume"));

    /* A modal can arrive while a page control is still held. Its release
     * must not activate the page, and the next distinct backdrop press may
     * legitimately trigger BACK. */
    CHECK(xiaotai_ui_pointer(0, x, y, true) == XIAOTAI_OK); step(18);
    CHECK(ui_display_at(0)->press.down);
    show(&modal);
    CHECK(ui_display_at(0)->press.suppressed);
    CHECK(xiaotai_ui_pointer(0, x, y, false) == XIAOTAI_OK); step(18);
    CHECK(drain() == 0 && posts == 2);
    CHECK(tap(4, 4) == 0 && posts == 3 && !strcmp(post[2].intent, "overlay.back"));
    return 0;
}

static int form_diag_case(void)
{
    const xiaotai_view_field_t field = {.kind = XIAOTAI_FIELD_STEPPER, .label = "音量",
        .intent = "media.volume", .min = 0, .max = 10, .state = XIAOTAI_STATE_CHECKED};
    const xiaotai_view_t view = {.kind = XIAOTAI_VIEW_FORM, .id = "diag_edges", .title = "设置",
                                 .fields = &field, .field_count = 1};
    ui_view_accept(&view);
    xiaotai_ui_info_t info;
    CHECK(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.ready && !info.owner_timer);
    /* The real background UI task takes the form state and fills AT+UI's
     * public controls snapshot, including the physical button states. */
    xiaotai_ui_control_t rows[CONTROLS];
    size_t count = 0;
    /* A request can be serviced at the end of the step that began before
     * this test posted the view. The next request sees the staged form. */
    for (unsigned i = 0; i < 3 && !count; ++i)
        CHECK(xiaotai_ui_controls(0, rows, CONTROLS, &count, 5000) == XIAOTAI_OK);
    CHECK(count == 2);
    CHECK(rows[0].element == XIAOTAI_UI_ELEMENT_FIELD && rows[1].element == XIAOTAI_UI_ELEMENT_FIELD);
    CHECK(rows[0].index == 0 && rows[1].index == 0 && rows[0].x < rows[1].x);
    CHECK(rows[0].state == (XIAOTAI_STATE_CHECKED | XIAOTAI_STATE_DISABLED));
    CHECK(rows[1].state == XIAOTAI_STATE_CHECKED);
    CHECK(!strcmp(rows[0].text, "音量") && !strcmp(rows[1].text, "音量") &&
          !strcmp(rows[0].intent, "media.volume") && !strcmp(rows[1].intent, "media.volume"));
    return 0;
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_has_class(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *child = find_label(lv_obj_get_child(obj, (int32_t)i), text);
        if (child) return child;
    }
    return NULL;
}

static lv_obj_t *find_control(const char *text)
{
    lv_obj_t *const label = find_label(lv_display_get_screen_active(ui_display_at(0)->host.root), text);
    return label ? lv_obj_get_parent(label) : NULL;
}

static void count_pressing(lv_event_t *event)
{
    ++*(unsigned *)lv_event_get_user_data(event);
}

static int ptt_edges_case(void)
{
    xiaotai_view_action_t actions[] = {
        {.label = "按下讲话", .role = XIAOTAI_ROLE_PRIMARY, .intent = "room.fixture.hold",
         .trigger = XIAOTAI_TRIGGER_TAP, .value = 0},
        {.label = "其他", .intent = "form.back"},
    };
    const xiaotai_view_t view = {.id = "room", .kind = XIAOTAI_VIEW_LIST,
        .title = "多人对讲", .actions = actions, .action_count = 2};
    show(&view);
    lv_obj_t *control = find_control(actions[0].label);
    lv_obj_t *other = find_control(actions[1].label);
    CHECK(control && other);
    CHECK(lv_obj_has_flag(control, LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_has_flag(control, LV_OBJ_FLAG_PRESS_LOCK));
    actions[0].trigger = XIAOTAI_TRIGGER_HOLD;
    actions[0].state = XIAOTAI_STATE_HOLD_FROM_POINTER;
    show(&view);
    CHECK(find_control(actions[0].label) == control && !lv_obj_has_flag(control, LV_OBJ_FLAG_PRESS_LOCK));
    actions[0].trigger = XIAOTAI_TRIGGER_TAP;
    actions[0].state = 0;
    show(&view);
    CHECK(find_control(actions[0].label) == control);
    const bool restored_default = lv_obj_has_flag(control, LV_OBJ_FLAG_PRESS_LOCK);
    printf("same LVGL object restored its original enabled press lock: %u\n", (unsigned)restored_default);
    CHECK(restored_default && lv_obj_has_flag(other, LV_OBJ_FLAG_PRESS_LOCK));

    unsigned ordinary_pressing = 0;
    CHECK(lv_obj_add_event_cb(control, count_pressing, LV_EVENT_PRESSING, &ordinary_pressing));
    lv_area_t area;
    lv_obj_get_coords(control, &area);
    const int16_t x = (int16_t)((area.x1 + area.x2) / 2);
    const int16_t y = (int16_t)((area.y1 + area.y2) / 2);
    CHECK(xiaotai_ui_pointer(0, x, y, true) == XIAOTAI_OK);
    for (unsigned i = 0; i < 4; ++i) step(50);
    CHECK(xiaotai_ui_pointer(0, x, y, false) == XIAOTAI_OK);
    step(20);
    CHECK(drain() == 0 && ordinary_pressing > 0 && !ptt_begin && !ptt_refresh && !ptt_end);
    CHECK(posts == 1 && !post[0].args.text[0]);
    printf("ordinary widget via actual pointer polling: pressing=%u local_refresh=%u\n", ordinary_pressing, ptt_refresh);
    CHECK(lv_obj_remove_event_cb_with_user_data(control, count_pressing, &ordinary_pressing));
    posts = 0;

    /* A control that was already unlocked must stay unlocked on return. */
    lv_obj_remove_flag(control, LV_OBJ_FLAG_PRESS_LOCK);
    actions[0].trigger = XIAOTAI_TRIGGER_HOLD;
    actions[0].state = XIAOTAI_STATE_HOLD_FROM_POINTER;
    show(&view);
    actions[0].trigger = XIAOTAI_TRIGGER_TAP;
    actions[0].state = 0;
    show(&view);
    CHECK(find_control(actions[0].label) == control && !lv_obj_has_flag(control, LV_OBJ_FLAG_PRESS_LOCK));
    printf("same LVGL object restored its original cleared press lock: 1\n");
    actions[0].trigger = XIAOTAI_TRIGGER_HOLD;
    actions[0].state = XIAOTAI_STATE_HOLD_FROM_POINTER;
    show(&view);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK && ptt_begin == 1U && !ptt_ticket);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESSING, NULL) == LV_RESULT_OK && !ptt_refresh);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESS_LOST, NULL) == LV_RESULT_OK && !ptt_end);

    actions[0].value = 12;
    show(&view);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK && ptt_begin == 2U && !ptt_ticket);
    CHECK(lv_obj_send_event(control, LV_EVENT_RELEASED, NULL) == LV_RESULT_OK && !ptt_end);
    accept_ptt = true;
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK && ptt_begin == 3U && ptt_ticket);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK && ptt_begin == 3U);
    CHECK(lv_obj_send_event(other, LV_EVENT_RELEASED, NULL) == LV_RESULT_OK && ptt_ticket && !ptt_end);
    step(749);
    CHECK(!ptt_refresh && ptt_ticket);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESSING, NULL) == LV_RESULT_OK && ptt_refresh == 1U);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESS_LOST, NULL) == LV_RESULT_OK && ptt_end == 1U && !ptt_ticket);
    lv_obj_delete(control);
    CHECK(ptt_end == 1U);

    xiaotai_view_t replacement = view;
    replacement.id = "room-2";
    show(&replacement);
    control = find_control(actions[0].label);
    CHECK(control);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK && ptt_begin == 4U && ptt_ticket);
    step(751);
    CHECK(ptt_refresh == 1U && ptt_ticket);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESSING, NULL) == LV_RESULT_OK && ptt_refresh == 2U);
    CHECK(ptt_end == 2U && !ptt_ticket);
    CHECK(lv_obj_send_event(control, LV_EVENT_RELEASED, NULL) == LV_RESULT_OK && ptt_end == 2U);
    CHECK(lv_obj_send_event(control, LV_EVENT_PRESSED, NULL) == LV_RESULT_OK && ptt_begin == 5U && ptt_ticket);
    lv_obj_delete(control);
    CHECK(drain() == 0 && !posts && !ptt_ticket && ptt_end == 3U);
    printf("real LVGL hold: begin=%u observed_refresh=%u exact_ends=%u queued_fallback=%zu\n",
           ptt_begin, ptt_refresh, ptt_end, posts);
    return 0;
}

int main(void)
{
    const bool diag = !strcmp(LCD_FORM_CASE, "form_diag");
    fixture_ptt = !strcmp(LCD_FORM_CASE, "ptt_edges");
    CHECK(setup(!diag) == 0);
    if (diag) RUN(form_diag_case);
    else if (!strcmp(LCD_FORM_CASE, "form")) RUN(form_case);
    else if (!strcmp(LCD_FORM_CASE, "choice_confirm")) RUN(choice_confirm_case);
    else if (!strcmp(LCD_FORM_CASE, "choice_matrix")) RUN(choice_matrix_case);
    else if (!strcmp(LCD_FORM_CASE, "keypad")) RUN(keypad_case);
    else if (!strcmp(LCD_FORM_CASE, "ptt_edges")) RUN(ptt_edges_case);
    else RUN(overlay_case);
    xiaotai_ui_reset();
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&drained);
    return 0;
}
