// SPDX-License-Identifier: MIT
#include "templates_wide.h"

#include <stdio.h>
#include <string.h>
#include "door_v4_assets.h"
#include "xiaotai/apps/door.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/ui_theme.h"

enum { BG = 0x0b0e10, BODY = 0xffffff, SUBTLE = 0xc4cfd1, ACCENT = 0x54e0c9, TEAL = 0x1fab96 };
enum { WIDTH = 1024, HEIGHT = 600 };
enum { MAX_CONTROLS = XIAOTAI_VIEW_ACTIONS_MAX + XIAOTAI_VIEW_ITEMS_MAX + XIAOTAI_VIEW_INDICATORS_MAX + XIAOTAI_VIEW_FIELDS_MAX };

LV_FONT_DECLARE(door_font_small);
LV_FONT_DECLARE(door_font_button);
LV_FONT_DECLARE(door_font_body);
LV_FONT_DECLARE(door_font_dialog);
LV_FONT_DECLARE(door_font_title);
LV_FONT_DECLARE(door_font_clock);

extern const uint8_t xiaotai_door_wallpaper[];
static const lv_image_dsc_t wallpaper = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = WIDTH, .h = HEIGHT, .stride = WIDTH * 2},
    .data_size = WIDTH * HEIGHT * 2, .data = xiaotai_door_wallpaper,
};
/* The direct painter needs a native-size, untransformed image draw task.
 * This 1-bit fallback is transparent, so it never paints a fake video frame. */
static const uint8_t video_clear[WIDTH / 8 * HEIGHT];

typedef enum { HOME, DEVICES, MONITOR, CALL, CONFIRM, RESULT, VOLUME } page_t;
typedef struct wide wide_t;
typedef struct {
    wide_t *owner;
    lv_obj_t *object;
    uint16_t index;
    uint8_t element;
    bool pressed, field_dirty;
} control_t;

struct wide {
    const xiaotai_ui_host_t *host;
    lv_obj_t *root, *sheet, *toast_box, *toast_label;
    lv_obj_t *title, *subtitle, *body, *clock, *date, *status, *online, *list, *slider, *percent;
    const lv_font_t *full;
    lv_font_t button_font, body_font;
    control_t controls[MAX_CONTROLS];
    size_t n;
    lv_image_dsc_t video_image;
    int32_t field_min, field_max, field_step;
    page_t page;
    uint8_t layer;
    uint16_t width, height;
    bool built, video, updating, sheet_pressed;
};

static bool equal(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, lv_opa_t opacity)
{
    if (!parent) return NULL;
    lv_obj_t *object = lv_obj_create(parent);
    if (!object) return NULL;
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font, uint32_t color, int align)
{
    if (!parent) return NULL;
    lv_obj_t *object = lv_label_create(parent);
    if (!object) return NULL;
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, w);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, align, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

static void text(lv_obj_t *object, const char *value)
{
    if (!value) value = "";
    if (object && strcmp(lv_label_get_text(object), value)) lv_label_set_text(object, value);
}

static void show(lv_obj_t *object, bool visible)
{
    if (!object) return;
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *image(lv_obj_t *parent, const lv_image_dsc_t *asset, int x, int y)
{
    if (!parent || !asset) return NULL;
    lv_obj_t *object = lv_image_create(parent);
    if (object) {
        lv_image_set_src(object, asset);
        lv_obj_set_pos(object, x, y);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    }
    return object;
}

static int32_t slider_value(const wide_t *s)
{
    const int64_t value = s->field_min + (int64_t)lv_slider_get_value(s->slider) * s->field_step;
    return value > s->field_max ? s->field_max : (int32_t)value;
}

static void control_event(lv_event_t *event)
{
    control_t *c = lv_event_get_user_data(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE && c->pressed) {
        /* LVGL drops the pressed object during rebuild and will not release it. */
        xiaotai_ui_host_input(c->owner->host, c->owner->layer, c->element, c->index,
                              XIAOTAI_UI_INPUT_RELEASE, 0, NULL);
        return;
    }
    if (c->owner->updating) return;
    xiaotai_ui_input_t kind;
    int32_t value = 0;
    switch (code) {
    case LV_EVENT_PRESSED: c->pressed = true; kind = XIAOTAI_UI_INPUT_PRESS; break;
    case LV_EVENT_CLICKED: kind = XIAOTAI_UI_INPUT_TAP; break;
    case LV_EVENT_LONG_PRESSED: kind = XIAOTAI_UI_INPUT_LONG_PRESS; break;
    case LV_EVENT_VALUE_CHANGED:
        if (c->element != XIAOTAI_UI_ELEMENT_FIELD) return;
        c->field_dirty = true;
        kind = XIAOTAI_UI_INPUT_CHANGE;
        value = slider_value(c->owner);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        c->pressed = false;
        if (c->element == XIAOTAI_UI_ELEMENT_FIELD && c->field_dirty) {
            c->field_dirty = false;
            value = slider_value(c->owner);
            xiaotai_ui_host_input(c->owner->host, c->owner->layer, c->element, c->index, XIAOTAI_UI_INPUT_COMMIT, value, NULL);
        }
        kind = XIAOTAI_UI_INPUT_RELEASE;
        break;
    default: return;
    }
    xiaotai_ui_host_input(c->owner->host, c->owner->layer, c->element, c->index, kind, value, NULL);
}

static void sheet_event(lv_event_t *event)
{
    wide_t *s = lv_event_get_user_data(event);
    xiaotai_ui_input_t input;
    if (s->updating) return;
    if (lv_event_get_code(event) == LV_EVENT_GESTURE && s->page == HOME) {
        lv_indev_t *device = lv_indev_active();
        const lv_dir_t direction = device ? lv_indev_get_gesture_dir(device) : LV_DIR_NONE;
        if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) return;
        input = direction == LV_DIR_LEFT ? XIAOTAI_UI_INPUT_SWIPE_LEFT : XIAOTAI_UI_INPUT_SWIPE_RIGHT;
    } else {
        if (lv_event_get_target_obj(event) != s->sheet) return;
        switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED: s->sheet_pressed = true; input = XIAOTAI_UI_INPUT_PRESS; break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST: s->sheet_pressed = false; input = XIAOTAI_UI_INPUT_RELEASE; break;
        default: return;
        }
    }
    xiaotai_ui_host_input(s->host, s->layer, XIAOTAI_UI_ELEMENT_OTHER, 0, input, 0, NULL);
}

static void bind(wide_t *s, lv_obj_t *object, uint8_t element, size_t index, uint8_t state, bool has_intent)
{
    if (!object || s->n >= MAX_CONTROLS || (state & XIAOTAI_STATE_HIDDEN)) return;
    control_t *c = &s->controls[s->n++];
    *c = (control_t){.owner = s, .object = object, .element = element, .index = (uint16_t)index};
    if (state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY)) {
        lv_obj_add_state(object, LV_STATE_DISABLED);
        lv_obj_set_style_opa(object, LV_OPA_40, 0);
    }
    if (!has_intent || (state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY))) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(object, control_event, LV_EVENT_ALL, c);
}

static bool usable(const xiaotai_view_action_t *action) { return !(action->state & XIAOTAI_STATE_HIDDEN); }

static lv_obj_t *hit(wide_t *s, lv_obj_t *parent, const xiaotai_view_action_t *action, size_t index,
                     int x, int y, int w, int h, uint32_t color, lv_opa_t opacity)
{
    if (!usable(action)) return NULL;
    lv_obj_t *object = box(parent, x, y, w, h, color, opacity);
    if (object) {
        lv_obj_set_style_radius(object, 8, 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_30, LV_STATE_PRESSED);
        bind(s, object, XIAOTAI_UI_ELEMENT_ACTION, index, action->state, action->intent != NULL);
    }
    return object;
}

static void icon_action(wide_t *s, const xiaotai_view_action_t *a, size_t i, int x, int y, bool home,
                        const lv_image_dsc_t *icon, const lv_image_dsc_t *circle, bool selected)
{
    const int w = home ? 176 : 112, start = (w - 72) / 2;
    lv_obj_t *object = hit(s, s->sheet, a, i, x, y, w, home ? 124 : 116, TEAL, selected ? LV_OPA_30 : LV_OPA_TRANSP);
    if (!object) return;
    if (circle) (void)image(object, circle, start, 0);
    else {
        lv_obj_t *tile = box(object, start, 0, 72, 72, 0x0e1214, LV_OPA_60);
        if (tile) lv_obj_set_style_radius(tile, 8, 0);
    }
    (void)image(object, icon, start + 18, 18);
    text(label(object, 0, home ? 88 : 80, w, &door_font_button, BODY, LV_TEXT_ALIGN_CENTER), a->label);
}

static void flat_action(wide_t *s, lv_obj_t *parent, const xiaotai_view_action_t *a, size_t i, int x, int y, int w,
                        const lv_image_dsc_t *icon, uint32_t color, lv_opa_t opacity)
{
    lv_obj_t *object = hit(s, parent, a, i, x, y, w, 64, color, opacity);
    if (!object) return;
    if (icon) (void)image(object, icon, 16, (64 - icon->header.h) / 2);
    const int left = icon ? 58 : 10;
    if (w > left + 10) text(label(object, left, 14, w - left - 10, &door_font_button, BODY,
                                  icon ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER), a->label);
}

static void place_video(wide_t *s, bool visible)
{
    const uint16_t stride = (s->width + 7U) / 8U;
    const size_t size = (size_t)stride * s->height;
    if (size > sizeof(video_clear)) return;
    s->video_image = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A1,
        .w = s->width, .h = s->height, .stride = stride}, .data_size = size, .data = video_clear};
    lv_obj_t *widget = image(s->sheet, &s->video_image, 0, 0);
    if (!widget) return;
    /* The video owner installs the source before it emits remote-ready. */
    show(widget, visible);
    if (xiaotai_ui_host_video_region(s->host, widget) == XIAOTAI_OK) s->video = true;
    else lv_obj_delete(widget);
}

static void build_home(wide_t *s, const xiaotai_view_t *v)
{
    const int cx = s->width / 2, h = s->height;
    const bool compact = h < 560;
    const bool video = (v->flags & XIAOTAI_VIEW_VIDEO) != 0;
    place_video(s, video);
    if (!video) lv_obj_set_style_bg_image_src(s->sheet, &wallpaper, 0);
    (void)box(s->sheet, 0, 0, s->width, h, BG, 46);
    s->title = label(s->sheet, 40, 32, min(620, s->width - 330), &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    s->clock = label(s->sheet, cx - 312, compact ? 88 : h / 2 - 174, 624, &door_font_clock, BODY, LV_TEXT_ALIGN_CENTER);
    s->date = label(s->sheet, cx - 312, compact ? 234 : h / 2 - 20, 624, &door_font_body, BODY, LV_TEXT_ALIGN_CENTER);
    s->online = image(s->sheet, &door_asset_online_8, cx - 75, compact ? 282 : h / 2 + 48);
    s->status = label(s->sheet, cx - 56, compact ? 267 : h / 2 + 32, 330, &door_font_small, BODY, LV_TEXT_ALIGN_LEFT);
    const int buttons = h - (compact ? 126 : 160);
    for (size_t i = 0; i < v->action_count; ++i) {
        const xiaotai_view_action_t *a = &v->actions[i];
        if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_DEVICES)) {
            lv_obj_t *peer = hit(s, s->sheet, a, i, s->width - 272, 24, 192, 56, BG, LV_OPA_TRANSP);
            text(label(peer, 16, 12, 168, &s->body_font, BODY, LV_TEXT_ALIGN_RIGHT), a->label);
        }
        else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_MONITOR))
            icon_action(s, a, i, cx - 200, buttons, true, &door_asset_video_36, NULL, false);
        else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_OPEN))
            icon_action(s, a, i, cx + 24, buttons, true, &door_asset_key_36, NULL, false);
        else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_STREAM_STOP)) {
            lv_obj_t *stop = hit(s, s->sheet, a, i, cx - 100, buttons - (compact ? 60 : 66), 200, 56,
                                 0x203c39, LV_OPA_COVER);
            lv_obj_t *caption = label(stop, 8, 10, 184, &door_font_body, BODY, LV_TEXT_ALIGN_CENTER);
            text(caption, a->label);
        } else if (equal(a->intent, XIAOTAI_INTENT_SYSTEM_MENU) && a->trigger == XIAOTAI_TRIGGER_LONG_PRESS)
            bind(s, s->title, XIAOTAI_UI_ELEMENT_ACTION, i, a->state, true);
    }
    for (size_t i = 0; i < v->indicator_count; ++i) {
        const xiaotai_view_indicator_t *a = &v->indicators[i];
        if (!equal(a->icon, XIAOTAI_ICON_WIFI)) continue;
        lv_obj_t *wifi = box(s->sheet, s->width - 76, 24, 60, 56, BG, 0);
        lv_obj_t *icon = image(wifi, &door_asset_wifi_24, 12, 16);
        if (icon) lv_obj_set_style_opa(icon, a->level < 0 ? LV_OPA_40 : LV_OPA_COVER, 0);
        bind(s, wifi, XIAOTAI_UI_ELEMENT_INDICATOR, i, 0, a->intent != NULL);
        break;
    }
}

static void build_devices(wide_t *s, const xiaotai_view_t *v)
{
    s->title = label(s->sheet, 104, 24, s->width - 130, &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    for (size_t i = 0; i < v->action_count; ++i)
        if (equal(v->actions[i].intent, XIAOTAI_INTENT_APP_DOOR_BACK))
            flat_action(s, s->sheet, &v->actions[i], i, 20, 16, 64, &door_asset_back_36, BG, LV_OPA_TRANSP);
    s->list = box(s->sheet, 32, 112, s->width - 64, s->height - 160, BG, LV_OPA_TRANSP);
    if (!s->list) return;
    lv_obj_add_flag(s->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s->list, LV_SCROLLBAR_MODE_AUTO);
    s->subtitle = label(s->list, 0, 24, s->width - 100, &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    s->body = label(s->list, 0, v->subtitle ? 88 : 24, s->width - 100, &door_font_body, SUBTLE, LV_TEXT_ALIGN_LEFT);
    int row_y = 0;
    for (size_t i = 0; i < v->item_count; ++i) {
        const xiaotai_view_item_t *item = &v->items[i];
        if (!equal(item->intent, XIAOTAI_INTENT_APP_DOOR_SELECT) || (item->state & XIAOTAI_STATE_HIDDEN)) continue;
        lv_obj_t *row = box(s->list, 0, row_y, s->width - 80, 88, 0x13171a, LV_OPA_COVER);
        row_y += 96;
        if (!row) continue;
        const bool checked = (item->state & XIAOTAI_STATE_CHECKED) != 0;
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(checked ? ACCENT : 0x384246), 0);
        text(label(row, 24, 8, s->width - 290, &s->button_font, BODY, LV_TEXT_ALIGN_LEFT), item->label);
        text(label(row, 24, 48, s->width - 290, &door_font_small, equal(item->detail, "在线") ? ACCENT : SUBTLE,
                   LV_TEXT_ALIGN_LEFT), item->detail);
        text(label(row, s->width - 280, 26, 176, &door_font_button, checked ? ACCENT : SUBTLE,
                   LV_TEXT_ALIGN_RIGHT), checked ? "已选择" : "选择");
        bind(s, row, XIAOTAI_UI_ELEMENT_ITEM, i, item->state, true);
    }
}

static xiaotai_app_door_call_view_t call_data(const xiaotai_view_t *v)
{
    xiaotai_app_door_call_view_t result = {.phase = XIAOTAI_APP_DOOR_CALL_CONNECTING};
    if (v->custom_data && v->custom_size == sizeof(result)) memcpy(&result, v->custom_data, sizeof(result));
    return result;
}

static void build_video_chrome(wide_t *s, const xiaotai_view_t *v)
{
    const int cx = s->width / 2, buttons = s->height - 134;
    const xiaotai_app_door_call_view_t call = call_data(v);
    place_video(s, (v->flags & XIAOTAI_VIEW_VIDEO) != 0);
    (void)box(s->sheet, 0, 0, s->width, 112, BG, 82);
    s->title = label(s->sheet, 32, 24, s->width - 260, &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    s->subtitle = label(s->sheet, 32, 70, s->width - 240, &s->body_font, BODY, LV_TEXT_ALIGN_LEFT);
    s->status = label(s->sheet, s->width - 184, 24, 152, &door_font_body, BODY, LV_TEXT_ALIGN_RIGHT);
    s->body = label(s->sheet, 80, s->height / 2 - 50, s->width - 160, &door_font_body, SUBTLE, LV_TEXT_ALIGN_CENTER);
    (void)box(s->sheet, 0, s->height - 146, s->width, 146, 0x0e1214, 214);
    const bool monitor = s->page == MONITOR;
    const bool ringing = call.phase == XIAOTAI_APP_DOOR_CALL_RINGING;
    const bool active = call.phase == XIAOTAI_APP_DOOR_CALL_ACTIVE;
    if (!monitor && (ringing || active))
        (void)image(s->sheet, ringing ? &door_asset_incoming_8 : &door_asset_call_online_8, s->width - 208, 37);
    for (size_t i = 0; i < v->action_count; ++i) {
        const xiaotai_view_action_t *a = &v->actions[i];
        if (monitor && equal(a->intent, XIAOTAI_INTENT_APP_DOOR_BACK))
            icon_action(s, a, i, cx - 168, buttons, false, &door_asset_back_36, &door_asset_circle_monitor_neutral_72, false);
        else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_OPEN))
            icon_action(s, a, i, cx + (monitor ? 56 : 24), buttons, false,
                        monitor ? &door_asset_key_monitor_36 : &door_asset_key_call_36,
                        monitor ? &door_asset_circle_monitor_action_72 : &door_asset_circle_action_72, false);
        else if (!monitor && equal(a->intent, "door.call.reject"))
            icon_action(s, a, i, cx - 168, buttons, false, &door_asset_hangup_36, &door_asset_circle_incoming_red_72, false);
        else if (!monitor && equal(a->intent, "door.call.accept"))
            icon_action(s, a, i, cx + 56, buttons, false, &door_asset_phone_36, &door_asset_circle_incoming_action_72, false);
        else if (!monitor && equal(a->intent, "door.call.hangup"))
            icon_action(s, a, i, cx + (active ? 184 : -56), buttons, false, &door_asset_hangup_36, &door_asset_circle_red_72, false);
        else if (!monitor && equal(a->intent, XIAOTAI_INTENT_APP_DOOR_MUTE))
            icon_action(s, a, i, cx - 296, buttons, false, &door_asset_mic_36, &door_asset_circle_neutral_72, call.muted);
        else if (!monitor && equal(a->intent, XIAOTAI_INTENT_APP_DOOR_VOLUME))
            icon_action(s, a, i, cx - 136, buttons, false, &door_asset_volume_36, &door_asset_circle_neutral_72, false);
    }
}

static void build_dialog(wide_t *s, const xiaotai_view_t *v)
{
    const bool volume = s->page == VOLUME, result = s->page == RESULT;
    const int w = volume ? 460 : 552, h = volume ? 240 : 340;
    const int x = volume ? s->width - w - 32 : (s->width - w) / 2;
    const int y = volume ? s->height - h - 80 : (s->height - h) / 2;
    lv_obj_add_flag(s->sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->sheet, sheet_event, LV_EVENT_ALL, s);
    lv_obj_t *panel = box(s->sheet, x, y, w, h, 0x13171a, 250);
    if (!panel) return;
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(BODY), 0);
    lv_obj_set_style_border_opa(panel, 56, 0);
    s->title = label(panel, volume || result ? 32 : 84, volume ? 32 : result ? 108 : 25,
        volume ? 300 : result ? 488 : 436, volume ? &door_font_dialog : &door_font_title, BODY,
        result ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT);
    if (!volume) {
        xiaotai_app_door_result_view_t outcome = {0};
        if (result && v->custom_data && v->custom_size == sizeof(outcome)) memcpy(&outcome, v->custom_data, sizeof(outcome));
        if (outcome.opened) {
            lv_obj_t *badge = box(panel, 250, 34, 52, 52, TEAL, LV_OPA_COVER);
            if (badge) {
                lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
                (void)image(badge, &door_asset_key_28, 12, 12);
            }
        } else (void)image(panel, result ? &door_asset_failure_52 : &door_asset_lock_32, result ? 250 : 32, 34);
        s->subtitle = label(panel, 32, 96, 488, &door_font_body, SUBTLE, LV_TEXT_ALIGN_LEFT);
        s->body = label(panel, 32, result ? 166 : 150, 488, &door_font_body, result ? SUBTLE : BODY,
                        result ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT);
    }
    for (size_t i = 0; i < v->action_count; ++i) {
        const xiaotai_view_action_t *a = &v->actions[i];
        if (s->page == CONFIRM && equal(a->intent, XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM)) {
            const bool confirm = a->role == XIAOTAI_ROLE_PRIMARY;
            flat_action(s, panel, a, i, confirm ? 288 : 32, 240, 232,
                        confirm ? &door_asset_key_28 : &door_asset_back_28, confirm ? TEAL : 0x0e1214, 214);
        } else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_BACK))
            flat_action(s, panel, a, i, volume ? 384 : 160, volume ? 16 : 248, volume ? 60 : 232,
                        volume ? &door_asset_close_28 : &door_asset_back_28, 0x0e1214, volume ? 0 : 214);
    }
    if (!volume) return;
    for (size_t i = 0; i < v->field_count; ++i) {
        const xiaotai_view_field_t *f = &v->fields[i];
        if (!equal(f->intent, XIAOTAI_INTENT_APP_DOOR_VOLUME_SET) || f->kind != XIAOTAI_FIELD_SLIDER ||
            (f->state & XIAOTAI_STATE_HIDDEN)) continue;
        s->slider = lv_slider_create(panel);
        if (!s->slider) return;
        lv_obj_remove_style_all(s->slider);
        lv_obj_set_pos(s->slider, 32, 130);
        lv_obj_set_size(s->slider, 304, 4);
        lv_obj_set_style_radius(s->slider, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s->slider, lv_color_hex(BODY), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s->slider, 56, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s->slider, lv_color_hex(ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_image_src(s->slider, &door_asset_knob_20, LV_PART_KNOB);
        lv_obj_set_style_pad_all(s->slider, 8, LV_PART_KNOB);
        lv_obj_set_ext_click_area(s->slider, 24);
        bind(s, s->slider, XIAOTAI_UI_ELEMENT_FIELD, i, f->state, true);
        s->percent = label(panel, 336, 114, 76, &door_font_body, BODY, LV_TEXT_ALIGN_RIGHT);
        break;
    }
}

static void release_sheet(wide_t *s)
{
    if (!s->sheet_pressed) return;
    s->sheet_pressed = false;
    xiaotai_ui_host_input(s->host, s->layer, XIAOTAI_UI_ELEMENT_OTHER, 0, XIAOTAI_UI_INPUT_RELEASE, 0, NULL);
}

static void rebuild(wide_t *s, const xiaotai_view_t *v)
{
    const int32_t scroll = s->list ? lv_obj_get_scroll_y(s->list) : 0;
    if (s->video) (void)xiaotai_ui_host_video_region(s->host, NULL);
    release_sheet(s);
    lv_obj_clean(s->sheet);
    lv_obj_remove_event_cb_with_user_data(s->sheet, sheet_event, s);
    lv_obj_remove_flag(s->sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_image_src(s->sheet, NULL, 0);
    lv_obj_set_style_bg_opa(s->sheet, s->page < CONFIRM ? LV_OPA_COVER : s->page == VOLUME ? 82 : 133, 0);
    s->title = s->subtitle = s->body = s->clock = s->date = s->status = s->online = s->list = s->slider = s->percent = NULL;
    s->n = 0;
    s->video = false;
    if (s->page == HOME) {
        lv_obj_add_flag(s->sheet, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s->sheet, sheet_event, LV_EVENT_ALL, s);
        build_home(s, v);
    } else if (s->page == DEVICES) {
        build_devices(s, v);
        if (s->list && scroll) lv_obj_scroll_to_y(s->list, scroll, LV_ANIM_OFF);
    } else if (s->page == MONITOR || s->page == CALL) build_video_chrome(s, v);
    else build_dialog(s, v);
    s->built = true;
}

static void update(void *state, const xiaotai_ui_canvas_t *canvas, const xiaotai_ui_layer_t *layer,
                   const xiaotai_ui_scene_t *scene)
{
    (void)canvas;
    wide_t *s = state;
    const xiaotai_view_t *v = &layer->view;
    s->updating = true;
    if (!s->built || (layer->changed & (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_ITEMS | XIAOTAI_CHANGED_ACTIONS |
                                       XIAOTAI_CHANGED_CUSTOM | XIAOTAI_CHANGED_FLAGS | XIAOTAI_CHANGED_INDICATORS)))
        rebuild(s, v);
    text(s->title, v->title);
    text(s->subtitle, v->subtitle);
    text(s->body, v->text);
    if (s->page == HOME) {
        static const char *const days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        char clock[20], date[48];
        if (scene && scene->clock.valid && scene->clock.weekday < 7) {
            (void)snprintf(clock, sizeof(clock), "%02u:%02u", scene->clock.hour, scene->clock.minute);
            (void)snprintf(date, sizeof(date), "%u 月 %u 日  %s", scene->clock.month, scene->clock.day, days[scene->clock.weekday]);
            text(s->clock, clock);
            text(s->date, date);
        } else {
            text(s->clock, "--:--");
            text(s->date, "正在同步时间");
        }
        xiaotai_app_door_home_view_t home = {.state = XIAOTAI_APP_DOOR_NOT_READY};
        if (v->custom_data && v->custom_size == sizeof(home)) memcpy(&home, v->custom_data, sizeof(home));
        if (s->online) {
            const bool online = home.state == XIAOTAI_APP_DOOR_STATION_ONLINE || home.state == XIAOTAI_APP_DOOR_STREAMING;
            lv_obj_set_style_image_recolor(s->online, lv_color_hex(online ? ACCENT : 0x8c999e), 0);
            lv_obj_set_style_image_recolor_opa(s->online, LV_OPA_COVER, 0);
        }
    }
    if (s->page == CALL && v->timer_mode != XIAOTAI_TIMER_NONE) {
        const unsigned sec = (unsigned)max(0, layer->timer_s);
        char timer[24];
        (void)snprintf(timer, sizeof(timer), "%02u:%02u", sec / 60, sec % 60);
        text(s->status, timer);
    } else text(s->status, s->page == MONITOR && !v->status ? "未连接" : v->status);
    if (s->slider) {
        for (size_t i = 0; i < s->n; ++i) if (s->controls[i].element == XIAOTAI_UI_ELEMENT_FIELD) {
            const uint16_t index = s->controls[i].index;
            if (index >= v->field_count) break;
            const xiaotai_view_field_t *field = &v->fields[index];
            char value[24];
            s->field_min = field->min;
            s->field_max = field->max;
            s->field_step = max(1, field->step);
            const int64_t span = (int64_t)field->max - field->min;
            const int32_t slots = (int32_t)((span + s->field_step - 1) / s->field_step);
            const int32_t offset = (int32_t)(((int64_t)field->value - field->min + s->field_step / 2) / s->field_step);
            lv_slider_set_range(s->slider, 0, slots);
            lv_slider_set_value(s->slider, field->value == field->max ? slots : offset, LV_ANIM_OFF);
            (void)snprintf(value, sizeof(value), "%ld%%", (long)field->value);
            text(s->percent, value);
            break;
        }
    }
    if (s->page == DEVICES && s->body) lv_obj_set_y(s->body, v->subtitle ? 88 : 24);
    s->updating = false;
}

static int match(const xiaotai_display_desc_t *d)
{
    return d && d->pixel == XIAOTAI_PIXEL_RGB565 && d->width >= 800 && d->height >= 480 ? 100 : 0;
}

static xiaotai_err_t open(const xiaotai_ui_host_t *host, const xiaotai_ui_canvas_t *c,
                         const xiaotai_ui_layer_t *layer, void **out)
{
    if (!out || !c || !c->root || c->backend != XIAOTAI_UI_BACKEND_LVGL || !layer) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    wide_t *s = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s));
    if (!s) return XIAOTAI_ERR_NO_MEM;
    const char *name = layer->view.custom;
    const char *id = layer->view.id;
    s->page = equal(name, XIAOTAI_APP_DOOR_TEMPLATE_HOME) ? HOME : equal(name, XIAOTAI_APP_DOOR_TEMPLATE_DEVICES) ? DEVICES :
        equal(name, XIAOTAI_APP_DOOR_TEMPLATE_MONITOR) ? MONITOR : equal(name, XIAOTAI_APP_DOOR_TEMPLATE_CALL) ? CALL :
        equal(id, XIAOTAI_APP_DOOR_OVERLAY_VOLUME) ? VOLUME : equal(id, XIAOTAI_APP_DOOR_OVERLAY_RESULT) ? RESULT : CONFIRM;
    s->host = host;
    s->layer = layer->view.layer;
    s->width = c->width;
    s->height = c->height;
    s->root = c->root;
    s->full = lv_obj_get_style_text_font(c->root, LV_PART_MAIN);
    s->button_font = door_font_button;
    s->button_font.fallback = s->full;
    s->body_font = door_font_body;
    s->body_font.fallback = s->full;
    s->sheet = box(c->root, 0, 0, c->width, c->height, BG, LV_OPA_COVER);
    if (!s->sheet) { xiaotai_mem_free(s); return XIAOTAI_ERR_NO_MEM; }
    *out = s;
    update(s, c, layer, NULL);
    return XIAOTAI_OK;
}

static void close(void *state)
{
    wide_t *s = state;
    s->updating = true;
    if (s->video) (void)xiaotai_ui_host_video_region(s->host, NULL);
    release_sheet(s);
    if (s->toast_box) lv_obj_delete(s->toast_box);
    lv_obj_delete(s->sheet);
    xiaotai_mem_free(s);
}

static bool toast(void *state, const char *message)
{
    wide_t *s = state;
    if (!message) { show(s->toast_box, false); return true; }
    if (!s->toast_box) {
        const int w = min(620, s->width - 48);
        lv_obj_t *top = lv_display_get_layer_top(lv_obj_get_display(s->root));
        s->toast_box = box(top, (s->width - w) / 2, s->height - 176, w, 48, 0x171f24, LV_OPA_90);
        if (s->toast_box) lv_obj_set_style_radius(s->toast_box, 8, 0);
        s->toast_label = label(s->toast_box, 12, 5, w - 24, &s->button_font, BODY, LV_TEXT_ALIGN_CENTER);
    }
    if (!s->toast_label) return false;
    text(s->toast_label, message);
    show(s->toast_box, true);
    return true;
}

static size_t controls(void *state, xiaotai_ui_control_t *out, size_t capacity)
{
    const wide_t *s = state;
    size_t count = 0;
    lv_obj_update_layout(s->root);
    for (size_t i = 0; i < s->n; ++i) {
        const control_t *c = &s->controls[i];
        lv_area_t a;
        lv_obj_get_coords(c->object, &a);
        if (c->element == XIAOTAI_UI_ELEMENT_ITEM && s->list) {
            lv_area_t visible;
            lv_obj_get_coords(s->list, &visible);
            a.x1 = max(a.x1, visible.x1);
            a.y1 = max(a.y1, visible.y1);
            a.x2 = min(a.x2, visible.x2);
            a.y2 = min(a.y2, visible.y2);
            if (a.x1 > a.x2 || a.y1 > a.y2) continue;
        }
        if (out && count < capacity)
            out[count] = (xiaotai_ui_control_t){.x = (int16_t)a.x1, .y = (int16_t)a.y1,
                .width = (uint16_t)lv_area_get_width(&a), .height = (uint16_t)lv_area_get_height(&a),
                .layer = s->layer, .element = c->element, .index = c->index};
        ++count;
    }
    return count;
}

xiaotai_err_t door_templates_register(void)
{
#define TEMPLATE(id) {.name = id, .backend = XIAOTAI_UI_BACKEND_LVGL, .match = match, \
                      .open = open, .update = update, .close = close, .toast = toast, .controls = controls}
    static const xiaotai_ui_template_t templates[] = {
        TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_HOME), TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_DEVICES),
        TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_MONITOR), TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_CALL),
        TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_DIALOG),
    };
#undef TEMPLATE
    for (size_t i = 0; i < sizeof(templates) / sizeof(*templates); ++i) {
        const xiaotai_err_t err = xiaotai_ui_template_register(&templates[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}
