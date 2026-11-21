// SPDX-License-Identifier: MIT
#pragma once
/* Shared by the console engine's host tests: a check that records the failure
 * and goes on, so one run reports every broken case, and a fake reply that
 * keeps what was written. */
#include <stdio.h>
#include <string.h>

#include "xiaotai/framework.h"

static int at_test_failures;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++at_test_failures;                                                               \
        }                                                                                     \
    } while (0)

/* Same, naming the input that failed. */
#define CHECK_TEXT(condition, text)                                                           \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed for \"%s\": %s\n", __FILE__, __LINE__,      \
                    (text) ? (const char *)(text) : "(null)", #condition);                    \
            ++at_test_failures;                                                               \
        }                                                                                     \
    } while (0)

/* What a reply received: the number of write calls and their texts joined
 * with '|' (a separator no tested line contains). */
typedef struct {
    unsigned writes;
    char text[2048];
} at_test_sink_t;

static inline void at_test_sink_write(void *ctx, const char *text)
{
    at_test_sink_t *sink = (at_test_sink_t *)ctx;
    size_t used = strlen(sink->text);
    snprintf(sink->text + used, sizeof(sink->text) - used, "%s%s", sink->writes ? "|" : "", text);
    ++sink->writes;
}

static inline xiaotai_reply_t at_test_reply(at_test_sink_t *sink)
{
    memset(sink, 0, sizeof(*sink));
    xiaotai_reply_t reply = {at_test_sink_write, sink};
    return reply;
}

static inline int at_test_result(const char *name)
{
    if (at_test_failures) {
        fprintf(stderr, "%s: %d check(s) failed\n", name, at_test_failures);
        return 1;
    }
    printf("%s: PASS\n", name);
    return 0;
}
