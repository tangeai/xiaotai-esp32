// SPDX-License-Identifier: MIT
/* Strict reads of replies and signaling envelopes, without allocation and
 * without a token table, so the signaling task can judge an 8 KiB message on
 * a small stack. The JSON library validates the whole text (its counting
 * mode: strict syntax, nesting at most XIAOTAI_JSON_DEPTH_MAX); only then
 * does this file walk the top-level members of the validated object, and it
 * hands every key and value it reads back to the library with a one-token
 * table, so escapes and numbers are decoded by one implementation.
 *
 * Framing follows deployed firmware's strict parse (cJSON with a required
 * terminator after a NUL check): a NUL byte anywhere refuses the text, a
 * UTF-8 byte-order mark is skipped in texts of 4 bytes or more, and bytes
 * 0x01..0x20 before and after the value count as blanks. Inside the value the
 * library's rules apply (its specification lists where they are stricter
 * than cJSON). */
#include <string.h>
#include "xiaotai/json.h"
#include "platform_internal.h"

typedef struct {
    const char *text;
    size_t length;
} span_text_t;

/* The validated top-level object of json[0, size), or false. */
static bool object_text(const char *json, size_t size, span_text_t *out)
{
    if (!json || size == 0 || memchr(json, '\0', size) != NULL) {
        return false;
    }
    size_t start = 0;
    size_t end = size;
    if (size >= 4 && memcmp(json, "\xEF\xBB\xBF", 3) == 0) {
        start = 3;
    }
    while (start < end && (unsigned char)json[start] <= 0x20U) {
        ++start;
    }
    while (end > start && (unsigned char)json[end - 1] <= 0x20U) {
        --end;
    }
    if (start == end || json[start] != '{') {
        return false;
    }
    size_t count = 0;
    if (xiaotai_json_parse(json + start, end - start, NULL, 0, &count, NULL) != XIAOTAI_OK) {
        return false;
    }
    out->text = json + start;
    out->length = end - start;
    return true;
}

/* The walkers below run on validated JSON only, so they need no error
 * handling beyond staying inside the text. */
static size_t skip_blank(const char *t, size_t n, size_t i)
{
    while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) {
        ++i;
    }
    return i;
}

/* i at an opening quote; returns the index after the closing quote. */
static size_t skip_string(const char *t, size_t n, size_t i)
{
    ++i;
    while (i < n && t[i] != '"') {
        i += t[i] == '\\' ? 2U : 1U;
    }
    return i < n ? i + 1 : n;
}

static size_t skip_value(const char *t, size_t n, size_t i)
{
    if (i >= n) {
        return n;
    }
    if (t[i] == '"') {
        return skip_string(t, n, i);
    }
    if (t[i] == '{' || t[i] == '[') {
        size_t depth = 0;
        while (i < n) {
            char c = t[i];
            if (c == '"') {
                i = skip_string(t, n, i);
                continue;
            }
            if (c == '{' || c == '[') {
                ++depth;
            } else if ((c == '}' || c == ']') && --depth == 0) {
                return i + 1;
            }
            ++i;
        }
        return n;
    }
    while (i < n && t[i] != ',' && t[i] != '}' && t[i] != ']' && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' &&
           t[i] != '\r') {
        ++i;
    }
    return i;
}

typedef struct {
    size_t key;        /* index of the key's opening quote */
    size_t key_end;    /* index after its closing quote */
    size_t value;
    size_t value_end;
} member_t;

/* Next top-level member at *cursor (initially 0, the object's '{'). */
static bool next_member(const span_text_t *object, size_t *cursor, member_t *member)
{
    const char *t = object->text;
    size_t n = object->length;
    size_t i = *cursor;
    if (i == 0) {
        i = skip_blank(t, n, 1);
    } else {
        i = skip_blank(t, n, i);
        if (i >= n || t[i] != ',') {
            return false;
        }
        i = skip_blank(t, n, i + 1);
    }
    if (i >= n || t[i] != '"') {
        return false;
    }
    member->key = i;
    member->key_end = skip_string(t, n, i);
    i = skip_blank(t, n, member->key_end);
    i = skip_blank(t, n, i + 1); /* past ':' */
    member->value = i;
    member->value_end = skip_value(t, n, i);
    *cursor = member->value_end;
    return true;
}

/* One value of the text as a document of its own (a one-token table): a
 * string with its quotes, or a number. False for containers. */
static bool single(const char *text, size_t length, xiaotai_json_token_t *token, xiaotai_json_doc_t *doc)
{
    size_t count = 0;
    return xiaotai_json_parse(text, length, token, 1, &count, doc) == XIAOTAI_OK && count == 1;
}

static bool key_is(const span_text_t *object, const member_t *member, const char *name)
{
    xiaotai_json_token_t token;
    xiaotai_json_doc_t doc;
    return single(object->text + member->key, member->key_end - member->key, &token, &doc) &&
           xiaotai_json_equals(&doc, 0, name);
}

/* A string value that decodes; its raw span relative to json. */
static bool string_span(const char *json, const span_text_t *object, const member_t *member,
                        platform_json_span_t *span)
{
    const char *text = object->text + member->value;
    size_t length = member->value_end - member->value;
    xiaotai_json_token_t token;
    xiaotai_json_doc_t doc;
    if (length < 2 || text[0] != '"' || !single(text, length, &token, &doc)) {
        return false;
    }
    size_t decoded = 0;
    if (xiaotai_json_string(&doc, 0, NULL, 0, &decoded) != XIAOTAI_ERR_INVALID_SIZE) {
        return false; /* refused escape: a lone surrogate or \u0000 */
    }
    span->offset = (uint32_t)(text + 1 - json);
    span->length = (uint32_t)(length - 2);
    return true;
}

xiaotai_err_t xiaotai_platform_json_code(const char *json, size_t size, int32_t *code)
{
    if (!code) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    span_text_t object;
    if (!object_text(json, size, &object)) {
        return XIAOTAI_ERR_NOT_FOUND;
    }
    size_t cursor = 0;
    member_t member;
    unsigned found = 0;
    int64_t value = 0;
    bool integral = false;
    while (next_member(&object, &cursor, &member)) {
        if (!key_is(&object, &member, "code")) {
            continue;
        }
        if (++found > 1) {
            return XIAOTAI_ERR_NOT_FOUND;
        }
        xiaotai_json_token_t token;
        xiaotai_json_doc_t doc;
        integral = single(object.text + member.value, member.value_end - member.value, &token, &doc) &&
                   xiaotai_json_int(&doc, 0, &value) == XIAOTAI_OK;
    }
    /* INT32_MIN stays XIAOTAI_PLATFORM_NO_CODE, never a code. */
    if (found != 1 || !integral || value <= (int64_t)INT32_MIN || value > (int64_t)INT32_MAX) {
        return XIAOTAI_ERR_NOT_FOUND;
    }
    *code = (int32_t)value;
    return XIAOTAI_OK;
}

xiaotai_err_t platform_json_envelope(const char *json, size_t size, platform_envelope_t *envelope)
{
    span_text_t object;
    if (!envelope || !object_text(json, size, &object)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    platform_envelope_t found = {0};
    bool have_type = false;
    bool seen_type = false;
    bool seen_channel = false;
    size_t cursor = 0;
    member_t member;
    while (next_member(&object, &cursor, &member)) {
        /* The first member of a name counts, as a lookup by name finds it. */
        if (!seen_type && key_is(&object, &member, "type")) {
            seen_type = true;
            have_type = string_span(json, &object, &member, &found.type);
        } else if (!seen_channel && key_is(&object, &member, "channel")) {
            seen_channel = true;
            const char *text = object.text + member.value;
            if (text[0] == '"') {
                /* A channel string that does not decode cannot be handed
                 * over, and the text it came in is refused as a whole. */
                if (!string_span(json, &object, &member, &found.channel)) {
                    return XIAOTAI_ERR_INVALID_ARG;
                }
                found.has_channel = true;
            }
        }
    }
    if (!have_type) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    *envelope = found;
    return XIAOTAI_OK;
}

static bool span_doc(const char *json, platform_json_span_t span, xiaotai_json_token_t *token,
                     xiaotai_json_doc_t *doc)
{
    if (!json || span.offset == 0 || json[span.offset - 1] != '"' || json[span.offset + span.length] != '"') {
        return false;
    }
    return single(json + span.offset - 1, (size_t)span.length + 2, token, doc);
}

xiaotai_err_t platform_json_span_text(const char *json, platform_json_span_t span, char *out, size_t capacity,
                                      size_t *length)
{
    xiaotai_json_token_t token;
    xiaotai_json_doc_t doc;
    if (!span_doc(json, span, &token, &doc)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    return xiaotai_json_string(&doc, 0, out, out ? capacity : 0, length);
}

bool platform_json_span_equals(const char *json, platform_json_span_t span, const char *text)
{
    xiaotai_json_token_t token;
    xiaotai_json_doc_t doc;
    return text && span_doc(json, span, &token, &doc) && xiaotai_json_equals(&doc, 0, text);
}

xiaotai_err_t platform_json_members(const char *json, size_t size, platform_json_member_fn visit, void *ctx)
{
    span_text_t object;
    if (!visit || !object_text(json, size, &object)) return XIAOTAI_ERR_INVALID_ARG;
    size_t cursor = 0;
    member_t m;
    while (next_member(&object, &cursor, &m)) {
        xiaotai_err_t err = visit(object.text + m.key, m.key_end - m.key,
            object.text + m.value, m.value_end - m.value, ctx);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t platform_json_field(const char *json, size_t size, const char *name, const char **value, size_t *length)
{
    span_text_t object;
    if (!object_text(json, size, &object)) return XIAOTAI_ERR_NOT_FOUND;
    size_t cursor = 0;
    member_t m;
    bool found = false;
    while (next_member(&object, &cursor, &m)) {
        if (!key_is(&object, &m, name)) continue;
        if (found) return XIAOTAI_ERR_NOT_FOUND;
        found = true;
        *value = object.text + m.value;
        *length = m.value_end - m.value;
    }
    return found ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t platform_json_text_read(const char *json, size_t size, char *out, size_t capacity)
{
    xiaotai_json_token_t token;
    xiaotai_json_doc_t doc;
    if (!single(json, size, &token, &doc)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_json_string(&doc, 0, out, capacity, NULL);
}

xiaotai_err_t platform_json_integer_read(const char *json, size_t size, int64_t *out)
{
    xiaotai_json_token_t token;
    xiaotai_json_doc_t doc;
    if (!single(json, size, &token, &doc)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_json_int(&doc, 0, out);
}
