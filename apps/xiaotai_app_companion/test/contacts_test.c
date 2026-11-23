// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_contact_entry.py
// Replaces legacy: tests/xmini/xmini_business/test_business.c::pending_response
#include "fakes.h"

#include <assert.h>
#include <string.h>
#include "xiaotai/modules/contacts.h"
#include "xiaotai/port/os.h"

#define ITEMS 8U
#define ENTRY_ITEMS 10U
#define MOD_PAGE 4U
static xiaotai_signal_t fence;
static xiaotai_contact_t roster[ENTRY_ITEMS];
static char pending[3][XIAOTAI_CONTACT_ID_MAX + 1U];
static size_t roster_count, pending_count;
static unsigned refreshes, shows, dials, video_dials, codes, decisions;
static int kind_filter;
static uint16_t page_number;
static uint32_t revision, pending_revision, generation;
static bool load_on_refresh, loading, video_available;
static xiaotai_err_t last_dial_result, load_error;
static bool pending_loading;
static xiaotai_err_t pending_error;
static int32_t pending_code;
static char target[XIAOTAI_CONTACT_ID_MAX + 1U], target_kind[8];
static xiaotai_contacts_change_kind_t decision;

static const xiaotai_contact_kind_def_t kinds[] = {
    {.name = "device", .friendships = true},
    {.name = "wechat"},
};

const xiaotai_contact_kind_def_t *xiaotai_contacts_kind(int kind)
{
    return kind >= 0 && (unsigned)kind < sizeof(kinds) / sizeof(kinds[0]) ? &kinds[kind] : NULL;
}

xiaotai_err_t xiaotai_contacts_kind_find(const char *name, int *kind)
{
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        if (strcmp(name, kinds[i].name)) continue;
        *kind = (int)i;
        return XIAOTAI_OK;
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

static unsigned filtered_count(void)
{
    unsigned count = 0;
    for (size_t i = 0; i < roster_count; ++i) count += kind_filter == XIAOTAI_CONTACT_ANY || roster[i].kind == kind_filter;
    return count;
}

void xiaotai_contacts_status(xiaotai_contacts_status_t *out)
{
    const unsigned total = filtered_count(), offset = page_number * MOD_PAGE;
    const unsigned left = total > offset ? total - offset : 0;
    *out = (xiaotai_contacts_status_t){.revision = revision, .generation = generation,
        .kind = (int8_t)kind_filter, .page = page_number, .pages = total ? (uint16_t)((total + MOD_PAGE - 1) / MOD_PAGE) : 1,
        .page_size = MOD_PAGE, .page_count = (uint8_t)(left > MOD_PAGE ? MOD_PAGE : left),
        .fresh = !loading && !load_error, .loading = loading, .error = load_error};
}

xiaotai_err_t xiaotai_contacts_at(uint8_t index, uint32_t flags, xiaotai_contact_t *out)
{
    assert(xiaotai_on_owner() && flags == XIAOTAI_CONTACTS_STALE_OK);
    if (!generation || index >= MOD_PAGE) return XIAOTAI_ERR_NOT_FOUND;
    unsigned i = page_number * MOD_PAGE + index;
    for (size_t p = 0; p < roster_count; ++p) {
        if (kind_filter != XIAOTAI_CONTACT_ANY && roster[p].kind != kind_filter) continue;
        if (i--) continue;
        *out = roster[p];
        return XIAOTAI_OK;
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_contacts_first(int kind, uint32_t flags, xiaotai_contact_t *out)
{
    assert(xiaotai_on_owner() && flags == XIAOTAI_CONTACTS_STALE_OK);
    for (size_t i = 0; i < roster_count; ++i) {
        if (roster[i].kind != kind) continue;
        *out = roster[i];
        return XIAOTAI_OK;
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_contacts_find(const char *id, int kind, uint32_t flags, xiaotai_contact_t *out)
{
    assert(xiaotai_on_owner() && flags == XIAOTAI_CONTACTS_STALE_OK);
    for (size_t i = 0; i < roster_count; ++i) {
        if (roster[i].kind != kind || strcmp(roster[i].id, id)) continue;
        *out = roster[i];
        return XIAOTAI_OK;
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

static void emit_change(const char *event)
{
    assert(xiaotai_on_owner());
    assert(xiaotai_event_emit(event, NULL, NULL, 0) == XIAOTAI_OK);
}

xiaotai_err_t xiaotai_contacts_show(int kind, uint16_t page)
{
    assert(xiaotai_on_owner());
    ++shows;
    kind_filter = kind;
    page_number = page;
    ++revision;
    emit_change(XIAOTAI_EVENT_CONTACTS_CHANGED);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_refresh(void)
{
    assert(xiaotai_on_owner());
    ++refreshes;
    loading = load_on_refresh;
    ++revision;
    emit_change(XIAOTAI_EVENT_CONTACTS_CHANGED);
    return XIAOTAI_OK;
}

void xiaotai_contacts_pending_status(xiaotai_contacts_pending_status_t *out)
{
    *out = (xiaotai_contacts_pending_status_t){.revision = pending_revision, .fresh = !pending_loading && !pending_error,
        .loading = pending_loading, .error = pending_error, .code = pending_code,
        .pages = 1, .page_size = 4, .page_count = (uint8_t)pending_count};
}

xiaotai_err_t xiaotai_contacts_pending_show(uint16_t page)
{
    assert(xiaotai_on_owner() && page == 0);
    ++pending_revision;
    emit_change(XIAOTAI_EVENT_CONTACTS_PENDING);
    return XIAOTAI_OK;
}

size_t xiaotai_contacts_pending_page(char (*ids)[XIAOTAI_CONTACT_ID_MAX + 1U], size_t capacity)
{
    assert(xiaotai_on_owner());
    const size_t count = pending_count < capacity ? pending_count : capacity;
    memcpy(ids, pending, count * sizeof(*ids));
    return count;
}

xiaotai_err_t xiaotai_contacts_change(const xiaotai_contacts_change_t *change)
{
    assert(xiaotai_on_owner());
    decision = change->kind;
    strcpy(target, change->id);
    ++decisions;
    return XIAOTAI_OK;
}

static xiaotai_err_t dial_contact(const char *kind, const char *id, bool video)
{
    assert(xiaotai_on_owner());
    strcpy(target, id);
    strcpy(target_kind, kind);
    if (last_dial_result != XIAOTAI_OK) return last_dial_result;
    ++dials;
    video_dials += video;
    return XIAOTAI_OK;
}

static bool video_supported(void) { return video_available; }
static xiaotai_err_t code(void) { assert(xiaotai_on_owner()); ++codes; return XIAOTAI_OK; }

extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_contacts;
static const xiaotai_app_companion_part_t routes = {.name = "routes", .dial_contact = dial_contact,
    .contact_video_available = video_supported, .show_wechat_code = code};
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {
    &xiaotai_app_companion_with_contacts, &routes, NULL,
};

static void signal(void *ctx) { (void)ctx; xiaotai_signal_give(&fence); }

static void flush(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        assert(xiaotai_run(signal, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void send(const char *intent, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_intent_post(intent, &args) == XIAOTAI_OK);
    flush();
}

static void row(unsigned i)
{
    assert(i < fixture.item_count && fixture.items[i].intent);
    send(fixture.items[i].intent, NULL, fixture.items[i].value);
}

static void action(const char *icon)
{
    for (unsigned i = 0; i < fixture.action_count; ++i) {
        const xiaotai_view_action_t entry = fixture.actions[i];
        if (!entry.icon || strcmp(entry.icon, icon)) continue;
        send(entry.intent, NULL, entry.value);
        return;
    }
    assert(false);
}

static void in_page(const char *page) { assert(!strcmp(fixture.id, page)); }

static void open_page(const char *page, int32_t value)
{
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, page, value);
    in_page(page);
}

static void go_back(const char *page)
{
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    in_page(page);
}

static void on_refresh(const xiaotai_args_t *args, void *ctx)
{
    (void)args; (void)ctx;
    (void)xiaotai_contacts_refresh();
}

static xiaotai_err_t install(void)
{
    const xiaotai_err_t err = companion_fixture_init();
    return err == XIAOTAI_OK ? xiaotai_intent_handle(XIAOTAI_INTENT_CONTACTS_REFRESH, on_refresh, NULL) : err;
}

static void boot(bool keys, bool entries)
{
    const xiaotai_app_companion_config_t app = {.contacts_per_page = entries ? 0 : 2, .requests_per_page = 2,
        .contacts_dial_on_select = keys, .contacts_number_unnamed_wechat = keys};
    assert(xiaotai_app_companion_configure(&app) == XIAOTAI_OK);
    memset(roster, 0, sizeof(roster));
    roster_count = entries ? ENTRY_ITEMS : ITEMS;
    for (unsigned i = 0; i < roster_count; ++i) {
        roster[i].kind = (int8_t)(i % 2U);
        roster[i].online = roster[i].online_known = true;
        roster[i].id[0] = 'a' + (char)i;
        roster[i].id[1] = '\0';
        static const char *const device_names[] = {"设备甲", "设备丙", "设备戊", "设备庚"};
        if (entries) roster[i].name[0] = 'A' + (char)i;
        else if (!roster[i].kind) strcpy(roster[i].name, device_names[i / 2U]);
    }
    if (!entries) {
        strcpy(roster[1].name, roster[1].id);
        memset(roster[3].id, 'x', XIAOTAI_CONTACT_ID_MAX);
    }
    strcpy(pending[0], "visitor-a");
    strcpy(pending[1], "visitor-b");
    strcpy(pending[2], "visitor-c");
    pending_count = 3;
    generation = 1;
    kind_filter = XIAOTAI_CONTACT_ANY;
    fixture = (companion_test_fixture_t){.needs = XIAOTAI_NEED_DISPLAY | (keys ? XIAOTAI_NEED_KEY : XIAOTAI_NEED_TOUCH)};
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.binding.generation = generation;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    static const xiaotai_module_t module = {.name = "probe", .init = install};
    static const xiaotai_module_t *const modules[] = {&module};
    static const xiaotai_application_t application = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground,
        .claim = xiaotai_app_companion_claim};
    static const xiaotai_application_t *const applications[] = {&application};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1,
        .applications = applications, .application_count = 1, .available_needs = fixture.needs};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    flush();
    in_page("home");
}

static void mutate(void *arg)
{
    switch ((uintptr_t)arg) {
    case 0: break;
    case 1: roster[3].id[0] = 'z'; break;
    case 2: roster_count = 0; break;
    case 3: video_available = true; break;
    case 4: loading = false; break;
    case 5: fixture.binding.generation = ++generation; break;
    case 6: last_dial_result = XIAOTAI_ERR_BUSY; break;
    case 7: load_error = XIAOTAI_ERR_INVALID_RESPONSE; break;
    case 8:
        pending_count = 1;
        ++pending_revision;
        emit_change(XIAOTAI_EVENT_CONTACTS_PENDING);
        return;
    case 9: load_error = XIAOTAI_OK; break;
    }
    ++revision;
    emit_change(XIAOTAI_EVENT_CONTACTS_CHANGED);
}

static void update(unsigned code_value)
{
    assert(xiaotai_run(mutate, (void *)(uintptr_t)code_value) == XIAOTAI_OK);
    flush();
}

typedef struct {
    xiaotai_err_t error;
    int32_t code;
    bool loading;
    size_t count;
} pending_response_t;

static void set_pending_response(void *ctx)
{
    const pending_response_t *result = ctx;
    pending_loading = result->loading;
    pending_error = result->error;
    pending_code = result->code;
    pending_count = result->count;
    ++pending_revision;
    emit_change(XIAOTAI_EVENT_CONTACTS_PENDING);
}

static void pending_response(pending_response_t result)
{
    assert(xiaotai_run(set_pending_response, &result) == XIAOTAI_OK);
    flush();
}

static void swap_pending_rows(void *unused)
{
    (void)unused;
    char first[sizeof(pending[0])];
    memcpy(first, pending[0], sizeof(first));
    memcpy(pending[0], pending[1], sizeof(first));
    memcpy(pending[1], first, sizeof(first));
    ++pending_revision;
    emit_change(XIAOTAI_EVENT_CONTACTS_PENDING);
}

static void pointer_pages(void)
{
    send(XIAOTAI_INTENT_SYSTEM_MENU, NULL, 0);
    in_page("menu");
    row(0);
    in_page("contacts");
    assert(shows == 1 && refreshes == 1 && fixture.talk_stops == 1);
    assert(fixture.item_count == 3 && !strcmp(fixture.items[1].label, "微信联系人"));
    action(XIAOTAI_ICON_REFRESH);
    assert(refreshes == 2);
    row(2);
    assert(page_number == 0 && fixture.item_count == 3 && !strcmp(fixture.items[0].label, "设备丙"));
    row(1);
    in_page("contact");
    assert(dials == 0 && fixture.subtitle[0] == '\0' && fixture.action_count == 2);
    assert(!strcmp(fixture.title, "微信联系人") && !strcmp(fixture.text, "微信语音"));
    update(3);
    assert(fixture.action_count == 3);
    action(XIAOTAI_ICON_VIDEO);
    assert(dials == 1 && video_dials == 1 && !strcmp(target_kind, "wechat"));
    assert(strlen(target) == XIAOTAI_CONTACT_ID_MAX && !strcmp(fixture.toast, "正在发起呼叫"));
    const xiaotai_view_action_t stale = fixture.actions[0];
    update(1);
    assert(fixture.action_count == 1 && !strcmp(fixture.text, "联系人已更新，请返回重试"));
    send(stale.intent, NULL, stale.value);
    send(XIAOTAI_INTENT_APP_COMPANION_DIAL, NULL, 0);
    assert(dials == 1 && fixture.last_refusal.value == XIAOTAI_ERR_NOT_FOUND);
    go_back("contacts");
    assert(!strcmp(fixture.items[0].label, "设备丙"));
    const xiaotai_view_item_t stale_row = fixture.items[0];
    row(2);
    assert(page_number == 1 && !strcmp(fixture.items[0].label, "设备戊"));
    send(stale_row.intent, NULL, stale_row.value);
    send(XIAOTAI_INTENT_APP_COMPANION_CONTACT, NULL, 30);
    in_page("contacts");
    assert(dials == 1 && fixture.last_refusal.value == XIAOTAI_ERR_NOT_FOUND);
    row(0);
    in_page("contact");
    assert(!strcmp(fixture.subtitle, "e") && !strcmp(fixture.text, "设备在线"));
    go_back("contacts");
    go_back("menu");
    assert(fixture.focus == 1);
    go_back("home");
    action(XIAOTAI_ICON_PHONE);
    assert(dials == 2 && !strcmp(target, "b"));
    update(2);
    send(XIAOTAI_INTENT_APP_COMPANION_DIAL, "first", 0);
    assert(codes == 1 && dials == 2);

    open_page("settings", 0);
    for (unsigned i = 0; i < fixture.item_count; ++i) assert(strcmp(fixture.items[i].label, "好友申请"));
}

static void pointer_entries(void)
{
    open_page("contacts", XIAOTAI_APP_COMPANION_CONTACTS_ALL);
    for (unsigned i = 0; i < ENTRY_ITEMS; ++i) {
        const unsigned index = i % MOD_PAGE;
        if (i && !index) row(MOD_PAGE);
        assert(index < fixture.item_count && !strcmp(fixture.items[index].label, roster[i].name));
        row(index);
        in_page("contact");
        assert(!strcmp(fixture.title, roster[i].name) && !dials);
        go_back("contacts");

        send(XIAOTAI_INTENT_APP_COMPANION_CONTACT, NULL, (int32_t)index);
        in_page("contact");
        assert(!strcmp(fixture.title, roster[i].name) && !dials);
        go_back("contacts");
    }
}

static void key_pages(void)
{
    assert(fixture.item_count >= 2 && !strcmp(fixture.items[0].label, "微信通话") &&
           !strcmp(fixture.items[1].label, "设备通话"));
    load_on_refresh = true;
    row(0);
    in_page("contacts");
    assert(kind_filter == 1 && fixture.talk_stops == 0 && refreshes == 1);
    assert(fixture.item_count == 0 && !strcmp(fixture.status, "正在加载"));
    update(4);
    assert(fixture.item_count == 3 && !strcmp(fixture.items[2].label, "更多联系人"));
    row(0);
    in_page("contacts");
    assert(dials == 1 && video_dials == 0 && !strcmp(target_kind, "wechat") && !strcmp(target, "b"));
    row(2);
    row(0);
    assert(dials == 2 && !strcmp(target, "f"));
    row(2);
    assert(page_number == 0 && !strcmp(fixture.items[0].label, "微信联系人 1"));
    update(6);
    row(0);
    assert(dials == 2 && fixture.last_refusal.value == XIAOTAI_ERR_BUSY);
    update(7);
    assert(!fixture.item_count && !strcmp(fixture.status, "加载失败"));
    update(9);
    assert(fixture.item_count == 3 && !strcmp(fixture.items[0].label, "微信联系人 1"));
    go_back("home");
    open_page("settings", 0);
    unsigned i;
    for (i = 0; i < fixture.item_count; ++i) {
        if (!strcmp(fixture.items[i].label, "好友申请")) { row(i); break; }
    }
    in_page("requests");
    assert(fixture.item_count == 3 && !strcmp(fixture.items[2].label, "更多申请"));
    row(2);
    assert(fixture.item_count == 2 && !strcmp(fixture.items[0].label, "visitor-c") &&
           !strcmp(fixture.items[1].label, "更多申请"));
    row(1);
    assert(fixture.item_count == 3 && !strcmp(fixture.items[0].label, "visitor-a"));
    row(2);
    row(0);
    in_page("request");
    action(XIAOTAI_ICON_ACCEPT);
    assert(decisions == 1 && decision == XIAOTAI_CONTACTS_ACCEPT && !strcmp(target, "visitor-c"));
    update(5);
    send(XIAOTAI_INTENT_APP_COMPANION_DECIDE, NULL, 0);
    assert(decisions == 1 && fixture.last_refusal.value == XIAOTAI_ERR_NOT_FOUND);
    update(8);
    assert(!strcmp(fixture.text, "申请已处理") && fixture.action_count == 1);
    send(XIAOTAI_INTENT_APP_COMPANION_DECIDE, NULL, 0);
    assert(decisions == 1);
    go_back("requests");
    pending_response((pending_response_t){.loading = true});
    assert(!fixture.item_count && !strcmp(fixture.status, "正在加载"));
    pending_response((pending_response_t){.error = XIAOTAI_FAIL, .code = 40209});
    assert(!fixture.item_count && !strcmp(fixture.status, "加载失败"));
    pending_response((pending_response_t){.error = XIAOTAI_FAIL, .code = 40211});
    assert(!fixture.item_count && !strcmp(fixture.status, "加载失败"));
    pending_response((pending_response_t){.error = XIAOTAI_ERR_TIMEOUT});
    assert(!fixture.item_count && !strcmp(fixture.status, "加载失败"));
    pending_response((pending_response_t){0});
    assert(!fixture.item_count && !strcmp(fixture.status, "暂无申请"));
    pending_response((pending_response_t){.count = 1});
    assert(fixture.item_count == 1 && !strcmp(fixture.items[0].label, "visitor-a"));
    pending_response((pending_response_t){.count = 2});
    row(0);
    in_page("request");
    assert(!strcmp(fixture.text, "visitor-a"));
    assert(xiaotai_run(swap_pending_rows, NULL) == XIAOTAI_OK);
    flush();
    assert(!strcmp(fixture.text, "visitor-a"));
    action(XIAOTAI_ICON_ACCEPT);
    assert(decisions == 2 && decision == XIAOTAI_CONTACTS_ACCEPT && !strcmp(target, "visitor-a"));
    go_back("requests");
    go_back("settings");
}

static void verify_room_member_lookup(void *unused)
{
    (void)unused;
    const xiaotai_app_companion_part_t *part = &xiaotai_app_companion_with_contacts;
    char name[8];
    assert(part->room_member_name("a", name, sizeof(name)) && !strcmp(name, "设备"));
    assert(!part->room_member_name("b", name, sizeof(name))); /* a WeChat ID is not a device ID */
    assert(!part->room_member_name("a", name, 2));           /* never return a partial UTF-8 character */
}

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    const bool keys = argc > 1 && !strcmp(argv[1], "keys");
    const bool entries = argc > 1 && !strcmp(argv[1], "entry");
    boot(keys, entries);
    if (entries) {
        pointer_entries();
    } else {
        assert(xiaotai_run(verify_room_member_lookup, NULL) == XIAOTAI_OK);
        flush();
        if (keys) key_pages();
        else pointer_pages();
    }
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&fence);
    return 0;
}
