// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_contact_query.py
#include "ai_contact_tools.h"
#include "companion_contact_route.h"
#include "companion_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/json.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof(*(a)))

companion_t companion;
static xiaotai_ai_talk_status_t talk;
static bool fail_alloc;
static xiaotai_err_t stop_error, dial_error, query_error, reply_error;
static unsigned query_submissions, cancellations, matches_read;
static uint16_t skipped;
static char trace[20], dialled[65], hint[128];
static unsigned refused;
static int kind_read;
static char id_read[65], name_read[65];
static xiaotai_signal_t done;
static struct {
    int32_t code;
    uint32_t request;
    size_t limit;
    char message[160], object[XIAOTAI_AI_TALK_TOOL_REPLY_MAX];
} reply;
static struct {
    const char *name;
    xiaotai_ai_talk_tool_fn_t handler;
    void *ctx;
} tools[XIAOTAI_AI_TALK_TOOLS_MAX], fallback;
static size_t tool_count;

static const xiaotai_contact_kind_def_t kinds[] = {
    {.name = "device", .server_type = "device", .presence = true},
    {.name = "wechat", .server_type = "voip"},
};

static xiaotai_contact_t available[] = {
    {.kind = 0, .id = "machine-a", .name = "Alice", .online_known = true, .online = true},
    {.kind = 0, .id = "machine-b", .name = "Bob", .online_known = true},
    {.kind = 1, .id = "wechat-c", .name = "Alice"},
};

static struct {
    xiaotai_contacts_answer_fn_t answer;
    void *ctx;
    uint32_t expires;
} query;
static uint32_t now;
static const char *const *answer_remarks;
static size_t response_size;

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    if (fail_alloc && kind == XIAOTAI_MEM_BULK && xiaotai_on_owner()) {
        fail_alloc = false;
        return NULL;
    }
    return __real_xiaotai_mem_alloc(kind, size);
}

static void note(char step)
{
    assert(xiaotai_on_owner());
    const size_t n = strlen(trace);
    assert(n + 1U < sizeof(trace));
    trace[n] = step;
    trace[n + 1U] = '\0';
}

static xiaotai_err_t dial(const char *kind, const char *id, bool video)
{
    assert(!video && !talk.session);
    assert(!strcmp(kind, strcmp(id, "wechat-c") ? "device" : "wechat"));
    strcpy(dialled, id);
    note('D');
    return dial_error;
}

static const xiaotai_app_companion_part_t call_part = {.name = "fixture", .dial_contact = dial};
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {&call_part, NULL};

void companion_refuse(const char *target, xiaotai_err_t error)
{
    assert(!strcmp(target, XIAOTAI_INTENT_APP_COMPANION_DIAL) && error != XIAOTAI_OK);
    ++refused;
    note('F');
}

void companion_ai_talk_hint(uint32_t session, const char *text)
{
    assert(talk.session == session);
    strcpy(hint, text);
    note('H');
}

void xiaotai_ai_talk_status(xiaotai_ai_talk_status_t *out) { *out = talk; }

xiaotai_err_t xiaotai_ai_talk_stop(uint32_t session)
{
    assert(talk.session == session);
    note('S');
    if (stop_error == XIAOTAI_OK) talk.session = 0;
    return stop_error;
}

xiaotai_err_t xiaotai_ai_talk_tool_register(const char *name, xiaotai_ai_talk_tool_fn_t handler, void *ctx)
{
    assert(tool_count < ARRAY_LEN(tools));
    for (size_t i = 0; i < tool_count; ++i) assert(strcmp(tools[i].name, name));
    tools[tool_count].name = name;
    tools[tool_count].handler = handler;
    tools[tool_count++].ctx = ctx;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_tool_fallback_register(xiaotai_ai_talk_tool_fn_t handler, void *ctx)
{
    assert(!fallback.handler);
    fallback.handler = handler;
    fallback.ctx = ctx;
    return XIAOTAI_OK;
}

static xiaotai_err_t capture(uint32_t session, uint32_t request, int32_t code, const char *message,
                            const char *object, size_t limit)
{
    assert(session == talk.session && request && strlen(object) < sizeof(reply.object));
    note('R');
    reply.code = code;
    reply.request = request;
    reply.limit = limit;
    snprintf(reply.message, sizeof(reply.message), "%s", message ? message : "");
    strcpy(reply.object, object);
    const xiaotai_err_t result = reply_error;
    reply_error = XIAOTAI_OK;
    return result;
}

xiaotai_err_t xiaotai_ai_talk_tool_reply(uint32_t session, uint32_t request, const char *object)
{
    return capture(session, request, 0, NULL, object, 0);
}

xiaotai_err_t xiaotai_ai_talk_tool_fail(uint32_t session, uint32_t request, int32_t code, const char *message,
                                       const char *object)
{
    return capture(session, request, code, message, object, 0);
}

xiaotai_err_t xiaotai_ai_talk_tool_respond(uint32_t session, uint32_t request,
                                         const xiaotai_ai_talk_tool_response_t *response)
{
    return capture(session, request, response->code, response->message, response->object, response->max_bytes);
}

const xiaotai_contact_kind_def_t *xiaotai_contacts_kind(int kind)
{
    return kind == 0 || kind == 1 ? &kinds[kind] : NULL;
}

xiaotai_err_t xiaotai_contacts_kind_find(const char *name, int *kind)
{
    for (unsigned i = 0; i < ARRAY_LEN(kinds); ++i) {
        if (!strcmp(name, kinds[i].name)) { *kind = (int)i; return XIAOTAI_OK; }
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_contacts_match(const char *name, int kind, const char *id, uint32_t flags,
                                     xiaotai_contact_t *contact, size_t *matches)
{
    assert(flags == XIAOTAI_CONTACTS_STALE_OK && name[0]);
    ++matches_read;
    strcpy(id_read, id);
    strcpy(name_read, name);
    kind_read = kind;
    *matches = 0;
    const xiaotai_contact_t *match = NULL;
    for (size_t i = 0; i < ARRAY_LEN(available); ++i) {
        if ((kind == XIAOTAI_CONTACT_ANY || kind == available[i].kind) && !strcmp(name, available[i].name)) {
            match = &available[i];
            ++*matches;
        }
    }
    if (*matches > 1) return XIAOTAI_ERR_INVALID_STATE;
    if (!match || (id[0] && strcmp(id, match->id))) return XIAOTAI_ERR_NOT_FOUND;
    *contact = *match;
    return XIAOTAI_OK;
}

void xiaotai_contacts_status(xiaotai_contacts_status_t *status) { *status = (xiaotai_contacts_status_t){.skipped = skipped}; }

xiaotai_err_t xiaotai_contacts_query(int kind, uint32_t timeout_ms, xiaotai_contacts_answer_fn_t answer, void *ctx)
{
    assert(kind == XIAOTAI_CONTACT_ANY && timeout_ms == 10000 && !query.answer);
    ++query_submissions;
    if (query_error != XIAOTAI_OK) return query_error;
    query.answer = answer;
    query.ctx = ctx;
    query.expires = now + timeout_ms;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_query_cancel(xiaotai_contacts_answer_fn_t answer, void *ctx)
{
    assert(query.answer && query.answer == answer && query.ctx == ctx);
    ++cancellations;
    query.answer = NULL;
    answer(&(const xiaotai_contacts_answer_t){.result = XIAOTAI_ERR_INVALID_STATE}, ctx);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_answer_remark(const xiaotai_contacts_answer_t *answer, size_t index,
                                             char *out, size_t capacity, size_t *length)
{
    assert(index < answer->count);
    const char *remark = answer_remarks ? answer_remarks[index] : answer->contacts[index].name;
    if (!remark || !*remark) return XIAOTAI_ERR_NOT_FOUND;
    const size_t bytes = strlen(remark);
    if (length) *length = bytes;
    if (bytes >= capacity) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(out, remark, bytes + 1U);
    return XIAOTAI_OK;
}

static void deliver_answer(const xiaotai_contacts_answer_t *answer)
{
    assert(query.answer);
    const xiaotai_contacts_answer_fn_t callback = query.answer;
    query.answer = NULL;
    callback(answer, query.ctx);
}

static void answer(const xiaotai_contact_t *people, size_t count, xiaotai_err_t result, bool truncated)
{
    deliver_answer(&(const xiaotai_contacts_answer_t){.result = result, .truncated = truncated,
        .contacts = people, .count = count, .response_bytes = response_size});
}

static void advance(uint32_t ms)
{
    now += ms;
    if (query.answer && now >= query.expires) {
        deliver_answer(&(const xiaotai_contacts_answer_t){.result = XIAOTAI_ERR_TIMEOUT, .deadline_expired = true});
    }
}

static void set_policy(xiaotai_companion_ai_contact_tools_t policy)
{
    assert(!query.answer);
    tool_count = 0;
    memset(&fallback, 0, sizeof(fallback));
    companion.config.ai_contact_tools = policy;
    companion_ai_contact_tools_reset();
    assert(companion_ai_contact_tools_init() == XIAOTAI_OK);
}

static void start_turn(uint32_t session, uint32_t link)
{
    talk = (xiaotai_ai_talk_status_t){.session = session, .link = link, .phase = XIAOTAI_AI_TALK_LISTENING};
    companion_ai_contact_tools_observe(session, link);
}

static void invoke(const char *name, bool device_action, uint32_t request, size_t id_size, const char *json)
{
    memset(&reply, 0, sizeof(reply));
    trace[0] = hint[0] = dialled[0] = '\0';
    const xiaotai_ai_talk_tool_call_t call = {.session = talk.session, .request = request, .name = name,
        .device_action = device_action, .id_text_bytes = id_size, .has_arguments = json != NULL,
        .arguments = json ? json : "{}", .size = json ? strlen(json) : 2U};
    for (size_t i = 0; i < tool_count; ++i) {
        if (strcmp(tools[i].name, name)) continue;
        tools[i].handler(&call, tools[i].ctx);
        return;
    }
    if (device_action && request && fallback.handler) fallback.handler(&call, fallback.ctx);
}

static void call_result(const char *expected_status, bool ok)
{
    assert(reply.request == 1 && reply.code == 0 && reply.message[0] == '\0');
    const char *message = ok ? "正在发起语音呼叫。" : hint;
    char expected[200];
    (void)snprintf(expected, sizeof(expected), "{\"ok\":%s,\"status\":\"%s\",\"message\":\"%s\"}", ok ? "true" : "false",
                   expected_status, message);
    assert(!strcmp(reply.object, expected));
    assert(!strcmp(trace, ok ? "RSD" : "RH"));
}

static void test_calls(void)
{
    set_policy(XIAOTAI_COMPANION_AI_CONTACT_TOOLS_DISABLED);
    assert(!tool_count && !fallback.handler);
    set_policy(XIAOTAI_COMPANION_AI_CONTACT_TOOLS_LEGACY);
    assert(tool_count == 11 && fallback.handler);
    static const char *const actions[] = {"call", "call_contact", "call_device", "device_call", "start_device_call",
        "call_wechat", "wechat_call", "start_voip_call"};
    static const char *const names[] = {"target_name", "target", "contact_name", "contact", "device_name",
        "device_alias", "nickname", "alias", "remark", "callee", "name"};
    static const char *const ids[] = {"target_id", "target_device_id", "device_id", "callee_device_id", "peer_id"};
    static const char *const channels[] = {"channel", "source", "type"};
    static const char *const device_channels[] = {"device", "call", "tirtc"};
    static const char *const wechat_channels[] = {"wx", "wechat", "voip"};
    static const char *const media[] = {"media", "call_type", "room_type"};
    char json[256];
    for (size_t i = 0; i < ARRAY_LEN(names); ++i) {
        start_turn(7, 99);
        const bool wechat = i % 2U;
        const char *id = wechat ? "wechat-c" : "machine-a";
        (void)snprintf(json, sizeof(json), "{\"%s\":\"Alice\",\"%s\":\"%s\",\"%s\":\"%s\",\"%s\":\"%s\"}",
            names[i], ids[i % ARRAY_LEN(ids)], id, channels[i % ARRAY_LEN(channels)],
            (wechat ? wechat_channels : device_channels)[i % 3U], media[i % ARRAY_LEN(media)], wechat ? "voice" : "audio");
        invoke(actions[i % ARRAY_LEN(actions)], true, 1, 0, json);
        call_result("accepted", true);
        assert(!strcmp(dialled, id) && !strcmp(id_read, id) && !strcmp(name_read, "Alice") && kind_read == wechat);
    }
    start_turn(7, 99);
    const unsigned before = matches_read;
    invoke("call", true, 1, 0, "{\"target_device_id\":\"machine-a\"}");
    call_result("not_found", false);
    assert(matches_read == before && !strcmp(hint, "通讯录里没有找到这个联系人。"));
    invoke("call", true, 1, 0, "{\"name\":\"Alice\",\"target_id\":\"machine-a\"}");
    call_result("ambiguous", false);
    assert(!strcmp(hint, "找到多个同名联系人，请说得更具体。"));
    invoke("call", true, 1, 0, "{\"target_name\":\"Bob\",\"target_id\":\"machine-a\",\"channel\":\"device\"}");
    call_result("not_found", false);
    invoke("call", true, 1, 0, "{\"target_name\":\"Alice\",\"name\":\"Bob\",\"channel\":\"device\",\"target_id\":\"machine-b\"}");
    call_result("not_found", false);
    invoke("call", true, 1, 0, "{\"target_name\":\"Bob\",\"channel\":\"wrong\"}");
    call_result("not_found", false);
    invoke("call", true, 1, 0, "{\"name\":\"Bob\",\"media\":\"video\"}");
    call_result("not_found", false);
    assert(!strcmp(hint, "小钛目前只支持语音呼叫。"));
    for (size_t i = 0; i < 2; ++i) {
        start_turn(7, 99);
        invoke(i ? "ai_call_intent" : "call_intent", false, 0, 0, "{\"target_name\":\"Bob\"}");
        assert(!reply.request && !strcmp(trace, "SD"));
    }
    start_turn(7, 99);
    invoke("device_call", false, 0, 0, "{\"target_name\":\"Bob\"}");
    assert(!trace[0]);
    invoke("call_intent", true, 1, 0, "{\"target_name\":\"Bob\"}");
    assert(!strcmp(trace, "R") && reply.code == -32601 && strstr(reply.object, "\"status\":\"unsupported_action\""));
    invoke("arbitrary", true, 1, 0, "{\"target_name\":\"Bob\"}");
    assert(!strcmp(trace, "R") && reply.code == -32601 && strstr(reply.object, "\"status\":\"unsupported_action\""));
    invoke("", true, 1, 0, "{}");
    assert(!strcmp(trace, "R") && reply.code == -32602 && strstr(reply.object, "\"status\":\"invalid_params\""));
    invoke("arbitrary", false, 0, 0, "{}");
    assert(!trace[0]);
    invoke("call_intent", false, 0, 0, NULL);
    assert(!trace[0]);
    stop_error = XIAOTAI_ERR_INVALID_STATE;
    invoke("call", true, 1, 0, "{\"target_name\":\"Bob\"}");
    assert(!strcmp(trace, "RSF") && refused == 1 && talk.session);
    stop_error = XIAOTAI_OK;
    dial_error = XIAOTAI_ERR_BUSY;
    invoke("call", true, 1, 0, "{\"target_name\":\"Bob\"}");
    assert(!strcmp(trace, "RSDF") && refused == 2);
    memset(available[1].id, 'b', 64);
    start_turn(7, 99);
    (void)snprintf(json, sizeof(json), "{\"name\":\"Bob\",\"target_id\":\"%s\"}", available[1].id);
    dial_error = XIAOTAI_OK;
    invoke("call", true, 1, 0, json);
    call_result("accepted", true);
    assert(strlen(dialled) == 64);
    start_turn(7, 99);
    (void)snprintf(json, sizeof(json),
        "{\"target_name\":\"%065d\",\"target\":7,\"name\":\"Bob\",\"target_id\":\"\",\"peer_id\":\"%s\"}",
        0, available[1].id);
    invoke("call", true, 1, 0, json);
    call_result("accepted", true);
    assert(strlen(dialled) == 64 && !strcmp(id_read, dialled) && !strcmp(name_read, "Bob"));
}

static xiaotai_json_doc_t reply_doc(void)
{
    static xiaotai_json_token_t tokens[320];
    xiaotai_json_doc_t doc;
    assert(xiaotai_json_parse(reply.object, strlen(reply.object), tokens, ARRAY_LEN(tokens), NULL, &doc) == XIAOTAI_OK);
    return doc;
}

static void query_failure(const char *status, int32_t code, bool legacy)
{
    assert(reply.request && reply.code == code && reply.limit == (legacy ? 4095 : 0));
    const xiaotai_json_doc_t doc = reply_doc();
    assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "status"), status));
    assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "count"), "0"));
    const int rows = xiaotai_json_get(&doc, 0, "contacts");
    assert(rows >= 0 && doc.tokens[rows].type == XIAOTAI_JSON_ARRAY && !doc.tokens[rows].children);
    if (legacy) assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "message"), reply.message));
    else assert(!strcmp(reply.message, status));
    bool ok = true;
    assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "ok"), &ok) == XIAOTAI_OK && !ok);
}

static xiaotai_json_doc_t query_result(unsigned count)
{
    assert(reply.request && !reply.code);
    const xiaotai_json_doc_t doc = reply_doc();
    char number[12];
    (void)snprintf(number, sizeof(number), "%u", count);
    assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "status"), "ok"));
    assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "count"), number));
    const int rows = xiaotai_json_get(&doc, 0, "contacts");
    assert(rows >= 0 && doc.tokens[rows].type == XIAOTAI_JSON_ARRAY && doc.tokens[rows].children == count);
    bool ok = false;
    assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "ok"), &ok) == XIAOTAI_OK && ok);
    return doc;
}

static void ask(const char *json, uint32_t request)
{
    invoke("query_contact_status", true, request, 0, json);
}

static void answer_available(void) { answer(available, ARRAY_LEN(available), XIAOTAI_OK, false); }

static void test_legacy_remark(void)
{
    char long_remark[151];
    memset(long_remark, 'a', sizeof(long_remark) - 1U);
    long_remark[sizeof(long_remark) - 1U] = '\0';
    long_remark[10] = '"';
    long_remark[11] = '\\';
    const char *const remarks[] = {long_remark, "machine-a", NULL};
    answer_remarks = remarks;
    ask("{\"status_filter\":\"all\"}", 32);
    answer_available();
    xiaotai_json_doc_t doc = query_result(3);
    assert(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "contacts[0].name"), long_remark));
    assert(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "contacts[2].name"), "微信联系人"));
    ask("{\"target\":\"machine-a\"}", 33);
    answer_available();
    doc = query_result(1);
    assert(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "contacts[0].device_id"), "machine-a"));
    assert(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "contacts[0].name"), long_remark));
    ask("{\"target\":\"Alice\"}", 34);
    answer_available();
    query_failure("not_found", -32000, true);
    ask("{\"target\":\"微信联系人\"}", 35);
    answer_available();
    (void)query_result(1);
    answer_remarks = NULL;
}

static void test_queries(void)
{
    set_policy(XIAOTAI_COMPANION_AI_CONTACT_TOOLS_LEGACY);
    start_turn(7, 99);
    ask("{}", 0);
    assert(!query.answer && !reply.request);
    ask("{}", 1);
    assert(query.answer && !reply.request);
    ask("{}", 2);
    assert(reply.request == 2 && query_submissions == 1);
    query_failure("busy", -32000, true);
    answer_available();
    assert(reply.request == 1 && reply.limit == 4095);
    xiaotai_json_doc_t doc = query_result(2);
    const int voip = xiaotai_json_find(&doc, 0, "contacts[1]");
    bool flag = true;
    assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, voip, "type"), "voip"));
    assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, voip, "online_known"), &flag) == XIAOTAI_OK && !flag);
    assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, voip, "online"), &flag) == XIAOTAI_OK && flag);
    assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "message"),
           "查询成功；微信联系人按约定视为可联系，非实时在线检测。"));
    ask("{\"status_filter\":\"offline\"}", 3);
    answer_available();
    doc = query_result(1);
    assert(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "contacts[0].device_id"), "machine-b"));
    ask("{\"status_filter\":\"all\"}", 4);
    answer_available();
    (void)query_result(3);
    ask("{\"target\":\"machine-b\"}", 5);
    answer_available();
    (void)query_result(1);
    ask("{\"target\":\" B o B \",\"status_filter\":\"online\"}", 6);
    answer_available();
    (void)query_result(1);
    ask("{\"target\":\"alICE\"}", 7);
    answer_available();
    query_failure("ambiguous", -32000, true);
    ask("{\"target\":\"missing\"}", 8);
    answer_available();
    query_failure("not_found", -32000, true);
    ask("{\"status_filter\":\"bad\"}", 9);
    query_failure("invalid_params", -32602, true);
    assert(!strcmp(reply.message, "status_filter 只能是 online、offline 或 all。"));
    ask("{\"target\":false}", 10);
    query_failure("invalid_params", -32602, true);
    ask("{\"status_filter\":false}", 11);
    query_failure("invalid_params", -32602, true);
    invoke("query_contact_status", true, 12, 129, "{}");
    query_failure("invalid_params", -32602, true);
    char too_long[96];
    (void)snprintf(too_long, sizeof(too_long), "{\"target\":\"%065d\"}", 0);
    ask(too_long, 13);
    query_failure("invalid_params", -32602, true);
    ask("{}", 14);
    advance(9999);
    assert(!reply.request && query.answer);
    advance(1);
    assert(!query.answer);
    query_failure("timeout", -32000, true);
    assert(!strcmp(reply.message, "联系人查询超时，请稍后重试。"));
    query_error = XIAOTAI_ERR_PLATFORM_OFFLINE;
    ask("{}", 15);
    query_failure("unavailable", -32000, true);
    query_error = XIAOTAI_OK;
    ask("{}", 16);
    answer(NULL, 0, XIAOTAI_FAIL, false);
    query_failure("upstream_error", -32000, true);
    ask("{}", 38);
    answer(NULL, 0, XIAOTAI_ERR_TIMEOUT, false);
    query_failure("upstream_error", -32000, true);
    ask("{}", 17);
    answer(NULL, 0, XIAOTAI_ERR_INVALID_SIZE, false);
    query_failure("response_too_large", -32000, true);
    ask("{}", 18);
    answer(NULL, 0, XIAOTAI_OK, true);
    query_failure("response_too_large", -32000, true);
    ask("{}", 19);
    skipped = 1;
    answer_available();
    query_failure("upstream_error", -32000, true);
    skipped = 0;
    const xiaotai_contact_t unknown = {.kind = 0, .id = "unclear"};
    ask("{}", 20);
    answer(&unknown, 1, XIAOTAI_OK, false);
    query_failure("status_unavailable", -32000, true);
    ask("{\"status_filter\":\"all\"}", 21);
    answer(&unknown, 1, XIAOTAI_OK, false);
    doc = query_result(1);
    const int online = xiaotai_json_find(&doc, 0, "contacts[0].online");
    assert(online >= 0 && doc.tokens[online].type == XIAOTAI_JSON_NULL);
    xiaotai_contact_t many[17];
    for (size_t i = 0; i < ARRAY_LEN(many); ++i) many[i] = available[0];
    ask("{\"status_filter\":\"all\"}", 22);
    answer(many, 16, XIAOTAI_OK, false);
    (void)query_result(16);
    ask("{\"status_filter\":\"all\"}", 23);
    answer(many, 17, XIAOTAI_OK, false);
    query_failure("response_too_large", -32000, true);
    ask("{}", 24);
    fail_alloc = true;
    answer_available();
    query_failure("no_memory", -32000, true);
    ask("{}", 25);
    reply_error = XIAOTAI_ERR_INVALID_SIZE;
    answer_available();
    query_failure("response_too_large", -32000, true);
    ask("{}", 26);
    const xiaotai_contacts_answer_fn_t old = query.answer;
    void *old_ctx = query.ctx;
    start_turn(8, 100);
    assert(cancellations == 1 && !query.answer && !reply.request);
    ask("{}", 27);
    old(&(const xiaotai_contacts_answer_t){.contacts = available, .count = 3}, old_ctx);
    assert(!reply.request && query.answer);
    answer_available();
    assert(reply.request == 27);
    (void)query_result(2);
    ask("{}", 28);
    start_turn(8, 101);
    assert(cancellations == 2 && !reply.request);
    ask("{}", 29);
    answer(NULL, 0, XIAOTAI_ERR_PLATFORM_STALE, false);
    assert(!reply.request);
    test_legacy_remark();
    response_size = 4096;
    ask("{}", 36);
    answer_available();
    (void)query_result(2);
    response_size = 4097;
    ask("{}", 37);
    answer_available();
    query_failure("response_too_large", -32000, true);
    response_size = 0;

    set_policy(XIAOTAI_COMPANION_AI_CONTACT_TOOLS_STANDARD);
    assert(tool_count == 11 && !fallback.handler);
    ask("{}", 39);
    answer(NULL, 0, XIAOTAI_ERR_TIMEOUT, false);
    query_failure("timeout", -32000, false);
    ask("{}", 40);
    advance(10000);
    query_failure("timeout", -32000, false);
    query_error = XIAOTAI_ERR_PLATFORM_OFFLINE;
    ask("{}", 30);
    query_failure("status_unavailable", -32000, false);
    query_error = XIAOTAI_OK;
    ask("{\"target\":\"machine-b\"}", 31);
    answer_available();
    query_failure("not_found", -32000, false);
}

static void test_query_lifecycle(void)
{
    set_policy(XIAOTAI_COMPANION_AI_CONTACT_TOOLS_LEGACY);
    start_turn(7, 99);
    const int64_t live = xiaotai_mem_host_outstanding();
    const unsigned submissions = query_submissions, cancelled = cancellations;
    for (uint32_t request = 1; request <= 500; ++request) {
        const uint32_t allocations = xiaotai_mem_host_allocations();
        ask("{\"status_filter\":\"all\"}", request);
        assert(query.answer && !reply.request && query_submissions == submissions + request);
        assert(xiaotai_mem_host_allocations() == allocations);
        const xiaotai_contacts_answer_fn_t completed = query.answer;
        void *const ticket = query.ctx;
        answer_available();
        (void)query_result(3);
        assert(!query.answer && reply.request == request && !strcmp(trace, "R"));
        assert(xiaotai_mem_host_outstanding() == live);

        /* Replayed owner delivery must not observe an outstanding query. */
        completed(&(const xiaotai_contacts_answer_t){.contacts = available, .count = ARRAY_LEN(available)}, ticket);
        assert(!strcmp(trace, "R") && reply.request == request && xiaotai_mem_host_outstanding() == live);
        assert(talk.phase == XIAOTAI_AI_TALK_LISTENING && talk.session == 7 && talk.link == 99 &&
               cancellations == cancelled);
    }
}

static void run(void *mode)
{
    if (!mode) test_calls();
    else if (!strcmp(mode, "query-lifecycle")) test_query_lifecycle();
    else test_queries();
    xiaotai_signal_give(&done);
}

int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && (!strcmp(argv[1], "query") || !strcmp(argv[1], "query-lifecycle"))));
    assert(xiaotai_signal_init(&done) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){0}) == XIAOTAI_OK);
    assert(xiaotai_run(run, argc == 2 ? argv[1] : NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&done, 5000));
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&done);
    return 0;
}
