/* P4 presentation only. The runtime owns media; navigation owns whether this
 * independent app is open. Re-rendering its page does not close the session. */
static EXT_RAM_BSS_ATTR struct {
    starter_room_snapshot_t snapshot;
    unsigned offset;
    bool create, room_layout, held, opened;
    uint32_t held_generation;
    int64_t due, held_at, submit_until;
    lv_obj_t *code, *status, *count, *ptt, *previous, *next, *leave, *confirm;
    lv_obj_t *names[STARTER_ROOM_PAGE_SIZE], *self[STARTER_ROOM_PAGE_SIZE], *mic[STARTER_ROOM_PAGE_SIZE];
    lv_obj_t *code_input, *password_input, *active_input, *keyboard, *create_button, *join_button;
} room_ui;

enum { ROOM_KEY_BACKSPACE = 10, ROOM_KEY_CONFIRM = 11 };
static void room_ui_tick(int64_t now);
static void room_ui_release(void)
{
    if (room_ui.held_generation) (void)starter_runtime_room_ptt(room_ui.held_generation, false);
    room_ui.held = false;
    room_ui.held_generation = 0;
}
static void room_ui_navigation(void)
{
    bool open = s_page == PAGE_ROOM || s_page == PAGE_ROOM_FORM;
    if (room_ui.opened == open) return;
    room_ui.opened = open;
    if (!open) room_ui_release();
    starter_runtime_room_set_open(open);
}
static void room_ui_reset(void)
{
    room_ui_navigation();
    room_ui_release();
    room_ui.code = room_ui.status = room_ui.count = room_ui.ptt = NULL;
    room_ui.previous = room_ui.next = room_ui.leave = room_ui.confirm = NULL;
    room_ui.code_input = room_ui.password_input = room_ui.active_input = room_ui.keyboard = NULL;
    room_ui.create_button = room_ui.join_button = NULL;
    memset(room_ui.names, 0, sizeof(room_ui.names));
    memset(room_ui.self, 0, sizeof(room_ui.self));
    memset(room_ui.mic, 0, sizeof(room_ui.mic));
    room_ui.due = room_ui.submit_until = 0;
}
static lv_obj_t *room_ui_button(lv_obj_t *parent, const char *text, int x, int y,
                               int w, int h, lv_event_cb_t cb, uintptr_t action)
{
    lv_obj_t *button = make_button(parent, text, x, y, w, h, (product_action_t)0);
    lv_obj_remove_event_cb(button, on_action);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, (void *)action);
    return button;
}
enum { ROOM_UI_BACK = 1, ROOM_UI_CREATE, ROOM_UI_JOIN, ROOM_UI_REFRESH,
       ROOM_UI_LEAVE, ROOM_UI_LEAVE_YES, ROOM_UI_CANCEL, ROOM_UI_PREV, ROOM_UI_NEXT };

static void room_ui_action(lv_event_t *event)
{
    note_interaction();
    unsigned action = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    room_ui_release();
    if (action == ROOM_UI_BACK) {
        s_page = s_page == PAGE_ROOM_FORM ? PAGE_ROOM : PAGE_MENU;
        render_page();
    } else if (action == ROOM_UI_CREATE || action == ROOM_UI_JOIN) {
        if (!room_ui.snapshot.app_open || !room_ui.snapshot.assignment_known || room_ui.snapshot.busy) return;
        room_ui.create = action == ROOM_UI_CREATE;
        s_page = PAGE_ROOM_FORM;
        render_page();
    } else if (action == ROOM_UI_REFRESH) {
        if (starter_runtime_room_refresh() != ESP_OK) s3_feedback("刷新未提交，请重试");
    } else if (action == ROOM_UI_PREV || action == ROOM_UI_NEXT) {
        if (action == ROOM_UI_PREV) room_ui.offset = room_ui.offset >= STARTER_ROOM_PAGE_SIZE ?
            room_ui.offset - STARTER_ROOM_PAGE_SIZE : 0;
        else if (room_ui.offset + STARTER_ROOM_PAGE_SIZE < room_ui.snapshot.member_count)
            room_ui.offset += STARTER_ROOM_PAGE_SIZE;
        room_ui.due = 0;
        room_ui_tick(monotonic_ms());
    } else if (action == ROOM_UI_LEAVE) {
        if (room_ui.confirm) return;
        lv_obj_t *panel = lv_obj_create(lv_scr_act());
        room_ui.confirm = panel;
        lv_obj_set_pos(panel, 4, 44);
        lv_obj_set_size(panel, 312, 192);
        set_bg(panel, lv_color_hex(S3_SURFACE));
        lv_obj_set_style_pad_all(panel, 0, 0);
        lv_obj_set_style_radius(panel, 8, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        (void)make_label(panel, "退出当前对讲房间？", 16, 30, 280, lv_color_hex(S3_TEXT));
        (void)make_label(panel, "退出后需要重新加入", 16, 62, 280, lv_color_hex(S3_MUTED));
        (void)room_ui_button(panel, "取消", 8, 128, 140, 48, room_ui_action, ROOM_UI_CANCEL);
        lv_obj_t *leave = room_ui_button(panel, "退出房间", 160, 128, 140, 48, room_ui_action, ROOM_UI_LEAVE_YES);
        lv_obj_set_style_bg_color(leave, lv_color_hex(S3_DANGER), 0);
    } else if (action == ROOM_UI_CANCEL || action == ROOM_UI_LEAVE_YES) {
        if (action == ROOM_UI_LEAVE_YES && starter_runtime_room_leave() != ESP_OK)
            s3_feedback("退出请求未提交，请重试");
        if (room_ui.confirm) lv_obj_del(room_ui.confirm);
        room_ui.confirm = NULL;
    }
}
static lv_obj_t *room_ui_icon(lv_obj_t *parent, const char *symbol, int x, int y, unsigned action)
{
    lv_obj_t *button = room_ui_button(parent, symbol, x, y, 40, 40, room_ui_action, action);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(button, 0), &lv_font_montserrat_14, 0);
    return button;
}
static void room_ui_header(lv_obj_t *screen, const char *title)
{
    (void)room_ui_icon(screen, LV_SYMBOL_LEFT, 4, 0, ROOM_UI_BACK);
    (void)make_label(screen, title, 56, 12, 124, lv_color_hex(S3_TEXT));
}
static void room_ui_ptt_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        room_ui_release();
        if (code == LV_EVENT_DELETE) return;
    } else if (code == LV_EVENT_PRESSED) {
        note_interaction();
        room_ui.held_generation = room_ui.snapshot.connection_generation;
        room_ui.held = !room_ui.confirm && starter_runtime_room_ptt(room_ui.held_generation, true);
        room_ui.held_at = monotonic_ms();
    } else if (code == LV_EVENT_PRESSING && room_ui.held) {
        note_interaction();
        int64_t now = monotonic_ms();
        if (now - room_ui.held_at > 750 ||
            !starter_runtime_room_ptt(room_ui.held_generation, true)) room_ui_release();
        room_ui.held_at = now;
    } else return;
    if (room_ui.ptt) {
        set_button_text(room_ui.ptt, room_ui.held ? "松开结束讲话" : "按下讲话");
        lv_obj_set_style_bg_color(room_ui.ptt, lv_color_hex(room_ui.held ? S3_ACCEPT : 0x292E32), 0);
        lv_obj_set_style_bg_color(room_ui.ptt, lv_color_hex(room_ui.held ? S3_ACCEPT : 0x465057), LV_STATE_PRESSED);
    }
}
static void room_ui_render(lv_obj_t *screen)
{
    (void)starter_runtime_room_read(room_ui.offset, &room_ui.snapshot);
    room_ui.room_layout = room_ui.snapshot.assigned;
    room_ui_header(screen, "多人对讲");
    (void)room_ui_icon(screen, LV_SYMBOL_REFRESH, room_ui.room_layout ? 184 : 276, 0, ROOM_UI_REFRESH);
    if (!room_ui.room_layout) {
        (void)make_label(screen, "对讲房间", 16, 56, 288, lv_color_hex(S3_TEXT));
        room_ui.create_button = room_ui_button(screen, "创建房间", 8, 94, 304, 44, room_ui_action, ROOM_UI_CREATE);
        room_ui.join_button = room_ui_button(screen, "加入房间", 8, 146, 304, 44, room_ui_action, ROOM_UI_JOIN);
        s3_set_enabled(room_ui.create_button, false);
        s3_set_enabled(room_ui.join_button, false);
        room_ui.status = make_label(screen, "", 12, 202, 296, lv_color_hex(S3_MUTED));
        return;
    }
    room_ui.leave = room_ui_button(screen, "退出房间", 228, 0, 84, 40, room_ui_action, ROOM_UI_LEAVE);
    lv_obj_set_style_bg_opa(room_ui.leave, LV_OPA_TRANSP, 0);
    room_ui.code = make_label(screen, "", 12, 46, 192, lv_color_hex(S3_TEXT));
    room_ui.count = make_label(screen, "", 12, 69, 200, lv_color_hex(S3_MUTED));
    room_ui.previous = room_ui_icon(screen, LV_SYMBOL_LEFT, 228, 44, ROOM_UI_PREV);
    room_ui.next = room_ui_icon(screen, LV_SYMBOL_RIGHT, 276, 44, ROOM_UI_NEXT);
    for (unsigned i = 0; i < STARTER_ROOM_PAGE_SIZE; ++i) {
        room_ui.names[i] = make_label(screen, "", 16, 98 + i * 27, 198, lv_color_hex(S3_TEXT));
        room_ui.self[i] = make_label(screen, "（本机）", 144, 98 + i * 27, 70, lv_color_hex(S3_MUTED));
        lv_obj_add_flag(room_ui.self[i], LV_OBJ_FLAG_HIDDEN);
        room_ui.mic[i] = make_label(screen, "", 220, 98 + i * 27, 88, lv_color_hex(S3_MUTED));
        lv_label_set_long_mode(room_ui.names[i], LV_LABEL_LONG_DOT);
        lv_obj_set_height(room_ui.names[i], 22);
    }
    room_ui.status = make_label(screen, "", 12, 178, 296, lv_color_hex(S3_MUTED));
    room_ui.ptt = make_button(screen, "按下讲话", 8, 201, 304, 35, (product_action_t)0);
    lv_obj_remove_event_cb(room_ui.ptt, on_action);
    lv_obj_clear_flag(room_ui.ptt, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(room_ui.ptt, room_ui_ptt_event, LV_EVENT_ALL, NULL);
}
static void room_ui_submit(lv_event_t *event)
{
    (void)event;
    if (s_page != PAGE_ROOM_FORM || !room_ui.snapshot.app_open ||
        !room_ui.snapshot.assignment_known || room_ui.snapshot.assigned ||
        monotonic_ms() < room_ui.submit_until || room_ui.snapshot.busy) return;
    note_interaction();
    const char *code = room_ui.code_input ? lv_textarea_get_text(room_ui.code_input) : "";
    const char *password = lv_textarea_get_text(room_ui.password_input);
    esp_err_t err = room_ui.create ? starter_runtime_room_create(password) : starter_runtime_room_join(code, password);
    if (err != ESP_OK) {
        s3_feedback(err == ESP_ERR_INVALID_ARG ? "房间号为6位，密码为4位或留空" : "请求未提交，请重试");
        return;
    }
    room_ui.submit_until = monotonic_ms() + 1000;
    lv_btnmatrix_set_btn_ctrl(room_ui.keyboard, ROOM_KEY_CONFIRM, LV_BTNMATRIX_CTRL_DISABLED);
    label_set_text_if_changed(room_ui.status, "提交中");
    lv_textarea_set_text(room_ui.password_input, "");
}
static void room_ui_select_input(lv_obj_t *input)
{
    if (room_ui.active_input) lv_obj_clear_state(room_ui.active_input, LV_STATE_FOCUSED);
    room_ui.active_input = input;
    if (input) lv_obj_add_state(input, LV_STATE_FOCUSED);
}
static void room_ui_focus(lv_event_t *event)
{
    room_ui_select_input(lv_event_get_target(event));
}
static void room_ui_keyboard(lv_event_t *event)
{
    if (s_page != PAGE_ROOM_FORM || !room_ui.active_input) return;
    note_interaction();
    unsigned key = lv_btnmatrix_get_selected_btn(lv_event_get_target(event));
    if (key == ROOM_KEY_CONFIRM) { room_ui_submit(event); return; }
    if (key == ROOM_KEY_BACKSPACE) lv_textarea_del_char(room_ui.active_input);
    else if (key < ROOM_KEY_BACKSPACE) {
        const char *text = lv_btnmatrix_get_btn_text(lv_event_get_target(event), key);
        if (text) lv_textarea_add_text(room_ui.active_input, text);
    }
}
static void room_ui_key_draw(lv_event_t *event)
{
    lv_obj_draw_part_dsc_t *part = lv_event_get_draw_part_dsc(event);
    if (part && part->part == LV_PART_ITEMS && part->id == ROOM_KEY_BACKSPACE && part->label_dsc)
        part->label_dsc->font = &lv_font_montserrat_14;
}
static lv_obj_t *room_ui_input(lv_obj_t *screen, const char *placeholder, int x, int w, int max, bool password)
{
    lv_obj_t *input = lv_textarea_create(screen);
    lv_obj_set_pos(input, x, 44);
    lv_obj_set_size(input, w, 36);
    set_bg(input, lv_color_hex(S3_SURFACE));
    lv_obj_set_style_text_font(input, &PRODUCT_TEXT_FONT, 0);
    lv_obj_set_style_text_color(input, lv_color_hex(S3_TEXT), 0);
    lv_obj_set_style_radius(input, 6, 0);
    lv_textarea_set_one_line(input, true);
    lv_obj_set_height(input, 36);
    lv_textarea_set_max_length(input, max);
    lv_textarea_set_accepted_chars(input, "0123456789");
    lv_textarea_set_placeholder_text(input, placeholder);
    lv_textarea_set_password_mode(input, password);
    lv_obj_add_event_cb(input, room_ui_focus, LV_EVENT_CLICKED, NULL);
    return input;
}
static void room_ui_form(lv_obj_t *screen)
{
    static const char *const keys[] = {"1", "2", "3", "4", "\n",
        "5", "6", "7", "8", "\n", "9", "0", LV_SYMBOL_BACKSPACE, "确认", ""};
    static const lv_btnmatrix_ctrl_t controls[] = {
        1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT,
        1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT,
        1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT,
        1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT,
        1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1,
        1 | LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CHECKED};
    room_ui_header(screen, room_ui.create ? "创建房间" : "加入房间");
    if (!room_ui.create) room_ui.code_input = room_ui_input(screen, "6位房间号", 8, 146, 6, false);
    room_ui.password_input = room_ui_input(screen, "4位密码（可选）", room_ui.create ? 8 : 162, room_ui.create ? 304 : 150, 4, true);
    room_ui.status = make_label(screen, "", 184, 12, 128, lv_color_hex(S3_MUTED));
    /* A page-local matrix avoids changing LVGL's global keyboard maps, which
     * are also used by Wi-Fi passwords and contact renaming. */
    room_ui.keyboard = lv_btnmatrix_create(screen);
    lv_btnmatrix_set_map(room_ui.keyboard, (const char **)keys);
    lv_btnmatrix_set_ctrl_map(room_ui.keyboard, controls);
    lv_obj_set_pos(room_ui.keyboard, 8, 84);
    lv_obj_set_size(room_ui.keyboard, 304, 152);
    lv_obj_set_style_bg_opa(room_ui.keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(room_ui.keyboard, 0, 0);
    lv_obj_set_style_pad_all(room_ui.keyboard, 0, 0);
    lv_obj_set_style_pad_row(room_ui.keyboard, 4, 0);
    lv_obj_set_style_pad_column(room_ui.keyboard, 6, 0);
    lv_obj_set_style_bg_color(room_ui.keyboard, lv_color_hex(0x292E32), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(room_ui.keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(room_ui.keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(room_ui.keyboard, lv_color_hex(S3_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(room_ui.keyboard, &PRODUCT_TEXT_FONT, LV_PART_ITEMS);
    lv_obj_set_style_radius(room_ui.keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(room_ui.keyboard, lv_color_hex(S3_ACCEPT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(room_ui.keyboard, lv_color_hex(0x465057), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(room_ui.keyboard, room_ui_keyboard, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(room_ui.keyboard, room_ui_key_draw, LV_EVENT_DRAW_PART_BEGIN, NULL);
    room_ui_select_input(room_ui.create ? room_ui.password_input : room_ui.code_input);
}
static void room_ui_tick(int64_t now)
{
    room_ui_navigation();
    if ((s_page != PAGE_ROOM && s_page != PAGE_ROOM_FORM) || now < room_ui.due) return;
    room_ui.due = now + 150;
    if (!starter_runtime_room_read(room_ui.offset, &room_ui.snapshot)) return;
    starter_room_snapshot_t *view = &room_ui.snapshot;
    if ((s_page == PAGE_ROOM && room_ui.room_layout != view->assigned) || (s_page == PAGE_ROOM_FORM && view->assigned)) {
        room_ui.offset = 0;
        s_page = PAGE_ROOM;
        render_page();
        return;
    }
    if (room_ui.offset >= view->member_count && room_ui.offset) { room_ui.offset = 0; room_ui.due = 0; return; }
    bool ready = view->app_open && view->assignment_known && !view->busy;
    s3_set_enabled(room_ui.create_button, ready);
    s3_set_enabled(room_ui.join_button, ready);
    if (room_ui.keyboard) {
        if (!ready || now < room_ui.submit_until)
            lv_btnmatrix_set_btn_ctrl(room_ui.keyboard, ROOM_KEY_CONFIRM, LV_BTNMATRIX_CTRL_DISABLED);
        else lv_btnmatrix_clear_btn_ctrl(room_ui.keyboard, ROOM_KEY_CONFIRM, LV_BTNMATRIX_CTRL_DISABLED);
    }
    const char *message = view->message;
    if (view->state == STARTER_ROOM_LISTENING) message = view->members_ready ?
        (view->members_truncated ? "列表显示前100位成员" : "房间已连接") : "正在同步成员列表";
    label_set_text_if_changed(room_ui.status, message);
    if (room_ui.status) { lv_label_set_long_mode(room_ui.status, LV_LABEL_LONG_DOT); lv_obj_set_height(room_ui.status, 20); }
    if (!room_ui.code) return;
    char text[64];
    snprintf(text, sizeof(text), "房间  %s", view->room_code);
    label_set_text_if_changed(room_ui.code, text);
    unsigned pages = (view->member_count + STARTER_ROOM_PAGE_SIZE - 1) / STARTER_ROOM_PAGE_SIZE;
    if (view->members_ready) snprintf(text, sizeof(text), "%u 人在线  %u/%u", view->online_count,
        room_ui.offset / STARTER_ROOM_PAGE_SIZE + 1, pages ? pages : 1);
    else snprintf(text, sizeof(text), "成员状态待同步");
    label_set_text_if_changed(room_ui.count, text);
    s3_set_enabled(room_ui.previous, room_ui.offset > 0);
    s3_set_enabled(room_ui.next, room_ui.offset + STARTER_ROOM_PAGE_SIZE < view->member_count);
    s3_set_enabled(room_ui.leave, view->state != STARTER_ROOM_LEAVING);
    for (unsigned i = 0; i < STARTER_ROOM_PAGE_SIZE; ++i) {
        starter_room_member_t *member = &view->members[i];
        bool visible = i < view->page_count;
        bool self = visible && member->self;
        const char *name = self && view->self_name[0] ? view->self_name : member->name;
        if (!name[0]) name = member->device_id;
        const char *mic = member->mic == 2 ? "正在讲话" : member->mic == 1 ? "麦克风开启" : "收听中";
        /* Keep the self marker outside ellipsized names/IDs. */
        lv_obj_set_width(room_ui.names[i], self ? 124 : 198);
        set_object_visible(room_ui.self[i], self);
        label_set_text_if_changed(room_ui.names[i], visible ? name : "");
        label_set_text_if_changed(room_ui.mic[i], visible ? mic : "");
        lv_obj_set_style_text_color(room_ui.mic[i], lv_color_hex(member->mic == 2 ? S3_ACCENT : S3_MUTED), 0);
    }
    bool can_talk = view->app_open && view->state == STARTER_ROOM_LISTENING && !s_preferences.microphone_muted && !room_ui.confirm;
    if (room_ui.held && (!can_talk || view->connection_generation != room_ui.held_generation)) room_ui_release();
    s3_set_enabled(room_ui.ptt, can_talk);
    set_button_text(room_ui.ptt, room_ui.held ? "松开结束讲话" : s_preferences.microphone_muted ? "麦克风已关闭" :
        can_talk ? "按下讲话" : "等待房间连接");
    lv_obj_set_style_bg_color(room_ui.ptt, lv_color_hex(room_ui.held ? S3_ACCEPT : 0x292E32), 0);
}
