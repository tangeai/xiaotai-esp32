// SPDX-License-Identifier: MIT
#include "ai_talk_internal.h"

#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/link.h"
#include "xiaotai/port/mem.h"

#define PENDING_MAX 4U
#define INLINE_ID_BYTES 96U
#define SHARED_REPLY_BYTES (XIAOTAI_AI_TALK_TOOL_REPLY_MAX + 256U)

typedef struct {
    const char *name;
    xiaotai_ai_talk_tool_fn_t handler;
    void *ctx;
    xiaotai_command_scope_t scope;
} tool_t;

typedef struct {
    uint32_t handle, session, link; /* handle == 0 denotes a free slot */
    size_t id_size;
    union {                       /* checked JSON literal, including string quotes */
        char small[INLINE_ID_BYTES];
        char *large;              /* only when id_size > INLINE_ID_BYTES */
    } id;
} pending_t;

static struct {
    size_t tool_count;
    tool_t tools[XIAOTAI_AI_TALK_TOOLS_MAX];
    tool_t fallback;
    pending_t pending[PENDING_MAX];
    uint32_t serial;
    char *reply;
} s_tools;

static const tool_t *lookup(const char *name)
{
    for (size_t i = 0; i < s_tools.tool_count; ++i) {
        if (strcmp(s_tools.tools[i].name, name) == 0) return &s_tools.tools[i];
    }
    return NULL;
}

xiaotai_err_t xiaotai_ai_talk_tool_register(const char *name, xiaotai_ai_talk_tool_fn_t handler, void *ctx)
{
    if (!name || !*name || strlen(name) >= XIAOTAI_AI_TALK_TOOL_NAME_MAX || !handler)
        return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_command_scope_t scope;
    if (xiaotai_command_scope_capture(&scope) != XIAOTAI_OK || lookup(name)) return XIAOTAI_ERR_INVALID_STATE;
    if (s_tools.tool_count == XIAOTAI_AI_TALK_TOOLS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_tools.tools[s_tools.tool_count++] = (tool_t){.name = name, .handler = handler, .ctx = ctx, .scope = scope};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_tool_fallback_register(xiaotai_ai_talk_tool_fn_t handler, void *ctx)
{
    if (!handler) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_command_scope_t scope;
    if (s_tools.fallback.handler || xiaotai_command_scope_capture(&scope) != XIAOTAI_OK)
        return XIAOTAI_ERR_INVALID_STATE;
    s_tools.fallback = (tool_t){.handler = handler, .ctx = ctx, .scope = scope};
    return XIAOTAI_OK;
}

void ai_talk_tools_start(void)
{
    if (!s_tools.reply && xiaotai_mem_has_external()) {
        s_tools.reply = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, SHARED_REPLY_BYTES);
    }
}

static const char *pending_id(const pending_t *entry)
{
    return entry->id_size > INLINE_ID_BYTES ? entry->id.large : entry->id.small;
}

static void discard(pending_t *entry)
{
    if (entry->id_size > INLINE_ID_BYTES) xiaotai_mem_free(entry->id.large);
    *entry = (pending_t){0};
}

void ai_talk_tools_end(uint32_t session)
{
    for (size_t i = 0; i < PENDING_MAX; ++i) {
        if (s_tools.pending[i].session == session) discard(&s_tools.pending[i]);
    }
}

void ai_talk_tools_reset(void)
{
    for (size_t i = 0; i < PENDING_MAX; ++i) discard(&s_tools.pending[i]);
    xiaotai_mem_free(s_tools.reply);
    memset(&s_tools, 0, sizeof(s_tools));
}

static const char *wire_id(const xiaotai_json_view_t *value, size_t *size)
{
    *size = 0;
    if (value->type != XIAOTAI_JSON_NUMBER && value->type != XIAOTAI_JSON_STRING) return NULL;
    /* Preserve wire spelling, including the quotes of a string ID. */
    *size = value->length;
    return value->text;
}

static xiaotai_json_view_t argument_object(const xiaotai_json_view_t *params)
{
    static const char *const wrappers[] = {"data", "arguments", "args", "input", "payload", "parameters"};
    if (params->type != XIAOTAI_JSON_OBJECT) return (xiaotai_json_view_t){0};
    xiaotai_json_view_t fields[sizeof(wrappers) / sizeof(wrappers[0])];
    ai_talk_fields(params, wrappers, fields, sizeof(fields) / sizeof(fields[0]));
    for (size_t i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); ++i) {
        if (fields[i].type == XIAOTAI_JSON_OBJECT) return fields[i];
    }
    return *params;
}

static bool action_name(const xiaotai_json_view_t *object, char *out)
{
    static const char *const names[] = {"action", "name", "tool", "function"};
    xiaotai_json_view_t fields[sizeof(names) / sizeof(names[0])];
    ai_talk_fields(object, names, fields, sizeof(fields) / sizeof(fields[0]));
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (xiaotai_json_view_string(&fields[i], out, XIAOTAI_AI_TALK_TOOL_NAME_MAX, NULL) == XIAOTAI_OK && out[0])
            return true;
    }
    return false;
}

static pending_t *reserve(uint32_t session, uint32_t link, const char *id, size_t size)
{
    for (size_t i = 0; i < PENDING_MAX; ++i) {
        pending_t *entry = &s_tools.pending[i];
        if (entry->handle) continue;
        /* The ingress command bounds size. No retained storage for rejects. */
        char *large = size > INLINE_ID_BYTES ? xiaotai_mem_alloc(XIAOTAI_MEM_BULK, size) : NULL;
        if (size > INLINE_ID_BYTES && !large) return NULL;
        if (++s_tools.serial == 0) ++s_tools.serial;
        *entry = (pending_t){.handle = s_tools.serial, .session = session, .link = link, .id_size = size};
        if (large) entry->id.large = large;
        memcpy(large ? large : entry->id.small, id, size);
        return entry;
    }
    return NULL;
}

static size_t integer_bytes(int32_t value)
{
    size_t bytes = value < 0 ? 2U : 1U;
    uint32_t magnitude = value < 0 ? 0U - (uint32_t)value : (uint32_t)value;
    for (; magnitude >= 10U; magnitude /= 10U) ++bytes;
    return bytes;
}

/* The writer copies both JSON literals verbatim. A printable error message
 * needs one additional byte for each quote or backslash. */
static size_t rpc_length(size_t id_size, size_t object_size, int32_t error, const char *message)
{
    size_t bytes = sizeof("{\"jsonrpc\":\"2.0\",\"id\":}") - 1U + id_size + object_size;
    if (!error) return bytes + sizeof(",\"result\":") - 1U;
    bytes += sizeof(",\"error\":{\"code\":,\"message\":\"\"}") - 1U + integer_bytes(error) + strlen(message);
    for (; *message; ++message) bytes += *message == '"' || *message == '\\';
    return bytes + (object_size ? sizeof(",\"data\":") - 1U : 0U);
}

static xiaotai_err_t reply_failed(xiaotai_err_t error)
{
    if (error != XIAOTAI_OK) ai_talk_count_tool_reply_failure();
    return error;
}

static xiaotai_err_t rpc_send_bounded(const char *id, size_t id_size, const char *object, size_t object_size,
                                     int32_t error, const char *message, pending_t *consume, size_t max_bytes)
{
    const size_t bytes = rpc_length(id_size, object_size, error, message);
    if (bytes > XIAOTAI_LINK_COMMAND_LIMIT_BYTES || (max_bytes && bytes > max_bytes))
        return reply_failed(XIAOTAI_ERR_INVALID_SIZE);
    const size_t capacity = bytes + 1U;
    const bool temporary = !s_tools.reply || capacity > SHARED_REPLY_BYTES;
    char *text = temporary ? xiaotai_mem_alloc(XIAOTAI_MEM_BULK, capacity) : s_tools.reply;
    if (!text) return reply_failed(XIAOTAI_ERR_NO_MEM);
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, text, capacity);
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, "jsonrpc", "2.0");
    xiaotai_json_put_raw(&writer, "id", id, id_size);
    if (error) {
        xiaotai_json_object_begin(&writer, "error");
        xiaotai_json_put_int(&writer, "code", error);
        xiaotai_json_put_string(&writer, "message", message);
        if (object) xiaotai_json_put_raw(&writer, "data", object, object_size);
        xiaotai_json_object_end(&writer);
    } else {
        xiaotai_json_put_raw(&writer, "result", object, object_size);
    }
    xiaotai_json_object_end(&writer);
    size_t length = 0;
    xiaotai_err_t result = xiaotai_json_writer_end(&writer, &length);
    if (result == XIAOTAI_OK) {
        /* A send may fail after partial delivery: don't send the same handle twice. */
        if (consume) discard(consume);
        result = ai_talk_send(text, length);
    }
    if (temporary) xiaotai_mem_free(text);
    return reply_failed(result);
}

static xiaotai_err_t rpc_send(const char *id, size_t id_size, const char *object, size_t object_size,
                             int32_t error, const char *message, pending_t *consume)
{
    return rpc_send_bounded(id, id_size, object, object_size, error, message, consume, 0);
}

static xiaotai_err_t check_reply_json(const char *object, size_t *size)
{
    *size = 0;
    if (!object) return XIAOTAI_OK;
    while (*size < XIAOTAI_AI_TALK_TOOL_REPLY_MAX && object[*size]) ++*size;
    if (*size == XIAOTAI_AI_TALK_TOOL_REPLY_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    const size_t first = strspn(object, " \t\r\n");
    if (first == *size || object[first] != '{') return XIAOTAI_ERR_INVALID_ARG;
    size_t tokens = 0;
    const xiaotai_err_t err = xiaotai_json_parse(object, *size, NULL, 0, &tokens, NULL);
    return err == XIAOTAI_OK ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
}

static bool error_word(const char *message)
{
    if (!message || !*message) return false;
    for (size_t i = 0; i < XIAOTAI_AI_TALK_TOOL_NAME_MAX; ++i) {
        const char c = message[i];
        if (!c) return true;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_')
            return false;
    }
    return false;
}

static bool error_description(const char *message)
{
    if (!message || !*message) return false;
    for (size_t i = 0; i <= 128U; ++i) {
        const unsigned char c = (unsigned char)message[i];
        if (!c) return true;
        if (c < 0x20U || c == 0x7fU) return false;
    }
    return false;
}

static xiaotai_err_t reply(uint32_t session, uint32_t request, const char *object, int32_t code, const char *message,
                           bool described, size_t max_bytes)
{
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (!request) return XIAOTAI_OK;
    uint32_t current, link;
    if (ai_talk_current(&current, &link) && current == session && link) {
        for (size_t i = 0; i < PENDING_MAX; ++i) {
            pending_t *entry = &s_tools.pending[i];
            if (entry->handle != request || entry->session != session || entry->link != link) continue;
            if ((message && code >= 0) || (!message && code) ||
                (message && !(described ? error_description(message) : error_word(message))))
                return XIAOTAI_ERR_INVALID_ARG;
            size_t size;
            const xiaotai_err_t err = check_reply_json(object, &size);
            if (err != XIAOTAI_OK) return err;
            return rpc_send_bounded(pending_id(entry), entry->id_size, object, size, code, message, entry, max_bytes);
        }
    }
    ai_talk_count_stale_reply();
    return XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t xiaotai_ai_talk_tool_reply(uint32_t session, uint32_t request, const char *result_json)
{
    return reply(session, request, result_json ? result_json : "{}", 0, NULL, false, 0);
}

xiaotai_err_t xiaotai_ai_talk_tool_fail(uint32_t session, uint32_t request, int32_t code, const char *message,
                                       const char *data_json)
{
    return reply(session, request, data_json, code, message ? message : "", false, 0);
}

xiaotai_err_t xiaotai_ai_talk_tool_respond(uint32_t session, uint32_t request,
                                          const xiaotai_ai_talk_tool_response_t *response)
{
    if (!response) return XIAOTAI_ERR_INVALID_ARG;
    return reply(session, request, response->code ? response->object : response->object ? response->object : "{}",
                 response->code, response->message, true, response->max_bytes);
}

typedef struct {
    const tool_t *tool;
    const xiaotai_ai_talk_tool_call_t *call;
    bool matched;
} invocation_t;

static xiaotai_err_t invoke_tool(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    const invocation_t *invocation = reply->ctx;
    ai_talk_count_tool(invocation->matched);
    invocation->tool->handler(invocation->call, invocation->tool->ctx);
    return XIAOTAI_OK;
}

void ai_talk_tools_dispatch(const ai_talk_message_t *message, bool device_action)
{
    uint32_t session, link;
    if (!ai_talk_current(&session, &link) || !link) return;
    size_t id_size = 0;
    const char *id = device_action ? wire_id(&message->id, &id_size) : NULL;
    const xiaotai_json_view_t arg = argument_object(&message->params);
    const bool has_arguments = arg.type == XIAOTAI_JSON_OBJECT;
    char name[XIAOTAI_AI_TALK_TOOL_NAME_MAX] = {0};
    const bool named = device_action ?
        (action_name(&message->params, name) || action_name(&arg, name)) :
        (xiaotai_json_view_string(&message->method, name, sizeof(name), NULL) == XIAOTAI_OK && name[0]);
    const tool_t *tool = named ? lookup(name) : NULL;
    const bool matched = tool != NULL;
    if (!tool && device_action && id && s_tools.fallback.handler) tool = &s_tools.fallback;
    if (!tool) {
        ai_talk_count_tool(false);
        if (id) {
            const bool invalid = !named;
            (void)rpc_send(id, id_size, NULL, 0, invalid ? XIAOTAI_AI_TALK_ERROR_INVALID_PARAMS :
                           XIAOTAI_AI_TALK_ERROR_UNSUPPORTED, invalid ? "invalid_params" : "unsupported_action", NULL);
        }
        return;
    }
    if (id) {
        for (size_t i = 0; i < PENDING_MAX; ++i) {
            const pending_t *active = &s_tools.pending[i];
            if (active->handle && active->session == session && active->link == link &&
                active->id_size == id_size && memcmp(pending_id(active), id, id_size) == 0) return;
        }
    }
    size_t size = 2;
    const char *arguments = "{}";
    char *owned = NULL;
    if (matched && has_arguments) {
        size = arg.length;
        owned = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, size + 1U);
        if (owned) {
            memcpy(owned, arg.text, size);
            owned[size] = '\0';
            arguments = owned;
        }
    }
    pending_t *entry = id && (!matched || !has_arguments || owned) ? reserve(session, link, id, id_size) : NULL;
    if ((matched && has_arguments && !owned) || (id && !entry)) {
        xiaotai_mem_free(owned);
        ai_talk_count_tool(false);
        if (id) (void)rpc_send(id, id_size, NULL, 0, XIAOTAI_AI_TALK_ERROR_FAILED, "busy", NULL);
        return;
    }
    size_t id_text_bytes = 0;
    if (id && message->id.type == XIAOTAI_JSON_STRING) {
        const xiaotai_err_t err = xiaotai_json_view_string(&message->id, NULL, 0, &id_text_bytes);
        if (err != XIAOTAI_OK && err != XIAOTAI_ERR_INVALID_SIZE) id_text_bytes = SIZE_MAX;
    }
    const xiaotai_ai_talk_tool_call_t call = {
        .session = session, .request = entry ? entry->handle : 0, .name = matched ? tool->name : named ? name : "",
        .arguments = arguments, .size = size, .device_action = device_action,
        .has_arguments = has_arguments, .id_text_bytes = id_text_bytes,
    };
    const invocation_t invocation = {.tool = tool, .call = &call, .matched = matched};
    const xiaotai_reply_t context = {.ctx = (void *)&invocation};
    /* A registered application must not inherit the transport/module's
     * privileged scope, including work its handler posts to the owner. */
    if (xiaotai_command_invoke(tool->scope, invoke_tool, 0, NULL, &context) != XIAOTAI_OK) {
        if (entry) discard(entry);
        ai_talk_count_tool(false);
        if (id) (void)rpc_send(id, id_size, NULL, 0, XIAOTAI_AI_TALK_ERROR_FAILED, "unavailable", NULL);
    }
    xiaotai_mem_free(owned);
}
