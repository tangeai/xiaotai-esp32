// SPDX-License-Identifier: MIT
#include "ai_contact_tools.h"
#include "companion_contact_route.h"
#include "companion_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/json.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/port/mem.h"

#define COUNT(array) (sizeof(array) / sizeof(*(array)))
#define QUERY_MAX 16U
#define LEGACY_WIRE_BYTES 4095U
#define QUERY_TIMEOUT_MS 10000U

typedef enum {ONLINE, OFFLINE, ALL} filter_t;
typedef struct {
    uint32_t ticket, session, link, request;
    filter_t filter;
    char target[XIAOTAI_CONTACT_NAME_MAX + 1U];
} pending_query_t;

static pending_query_t pending;

static bool legacy(void)
{
    return companion.config.ai_contact_tools == XIAOTAI_COMPANION_AI_CONTACT_TOOLS_LEGACY;
}

static const char *const call_actions[] = {
    "call", "call_contact", "call_device", "device_call", "start_device_call",
    "call_wechat", "wechat_call", "start_voip_call", "call_intent", "ai_call_intent",
};
enum {CALL_REQUEST_NAMES = 8};
static const char request_error[] = "设备未实现该动作或参数无效。";
static const char query_error[] = "联系人查询失败，未使用过期状态。";
static const char too_large[] = "查询结果过大，请指定联系人。";

static void headers(xiaotai_json_writer_t *writer, bool ok, const char *status, const char *message)
{
    xiaotai_json_object_begin(writer, NULL);
    xiaotai_json_put_bool(writer, "ok", ok);
    xiaotai_json_put_string(writer, "status", status);
    xiaotai_json_put_string(writer, "message", message);
}

static void query_failure(uint32_t session, uint32_t request, const char *status, const char *message)
{
    char data[272];
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, data, sizeof(data));
    headers(&writer, false, status, message);
    xiaotai_json_put_string(&writer, "count", "0");
    xiaotai_json_array_begin(&writer, "contacts");
    xiaotai_json_array_end(&writer);
    xiaotai_json_object_end(&writer);
    if (xiaotai_json_writer_end(&writer, NULL) != XIAOTAI_OK) return;
    const int32_t code = !strcmp(status, "invalid_params") ? XIAOTAI_AI_TALK_ERROR_INVALID_PARAMS :
        !strcmp(status, "unsupported_action") ? XIAOTAI_AI_TALK_ERROR_UNSUPPORTED : XIAOTAI_AI_TALK_ERROR_FAILED;
    if (legacy()) {
        const xiaotai_ai_talk_tool_response_t response = {.object = data, .code = code, .message = message,
            .max_bytes = LEGACY_WIRE_BYTES};
        (void)xiaotai_ai_talk_tool_respond(session, request, &response);
    } else {
        (void)xiaotai_ai_talk_tool_fail(session, request, code, status, data);
    }
}

static void unrecognized(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (call->request) query_failure(call->session, call->request,
                                     *call->name ? "unsupported_action" : "invalid_params", request_error);
}

static void call_reply(const xiaotai_ai_talk_tool_call_t *call, bool ok, const char *status, const char *message)
{
    char data[184];
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, data, sizeof(data));
    headers(&writer, ok, status, message);
    xiaotai_json_object_end(&writer);
    if (call->device_action && call->request && xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_OK)
        (void)xiaotai_ai_talk_tool_reply(call->session, call->request, data);
}

static void first_string(const xiaotai_ai_talk_tool_call_t *call, const char *const *names, size_t count,
                         char *out, size_t capacity)
{
    for (size_t i = 0; i < count; ++i)
        if (xiaotai_json_text_string(call->arguments, call->size, names[i], out, capacity, NULL) == XIAOTAI_OK && out[0])
            return;
    out[0] = '\0';
}

static bool channel_kind(const char *channel, int *kind)
{
    if (!*channel) {
        *kind = XIAOTAI_CONTACT_ANY;
        return true;
    }
    const char *name = NULL;
    if (!strcmp(channel, "wx") || !strcmp(channel, "wechat") || !strcmp(channel, "voip")) name = "wechat";
    else if (!strcmp(channel, "device") || !strcmp(channel, "call") || !strcmp(channel, "tirtc")) name = "device";
    return name && xiaotai_contacts_kind_find(name, kind) == XIAOTAI_OK;
}

static void call_tool(const xiaotai_ai_talk_tool_call_t *call, void *ctx)
{
    const bool notification = (uintptr_t)ctx >= CALL_REQUEST_NAMES;
    if (notification == call->device_action) {
        if (call->device_action) unrecognized(call, NULL);
        return;
    }
    if (notification && !call->has_arguments) return;
    const char *const id_names[] = {"target_id", "target_device_id", "device_id", "callee_device_id", "peer_id"};
    const char *const name_names[] = {"target_name", "target", "contact_name", "contact", "device_name",
        "device_alias", "nickname", "alias", "remark", "callee", "name"};
    const char *const channel_names[] = {"channel", "source", "type"};
    const char *const media_names[] = {"media", "call_type", "room_type"};
    char id[65], name[65], channel[16], media[16];
    first_string(call, id_names, COUNT(id_names), id, sizeof(id));
    first_string(call, name_names, COUNT(name_names), name, sizeof(name));
    first_string(call, channel_names, COUNT(channel_names), channel, sizeof(channel));
    first_string(call, media_names, COUNT(media_names), media, sizeof(media));
    const bool audio = !media[0] || !strcmp(media, "audio") || !strcmp(media, "voice");
    size_t matches = 0;
    int kind;
    xiaotai_contact_t contact;
    if (audio && name[0] && channel_kind(channel, &kind) &&
        xiaotai_contacts_match(name, kind, id, XIAOTAI_CONTACTS_STALE_OK, &contact, &matches) == XIAOTAI_OK) {
        call_reply(call, true, "accepted", "正在发起语音呼叫。");
        xiaotai_err_t err = xiaotai_ai_talk_stop(call->session);
        if (err == XIAOTAI_OK) err = companion_dial_contact(&contact, false);
        if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_INTENT_APP_COMPANION_DIAL, err);
    } else {
        const char *message = !audio ? "小钛目前只支持语音呼叫。" :
            matches > 1 ? "找到多个同名联系人，请说得更具体。" : "通讯录里没有找到这个联系人。";
        call_reply(call, false, matches > 1 ? "ambiguous" : "not_found", message);
        companion_ai_talk_hint(call->session, message);
    }
}

static unsigned char normalized_next(const char **cursor)
{
    while (**cursor) {
        const unsigned char c = (unsigned char)*(*cursor)++;
        if (c == ' ' || (c >= '\t' && c <= '\r')) continue;
        return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
    }
    return 0;
}

static bool named(const char *a, const char *b)
{
    unsigned char letter = normalized_next(&a);
    if (!letter) return false;
    while (letter == normalized_next(&b)) {
        if (!letter) return true;
        letter = normalized_next(&a);
    }
    return false;
}

static const char *display_name(const xiaotai_contact_t *person, bool wechat)
{
    return person->name[0] ? person->name : wechat ? "微信联系人" : person->id;
}

static const char *unnamed(const xiaotai_contact_t *person, bool wechat)
{
    return wechat ? "微信联系人" : person->id;
}

static xiaotai_err_t reply_name(const xiaotai_contacts_answer_t *answer, size_t index, bool wechat,
                                char small[65], char **allocated, const char **name)
{
    const xiaotai_contact_t *person = &answer->contacts[index];
    *allocated = NULL;
    *name = display_name(person, wechat);
    if (!legacy()) return XIAOTAI_OK;
    size_t size = 0;
    xiaotai_err_t err = xiaotai_contacts_answer_remark(answer, index, small, 65, &size);
    *name = small;
    if (err == XIAOTAI_ERR_INVALID_SIZE && size < LEGACY_WIRE_BYTES) {
        *allocated = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, size + 1U);
        err = *allocated ? xiaotai_contacts_answer_remark(answer, index, *allocated, size + 1U, NULL) :
                           XIAOTAI_ERR_NO_MEM;
        *name = *allocated;
    }
    if (err == XIAOTAI_ERR_NOT_FOUND) {
        *name = unnamed(person, wechat);
        return XIAOTAI_OK;
    }
    return err;
}

static bool is_wechat(const xiaotai_contact_t *person)
{
    const xiaotai_contact_kind_def_t *kind = xiaotai_contacts_kind(person->kind);
    return kind && !strcmp(kind->name, "wechat");
}

static void serialize(uint32_t session, uint32_t request, const xiaotai_contacts_answer_t *answer, const size_t *people,
                      size_t count, bool assumed)
{
    const size_t capacity = XIAOTAI_AI_TALK_TOOL_REPLY_MAX;
    char *data = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, capacity);
    if (!data) {
        query_failure(session, request, "no_memory", "联系人查询内存不足。");
        return;
    }
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, data, capacity);
    headers(&writer, true, "ok", assumed ? "查询成功；微信联系人按约定视为可联系，非实时在线检测。" : "查询成功。");
    char number[12];
    (void)snprintf(number, sizeof(number), "%u", (unsigned)count);
    xiaotai_json_put_string(&writer, "count", number);
    xiaotai_json_array_begin(&writer, "contacts");
    xiaotai_err_t err = XIAOTAI_OK;
    for (size_t i = 0; err == XIAOTAI_OK && i < count; ++i) {
        const xiaotai_contact_t *person = &answer->contacts[people[i]];
        const bool wechat = is_wechat(person);
        char small[65], *allocated;
        const char *name;
        err = reply_name(answer, people[i], wechat, small, &allocated, &name);
        if (err != XIAOTAI_OK) { xiaotai_mem_free(allocated); break; }
        xiaotai_json_object_begin(&writer, NULL);
        xiaotai_json_put_string(&writer, "name", name);
        xiaotai_mem_free(allocated);
        xiaotai_json_put_string(&writer, "device_id", person->id);
        if (wechat || person->online_known) xiaotai_json_put_bool(&writer, "online", wechat || person->online);
        else xiaotai_json_put_null(&writer, "online");
        xiaotai_json_put_string(&writer, "type", wechat ? "voip" : "device");
        xiaotai_json_put_bool(&writer, "online_known", !wechat && person->online_known);
        xiaotai_json_object_end(&writer);
    }
    xiaotai_json_array_end(&writer);
    xiaotai_json_object_end(&writer);
    if (err == XIAOTAI_OK) err = xiaotai_json_writer_end(&writer, NULL);
    if (err == XIAOTAI_OK) {
        if (legacy()) {
            const xiaotai_ai_talk_tool_response_t response = {.object = data, .max_bytes = LEGACY_WIRE_BYTES};
            err = xiaotai_ai_talk_tool_respond(session, request, &response);
        } else {
            err = xiaotai_ai_talk_tool_reply(session, request, data);
        }
    }
    xiaotai_mem_free(data);
    if (err == XIAOTAI_ERR_INVALID_SIZE) query_failure(session, request, "response_too_large", too_large);
    else if (err == XIAOTAI_ERR_NO_MEM) query_failure(session, request, "no_memory", "联系人查询内存不足。");
}

static void answer_query(const xiaotai_contacts_answer_t *answer, void *ctx)
{
    if (!pending.ticket || (uint32_t)(uintptr_t)ctx != pending.ticket) return;
    const pending_query_t query = pending;
    pending.ticket = 0;
    xiaotai_ai_talk_status_t talk;
    xiaotai_ai_talk_status(&talk);
    if (query.session != talk.session || query.link != talk.link ||
        talk.phase < XIAOTAI_AI_TALK_LISTENING || talk.phase > XIAOTAI_AI_TALK_SPEAKING ||
        answer->result == XIAOTAI_ERR_PLATFORM_STALE) return;
    const char *error = NULL;
    const char *message = query_error;
    if (answer->result == XIAOTAI_ERR_TIMEOUT && (!legacy() || answer->deadline_expired)) {
        error = "timeout";
        message = "联系人查询超时，请稍后重试。";
    } else if (answer->result == XIAOTAI_ERR_INVALID_SIZE || answer->truncated ||
               (legacy() && answer->response_bytes > LEGACY_WIRE_BYTES + 1U)) {
        error = "response_too_large";
    } else if (answer->result != XIAOTAI_OK) {
        error = "upstream_error";
    }
    xiaotai_contacts_status_t contacts;
    xiaotai_contacts_status(&contacts);
    if (!error && legacy() && contacts.skipped) error = "upstream_error";
    bool assumed = false, by_id = false;
    const char *cursor = query.target;
    const bool has_target = normalized_next(&cursor) != 0;
    if (!error && legacy() && has_target) {
        for (size_t i = 0; i < answer->count; ++i) {
            if (!strcmp(query.target, answer->contacts[i].id)) { by_id = true; break; }
        }
    }
    size_t people[QUERY_MAX];
    size_t count = 0;
    for (size_t i = 0; !error && i < answer->count; ++i) {
        const xiaotai_contact_t *person = &answer->contacts[i];
        const bool wechat = is_wechat(person);
        if (has_target) {
            if (by_id) {
                if (strcmp(query.target, person->id)) continue;
            } else {
                char small[65], *allocated;
                const char *name;
                const xiaotai_err_t err = reply_name(answer, i, wechat, small, &allocated, &name);
                const bool matches = err == XIAOTAI_OK && named(query.target, name);
                xiaotai_mem_free(allocated);
                if (err != XIAOTAI_OK) {
                    error = err == XIAOTAI_ERR_NO_MEM ? "no_memory" : "upstream_error";
                    if (err == XIAOTAI_ERR_NO_MEM) message = "联系人查询内存不足。";
                    break;
                }
                if (!matches) continue;
            }
        }
        if (!wechat && !person->online_known && (has_target || query.filter != ALL)) {
            error = "status_unavailable";
            break;
        }
        if (!has_target && query.filter != ALL && ((wechat || person->online) != (query.filter == ONLINE))) continue;
        if (count == QUERY_MAX) {
            error = "response_too_large";
            break;
        }
        people[count++] = i;
        assumed = assumed || wechat;
    }
    if (!error && has_target && count != 1U) error = count == 0U ? "not_found" : "ambiguous";
    if (error) query_failure(query.session, query.request, error, message);
    else serialize(query.session, query.request, answer, people, count, assumed);
}

void companion_ai_contact_tools_observe(uint32_t session, uint32_t link)
{
    if (pending.ticket && (pending.session != session || pending.link != link)) {
        const uint32_t ticket = pending.ticket;
        pending.ticket = 0;
        (void)xiaotai_contacts_query_cancel(answer_query, (void *)(uintptr_t)ticket);
    }
}

static xiaotai_err_t parse_query(const xiaotai_ai_talk_tool_call_t *call, pending_query_t *query, const char **description)
{
    size_t needed = 0;
    if (xiaotai_json_parse(call->arguments, call->size, NULL, 0, &needed, NULL) != XIAOTAI_OK)
        return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_json_token_t small[12];
    xiaotai_json_token_t *tokens = needed > COUNT(small) ?
        xiaotai_mem_alloc(XIAOTAI_MEM_BULK, needed * sizeof(*tokens)) : small;
    if (!tokens) return XIAOTAI_ERR_NO_MEM;
    xiaotai_json_doc_t doc;
    xiaotai_err_t err = xiaotai_json_parse(call->arguments, call->size, tokens, needed, NULL, &doc);
    if (err == XIAOTAI_OK) {
        const int target = xiaotai_json_get(&doc, 0, "target");
        const int filter = xiaotai_json_get(&doc, 0, "status_filter");
        if (target >= 0 && xiaotai_json_string(&doc, target, query->target, sizeof(query->target), NULL) != XIAOTAI_OK)
            err = XIAOTAI_ERR_INVALID_ARG;
        else if (filter < 0 || xiaotai_json_equals(&doc, filter, "online")) query->filter = ONLINE;
        else if (xiaotai_json_equals(&doc, filter, "offline")) query->filter = OFFLINE;
        else if (xiaotai_json_equals(&doc, filter, "all")) query->filter = ALL;
        else {
            if (doc.tokens[filter].type == XIAOTAI_JSON_STRING)
                *description = "status_filter 只能是 online、offline 或 all。";
            err = XIAOTAI_ERR_INVALID_ARG;
        }
    }
    if (tokens != small) xiaotai_mem_free(tokens);
    return err;
}

static void query_tool(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action || !call->request) return;
    xiaotai_ai_talk_status_t talk;
    xiaotai_ai_talk_status(&talk);
    if (talk.session != call->session || !talk.link) return;
    companion_ai_contact_tools_observe(talk.session, talk.link);
    pending_query_t query = {.request = call->request, .session = call->session, .link = talk.link};
    const char *description = "查询参数无效。";
    xiaotai_err_t err = legacy() && call->id_text_bytes > 128U ?
        XIAOTAI_ERR_INVALID_ARG : parse_query(call, &query, &description);
    const char *failure;
    if (err == XIAOTAI_ERR_NO_MEM) {
        failure = "no_memory";
        description = "联系人查询内存不足。";
    } else if (err != XIAOTAI_OK) {
        failure = "invalid_params";
    } else if (pending.ticket) {
        failure = "busy";
        description = "已有联系人查询正在进行。";
    } else {
        query.ticket = xiaotai_generation_next();
        err = xiaotai_contacts_query(XIAOTAI_CONTACT_ANY, QUERY_TIMEOUT_MS, answer_query,
                                     (void *)(uintptr_t)query.ticket);
        if (err == XIAOTAI_OK) {
            pending = query;
            return;
        }
        if (!legacy() && err == XIAOTAI_ERR_PLATFORM_OFFLINE) {
            failure = "status_unavailable";
            description = query_error;
        } else {
            failure = "unavailable";
            description = "联系人服务暂不可用。";
        }
    }
    query_failure(call->session, call->request, failure, description);
}

xiaotai_err_t companion_ai_contact_tools_init(void)
{
    if (companion.config.ai_contact_tools == XIAOTAI_COMPANION_AI_CONTACT_TOOLS_DISABLED) return XIAOTAI_OK;
    xiaotai_err_t err = XIAOTAI_OK;
    for (size_t i = 0; i < COUNT(call_actions) && err == XIAOTAI_OK; ++i)
        err = xiaotai_ai_talk_tool_register(call_actions[i], call_tool, (void *)(uintptr_t)i);
    if (err == XIAOTAI_OK) err = xiaotai_ai_talk_tool_register("query_contact_status", query_tool, NULL);
    if (err == XIAOTAI_OK && legacy()) err = xiaotai_ai_talk_tool_fallback_register(unrecognized, NULL);
    return err;
}

void companion_ai_contact_tools_reset(void) { memset(&pending, 0, sizeof(pending)); }
