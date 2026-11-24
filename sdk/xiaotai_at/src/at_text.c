// SPDX-License-Identifier: MIT
/* Text helpers of the console vocabulary (xiaotai/at_command.h): final-line
 * error names, one formatted reply line, and the argument checks every
 * command author shares. Pure functions apart from xiaotai_at_printf, which
 * writes through the reply it is given; nothing here allocates, blocks or
 * keeps state, so any task may call them.
 *
 * The argument rules are the strictest of the earlier consoles, applied to
 * every product alike: numbers are plain decimal digits (no sign, space,
 * radix prefix or trailing character), identifiers are 1..64 characters of
 * [0-9A-Za-z_-], and any other text travels as hexadecimal with "-" for the
 * empty text and no zero byte inside. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/at_command.h"

/* ---------------------------------------------------------------- error names */

const char *xiaotai_at_error_name(xiaotai_err_t error, char *scratch, size_t capacity)
{
    switch (error) {
    case XIAOTAI_FAIL: return "FAIL";
    case XIAOTAI_ERR_NO_MEM: return "NO_MEM";
    case XIAOTAI_ERR_INVALID_ARG: return "INVALID_ARG";
    case XIAOTAI_ERR_INVALID_STATE: return "INVALID_STATE";
    case XIAOTAI_ERR_INVALID_SIZE: return "INVALID_SIZE";
    case XIAOTAI_ERR_NOT_FOUND: return "NOT_FOUND";
    case XIAOTAI_ERR_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case XIAOTAI_ERR_TIMEOUT: return "TIMEOUT";
    case XIAOTAI_ERR_BUSY: return "BUSY";
    case XIAOTAI_ERR_NOT_ALLOWED: return "NOT_ALLOWED";
    default: break;
    }
    if (!scratch || capacity == 0) {
        return "FAIL";
    }
    /* The code's bit pattern, so a platform code passed through unchanged
     * (0x3001) reads as in the platform's own documentation and a negative
     * code still has a fixed width. */
    int written = snprintf(scratch, capacity, "0x%" PRIX32, (uint32_t)error);
    if (written < 0 || (size_t)written >= capacity) {
        scratch[0] = '\0';
        return "FAIL";
    }
    return scratch;
}

/* ---------------------------------------------------------------- reply lines */

void xiaotai_at_printf(const xiaotai_reply_t *reply, const char *format, ...)
{
    if (!reply || !reply->write || !format) {
        return;
    }
    char line[XIAOTAI_AT_OUT_LINE_MAX + 1];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0) {
        xiaotai_at_reply_output_fault(reply, XIAOTAI_AT_OUTPUT_MALFORMED);
        return;
    }
    /* A cut line would read as a complete record with a wrong last value, so
     * nothing of it is written and the command is made to fail instead. */
    if ((size_t)length > XIAOTAI_AT_OUT_LINE_MAX) {
        xiaotai_at_reply_output_fault(reply, XIAOTAI_AT_OUTPUT_TOO_LONG);
        return;
    }
    if (memchr(line, '\n', (size_t)length) || memchr(line, '\r', (size_t)length) ||
        memchr(line, '\0', (size_t)length)) {
        xiaotai_at_reply_output_fault(reply, XIAOTAI_AT_OUTPUT_MALFORMED);
        return;
    }
    reply->write(reply->ctx, line);
}

/* ------------------------------------------------------------------ arguments */

/* Plain decimal: 1..max_digits digits and a value not above limit. A leading
 * zero is a digit like any other ("007" is 7) but counts against
 * max_digits, so no input is longer than the widest value it can hold. */
static bool parse_digits(const char *text, size_t max_digits, uint64_t limit, uint64_t *value)
{
    if (!text || !*text) {
        return false;
    }
    uint64_t result = 0;
    size_t count = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || ++count > max_digits) {
            return false;
        }
        unsigned digit = (unsigned)(*p - '0');
        /* result * 10 + digit <= limit, without wrapping for any limit. */
        if (result > limit / 10U) {
            return false;
        }
        result *= 10U;
        if (digit > limit - result) {
            return false;
        }
        result += digit;
    }
    *value = result;
    return true;
}

bool xiaotai_at_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    uint64_t parsed;
    if (min > max || !parse_digits(text, 10, max, &parsed) || parsed < min) {
        return false;
    }
    if (value) {
        *value = (uint32_t)parsed;
    }
    return true;
}

bool xiaotai_at_parse_i32(const char *text, int32_t min, int32_t max, int32_t *value)
{
    if (!text || min > max) {
        return false;
    }
    bool negative = text[0] == '-';
    /* The magnitude of INT32_MIN is one more than INT32_MAX. */
    uint64_t limit = negative ? (uint64_t)INT32_MAX + 1U : (uint64_t)INT32_MAX;
    uint64_t magnitude;
    if (!parse_digits(negative ? text + 1 : text, 10, limit, &magnitude)) {
        return false;
    }
    int64_t parsed = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    if (parsed < min || parsed > max) {
        return false;
    }
    if (value) {
        *value = (int32_t)parsed;
    }
    return true;
}

bool xiaotai_at_parse_u64(const char *text, uint64_t max, uint64_t *value)
{
    uint64_t parsed;
    if (!parse_digits(text, 20, max, &parsed)) {
        return false;
    }
    if (value) {
        *value = parsed;
    }
    return true;
}

bool xiaotai_at_parse_flag(const char *text, bool *value)
{
    if (!text || (text[0] != '0' && text[0] != '1') || text[1] != '\0') {
        return false;
    }
    if (value) {
        *value = text[0] == '1';
    }
    return true;
}

static bool id_character(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
}

bool xiaotai_at_is_id(const char *text, size_t max_length)
{
    if (!text) {
        return false;
    }
    if (max_length > XIAOTAI_AT_ID_MAX) {
        max_length = XIAOTAI_AT_ID_MAX;
    }
    size_t length = 0;
    for (; text[length]; ++length) {
        if (length >= max_length || !id_character(text[length])) {
            return false;
        }
    }
    return length > 0;
}

bool xiaotai_at_is_digits(const char *text, size_t count)
{
    if (!text) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false; /* also stops at the terminator of a shorter text */
        }
    }
    return text[count] == '\0';
}

int xiaotai_at_keyword(const char *word, const char *const *keywords)
{
    if (!word || !keywords) {
        return -1;
    }
    for (int i = 0; keywords[i]; ++i) {
        if (!strcmp(word, keywords[i])) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- hexadecimal */

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

/* Checks the whole text before anything is written, so a rejected argument
 * (a secret among them) never leaves a partial decode behind. *bytes receives
 * the decoded size. */
static xiaotai_err_t hex_check(const char *hex, bool allow_zero, size_t *bytes)
{
    if (!hex || !*hex) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    size_t length = 0;
    for (; hex[length] && hex[length + 1]; length += 2) {
        int high = hex_value(hex[length]);
        int low = hex_value(hex[length + 1]);
        if (high < 0 || low < 0 || (!allow_zero && high == 0 && low == 0)) {
            return XIAOTAI_ERR_INVALID_ARG;
        }
    }
    if (hex[length]) {
        return XIAOTAI_ERR_INVALID_ARG; /* odd length */
    }
    *bytes = length / 2U;
    return XIAOTAI_OK;
}

static void hex_fill(const char *hex, uint8_t *out, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) {
        out[i] = (uint8_t)((hex_value(hex[2 * i]) << 4) | hex_value(hex[2 * i + 1]));
    }
}

xiaotai_err_t xiaotai_at_hex_decode(const char *hex, uint8_t *out, size_t capacity, size_t *size)
{
    size_t bytes = 0;
    if (size) {
        *size = 0;
    }
    xiaotai_err_t err = hex_check(hex, true, &bytes);
    if (err != XIAOTAI_OK) {
        return err;
    }
    if (!out || bytes > capacity) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    hex_fill(hex, out, bytes);
    if (size) {
        *size = bytes;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_at_text_decode(const char *hex, char *out, size_t capacity)
{
    if (out && capacity) {
        out[0] = '\0';
    }
    if (hex && hex[0] == '-' && hex[1] == '\0') {
        return out && capacity ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_SIZE;
    }
    size_t bytes = 0;
    /* A zero byte would end the C string early: the owner would store a
     * shorter text than the one sent. */
    xiaotai_err_t err = hex_check(hex, false, &bytes);
    if (err != XIAOTAI_OK) {
        return err;
    }
    if (!out || bytes >= capacity) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    hex_fill(hex, (uint8_t *)out, bytes);
    out[bytes] = '\0';
    return XIAOTAI_OK;
}

size_t xiaotai_at_hex_encode(const void *data, size_t size, char *out, size_t capacity)
{
    static const char digits[] = "0123456789ABCDEF";
    if (!out || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if ((size && !data) || size > (capacity - 1U) / 2U) {
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < size; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 0x0FU];
    }
    out[2 * size] = '\0';
    return 2 * size;
}

size_t xiaotai_at_text_encode(const char *text, char *out, size_t capacity)
{
    if (!out || capacity == 0) {
        return 0;
    }
    if (!text || !*text) {
        if (capacity < 2) {
            out[0] = '\0';
            return 0;
        }
        out[0] = '-';
        out[1] = '\0';
        return 1;
    }
    return xiaotai_at_hex_encode(text, strlen(text), out, capacity);
}
