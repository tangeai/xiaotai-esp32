// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
// Replaces legacy: tests/xmini/xmini_business/test_business.c::contacts_response
// Replaces legacy: tests/mz28/test_voip_join_lifetime.c::contacts_response
/* mod_contacts_parse: the list reply read through both source descriptors.
 *
 * Call service (every kind in one list, "type" selects the kind): the
 * acceptance rule (HTTP 200, exactly one integral code 200, data.contacts an
 * array; anything else keeps the cache byte for byte), the entry rules (only
 * registered types, id 1..64 bytes, a kind's required extra field, extra
 * fields of at most 64 bytes, remark -> name -> "", names cut at a character
 * boundary, online only for kinds with presence), skipped and unregistered
 * counts, the first entry of each kind, and the cache range when the list is
 * longer than the cache. Voip service (no type field, written as its replies
 * are: code 0, wx_open_id / wx_app_id / remark): good entries, an entry
 * without wx_app_id skipped, a reply with code 200 refused.
 *
 * Ported: the list cases of tests/regression/test_contact_query.py and the
 * reply cases of the companion, voice-terminal and C3 business tests'
 * --contacts-response (see the notes of MC-1 for the one rule that changed:
 * one bad entry no longer voids the whole list). */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contacts_internal.h"
#include "xiaotai/port/mem.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

static const xiaotai_contact_kind_def_t DEVICE = {
    .name = "device", .alias = "DEV", .server_type = "device", .presence = true, .friendships = true};
static const xiaotai_contact_kind_def_t WECHAT = {
    .name = "wechat",
    .alias = "WX",
    .server_type = "voip",
    .extra_fields = {"wx_app_id", "wx_model_id"},
    .extra_required = 1U,
};
static const xiaotai_contact_kind_def_t *const KINDS[] = {&DEVICE, &WECHAT};

static const xiaotai_contacts_source_t CALL = {
    .service = "call", .path = "/v1/call/device/contacts", .success_code = 200,
    .id_field = "device_id", .type_field = "type", .friendships = true};
static const xiaotai_contacts_source_t VOIP = {
    .service = "voip", .path = "/v1/voip/device/contacts", .success_code = 0,
    .id_field = "wx_open_id", .default_type = "voip"};

enum { DEV = 0, WX = 1 };

static xiaotai_contact_t s_cache[64];
static xiaotai_contact_t s_firsts[XIAOTAI_CONTACT_KINDS_MAX];
static char s_text[32768];

static contacts_parse_in_t input(const xiaotai_contacts_source_t *source, uint16_t capacity)
{
    contacts_parse_in_t in = {
        .source = source,
        .kinds = KINDS,
        .kind_count = 2,
        .kind = XIAOTAI_CONTACT_ANY,
        .page = 0,
        .page_size = 4,
        .capacity = capacity,
        .cache = s_cache,
        .firsts = s_firsts,
        .token_capacity = 2048,
    };
    return in;
}

static const char *reply(const char *code, const char *entries)
{
    snprintf(s_text, sizeof(s_text), "{\"code\":%s,\"data\":{\"contacts\":[%s]}}", code, entries);
    return s_text;
}

static xiaotai_err_t parse(const contacts_parse_in_t *in, int http, const char *body, contacts_parse_out_t *out)
{
    return contacts_parse_list(in, http, body, body ? strlen(body) : 0, out);
}

static const char *repeat(char c, size_t n)
{
    static char text[512];
    CHECK(n < sizeof(text));
    memset(text, c, n);
    text[n] = '\0';
    return text;
}

/* ------------------------------------------------------------ call source */

static void test_mixed_list(void)
{
    char long_id[70];
    snprintf(long_id, sizeof(long_id), "%s", repeat('x', 65));
    char entries[2048];
    snprintf(entries, sizeof(entries),
             "{\"type\":\"device\",\"device_id\":\"d_A\",\"remark\":\"小王\",\"online\":true},"
             "{\"type\":\"device\",\"device_id\":\"d_B\",\"remark\":\"Alice\",\"online\":false},"
             "{\"type\":\"voip\",\"device_id\":\"wx_C\",\"remark\":\"妈妈\",\"wx_app_id\":\"wxapp\","
             "\"wx_model_id\":\"m1\",\"online\":true},"
             "{\"type\":\"future\",\"device_id\":\"no_route\"},"
             "{\"type\":\"device\",\"device_id\":\"%s\"},"
             "{\"device_id\":\"no_type\"},"
             "{\"type\":\"voip\",\"device_id\":\"missing-app\"},"
             "{\"type\":\"device\",\"device_id\":\"d_D\",\"name\":\"Server Name\"},"
             "{\"type\":\"device\",\"device_id\":\"d_E\",\"remark\":\"\",\"name\":\"N2\",\"online\":\"yes\"},"
             "{\"type\":\"device\",\"device_id\":\"d_F\",\"remark\":\"\",\"device_name\":\"客厅\"},"
             "5,"
             "{\"type\":\"device\",\"device_id\":7},"
             "{\"type\":\"voip\",\"device_id\":\"wx_G\",\"wx_app_id\":\"a\"}",
             long_id);
    contacts_parse_in_t in = input(&CALL, 64);
    contacts_parse_out_t out;
    xiaotai_mem_host_clear();
    CHECK(parse(&in, 200, reply("200", entries), &out) == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_allocations() == 0);
    CHECK(out.total == 7 && out.totals[DEV] == 5 && out.totals[WX] == 2);
    CHECK(out.skipped == 6 && out.unregistered == 1);
    CHECK(out.cache_kind == XIAOTAI_CONTACT_ANY && out.cache_offset == 0 && out.cached == 7 && out.dropped == 0);
    const char *ids[] = {"d_A", "d_B", "wx_C", "d_D", "d_E", "d_F", "wx_G"};
    for (int i = 0; i < 7; ++i) CHECK(strcmp(s_cache[i].id, ids[i]) == 0);
    CHECK(s_cache[0].kind == DEV && strcmp(s_cache[0].name, "小王") == 0);
    CHECK(s_cache[0].online_known && s_cache[0].online);
    CHECK(s_cache[1].online_known && !s_cache[1].online);
    /* A kind without presence never reports one, whatever the entry says. */
    CHECK(s_cache[2].kind == WX && !s_cache[2].online_known && !s_cache[2].online);
    CHECK(strcmp(s_cache[2].extra[0], "wxapp") == 0 && strcmp(s_cache[2].extra[1], "m1") == 0);
    CHECK(strcmp(s_cache[3].name, "Server Name") == 0 && !s_cache[3].online_known);
    CHECK(strcmp(s_cache[4].name, "N2") == 0 && !s_cache[4].online_known);
    CHECK(strcmp(s_cache[5].name, "客厅") == 0);
    CHECK(s_cache[0].extra[0][0] == '\0' && s_cache[0].extra[1][0] == '\0');
    CHECK(strcmp(s_cache[6].extra[0], "a") == 0 && s_cache[6].extra[1][0] == '\0');
    CHECK(out.first[DEV] && strcmp(s_firsts[DEV].id, "d_A") == 0);
    CHECK(out.first[WX] && strcmp(s_firsts[WX].id, "wx_C") == 0 && strcmp(s_firsts[WX].extra[0], "wxapp") == 0);
}

/* Replies that are not a contact list leave the cache as it was. */
static void test_refused_replies(void)
{
    static const char good[] = "{\"code\":200,\"data\":{\"contacts\":[{\"type\":\"device\",\"device_id\":\"x\"}]}}";
    static const char empty[] = "{\"code\":200,\"data\":{\"contacts\":[]}}";
    const struct {
        int http;
        const char *body;
    } refused[] = {
        {503, good},
        {201, good},
        {401, empty},
        {0, NULL},
        {200, NULL},
        {200, ""},
        {200, "{\"code\":0,\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":200.5,\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":2e2,\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":\"200\",\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":200,\"code\":0,\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":200,\"code\":200,\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":200,\"c\\u006fde\":200,\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":200,\"data\":{\"contacts\":[]}} stray"},
        {200, "{\"code\":200,\"data\":{\"contacts\":{}}}"},
        {200, "{\"code\":200,\"data\":{\"contacts\":{},\"contacts\":[]}}"},
        {200, "{\"code\":200,\"data\":{\"contacts\":null}}"},
        {200, "{\"code\":200,\"data\":null}"},
        {200, "{\"code\":200}"},
        {200, "[{\"code\":200}]"},
        {200, "{\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":200,\"data\":{\"contacts\":[}}"},
    };
    contacts_parse_in_t in = input(&CALL, 8);
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        memset(s_cache, 0xA5, sizeof(s_cache));
        memset(s_firsts, 0x5A, sizeof(s_firsts));
        unsigned char cache_before[sizeof(s_cache)], firsts_before[sizeof(s_firsts)];
        memcpy(cache_before, s_cache, sizeof(s_cache));
        memcpy(firsts_before, s_firsts, sizeof(s_firsts));
        contacts_parse_out_t out;
        xiaotai_err_t err = parse(&in, refused[i].http, refused[i].body, &out);
        if (err != XIAOTAI_FAIL) fprintf(stderr, "case %zu accepted\n", i);
        CHECK(err == XIAOTAI_FAIL);
        CHECK(memcmp(cache_before, s_cache, sizeof(s_cache)) == 0);
        CHECK(memcmp(firsts_before, s_firsts, sizeof(s_firsts)) == 0);
        CHECK(out.total == 0 && out.cached == 0);
    }
    /* The authoritative empty list is a list. */
    contacts_parse_out_t out;
    CHECK(parse(&in, 200, empty, &out) == XIAOTAI_OK);
    CHECK(out.total == 0 && out.cached == 0 && out.skipped == 0 && !out.first[DEV] && !out.first[WX]);
    /* Members around the list do not matter; a nested "code" is not the code. */
    CHECK(parse(&in, 200,
                "{\"msg\":\"ok\",\"data\":{\"code\":0,\"contacts\":[{\"type\":\"device\",\"device_id\":\"x\"}],"
                "\"more\":[1,2]},\"code\":200}",
                &out) == XIAOTAI_OK);
    CHECK(out.total == 1 && strcmp(s_cache[0].id, "x") == 0);
}

static void test_token_limit(void)
{
    contacts_parse_in_t in = input(&CALL, 8);
    const char *body = reply("200", "{\"type\":\"device\",\"device_id\":\"a\"},{\"type\":\"device\",\"device_id\":\"b\"}");
    /* 1 + 2*(1+1) envelope tokens + array, 2 objects of 2 members: 17 tokens. */
    in.token_capacity = 16;
    contacts_parse_out_t out;
    CHECK(parse(&in, 200, body, &out) == XIAOTAI_ERR_INVALID_SIZE);
    in.token_capacity = 17;
    CHECK(parse(&in, 200, body, &out) == XIAOTAI_OK && out.total == 2);
    in.token_capacity = 16;
    /* Malformed trailing data wins over a token-count overflow. */
    strcat(s_text, " stray");
    CHECK(parse(&in, 200, body, &out) == XIAOTAI_FAIL);
}

static void test_borrowed_reply(void)
{
    static const char text[] =
        " \t{\"c\\u006fde\":200,\"data\":{\"contacts\":["
        "{\"type\":\"device\",\"device\\u005fid\":\"first\",\"device_id\":\"second\","
        "\"remark\":\"\\uD83D\\uDE00\",\"remark\":false,\"online\":null,\"online\":true},"
        "{\"type\":\"device\",\"device_id\":false,\"device_id\":\"ignored\"}]}} \r\n";
    char bounded[sizeof(text) - 1U];
    memcpy(bounded, text, sizeof(bounded));
    contacts_parse_in_t in = input(&CALL, 8);
    contacts_reply_t parsed;
    CHECK(contacts_parse_reply(&in, 200, bounded, sizeof(bounded), &parsed) == XIAOTAI_OK);
    CHECK(parsed.body_size == sizeof(bounded));
    contacts_parse_out_t out;
    contacts_parse_fill(&in, &parsed, &out);
    CHECK(out.total == 1 && out.cached == 1 && out.skipped == 1);
    CHECK(!strcmp(s_cache[0].id, "first") && !s_cache[0].online_known);
    char bytes[5];
    size_t size;
    CHECK(contacts_query_remark(&in, &parsed, DEV, 0, bytes, sizeof(bytes), &size) == XIAOTAI_OK);
    CHECK(size == 4 && !memcmp(bytes, "\xf0\x9f\x98\x80", 5));
    CHECK(contacts_query_remark(&in, &parsed, DEV, 1, bytes, sizeof(bytes), &size) == XIAOTAI_ERR_NOT_FOUND);
}

static void test_lengths(void)
{
    contacts_parse_in_t in = input(&CALL, 64);
    char entries[4096];
    char name300[310];
    snprintf(name300, sizeof(name300), "%s", repeat('r', 300));
    char id64[70], id65[70], app64[70], app65[70];
    snprintf(id64, sizeof(id64), "%s", repeat('i', 64));
    snprintf(id65, sizeof(id65), "%s", repeat('j', 65));
    snprintf(app64, sizeof(app64), "%s", repeat('a', 64));
    snprintf(app65, sizeof(app65), "%s", repeat('b', 65));
    char han[200] = "";
    for (int i = 0; i < 30; ++i) strcat(han, "汉");
    char ascii65[70];
    snprintf(ascii65, sizeof(ascii65), "%s", repeat('n', 65));
    snprintf(entries, sizeof(entries),
             "{\"type\":\"device\",\"device_id\":\"%s\",\"remark\":\"%s\"},"          /* id 64, 90-byte name */
             "{\"type\":\"device\",\"device_id\":\"%s\"},"                            /* id 65: skipped */
             "{\"type\":\"voip\",\"device_id\":\"w1\",\"wx_app_id\":\"%s\"},"         /* extra 64 */
             "{\"type\":\"voip\",\"device_id\":\"w2\",\"wx_app_id\":\"%s\"},"         /* required extra 65 */
             "{\"type\":\"voip\",\"device_id\":\"w3\",\"wx_app_id\":\"a\",\"wx_model_id\":\"%s\"}," /* optional 65 */
             "{\"type\":\"voip\",\"device_id\":\"w4\",\"wx_app_id\":\"\"},"           /* required empty */
             "{\"type\":\"device\",\"device_id\":\"d1\",\"remark\":\"%s\"},"          /* 65 ASCII */
             "{\"type\":\"device\",\"device_id\":\"d2\",\"remark\":\"%s\",\"name\":\"fallback\"}," /* 300 bytes */
             "{\"type\":\"device\",\"device_id\":\"d3\",\"remark\":\"\\u6c49\\u5b57 A\"},"
             "{\"type\":\"device\",\"device_id\":\"\"},"
             "{\"type\":\"device\",\"device_id\":\"d4\",\"remark\":\"\\ud800\",\"name\":\"bad surrogate\"}",
             id64, han, id65, app64, app65, app65, ascii65, name300);
    contacts_parse_out_t out;
    CHECK(parse(&in, 200, reply("200", entries), &out) == XIAOTAI_OK);
    CHECK(out.total == 6 && out.skipped == 5 && out.unregistered == 0);
    CHECK(strcmp(s_cache[0].id, id64) == 0);
    /* 21 whole characters of 3 bytes: 63 bytes, the 22nd would not fit 64. */
    CHECK(strlen(s_cache[0].name) == 63 && strncmp(s_cache[0].name, han, 63) == 0);
    CHECK(strcmp(s_cache[1].id, "w1") == 0 && strcmp(s_cache[1].extra[0], app64) == 0);
    CHECK(strcmp(s_cache[2].id, "d1") == 0 && strlen(s_cache[2].name) == 64);
    CHECK(strcmp(s_cache[3].id, "d2") == 0 && strcmp(s_cache[3].name, "fallback") == 0);
    CHECK(strcmp(s_cache[4].id, "d3") == 0 && strcmp(s_cache[4].name, "汉字 A") == 0);
    CHECK(strcmp(s_cache[5].id, "d4") == 0 && strcmp(s_cache[5].name, "bad surrogate") == 0);
}

static void test_copy_utf8(void)
{
    char out[8];
    contacts_copy_utf8(out, sizeof(out), "abc");
    CHECK(strcmp(out, "abc") == 0);
    contacts_copy_utf8(out, sizeof(out), "ab\xC3");
    CHECK(strcmp(out, "ab") == 0);
    contacts_copy_utf8(out, sizeof(out), "\xFFx");
    CHECK(out[0] == '\0');
    contacts_copy_utf8(out, 7, "a汉汉"); /* 1 + 3 bytes fit 6, the next 3 do not */
    CHECK(strcmp(out, "a汉") == 0);
    contacts_copy_utf8(out, 8, "a汉汉"); /* 7 bytes and the terminator */
    CHECK(strcmp(out, "a汉汉") == 0);
    contacts_copy_utf8(out, 4, "\xF0\x9F\x98\x80");
    CHECK(out[0] == '\0');
    contacts_copy_utf8(out, 5, "\xF0\x9F\x98\x80z");
    CHECK(strcmp(out, "\xF0\x9F\x98\x80") == 0);
}

static void ids_are(const char *prefix, unsigned first, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        char id[16];
        snprintf(id, sizeof(id), "%s%02u", prefix, first + i);
        if (strcmp(s_cache[i].id, id) != 0) fprintf(stderr, "slot %u: %s, expected %s\n", i, s_cache[i].id, id);
        CHECK(strcmp(s_cache[i].id, id) == 0);
    }
}

/* 20 devices, a cache of 8 and pages of 4: the cache keeps the selected page
 * with what surrounds it, and the last page when the page is beyond the end. */
static void test_range(void)
{
    char entries[4096] = "";
    for (unsigned i = 0; i < 20; ++i) {
        char entry[96];
        snprintf(entry, sizeof(entry), "%s{\"type\":\"device\",\"device_id\":\"d%02u\"}", i ? "," : "", i);
        strcat(entries, entry);
    }
    char body[4096];
    snprintf(body, sizeof(body), "%s", reply("200", entries));
    contacts_parse_in_t in = input(&CALL, 8);
    contacts_parse_out_t out;
    const struct {
        uint16_t page, offset;
    } cases[] = {{0, 0}, {1, 2}, {2, 6}, {3, 10}, {4, 12}, {9, 12}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        in.page = cases[i].page;
        CHECK(parse(&in, 200, body, &out) == XIAOTAI_OK);
        CHECK(out.total == 20 && out.cached == 8 && out.dropped == 12);
        CHECK(out.cache_kind == XIAOTAI_CONTACT_ANY && out.cache_offset == cases[i].offset);
        ids_are("d", cases[i].offset, 8);
        CHECK(out.first[DEV] && strcmp(s_firsts[DEV].id, "d00") == 0);
    }

    /* Mixed: every third entry is a voip contact (6 of them, 14 devices). */
    entries[0] = '\0';
    for (unsigned i = 0; i < 20; ++i) {
        char entry[128];
        if (i % 3 == 2) {
            snprintf(entry, sizeof(entry), "%s{\"type\":\"voip\",\"device_id\":\"w%02u\",\"wx_app_id\":\"a\"}",
                     i ? "," : "", i);
        } else {
            snprintf(entry, sizeof(entry), "%s{\"type\":\"device\",\"device_id\":\"d%02u\"}", i ? "," : "", i);
        }
        strcat(entries, entry);
    }
    snprintf(body, sizeof(body), "%s", reply("200", entries));
    in.kind = WX;
    in.page = 0;
    CHECK(parse(&in, 200, body, &out) == XIAOTAI_OK);
    CHECK(out.totals[DEV] == 14 && out.totals[WX] == 6);
    CHECK(out.cache_kind == WX && out.cache_offset == 0 && out.cached == 6);
    for (unsigned i = 0; i < 6; ++i) CHECK(s_cache[i].kind == WX && s_cache[i].id[0] == 'w');
    CHECK(strcmp(s_cache[0].id, "w02") == 0 && strcmp(s_cache[5].id, "w17") == 0);
    /* Devices, page 3 of pages of 4: positions 12 and 13 of 14. The window
     * ends at the last device; the first device stays known outside it. */
    in.kind = DEV;
    in.page = 3;
    CHECK(parse(&in, 200, body, &out) == XIAOTAI_OK);
    CHECK(out.cache_kind == DEV && out.cache_offset == 6 && out.cached == 8);
    CHECK(strcmp(s_cache[0].id, "d09") == 0 && strcmp(s_cache[7].id, "d19") == 0);
    CHECK(out.first[DEV] && strcmp(s_firsts[DEV].id, "d00") == 0);
    CHECK(out.first[WX] && strcmp(s_firsts[WX].id, "w02") == 0);
    /* When the whole list fits, it is kept whole whatever the selection. */
    in.capacity = 20;
    CHECK(parse(&in, 200, body, &out) == XIAOTAI_OK);
    CHECK(out.cache_kind == XIAOTAI_CONTACT_ANY && out.cached == 20 && out.dropped == 0);
}

/* ------------------------------------------------------------ voip source */

static void test_voip_source(void)
{
    contacts_parse_in_t in = input(&VOIP, 8);
    contacts_parse_out_t out;
    const char *body = reply("0",
                             "{\"wx_open_id\":\"person\",\"wx_app_id\":\"app\",\"remark\":\"Mum\"},"
                             "{\"wx_open_id\":\"nobody\"},"
                             "{\"wx_open_id\":\"\",\"wx_app_id\":\"app\"},"
                             "{\"wx_open_id\":\"second\",\"wx_app_id\":\"app2\"}");
    CHECK(parse(&in, 200, body, &out) == XIAOTAI_OK);
    CHECK(out.total == 2 && out.totals[WX] == 2 && out.totals[DEV] == 0);
    CHECK(out.skipped == 2 && out.unregistered == 0);
    CHECK(s_cache[0].kind == WX && strcmp(s_cache[0].id, "person") == 0);
    CHECK(strcmp(s_cache[0].extra[0], "app") == 0 && s_cache[0].extra[1][0] == '\0');
    CHECK(strcmp(s_cache[0].name, "Mum") == 0 && !s_cache[0].online_known);
    CHECK(strcmp(s_cache[1].id, "second") == 0 && s_cache[1].name[0] == '\0');
    CHECK(!out.first[DEV] && out.first[WX] && strcmp(s_firsts[WX].id, "person") == 0);

    static const char empty[] = "{\"code\":0,\"data\":{\"contacts\":[]}}";
    const struct {
        int http;
        const char *body;
    } refused[] = {
        {201, empty},
        {503, empty},
        {0, NULL},
        {200, "{\"code\":200,\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":0.5,\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":0,\"code\":200,\"data\":{\"contacts\":[]}}"},
        {200, "{\"code\":0,\"data\":{\"contacts\":[]}} trailing"},
        {200, "{\"code\":0,\"data\":{\"contacts\":{}}}"},
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        CHECK(parse(&in, refused[i].http, refused[i].body, &out) == XIAOTAI_FAIL);
    }
    CHECK(parse(&in, 200, empty, &out) == XIAOTAI_OK && out.total == 0);

    /* Without a kind for the default type every entry is unregistered. */
    in.kind_count = 1;
    CHECK(parse(&in, 200, body, &out) == XIAOTAI_OK);
    CHECK(out.total == 0 && out.skipped == 4 && out.unregistered == 4);
}

/* The business test's reply set of the C3 companion: a list with a voip entry
 * lacking wx_app_id. Today the bad entry is skipped and the rest is kept. */
static void test_business_reply_set(void)
{
    contacts_parse_in_t in = input(&CALL, 8);
    contacts_parse_out_t out;
    CHECK(parse(&in, 200,
                "{\"code\":200,\"data\":{\"contacts\":[{\"type\":\"device\",\"device_id\":\"device-b\"},"
                "{\"type\":\"voip\",\"device_id\":\"openid\",\"wx_app_id\":\"wxapp\"}]}}",
                &out) == XIAOTAI_OK);
    CHECK(out.total == 2 && strcmp(s_cache[0].id, "device-b") == 0 && strcmp(s_cache[1].id, "openid") == 0);
    CHECK(parse(&in, 200,
                "{\"code\":200,\"data\":{\"contacts\":[{\"type\":\"device\",\"device_id\":\"other\"},"
                "{\"type\":\"voip\",\"device_id\":\"missing-app\"}]}}",
                &out) == XIAOTAI_OK);
    CHECK(out.total == 1 && out.skipped == 1 && strcmp(s_cache[0].id, "other") == 0);
}

int main(void)
{
    test_mixed_list();
    test_refused_replies();
    test_token_limit();
    test_borrowed_reply();
    test_lengths();
    test_copy_utf8();
    test_range();
    test_voip_source();
    test_business_reply_set();
    puts("mod_contacts_parse: PASS");
    return 0;
}
