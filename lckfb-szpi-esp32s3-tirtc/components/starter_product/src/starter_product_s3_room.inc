/* 320x240: fixed header + room identity + three member rows + 44px PTT.
 * Paging keeps the LVGL pool bounded even in a 100-member room. */
static EXT_RAM_BSS_ATTR struct {
    starter_room_view_t view;
    lv_obj_t *code;
    lv_obj_t *state;
    lv_obj_t *members[STARTER_ROOM_PAGE_SIZE];
    lv_obj_t *self[STARTER_ROOM_PAGE_SIZE];
    lv_obj_t *status[STARTER_ROOM_PAGE_SIZE];
    lv_obj_t *ptt;
    lv_obj_t *counter;
    lv_obj_t *previous;
    lv_obj_t *next;
    lv_obj_t *entry;
    lv_obj_t *password;
    lv_obj_t *keyboard;
    lv_obj_t *active_input;
    lv_obj_t *leave;
    lv_obj_t *create_button;
    lv_obj_t *join_button;
    uint16_t offset;
    uint32_t held_generation;
    bool form;
    bool create;
    bool rendered_assigned;
    bool opened;
    int64_t next_refresh;
} s3_room;

/* Instance-local button matrix: never replace LVGL's global keyboard maps.
 * Read-only maps stay in Flash; the existing LVGL pool owns the 12 key areas. */
static const char * const s3_room_keymap[] = {
    "1", "2", "3", "4", "\n",
    "5", "6", "7", "8", "\n",
    "9", "0", LV_SYMBOL_BACKSPACE, "确认", ""
};
enum { S3_ROOM_KEY_BACKSPACE = 10, S3_ROOM_KEY_CONFIRM = 11, S3_ROOM_KEY_COUNT = 12 };
static const lv_btnmatrix_ctrl_t s3_room_keyctrl[] = {
    1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT,
    1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT,
    1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT,
    1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT,
    1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1 | LV_BTNMATRIX_CTRL_NO_REPEAT, 1,
    1 | LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_CHECKED
};

static const char *s3_room_state_text(const starter_room_view_t *view)
{
    if (!view->assignment_known && view->phase != STARTER_ROOM_ERROR) return "正在同步";
    if (view->busy && view->phase != STARTER_ROOM_CONNECTING) return "正在同步";
    switch (view->phase) {
    case STARTER_ROOM_EMPTY: return "尚未加入房间";
    case STARTER_ROOM_SYNCING: return "正在同步";
    case STARTER_ROOM_CONNECTING: return "正在加入";
    case STARTER_ROOM_LISTENING: return "收听中";
    case STARTER_ROOM_SPEAKING: return "正在讲话";
    case STARTER_ROOM_SUSPENDED: return "通话中，稍后恢复";
    case STARTER_ROOM_OFFLINE: return "未连接";
    case STARTER_ROOM_ERROR:
        switch (view->error) {
        case 40320: return "密码错误";
        case 40400: return "房间不存在";
        case 40920: return "房间已满";
        case 40921: return "等待旧连接释放";
        case 40923: return "请先退出当前房间";
        case 42920: return "密码错误过多，请稍后";
        case 42900: return "操作过快，请稍后";
        default: return "连接失败，请刷新";
        }
    default: return "等待连接";
    }
}

static void s3_room_release(void)
{
    if (s3_room.held_generation) {
        (void)starter_runtime_room_ptt(s3_room.held_generation, false);
        s3_room.held_generation = 0;
    }
}

/* Runs for every page replacement, including incoming calls, AI, binding and
 * network setup. Re-rendering this page/forms must not disconnect the room. */
static void s3_room_navigation(void)
{
    bool open = s_page == PAGE_ROOM;
    if (s3_room.opened == open) return;
    s3_room.opened = open;
    if (!open) s3_room_release();
    starter_runtime_room_set_open(open);
}

static void s3_room_ptt_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        note_interaction();
        uint32_t generation = s3_room.view.generation;
        if (starter_runtime_room_ptt(generation, true) == ESP_OK)
            s3_room.held_generation = generation;
        else s3_feedback("尚未就绪或麦克风已关闭");
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DELETE) {
        s3_room_release();
    }
}

static void s3_room_select_input(lv_obj_t *input)
{
    if (s3_room.active_input == input) return;
    if (s3_room.active_input) lv_obj_clear_state(s3_room.active_input, LV_STATE_FOCUSED);
    s3_room.active_input = input;
    if (input) lv_obj_add_state(input, LV_STATE_FOCUSED);
}

static void s3_room_focus(lv_event_t *event)
{
    if (!s3_room.keyboard) return;
    note_interaction();
    s3_room_select_input(lv_event_get_target(event));
}

static void s3_room_submit(void)
{
    const char *password = lv_textarea_get_text(s3_room.password);
    esp_err_t err = s3_room.create ? starter_runtime_room_create(password) :
        starter_runtime_room_join(lv_textarea_get_text(s3_room.entry), password);
    if (err == ESP_OK) {
        s3_room.form = false;
        render_page();
    } else s3_feedback(err == ESP_ERR_INVALID_ARG ? "房间号六位，密码四位或留空" : "操作未受理，请重试");
}

static void s3_room_key(lv_event_t *event)
{
    if (!s3_room.form || s_page != PAGE_ROOM || !s3_room.active_input) return;
    uint16_t key = lv_btnmatrix_get_selected_btn(lv_event_get_target(event));
    if (key >= S3_ROOM_KEY_COUNT) return;
    note_interaction();
    if (key == S3_ROOM_KEY_CONFIRM) { s3_room_submit(); return; } /* May delete this page. */
    if (key == S3_ROOM_KEY_BACKSPACE) lv_textarea_del_char(s3_room.active_input);
    else lv_textarea_add_text(s3_room.active_input,
                             lv_btnmatrix_get_btn_text(lv_event_get_target(event), key));
}

static void s3_room_key_draw(lv_event_t *event)
{
    lv_obj_draw_part_dsc_t *part = lv_event_get_draw_part_dsc(event);
    if (lv_obj_draw_part_check_type(part, &lv_btnmatrix_class, LV_BTNMATRIX_DRAW_PART_BTN) &&
        part->id == S3_ROOM_KEY_BACKSPACE)
        part->label_dsc->font = &lv_font_montserrat_14;
}

static lv_obj_t *s3_room_input(lv_obj_t *screen, int x, int width, const char *hint, unsigned length)
{
    lv_obj_t *input = lv_textarea_create(screen);
    lv_obj_set_pos(input, x, 44);
    lv_obj_set_size(input, width, 36);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, length);
    lv_textarea_set_accepted_chars(input, "0123456789");
    lv_textarea_set_placeholder_text(input, hint);
    lv_obj_set_style_text_font(input, &PRODUCT_TEXT_FONT, 0);
    lv_obj_set_style_pad_all(input, 6, 0);
    lv_obj_set_style_radius(input, 6, 0);
    lv_obj_set_style_border_width(input, 2, 0);
    lv_obj_set_style_border_color(input, lv_color_hex(0x343A3F), 0);
    lv_obj_set_style_border_color(input, lv_color_hex(S3_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(input, lv_color_hex(S3_SURFACE), 0);
    lv_obj_set_style_text_color(input, lv_color_hex(S3_TEXT), 0);
    lv_obj_add_event_cb(input, s3_room_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(input, s3_room_focus, LV_EVENT_CLICKED, NULL);
    return input;
}

static void s3_room_render(lv_obj_t *screen)
{
    s3_room_navigation();
    s3_room.keyboard = NULL;
    s3_room.active_input = NULL;
    s3_room.next_refresh = 0;
    (void)starter_runtime_room_read(s3_room.offset, &s3_room.view);
    s3_room.rendered_assigned = s3_room.view.assigned;
    s3_room.leave = NULL;
    s3_room.create_button = NULL;
    s3_room.join_button = NULL;
    s3_header(screen, s3_room.form ? (s3_room.create ? "创建房间" : "加入房间") : "多人对讲",
              (product_action_t)S3_ACTION_ROOM_BACK);
    if (s3_room.form) {
        s3_room.entry = s3_room.create ? NULL : s3_room_input(screen, 8, 146, "六位房间号", 6);
        s3_room.password = s3_room_input(screen, s3_room.create ? 8 : 162,
                                       s3_room.create ? 304 : 150, "密码（可选）", 4);
        lv_textarea_set_password_mode(s3_room.password, true);
        s3_room.keyboard = lv_btnmatrix_create(screen);
        lv_obj_remove_style_all(s3_room.keyboard);
        lv_obj_clear_flag(s3_room.keyboard, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        /* 4 columns x 3 rows: about 72x48 px per key, with no duplicate submit
         * bar. Constant matrix maps are retained, never written by LVGL. */
        lv_obj_set_align(s3_room.keyboard, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(s3_room.keyboard, 8, 84);
        lv_obj_set_size(s3_room.keyboard, 304, 152);
        lv_obj_set_style_bg_opa(s3_room.keyboard, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(s3_room.keyboard, 0, 0);
        lv_obj_set_style_pad_row(s3_room.keyboard, 4, 0);
        lv_obj_set_style_pad_column(s3_room.keyboard, 6, 0);
        lv_obj_set_style_radius(s3_room.keyboard, 6, LV_PART_ITEMS);
        lv_obj_set_style_bg_opa(s3_room.keyboard, LV_OPA_COVER, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(s3_room.keyboard, lv_color_hex(0x292E32), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(s3_room.keyboard, lv_color_hex(0x465057), LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(s3_room.keyboard, lv_color_hex(S3_ACCEPT), LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s3_room.keyboard, lv_color_hex(0x348568),
                                  LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_set_style_text_font(s3_room.keyboard, &PRODUCT_TEXT_FONT, LV_PART_ITEMS);
        lv_obj_set_style_text_color(s3_room.keyboard, lv_color_hex(S3_TEXT), LV_PART_ITEMS);
        lv_btnmatrix_set_map(s3_room.keyboard, (const char **)s3_room_keymap);
        lv_btnmatrix_set_ctrl_map(s3_room.keyboard, s3_room_keyctrl);
        lv_obj_add_event_cb(s3_room.keyboard, s3_room_key_draw, LV_EVENT_DRAW_PART_BEGIN, NULL);
        lv_obj_add_event_cb(s3_room.keyboard, s3_room_key, LV_EVENT_VALUE_CHANGED, NULL);
        s3_room_select_input(s3_room.create ? s3_room.password : s3_room.entry);
        return;
    }
    if (!s3_room.view.assigned) {
        lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_wifi_signal, 236, 10);
        (void)s3_icon(screen, LV_SYMBOL_REFRESH, 272, 0, 44, (product_action_t)S3_ACTION_ROOM_REFRESH);
        s3_room.state = make_label(screen, s3_room_state_text(&s3_room.view), 12, 52, 296, lv_color_hex(S3_MUTED));
        s3_room.create_button = make_button(screen, "创建房间", 8, 87, 304, 60, (product_action_t)S3_ACTION_ROOM_CREATE);
        s3_room.join_button = make_button(screen, "加入房间", 8, 157, 304, 60, (product_action_t)S3_ACTION_ROOM_JOIN);
        bool ready = s3_room.view.app_open && s3_room.view.assignment_known && !s3_room.view.busy;
        s3_set_enabled(s3_room.create_button, ready);
        s3_set_enabled(s3_room.join_button, ready);
        return;
    }
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_header_title, 126);
    lv_obj_set_pos(s_wifi_signal, 192, 10);
    s3_room.leave = make_button(screen, "退出房间", 228, 0, 84, 44, (product_action_t)S3_ACTION_ROOM_LEAVE);
    s3_room.code = make_label(screen, "", 12, 46, 170, lv_color_hex(S3_TEXT));
    s3_room.state = make_label(screen, "", 176, 46, 140, lv_color_hex(S3_ACCENT));
    lv_label_set_long_mode(s3_room.state, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s3_room.state, 24);
    for (unsigned i = 0; i < STARTER_ROOM_PAGE_SIZE; ++i) {
        s3_room.members[i] = make_label(screen, "", 12, 76 + 26 * i, 204, lv_color_hex(S3_TEXT));
        lv_label_set_long_mode(s3_room.members[i], LV_LABEL_LONG_DOT);
        lv_obj_set_height(s3_room.members[i], 24);
        /* Reserve the self marker separately so a long name/ID cannot hide it. */
        s3_room.self[i] = make_label(screen, "（本机）", 144, 76 + 26 * i, 72, lv_color_hex(S3_MUTED));
        lv_obj_set_height(s3_room.self[i], 24);
        lv_obj_add_flag(s3_room.self[i], LV_OBJ_FLAG_HIDDEN);
        s3_room.status[i] = make_label(screen, "", 222, 76 + 26 * i, 94, lv_color_hex(S3_ACCENT));
        lv_obj_set_height(s3_room.status[i], 24);
    }
    s3_room.counter = make_label(screen, "", 12, 162, 188, lv_color_hex(S3_MUTED));
    s3_room.previous = s3_icon(screen, LV_SYMBOL_LEFT, 204, 154, 50, (product_action_t)S3_ACTION_ROOM_PREVIOUS);
    s3_room.next = s3_icon(screen, LV_SYMBOL_RIGHT, 264, 154, 48, (product_action_t)S3_ACTION_ROOM_NEXT);
    lv_obj_set_height(s3_room.previous, 36);
    lv_obj_set_height(s3_room.next, 36);
    s3_room.ptt = make_button(screen, "按住讲话", 8, 192, 304, 44, 0);
    lv_obj_set_style_bg_color(s3_room.ptt, lv_color_hex(S3_ACCEPT), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s3_room.ptt, s3_room_ptt_event, LV_EVENT_ALL, NULL);
}

static bool s3_room_action(product_action_t action)
{
    switch ((unsigned)action) {
    case S3_ACTION_ROOM:
        if (!enter_page(PAGE_ROOM)) return true;
        s3_room.form = false;
        s3_room.offset = 0;
        break;
    case S3_ACTION_ROOM_BACK:
        s3_room_release();
        if (s3_room.form) s3_room.form = false;
        else {
            s_page = PAGE_MENU;
            s3_room_navigation(); /* Disconnect only; keep the server assignment. */
        }
        break;
    case S3_ACTION_ROOM_CREATE:
    case S3_ACTION_ROOM_JOIN:
        if (s3_room.view.busy || !s3_room.view.app_open || !s3_room.view.assignment_known) {
            s3_feedback("请等待房间同步完成"); return true;
        }
        s3_room.form = true;
        s3_room.create = (unsigned)action == S3_ACTION_ROOM_CREATE;
        break;
    case S3_ACTION_ROOM_LEAVE:
        s3_room_release();
        if (starter_runtime_room_leave() != ESP_OK) s3_feedback("退出未提交，请重试");
        return true;
    case S3_ACTION_ROOM_REFRESH:
        if (starter_runtime_room_refresh() != ESP_OK) s3_feedback("刷新未提交，请重试");
        return true;
    case S3_ACTION_ROOM_PREVIOUS:
        if (s3_room.offset >= 3) s3_room.offset -= 3;
        s3_room.next_refresh = 0;
        return true;
    case S3_ACTION_ROOM_NEXT:
        if (s3_room.offset + 3 < s3_room.view.member_count) s3_room.offset += 3;
        s3_room.next_refresh = 0;
        return true;
    default: return false;
    }
    render_page();
    return true;
}

static void s3_room_refresh(int64_t now, const starter_runtime_product_snapshot_t *product)
{
    s3_room_navigation();
    if (s_page != PAGE_ROOM) { s3_room_release(); return; }
    if (s3_room.held_generation) note_interaction();
    if (now < s3_room.next_refresh) return;
    s3_room.next_refresh = now + 100;
    if (!starter_runtime_room_read(s3_room.offset, &s3_room.view)) return;
    if (s3_room.held_generation && s3_room.held_generation != s3_room.view.generation) s3_room_release();
    if (s3_room.form) {
        if (s3_room.view.assignment_known && s3_room.view.assigned) {
            s3_room.form = false;
            render_page();
        }
        return;
    }
    if (s3_room.rendered_assigned != s3_room.view.assigned) { render_page(); return; }
    label_set_text_if_changed(s3_room.state, s3_room_state_text(&s3_room.view));
    if (!s3_room.view.assigned) {
        bool ready = s3_room.view.app_open && s3_room.view.assignment_known && !s3_room.view.busy;
        s3_set_enabled(s3_room.create_button, ready);
        s3_set_enabled(s3_room.join_button, ready);
        return;
    }
    s3_room.offset = s3_room.view.offset;
    char text[80];
    snprintf(text, sizeof(text), "房间 %s", s3_room.view.room_code);
    label_set_text_if_changed(s3_room.code, text);
    snprintf(text, sizeof(text), s3_room.view.snapshot_ready ? "%u 人在线  %u/%u" : "等待成员同步",
             s3_room.view.member_count, s3_room.offset / 3 + 1,
             (s3_room.view.member_count + 2) / 3 > 0 ? (s3_room.view.member_count + 2) / 3 : 1);
    label_set_text_if_changed(s3_room.counter, text);
    for (unsigned i = 0; i < STARTER_ROOM_PAGE_SIZE; ++i) {
        starter_room_member_t *member = &s3_room.view.members[i];
        const char *name = member->self && s3_room.view.self_name[0] ? s3_room.view.self_name : member->device_id;
        if (!member->self && i < s3_room.view.count) {
            for (unsigned j = 0; j < product->contact_count; ++j) {
                const starter_product_contact_t *contact = &product->contacts[j];
                if (contact->source == STARTER_CONTACT_DEVICE &&
                    strcmp(contact->id, member->device_id) == 0 && contact->name[0]) {
                    name = contact->name;
                    break;
                }
            }
        }
        label_set_text_if_changed(s3_room.members[i], i < s3_room.view.count ?
                                 name : "");
        bool self = i < s3_room.view.count && member->self;
        lv_obj_set_width(s3_room.members[i], self ? 128 : 204);
        if (self) lv_obj_clear_flag(s3_room.self[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s3_room.self[i], LV_OBJ_FLAG_HIDDEN);
        bool speaking = member->self ? s3_room.view.phase == STARTER_ROOM_SPEAKING : member->speaking;
        label_set_text_if_changed(s3_room.status[i], i < s3_room.view.count ?
                                 (speaking ? "讲话中" : "收听中") : "");
    }
    bool active = s3_room.view.phase == STARTER_ROOM_LISTENING || s3_room.view.phase == STARTER_ROOM_SPEAKING;
    s3_set_enabled(s3_room.ptt, active && !s3_room.view.busy);
    s3_set_enabled(s3_room.leave, s3_room.view.assignment_known && !s3_room.view.busy);
    s3_set_enabled(s3_room.previous, s3_room.offset > 0);
    s3_set_enabled(s3_room.next, s3_room.offset + 3 < s3_room.view.member_count);
    label_set_text_if_changed(lv_obj_get_child(s3_room.ptt, 0),
        s3_room.view.phase == STARTER_ROOM_SPEAKING ? "正在讲话，松开收听" : "按住讲话");
    lv_obj_set_style_bg_color(s3_room.ptt, lv_color_hex(s3_room.view.phase == STARTER_ROOM_SPEAKING ?
                                                    S3_ACCEPT : S3_SURFACE), 0);
}
