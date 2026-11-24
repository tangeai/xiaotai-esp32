// SPDX-License-Identifier: MIT
#include "at_test_help.h"
#include <string.h>

typedef bool (*record_fn)(void *ctx, const char *record, size_t length);

static bool read_help(const char *transcript, record_fn accept, void *ctx)
{
    static const char ready[] = "+READY: ";
    static const char help[] = "+HELP: ";
    if (!transcript) return false;
    bool first = true, has_help = false;
    for (const char *line = transcript; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) return false;
        size_t length = (size_t)(end - line);
        if (length && line[length - 1] == '\r') --length;
        if (memchr(line, '\r', length)) return false;
        if (length == 2 && !memcmp(line, "OK", 2)) return has_help && !end[1];
        if (length > sizeof(help) - 1 && !memcmp(line, help, sizeof(help) - 1)) {
            if (!accept(ctx, line + sizeof(help) - 1, length - (sizeof(help) - 1))) return false;
            has_help = true;
        } else if (!(first && length > sizeof(ready) - 1 && !memcmp(line, ready, sizeof(ready) - 1))) {
            return false;
        }
        first = false;
        line = end + 1;
    }
    return false;
}

static bool write_record(void *ctx, const char *record, size_t length)
{
    FILE *out = ctx;
    return fwrite(record, 1, length, out) == length && fputc('\n', out) != EOF;
}

bool at_test_help_write(FILE *out, const char *transcript)
{
    return out && read_help(transcript, write_record, out) && fflush(out) == 0;
}

typedef struct {
    const char *record;
    size_t length;
    size_t matches;
} match_t;

static bool find_record(void *ctx, const char *record, size_t length)
{
    match_t *match = ctx;
    if (match->length == length && !memcmp(match->record, record, length)) ++match->matches;
    return true;
}

bool at_test_help_has(const char *transcript, const char *record)
{
    if (!record || !*record) return false;
    match_t match = {.record = record, .length = strlen(record)};
    return read_help(transcript, find_record, &match) && match.matches == 1;
}
