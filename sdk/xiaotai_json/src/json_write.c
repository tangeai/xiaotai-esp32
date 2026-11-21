// SPDX-License-Identifier: MIT
/* Writer into a caller's buffer. Every call appends one whole item (comma,
 * key, value) or nothing: its size is computed first, so after an overflow the
 * buffer holds the complete items written before it, terminated, and every
 * later call is a no-op.
 *
 * Misuse that would make the text not JSON is refused the same way as an
 * overflow (overflow set, what was written kept, writer_end INVALID_SIZE).
 * The frozen writer struct has no bit for the kind of an open level, so the
 * checks are in two steps:
 * - at the call, what is visible in O(1): a second top-level value, a key at
 *   the top level, an end with nothing open, an empty raw value, and, while
 *   the innermost level is still empty (its opening bracket is then the last
 *   byte written), a missing or extra key on its first item and an end of the
 *   other kind;
 * - at writer_end, the whole text goes once through the reader in counting
 *   mode (linear, no table, no allocation). That catches the rest (a missing
 *   or extra key after the first item, an end of the other kind after items,
 *   a raw value that is not JSON or nests past XIAOTAI_JSON_DEPTH_MAX), so
 *   XIAOTAI_OK always means the reader accepts the text. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "xiaotai/json.h"

static const char HEX[] = "0123456789abcdef";

/* Bytes of value once escaped, without the quotes. */
static size_t escaped_length(const char *value, size_t length)
{
    size_t n = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            n += 2;
        } else if (c < 0x20) {
            n += 6;
        } else {
            n += 1;
        }
    }
    return n;
}

static char *write_escaped(char *out, const char *value, size_t length)
{
    *out++ = '"';
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        char simple = 0;
        switch (c) {
        case '"':
            simple = '"';
            break;
        case '\\':
            simple = '\\';
            break;
        case '\b':
            simple = 'b';
            break;
        case '\f':
            simple = 'f';
            break;
        case '\n':
            simple = 'n';
            break;
        case '\r':
            simple = 'r';
            break;
        case '\t':
            simple = 't';
            break;
        default:
            break;
        }
        if (simple) {
            *out++ = '\\';
            *out++ = simple;
        } else if (c < 0x20) {
            *out++ = '\\';
            *out++ = 'u';
            *out++ = '0';
            *out++ = '0';
            *out++ = HEX[c >> 4];
            *out++ = HEX[c & 0x0F];
        } else {
            *out++ = (char)c;
        }
    }
    *out++ = '"';
    return out;
}

static void refuse(xiaotai_json_writer_t *writer)
{
    writer->overflow = true;
}

static bool usable(const xiaotai_json_writer_t *writer)
{
    return writer && !writer->overflow;
}

/* True while nothing has been written in the innermost open level. */
static bool level_empty(const xiaotai_json_writer_t *writer)
{
    return writer->depth > 0 && ((writer->first >> (writer->depth - 1U)) & 1U);
}

/* The opening bracket of the innermost level; valid only while it is empty,
 * when that bracket is the last byte written (begin commits it last and no
 * other call writes into an empty level without clearing its bit). */
static char empty_level_open(const xiaotai_json_writer_t *writer)
{
    return writer->buffer[writer->length - 1];
}

/* Space for the separator and key of the next item at the current level, or
 * SIZE_MAX when the item is not allowed there. */
static size_t prefix_length(const xiaotai_json_writer_t *writer, const char *key)
{
    if (writer->depth == 0) {
        /* One top-level value, without a key. */
        return key || writer->length > 0 ? SIZE_MAX : 0;
    }
    bool empty = level_empty(writer);
    if (empty && (empty_level_open(writer) == '{') != (key != NULL)) {
        /* An object member needs a key, an array element has none. */
        return SIZE_MAX;
    }
    size_t n = empty ? 0 : 1;
    if (key) {
        n += escaped_length(key, strlen(key)) + 3; /* quotes and ':' */
    }
    return n;
}

static char *write_prefix(xiaotai_json_writer_t *writer, char *out, const char *key)
{
    if (writer->depth == 0) {
        return out;
    }
    uint32_t bit = 1U << (writer->depth - 1U);
    if (writer->first & bit) {
        writer->first &= ~bit;
    } else {
        *out++ = ',';
    }
    if (key) {
        out = write_escaped(out, key, strlen(key));
        *out++ = ':';
    }
    return out;
}

/* Reserves body bytes after the prefix; NULL (and the writer refused) when
 * they do not fit with the terminator. */
static char *reserve(xiaotai_json_writer_t *writer, const char *key, size_t body)
{
    size_t prefix = prefix_length(writer, key);
    if (prefix == SIZE_MAX) {
        refuse(writer);
        return NULL;
    }
    size_t room = writer->capacity - writer->length - 1; /* the terminator stays */
    if (prefix > room || body > room - prefix) {
        refuse(writer);
        return NULL;
    }
    return write_prefix(writer, writer->buffer + writer->length, key);
}

static void commit(xiaotai_json_writer_t *writer, char *end)
{
    *end = '\0';
    writer->length = (size_t)(end - writer->buffer);
}

void xiaotai_json_writer_init(xiaotai_json_writer_t *writer, char *buffer, size_t capacity)
{
    if (!writer) {
        return;
    }
    writer->buffer = buffer;
    writer->capacity = buffer ? capacity : 0;
    writer->length = 0;
    writer->first = 0;
    writer->depth = 0;
    /* No room even for the terminator: nothing can ever be written. */
    writer->overflow = writer->capacity == 0;
    if (!writer->overflow) {
        buffer[0] = '\0';
    }
}

xiaotai_err_t xiaotai_json_writer_end(xiaotai_json_writer_t *writer, size_t *length)
{
    if (length) {
        *length = 0;
    }
    if (!writer) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (writer->overflow) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    if (writer->depth > 0 || writer->length == 0) {
        /* Something still open, or no value at all: not a JSON text yet. */
        return XIAOTAI_ERR_INVALID_STATE;
    }
    /* Misuse the calls could not see (see the top of this file): the text is
     * what the reader would refuse. Counting mode stores no token. */
    if (xiaotai_json_parse(writer->buffer, writer->length, NULL, 0, NULL, NULL) != XIAOTAI_OK) {
        refuse(writer);
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    if (length) {
        *length = writer->length;
    }
    return XIAOTAI_OK;
}

static void begin(xiaotai_json_writer_t *writer, const char *key, char open)
{
    if (!usable(writer)) {
        return;
    }
    if (writer->depth >= XIAOTAI_JSON_DEPTH_MAX) {
        refuse(writer);
        return;
    }
    char *out = reserve(writer, key, 1);
    if (!out) {
        return;
    }
    *out++ = open;
    commit(writer, out);
    writer->first |= 1U << writer->depth;
    ++writer->depth;
}

static void end(xiaotai_json_writer_t *writer, char close)
{
    if (!usable(writer)) {
        return;
    }
    if (writer->depth == 0 || writer->capacity - writer->length < 2) {
        refuse(writer);
        return;
    }
    if (level_empty(writer) && empty_level_open(writer) != (close == '}' ? '{' : '[')) {
        /* An end of the other kind, visible while the level is empty. */
        refuse(writer);
        return;
    }
    --writer->depth;
    writer->first &= ~(1U << writer->depth);
    char *out = writer->buffer + writer->length;
    *out++ = close;
    commit(writer, out);
}

void xiaotai_json_object_begin(xiaotai_json_writer_t *writer, const char *key)
{
    begin(writer, key, '{');
}

void xiaotai_json_object_end(xiaotai_json_writer_t *writer)
{
    end(writer, '}');
}

void xiaotai_json_array_begin(xiaotai_json_writer_t *writer, const char *key)
{
    begin(writer, key, '[');
}

void xiaotai_json_array_end(xiaotai_json_writer_t *writer)
{
    end(writer, ']');
}

/* A value written as given (numbers, literals, raw JSON). */
static void put_verbatim(xiaotai_json_writer_t *writer, const char *key, const char *value, size_t length)
{
    if (!usable(writer)) {
        return;
    }
    char *out = reserve(writer, key, length);
    if (!out) {
        return;
    }
    memcpy(out, value, length);
    commit(writer, out + length);
}

void xiaotai_json_put_string_n(xiaotai_json_writer_t *writer, const char *key, const char *value, size_t length)
{
    if (!usable(writer)) {
        return;
    }
    if (!value) {
        if (length > 0) {
            refuse(writer);
            return;
        }
        value = "";
    }
    char *out = reserve(writer, key, escaped_length(value, length) + 2);
    if (!out) {
        return;
    }
    commit(writer, write_escaped(out, value, length));
}

void xiaotai_json_put_string(xiaotai_json_writer_t *writer, const char *key, const char *value)
{
    if (!value) {
        xiaotai_json_put_null(writer, key);
        return;
    }
    xiaotai_json_put_string_n(writer, key, value, strlen(value));
}

void xiaotai_json_put_int(xiaotai_json_writer_t *writer, const char *key, int64_t value)
{
    /* Exact decimal text for the whole int64 range (INT64_MIN through the
     * unsigned magnitude), never a floating-point format. */
    char digits[20];
    size_t n = 0;
    uint64_t magnitude = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    do {
        digits[n++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude > 0);
    char text[21];
    size_t length = 0;
    if (value < 0) {
        text[length++] = '-';
    }
    while (n > 0) {
        text[length++] = digits[--n];
    }
    put_verbatim(writer, key, text, length);
}

void xiaotai_json_put_bool(xiaotai_json_writer_t *writer, const char *key, bool value)
{
    put_verbatim(writer, key, value ? "true" : "false", value ? 4U : 5U);
}

void xiaotai_json_put_null(xiaotai_json_writer_t *writer, const char *key)
{
    put_verbatim(writer, key, "null", 4);
}

void xiaotai_json_put_raw(xiaotai_json_writer_t *writer, const char *key, const char *json, size_t length)
{
    if (!usable(writer)) {
        return;
    }
    if (!json || length == 0) {
        refuse(writer);
        return;
    }
    put_verbatim(writer, key, json, length);
}
