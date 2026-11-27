// SPDX-License-Identifier: MIT
#include <string.h>
#include "lcd_internal.h"
#include "lcd_face_live.h"
#include "src/display/lv_display_private.h"
#include "src/misc/lv_text_private.h"
#include "xiaotai/port/mem.h"

/* A page is built of dozens of these while somebody waits for it, and a style
 * property costs tens of microseconds on the slower chips: the toolkit's own
 * theme is kept out, and what an object inherits or starts with is not set. */
static lv_obj_t *bare(lv_obj_t *parent, lv_obj_t *(*create)(lv_obj_t *))
{
    lv_display_t *const display = lv_display_get_default();
    lv_theme_t *const theme = display->theme;
    display->theme = NULL;
    lv_obj_t *obj = create(parent);
    display->theme = theme;
    return obj;
}

lv_obj_t *lcd_panel(lv_obj_t *parent, lcd_box_t box, uint32_t color, lv_opa_t opacity, int32_t radius)
{
    lv_obj_t *obj = bare(parent, lv_obj_create);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    if (box.x || box.y) lv_obj_set_pos(obj, box.x, box.y);
    lv_obj_set_size(obj, box.width, box.height);
    if (opacity != LV_OPA_TRANSP) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(obj, opacity, 0);
    }
    if (radius) lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

lv_obj_t *lcd_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = bare(parent, lv_label_create);
    lv_label_set_text_static(obj, "");   /* LVGL starts a label as "Text"; one never filled must stay blank */
    if (lv_obj_get_style_text_font(obj, 0) != font) lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

void lcd_text(lv_obj_t *obj, const char *text)
{
    if (!text) text = "";
    if (obj && strcmp(lv_label_get_text(obj), text)) lv_label_set_text(obj, text);
}

static size_t char_bytes(const char *at)
{
    const unsigned char lead = (unsigned char)*at;
    size_t n = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
    while (n > 1 && memchr(at, '\0', n)) --n;
    return n;
}

static bool one_of(const char *set, const char *at)
{
    const size_t size = *at ? char_bytes(at) : 0;
    for (; size && *set; set += char_bytes(set))
        if (char_bytes(set) == size && !memcmp(set, at, size)) return true;
    return false;
}

static const char *char_before(const char *start, const char *at)
{
    if (at <= start) return start;
    do --at; while (at > start && ((unsigned char)*at & 0xc0) == 0x80);
    return at;
}

static bool closing(const char *at) { return one_of("，。、；：？！”’）》】…·,.;:?!)%]", at); }
static bool word(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool joint(char c) { return c == ':' || c == '-' || c == '.' || c == '/' || c == '_'; }

static bool opening(const char *at) { return one_of("“‘（《【([", at); }

void lcd_prose(lcd_state_t *s, lv_obj_t *obj, const char *text, int32_t width)
{
    if (!text) text = "";
    /* Every line break takes at most one character of its own. */
    const size_t size = strlen(text);
    char *out = size && width > 0 ? xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 2 * size + 1) : NULL;
    if (!out) {
        lcd_text(obj, text);
        return;
    }
    lv_text_attributes_t attributes = {0};
    attributes.max_width = width;
    size_t used = 0;
    const char *at = text;
    while (*at) {
        const uint32_t n = lv_text_get_next_line(at, LV_TEXT_LEN_MAX, s->m.text, NULL, &attributes);
        if (!n) break;
        const char *next = at + n, *cut = next;
        if (*next && next[-1] != '\n') {
            for (int moved = 0; moved < 3 && cut > at; ++moved) {
                const char *last = char_before(at, cut);
                if (!closing(cut) && !opening(last)) break;
                cut = last;
            }
            /* "7:30", "Wi-Fi": LVGL breaks after the mark, the reader does not. */
            if (cut > at && word(*cut) && joint(cut[-1])) {
                const char *start = cut;
                while (start > at && (word(start[-1]) || joint(start[-1]))) --start;
                if (start > at && cut - start <= 12) cut = start;
            }
        }
        if (cut <= at) cut = next;   /* a line of nothing but marks stays as it is */
        memcpy(out + used, at, (size_t)(cut - at));
        used += (size_t)(cut - at);
        if (cut != next) out[used++] = '\n';
        at = cut;
    }
    const size_t rest = strlen(at);
    memcpy(out + used, at, rest + 1U);
    lcd_text(obj, out);
    xiaotai_mem_free(out);
}

void lcd_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lcd_layout_t lcd_canvas_units(const lcd_layer_t *layer)
{
    return (lcd_layout_t){
        (uint16_t)(layer->canvas.width * XIAOTAI_THEME_LCD_LOGICAL_WIDTH / layer->unit.width),
        (uint16_t)(layer->canvas.height * XIAOTAI_THEME_LCD_LOGICAL_HEIGHT / layer->unit.height)};
}

int32_t lcd_dp(const lcd_state_t *s, int32_t pixels) { return (pixels * s->m.dp + 8) / 16; }

int32_t lcd_line_height(const lcd_state_t *s) { return lv_font_get_line_height(s->m.text); }

void lcd_surface_style(lcd_state_t *s, lv_obj_t *obj, bool pressable)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_SURFACE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, lcd_dp(s, LCD_RADIUS), 0);
    if (pressable) lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_SURFACE_HIGH), LV_STATE_PRESSED);
}

void lcd_scroll_style(lcd_state_t *s, lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_FAINT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(obj, lcd_dp(s, 3), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(obj, 1, LV_PART_SCROLLBAR);
}

static void control_event(lv_event_t *event)
{
    lcd_control_t *c = lv_event_get_user_data(event);
    xiaotai_ui_input_t input;
    const lv_event_code_t code = lv_event_get_code(event);
    /* The circle of a round button shows the press of its whole touch area. */
    if (c->badge && code == LV_EVENT_PRESSED) lv_obj_add_state(c->badge, LV_STATE_PRESSED);
    if (c->badge && (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST))
        lv_obj_remove_state(c->badge, LV_STATE_PRESSED);
    switch (code) {
    case LV_EVENT_CLICKED:
        if (c->face && lcd_face_live_stroked(c->state)) return;
        input = XIAOTAI_UI_INPUT_TAP;
        break;
    case LV_EVENT_PRESSED:
        c->pointer_down = c->pointer_hold;
        input = XIAOTAI_UI_INPUT_PRESS;
        break;
    case LV_EVENT_PRESSING:
        if (!c->pointer_down) return;
        input = XIAOTAI_UI_INPUT_HOLD_REFRESH;
        break;
    case LV_EVENT_DELETE:
        if (!c->pointer_hold && !c->pointer_down) return;
        c->obj = c->label = c->image = c->icon = c->badge = NULL;
        if (!c->pointer_down) return;
        c->pointer_down = false;
        input = XIAOTAI_UI_INPUT_RELEASE;
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (c->pointer_hold && !c->pointer_down) return;
        c->pointer_down = false;
        input = XIAOTAI_UI_INPUT_RELEASE;
        break;
    case LV_EVENT_LONG_PRESSED: input = XIAOTAI_UI_INPUT_LONG_PRESS; break;
    default: return;
    }
    xiaotai_ui_host_input(c->state->host, c->layer, c->element, c->index, input, 0, NULL);
}

void lcd_bind(lcd_control_t *control, lcd_state_t *state, lv_obj_t *parent,
              uint8_t layer, uint8_t element, uint16_t index)
{
    if (control->obj) return;
    *control = (lcd_control_t){.state = state, .layer = layer, .element = element, .index = index};
    control->obj = lcd_panel(parent, (lcd_box_t){0}, 0, LV_OPA_TRANSP, lcd_dp(state, LCD_RADIUS));
    control->label = lcd_label(control->obj, state->m.text, LCD_TEXT);
    lv_label_set_long_mode(control->label, LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(control->obj, control_event, LV_EVENT_ALL, control);
}

/* Most controls have words or a picture, not both: the picture's label comes
 * with the first one. */
static lv_obj_t *icon_of(lcd_control_t *control)
{
    if (!control->icon) control->icon = lcd_label(control->obj, control->state->m.icons, LCD_TEXT);
    return control->icon;
}

typedef struct { uint32_t fill, pressed, ink; lv_opa_t opacity; } tone_colors_t;

static tone_colors_t tone_colors(lcd_tone_t tone)
{
    switch (tone) {
    case LCD_TONE_ACCENT: return (tone_colors_t){LCD_ACCENT, 0x46a6cc, LCD_ACCENT_INK, LV_OPA_COVER};
    case LCD_TONE_POSITIVE: return (tone_colors_t){LCD_POSITIVE, 0x18875c, LCD_TEXT, LV_OPA_COVER};
    case LCD_TONE_DESTRUCTIVE: return (tone_colors_t){LCD_DESTRUCTIVE, 0xb5353e, LCD_TEXT, LV_OPA_COVER};
    case LCD_TONE_QUIET: return (tone_colors_t){LCD_SURFACE_HIGH, LCD_SURFACE_HIGH, LCD_TEXT, LV_OPA_TRANSP};
    default: return (tone_colors_t){LCD_SURFACE_HIGH, LCD_OUTLINE, LCD_TEXT, LV_OPA_COVER};
    }
}

void lcd_enable(lv_obj_t *obj, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void fill(lv_obj_t *obj, tone_colors_t colors, bool enabled)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(colors.fill), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(colors.pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, colors.opacity == LV_OPA_COVER && !enabled ? LV_OPA_40 : colors.opacity, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
}

void lcd_control_update(lcd_control_t *control, lcd_box_t box, const char *label, const char *symbol,
                        bool visible, bool enabled, lcd_tone_t tone)
{
    lcd_visible(control->obj, visible);
    if (!visible) return;
    const lcd_state_t *s = control->state;
    const bool text = label && label[0];
    const tone_colors_t colors = tone_colors(tone);
    const uint32_t ink = enabled ? colors.ink : LCD_FAINT;
    lcd_place(control->obj, box);
    lcd_visible(control->badge, false);
    const int32_t radius = lcd_dp(s, LCD_RADIUS);
    lv_obj_set_style_radius(control->obj, radius < box.height / 2 ? radius : box.height / 2, 0);
    fill(control->obj, colors, enabled);
    lv_obj_set_style_text_color(control->label, lv_color_hex(ink), 0);
    lcd_text(control->label, text ? label : "");
    lcd_visible(control->label, text);

    lv_point_t text_size = {0}, icon_size = {0};
    if (text) lv_text_get_size(&text_size, label, s->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (symbol) lv_text_get_size(&icon_size, symbol, s->m.icons, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t gap = lcd_dp(s, 6);
    /* An icon keeps its place beside a label only when both fit with a margin. */
    const bool icon = symbol && (!text || text_size.x + icon_size.x + gap + 2 * gap <= box.width);
    lcd_visible(icon ? icon_of(control) : control->icon, icon);
    if (icon) {
        lv_obj_set_style_text_color(control->icon, lv_color_hex(ink), 0);
        lcd_text(control->icon, symbol);
    }
    if (icon && text) {
        lv_obj_set_width(control->label, LV_SIZE_CONTENT);
        lv_obj_align(control->icon, LV_ALIGN_CENTER, -(text_size.x + gap) / 2, 0);
        lv_obj_align(control->label, LV_ALIGN_CENTER, (icon_size.x + gap) / 2, 0);
    } else if (icon) {
        lv_obj_align(control->icon, LV_ALIGN_CENTER, 0, 0);
    } else if (text) {
        lv_obj_set_width(control->label, lv_pct(100));
        lv_obj_set_style_text_align(control->label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(control->label, LV_ALIGN_CENTER, 0, 0);
    }
    lcd_enable(control->obj, enabled);
}

void lcd_round_update(lcd_control_t *control, lcd_box_t box, int32_t diameter, const char *label,
                      const char *symbol, bool enabled, lcd_tone_t tone)
{
    lcd_visible(control->obj, true);
    lcd_state_t *s = control->state;
    lcd_place(control->obj, box);
    lv_obj_set_style_bg_opa(control->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(control->obj, LV_OPA_TRANSP, LV_STATE_PRESSED);
    if (!control->badge) {
        control->badge = lcd_panel(control->obj, (lcd_box_t){0}, LCD_SURFACE_HIGH, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        lv_obj_move_to_index(control->badge, 0);
    }
    lcd_visible(control->badge, true);
    lv_obj_set_size(control->badge, diameter, diameter);
    lv_obj_align(control->badge, LV_ALIGN_TOP_MID, 0, 0);
    tone_colors_t colors = tone_colors(tone);
    if (tone == LCD_TONE_QUIET) colors.opacity = LV_OPA_COVER;   /* a circle always shows */
    fill(control->badge, colors, enabled);
    lcd_visible(symbol ? icon_of(control) : control->icon, symbol != NULL);
    if (symbol) {
        lv_obj_set_style_text_color(control->icon, lv_color_hex(enabled ? colors.ink : LCD_FAINT), 0);
        lcd_text(control->icon, symbol);
        lv_obj_align(control->icon, LV_ALIGN_TOP_MID, 0, (diameter - lv_font_get_line_height(s->m.icons)) / 2);
    }
    const bool text = label && label[0];
    lcd_visible(control->label, text);
    if (text) {
        lcd_text(control->label, label);
        lv_obj_set_width(control->label, lv_pct(100));
        lv_obj_set_style_text_align(control->label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(control->label, lv_color_hex(enabled ? LCD_TEXT : LCD_FAINT), 0);
        lv_obj_align(control->label, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    lcd_enable(control->obj, enabled);
}

lcd_tone_t lcd_action_tone(const xiaotai_view_action_t *action)
{
    if (action->style == XIAOTAI_STYLE_DESTRUCTIVE) return LCD_TONE_DESTRUCTIVE;
    if (action->style == XIAOTAI_STYLE_POSITIVE) return LCD_TONE_POSITIVE;
    if (action->style == XIAOTAI_STYLE_QUIET) return LCD_TONE_QUIET;
    return action->role == XIAOTAI_ROLE_PRIMARY && action->label && action->label[0] ? LCD_TONE_ACCENT :
                                                                                       LCD_TONE_NEUTRAL;
}

const char *lcd_icon(const char *name)
{
    static const struct { const char *name, *glyph; } icons[] = {
        {XIAOTAI_ICON_BACK, LCD_ICON_BACK}, {XIAOTAI_ICON_PREVIOUS, LCD_ICON_BACK},
        {XIAOTAI_ICON_NEXT, LCD_ICON_NEXT}, {XIAOTAI_ICON_MENU, LCD_ICON_MORE},
        {XIAOTAI_ICON_PHONE, LCD_ICON_PHONE}, {XIAOTAI_ICON_ACCEPT, LCD_ICON_CHECK},
        {XIAOTAI_ICON_HANGUP, LCD_ICON_HANGUP}, {XIAOTAI_ICON_STOP, LCD_ICON_STOP},
        {XIAOTAI_ICON_WIFI, LCD_ICON_WIFI}, {XIAOTAI_ICON_VOLUME, LCD_ICON_VOLUME},
        {XIAOTAI_ICON_MIC, LCD_ICON_MIC}, {XIAOTAI_ICON_MIC_OFF, LCD_ICON_MIC_OFF},
        {XIAOTAI_ICON_CAMERA, LCD_ICON_VIDEO}, {XIAOTAI_ICON_CAMERA_OFF, LCD_ICON_VIDEO_OFF},
        {XIAOTAI_ICON_SETTINGS, LCD_ICON_SETTINGS}, {XIAOTAI_ICON_REFRESH, LCD_ICON_REFRESH},
        {XIAOTAI_ICON_VIDEO, LCD_ICON_VIDEO}, {XIAOTAI_ICON_LOCK, LCD_ICON_LOCK},
        {XIAOTAI_ICON_CONTACTS, LCD_ICON_USER}, {XIAOTAI_ICON_GROUP, LCD_ICON_USERS},
        {XIAOTAI_ICON_FACE, LCD_ICON_SMILE}, {XIAOTAI_ICON_SOUND, LCD_ICON_SOUND},
        {XIAOTAI_ICON_PULSE, LCD_ICON_PULSE}, {XIAOTAI_ICON_ALARM, LCD_ICON_BELL},
        {XIAOTAI_ICON_TIMER, LCD_ICON_TIMER}, {XIAOTAI_ICON_CLOCK, LCD_ICON_CLOCK},
        {XIAOTAI_ICON_DOOR, LCD_ICON_DOOR}, {XIAOTAI_ICON_KEY, LCD_ICON_KEY},
        {XIAOTAI_ICON_INFO, LCD_ICON_INFO}, {XIAOTAI_ICON_SPEAKER_OFF, LCD_ICON_VOLUME_OFF},
        {XIAOTAI_ICON_CALL_OUT, LCD_ICON_CALL_OUT}, {XIAOTAI_ICON_CALL_IN, LCD_ICON_CALL_IN},
        {XIAOTAI_ICON_CALL_MISSED, LCD_ICON_CALL_MISSED}, {XIAOTAI_ICON_PLAY, LCD_ICON_PLAY},
    };
    for (size_t i = 0; name && i < sizeof(icons) / sizeof(icons[0]); ++i)
        if (!strcmp(name, icons[i].name)) return icons[i].glyph;
    return NULL;
}

bool lcd_measure_control(const lcd_control_t *control, xiaotai_ui_control_t *out)
{
    if (!control->obj || !lv_obj_is_visible(control->obj)) return false;
    if (out) {
        lv_area_t area;
        lv_obj_get_coords(control->obj, &area);
        *out = (xiaotai_ui_control_t){.x = area.x1, .y = area.y1, .width = lv_area_get_width(&area),
            .height = lv_area_get_height(&area), .layer = control->layer, .element = control->element,
            .index = control->index};
    }
    return true;
}
