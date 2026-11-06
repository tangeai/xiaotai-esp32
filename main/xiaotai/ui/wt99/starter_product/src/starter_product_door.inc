/* V4 indoor unit, native 1024x600. Runtime retains call/media ownership.
 * This LVGL owner never substitutes demo video or manufactures a lock ACK. */
#define DOOR_BG 0x0B0E10
#define DOOR_MUTED 0xC4CFD1
#define DOOR_ACCENT 0x54E0C9
#define DOOR_ACTION 0x1FAB96
LV_FONT_DECLARE(ui_font_door_18);
LV_FONT_DECLARE(ui_font_door_24);
LV_FONT_DECLARE(ui_font_door_body_24);
LV_FONT_DECLARE(ui_font_door_26);
LV_FONT_DECLARE(ui_font_door_32);
LV_FONT_DECLARE(ui_font_door_clock_128);
#include "door_v4_assets.h"
_Static_assert(LCD_H_RES == 1024 && LCD_V_RES == 600, "V4 indoor UI requires the WT99 7-inch panel");

extern const uint8_t door_v4_wallpaper_start[] asm("_binary_door_v4_wallpaper_rgb565_start");
static const lv_image_dsc_t door_wallpaper = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = 1024, .h = 600, .stride = 2048},
    
    .data_size = 1024 * 600 * 2, .data = door_v4_wallpaper_start,
};
typedef enum { DOOR_MODAL_NONE, DOOR_MODAL_UNLOCK, DOOR_MODAL_RESULT, DOOR_MODAL_VOLUME } door_modal_t;
static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *clock, *date, *status, *online, *network;
    lv_obj_t *waiting, *duration, *title, *volume, *mute_text;
    lv_obj_t *unlock, *modal, *volume_value;
    char device_ids[STARTER_PRODUCT_CONTACTS_MAX][65];
    char unlock_peer[65];
    uint32_t unlock_generation;
    door_modal_t modal_kind;
    uint32_t call_generation;
    int64_t call_started;
    int online_style, network_style, mute_style;
    bool volume_dirty;
} door_ui;

static const char *door_page_name(void)
{
    switch (s_page) {
    case PAGE_HOME_DOOR: return "door";
    case PAGE_HOME_FACE: return "face";
    case PAGE_HOME_CLOCK: return "clock";
    case PAGE_DOOR_DEVICES: return "devices";
    case PAGE_DOOR_MONITOR: return "monitor";
    case PAGE_CALL: return "call";
    default: return "other";
    }
}
static bool door_is_home(void)
{
    return s_page == PAGE_HOME_DOOR || s_page == PAGE_HOME_FACE || s_page == PAGE_HOME_CLOCK;
}
static bool door_busy(starter_runtime_state_t state)
{
    return state == STARTER_RUNTIME_CALL_INCOMING || state == STARTER_RUNTIME_CALL_CONNECTING ||
           state == STARTER_RUNTIME_CALL_ACTIVE;
}
static void door_close_modal(void)
{
    /* A remote hangup can destroy the slider before RELEASED is delivered. */
    if (door_ui.volume_dirty) preferences_save();
    door_ui.volume_dirty = false;
    if (door_ui.modal) lv_obj_del(door_ui.modal);
    door_ui.modal = door_ui.volume_value = NULL;
    door_ui.modal_kind = DOOR_MODAL_NONE;
}
static void door_reset_page(void)
{
    door_close_modal();
    door_ui.clock = door_ui.date = door_ui.status = door_ui.online = door_ui.network = NULL;
    door_ui.waiting = door_ui.duration = door_ui.title = NULL;
    door_ui.volume = door_ui.mute_text = door_ui.unlock = NULL;
    door_ui.online_style = door_ui.network_style = door_ui.mute_style = -1;
    memset(door_ui.device_ids, 0, sizeof(door_ui.device_ids));
}
static void door_home_step(bool next)
{
    if (!door_is_home() || door_ui.modal || door_busy(starter_runtime_status().state) || s3_ui.wechat_qr) return;
    product_page_t target;
    if (next) target = s_page == PAGE_HOME_DOOR ? PAGE_HOME_FACE :
                       s_page == PAGE_HOME_FACE ? PAGE_HOME_CLOCK : PAGE_HOME_DOOR;
    else target = s_page == PAGE_HOME_DOOR ? PAGE_HOME_CLOCK :
                  s_page == PAGE_HOME_CLOCK ? PAGE_HOME_FACE : PAGE_HOME_DOOR;
    if (enter_page(target)) render_page();
    else s3_feedback("操作未受理，请重试");
}
static lv_obj_t *door_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    product_native_pos(box, x, y);
    product_native_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, opa, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return box;
}
static lv_obj_t *door_label(lv_obj_t *parent, const char *text, int x, int y, int w,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    product_native_pos(label, x, y);
    product_native_size(label, w, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}
static lv_obj_t *door_icon(lv_obj_t *parent, const lv_img_dsc_t *src, int x, int y)
{
    lv_obj_t *image = lv_img_create(parent);
    lv_img_set_src(image, src);
    product_native_pos(image, x, y);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    return image;
}
static lv_obj_t *door_hit(lv_obj_t *parent, int x, int y, int w, int h, product_action_t action)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_remove_style_all(button);
    product_native_pos(button, x, y);
    product_native_size(button, w, h);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, on_action, LV_EVENT_CLICKED, (void *)(uintptr_t)action);
    return button;
}
static lv_obj_t *door_action(lv_obj_t *parent, const char *text, int x, int y,
                             const lv_img_dsc_t *icon, const lv_img_dsc_t *circle,
                             product_action_t action, bool home)
{
    int w = home ? 176 : 112;
    lv_obj_t *button = door_hit(parent, x, y, w, home ? 124 : 116, action);
    int left = (w - 72) / 2;
    if (circle) (void)door_icon(button, circle, left, 0);
    else {
        lv_obj_t *box = door_box(button, left, 0, 72, 72, 0x0E1214, 153);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(box, 56, 0);
    }
    (void)door_icon(button, icon, left + 18, 18);
    lv_obj_t *label = door_label(button, text, 0, home ? 88 : 80, w, &ui_font_door_24, 0xFFFFFF);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    if (action == ACTION_CALL_MUTE) door_ui.mute_text = label;
    return button;
}
static void door_more(lv_event_t *event)
{
    (void)event;
    note_interaction();
    if (door_ui.modal || door_busy(starter_runtime_status().state)) return;
    s_page = PAGE_MENU;
    render_page();
}
static void door_render_home(lv_obj_t *screen)
{
    set_bg(screen, lv_color_hex(DOOR_BG));
    lv_obj_set_style_bg_img_src(screen, &door_wallpaper, 0);
    (void)door_box(screen, 0, 0, 1024, 600, DOOR_BG, 46);
    lv_obj_t *brand = door_label(screen, "小钛可视门铃", 40, 32, 620, &ui_font_door_32, 0xFFFFFF);
    lv_obj_add_flag(brand, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(brand, door_more, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *peer = door_hit(screen, 752, 24, 192, 56, ACTION_DOOR_DEVICES);
    lv_obj_t *name = door_label(peer, "入户门", 16, 12, 168, &ui_font_door_body_24, 0xFFFFFF);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *wifi = door_hit(screen, 948, 24, 60, 56, ACTION_NETWORK);
    door_ui.network = door_icon(wifi, &door_asset_wifi_24, 12, 16);
    door_ui.clock = door_label(screen, "--:--", 200, 126, 624, &ui_font_door_clock_128, 0xFFFFFF);
    lv_obj_set_style_text_align(door_ui.clock, LV_TEXT_ALIGN_CENTER, 0);
    door_ui.date = door_label(screen, "正在同步时间", 200, 280, 624, &ui_font_door_body_24, 0xFFFFFF);
    lv_obj_set_style_text_align(door_ui.date, LV_TEXT_ALIGN_CENTER, 0);
    door_ui.online = door_icon(screen, &door_asset_online_8, 437, 348);
    door_ui.status = door_label(screen, "正在同步状态", 456, 332, 330, &ui_font_door_18, 0xFFFFFF);
    (void)door_action(screen, "查看门口", 312, 440, &door_asset_video_36, NULL, ACTION_DOOR_MONITOR, true);
    (void)door_action(screen, "开门", 536, 440, &door_asset_key_36, NULL, ACTION_DOOR_UNLOCK, true);
}
static const starter_product_contact_t *door_selected(const starter_runtime_product_snapshot_t *product)
{
    for (uint8_t i = 0; i < product->contact_count && i < STARTER_PRODUCT_CONTACTS_MAX; ++i) {
        const starter_product_contact_t *contact = &product->contacts[i];
        if (contact->source == STARTER_CONTACT_DEVICE && strcmp(contact->id, s_preferences.door_peer_id) == 0) return contact;
    }
    return NULL;
}
static const char *door_monitor_reason(starter_runtime_status_t runtime, const starter_runtime_product_snapshot_t *product)
{
    if (!s_preferences.door_peer_id[0]) return "请先选择门外机";
    if (!wifi_manager_connected()) return "网络未连接";
    if (!platform_client_ready()) return "设备尚未就绪，请检查绑定";
    const starter_product_contact_t *peer = door_selected(product);
    if (!peer) return "门外机已移除，请重新选择";
    if (!peer->online) return "门外机离线，请稍后再试";
    if (runtime.state != STARTER_RUNTIME_WAITING) return "当前功能正在使用，请稍后再试";
    return "暂未接入门口查看服务";
}
static void door_refresh(starter_runtime_status_t runtime, const starter_runtime_product_snapshot_t *product)
{
    if (door_ui.modal) note_interaction();
    if (s_page == PAGE_DOOR_MONITOR) {
        label_set_text_if_changed(door_ui.waiting, door_monitor_reason(runtime, product));
        return;
    }
    if (s_page != PAGE_HOME_DOOR || door_ui.status == NULL) return;
    time_t now = time(NULL);
    struct tm local;
    char clock[16], date[80];
    if (localtime_r(&now, &local) && local.tm_year >= 124) {
        static const char *const days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        (void)snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        (void)snprintf(date, sizeof(date), "%d 月 %d 日  %s", local.tm_mon + 1, local.tm_mday, days[local.tm_wday]);
    } else {
        (void)snprintf(clock, sizeof(clock), "--:--");
        (void)snprintf(date, sizeof(date), "正在同步时间");
    }
    label_set_text_if_changed(door_ui.clock, clock);
    label_set_text_if_changed(door_ui.date, date);
    const starter_product_contact_t *peer = door_selected(product);
    bool online = wifi_manager_connected() && platform_client_ready() && peer && peer->online;
    const char *status = !wifi_manager_connected() ? "网络未连接" :
        !platform_client_ready() ? "设备尚未就绪" : !s_preferences.door_peer_id[0] ? "尚未选择门外机" :
        !peer ? "门外机已移除" : !peer->online ? "门外机离线" :
        runtime.state == STARTER_RUNTIME_H5_ACTIVE ? "远程查看中" : "门外机在线";
    label_set_text_if_changed(door_ui.status, status);
    if (door_ui.online_style != (int)online) {
        lv_obj_set_style_img_recolor(door_ui.online, lv_color_hex(online ? DOOR_ACCENT : 0x8C999E), 0);
        lv_obj_set_style_img_recolor_opa(door_ui.online, LV_OPA_COVER, 0);
        door_ui.online_style = online;
    }
    bool network = wifi_manager_connected();
    if (door_ui.network_style != (int)network) {
        lv_obj_set_style_opa(door_ui.network, network ? LV_OPA_COVER : LV_OPA_40, 0);
        door_ui.network_style = network;
    }
}
static void door_render_devices(lv_obj_t *screen)
{
    set_bg(screen, lv_color_hex(DOOR_BG));
    lv_obj_t *back = door_hit(screen, 20, 16, 64, 64, ACTION_DOOR_BACK);
    (void)door_icon(back, &door_asset_back_36, 14, 14);
    (void)door_label(screen, "选择门外机", 104, 24, 760, &ui_font_door_32, 0xFFFFFF);
    lv_obj_t *list = door_box(screen, 32, 112, 960, 440, DOOR_BG, LV_OPA_TRANSP);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    static EXT_RAM_BSS_ATTR starter_runtime_product_snapshot_t product;
    if (!starter_runtime_try_product_snapshot(&product)) {
        (void)door_label(list, "设备状态暂不可用，请返回重试", 0, 24, 900, &ui_font_door_body_24, DOOR_MUTED);
        return;
    }
    unsigned count = 0;
    for (uint8_t i = 0; i < product.contact_count && i < STARTER_PRODUCT_CONTACTS_MAX; ++i) {
        const starter_product_contact_t *contact = &product.contacts[i];
        if (contact->source != STARTER_CONTACT_DEVICE) continue;
        (void)snprintf(door_ui.device_ids[count], sizeof(door_ui.device_ids[count]), "%s", contact->id);
        lv_obj_t *row = door_hit(list, 0, count * 96, 944, 88, (product_action_t)(ACTION_DOOR_DEVICE_BASE + count));
        lv_obj_set_style_bg_color(row, lv_color_hex(0x13171A), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        bool selected = strcmp(contact->id, s_preferences.door_peer_id) == 0;
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(selected ? DOOR_ACCENT : 0x384246), 0);
        (void)door_label(row, contact->name[0] ? contact->name : contact->id, 24, 8, 700, &ui_font_door_24, 0xFFFFFF);
        (void)door_label(row, contact->online ? "在线" : "离线", 24, 48, 700, &ui_font_door_18, contact->online ? DOOR_ACCENT : DOOR_MUTED);
        lv_obj_t *mark = door_label(row, selected ? "已选择" : "选择", 744, 26, 176, &ui_font_door_24, selected ? DOOR_ACCENT : DOOR_MUTED);
        lv_obj_set_style_text_align(mark, LV_TEXT_ALIGN_RIGHT, 0);
        count++;
    }
    if (!count) {
        (void)door_label(list, "暂无设备联系人", 0, 24, 900, &ui_font_door_32, 0xFFFFFF);
        (void)door_label(list, "请先在平台添加门外机，再同步通讯录", 0, 88, 900, &ui_font_door_body_24, DOOR_MUTED);
    }
}
static void door_video_chrome(lv_obj_t *screen)
{
    set_bg(screen, lv_color_hex(DOOR_BG));
    (void)door_box(screen, 0, 0, 1024, 112, DOOR_BG, 82);
    door_ui.title = door_label(screen, "", 32, 24, 700, &ui_font_door_32, 0xFFFFFF);
    s_call_peer_label = door_label(screen, "入户门", 32, 70, 760, &ui_font_door_body_24, 0xFFFFFF);
    door_ui.duration = door_label(screen, "", 840, 24, 152, &ui_font_door_body_24, 0xFFFFFF);
    lv_obj_set_style_text_align(door_ui.duration, LV_TEXT_ALIGN_RIGHT, 0);
    door_ui.online = door_icon(screen, &door_asset_online_8, 816, 37);
    (void)door_box(screen, 0, 454, 1024, 146, 0x0E1214, 214);
}
static void door_render_monitor(lv_obj_t *screen)
{
    door_video_chrome(screen);
    label_set_text_if_changed(door_ui.title, "门口画面");
    label_set_text_if_changed(door_ui.duration, "未连接");
    set_object_visible(door_ui.online, false);
    door_ui.waiting = door_label(screen, "", 112, 250, 800, &ui_font_door_body_24, DOOR_MUTED);
    lv_obj_set_style_text_align(door_ui.waiting, LV_TEXT_ALIGN_CENTER, 0);
    (void)door_action(screen, "返回", 344, 466, &door_asset_back_36, &door_asset_circle_monitor_neutral_72, ACTION_DOOR_BACK, false);
    (void)door_action(screen, "开门", 568, 466, &door_asset_key_monitor_36, &door_asset_circle_monitor_action_72, ACTION_DOOR_UNLOCK, false);
}
static void door_render_call(lv_obj_t *screen)
{
    door_video_chrome(screen);
    door_ui.waiting = door_label(screen, "接听后显示门口画面", 112, 250, 800, &ui_font_door_body_24, DOOR_MUTED);
    lv_obj_set_style_text_align(door_ui.waiting, LV_TEXT_ALIGN_CENTER, 0);
    s_call_reject_button = door_action(screen, "拒绝", 344, 466, &door_asset_hangup_36, &door_asset_circle_incoming_red_72, ACTION_CALL_REJECT, false);
    s_call_accept_button = door_action(screen, "接听", 568, 466, &door_asset_phone_36, &door_asset_circle_incoming_action_72, ACTION_CALL_ACCEPT, false);
    s_call_mute_button = door_action(screen, "静音", 216, 466, &door_asset_mic_36, &door_asset_circle_neutral_72, ACTION_CALL_MUTE, false);
    door_ui.volume = door_action(screen, "音量", 376, 466, &door_asset_volume_36, &door_asset_circle_neutral_72, ACTION_DOOR_VOLUME, false);
    door_ui.unlock = door_action(screen, "开门", 536, 466, &door_asset_key_call_36, &door_asset_circle_action_72, ACTION_DOOR_UNLOCK, false);
    s_call_hangup_button = door_action(screen, "挂断", 696, 466, &door_asset_hangup_36, &door_asset_circle_red_72, ACTION_CALL_HANGUP, false);
}
static void door_refresh_call(starter_runtime_status_t runtime, const starter_runtime_product_snapshot_t *product)
{
    if (s_page != PAGE_CALL || product == NULL || s_call_peer_label == NULL) return;
    bool incoming = runtime.state == STARTER_RUNTIME_CALL_INCOMING;
    bool active = runtime.state == STARTER_RUNTIME_CALL_ACTIVE;
    int64_t now = monotonic_ms();
    if (active && door_ui.call_generation != runtime.session_generation) {
        door_ui.call_generation = runtime.session_generation;
        door_ui.call_started = now;
    }
    if (door_ui.modal && (!active || door_ui.unlock_generation != runtime.session_generation)) door_close_modal();
    if (s3_ui.pending_action == ACTION_CALL_MUTE &&
        product->call_microphone_muted == s_preferences.microphone_muted) s3_ui.pending_until = 0;
    bool pending = s3_ui.pending_until > now && s3_ui.pending_state == runtime.state;
    label_set_text_if_changed(door_ui.title, incoming ? "门口有人" : active ? "正在通话" : "正在连接");
    char peer[160], status[40];
    (void)snprintf(peer, sizeof(peer), "%s · %s", product->call_peer[0] ? product->call_peer : "入户门",
                   incoming ? "门口来电" : active ? (product->call_video ? "双向视频通话" : "双向语音通话") : "正在建立通话");
    label_set_text_if_changed(s_call_peer_label, peer);
    if (pending) (void)snprintf(status, sizeof(status), "%s", s3_ui.pending_action == ACTION_CALL_ACCEPT ?
        "正在接听" : s3_ui.pending_action == ACTION_CALL_REJECT ? "正在拒接" :
        s3_ui.pending_action == ACTION_CALL_MUTE ? "正在设置" : "正在结束");
    else if (active) {
        unsigned seconds = (unsigned)((now - door_ui.call_started) / 1000);
        (void)snprintf(status, sizeof(status), "%02u:%02u", seconds / 60, seconds % 60);
    } else (void)snprintf(status, sizeof(status), "%s", incoming ? "待接听" : "连接中");
    label_set_text_if_changed(door_ui.duration, status);
    label_set_text_if_changed(door_ui.waiting, incoming ? "接听后显示门口画面" : "正在建立通话");
    set_object_visible(door_ui.waiting, !active);
    const lv_img_dsc_t *indicator = incoming ? &door_asset_incoming_8 : &door_asset_call_online_8;
    if (lv_img_get_src(door_ui.online) != indicator) lv_img_set_src(door_ui.online, indicator);
    set_object_visible(door_ui.online, incoming || active);
    set_object_visible(s_call_accept_button, incoming);
    set_object_visible(s_call_reject_button, incoming);
    set_object_visible(s_call_mute_button, active);
    set_object_visible(door_ui.volume, active);
    set_object_visible(door_ui.unlock, active);
    set_object_visible(s_call_hangup_button, !incoming);
    product_native_pos(s_call_hangup_button, active ? 696 : 456, 466);
    label_set_text_if_changed(door_ui.mute_text, product->call_microphone_muted ? "取消静音" : "静音");
    if (door_ui.mute_style != (int)product->call_microphone_muted) {
        lv_obj_set_style_bg_color(s_call_mute_button, lv_color_hex(DOOR_ACTION), 0);
        lv_obj_set_style_bg_opa(s_call_mute_button, product->call_microphone_muted ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        door_ui.mute_style = product->call_microphone_muted;
    }
    s3_set_enabled(s_call_accept_button, !pending);
    s3_set_enabled(s_call_reject_button, !pending);
    s3_set_enabled(s_call_hangup_button, !pending);
    s3_set_enabled(s_call_mute_button, !pending);
}

static lv_obj_t *door_modal_button(const char *text, int x, int y, product_action_t action,
                                   const lv_img_dsc_t *icon, uint32_t color)
{
    lv_obj_t *button = door_hit(door_ui.modal, x, y, 232, 64, action);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(button, 214, 0);
    (void)door_icon(button, icon, 24, 18);
    (void)door_label(button, text, 64, 14, 152, &ui_font_door_24, 0xFFFFFF);
    return button;
}
static void door_modal_base(door_modal_t kind)
{
    door_close_modal();
    door_ui.modal_kind = kind;
    door_ui.modal = door_box(lv_layer_top(), 0, 0, 1024, 600, DOOR_BG, kind == DOOR_MODAL_VOLUME ? 82 : 133);
    lv_obj_add_flag(door_ui.modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *panel = door_box(door_ui.modal, kind == DOOR_MODAL_VOLUME ? 532 : 236,
        kind == DOOR_MODAL_VOLUME ? 280 : 130, kind == DOOR_MODAL_VOLUME ? 460 : 552,
        kind == DOOR_MODAL_VOLUME ? 240 : 340, 0x13171A, 250);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(panel, 56, 0);
}
static void door_unlock_confirmation(void)
{
    door_ui.unlock_generation = starter_runtime_status().session_generation;
    (void)snprintf(door_ui.unlock_peer, sizeof(door_ui.unlock_peer), "%s", s_preferences.door_peer_id);
    door_modal_base(DOOR_MODAL_UNLOCK);
    (void)door_icon(door_ui.modal, &door_asset_lock_32, 268, 164);
    (void)door_label(door_ui.modal, "确认开门？", 320, 155, 436, &ui_font_door_32, 0xFFFFFF);
    (void)door_label(door_ui.modal, "入户门 · 门锁", 268, 226, 488, &ui_font_door_body_24, DOOR_MUTED);
    (void)door_label(door_ui.modal, "开门后，门口访客将可进入。", 268, 280, 488, &ui_font_door_body_24, 0xFFFFFF);
    (void)door_modal_button("取消", 268, 370, ACTION_DOOR_MODAL_CLOSE, &door_asset_back_28, 0x0E1214);
    (void)door_modal_button("确认开门", 524, 370, ACTION_DOOR_UNLOCK_CONFIRM, &door_asset_key_28, DOOR_ACTION);
}
static void door_unlock_unavailable(void)
{
    /* No lock driver/protocol is configured. Do not send an undefined message,
     * drive an unknown GPIO, or manufacture a successful execution. */
    const char *reason = !door_ui.unlock_peer[0] ? "尚未选择门外机，未发送开门指令" :
        strcmp(door_ui.unlock_peer, s_preferences.door_peer_id) != 0 ? "门外机已变更，未发送开门指令" :
        "门锁尚未配置，未发送开门指令";
    door_modal_base(DOOR_MODAL_RESULT);
    (void)door_icon(door_ui.modal, &door_asset_failure_52, 486, 164);
    lv_obj_t *title = door_label(door_ui.modal, "暂时无法开门", 268, 238, 488, &ui_font_door_32, 0xFFFFFF);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *body = door_label(door_ui.modal, reason, 268, 296, 488, &ui_font_door_body_24, DOOR_MUTED);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    (void)door_modal_button("返回", 396, 378, ACTION_DOOR_MODAL_CLOSE, &door_asset_back_28, 0x0E1214);
}
static void door_volume_changed(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    uint8_t value = (uint8_t)lv_slider_get_value(slider);
    note_interaction();
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        esp_err_t err = starter_media_set_speaker_volume(value);
        if (err == ESP_OK) {
            if (s_preferences.volume != value) door_ui.volume_dirty = true;
            s_preferences.volume = value;
        }
        else {
            lv_slider_set_value(slider, s_preferences.volume, LV_ANIM_OFF);
            s3_feedback("音量设置未受理，请重试");
        }
        char text[12];
        (void)snprintf(text, sizeof(text), "%u%%", s_preferences.volume * 10U);
        label_set_text_if_changed(door_ui.volume_value, text);
    } else if (lv_event_get_code(event) == LV_EVENT_RELEASED && door_ui.volume_dirty) {
        preferences_save();
        door_ui.volume_dirty = false;
    }
}
static void door_volume_modal(void)
{
    door_ui.unlock_generation = starter_runtime_status().session_generation;
    door_modal_base(DOOR_MODAL_VOLUME);
    (void)door_label(door_ui.modal, "通话音量", 564, 312, 300, &ui_font_door_26, 0xFFFFFF);
    lv_obj_t *close = door_hit(door_ui.modal, 916, 296, 60, 60, ACTION_DOOR_MODAL_CLOSE);
    (void)door_icon(close, &door_asset_close_28, 16, 16);
    lv_obj_t *slider = lv_slider_create(door_ui.modal);
    lv_obj_remove_style_all(slider);
    product_native_pos(slider, 564, 410);
    product_native_size(slider, 304, 4);
    lv_obj_set_style_radius(slider, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, 56, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(DOOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_img_src(slider, &door_asset_knob_20, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 24);
    lv_slider_set_range(slider, 0, 10);
    lv_slider_set_value(slider, s_preferences.volume, LV_ANIM_OFF);
    char text[12];
    (void)snprintf(text, sizeof(text), "%u%%", s_preferences.volume * 10U);
    door_ui.volume_value = door_label(door_ui.modal, text, 868, 394, 76, &ui_font_door_body_24, 0xFFFFFF);
    lv_obj_set_style_text_align(door_ui.volume_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_event_cb(slider, door_volume_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, door_volume_changed, LV_EVENT_RELEASED, NULL);
}
static void door_control_tick(void)
{
    int control = atomic_exchange(&s_door_control, 0);
    if (control == 0) return;
    bool accepted = true;
    starter_runtime_status_t runtime = starter_runtime_status();
    if (control != STARTER_PRODUCT_DOOR_STATUS) {
        if (s3_ui.setup_active || door_ui.modal || runtime.state != STARTER_RUNTIME_WAITING) accepted = false;
        else {
            note_interaction();
            switch (control) {
            case STARTER_PRODUCT_DOOR_HOME:
                accepted = enter_page(PAGE_HOME_DOOR);
                if (accepted) render_page();
                break;
            case STARTER_PRODUCT_DOOR_NEXT:
            case STARTER_PRODUCT_DOOR_PREVIOUS:
                accepted = door_is_home();
                if (accepted) door_home_step(control == STARTER_PRODUCT_DOOR_NEXT);
                break;
            case STARTER_PRODUCT_DOOR_VIEWER:
            case STARTER_PRODUCT_DOOR_SENDER:
                /* Legacy maintenance commands remain; no role picker or
                 * outgoing-call button is exposed in the indoor unit. */
                accepted = s_page == PAGE_HOME_DOOR;
                if (accepted) (void)door_handle_action(control == STARTER_PRODUCT_DOOR_VIEWER ? ACTION_DOOR_VIEWER : ACTION_DOOR_SENDER);
                break;
            case STARTER_PRODUCT_DOOR_DEVICES:
                accepted = s_page == PAGE_HOME_DOOR;
                if (accepted) (void)door_handle_action(ACTION_DOOR_DEVICES);
                break;
            case STARTER_PRODUCT_DOOR_BACK:
                accepted = s_page == PAGE_DOOR_DEVICES || s_page == PAGE_DOOR_MONITOR;
                if (accepted) (void)door_handle_action(ACTION_DOOR_BACK);
                break;
            default: accepted = false; break;
            }
        }
    }
    flockfile(stdout);
    printf("+DOOR:control=%d,accepted=%u,page=%s,role=%s,peer_selected=%u,save_pending=%u,awake=%u,setup=%u,lock_configured=0\r\n",
        control, (unsigned)accepted, door_page_name(), s_preferences.door_sender ? "sender" : "viewer",
        (unsigned)(s_preferences.door_peer_id[0] != '\0'), (unsigned)(s_preferences_pending || s_preferences_ticket != 0),
        (unsigned)s_display_awake, (unsigned)s3_ui.setup_active);
    fflush(stdout);
    funlockfile(stdout);
}
static bool door_handle_action(product_action_t action)
{
    if (action == ACTION_CALL_MUTE && s_door_call && s_page == PAGE_CALL) {
        if (door_ui.modal || s3_ui.setup_active) return true;
        starter_runtime_status_t runtime = starter_runtime_status();
        if (runtime.state != STARTER_RUNTIME_CALL_ACTIVE ||
            (s3_ui.pending_until > monotonic_ms() && s3_ui.pending_state == runtime.state)) return true;
        static EXT_RAM_BSS_ATTR starter_runtime_product_snapshot_t product;
        if (!starter_runtime_try_product_snapshot(&product)) {
            s3_feedback("设备状态暂不可用，请重试");
            return true;
        }
        bool muted = !product.call_microphone_muted;
        if (s3_submit(action, starter_runtime_call_set_microphone_muted(muted), runtime.state)) {
            s_preferences.microphone_muted = muted;
            preferences_save();
        }
        return true;
    }
    if (action == ACTION_HOME_PREVIOUS || action == ACTION_HOME_NEXT) {
        door_home_step(action == ACTION_HOME_NEXT);
        return true;
    }
    if (action < ACTION_DOOR_VIEWER || action >= ACTION_DOOR_DEVICE_BASE + STARTER_PRODUCT_CONTACTS_MAX) return false;
    if (s3_ui.setup_active) return true;
    starter_runtime_status_t runtime = starter_runtime_status();
    if (action == ACTION_DOOR_MODAL_CLOSE) {
        door_close_modal();
        return true;
    }
    if (action == ACTION_DOOR_UNLOCK_CONFIRM) {
        if (door_ui.modal_kind == DOOR_MODAL_UNLOCK) door_unlock_unavailable();
        return true;
    }
    if (door_ui.modal) return true;
    if (action == ACTION_DOOR_UNLOCK) {
        if (s_page == PAGE_HOME_DOOR || s_page == PAGE_DOOR_MONITOR ||
            (s_door_call && s_page == PAGE_CALL && runtime.state == STARTER_RUNTIME_CALL_ACTIVE)) door_unlock_confirmation();
        return true;
    }
    if (action == ACTION_DOOR_VOLUME) {
        if (s_door_call && s_page == PAGE_CALL && runtime.state == STARTER_RUNTIME_CALL_ACTIVE) door_volume_modal();
        return true;
    }
    if (door_busy(runtime.state)) return true;
    if (action == ACTION_DOOR_BACK && (s_page == PAGE_DOOR_DEVICES || s_page == PAGE_DOOR_MONITOR)) {
        s_page = PAGE_HOME_DOOR;
        render_page();
    } else if (action == ACTION_DOOR_MONITOR && s_page == PAGE_HOME_DOOR) {
        /* Outbound monitoring is not a device call. The current runtime has
         * no monitor service; expose its availability without ringing anyone. */
        if (enter_page(PAGE_DOOR_MONITOR)) render_page();
        else s3_feedback("操作未受理，请重试");
    } else if (action == ACTION_DOOR_DEVICES && s_page == PAGE_HOME_DOOR) {
        if (enter_page(PAGE_DOOR_DEVICES)) {
            render_page();
            if (starter_runtime_contacts_refresh() != ESP_OK) s3_feedback("同步未受理，请重试");
        } else s3_feedback("操作未受理，请重试");
    } else if (action >= ACTION_DOOR_DEVICE_BASE && s_page == PAGE_DOOR_DEVICES) {
        unsigned index = (unsigned)(action - ACTION_DOOR_DEVICE_BASE);
        if (index < STARTER_PRODUCT_CONTACTS_MAX && door_ui.device_ids[index][0]) {
            (void)snprintf(s_preferences.door_peer_id, sizeof(s_preferences.door_peer_id), "%s", door_ui.device_ids[index]);
            preferences_save();
            s_page = PAGE_HOME_DOOR;
            render_page();
            s3_feedback("已选择门外机");
        }
    } else if ((action == ACTION_DOOR_VIEWER || action == ACTION_DOOR_SENDER) && s_page == PAGE_HOME_DOOR) {
        uint8_t sender = action == ACTION_DOOR_SENDER;
        if (s_preferences.door_sender != sender) {
            s_preferences.door_sender = sender;
            preferences_save();
        }
    }
    return true;
}
