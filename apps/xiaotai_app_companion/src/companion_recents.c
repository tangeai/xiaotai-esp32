// SPDX-License-Identifier: MIT
#include "companion_recents.h"

#include <stdio.h>
#include <string.h>
#include "companion_call_ui.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define PAGE XIAOTAI_APP_COMPANION_PAGE_RECENTS
#define ROW_INTENT "companion.recent.row"
#define ALL_INTENT "companion.recent.all"
#define RECENTS 8U
#define DETAIL_BYTES 64U
_Static_assert(RECENTS * DETAIL_BYTES <= sizeof(((companion_view_buffer_t *)0)->details), "a line for every entry");

typedef struct {
    char id[XIAOTAI_CONTACT_ID_MAX + 1U];
    char name[XIAOTAI_CONTACT_NAME_MAX + 1U];
    int64_t at_us;
    uint32_t serial;
    uint8_t missed;             /* calls in a row nobody answered here */
    bool wechat, outgoing, answered, used;
} recent_t;

static struct {
    recent_t *list;             /* newest first */
    char dialled[XIAOTAI_CONTACT_ID_MAX + 1U];
    bool dialled_wechat;
    uint32_t serial, unseen;
    char notice[32];
} s;

static int kind_of(bool wechat)
{
    int kind;
    return xiaotai_contacts_kind_find(wechat ? "wechat" : "device", &kind) == XIAOTAI_OK ? kind : -2;
}

bool companion_recents_any(void) { return s.list && s.list[0].used; }

void companion_recents_dialling(const xiaotai_contact_t *contact)
{
    const xiaotai_contact_kind_def_t *kind = xiaotai_contacts_kind(contact->kind);
    s.dialled_wechat = kind && !strcmp(kind->name, "wechat");
    strcpy(s.dialled, contact->id);
}

/* The contact of an entry as it is now; false: it is not one (any more). */
static bool resolve(const recent_t *entry, xiaotai_contact_t *contact)
{
    const int kind = kind_of(entry->wechat);
    if (kind < 0) return false;
    if (entry->id[0]) return xiaotai_contacts_find(entry->id, kind, XIAOTAI_CONTACTS_STALE_OK, contact) == XIAOTAI_OK;
    return entry->name[0] &&
        xiaotai_contacts_match(entry->name, kind, NULL, XIAOTAI_CONTACTS_STALE_OK, contact, NULL) == XIAOTAI_OK;
}

static void copy(char *out, size_t capacity, const char *text)
{
    size_t n = strnlen(text, capacity);
    if (n == capacity) {
        --n;
        while (n && ((unsigned char)text[n] & 0xc0U) == 0x80U) --n;
    }
    memcpy(out, text, n);
    out[n] = '\0';
}

/* One entry for each person: theirs moves to the top, or the oldest goes. */
static recent_t *take(bool wechat, const char *id, const char *name)
{
    size_t at = 0;
    for (; at < RECENTS && s.list[at].used; ++at) {
        const recent_t *entry = &s.list[at];
        if (entry->wechat != wechat) continue;
        if (id[0] && entry->id[0] ? !strcmp(entry->id, id) : name[0] && !strcmp(entry->name, name)) break;
    }
    recent_t moved = {.wechat = wechat};
    if (at < RECENTS && s.list[at].used) moved = s.list[at];
    else if (at == RECENTS) at = RECENTS - 1U;
    memmove(&s.list[1], &s.list[0], at * sizeof(s.list[0]));
    s.list[0] = moved;
    return &s.list[0];
}

static void began(const companion_call_snapshot_t *call, bool wechat)
{
    char id[sizeof(s.list[0].id)] = "";
    if (wechat && call->outgoing && s.dialled_wechat) copy(id, sizeof(id), s.dialled);
    else if (!wechat && call->peer_id[0]) copy(id, sizeof(id), call->peer_id);
    if (call->outgoing) s.dialled[0] = '\0';
    const char *name = call->peer[0] ? call->peer : wechat ? "微信联系人" : "未知设备";
    recent_t *entry = take(wechat, id, name);
    if (id[0]) copy(entry->id, sizeof(entry->id), id);
    copy(entry->name, sizeof(entry->name), name);
    entry->at_us = xiaotai_time_us();
    entry->outgoing = call->outgoing;
    entry->answered = false;
    entry->used = true;
    entry->serial = ++s.serial;
}

static void ended(const companion_call_snapshot_t *call, const companion_call_snapshot_t *end, bool wechat, bool answered)
{
    recent_t *entry = &s.list[0];
    if (!entry->used || entry->wechat != wechat || entry->outgoing != call->outgoing) return;
    entry->at_us = xiaotai_time_us();
    entry->answered = answered;
    if (answered || call->outgoing || end->local_end) {
        entry->missed = 0;
        return;
    }
    if (entry->missed < 99U) ++entry->missed;
    ++s.unseen;
}

bool companion_recents_home_notice(const char **text)
{
    if (!s.unseen) return false;
    if (s.unseen == 1U) strcpy(s.notice, "有未接来电");
    else (void)snprintf(s.notice, sizeof(s.notice), "%lu 个未接来电", (unsigned long)(s.unseen > 99U ? 99U : s.unseen));
    *text = s.notice;
    return true;
}

static void call_again(void)
{
    xiaotai_contact_t contact;
    if (companion_recents_any() && resolve(&s.list[0], &contact)) companion_contact_dial(&contact, false);
    else companion_refuse_text(XIAOTAI_INTENT_APP_COMPANION_DIAL, XIAOTAI_ERR_NOT_FOUND, "联系人已变化，请在通讯录里重新选");
}

static void show_code(void)
{
    const xiaotai_err_t err = companion_show_wechat_code();
    if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_INTENT_APP_COMPANION_DIAL, err);
}

static void offer(companion_call_follow_t follow)
{
    xiaotai_contact_t contact;
    if (follow == COMPANION_CALL_REDIAL && companion_recents_any() && resolve(&s.list[0], &contact))
        companion_session_result_offer(s.list[0].outgoing ? "再拨一次" : "回拨", XIAOTAI_ICON_PHONE, call_again);
    else if (follow == COMPANION_CALL_AUTHORIZE)
        companion_session_result_offer("小程序码", NULL, show_code);
}

static void age(char *out, size_t capacity, int64_t at_us)
{
    const int64_t minutes = (xiaotai_time_us() - at_us) / 60000000;
    if (minutes < 1) (void)snprintf(out, capacity, "刚刚");
    else if (minutes < 60) (void)snprintf(out, capacity, "%d 分钟前", (int)minutes);
    else if (minutes < 24 * 60) (void)snprintf(out, capacity, "%d 小时前", (int)(minutes / 60));
    else (void)snprintf(out, capacity, "%d 天前", (int)(minutes / 1440 > 999 ? 999 : minutes / 1440));
}

static void build(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    s.unseen = 0;
    view->kind = XIAOTAI_VIEW_LIST;
    view->items = companion.buffer->items;
    for (size_t i = 0; i < RECENTS && s.list[i].used; ++i) {
        const recent_t *entry = &s.list[i];
        char when[24], *detail = companion.buffer->details + i * DETAIL_BYTES;
        age(when, sizeof(when), entry->at_us);
        const char *what = entry->missed ? "未接来电" : entry->outgoing ? (entry->answered ? "呼出" : "呼出未接通") :
                           entry->answered ? "来电" : "来电已拒接";
        if (entry->missed > 1U) (void)snprintf(detail, DETAIL_BYTES, "%s %u 次 · %s", what, entry->missed, when);
        else (void)snprintf(detail, DETAIL_BYTES, "%s · %s", what, when);
        companion.buffer->items[view->item_count++] = (xiaotai_view_item_t){.label = entry->name, .detail = detail,
            .icon = entry->missed ? XIAOTAI_ICON_CALL_MISSED : entry->outgoing ? XIAOTAI_ICON_CALL_OUT : XIAOTAI_ICON_CALL_IN,
            .intent = ROW_INTENT, .value = (int32_t)(entry->serial & INT32_MAX)};
    }
    if (!view->item_count) view->status = "还没有通话";
    companion.buffer->actions[0] = (xiaotai_view_action_t){.label = "通讯录", .icon = XIAOTAI_ICON_CONTACTS,
        .intent = ALL_INTENT, .role = XIAOTAI_ROLE_PRIMARY};
    companion.buffer->actions[1] = (xiaotai_view_action_t){.role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK,
        .intent = XIAOTAI_INTENT_APP_COMPANION_BACK};
    view->actions = companion.buffer->actions;
    view->action_count = 2;
}

static void on_row(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!companion_on_page(PAGE)) { ++companion.status.stale; return; }
    for (size_t i = 0; i < RECENTS && s.list[i].used; ++i) {
        if ((int32_t)(s.list[i].serial & INT32_MAX) != args->value) continue;
        xiaotai_contact_t contact;
        if (resolve(&s.list[i], &contact)) companion_contact_open(&contact);
        else companion_refuse_text(ROW_INTENT, XIAOTAI_ERR_NOT_FOUND, "不在通讯录里，请让对方重新添加");
        return;
    }
    ++companion.status.stale;
}

static void on_all(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    if (!companion_on_page(PAGE)) { ++companion.status.stale; return; }
    const xiaotai_err_t err = companion_show_page(XIAOTAI_APP_COMPANION_PAGE_CONTACTS, 0);
    if (err != XIAOTAI_OK) companion_refuse(ALL_INTENT, err);
}

xiaotai_err_t companion_recents_init(void)
{
    if (!companion.has_display || !(companion.style->needs_all & XIAOTAI_NEED_TOUCH)) return XIAOTAI_OK;
    static const companion_page_t page = {.id = PAGE, .title = "最近通话", .icon = XIAOTAI_ICON_PHONE, .build = build};
    static const companion_call_log_ops_t ops = {began, ended, offer};
    s.list = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, RECENTS, sizeof(s.list[0]));
    if (!s.list) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t err = companion_page_register(&page);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(ROW_INTENT, on_row, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(ALL_INTENT, on_all, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) companion_call_ui_set_log(&ops);
    return err;
}

void companion_recents_reset(void)
{
    companion_call_ui_set_log(NULL);
    xiaotai_mem_free(s.list);
    memset(&s, 0, sizeof(s));
}
