// SPDX-License-Identifier: MIT
#pragma once
/* JSON reading and writing without allocation, for every component.
 *
 * Reading is two steps. xiaotai_json_parse() tokenizes a text into a table the
 * caller provides (a token is 16 bytes; a 4 KiB signal of 64 fields needs
 * about 130 tokens, about 2 KiB): objects, arrays, strings and primitives with their place
 * in the text and their parent, nothing copied. The queries below then find a
 * value by key or path and read it strictly. With no table the parse only
 * counts, so a caller can size the table (or refuse the text) first, and
 * xiaotai_json_count() counts the elements of one array without storing any
 * token, for lists longer than any table a small device can hold.
 *
 * Limits: text up to XIAOTAI_JSON_TEXT_MAX bytes, nesting up to
 * XIAOTAI_JSON_DEPTH_MAX. Strict: one top-level value, no trailing data, no
 * comments, no trailing commas, no unescaped control characters in strings,
 * numbers as RFC 8259 writes them. Duplicate keys are kept; lookups return the
 * first.
 *
 * Writing goes into a caller's buffer: the writer escapes strings, places
 * commas and closes nothing implicitly. A buffer that is too small makes
 * every later call a no-op and xiaotai_json_writer_end() report it, so a
 * builder checks once at the end.
 *
 * Every function is [any, rt] (xiaotai/framework.h): no allocation, no lock,
 * no log, and its time is linear in the text. Status: contract; work package
 * J1 implements it with host tests, before the module work packages that
 * read signals and replies, which use it instead of tokenizers of their own. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_JSON_TEXT_MAX 0x7FFFFFFFU
/* Deepest nesting read or written: deployed firmware accepts recognition task
 * replies and platform envelopes nested up to 32 levels. The writer keeps one
 * bit per level in a uint32_t. */
#define XIAOTAI_JSON_DEPTH_MAX 32U
/* Exact integers: what a double holds without loss (2^53 - 1). */
#define XIAOTAI_JSON_INT_MAX 9007199254740991LL

typedef enum {
    XIAOTAI_JSON_OBJECT = 1,
    XIAOTAI_JSON_ARRAY,
    XIAOTAI_JSON_STRING,     /* start/end exclude the quotes; escapes not decoded */
    XIAOTAI_JSON_NUMBER,
    XIAOTAI_JSON_TRUE,
    XIAOTAI_JSON_FALSE,
    XIAOTAI_JSON_NULL,
} xiaotai_json_type_t;

typedef struct {
    uint8_t type;            /* xiaotai_json_type_t */
    uint8_t reserved;
    uint16_t children;       /* object: keys; array: elements; others 0 (saturates at 65535) */
    int32_t parent;          /* token index; -1 for the top-level value. A key's parent is its object, a
                              * value's parent is its key */
    uint32_t start, end;     /* byte offsets in the text, end exclusive */
} xiaotai_json_token_t;

typedef struct {
    const char *text;
    size_t length;
    const xiaotai_json_token_t *tokens;
    size_t count;
} xiaotai_json_doc_t;

/* Tokenizes text (length bytes, not necessarily terminated). tokens NULL
 * (capacity 0): only counts; *count is the number needed. Otherwise fills
 * tokens in text order and sets doc for the queries (doc may be NULL when
 * only counting).
 * XIAOTAI_ERR_INVALID_ARG: not JSON by the rules above (*count: the tokens
 *   up to the error);
 * XIAOTAI_ERR_INVALID_SIZE: more tokens than capacity (*count: how many the
 *   whole text needs), deeper than XIAOTAI_JSON_DEPTH_MAX, or a text longer
 *   than XIAOTAI_JSON_TEXT_MAX. */
xiaotai_err_t xiaotai_json_parse(const char *text, size_t length, xiaotai_json_token_t *tokens, size_t capacity,
                                 size_t *count, xiaotai_json_doc_t *doc);

/* Elements of the array at path (see xiaotai_json_find; "" is the top level)
 * without a token table: XIAOTAI_ERR_NOT_FOUND when the path does not lead to
 * an array; the parse errors above otherwise. */
xiaotai_err_t xiaotai_json_count(const char *text, size_t length, const char *path, size_t *count);

/* Reads the decoded string at path without allocating or filling a token
 * table; paths and duplicate keys follow xiaotai_json_count. Validates the
 * whole text even if the matching field comes first. NOT_FOUND for an absent
 * path or a value of another type; output and decoded length as in
 * xiaotai_json_string. */
xiaotai_err_t xiaotai_json_text_string(const char *text, size_t length, const char *path,
                                       char *out, size_t capacity, size_t *decoded);

/* Index of the value under key in the object at index `object`; -1 when
 * `object` is not an object or has no such key. key is compared after
 * decoding the stored key's escapes. */
int xiaotai_json_get(const xiaotai_json_doc_t *doc, int object, const char *key);
/* Index of the element at position in the array at index `array`; -1 past
 * the end or when `array` is not an array. */
int xiaotai_json_at(const xiaotai_json_doc_t *doc, int array, size_t position);
/* Index of the value at a path from `from` (0: the top level): keys separated
 * by '.', array positions as [n]: "data.list[2].id". A key that contains '.'
 * or '[' cannot be named by a path: use xiaotai_json_get. -1 when absent. */
int xiaotai_json_find(const xiaotai_json_doc_t *doc, int from, const char *path);

/* Strict reads of the value at index. XIAOTAI_ERR_NOT_FOUND for index -1;
 * XIAOTAI_ERR_INVALID_ARG for another type (or, for integers, a fraction, an
 * exponent, a value outside +-XIAOTAI_JSON_INT_MAX, or a string); nothing is
 * written then. */
xiaotai_err_t xiaotai_json_int(const xiaotai_json_doc_t *doc, int index, int64_t *value);
xiaotai_err_t xiaotai_json_bool(const xiaotai_json_doc_t *doc, int index, bool *value);
/* The string at index, escapes decoded (\uXXXX and surrogate pairs to UTF-8;
 * \u0000 is refused), terminated. XIAOTAI_ERR_INVALID_SIZE when it does not
 * fit capacity with its terminator (*length, when not NULL, is the decoded
 * length either way). */
xiaotai_err_t xiaotai_json_string(const xiaotai_json_doc_t *doc, int index, char *out, size_t capacity,
                                  size_t *length);
/* True when the value at index is a string equal to text after decoding. */
bool xiaotai_json_equals(const xiaotai_json_doc_t *doc, int index, const char *text);
/* The raw text of the value at index (strings without their quotes, escapes
 * not decoded), for passing a sub-object on unchanged. */
const char *xiaotai_json_raw(const xiaotai_json_doc_t *doc, int index, size_t *length);

/* ------------------------------------------------------------------ writing */

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;             /* without the terminator, which is always kept */
    uint32_t first;            /* bit per open level: nothing written in it yet */
    uint8_t depth;
    bool overflow;             /* the buffer was too small, or the nesting too deep */
} xiaotai_json_writer_t;

void xiaotai_json_writer_init(xiaotai_json_writer_t *writer, char *buffer, size_t capacity);
/* XIAOTAI_OK with the text terminated; XIAOTAI_ERR_INVALID_SIZE after an
 * overflow; XIAOTAI_ERR_INVALID_STATE with an object or array still open. */
xiaotai_err_t xiaotai_json_writer_end(xiaotai_json_writer_t *writer, size_t *length);

/* key: NULL inside an array or at the top level, the member name inside an
 * object (escaped like a string value). */
void xiaotai_json_object_begin(xiaotai_json_writer_t *writer, const char *key);
void xiaotai_json_object_end(xiaotai_json_writer_t *writer);
void xiaotai_json_array_begin(xiaotai_json_writer_t *writer, const char *key);
void xiaotai_json_array_end(xiaotai_json_writer_t *writer);
/* value NULL writes null. */
void xiaotai_json_put_string(xiaotai_json_writer_t *writer, const char *key, const char *value);
/* Exactly length bytes of value, escaped. */
void xiaotai_json_put_string_n(xiaotai_json_writer_t *writer, const char *key, const char *value, size_t length);
void xiaotai_json_put_int(xiaotai_json_writer_t *writer, const char *key, int64_t value);
void xiaotai_json_put_bool(xiaotai_json_writer_t *writer, const char *key, bool value);
void xiaotai_json_put_null(xiaotai_json_writer_t *writer, const char *key);
/* A value that is already JSON (a sub-object passed on), copied unchanged. */
void xiaotai_json_put_raw(xiaotai_json_writer_t *writer, const char *key, const char *json, size_t length);

#ifdef __cplusplus
}
#endif
