// SPDX-License-Identifier: MIT
#include <string.h>

#include "contacts_internal.h"
#include "xiaotai/at_command.h"

static xiaotai_err_t contacts_list(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    xiaotai_contacts_status_t s;
    xiaotai_contacts_status(&s);
    const xiaotai_contact_kind_def_t *filter = xiaotai_contacts_kind(s.kind);
    const char *label = !filter ? "all" : !strcmp(filter->name, "device") ? "dev" :
                        !strcmp(filter->name, "wechat") ? "wx" : filter->name;
    xiaotai_at_printf(out, "+CONTACTS: cached=%u total=%u filter=%s page=%u loading=%u error=%d",
                      (unsigned)s.cached, (unsigned)s.total, label, (unsigned)s.page, (unsigned)s.loading, s.error);
    for (uint8_t i = 0; i < s.page_count; ++i) {
        xiaotai_contact_t c;
        if (xiaotai_contacts_at(i, XIAOTAI_CONTACTS_STALE_OK, &c) != XIAOTAI_OK) break;
        const xiaotai_contact_kind_def_t *kind = xiaotai_contacts_kind(c.kind);
        if (!kind) continue;
        char hex[2 * XIAOTAI_CONTACT_NAME_MAX + 1U];
        (void)xiaotai_at_text_encode(c.name, hex, sizeof(hex));
        xiaotai_at_printf(out, "+CONTACT: index=%u source=%s online=%s id=%s name_hex=%s", (unsigned)i,
            kind->name, c.online_known ? (c.online ? "1" : "0") : "-",
            kind->friendships && xiaotai_at_is_id(c.id, XIAOTAI_CONTACT_ID_MAX) ? c.id : "-", hex);
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t refresh(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)argv; (void)out;
    return xiaotai_contacts_refresh();
}

static bool all(const char *text)
{
    return strlen(text) == 3 && (text[0] == 'A' || text[0] == 'a') &&
           (text[1] == 'L' || text[1] == 'l') && (text[2] == 'L' || text[2] == 'l');
}

static xiaotai_err_t show_page(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    int kind = XIAOTAI_CONTACT_ANY;
    uint32_t page = 0;
    if (!all(argv[0]) && xiaotai_contacts_kind_find(argv[0], &kind) != XIAOTAI_OK) return XIAOTAI_ERR_INVALID_ARG;
    if (argc == 2 && !xiaotai_at_parse_u32(argv[1], 0, UINT16_MAX, &page)) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t err = xiaotai_contacts_show(kind, (uint16_t)page);
    return err == XIAOTAI_OK ? contacts_list(0, NULL, out) : err;
}

static xiaotai_err_t show_pending(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    xiaotai_contacts_pending_status_t s;
    xiaotai_contacts_pending_status(&s);
    uint32_t page = s.page;
    if (argc && !xiaotai_at_parse_u32(argv[0], 0, UINT16_MAX, &page)) return XIAOTAI_ERR_INVALID_ARG;
    if (!g_contacts.source->friendships || argc || (!s.fresh && !s.loading && s.error == XIAOTAI_OK)) {
        const xiaotai_err_t err = xiaotai_contacts_pending_show((uint16_t)page);
        if (err != XIAOTAI_OK) return err;
        xiaotai_contacts_pending_status(&s);
    }
    xiaotai_at_printf(out, "+PENDING: cached=%u total=%u error=%d revision=%lu page=%u loading=%u",
        (unsigned)s.page_count, (unsigned)s.total, s.code ? (int)s.code : s.error, (unsigned long)s.revision, (unsigned)s.page,
        (unsigned)s.loading);
    for (uint8_t i = 0; i < s.page_count; ++i) {
        /* This handler is the only writer's task, so the page is stable. */
        const char *id = g_contacts.pending_ids[i];
        xiaotai_at_printf(out, "+PENDING-ITEM: index=%u id=%s", (unsigned)i,
            xiaotai_at_is_id(id, XIAOTAI_CONTACT_ID_MAX) ? id : "-");
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t by_id(xiaotai_contacts_change_kind_t kind, const char *id, const char *remark)
{
    if (!xiaotai_at_is_id(id, XIAOTAI_CONTACT_ID_MAX)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_contacts_change(&(xiaotai_contacts_change_t){.kind = kind, .id = id, .remark = remark});
}

static xiaotai_err_t request(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)out;
    return by_id(XIAOTAI_CONTACTS_REQUEST, argv[0], NULL);
}

static xiaotai_err_t accept(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)out;
    return by_id(XIAOTAI_CONTACTS_ACCEPT, argv[0], NULL);
}

static xiaotai_err_t reject(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)out;
    return by_id(XIAOTAI_CONTACTS_REJECT, argv[0], NULL);
}

static xiaotai_err_t remove_contact(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)out;
    return by_id(XIAOTAI_CONTACTS_REMOVE, argv[0], NULL);
}

static xiaotai_err_t remark(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)out;
    char text[XIAOTAI_CONTACT_REMARK_BYTES_MAX + 1U];
    const xiaotai_err_t err = xiaotai_at_text_decode(argv[1], text, sizeof(text));
    return err == XIAOTAI_OK ? by_id(XIAOTAI_CONTACTS_REMARK, argv[0], text) : err;
}

static void report(const xiaotai_reply_t *out)
{
    xiaotai_contacts_status_t status;
    xiaotai_contacts_status(&status);
    xiaotai_contacts_stats_t s;
    xiaotai_contacts_stats(&s);
    xiaotai_at_printf(out, "+CONTACTS: fresh=%u cached=%u total=%u loading=%u error=%d fetches=%lu failures=%lu"
        " deferred=%lu malformed=%lu skipped=%lu unregistered=%lu dropped=%lu signals=%lu stale=%lu cleared=%lu"
        " queries=%lu query_timeouts=%lu changes=%lu change_failures=%lu pending_fetches=%lu pending_failures=%lu",
        (unsigned)status.fresh, (unsigned)status.cached, (unsigned)status.total, (unsigned)status.loading, status.error,
        (unsigned long)s.fetches, (unsigned long)s.fetch_failures, (unsigned long)s.fetches_deferred,
        (unsigned long)s.replies_malformed, (unsigned long)s.entries_skipped, (unsigned long)s.entries_unregistered,
        (unsigned long)s.entries_dropped, (unsigned long)s.signals, (unsigned long)s.stale_replies, (unsigned long)s.cleared,
        (unsigned long)s.queries, (unsigned long)s.query_timeouts, (unsigned long)s.changes, (unsigned long)s.change_failures,
        (unsigned long)s.pending_fetches, (unsigned long)s.pending_failures);
}

xiaotai_err_t contacts_at_init(void)
{
    static const xiaotai_at_verb_t list[] = {
        {.run = contacts_list}, {.word = "REFRESH", .run = refresh},
        {.word = "PAGE", .min_args = 1, .max_args = 2, .usage = "<ALL|kind> [<page>]", .run = show_page},
        {.word = "PENDING", .max_args = 1, .usage = "[<page>]", .run = show_pending},
    };
    static const xiaotai_at_verb_t change[] = {
        {.word = "REQUEST", .min_args = 1, .max_args = 1, .usage = "<id>", .run = request},
        {.word = "ACCEPT", .min_args = 1, .max_args = 1, .usage = "<id>", .run = accept},
        {.word = "REJECT", .min_args = 1, .max_args = 1, .usage = "<id>", .run = reject},
        {.word = "DELETE", .min_args = 1, .max_args = 1, .usage = "<id>", .run = remove_contact},
        {.word = "REMARK", .min_args = 2, .max_args = 2, .usage = "<id> <hex|->", .run = remark},
    };
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+CONTACTS", list, sizeof(list) / sizeof(list[0]));
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+CONTACT", change, sizeof(change) / sizeof(change[0]));
    return err == XIAOTAI_OK ? xiaotai_diag_register("contacts", report) : err;
}
