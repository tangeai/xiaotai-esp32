/* Shared product presentation. Included by starter_product.c so LVGL ownership stays
 * in one task. No network operations, media buffers or additional tasks here. */
#define S3_TEXT 0xF1F3F4
#define S3_CAPTION_AI 0xB5DFFF
#define S3_CAPTION_USER 0xBCE8CF
#define S3_MUTED 0xA6ADB4
#define S3_ACCENT 0x9BD8CF
#define S3_SURFACE 0x22272B
#define S3_DANGER 0xAE3947
#define S3_ACCEPT 0x246F53

#include "starter_product_wechat_qr.inc"
#if CONFIG_IDF_TARGET_ESP32P4
#include "starter_product_p4_phone.inc"
#else
#include "starter_product_s3_phone.inc"
#endif

enum {
    S3_ACTION_AI_STOP = 40,
    S3_ACTION_REFRESH,
    S3_ACTION_CLOCK,
    S3_ACTION_BACK,
    S3_ACTION_AI_TOGGLE,
    S3_ACTION_WECHAT,
    S3_ACTION_QR_CLOSE,
    S3_ACTION_WIFI_DISCONNECT,
    S3_ACTION_BINDING_RETRY,
};

/* UI-only state belongs in external RAM; it is never used with cache off. */
static EXT_RAM_BSS_ATTR struct {
    lv_style_t button_style;
    lv_style_t pressed_style;
    lv_style_t disabled_style;
    bool styles_ready;
    lv_obj_t *caption_panel;
    lv_obj_t *wake_hint;
    lv_obj_t *call_result;
    lv_obj_t *wechat_button;
    lv_obj_t *wechat_qr;
    lv_obj_t *contacts_refresh;
    lv_obj_t *binding_code;
    lv_obj_t *binding_hint;
    lv_obj_t *binding_retry;
    bool setup_active;
    product_page_t contacts_origin;
    lv_obj_t *volume_down;
    lv_obj_t *volume_up;
    lv_obj_t *network_text;
    lv_obj_t *network_disconnect;
    lv_obj_t *toast;
    int64_t toast_until;
    int64_t network_due;
    int64_t pending_until;
    int64_t call_started;
    uint32_t call_generation;
    starter_runtime_state_t pending_state;
    product_action_t pending_action;
    starter_runtime_caption_t caption;
    char network_cache[640];
} s3_ui;

static void s3_style_button(lv_obj_t *button)
{
    /* Reuse immutable styles rather than allocating them for every contact or
     * expression button. The existing 32 KiB LVGL pool stays unchanged. */
    if (!s3_ui.styles_ready) {
        lv_style_init(&s3_ui.button_style);
        lv_style_set_radius(&s3_ui.button_style, 6);
        lv_style_set_pad_all(&s3_ui.button_style, 0);
        lv_style_set_bg_opa(&s3_ui.button_style, LV_OPA_COVER);
        lv_style_set_bg_color(&s3_ui.button_style, lv_color_hex(0x292E32));
        lv_style_set_text_color(&s3_ui.button_style, lv_color_hex(S3_TEXT));
        lv_style_init(&s3_ui.pressed_style);
        lv_style_set_bg_color(&s3_ui.pressed_style, lv_color_hex(0x465057));
        lv_style_init(&s3_ui.disabled_style);
        lv_style_set_bg_color(&s3_ui.disabled_style, lv_color_hex(0x222629));
        lv_style_set_text_color(&s3_ui.disabled_style, lv_color_hex(0x81898F));
        s3_ui.styles_ready = true;
    }
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, &s3_ui.button_style, 0);
    lv_obj_add_style(button, &s3_ui.pressed_style, LV_STATE_PRESSED);
    lv_obj_add_style(button, &s3_ui.disabled_style, LV_STATE_DISABLED);
}

static void s3_feedback(const char *text)
{
    if (s3_ui.toast == NULL) {
        s3_ui.toast = make_label(lv_layer_top(), "", 8, 42, 304,
                                lv_color_hex(S3_TEXT));
        set_bg(s3_ui.toast, lv_color_hex(0x3C4348));
        lv_obj_set_style_pad_all(s3_ui.toast, 8, 0);
        lv_obj_set_style_radius(s3_ui.toast, 6, 0);
        lv_obj_set_style_text_align(s3_ui.toast, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(s3_ui.toast, LV_OBJ_FLAG_CLICKABLE);
    }
    label_set_text_if_changed(s3_ui.toast, text);
    lv_obj_clear_flag(s3_ui.toast, LV_OBJ_FLAG_HIDDEN);
    s3_ui.toast_until = monotonic_ms() + 2400;
}

static void s3_set_enabled(lv_obj_t *object, bool enabled)
{
    if (object == NULL) return;
    if (enabled) lv_obj_clear_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static lv_obj_t *s3_icon(lv_obj_t *parent, const char *symbol,
                        int x, int y, int w, product_action_t action)
{
    lv_obj_t *button = make_button(parent, "", x, y, w, 44, action);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    return button;
}

static void s3_draw_wechat(lv_event_t *event)
{
#if CONFIG_IDF_TARGET_ESP32P4
    const lv_img_dsc_t *icon = &p4_phone_shortcut_image;
#else
    const lv_img_dsc_t *icon = &s3_phone_shortcut_image;
#endif
    /* Keep the source aspect ratio and alpha; the extended hit area is unchanged. */
    lv_obj_t *button = lv_event_get_target(event);
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    const lv_coord_t width = icon->header.w;
    const lv_coord_t height = icon->header.h;
    area.x1 += (lv_area_get_width(&area) - width) / 2;
    area.y1 += (lv_area_get_height(&area) - height) / 2;
    area.x2 = area.x1 + width - 1;
    area.y2 = area.y1 + height - 1;
    lv_draw_img_dsc_t d;
    lv_draw_img_dsc_init(&d);
    lv_obj_init_draw_img_dsc(button, LV_PART_MAIN, &d);
    /* Back the transparent handset with white, inset inside the opaque green
     * disc so the outer antialiased edge keeps its original transparency. */
#if CONFIG_IDF_TARGET_ESP32P4
    lv_area_t backing = {area.x1 + 2, area.y1 + 2, area.x2 - 2, area.y2 - 2};
#else
    lv_area_t backing = {area.x1 + 4, area.y1 + 4, area.x2 - 4, area.y2 - 4};
#endif
    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = lv_color_white();
    fill.bg_opa = d.opa;
    fill.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(lv_event_get_draw_ctx(event), &fill, &backing);
    lv_draw_img(lv_event_get_draw_ctx(event), &d, &area, icon);
}

static void s3_close_wechat_qr(void)
{
    if (s3_ui.wechat_qr == NULL) return;
    lv_obj_del(s3_ui.wechat_qr);
    s3_ui.wechat_qr = NULL;
    s3_face_visibility_changed(s_display_awake);
}

static void s3_show_wechat_qr(void)
{
    if (s3_ui.wechat_qr != NULL) return;
    lv_obj_t *overlay = lv_obj_create(lv_scr_act());
    s3_ui.wechat_qr = overlay;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 320, 240);
    set_bg(overlay, lv_color_hex(HOME_BACKGROUND_COLOR));
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, on_action, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)S3_ACTION_QR_CLOSE);
    lv_obj_t *title = make_label(overlay, "微信扫码添加联系人", 12, 10, 296, lv_color_hex(S3_TEXT));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *qr = lv_img_create(overlay);
    lv_img_set_src(qr, &s3_wechat_qr_image);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_add_flag(qr, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    s3_face_visibility_changed(false);
    if (s3_ui.toast) lv_obj_add_flag(s3_ui.toast, LV_OBJ_FLAG_HIDDEN);
}

static void s3_header(lv_obj_t *screen, const char *title, product_action_t back)
{
    (void)s3_icon(screen, LV_SYMBOL_LEFT, 4, 0, 44, back);
    s_header_title = make_label(screen, title, 56, 12, 158, lv_color_hex(S3_TEXT));
    lv_label_set_long_mode(s_header_title, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_header_title, 20);
    render_header_status(screen);
}

static lv_obj_t *s3_scroller(lv_obj_t *screen, int y, int height)
{
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_pos(panel, 8, y);
    lv_obj_set_size(panel, 304, height);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(panel, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(panel, lv_color_hex(S3_MUTED), LV_PART_SCROLLBAR);
    return panel;
}

static lv_obj_t *s3_row(lv_obj_t *panel, int y)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, 296, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x343A3F), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void s3_choice_changed(lv_event_t *event)
{
    product_action_t action = (product_action_t)(uintptr_t)lv_event_get_user_data(event);
    uint16_t selected = lv_dropdown_get_selected(lv_event_get_target(event));
    note_interaction();
    if (action == ACTION_SLEEP && selected < sizeof(s_sleep_minutes) / sizeof(s_sleep_minutes[0])) {
        s_preferences.sleep_index = (uint8_t)selected;
    } else if (action == ACTION_ACK_VOICE && selected < 2U) {
        s_preferences.acknowledgement_male = selected != 0;
    } else return;
    preferences_save();
}

static lv_obj_t *s3_choice(lv_obj_t *row, const char *options, product_action_t action)
{
    lv_obj_t *choice = lv_dropdown_create(row);
    lv_obj_set_pos(choice, 156, 2);
    lv_obj_set_size(choice, 138, 44);
    lv_dropdown_set_options(choice, options);
    lv_dropdown_set_symbol(choice, NULL);
    lv_obj_set_style_text_font(choice, &PRODUCT_TEXT_FONT, 0);
    lv_obj_set_style_text_color(choice, lv_color_hex(S3_TEXT), 0);
    set_bg(choice, lv_color_hex(S3_SURFACE));
    lv_obj_set_style_radius(choice, 6, 0);
    lv_obj_t *list = lv_dropdown_get_list(choice);
    lv_obj_set_style_text_font(list, &PRODUCT_TEXT_FONT, 0);
    lv_obj_set_style_text_color(list, lv_color_hex(S3_TEXT), 0);
    lv_obj_set_style_pad_ver(list, 10, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(list, lv_color_hex(S3_ACCEPT), LV_PART_SELECTED | LV_STATE_CHECKED);
    set_bg(list, lv_color_hex(S3_SURFACE));
    lv_obj_t *arrow = make_label(choice, LV_SYMBOL_DOWN, 108, 0, 16, lv_color_hex(S3_MUTED));
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(choice, s3_choice_changed, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)action);
    return choice;
}

static lv_obj_t *s3_switch(lv_obj_t *row, product_action_t action)
{
    lv_obj_t *control = lv_switch_create(row);
    lv_obj_set_pos(control, 234, 10);
    lv_obj_set_size(control, 56, 28);
    /* 44px vertical touch target, contained inside its own 48px row. */
    lv_obj_set_ext_click_area(control, 8);
    lv_obj_set_style_bg_color(control, lv_color_hex(S3_ACCEPT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(control, on_action, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)action);
    return control;
}

static void s3_render_settings(lv_obj_t *screen)
{
    s3_header(screen, "设置", ACTION_MENU);
    lv_obj_t *panel = s3_scroller(screen, 46, 190);
    s3_ui.volume_down = s3_icon(panel, LV_SYMBOL_MINUS, 0, 0, 48, ACTION_VOLUME_DOWN);
    s3_ui.volume_up = s3_icon(panel, LV_SYMBOL_PLUS, 248, 0, 48, ACTION_VOLUME_UP);
    s_settings_volume = make_label(panel, "", 56, 13, 184, lv_color_hex(S3_TEXT));
    lv_obj_set_style_text_align(s_settings_volume, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *row = s3_row(panel, 52);
    (void)make_label(row, "扬声器", 4, 14, 152, lv_color_hex(S3_TEXT));
    s_settings_speaker = s3_switch(row, ACTION_SPEAKER_MUTE);
    row = s3_row(panel, 104);
    (void)make_label(row, "麦克风", 4, 14, 152, lv_color_hex(S3_TEXT));
    s_settings_microphone = s3_switch(row, ACTION_MIC_MUTE);
    row = s3_row(panel, 156);
    (void)make_label(row, "自动息屏", 4, 14, 140, lv_color_hex(S3_TEXT));
    s_settings_sleep = s3_choice(row, "1 分钟\n5 分钟\n10 分钟\n30 分钟\n永不", ACTION_SLEEP);
    row = s3_row(panel, 208);
    (void)make_label(row, "回应声", 4, 14, 140, lv_color_hex(S3_TEXT));
    s_settings_acknowledgement = s3_choice(row, "女声\n男声", ACTION_ACK_VOICE);
    s3_refresh_settings();
}

static void s3_refresh_settings(void)
{
    if (s_page != PAGE_SETTINGS || s_settings_volume == NULL) return;
    char text[40];
    (void)snprintf(text, sizeof(text), "音量  %u / 10", s_preferences.volume);
    label_set_text_if_changed(s_settings_volume, text);
    s3_set_enabled(s3_ui.volume_down, s_preferences.volume > 0);
    s3_set_enabled(s3_ui.volume_up, s_preferences.volume < 10);
    if (s_preferences.speaker_muted) lv_obj_clear_state(s_settings_speaker, LV_STATE_CHECKED);
    else lv_obj_add_state(s_settings_speaker, LV_STATE_CHECKED);
    if (s_preferences.microphone_muted) lv_obj_clear_state(s_settings_microphone, LV_STATE_CHECKED);
    else lv_obj_add_state(s_settings_microphone, LV_STATE_CHECKED);
    lv_dropdown_set_selected(s_settings_sleep, s_preferences.sleep_index);
    lv_dropdown_set_selected(s_settings_acknowledgement, s_preferences.acknowledgement_male ? 1 : 0);
}

static void s3_render_home(lv_obj_t *screen)
{
#if CONFIG_IDF_TARGET_ESP32P4
    p4_home_background_show(screen);
#endif
    /* Keep the original robot mark and brand; state text owns its own column. */
    render_header(screen, "小钛");
    lv_obj_set_width(s_header_title, 36);
#if CONFIG_IDF_TARGET_ESP32P4
    lv_obj_set_pos(s_wifi_signal, 244, 6);
    lv_obj_set_pos(s_clock, 200, 8);
    lv_obj_set_width(s_clock, 40);
    lv_label_set_long_mode(s_clock, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(S3_ACCENT), 0);
#else
    /* The 22 px icon ends before the menu's extended hit area at x=260. */
    lv_obj_set_pos(s_wifi_signal, 238, 7);
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
#endif
    s_state = make_label(screen, "待唤醒", 80, 8, 100, lv_color_hex(S3_ACCENT));
    lv_label_set_long_mode(s_state, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_state, 20);
#if CONFIG_IDF_TARGET_ESP32P4
    /* Text uses physical font metrics; share the WiFi/menu vertical centre. */
    lv_obj_t *header_labels[] = {s_header_title, s_state, s_clock};
    for (unsigned i = 0; i < sizeof(header_labels) / sizeof(header_labels[0]); ++i) {
        lv_coord_t line_height = lv_font_get_line_height(
            lv_obj_get_style_text_font(header_labels[i], LV_PART_MAIN));
        lv_obj_set_style_height(header_labels[i], line_height, 0);
        lv_obj_set_style_y(header_labels[i], product_y(18) - line_height / 2, 0);
    }
#endif
    lv_obj_t *more = s3_icon(screen, LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET,
                             280, 6, 32, ACTION_MENU);
    lv_obj_set_height(more, 24);
    lv_obj_set_style_bg_opa(more, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(more, LV_OPA_30, LV_STATE_PRESSED);
    /* Click padding is physical: S3 shows 32x24 px but accepts x=260..319,
     * y=0..49. It ends above the face and right arrow; P4 keeps its scaling. */
    lv_obj_set_ext_click_area(more, 20);
#if CONFIG_IDF_TARGET_ESP32P4
    s3_ui.wechat_button = make_button(screen, "", 282, 156, 32, 32,
                                      (product_action_t)S3_ACTION_WECHAT);
    /* Physical hit area: x=409..479, y=194..263. Keep clear of the arrow,
     * face tap and captions while shrinking only the visible shortcut. */
    lv_obj_set_ext_click_area(s3_ui.wechat_button, 14);
#else
    s3_ui.wechat_button = make_button(screen, "", 276, 150, 44, 44,
                                      (product_action_t)S3_ACTION_WECHAT);
#endif
    lv_obj_set_style_bg_opa(s3_ui.wechat_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s3_ui.wechat_button, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s3_ui.wechat_button, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_img_opa(s3_ui.wechat_button, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_img_opa(s3_ui.wechat_button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s3_ui.wechat_button, s3_draw_wechat, LV_EVENT_DRAW_MAIN, NULL);
    if (s_page == PAGE_HOME_CLOCK) {
        s_face = make_label(screen, "--:--", 24, 60, 272, lv_color_hex(S3_TEXT));
        lv_obj_set_style_text_font(s_face, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_align(s_face, LV_TEXT_ALIGN_CENTER, 0);
        s_date = make_label(screen, "", 40, 118, 240, lv_color_hex(S3_MUTED));
        lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        create_expression_face(screen, (320 - FACE_CANVAS_W) / 2,
                                (240 - FACE_CANVAS_H) / 2);
    }
    s3_ui.caption_panel = s3_scroller(screen, 200, 34);
    lv_obj_clear_flag(s3_ui.caption_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                     LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_scrollbar_mode(s3_ui.caption_panel, LV_SCROLLBAR_MODE_OFF);
    s_subtitle = make_label(s3_ui.caption_panel, "", 8, 0, 284, lv_color_hex(S3_TEXT));
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_subtitle, 4, 0);
    /* Font metrics and align offsets are physical pixels. Bottom alignment
     * clips older lines without copying UTF-8 text or adding scroll latency. */
#if CONFIG_IDF_TARGET_ESP32P4
    const lv_coord_t bottom_inset = product_y(6);
#else
    /* Two 20 px lines + 4 px spacing occupy y=194..237 on S3; the 44 px
     * WeChat shortcut ends at y=193, so neither text nor hit targets overlap. */
    const lv_coord_t bottom_inset = 2;
#endif
    lv_coord_t caption_height = 2 * lv_font_get_line_height(
        lv_obj_get_style_text_font(s_subtitle, LV_PART_MAIN)) +
        lv_obj_get_style_text_line_space(s_subtitle, LV_PART_MAIN);
    lv_obj_set_style_height(s3_ui.caption_panel, caption_height, 0);
    lv_obj_align(s3_ui.caption_panel, LV_ALIGN_BOTTOM_MID, 0, -bottom_inset);
    lv_obj_align(s_subtitle, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* Populate a cached long caption only after its new viewport is laid out. */
    /* A persistent idle hint, separate from the caption scroll position.
     * Runtime state owns visibility; no periodic popup or wake side effect. */
    s3_ui.wake_hint = make_label(screen, "", 16, 180, 288, lv_color_hex(S3_MUTED));
    lv_label_set_long_mode(s3_ui.wake_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s3_ui.wake_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s3_ui.wake_hint, 5, 0);
    lv_obj_clear_flag(s3_ui.wake_hint, LV_OBJ_FLAG_CLICKABLE);
#if CONFIG_IDF_TARGET_ESP32S3
    lv_obj_set_style_text_line_space(s3_ui.wake_hint, 4, 0);
#endif
    lv_obj_align(s3_ui.wake_hint, LV_ALIGN_BOTTOM_MID, 0, -bottom_inset);
    /* HOME_SUBTITLE_TRANSPARENT: no separate bubble or animated overlay. */
    lv_obj_t *tap = lv_obj_create(screen);
    lv_obj_set_pos(tap, 48, (240 - FACE_CANVAS_H) / 2);
    lv_obj_set_size(tap, 224, FACE_CANVAS_H);
    lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap, 0, 0);
    lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tap, on_home_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *left = s3_icon(screen, LV_SYMBOL_LEFT, 0, 94, 44,
                             (product_action_t)S3_ACTION_CLOCK);
    lv_obj_t *right = s3_icon(screen, LV_SYMBOL_RIGHT, 276, 94, 44,
                              (product_action_t)S3_ACTION_CLOCK);
    lv_obj_t *arrows[] = {left, right};
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_height(arrows[i], 52);
        lv_obj_set_style_bg_opa(arrows[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(arrows[i], LV_OPA_TRANSP, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(arrows[i], lv_color_hex(0x788187), 0);
        lv_obj_set_style_text_color(arrows[i], lv_color_hex(S3_MUTED), LV_STATE_PRESSED);
    }
    /* The face canvas includes a blank top margin. Keep header hit targets
     * above that margin and above the transparent face-tap object. */
    lv_obj_move_foreground(s_state);
    lv_obj_move_foreground(s_wifi_signal);
#if CONFIG_IDF_TARGET_ESP32P4
    lv_obj_move_foreground(s_clock);
#endif
    lv_obj_move_foreground(more);
    lv_obj_move_foreground(s3_ui.wechat_button);
}

#include "starter_product_room.inc"

static void s3_render_menu(lv_obj_t *screen)
{
    s3_header(screen, "更多功能", ACTION_HOME);
    (void)make_button(screen, "通讯录", 8, 46, 304, 37, ACTION_CONTACTS);
    lv_obj_t *room = make_button(screen, "多人对讲", 8, 89, 304, 37, ACTION_ROOM);
    lv_obj_set_style_bg_color(room, lv_color_hex(0x233D37), 0);
    (void)make_label(screen, "设备", 12, 132, 280, lv_color_hex(S3_MUTED));
    (void)make_button(screen, "设备设置", 8, 155, 148, 36, ACTION_SETTINGS);
    (void)make_button(screen, "待机表情", 164, 155, 148, 36, ACTION_EMOJIS);
    (void)make_button(screen, "网络信息", 8, 197, 148, 36, ACTION_NETWORK);
    (void)make_button(screen, "运行状态", 164, 197, 148, 36, ACTION_DIAGNOSTICS);
}

static void s3_render_contacts(lv_obj_t *screen)
{
    starter_runtime_product_snapshot_t product = starter_runtime_product_snapshot();
    s3_header(screen, "通讯录", (product_action_t)S3_ACTION_BACK);
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_wifi_signal, 240, 10);
    s3_ui.contacts_refresh = s3_icon(screen, LV_SYMBOL_REFRESH, 272, 0, 44,
                                     (product_action_t)S3_ACTION_REFRESH);
    lv_obj_t *list = s3_scroller(screen, 48, 188);
    if (!product.contact_count) {
        (void)make_label(list, "暂无联系人", 12, 16, 280, lv_color_hex(S3_TEXT));
        (void)make_label(list, "绑定设备或完成微信授权后同步", 12, 48, 280, lv_color_hex(S3_MUTED));
    }
    for (uint8_t i = 0; i < product.contact_count; ++i) {
        const starter_product_contact_t *contact = &product.contacts[i];
        lv_obj_t *row = make_button(list, "", 0, i * 66, 296, 60,
                                    (product_action_t)(ACTION_CONTACT_BASE + i));
        lv_obj_t *name = make_label(row, contact->name, 12, 8, 232, lv_color_hex(S3_TEXT));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_height(name, 20);
        const char *detail = contact->source == STARTER_CONTACT_WECHAT ? "微信联系人" :
                              contact->online ? "设备 / 在线" : "设备 / 离线";
        (void)make_label(row, detail, 12, 34, 232,
            lv_color_hex(contact->source == STARTER_CONTACT_DEVICE && contact->online ? S3_ACCENT : S3_MUTED));
        lv_obj_t *chevron = make_label(row, LV_SYMBOL_RIGHT, 268, 22, 16, lv_color_hex(S3_MUTED));
        lv_obj_set_style_text_font(chevron, &lv_font_montserrat_14, 0);
    }
}

static void s3_render_contact_detail(lv_obj_t *screen)
{
    starter_runtime_product_snapshot_t product = starter_runtime_product_snapshot();
    s3_header(screen, "联系人", ACTION_CONTACTS_BACK);
    if (s_selected_contact >= product.contact_count) {
        (void)make_label(screen, "联系人已更新，请返回重试", 16, 84, 288, lv_color_hex(S3_MUTED));
        return;
    }
    const starter_product_contact_t *contact = &product.contacts[s_selected_contact];
    lv_obj_t *name = make_label(screen, contact->name, 16, 60, 288, lv_color_hex(S3_TEXT));
    lv_obj_set_height(name, 44);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    const char *state = contact->source == STARTER_CONTACT_WECHAT ? "微信语音" :
                        contact->online ? "设备在线" : "设备离线";
    (void)make_label(screen, state, 16, 116, 288, lv_color_hex(S3_ACCENT));
    /* OpenID is an internal routing key, not a useful contact identifier. */
    lv_obj_t *id = make_label(screen, contact->source == STARTER_CONTACT_WECHAT ?
        "微信联系人" : contact->id, 16, 144, 288, lv_color_hex(S3_MUTED));
    lv_label_set_long_mode(id, LV_LABEL_LONG_DOT);
    lv_obj_set_height(id, 36);
#if CONFIG_IDF_TARGET_ESP32P4
    lv_obj_t *call = make_button(screen, "语音呼叫", 8, 192, 148, 44, ACTION_VOICE_CALL);
    (void)make_button(screen, "视频呼叫", 164, 192, 148, 44, ACTION_VIDEO_CALL);
#else
    lv_obj_t *call = make_button(screen, "语音呼叫", 8, 192, 304, 44, ACTION_VOICE_CALL);
#endif
    lv_obj_set_style_bg_color(call, lv_color_hex(S3_ACCEPT), 0);
}

static void s3_render_call(lv_obj_t *screen)
{
    render_header(screen, "语音通话");
    s_call_peer_label = make_label(screen, "", 16, 58, 288, lv_color_hex(S3_TEXT));
    lv_obj_set_style_text_align(s_call_peer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(s_call_peer_label, 44);
    lv_label_set_long_mode(s_call_peer_label, LV_LABEL_LONG_DOT);
    s_call_status_label = make_label(screen, "", 16, 116, 288, lv_color_hex(S3_ACCENT));
    lv_obj_set_style_text_align(s_call_status_label, LV_TEXT_ALIGN_CENTER, 0);
    s_call_accept_button = make_button(screen, "接听", 164, 180, 148, 56, ACTION_CALL_ACCEPT);
    s_call_reject_button = make_button(screen, "拒绝", 8, 180, 148, 56, ACTION_CALL_REJECT);
    s_call_mute_button = make_button(screen, "关闭麦克风", 8, 180, 148, 56, ACTION_CALL_MUTE);
    s_call_hangup_button = make_button(screen, "挂断", 164, 180, 148, 56, ACTION_CALL_HANGUP);
    lv_obj_set_style_bg_color(s_call_accept_button, lv_color_hex(S3_ACCEPT), 0);
    lv_obj_set_style_bg_color(s_call_reject_button, lv_color_hex(S3_DANGER), 0);
    lv_obj_set_style_bg_color(s_call_hangup_button, lv_color_hex(S3_DANGER), 0);
}

static void s3_network_text(void)
{
    char details[640];
    if (wifi_manager_connected() && s3_ui.setup_active) {
        (void)snprintf(details, sizeof(details), "Wi-Fi 已连接\n\n正在检查设备配置…");
    } else if (wifi_manager_connected()) {
        wifi_config_t config = {0};
        esp_netif_ip_info_t ip = {0};
        (void)esp_wifi_get_config(WIFI_IF_STA, &config);
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) (void)esp_netif_get_ip_info(netif, &ip);
        int8_t rssi = 0;
        (void)wifi_manager_signal_dbm(&rssi);
        char device_id[65] = {0};
        starter_runtime_copy_device_id(device_id, sizeof(device_id));
        (void)snprintf(details, sizeof(details),
            "Wi-Fi 已连接\n%.32s\n\nIP  " IPSTR "\n信号  %d dBm\n\n设备ID\n%s\n\n平台服务  %s\n消息服务  %s",
            (char *)config.sta.ssid, IP2STR(&ip.ip), (int)rssi, device_id,
            platform_client_ready() ? "已连接" : "未连接",
            platform_client_mqtt_connected() ? "已连接" : "未连接");
    } else if (wifi_manager_provisioning()) {
        (void)snprintf(details, sizeof(details),
            "热点配网\n\n1. 手机连接热点\n%s（无需密码）\n\n2. 用手机端打开并连接\n%s\n\n%s",
            wifi_manager_provisioning_ssid(), wifi_manager_provisioning_url(),
            wifi_manager_provisioning_status());
    } else if (wifi_manager_manually_disconnected()) {
        (void)snprintf(details, sizeof(details), "正在开启热点配网…");
    } else {
        (void)snprintf(details, sizeof(details), "%s",
            wifi_manager_connection_failed() ? "正在重连已保存的 Wi-Fi…" : "正在连接 Wi-Fi…");
    }
    if (strcmp(s3_ui.network_cache, details)) {
        (void)snprintf(s3_ui.network_cache, sizeof(s3_ui.network_cache), "%s", details);
        label_set_text_if_changed(s3_ui.network_text, details);
    }
    bool connected = wifi_manager_connected();
    bool show_action = connected || !wifi_manager_provisioning();
    set_object_visible(s3_ui.network_disconnect, show_action);
    set_button_text(s3_ui.network_disconnect, connected ? "断开当前连接" : "热点配网");
    lv_obj_set_height(lv_obj_get_parent(s3_ui.network_text), show_action ? 132 : 188);
    s3_set_enabled(s3_ui.network_disconnect, !wifi_manager_control_pending());
    if (wifi_manager_control_error() != ESP_OK) {
        label_set_text_if_changed(s3_ui.network_text,
            connected ? "断开失败，请重试" : "热点启动失败，请点击热点配网重试");
        s3_ui.network_cache[0] = '\0';
    }
}

static void s3_render_network(lv_obj_t *screen)
{
    if (s3_ui.setup_active) render_header(screen, "Wi-Fi 配网");
    else s3_header(screen, "网络信息", ACTION_NETWORK_BACK);
    lv_obj_t *panel = s3_scroller(screen, 48, 132);
    s3_ui.network_text = make_label(panel, "", 8, 0, 284, lv_color_hex(S3_TEXT));
    lv_obj_set_style_text_line_space(s3_ui.network_text, 5, 0);
    s3_ui.network_disconnect = make_button(screen, "断开当前连接", 8, 188, 304, 48,
        (product_action_t)S3_ACTION_WIFI_DISCONNECT);
    s3_network_text();
}

static void s3_render_binding(lv_obj_t *screen)
{
    render_header(screen, "绑定设备");
    (void)make_label(screen, "网络已连接", 16, 50, 240, lv_color_hex(S3_ACCENT));
    /* BINDING_CODE_TOP_LEVEL: digits have their own measured, unclipped area. */
    lv_obj_t *code = make_label(screen, s_previous_verification_code[0] ?
        s_previous_verification_code : "------", 16, 90, 288, lv_color_hex(S3_TEXT));
    s3_ui.binding_code = code;
    lv_obj_set_style_text_font(code, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
    (void)make_label(screen, "打开下方网址，输入上方验证码", 16, 150, 288, lv_color_hex(S3_TEXT));
    lv_obj_t *website = make_label(screen, "https://xiaotai.chat/",
                                   16, 176, 288, lv_color_hex(S3_ACCENT));
    lv_obj_set_style_text_align(website, LV_TEXT_ALIGN_CENTER, 0);
    s3_ui.binding_hint = make_label(screen, "正在获取验证码…", 16, 210, 288, lv_color_hex(S3_MUTED));
    s3_ui.binding_retry = s3_icon(screen, LV_SYMBOL_REFRESH, 268, 44, 44,
                                (product_action_t)S3_ACTION_BINDING_RETRY);
    lv_obj_add_flag(s3_ui.binding_retry, LV_OBJ_FLAG_HIDDEN);
}

static void s3_update_setup(int64_t now, bool connected, bool idle, bool provisioning,
                            const char *verification_code)
{
    /* Offline configuration owns navigation even if an old session is still
     * tearing down. Its lifecycle remains with runtime; never clear it here. */
    starter_product_binding_state_t binding = atomic_load_explicit(&s_binding_ui_state, memory_order_acquire);
    if (platform_client_reconciling() && binding != STARTER_BINDING_FAILED)
        binding = platform_client_known_unbound() ? STARTER_BINDING_REQUIRED : STARTER_BINDING_CHECKING;
    if (connected && !idle && !s3_ui.setup_active && binding == STARTER_BINDING_READY) return;
    product_page_t target = s_page;
    if (!connected) {
        s3_ui.setup_active = true;
        target = PAGE_NETWORK;
    } else if (binding != STARTER_BINDING_READY || provisioning) {
        s3_ui.setup_active = true;
        target = PAGE_BINDING;
        (void)snprintf(s_previous_verification_code, sizeof(s_previous_verification_code), "%s",
                       provisioning && verification_code ? verification_code : "");
    } else if (s3_ui.setup_active) {
        s3_ui.setup_active = false;
        s_previous_verification_code[0] = '\0';
        if (s_page == PAGE_NETWORK || s_page == PAGE_BINDING) target = PAGE_HOME_FACE;
    }
    if (target != s_page) {
        s_page = target;
        render_page();
        note_interaction();
        ESP_LOGI(TAG, "setup view: page=%u wifi=%u binding=%u", (unsigned)target,
                 (unsigned)connected, (unsigned)binding);
    }
    if (s3_ui.setup_active) note_interaction();
    if (s_page == PAGE_BINDING && s3_ui.binding_code != NULL) {
        label_set_text_if_changed(s3_ui.binding_code,
            s_previous_verification_code[0] ? s_previous_verification_code : "------");
        label_set_text_if_changed(s3_ui.binding_hint, binding == STARTER_BINDING_FAILED ?
            "绑定未完成，请点击右上角重试" : binding == STARTER_BINDING_CHECKING ?
            "正在检查设备配置…" : s_previous_verification_code[0] ?
            "等待绑定，完成后进入首页" : "正在获取验证码…");
        if (binding == STARTER_BINDING_FAILED)
            lv_obj_clear_flag(s3_ui.binding_retry, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s3_ui.binding_retry, LV_OBJ_FLAG_HIDDEN);
    }
    /* The AP can become ready seconds after the page is created. Its SSID,
     * URL and control results must refresh without a runtime snapshot too. */
    if (s3_ui.network_text && now >= s3_ui.network_due) {
        s3_ui.network_due = now + 1000;
        s3_network_text();
    }
}

static void s3_render_emojis(lv_obj_t *screen)
{
    s3_header(screen, "待机表情", ACTION_MENU);
    lv_obj_t *panel = s3_scroller(screen, 48, 188);
    for (uint8_t i = 0; i < sizeof(s_emoji_names) / sizeof(s_emoji_names[0]); ++i) {
        lv_obj_t *button = make_button(panel, s_emoji_names[i], (i % 3) * 100,
            (i / 3) * 52, 94, 46, (product_action_t)(ACTION_EMOJI_BASE + i));
        if (i == s_preferences.emoji_index) {
            lv_obj_set_style_border_width(button, 2, 0);
            lv_obj_set_style_border_color(button, lv_color_hex(S3_ACCENT), 0);
        }
    }
}

static void s3_reset_page(void)
{
    room_ui_reset();
    /* Top-layer toast and pending runtime intent survive page replacement. */
    if (s3_ui.toast && s_page == PAGE_CALL) lv_obj_add_flag(s3_ui.toast, LV_OBJ_FLAG_HIDDEN);
    s3_ui.caption_panel = NULL;
    s3_ui.wake_hint = NULL;
    s3_ui.call_result = NULL;
    s3_ui.wechat_button = NULL;
    s3_ui.wechat_qr = NULL;
    s3_ui.contacts_refresh = NULL;
    s3_ui.binding_code = NULL;
    s3_ui.binding_hint = NULL;
    s3_ui.binding_retry = NULL;
    s3_ui.volume_down = NULL;
    s3_ui.volume_up = NULL;
    s3_ui.network_text = NULL;
    s3_ui.network_disconnect = NULL;
    s3_ui.network_cache[0] = '\0';
    s3_ui.network_due = 0;
}

static bool s3_render_page(lv_obj_t *screen)
{
    switch (s_page) {
    case PAGE_HOME_FACE:
    case PAGE_HOME_CLOCK: s3_render_home(screen); break;
    case PAGE_MENU: s3_render_menu(screen); break;
    case PAGE_ROOM: room_ui_render(screen); break;
    case PAGE_ROOM_FORM: room_ui_form(screen); break;
    /* Old navigation intents now lead to the single home conversation view. */
    case PAGE_AI_CHAT: s_page = PAGE_HOME_FACE; s3_render_home(screen); break;
    case PAGE_SETTINGS: s3_render_settings(screen); break;
    case PAGE_CONTACTS: s3_render_contacts(screen); break;
    case PAGE_CONTACT_DETAIL: s3_render_contact_detail(screen); break;
    case PAGE_CALL:
#if CONFIG_IDF_TARGET_ESP32P4
        render_call(screen);
#else
        s3_render_call(screen);
#endif
        break;
    case PAGE_NETWORK: s3_render_network(screen); break;
    case PAGE_BINDING: s3_render_binding(screen); break;
    case PAGE_EMOJIS: s3_render_emojis(screen); break;
    case PAGE_EMOJI_PREVIEW:
        s3_header(screen, s_emoji_names[s_preview_emoji], ACTION_EMOJIS);
        create_expression_face(screen, 50, 48);
        apply_expression(s_emoji_keys[s_preview_emoji], STARTER_AI_UI_IDLE, 1U);
        (void)make_button(screen, "应用到主页", 8, 192, 304, 44, ACTION_EMOJI_APPLY);
        break;
    case PAGE_DIAGNOSTICS:
        s3_header(screen, "运行状态", ACTION_MENU);
        (void)make_button(screen, "资源", 8, 48, 96, 44, ACTION_DIAG_SYSTEM);
        (void)make_button(screen, "音频", 112, 48, 96, 44, ACTION_DIAG_AUDIO);
        (void)make_button(screen, "事件", 216, 48, 96, 44, ACTION_DIAG_EVENTS);
        s_diagnostics_panel = s3_scroller(screen, 100, 136);
        s_diagnostics_label = make_label(s_diagnostics_panel, "", 4, 0, 284, lv_color_hex(S3_TEXT));
        refresh_diagnostics(monotonic_ms());
        break;
    case PAGE_CALL_RESULT:
        s3_header(screen, "通话结束", ACTION_HOME);
        s3_ui.call_result = make_label(screen, s_call_result[0] ? s_call_result : "通话已结束",
                         16, 88, 288, lv_color_hex(S3_TEXT));
        (void)make_button(screen, "返回首页", 8, 192, 304, 44, ACTION_HOME);
        break;
    default: return false;
    }
    /* Setup pages use network/binding state, never the runtime snapshot.
     * This also avoids its 50 ms lock wait when unbinding during a call. */
    if (s_page == PAGE_NETWORK || s_page == PAGE_BINDING) return true;
    starter_runtime_product_snapshot_t product = starter_runtime_product_snapshot();
    s3_refresh_ui(monotonic_ms(), starter_runtime_status(), &product);
    return true;
}

static bool s3_submit(product_action_t action, esp_err_t result,
                       starter_runtime_state_t requested_from)
{
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "UI intent rejected: action=%u error=%s", (unsigned)action, esp_err_to_name(result));
        s3_feedback(action == ACTION_AI && result == ESP_ERR_INVALID_STATE ?
                    "设备尚未就绪，请稍候" : "操作未受理，请重试");
        return false;
    }
    /* Queue acceptance is not connection success. Wait for the real snapshot. */
    s3_ui.pending_action = action;
    s3_ui.pending_state = requested_from;
    s3_ui.pending_until = monotonic_ms() + 1500;
    return true;
}

static bool s3_ai_request_pending(void)
{
    return s3_ui.pending_action == ACTION_AI && monotonic_ms() < s3_ui.pending_until;
}

static void s3_note_ai_request(starter_runtime_state_t from)
{
    (void)s3_submit(ACTION_AI, ESP_OK, from);
}

static bool s3_handle_action(product_action_t action)
{
    if (action == ACTION_ROOM) {
        if (!wifi_manager_connected()) return true;
        s_page = PAGE_ROOM;
        room_ui.offset = 0;
        render_page();
        return true;
    }
    if ((unsigned)action == S3_ACTION_BINDING_RETRY) {
        if (wifi_manager_connected() && atomic_load(&s_binding_ui_state) == STARTER_BINDING_FAILED) {
            platform_client_retry_binding();
            starter_product_set_binding_state(STARTER_BINDING_REQUIRED);
        }
        return true;
    }
    if ((unsigned)action == S3_ACTION_WIFI_DISCONNECT) {
        esp_err_t err = wifi_manager_disconnect();
        if (err != ESP_OK) s3_feedback("网络操作未提交，请重试");
        s3_ui.network_due = 0;
        return true;
    }
    /* Also reject clicks queued from a page that disappeared on Wi-Fi loss. */
    if (!wifi_manager_connected()) return true;
    starter_runtime_status_t runtime = starter_runtime_status();
    bool ai = runtime.state == STARTER_RUNTIME_AI_ACTIVE || runtime.state == STARTER_RUNTIME_AI_CONNECTING;
    if ((unsigned)action == S3_ACTION_QR_CLOSE) {
        s3_close_wechat_qr();
        return true;
    }
    if ((unsigned)action == S3_ACTION_WECHAT) {
        if (s3_ui.pending_action == action && monotonic_ms() < s3_ui.pending_until) return true;
        starter_runtime_product_snapshot_t product = starter_runtime_product_snapshot();
        uint8_t selected = product.contact_count;
        for (uint8_t i = 0; i < product.contact_count; ++i) {
            if (product.contacts[i].source != STARTER_CONTACT_WECHAT) continue;
            /* The shortcut dials the first WeChat entry in current list order. */
            selected = i;
            break;
        }
        if (selected == product.contact_count) s3_show_wechat_qr();
        else {
            if (s3_submit(action, starter_runtime_call_contact(selected), runtime.state))
                s3_feedback("正在发起呼叫");
        }
        return true;
    }
    if ((unsigned)action == S3_ACTION_AI_TOGGLE) {
        /* Resolve on the owner task, not from a possibly stale button label.
         * An accepted wake may still be queued while runtime reports WAITING. */
        if ((unsigned)s3_ui.pending_action == S3_ACTION_AI_STOP &&
            monotonic_ms() < s3_ui.pending_until) return true;
        action = ai || s3_ai_request_pending() ?
            (product_action_t)S3_ACTION_AI_STOP : ACTION_AI;
    }
    if ((unsigned)action == S3_ACTION_BACK) {
        /* Leaving a view never owns or tears down an ongoing AI session. */
        product_page_t origin = s3_ui.contacts_origin;
        s_page = origin == PAGE_HOME_CLOCK ? PAGE_HOME_CLOCK :
                 origin == PAGE_HOME_FACE ? PAGE_HOME_FACE : PAGE_MENU;
        render_page();
        return true;
    }
    if (action == ACTION_CONTACTS && s_page != PAGE_CONTACTS && s_page != PAGE_CONTACT_DETAIL) {
        product_page_t origin = s_page;
        if (!enter_page(PAGE_CONTACTS)) {
            s3_feedback("操作未受理，请重试");
            return true;
        }
        s3_ui.contacts_origin = origin;
        if (starter_runtime_contacts_refresh() != ESP_OK) s3_feedback("同步未受理，请重试");
        render_page();
        return true;
    }
    if ((unsigned)action == S3_ACTION_CLOCK) {
        s_page = s_page == PAGE_HOME_CLOCK ? PAGE_HOME_FACE : PAGE_HOME_CLOCK;
        render_page();
        return true;
    }
    if (s3_ui.pending_action == action && monotonic_ms() < s3_ui.pending_until) return true;
    if (action == ACTION_AI) {
        if ((unsigned)s3_ui.pending_action == S3_ACTION_AI_STOP &&
            monotonic_ms() < s3_ui.pending_until) return true;
        if (!ai && !s3_submit(action, starter_runtime_ai_start(), runtime.state)) return true;
        /* A tap and a queued wake share this pending intent. Do not recreate
         * the canvas, blink timeline or caption scroller on repeat taps. */
        if (s_page != PAGE_HOME_FACE && s_page != PAGE_HOME_CLOCK) {
            s_page = PAGE_HOME_FACE;
            render_page();
        }
        return true;
    }
    if ((unsigned)action == S3_ACTION_AI_STOP) {
        if ((ai || s3_ai_request_pending()) &&
            s3_submit(action, starter_runtime_ai_stop(), runtime.state)) {
            s_voice_feedback[0] = '\0';
            s_pending_ai_ack.expires_ms = 0;
        }
    } else if ((unsigned)action == S3_ACTION_REFRESH) {
        if (s3_submit(action, starter_runtime_contacts_refresh(), runtime.state)) s3_feedback("已请求同步联系人");
    } else if (action == ACTION_CALL_ACCEPT) {
        (void)s3_submit(action, starter_runtime_call_accept(), runtime.state);
    } else if (action == ACTION_CALL_REJECT) {
        (void)s3_submit(action, starter_runtime_call_reject(), runtime.state);
    } else if (action == ACTION_CALL_HANGUP) {
        (void)s3_submit(action, starter_runtime_call_hangup(), runtime.state);
    } else if (action == ACTION_VOICE_CALL) {
        if (s3_submit(action, starter_runtime_call_contact(s_selected_contact), runtime.state)) s3_feedback("正在发起呼叫");
#if CONFIG_IDF_TARGET_ESP32P4
    } else if (action == ACTION_VIDEO_CALL) {
        if (s3_submit(action, starter_runtime_call_contact_video(s_selected_contact), runtime.state)) s3_feedback("正在发起呼叫");
#endif
    } else return false;
    starter_runtime_product_snapshot_t product = starter_runtime_product_snapshot();
    s3_refresh_ui(monotonic_ms(), starter_runtime_status(), &product);
    return true;
}

/* One predicate owns both the header and the wake hint. A call transition
 * can briefly precede its page transition; it must not advertise WAITING. */
static void s3_refresh_home_status(starter_runtime_status_t runtime,
                                   const starter_runtime_product_snapshot_t *product,
                                   bool ai_pending)
{
    bool idle = runtime.state == STARTER_RUNTIME_WAITING && !ai_pending;
    const char *state;
    if (idle) {
        state = s_voice_feedback[0] ? s_voice_feedback :
            s_preferences.microphone_muted ? "麦克风关闭" : "待唤醒";
    } else if (ai_pending) {
        state = (unsigned)s3_ui.pending_action == S3_ACTION_AI_STOP ? "结束对话中" : "正在连接 AI";
    } else {
        switch (runtime.state) {
        case STARTER_RUNTIME_H5_ACTIVE: state = "远程查看中"; break;
        case STARTER_RUNTIME_AI_CONNECTING: state = "正在连接 AI"; break;
        case STARTER_RUNTIME_AI_ACTIVE: state = phase_text(product->ai_phase); break;
        case STARTER_RUNTIME_CALL_INCOMING: state = "语音来电"; break;
        case STARTER_RUNTIME_CALL_CONNECTING: state = "正在建立通话"; break;
        case STARTER_RUNTIME_CALL_ACTIVE: state = "通话中"; break;
        case STARTER_RUNTIME_ROOM_CONNECTING: state = "正在进入对讲"; break;
        case STARTER_RUNTIME_ROOM_ACTIVE: state = "多人对讲中"; break;
        default: state = "状态同步中"; break;
        }
    }
    label_set_text_if_changed(s_state, state);
    if (idle) {
        label_set_text_if_changed(s3_ui.wake_hint, s_preferences.microphone_muted ?
            "麦克风已关闭\n点击表情唤醒对话" :
            "尝试说“你好小钛”\n或者点击表情唤醒对话");
        lv_obj_clear_flag(s3_ui.wake_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s3_ui.caption_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s3_ui.wake_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s3_ui.caption_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void s3_refresh_ui(int64_t now, starter_runtime_status_t runtime,
                          const starter_runtime_product_snapshot_t *product)
{
    room_ui_tick(now);
    bool pending = s3_ui.pending_until > now && s3_ui.pending_state == runtime.state;
    if (!pending) s3_ui.pending_action = 0;
    if (s3_ui.toast && now >= s3_ui.toast_until)
        lv_obj_add_flag(s3_ui.toast, LV_OBJ_FLAG_HIDDEN);
    if (s3_ui.wechat_qr) note_interaction();
    s3_set_enabled(s3_ui.contacts_refresh, !(pending && (unsigned)s3_ui.pending_action == S3_ACTION_REFRESH));
    s3_set_enabled(s3_ui.wechat_button, !(pending && (unsigned)s3_ui.pending_action == S3_ACTION_WECHAT));
    bool ai = runtime.state == STARTER_RUNTIME_AI_CONNECTING || runtime.state == STARTER_RUNTIME_AI_ACTIVE;
    if (s_page == PAGE_HOME_FACE || s_page == PAGE_HOME_CLOCK) {
        bool ai_pending = pending && (s3_ui.pending_action == ACTION_AI ||
                                     (unsigned)s3_ui.pending_action == S3_ACTION_AI_STOP);
        s3_refresh_home_status(runtime, product, ai_pending);
        if (!ai && !ai_pending) {
            if (lv_label_get_text(s_subtitle)[0] != '\0') lv_label_set_text_static(s_subtitle, "");
        } else {
            bool changed = starter_runtime_caption_read(&s3_ui.caption);
            /* Both speakers follow the newest two lines immediately. Runtime
             * still owns paragraph/snapshot merging; the UI only clips the view. */
            if (changed || lv_label_get_text(s_subtitle) != s3_ui.caption.text) {
                lv_obj_set_style_text_color(s_subtitle,
                    lv_color_hex(s3_ui.caption.is_ai ? S3_CAPTION_AI : S3_CAPTION_USER), 0);
                lv_obj_update_layout(s3_ui.caption_panel);
                lv_label_set_text_static(s_subtitle, s3_ui.caption.text);
                lv_obj_update_layout(s3_ui.caption_panel);
            }
        }
    }
    /* Runtime clears its product snapshot before publishing WAITING. Neither
     * that transient empty snapshot nor WAITING is a new outgoing call. The
     * owner tick performs the page transition; keep the last valid view here. */
    bool call = runtime.state == STARTER_RUNTIME_CALL_INCOMING ||
                runtime.state == STARTER_RUNTIME_CALL_CONNECTING ||
                runtime.state == STARTER_RUNTIME_CALL_ACTIVE;
    if (s_page == PAGE_CALL && call && product->call_peer[0]) {
#if CONFIG_IDF_TARGET_ESP32P4
        /* Use the rendered tree, not a snapshot cleared during teardown. */
        if (s_call_camera_button != NULL) {
            refresh_call_controls(runtime, product);
            s3_set_enabled(s_call_accept_button, !pending);
            s3_set_enabled(s_call_reject_button, !pending);
            s3_set_enabled(s_call_hangup_button, !pending);
        } else
#endif
        {
            bool incoming = runtime.state == STARTER_RUNTIME_CALL_INCOMING;
            bool active = runtime.state == STARTER_RUNTIME_CALL_ACTIVE;
            if (active && s3_ui.call_generation != runtime.session_generation) {
                s3_ui.call_generation = runtime.session_generation;
                s3_ui.call_started = now;
            }
            label_set_text_if_changed(s_header_title, product->call_wechat ? "微信语音" : "设备语音");
            label_set_text_if_changed(s_call_peer_label, product->call_peer[0] ? product->call_peer : "未知联系人");
            char status[80];
            if (pending) (void)snprintf(status, sizeof(status), "%s",
                s3_ui.pending_action == ACTION_CALL_ACCEPT ? "正在接听…" :
                s3_ui.pending_action == ACTION_VOICE_CALL ? "正在发起呼叫…" :
                s3_ui.pending_action == ACTION_CALL_REJECT ? "正在拒绝来电…" :
                active ? "正在结束通话…" : "正在取消呼叫…");
            else if (active) {
                unsigned seconds = (unsigned)((now - s3_ui.call_started) / 1000);
                (void)snprintf(status, sizeof(status), "%02u:%02u%s", seconds / 60, seconds % 60,
                               s_preferences.microphone_muted ? " / 麦克风已关闭" : " / 通话中");
            } else (void)snprintf(status, sizeof(status), "%s", incoming ? "语音来电" :
                product->call_incoming ? "正在建立通话…" : "正在呼叫…");
            label_set_text_if_changed(s_call_status_label, status);
            set_object_visible(s_call_accept_button, incoming);
            set_object_visible(s_call_reject_button, incoming);
            set_object_visible(s_call_mute_button, active);
            set_object_visible(s_call_hangup_button, !incoming);
            s3_set_enabled(s_call_accept_button, !pending);
            s3_set_enabled(s_call_reject_button, !pending);
            s3_set_enabled(s_call_hangup_button, !pending);
            set_button_text(s_call_mute_button, s_preferences.microphone_muted ? "开启麦克风" : "关闭麦克风");
            set_button_text(s_call_hangup_button, active ? "挂断" : "取消呼叫");
            /* Keep controls in the same bottom corners through all call phases. */
            lv_obj_set_pos(s_call_mute_button, 8, 180);
            lv_obj_set_size(s_call_mute_button, 148, 56);
            lv_obj_set_pos(s_call_hangup_button, active ? 164 : 8, 180);
            lv_obj_set_size(s_call_hangup_button, active ? 148 : 304, 56);
        }
    }
    /* The reason may be published just after WAITING. Update the result page
     * without rebuilding it or losing the user's reading position. */
    if (s3_ui.call_result && product->call_result[0]) {
        (void)snprintf(s_call_result, sizeof(s_call_result), "%s", product->call_result);
        label_set_text_if_changed(s3_ui.call_result, s_call_result);
    }
}
