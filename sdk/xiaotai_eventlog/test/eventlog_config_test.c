// SPDX-License-Identifier: MIT
/* The event log's build options: the Kconfig a product edits and the
 * configuration header the code reads must agree, and a value set in the
 * build must reach the macros the public header publishes.
 *
 *   - every option in Kconfig has a macro and every macro an option;
 *   - the header's default (the host's value) equals the Kconfig default;
 *   - each default lies inside its Kconfig range;
 *   - a unit built with every option changed sees the changed values through
 *     xiaotai/eventlog.h (the header callers include), not the defaults;
 *   - the per-topic value count stays a fixed part of the data format. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xiaotai/eventlog.h"

#include "eventlog_config_test.h"

#ifndef XIAOTAI_TEST_KCONFIG
#error "XIAOTAI_TEST_KCONFIG must name the component's Kconfig"
#endif

typedef struct {
    char name[64];
    long range_low;
    long range_high;
    long fallback;
    int has_range;
    int has_default;
} kconfig_option_t;

static int s_failures;

#define CHECK(condition, ...)                                         \
    do {                                                              \
        if (!(condition)) {                                           \
            ++s_failures;                                             \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

/* Reads every "config", "range" and "default" line; returns the option count. */
static size_t read_kconfig(const char *path, kconfig_option_t *options, size_t capacity)
{
    FILE *file = fopen(path, "r");
    CHECK(file != NULL, "cannot open %s", path);
    if (!file) return 0;
    char line[256];
    size_t count = 0;
    kconfig_option_t *current = NULL;
    while (fgets(line, sizeof(line), file)) {
        char keyword[16] = "";
        char word[64];
        long low = 0;
        long high = 0;
        /* Help text is free prose: only a line whose first word is exactly a
         * keyword counts. */
        if (sscanf(line, " %15s", keyword) != 1) continue;
        if (strcmp(keyword, "config") == 0 && sscanf(line, " config %63s", word) == 1) {
            CHECK(count < capacity, "more options than the test expects");
            if (count >= capacity) break;
            current = &options[count++];
            memset(current, 0, sizeof(*current));
            snprintf(current->name, sizeof(current->name), "%s", word);
        } else if (current && strcmp(keyword, "range") == 0 && sscanf(line, " range %ld %ld", &low, &high) == 2) {
            current->range_low = low;
            current->range_high = high;
            current->has_range = 1;
        } else if (current && strcmp(keyword, "default") == 0 && sscanf(line, " default %ld", &low) == 1) {
            current->fallback = low;
            current->has_default = 1;
        }
    }
    fclose(file);
    return count;
}

int main(void)
{
    const struct {
        const char *name;
        long value;
    } macros[] = {
        {"XIAOTAI_EVENTLOG_SINKS_MAX", XIAOTAI_EVENTLOG_SINKS_MAX},
        {"XIAOTAI_EVENTLOG_TOPICS_MAX", XIAOTAI_EVENTLOG_TOPICS_MAX},
    };
    const size_t macro_count = sizeof(macros) / sizeof(macros[0]);

    kconfig_option_t options[16];
    const size_t count = read_kconfig(XIAOTAI_TEST_KCONFIG, options, sizeof(options) / sizeof(options[0]));
    CHECK(count == macro_count, "Kconfig has %zu options, the configuration header %zu", count, macro_count);

    for (size_t i = 0; i < count; ++i) {
        const kconfig_option_t *option = &options[i];
        size_t found = macro_count;
        for (size_t m = 0; m < macro_count; ++m) {
            if (strcmp(option->name, macros[m].name) == 0) found = m;
        }
        CHECK(found < macro_count, "Kconfig option %s has no macro in xiaotai/eventlog_config.h", option->name);
        CHECK(option->has_default && option->has_range, "%s needs a default and a range", option->name);
        if (found == macro_count || !option->has_default || !option->has_range) continue;
        CHECK(option->fallback == macros[found].value, "%s: Kconfig default %ld, header default %ld",
              option->name, option->fallback, macros[found].value);
        CHECK(option->range_low >= 1 && option->range_low <= option->fallback &&
                  option->fallback <= option->range_high,
              "%s: default %ld outside %ld..%ld (or a range starting below 1)", option->name, option->fallback,
              option->range_low, option->range_high);
    }
    for (size_t m = 0; m < macro_count; ++m) {
        size_t seen = 0;
        for (size_t i = 0; i < count; ++i) seen += strcmp(options[i].name, macros[m].name) == 0;
        CHECK(seen == 1, "%s appears %zu times in Kconfig", macros[m].name, seen);
    }

    /* The defaults the host uses (Kconfig defaults, so equal on ESP-IDF). */
    CHECK(XIAOTAI_EVENTLOG_SINKS_MAX == 4, "sinks %d", XIAOTAI_EVENTLOG_SINKS_MAX);
    CHECK(XIAOTAI_EVENTLOG_TOPICS_MAX == 16, "topics %d", XIAOTAI_EVENTLOG_TOPICS_MAX);
    /* Part of the snapshot layout, not an option. */
    CHECK(XIAOTAI_EVENTLOG_TOPIC_VALUES == 17U, "topic values %u", (unsigned)XIAOTAI_EVENTLOG_TOPIC_VALUES);
    const size_t snapshot_values = sizeof(((const xiaotai_eventlog_topic_snapshot_t *)0)->values) / sizeof(uint32_t);
    CHECK(snapshot_values == XIAOTAI_EVENTLOG_TOPIC_VALUES, "snapshot holds %zu values", snapshot_values);

    eventlog_config_values_t changed;
    eventlog_config_overridden(&changed);
    CHECK(changed.sinks == 6, "overridden sinks %ld", changed.sinks);
    CHECK(changed.topics == 32, "overridden topics %ld", changed.topics);

    if (s_failures) {
        fprintf(stderr, "eventlog_config: %d failure(s)\n", s_failures);
        return EXIT_FAILURE;
    }
    printf("eventlog_config: ok\n");
    return EXIT_SUCCESS;
}
