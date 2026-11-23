// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <stdio.h>
#include <string.h>
#include "companion_contact_route.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/mem.h"

#define CONTACTS_PAGE XIAOTAI_APP_COMPANION_PAGE_CONTACTS
#define CONTACT_PAGE XIAOTAI_APP_COMPANION_PAGE_CONTACT
#define REQUESTS_PAGE XIAOTAI_APP_COMPANION_PAGE_REQUESTS
#define REQUEST_PAGE XIAOTAI_APP_COMPANION_PAGE_REQUEST
#define CONTACT_INTENT XIAOTAI_INTENT_APP_COMPANION_CONTACT
#define DIAL_INTENT XIAOTAI_INTENT_APP_COMPANION_DIAL
#define REQUEST_INTENT XIAOTAI_INTENT_APP_COMPANION_REQUEST
#define DECIDE_INTENT XIAOTAI_INTENT_APP_COMPANION_DECIDE
#define ROW_INTENT "companion.contact.row"
#define ACTION_INTENT "companion.contact.action"
#define LABEL_BYTES (XIAOTAI_CONTACT_NAME_MAX + 1U)
#define ID_BYTES (XIAOTAI_CONTACT_ID_MAX + 1U)

typedef struct {
    uint32_t revision, generation;
    uint16_t page, pages;
    uint8_t page_size, count;
    int kind;
    xiaotai_err_t error;
    bool loading, fresh;
} directory_page_t;

typedef struct {
    directory_page_t page;
    uint32_t token;
    uint8_t offset, count;
    bool more, empty_refresh;
} list_view_t;

/* Only the rendered page stamp and selected key are retained, not a second
 * list. The module remains the authority for stale ids and actual dialing. */
static list_view_t s_lists[2];
static struct {
    char id[ID_BYTES];
    directory_page_t page;
    uint32_t ordinal;
    uint8_t offset, limit;
    int kind;
} s_selected[2];
static uint32_t s_serial, s_detail_token;
static char *s_large_labels;

static uint32_t next_token(void)
{
    s_serial = (s_serial + 1U) % (1U + INT32_MAX / XIAOTAI_VIEW_ITEMS_MAX);
    return s_serial;
}

static int32_t token_value(uint32_t token, unsigned index)
{
    return (int32_t)(token * XIAOTAI_VIEW_ITEMS_MAX + index);
}

static int named_kind(const char *name)
{
    int kind;
    return xiaotai_contacts_kind_find(name, &kind) == XIAOTAI_OK ? kind : -2;
}

static int filter_kind(int32_t arg)
{
    switch (arg) {
    case XIAOTAI_APP_COMPANION_CONTACTS_ALL: return XIAOTAI_CONTACT_ANY;
    case XIAOTAI_APP_COMPANION_CONTACTS_DEVICE: return named_kind("device");
    case XIAOTAI_APP_COMPANION_CONTACTS_WECHAT: return named_kind("wechat");
    default: return -2;
    }
}

static bool valid_filter(int32_t arg) { return filter_kind(arg) >= XIAOTAI_CONTACT_ANY; }

static const char *list_title(int32_t arg)
{
    switch (arg) {
    case XIAOTAI_APP_COMPANION_CONTACTS_DEVICE: return "设备通话";
    case XIAOTAI_APP_COMPANION_CONTACTS_WECHAT: return "微信通话";
    default: return "通讯录";
    }
}

static bool is_wechat(const xiaotai_contact_t *contact)
{
    const xiaotai_contact_kind_def_t *kind = xiaotai_contacts_kind(contact->kind);
    return kind && !strcmp(kind->name, "wechat");
}

static void copy_name(char *out, const xiaotai_contact_t *contact, uint32_t ordinal)
{
    const bool wx = is_wechat(contact);
    if (contact->name[0] && (!wx || strcmp(contact->name, contact->id))) strcpy(out, contact->name);
    else if (!wx) strcpy(out, contact->id);
    else if (companion.config.contacts_number_unnamed_wechat)
        (void)snprintf(out, LABEL_BYTES, "微信联系人 %lu", (unsigned long)ordinal);
    else strcpy(out, "微信联系人");
}

static char *labels(void) { return s_large_labels ? s_large_labels : companion.buffer->details; }

static directory_page_t read_page(bool pending)
{
    if (pending) {
        xiaotai_contacts_pending_status_t page;
        xiaotai_binding_status_t binding;
        xiaotai_contacts_pending_status(&page);
        xiaotai_binding_status(&binding);
        return (directory_page_t){.revision = page.revision, .generation = binding.generation,
            .page = page.page, .pages = page.pages,
            .page_size = page.page_size, .count = page.page_count, .loading = page.loading, .fresh = page.fresh,
            .error = page.error};
    }
    xiaotai_contacts_status_t page;
    xiaotai_contacts_status(&page);
    return (directory_page_t){.revision = page.revision, .generation = page.generation,
        .page = page.page, .pages = page.pages, .page_size = page.page_size, .count = page.page_count,
        .kind = page.kind, .loading = page.loading, .fresh = page.fresh, .error = page.error};
}

static bool same_page(const directory_page_t *a, const directory_page_t *b)
{
    return a->kind == b->kind && a->page == b->page && a->generation == b->generation;
}

static uint8_t page_limit(bool pending)
{
    return pending ? companion.config.requests_per_page : companion.config.contacts_per_page;
}

static bool rows_ready(const directory_page_t *page, bool pending)
{
    if (!page->count || xiaotai_quiesced()) return false;
    return !(pending || companion.config.contacts_dial_on_select) ||
           (!page->loading && page->error == XIAOTAI_OK);
}

static bool empty_refresh_page(const directory_page_t *page, int32_t arg, bool pending)
{
    return companion.config.contacts_refresh_empty_on_confirm && page->generation && !page->page &&
           !page->count && page->fresh && !page->loading && page->error == XIAOTAI_OK &&
           (pending || (arg == XIAOTAI_APP_COMPANION_CONTACTS_WECHAT && page->kind == named_kind("wechat")));
}

static const char *list_status(const directory_page_t *page, bool pending)
{
    if (xiaotai_quiesced()) return "设备维护中";
    if (page->loading) return "正在加载";
    if (page->error != XIAOTAI_OK) return "加载失败";
    if (page->count) return NULL;
    if (!companion_home_network_up()) return "网络未连接";
    return pending ? "暂无申请" : "暂无联系人";
}

static void add_back(xiaotai_view_t *view)
{
    companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){
        .role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK, .intent = XIAOTAI_INTENT_APP_COMPANION_BACK};
    view->actions = companion.buffer->actions;
}

static size_t read_pending(void)
{
    return xiaotai_contacts_pending_page((char (*)[ID_BYTES])companion.buffer->details,
        sizeof(companion.buffer->details) / ID_BYTES);
}

static const char *pending_id(unsigned index) { return companion.buffer->details + index * ID_BYTES; }

static void build_list(xiaotai_view_t *view, int32_t arg, bool pending)
{
    list_view_t *list = &s_lists[pending];
    const directory_page_t page = read_page(pending);
    const uint8_t offset = same_page(&list->page, &page) && list->offset < page.count ? list->offset : 0;
    *list = (list_view_t){.page = page, .offset = offset, .token = next_token()};
    view->kind = XIAOTAI_VIEW_LIST;
    view->title = pending ? "好友申请" : list_title(arg);
    view->items = companion.buffer->items;
    view->status = list_status(&page, pending);
    if (!pending && page.kind != filter_kind(arg)) {
        if (page.count && !view->status) view->status = "联系人已更新，请返回重试";
    } else if (rows_ready(&page, pending)) {
        const size_t available = pending ? read_pending() : page.count;
        for (unsigned i = offset; i < available && list->count < page_limit(pending); ++i) {
            const unsigned row = list->count;
            xiaotai_view_item_t item = {.intent = ROW_INTENT, .value = token_value(list->token, row)};
            if (pending) {
                item.label = pending_id(i);
            } else {
                xiaotai_contact_t contact;
                if (xiaotai_contacts_at((uint8_t)i, XIAOTAI_CONTACTS_STALE_OK, &contact) != XIAOTAI_OK) break;
                char *label = labels() + row * LABEL_BYTES;
                copy_name(label, &contact, (uint32_t)page.page * page.page_size + i + 1U);
                item.label = label;
                item.detail = is_wechat(&contact) ? "微信联系人" :
                    contact.online_known && contact.online ? "设备 · 在线" : "设备 · 离线";
            }
            companion.buffer->items[list->count++] = item;
        }
        list->more = list->count && (available > page_limit(pending) || page.pages > 1U);
        if (list->more) companion.buffer->items[list->count] = (xiaotai_view_item_t){
            .label = pending ? "更多申请" : "更多联系人", .intent = ROW_INTENT,
            .value = token_value(list->token, list->count)};
    }
    view->item_count = list->count + (size_t)list->more;
    list->empty_refresh = empty_refresh_page(&page, arg, pending) &&
                          !xiaotai_quiesced() && companion_home_network_up();
    if (list->empty_refresh) companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){
        .label = "刷新", .icon = XIAOTAI_ICON_REFRESH, .role = XIAOTAI_ROLE_PRIMARY,
        .intent = ROW_INTENT, .value = token_value(list->token, 0)};
    else if (!pending) companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){
        .label = "刷新", .icon = XIAOTAI_ICON_REFRESH, .intent = XIAOTAI_INTENT_CONTACTS_REFRESH};
    add_back(view);
}

static void build_contacts(xiaotai_view_t *view, int32_t arg) { build_list(view, arg, false); }
static void build_requests(xiaotai_view_t *view, int32_t arg) { build_list(view, arg, true); }

static bool selected_current(bool pending, xiaotai_contact_t *contact)
{
    if (!s_selected[pending].id[0]) return false;
    const directory_page_t page = read_page(pending);
    if (!same_page(&page, &s_selected[pending].page) || !rows_ready(&page, pending)) return false;
    const size_t count = pending ? read_pending() : page.count;
    unsigned i = s_selected[pending].offset;
    const unsigned end = i + s_selected[pending].limit;
    for (; i < count && i < end; ++i) {
        if (pending) {
            if (!strcmp(pending_id(i), s_selected[pending].id)) return true;
        } else {
            if (xiaotai_contacts_at((uint8_t)i, XIAOTAI_CONTACTS_STALE_OK, contact) != XIAOTAI_OK) return false;
            if (contact->kind == s_selected[pending].kind && !strcmp(contact->id, s_selected[pending].id)) return true;
        }
    }
    return false;
}

static void add_action(xiaotai_view_t *view, const char *label, const char *icon, unsigned i, uint8_t role)
{
    companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){
        .label = label, .icon = icon, .role = role, .intent = ACTION_INTENT, .value = token_value(s_detail_token, i)};
}

static void build_contact(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    s_detail_token = next_token();
    view->kind = XIAOTAI_VIEW_NOTICE;
    xiaotai_contact_t contact;
    if (selected_current(false, &contact)) {
        const bool wx = is_wechat(&contact);
        char *label = labels();
        copy_name(label, &contact, s_selected[0].ordinal);
        view->title = label;
        view->text = wx ? "微信语音" : contact.online_known && contact.online ? "设备在线" : "设备离线";
        if (!wx) {
            strcpy(label + LABEL_BYTES, contact.id);
            view->subtitle = label + LABEL_BYTES;
        }
        add_action(view, "语音呼叫", XIAOTAI_ICON_PHONE, 0, XIAOTAI_ROLE_PRIMARY);
        if (companion_contact_video_supported(&contact)) {
            add_action(view, "视频呼叫", XIAOTAI_ICON_VIDEO, 1, XIAOTAI_ROLE_NONE);
        }
    } else view->text = "联系人已更新，请返回重试";
    add_back(view);
}

static void build_request(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    s_detail_token = next_token();
    view->kind = XIAOTAI_VIEW_NOTICE;
    if (selected_current(true, NULL)) {
        view->text = s_selected[1].id;
        add_action(view, "接受", XIAOTAI_ICON_ACCEPT, 1, XIAOTAI_ROLE_PRIMARY);
        add_action(view, "拒绝", XIAOTAI_ICON_HANGUP, 0, XIAOTAI_ROLE_NONE);
    } else {
        const directory_page_t page = read_page(true);
        view->text = page.loading ? "正在加载" : page.error == XIAOTAI_OK ? "申请已处理" : "加载失败";
    }
    add_back(view);
}

static bool foreground_action(void)
{
    if (companion.status.foreground && !companion.status.setup && !xiaotai_quiesced()) return true;
    ++companion.status.stale;
    return false;
}

static void stale_row(const char *intent, bool pending)
{
    companion_refuse_text(intent, XIAOTAI_ERR_NOT_FOUND, pending ? "申请已处理" : "联系人已更新，请返回重试");
}

static void dial(const xiaotai_contact_t *contact, bool video)
{
    const xiaotai_err_t err = companion_dial_contact(contact, video);
    if (err == XIAOTAI_OK) xiaotai_view_toast("正在发起呼叫", companion_toast_ms());
    else companion_refuse(DIAL_INTENT, err);
}

static void next_page(bool pending, list_view_t *list, const directory_page_t *page)
{
    const unsigned next = list->offset + list->count;
    xiaotai_err_t err = XIAOTAI_OK;
    if (next < page->count) list->offset = (uint8_t)next;
    else if (page->pages <= 1U) list->offset = 0;
    else {
        const uint16_t index = (uint16_t)((page->page + 1U) % page->pages);
        err = pending ? xiaotai_contacts_pending_show(index) : xiaotai_contacts_show(page->kind, index);
        if (err == XIAOTAI_OK) list->offset = 0;
    }
    if (err == XIAOTAI_OK) companion_render();
    else companion_refuse(pending ? REQUEST_INTENT : CONTACT_INTENT, err);
}

static void refresh_empty(bool pending, uint32_t generation, const char *intent)
{
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (platform.gated) {
        companion_refuse(intent, XIAOTAI_ERR_PLATFORM_GATED);
        return;
    }
    if (!companion_home_network_up() || platform.phase != XIAOTAI_PLATFORM_ONLINE || !platform.authorized ||
        platform.generation != generation) {
        companion_refuse(intent, XIAOTAI_ERR_PLATFORM_OFFLINE);
        return;
    }
    const xiaotai_err_t err = pending ? xiaotai_contacts_pending_show(0) : xiaotai_contacts_refresh();
    if (err == XIAOTAI_OK) companion_render();
    else companion_refuse(intent, err);
}

static void select_row(bool pending, const xiaotai_args_t *args, bool has_token)
{
    if (!foreground_action()) return;
    if (!companion_on_page(pending ? REQUESTS_PAGE : CONTACTS_PAGE)) { ++companion.status.stale; return; }
    const char *intent = pending ? REQUEST_INTENT : CONTACT_INTENT;
    list_view_t *list = &s_lists[pending];
    const directory_page_t page = read_page(pending);
    const uint32_t value = (uint32_t)args->value;
    const unsigned row = has_token ? value % XIAOTAI_VIEW_ITEMS_MAX : value;
    if (args->value < 0 || (has_token && value / XIAOTAI_VIEW_ITEMS_MAX != list->token) ||
        !same_page(&page, &list->page) || page.revision != list->page.revision) {
        stale_row(intent, pending);
        return;
    }
    if (has_token && !row && list->empty_refresh &&
        empty_refresh_page(&page, companion.stack[companion.depth - 1U].arg, pending)) {
        refresh_empty(pending, page.generation, intent);
        return;
    }
    if (!rows_ready(&page, pending)) { stale_row(intent, pending); return; }
    if (row == list->count && list->more && has_token) { next_page(pending, list, &page); return; }
    if (row >= list->count) { stale_row(intent, pending); return; }
    const unsigned index = list->offset + row;
    if (pending) {
        if (index >= read_pending()) { stale_row(intent, true); return; }
        strcpy(s_selected[1].id, pending_id(index));
    } else {
        xiaotai_contact_t contact;
        if (xiaotai_contacts_at((uint8_t)index, XIAOTAI_CONTACTS_STALE_OK, &contact) != XIAOTAI_OK) {
            stale_row(intent, false);
            return;
        }
        if (companion.config.contacts_dial_on_select) { dial(&contact, false); return; }
        strcpy(s_selected[0].id, contact.id);
        s_selected[0].kind = contact.kind;
        s_selected[0].ordinal = (uint32_t)page.page * page.page_size + index + 1U;
    }
    s_selected[pending].page = page;
    s_selected[pending].offset = list->offset;
    s_selected[pending].limit = page_limit(pending);
    const xiaotai_err_t err = companion_show_page(pending ? REQUEST_PAGE : CONTACT_PAGE, 0);
    if (err != XIAOTAI_OK) companion_refuse(intent, err);
}

static void on_contact(const xiaotai_args_t *args, void *ctx) { (void)ctx; select_row(false, args, false); }
static void on_request(const xiaotai_args_t *args, void *ctx) { (void)ctx; select_row(true, args, false); }

static void on_row(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    select_row(companion_on_page(REQUESTS_PAGE), args, true);
}

static void first_contact(bool video)
{
    const int kind = named_kind("wechat");
    xiaotai_contact_t contact;
    xiaotai_err_t err = kind < 0 ? XIAOTAI_ERR_NOT_FOUND :
        xiaotai_contacts_first(kind, XIAOTAI_CONTACTS_STALE_OK, &contact);
    if (err == XIAOTAI_OK) dial(&contact, video);
    else if (err == XIAOTAI_ERR_NOT_FOUND || err == XIAOTAI_ERR_INVALID_STATE) {
        err = companion_show_wechat_code();
        if (err != XIAOTAI_OK) companion_refuse(DIAL_INTENT, err);
    } else companion_refuse(DIAL_INTENT, err);
}

static void on_first(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (foreground_action() && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) first_contact(false);
}

static void on_dial(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!foreground_action()) return;
    if ((args->value != 0 && args->value != 1) || (args->text[0] && strcmp(args->text, "first"))) {
        companion_refuse(DIAL_INTENT, XIAOTAI_ERR_INVALID_ARG);
        return;
    }
    if (args->text[0]) { first_contact(args->value != 0); return; }
    xiaotai_contact_t contact;
    if (companion_on_page(CONTACT_PAGE) && selected_current(false, &contact)) dial(&contact, args->value != 0);
    else stale_row(DIAL_INTENT, false);
}

static void on_decide(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!foreground_action()) return;
    if (args->value != 0 && args->value != 1) { companion_refuse(DECIDE_INTENT, XIAOTAI_ERR_INVALID_ARG); return; }
    if (!companion_on_page(REQUEST_PAGE) || !selected_current(true, NULL)) { stale_row(DECIDE_INTENT, true); return; }
    const xiaotai_contacts_change_t change = {.kind = args->value ? XIAOTAI_CONTACTS_ACCEPT : XIAOTAI_CONTACTS_REJECT,
        .id = s_selected[1].id};
    const xiaotai_err_t err = xiaotai_contacts_change(&change);
    if (err != XIAOTAI_OK) companion_refuse(DECIDE_INTENT, err);
}

static void on_action(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!foreground_action()) return;
    const bool pending = companion_on_page(REQUEST_PAGE);
    if (!pending && !companion_on_page(CONTACT_PAGE)) { ++companion.status.stale; return; }
    const uint32_t value = (uint32_t)args->value;
    if (args->value < 0 || value / XIAOTAI_VIEW_ITEMS_MAX != s_detail_token) {
        stale_row(pending ? DECIDE_INTENT : DIAL_INTENT, pending);
        return;
    }
    const xiaotai_args_t action = {.value = (int32_t)(value % XIAOTAI_VIEW_ITEMS_MAX)};
    if (pending) on_decide(&action, NULL);
    else on_dial(&action, NULL);
}

static void entered_contacts(void)
{
    const int kind = filter_kind(companion.stack[companion.depth - 1U].arg);
    s_lists[0].offset = 0;
    xiaotai_err_t err = xiaotai_contacts_show(kind, 0);
    if (err == XIAOTAI_OK && !read_page(false).loading) err = xiaotai_contacts_refresh();
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_PLATFORM_OFFLINE) companion_refuse(CONTACT_INTENT, err);
    companion_render();
}

static void entered_requests(void)
{
    s_lists[1].offset = 0;
    const xiaotai_err_t err = xiaotai_contacts_pending_show(0);
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_PLATFORM_OFFLINE) companion_refuse(REQUEST_INTENT, err);
    companion_render();
}

static void changed(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    const bool pending = !strcmp(event->name, XIAOTAI_EVENT_CONTACTS_PENDING);
    if (companion_on_page(pending ? REQUESTS_PAGE : CONTACTS_PAGE) ||
        companion_on_page(pending ? REQUEST_PAGE : CONTACT_PAGE)) companion_render();
}

static xiaotai_err_t init(void)
{
    static const companion_page_t list = {.id = CONTACTS_PAGE, .title = "通讯录", .icon = XIAOTAI_ICON_PHONE,
        .entry_title = list_title, .arg_valid = valid_filter, .build = build_contacts, .entered = entered_contacts};
    static const companion_page_t detail = {.id = CONTACT_PAGE, .title = "联系人", .build = build_contact};
    static const companion_page_t requests = {.id = REQUESTS_PAGE, .title = "好友申请",
        .build = build_requests, .entered = entered_requests};
    static const companion_page_t request = {.id = REQUEST_PAGE, .title = "申请人", .build = build_request};
    const xiaotai_contact_kind_def_t *device = xiaotai_contacts_kind(named_kind("device"));
    const bool friendships = device && device->friendships;
    xiaotai_err_t err = companion_page_register(&list);
    if (err == XIAOTAI_OK) err = companion_page_register(&detail);
    if (err == XIAOTAI_OK && friendships) err = companion_page_register(&requests);
    if (err == XIAOTAI_OK && friendships) err = companion_page_register(&request);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(CONTACT_INTENT, on_contact, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(DIAL_INTENT, on_dial, NULL);
    if (err == XIAOTAI_OK && friendships) err = xiaotai_intent_handle(REQUEST_INTENT, on_request, NULL);
    if (err == XIAOTAI_OK && friendships) err = xiaotai_intent_handle(DECIDE_INTENT, on_decide, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(ROW_INTENT, on_row, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(ACTION_INTENT, on_action, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK && named_kind("wechat") >= 0 && !companion.config.contacts_dial_on_select) {
        err = xiaotai_intent_handle_ex(COMPANION_PHONE_INTENT, on_first, NULL, XIAOTAI_INTENT_LOCAL);
    }
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_CONTACTS_CHANGED, changed, NULL);
    if (err == XIAOTAI_OK && friendships) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_CONTACTS_PENDING, changed, NULL);
    const size_t bytes = (size_t)page_limit(false) * LABEL_BYTES;
    if (err == XIAOTAI_OK && companion.has_display && bytes > sizeof(companion.buffer->details)) {
        s_large_labels = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
        if (!s_large_labels) err = XIAOTAI_ERR_NO_MEM;
    }
    return err;
}

static void reset(void)
{
    xiaotai_mem_free(s_large_labels);
    s_large_labels = NULL;
    memset(s_lists, 0, sizeof(s_lists));
    memset(s_selected, 0, sizeof(s_selected));
    s_serial = s_detail_token = 0;
}

static bool room_member_name(const char *id, char *out, size_t capacity)
{
    const int kind = named_kind("device");
    xiaotai_contact_t contact;
    if (kind < 0 || !capacity || xiaotai_contacts_find(id, kind, XIAOTAI_CONTACTS_STALE_OK, &contact) != XIAOTAI_OK ||
        !contact.name[0]) return false;
    size_t n = strnlen(contact.name, capacity);
    if (n == capacity) {
        --n;
        while (n && ((unsigned char)contact.name[n] & 0xc0U) == 0x80U) --n;
    }
    if (!n) return false;
    memcpy(out, contact.name, n);
    out[n] = '\0';
    return true;
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_contacts = {.name = "contacts",
    .init = init, .reset = reset, .room_member_name = room_member_name};
