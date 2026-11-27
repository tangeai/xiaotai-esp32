// SPDX-License-Identifier: MIT
/* Reads of a tool call's argument object (xiaotai/modules/ai_talk.h, tools):
 * strict about meaning, lenient about the spelling a language model picks for
 * a number, a boolean or an enumeration word. Pure functions of the call. */
#include <string.h>
#include "xiaotai/json.h"
#include "xiaotai/modules/ai_talk.h"

/* The object, and a key and a value per member. A nested value needs more
 * tokens and is refused with the whole object: no tool takes one. */
#define TOKENS (1U + 2U * XIAOTAI_AI_TALK_TOOL_ARGS_MAX)
#define NUMBER_BYTES 24U
#define TEXT_BYTES 256U

typedef struct {
    xiaotai_json_token_t tokens[TOKENS];
    xiaotai_json_doc_t doc;
    int index;
} argument_t;

static xiaotai_err_t find(const xiaotai_ai_talk_tool_call_t *call, const char *name, argument_t *arg)
{
    if (!call || !call->arguments || !name) return XIAOTAI_ERR_INVALID_ARG;
    size_t count = 0;
    if (xiaotai_json_parse(call->arguments, call->size, arg->tokens, TOKENS, &count, &arg->doc) != XIAOTAI_OK)
        return XIAOTAI_ERR_INVALID_ARG;
    arg->index = xiaotai_json_get(&arg->doc, 0, name);
    if (arg->index < 0 || arg->tokens[arg->index].type == XIAOTAI_JSON_NULL) return XIAOTAI_ERR_NOT_FOUND;
    return XIAOTAI_OK;
}

static void trim(char *text)
{
    size_t begin = 0, end = strlen(text);
    while (text[begin] == ' ') ++begin;
    while (end > begin && text[end - 1U] == ' ') --end;
    memmove(text, text + begin, end - begin);
    text[end - begin] = 0;
}

/* The value's text without surrounding spaces; false when it is not a string,
 * a number or a boolean, or does not fit. */
static bool scalar(const argument_t *arg, char *out, size_t capacity)
{
    const xiaotai_json_token_t *token = &arg->tokens[arg->index];
    if (token->type == XIAOTAI_JSON_STRING) {
        if (xiaotai_json_string(&arg->doc, arg->index, out, capacity, NULL) != XIAOTAI_OK) return false;
    } else if (token->type == XIAOTAI_JSON_NUMBER || token->type == XIAOTAI_JSON_TRUE ||
               token->type == XIAOTAI_JSON_FALSE) {
        size_t length = 0;
        const char *raw = xiaotai_json_raw(&arg->doc, arg->index, &length);
        if (!raw || length >= capacity) return false;
        memcpy(out, raw, length);
        out[length] = 0;
    } else {
        return false;
    }
    trim(out);
    return true;
}

static void lower(char *text)
{
    for (; *text; ++text) {
        if (*text >= 'A' && *text <= 'Z') *text = (char)(*text + ('a' - 'A'));
    }
}

xiaotai_err_t xiaotai_ai_talk_tool_arg_int(const xiaotai_ai_talk_tool_call_t *call, const char *name, int32_t *value)
{
    argument_t arg;
    const xiaotai_err_t err = find(call, name, &arg);
    if (err != XIAOTAI_OK) return err;
    char text[NUMBER_BYTES];
    if (!value || !scalar(&arg, text, sizeof(text))) return XIAOTAI_ERR_INVALID_ARG;
    if (!text[0]) return XIAOTAI_ERR_NOT_FOUND;
    const char *p = text;
    const bool negative = *p == '-';
    if (*p == '-' || *p == '+') ++p;
    if (*p < '0' || *p > '9') return XIAOTAI_ERR_INVALID_ARG;
    int64_t number = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        number = number * 10 + (*p - '0');
        if (number > (int64_t)INT32_MAX + 1) return XIAOTAI_ERR_INVALID_ARG;
    }
    if (*p == '.') {
        for (++p; *p == '0'; ++p) {}
    }
    if (*p == '%' && !p[1]) ++p;
    if (*p) return XIAOTAI_ERR_INVALID_ARG;
    if (negative) number = -number;
    if (number > INT32_MAX || number < INT32_MIN) return XIAOTAI_ERR_INVALID_ARG;
    *value = (int32_t)number;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_tool_arg_bool(const xiaotai_ai_talk_tool_call_t *call, const char *name, bool *value)
{
    argument_t arg;
    const xiaotai_err_t err = find(call, name, &arg);
    if (err != XIAOTAI_OK) return err;
    char text[8];
    if (!value || !scalar(&arg, text, sizeof(text))) return XIAOTAI_ERR_INVALID_ARG;
    if (!text[0]) return XIAOTAI_ERR_NOT_FOUND;
    lower(text);
    if (!strcmp(text, "true") || !strcmp(text, "1")) *value = true;
    else if (!strcmp(text, "false") || !strcmp(text, "0")) *value = false;
    else return XIAOTAI_ERR_INVALID_ARG;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_tool_arg_word(const xiaotai_ai_talk_tool_call_t *call, const char *name,
                                            char *out, size_t capacity)
{
    if (!out || !capacity) return XIAOTAI_ERR_INVALID_ARG;
    out[0] = 0;
    argument_t arg;
    const xiaotai_err_t err = find(call, name, &arg);
    if (err != XIAOTAI_OK) return err;
    const uint8_t type = arg.tokens[arg.index].type;
    if (type != XIAOTAI_JSON_STRING && type != XIAOTAI_JSON_NUMBER && type != XIAOTAI_JSON_TRUE &&
        type != XIAOTAI_JSON_FALSE) return XIAOTAI_ERR_INVALID_ARG;
    if (!scalar(&arg, out, capacity)) {
        out[0] = 0;
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    lower(out);
    return out[0] ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_ai_talk_tool_arg_text(const xiaotai_ai_talk_tool_call_t *call, const char *name,
                                            char *out, size_t capacity)
{
    if (!out || !capacity) return XIAOTAI_ERR_INVALID_ARG;
    out[0] = 0;
    argument_t arg;
    const xiaotai_err_t err = find(call, name, &arg);
    if (err != XIAOTAI_OK) return err;
    const xiaotai_json_token_t *token = &arg.tokens[arg.index];
    /* Decoded text is never longer than its escaped spelling. */
    char whole[TEXT_BYTES];
    if (token->type != XIAOTAI_JSON_STRING || token->end - token->start >= sizeof(whole) ||
        xiaotai_json_string(&arg.doc, arg.index, whole, sizeof(whole), NULL) != XIAOTAI_OK)
        return XIAOTAI_ERR_INVALID_ARG;
    trim(whole);
    size_t length = strlen(whole);
    if (length > capacity - 1U) {
        length = capacity - 1U;
        while (length && ((unsigned char)whole[length] & 0xC0U) == 0x80U) --length;
    }
    memcpy(out, whole, length);
    out[length] = 0;
    return length ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}
