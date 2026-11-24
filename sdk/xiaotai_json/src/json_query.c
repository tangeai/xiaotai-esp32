// SPDX-License-Identifier: MIT
/* Queries and strict reads over a token table from xiaotai_json_parse().
 * Tokens are in text order and a token's subtree is the run of tokens after it
 * that start before its end, so siblings are found by skipping that run: no
 * sibling links, and every walk is linear in the tokens it passes. */
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "json_internal.h"
#include "xiaotai/json_view.h"

/* ------------------------------------------------------- string decoding */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Four hex digits of a \u escape whose 'u' is at raw[at - 1]. */
static bool hex4(const char *raw, size_t length, size_t at, uint32_t *value)
{
    if (at + 4 > length) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = at; i < at + 4; ++i) {
        int digit = hex_value(raw[i]);
        if (digit < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)digit;
    }
    *value = v;
    return true;
}

static size_t utf8_encode(uint32_t cp, char unit[4])
{
    if (cp < 0x80) {
        unit[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        unit[0] = (char)(0xC0 | (cp >> 6));
        unit[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        unit[0] = (char)(0xE0 | (cp >> 12));
        unit[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        unit[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    unit[0] = (char)(0xF0 | (cp >> 18));
    unit[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    unit[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    unit[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decodes the byte or escape at raw[*pos] into unit and advances *pos.
 * Returns the number of bytes in unit (1..4), 0 for a refused escape. Raw
 * bytes other than escapes pass unchanged: the tokenizer already refused
 * control characters, and UTF-8 is not re-validated (no reader in the
 * existing firmware did; a caller that renders text checks it). */
static size_t decode_unit(const char *raw, size_t length, size_t *pos, char unit[4])
{
    size_t i = *pos;
    if (raw[i] != '\\') {
        unit[0] = raw[i];
        *pos = i + 1;
        return 1;
    }
    if (i + 1 >= length) {
        return 0;
    }
    char simple;
    switch (raw[i + 1]) {
    case '"':
        simple = '"';
        break;
    case '\\':
        simple = '\\';
        break;
    case '/':
        simple = '/';
        break;
    case 'b':
        simple = '\b';
        break;
    case 'f':
        simple = '\f';
        break;
    case 'n':
        simple = '\n';
        break;
    case 'r':
        simple = '\r';
        break;
    case 't':
        simple = '\t';
        break;
    case 'u': {
        uint32_t cp;
        if (!hex4(raw, length, i + 2, &cp)) {
            return 0;
        }
        size_t next = i + 6;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            /* A high surrogate must be followed by an escaped low one. */
            uint32_t low;
            if (next + 2 > length || raw[next] != '\\' || raw[next + 1] != 'u' ||
                !hex4(raw, length, next + 2, &low) || low < 0xDC00 || low > 0xDFFF) {
                return 0;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            next += 6;
        } else if ((cp >= 0xDC00 && cp <= 0xDFFF) || cp == 0) {
            /* A lone low surrogate has no character; NUL would cut a C string. */
            return 0;
        }
        *pos = next;
        return utf8_encode(cp, unit);
    }
    default:
        return 0;
    }
    unit[0] = simple;
    *pos = i + 2;
    return 1;
}

xiaotai_err_t xiaotai_json_decode(const char *raw, size_t raw_length, char *out, size_t capacity, size_t *length)
{
    /* First pass measures and validates, so a refused escape writes nothing. */
    size_t decoded = 0;
    for (size_t pos = 0; pos < raw_length;) {
        char unit[4];
        size_t n = decode_unit(raw, raw_length, &pos, unit);
        if (n == 0) {
            return XIAOTAI_ERR_INVALID_ARG;
        }
        decoded += n;
    }
    if (length) {
        *length = decoded;
    }
    if (!out || decoded >= capacity) {
        if (out && capacity > 0) {
            out[0] = '\0';
        }
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    size_t written = 0;
    for (size_t pos = 0; pos < raw_length;) {
        char unit[4];
        size_t n = decode_unit(raw, raw_length, &pos, unit);
        memcpy(out + written, unit, n);
        written += n;
    }
    out[written] = '\0';
    return XIAOTAI_OK;
}

bool xiaotai_json_raw_equals(const char *raw, size_t raw_length, const char *text, size_t length)
{
    size_t matched = 0;
    for (size_t pos = 0; pos < raw_length;) {
        char unit[4];
        size_t n = decode_unit(raw, raw_length, &pos, unit);
        if (n == 0 || n > length - matched || memcmp(text + matched, unit, n) != 0) {
            return false;
        }
        matched += n;
    }
    return matched == length;
}

/* ---------------------------------------------------------------- paths */

int xiaotai_json_path_next(const char **cursor, xiaotai_json_segment_t *segment)
{
    const char *p = *cursor;
    if (*p == '\0') {
        return 0;
    }
    if (*p == '[') {
        ++p;
        if (*p < '0' || *p > '9') {
            return -1;
        }
        size_t position = 0;
        while (*p >= '0' && *p <= '9') {
            size_t digit = (size_t)(*p - '0');
            if (position > (SIZE_MAX - digit) / 10) {
                return -1;
            }
            position = position * 10 + digit;
            ++p;
        }
        if (*p != ']') {
            return -1;
        }
        ++p;
        /* After a position: another position, ".name", or the end. */
        if (*p == '.') {
            ++p;
            if (*p == '\0' || *p == '.' || *p == '[') {
                return -1;
            }
        } else if (*p != '\0' && *p != '[') {
            return -1;
        }
        segment->key = NULL;
        segment->key_length = 0;
        segment->position = position;
        segment->is_position = true;
        *cursor = p;
        return 1;
    }
    const char *key = p;
    while (*p != '\0' && *p != '.' && *p != '[') {
        ++p;
    }
    if (p == key) {
        return -1;
    }
    segment->key = key;
    segment->key_length = (size_t)(p - key);
    segment->position = 0;
    segment->is_position = false;
    if (*p == '.') {
        ++p;
        if (*p == '\0' || *p == '.' || *p == '[') {
            return -1;
        }
    }
    *cursor = p;
    return 1;
}

/* -------------------------------------------------------------- queries */

static bool valid_index(const xiaotai_json_doc_t *doc, int index)
{
    return doc && doc->tokens && index >= 0 && (size_t)index < doc->count;
}

/* Index of the first token after the subtree of token i. */
static size_t skip(const xiaotai_json_doc_t *doc, size_t i)
{
    uint32_t end = doc->tokens[i].end;
    size_t j = i + 1;
    while (j < doc->count && doc->tokens[j].start < end) {
        ++j;
    }
    return j;
}

/* True while token j is inside container c. */
static bool inside(const xiaotai_json_doc_t *doc, size_t c, size_t j)
{
    return j < doc->count && doc->tokens[j].start < doc->tokens[c].end;
}

static int get_n(const xiaotai_json_doc_t *doc, int object, const char *key, size_t key_length)
{
    if (!valid_index(doc, object) || doc->tokens[object].type != XIAOTAI_JSON_OBJECT) {
        return -1;
    }
    size_t c = (size_t)object;
    size_t k = c + 1;
    while (inside(doc, c, k) && k + 1 < doc->count) {
        const xiaotai_json_token_t *name = &doc->tokens[k];
        if (xiaotai_json_raw_equals(doc->text + name->start, name->end - name->start, key, key_length)) {
            return (int)(k + 1);
        }
        k = skip(doc, k + 1);
    }
    return -1;
}

int xiaotai_json_get(const xiaotai_json_doc_t *doc, int object, const char *key)
{
    if (!key) {
        return -1;
    }
    return get_n(doc, object, key, strlen(key));
}

int xiaotai_json_at(const xiaotai_json_doc_t *doc, int array, size_t position)
{
    if (!valid_index(doc, array) || doc->tokens[array].type != XIAOTAI_JSON_ARRAY) {
        return -1;
    }
    size_t c = (size_t)array;
    size_t e = c + 1;
    for (size_t i = 0; inside(doc, c, e); ++i) {
        if (i == position) {
            return (int)e;
        }
        e = skip(doc, e);
    }
    return -1;
}

int xiaotai_json_find(const xiaotai_json_doc_t *doc, int from, const char *path)
{
    if (!valid_index(doc, from) || !path) {
        return -1;
    }
    int index = from;
    const char *cursor = path;
    xiaotai_json_segment_t segment;
    int step;
    while ((step = xiaotai_json_path_next(&cursor, &segment)) > 0) {
        index = segment.is_position ? xiaotai_json_at(doc, index, segment.position)
                                    : get_n(doc, index, segment.key, segment.key_length);
        if (index < 0) {
            return -1;
        }
    }
    return step < 0 ? -1 : index;
}

/* --------------------------------------------------------- strict reads */

/* NOT_FOUND for -1 (an absent value from a lookup), INVALID_ARG for any other
 * index that is not in the table. */
static xiaotai_err_t check(const xiaotai_json_doc_t *doc, int index)
{
    if (index == -1) {
        return XIAOTAI_ERR_NOT_FOUND;
    }
    return valid_index(doc, index) ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
}

static xiaotai_err_t read_int(const char *p, size_t length, int64_t *value)
{
    if (!value || !p || !length) return XIAOTAI_ERR_INVALID_ARG;
    const char *end = p + length;
    bool negative = *p == '-';
    if (negative) {
        ++p;
    }
    /* The tokenizer guarantees at least one digit; a fraction or an exponent
     * stops the digits before the end. */
    int64_t magnitude = 0;
    for (; p < end; ++p) {
        if (*p < '0' || *p > '9') {
            return XIAOTAI_ERR_INVALID_ARG;
        }
        magnitude = magnitude * 10 + (*p - '0');
        if (magnitude > XIAOTAI_JSON_INT_MAX) {
            return XIAOTAI_ERR_INVALID_ARG;
        }
    }
    *value = negative ? -magnitude : magnitude;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_json_int(const xiaotai_json_doc_t *doc, int index, int64_t *value)
{
    xiaotai_err_t err = check(doc, index);
    if (err != XIAOTAI_OK) return err;
    const xiaotai_json_token_t *token = &doc->tokens[index];
    if (token->type != XIAOTAI_JSON_NUMBER) return XIAOTAI_ERR_INVALID_ARG;
    return read_int(doc->text + token->start, token->end - token->start, value);
}

static xiaotai_err_t read_bool(xiaotai_json_type_t type, bool *value)
{
    if (!value || (type != XIAOTAI_JSON_TRUE && type != XIAOTAI_JSON_FALSE)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    *value = type == XIAOTAI_JSON_TRUE;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_json_bool(const xiaotai_json_doc_t *doc, int index, bool *value)
{
    xiaotai_err_t err = check(doc, index);
    if (err != XIAOTAI_OK) {
        return err;
    }
    return read_bool(doc->tokens[index].type, value);
}

xiaotai_err_t xiaotai_json_string(const xiaotai_json_doc_t *doc, int index, char *out, size_t capacity,
                                  size_t *length)
{
    xiaotai_err_t err = check(doc, index);
    if (err != XIAOTAI_OK) {
        return err;
    }
    const xiaotai_json_token_t *token = &doc->tokens[index];
    if (token->type != XIAOTAI_JSON_STRING || (!out && capacity > 0)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    return xiaotai_json_decode(doc->text + token->start, token->end - token->start, out, capacity, length);
}

bool xiaotai_json_equals(const xiaotai_json_doc_t *doc, int index, const char *text)
{
    if (!text || !valid_index(doc, index) || doc->tokens[index].type != XIAOTAI_JSON_STRING) {
        return false;
    }
    const xiaotai_json_token_t *token = &doc->tokens[index];
    return xiaotai_json_raw_equals(doc->text + token->start, token->end - token->start, text, strlen(text));
}

const char *xiaotai_json_raw(const xiaotai_json_doc_t *doc, int index, size_t *length)
{
    if (!valid_index(doc, index)) {
        if (length) {
            *length = 0;
        }
        return NULL;
    }
    const xiaotai_json_token_t *token = &doc->tokens[index];
    if (length) {
        *length = token->end - token->start;
    }
    return doc->text + token->start;
}

xiaotai_err_t xiaotai_json_view_int(const xiaotai_json_view_t *view, int64_t *value)
{
    if (!view || !view->type) return XIAOTAI_ERR_NOT_FOUND;
    if (view->type != XIAOTAI_JSON_NUMBER) return XIAOTAI_ERR_INVALID_ARG;
    return read_int(view->text, view->length, value);
}

xiaotai_err_t xiaotai_json_view_bool(const xiaotai_json_view_t *view, bool *value)
{
    if (!view || !view->type) return XIAOTAI_ERR_NOT_FOUND;
    return read_bool(view->type, value);
}

static bool view_is_string(const xiaotai_json_view_t *view)
{
    return view && view->type == XIAOTAI_JSON_STRING && view->text && view->length >= 2;
}

xiaotai_err_t xiaotai_json_view_string(const xiaotai_json_view_t *view, char *out, size_t capacity, size_t *length)
{
    if (!view || !view->type) return XIAOTAI_ERR_NOT_FOUND;
    if (!view_is_string(view) || (!out && capacity > 0)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_json_decode(view->text + 1, view->length - 2, out, capacity, length);
}

bool xiaotai_json_view_equals(const xiaotai_json_view_t *view, const char *text)
{
    return view_is_string(view) && text &&
        xiaotai_json_raw_equals(view->text + 1, view->length - 2, text, strlen(text));
}
