// SPDX-License-Identifier: MIT
#pragma once
/* Shared by the xiaotai_json host tests: a check that records the failure and
 * goes on (one run reports every broken case), exact-size copies of texts so
 * the address sanitizer catches any read past `length` (the reader is given
 * texts that are not terminated), and a growable text builder for the large
 * cases. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int json_test_failures;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++json_test_failures;                                                             \
        }                                                                                     \
    } while (0)

/* Same, naming the input that failed. */
#define CHECK_TEXT(condition, text)                                                           \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed for %s: %s\n", __FILE__, __LINE__, (text),  \
                    #condition);                                                              \
            ++json_test_failures;                                                             \
        }                                                                                     \
    } while (0)

/* length bytes of text in a block of exactly that size (at least one byte, so
 * malloc(0) never returns NULL for an empty text). */
static inline char *json_test_exact(const char *text, size_t length)
{
    char *copy = (char *)malloc(length ? length : 1);
    if (!copy) {
        abort();
    }
    if (length) {
        memcpy(copy, text, length);
    }
    return copy;
}

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
} json_test_builder_t;

static inline void json_test_append(json_test_builder_t *b, const char *format, ...)
{
    for (;;) {
        va_list args;
        va_start(args, format);
        size_t room = b->capacity - b->length;
        int n = b->text ? vsnprintf(b->text + b->length, room, format, args) : -1;
        va_end(args);
        if (b->text && n >= 0 && (size_t)n < room) {
            b->length += (size_t)n;
            return;
        }
        size_t grown = b->capacity ? b->capacity * 2 : 4096;
        char *text = (char *)realloc(b->text, grown);
        if (!text) {
            abort();
        }
        b->text = text;
        b->capacity = grown;
    }
}

static inline void json_test_repeat(json_test_builder_t *b, const char *piece, size_t times)
{
    for (size_t i = 0; i < times; ++i) {
        json_test_append(b, "%s", piece);
    }
}
