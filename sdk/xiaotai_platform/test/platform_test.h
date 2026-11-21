// SPDX-License-Identifier: MIT
#pragma once
/* Shared by the platform core's host tests: checks that record a failure and
 * go on (one run reports every broken case), and exact-size copies of texts
 * so the address sanitizer catches any read past the given length. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int platform_test_failures;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++platform_test_failures;                                                         \
        }                                                                                     \
    } while (0)

/* Same, naming the input that failed. */
#define CHECK_TEXT(condition, text)                                                           \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed for \"%s\": %s\n", __FILE__, __LINE__,      \
                    (text), #condition);                                                      \
            ++platform_test_failures;                                                         \
        }                                                                                     \
    } while (0)

/* length bytes of text in a block of exactly that size (at least one byte). */
static inline char *platform_test_exact(const char *text, size_t length)
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

static inline int platform_test_result(const char *name)
{
    if (platform_test_failures) {
        fprintf(stderr, "FAIL: %s: %d check(s) failed\n", name, platform_test_failures);
        return 1;
    }
    printf("PASS: %s\n", name);
    return 0;
}
