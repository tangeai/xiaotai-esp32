// SPDX-License-Identifier: MIT
/* Tokenizer (xiaotai_json_parse) and table-free array counting
 * (xiaotai_json_count). One forward pass over the text with an explicit stack of
 * XIAOTAI_JSON_DEPTH_MAX entries (128 bytes): no recursion, so the stack use
 * does not grow with the nesting of untrusted input, and no allocation. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "json_internal.h"
#include "xiaotai/json_view.h"

typedef enum {
    EXPECT_VALUE,
    EXPECT_KEY,
    AFTER_VALUE,
} parse_state_t;

typedef struct {
    const char *text;
    size_t length;
    size_t pos;
    xiaotai_json_token_t *tokens;
    size_t capacity;
    size_t count;                                /* tokens seen so far, stored or not */
    int32_t stack[XIAOTAI_JSON_DEPTH_MAX];       /* token index of each open container */
    uint32_t objects;                            /* bit per open level: an object, not an array */
    uint32_t depth;
} parser_t;

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void skip_ws(const char *text, size_t length, size_t *pos)
{
    while (*pos < length && is_ws(text[*pos])) {
        ++*pos;
    }
}

/* Appends a token. Every token is counted; only those within the capacity are
 * stored. Returns the token's index (stored or not). */
static int32_t emit(parser_t *p, uint8_t type, size_t start, size_t end, int32_t parent)
{
    size_t index = p->count++;
    if (index < p->capacity) {
        xiaotai_json_token_t *token = &p->tokens[index];
        token->type = type;
        token->reserved = 0;
        token->children = 0;
        token->parent = parent;
        token->start = (uint32_t)start;
        token->end = (uint32_t)end;
    }
    return (int32_t)index;
}

static void add_child(parser_t *p, int32_t container)
{
    if ((size_t)container < p->capacity && p->tokens[container].children < UINT16_MAX) {
        ++p->tokens[container].children;
    }
}

static bool top_is_object(const parser_t *p)
{
    return (p->objects >> (p->depth - 1U)) & 1U;
}

/* String at p->pos (the opening quote). On success p->pos is after the closing
 * quote and *start, *end delimit the raw bytes between the quotes. */
static bool scan_string(parser_t *p, size_t *start, size_t *end)
{
    size_t i = p->pos + 1;
    while (i < p->length) {
        unsigned char c = (unsigned char)p->text[i];
        if (c == '"') {
            *start = p->pos + 1;
            *end = i;
            p->pos = i + 1;
            return true;
        }
        if (c < 0x20) {
            return false;
        }
        if (c != '\\') {
            ++i;
            continue;
        }
        if (i + 1 >= p->length) {
            return false;
        }
        char escape = p->text[i + 1];
        if (escape == 'u') {
            if (i + 6 > p->length) {
                return false;
            }
            for (size_t k = i + 2; k < i + 6; ++k) {
                if (!is_hex(p->text[k])) {
                    return false;
                }
            }
            i += 6;
        } else if (escape != '\0' && strchr("\"\\/bfnrt", escape)) {
            i += 2;
        } else {
            return false;
        }
    }
    return false;
}

/* Number at p->pos as RFC 8259 writes it: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)? */
static bool scan_number(parser_t *p)
{
    const char *t = p->text;
    size_t n = p->length;
    size_t i = p->pos;
    if (i < n && t[i] == '-') {
        ++i;
    }
    if (i >= n || !is_digit(t[i])) {
        return false;
    }
    if (t[i] == '0') {
        ++i;
    } else {
        while (i < n && is_digit(t[i])) {
            ++i;
        }
    }
    if (i < n && t[i] == '.') {
        ++i;
        if (i >= n || !is_digit(t[i])) {
            return false;
        }
        while (i < n && is_digit(t[i])) {
            ++i;
        }
    }
    if (i < n && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < n && (t[i] == '+' || t[i] == '-')) {
            ++i;
        }
        if (i >= n || !is_digit(t[i])) {
            return false;
        }
        while (i < n && is_digit(t[i])) {
            ++i;
        }
    }
    p->pos = i;
    return true;
}

static bool scan_literal(parser_t *p, const char *word)
{
    size_t length = strlen(word);
    if (p->length - p->pos < length || memcmp(p->text + p->pos, word, length) != 0) {
        return false;
    }
    p->pos += length;
    return true;
}

static xiaotai_err_t run(parser_t *p)
{
    parse_state_t state = EXPECT_VALUE;
    int32_t pending_key = -1;
    for (;;) {
        skip_ws(p->text, p->length, &p->pos);
        if (state == AFTER_VALUE && p->depth == 0) {
            /* One top-level value and nothing but whitespace after it. */
            return p->pos == p->length ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
        }
        if (p->pos >= p->length) {
            return XIAOTAI_ERR_INVALID_ARG;
        }
        char c = p->text[p->pos];
        if (state == AFTER_VALUE) {
            bool object = top_is_object(p);
            if (c == ',') {
                ++p->pos;
                state = object ? EXPECT_KEY : EXPECT_VALUE;
                continue;
            }
            if (c != (object ? '}' : ']')) {
                return XIAOTAI_ERR_INVALID_ARG;
            }
            int32_t container = p->stack[--p->depth];
            if ((size_t)container < p->capacity) {
                p->tokens[container].end = (uint32_t)(p->pos + 1);
            }
            ++p->pos;
            continue;
        }
        if (state == EXPECT_KEY) {
            size_t start, end;
            if (c != '"' || !scan_string(p, &start, &end)) {
                return XIAOTAI_ERR_INVALID_ARG;
            }
            int32_t object = p->stack[p->depth - 1U];
            pending_key = emit(p, XIAOTAI_JSON_STRING, start, end, object);
            add_child(p, object);
            skip_ws(p->text, p->length, &p->pos);
            if (p->pos >= p->length || p->text[p->pos] != ':') {
                return XIAOTAI_ERR_INVALID_ARG;
            }
            ++p->pos;
            state = EXPECT_VALUE;
            continue;
        }

        /* EXPECT_VALUE: a value's parent is its key inside an object, the
         * array inside an array, and -1 at the top level. */
        int32_t parent = -1;
        if (p->depth > 0) {
            if (top_is_object(p)) {
                parent = pending_key;
            } else {
                parent = p->stack[p->depth - 1U];
                add_child(p, parent);
            }
        }
        size_t start = p->pos;
        if (c == '{' || c == '[') {
            if (p->depth >= XIAOTAI_JSON_DEPTH_MAX) {
                return XIAOTAI_ERR_INVALID_SIZE;
            }
            bool object = c == '{';
            int32_t index = emit(p, object ? XIAOTAI_JSON_OBJECT : XIAOTAI_JSON_ARRAY, start, start, parent);
            p->stack[p->depth] = index;
            if (object) {
                p->objects |= 1U << p->depth;
            } else {
                p->objects &= ~(1U << p->depth);
            }
            ++p->depth;
            ++p->pos;
            skip_ws(p->text, p->length, &p->pos);
            if (p->pos < p->length && p->text[p->pos] == (object ? '}' : ']')) {
                /* Empty container: close it here, so a trailing comma can
                 * never be mistaken for one. */
                state = AFTER_VALUE;
                continue;
            }
            state = object ? EXPECT_KEY : EXPECT_VALUE;
            continue;
        }
        if (c == '"') {
            size_t raw_start, raw_end;
            if (!scan_string(p, &raw_start, &raw_end)) {
                return XIAOTAI_ERR_INVALID_ARG;
            }
            emit(p, XIAOTAI_JSON_STRING, raw_start, raw_end, parent);
        } else if (c == '-' || is_digit(c)) {
            if (!scan_number(p)) {
                return XIAOTAI_ERR_INVALID_ARG;
            }
            emit(p, XIAOTAI_JSON_NUMBER, start, p->pos, parent);
        } else if (c == 't' && scan_literal(p, "true")) {
            emit(p, XIAOTAI_JSON_TRUE, start, p->pos, parent);
        } else if (c == 'f' && scan_literal(p, "false")) {
            emit(p, XIAOTAI_JSON_FALSE, start, p->pos, parent);
        } else if (c == 'n' && scan_literal(p, "null")) {
            emit(p, XIAOTAI_JSON_NULL, start, p->pos, parent);
        } else {
            return XIAOTAI_ERR_INVALID_ARG;
        }
        state = AFTER_VALUE;
    }
}

xiaotai_err_t xiaotai_json_parse(const char *text, size_t length, xiaotai_json_token_t *tokens, size_t capacity,
                                 size_t *count, xiaotai_json_doc_t *doc)
{
    if (doc) {
        /* Cleared first, so a failed parse never leaves a usable table behind. */
        doc->text = text;
        doc->length = 0;
        doc->tokens = NULL;
        doc->count = 0;
    }
    if (count) {
        *count = 0;
    }
    if (length > XIAOTAI_JSON_TEXT_MAX) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    if (!text && length > 0) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    parser_t p = {
        .text = text,
        .length = length,
        .tokens = tokens,
        .capacity = tokens ? capacity : 0,
    };
    xiaotai_err_t err = run(&p);
    if (count) {
        *count = p.count;
    }
    if (err != XIAOTAI_OK) {
        return err;
    }
    if (!tokens) {
        return XIAOTAI_OK;
    }
    if (p.count > capacity) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    if (doc) {
        doc->length = length;
        doc->tokens = tokens;
        doc->count = p.count;
    }
    return XIAOTAI_OK;
}

/* ------------------------------------------------- navigation without tokens
 * The helpers below run only on text xiaotai_json_parse() has accepted, so
 * they skip without re-checking the grammar. */

static size_t skip_string(const char *text, size_t length, size_t pos)
{
    /* pos is the opening quote; returns the position after the closing quote. */
    for (++pos; pos < length; ++pos) {
        if (text[pos] == '\\') {
            ++pos;
        } else if (text[pos] == '"') {
            return pos + 1;
        }
    }
    return length;
}

static size_t skip_value(const char *text, size_t length, size_t pos)
{
    if (pos >= length) return length;
    char c = text[pos];
    if (c == '"') {
        return skip_string(text, length, pos);
    }
    if (c != '{' && c != '[') {
        while (pos < length && text[pos] != ',' && text[pos] != ']' && text[pos] != '}' && !is_ws(text[pos])) {
            ++pos;
        }
        return pos;
    }
    size_t depth = 0;
    while (pos < length) {
        c = text[pos];
        if (c == '"') {
            pos = skip_string(text, length, pos);
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (--depth == 0) {
                return pos + 1;
            }
        }
        ++pos;
    }
    return length;
}

/* From the opening bracket of a container to its first item (or its closing
 * bracket); false when it is empty. */
static bool first_item(const char *text, size_t length, size_t *pos)
{
    ++*pos;
    skip_ws(text, length, pos);
    return *pos < length && text[*pos] != '}' && text[*pos] != ']';
}

/* After an item: to the next one; false at the closing bracket. */
static bool next_item(const char *text, size_t length, size_t *pos)
{
    skip_ws(text, length, pos);
    if (*pos >= length || text[*pos] != ',') {
        return false;
    }
    ++*pos;
    skip_ws(text, length, pos);
    return *pos < length;
}

/* The key is quoted; returns its end separately from the value's start. */
static bool take_key(const char *text, size_t length, size_t *pos, size_t *after)
{
    if (*pos >= length || text[*pos] != '"') return false;
    *after = *pos = skip_string(text, length, *pos);
    skip_ws(text, length, pos);
    if (*pos >= length || text[*pos] != ':') return false;
    ++*pos;
    skip_ws(text, length, pos);
    return *pos < length;
}

static bool member(const char *text, size_t length, size_t *pos, const char *key, size_t key_length)
{
    if (text[*pos] != '{' || !first_item(text, length, pos)) {
        return false;
    }
    do {
        size_t raw = *pos + 1;
        size_t after;
        if (!take_key(text, length, pos, &after)) return false;
        if (xiaotai_json_raw_equals(text + raw, after - 1 - raw, key, key_length)) {
            return true; /* the first of duplicate keys, as xiaotai_json_get */
        }
        *pos = skip_value(text, length, *pos);
    } while (next_item(text, length, pos));
    return false;
}

static bool element(const char *text, size_t length, size_t *pos, size_t position)
{
    if (text[*pos] != '[' || !first_item(text, length, pos)) {
        return false;
    }
    for (size_t i = 0; i < position; ++i) {
        *pos = skip_value(text, length, *pos);
        if (!next_item(text, length, pos)) {
            return false;
        }
    }
    return true;
}

static xiaotai_err_t locate(const char *text, size_t length, const char *path, size_t *pos)
{
    const xiaotai_err_t err = xiaotai_json_parse(text, length, NULL, 0, NULL, NULL);
    if (err != XIAOTAI_OK) {
        return err;
    }
    *pos = 0;
    skip_ws(text, length, pos);
    const char *cursor = path ? path : "";
    xiaotai_json_segment_t segment;
    int step;
    while ((step = xiaotai_json_path_next(&cursor, &segment)) > 0) {
        bool found = segment.is_position ? element(text, length, pos, segment.position)
                                         : member(text, length, pos, segment.key, segment.key_length);
        if (!found) {
            return XIAOTAI_ERR_NOT_FOUND;
        }
    }
    return step < 0 ? XIAOTAI_ERR_NOT_FOUND : XIAOTAI_OK;
}

xiaotai_err_t xiaotai_json_count(const char *text, size_t length, const char *path, size_t *count)
{
    if (!count) return XIAOTAI_ERR_INVALID_ARG;
    *count = 0;
    size_t pos;
    const xiaotai_err_t err = locate(text, length, path, &pos);
    if (err != XIAOTAI_OK) return err;
    if (text[pos] != '[') {
        return XIAOTAI_ERR_NOT_FOUND;
    }
    size_t elements = 0;
    if (first_item(text, length, &pos)) {
        do {
            ++elements;
            pos = skip_value(text, length, pos);
        } while (next_item(text, length, &pos));
    }
    *count = elements;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_json_text_string(const char *text, size_t length, const char *path,
                                       char *out, size_t capacity, size_t *decoded)
{
    size_t pos;
    const xiaotai_err_t err = locate(text, length, path, &pos);
    if (err != XIAOTAI_OK) return err;
    if (text[pos] != '"') return XIAOTAI_ERR_NOT_FOUND;
    const size_t end = skip_string(text, length, pos);
    return xiaotai_json_decode(text + pos + 1U, end - pos - 2U, out, capacity, decoded);
}

static xiaotai_json_view_t make_view(const char *text, size_t start, size_t end)
{
    xiaotai_json_type_t type;
    switch (text[start]) {
    case '{': type = XIAOTAI_JSON_OBJECT; break;
    case '[': type = XIAOTAI_JSON_ARRAY; break;
    case '"': type = XIAOTAI_JSON_STRING; break;
    case 't': type = XIAOTAI_JSON_TRUE; break;
    case 'f': type = XIAOTAI_JSON_FALSE; break;
    case 'n': type = XIAOTAI_JSON_NULL; break;
    default: type = XIAOTAI_JSON_NUMBER; break;
    }
    return (xiaotai_json_view_t){.text = text + start, .length = end - start, .type = type};
}

xiaotai_err_t xiaotai_json_view_parse(const char *text, size_t length, size_t *count, xiaotai_json_view_t *root)
{
    if (root) *root = (xiaotai_json_view_t){0};
    const xiaotai_err_t err = xiaotai_json_parse(text, length, NULL, 0, count, NULL);
    if (err == XIAOTAI_OK && root) {
        size_t start = 0;
        skip_ws(text, length, &start);
        while (is_ws(text[length - 1])) --length;
        *root = make_view(text, start, length);
    }
    return err;
}

bool xiaotai_json_view_get(const xiaotai_json_view_t *object, const char *key, xiaotai_json_view_t *out)
{
    if (!out) return false;
    xiaotai_json_view_t result = {0};
    size_t pos = 0;
    if (object && object->type == XIAOTAI_JSON_OBJECT && object->text && object->length && key &&
        member(object->text, object->length, &pos, key, strlen(key))) {
        result = make_view(object->text, pos, skip_value(object->text, object->length, pos));
    }
    *out = result;
    return result.type != 0;
}

void xiaotai_json_view_iter(const xiaotai_json_view_t *container, xiaotai_json_view_iter_t *iter)
{
    if (!iter) return;
    xiaotai_json_view_iter_t result = {0};
    if (container && (container->type == XIAOTAI_JSON_OBJECT || container->type == XIAOTAI_JSON_ARRAY) &&
        container->text && container->length && first_item(container->text, container->length, &result.next)) {
        result.container = *container;
    }
    *iter = result;
}

bool xiaotai_json_view_next(xiaotai_json_view_iter_t *iter, xiaotai_json_view_t *key, xiaotai_json_view_t *value)
{
    if (key) *key = (xiaotai_json_view_t){0};
    if (!value) return false;
    *value = (xiaotai_json_view_t){0};
    if (!iter || !iter->next || iter->next >= iter->container.length) return false;
    const char *text = iter->container.text;
    const size_t length = iter->container.length;
    size_t pos = iter->next;
    if (iter->container.type == XIAOTAI_JSON_OBJECT) {
        const size_t start = pos;
        size_t after;
        if (!take_key(text, length, &pos, &after)) {
            iter->next = 0;
            return false;
        }
        if (key) *key = make_view(text, start, after);
    }
    const size_t end = skip_value(text, length, pos);
    *value = make_view(text, pos, end);
    size_t next = end;
    iter->next = next_item(text, length, &next) ? next : 0;
    return true;
}
