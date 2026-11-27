// SPDX-License-Identifier: MIT
/* Signaling envelopes (src/json_scan.c): what makes a complete message
 * well-formed, as deployed firmware judged it (platform_mqtt_parse_json and
 * the runtime's read of "type" and "channel"): one JSON object, nested at
 * most 32 deep, no NUL byte, blanks and a byte-order mark around it skipped;
 * the first "type" a string; "channel" read only when it is a string; type
 * and channel decoded (escapes) and refused when they cannot be C strings.
 * Spans point into the caller's text and decode on demand; an 8191-byte
 * message is judged without a token table and without any port allocation.
 * Every text is read from an exact-size copy without a terminator. */
#include <stdbool.h>
#include <stdint.h>
#include "platform_internal.h"
#include "platform_test.h"
#include "xiaotai_mem_host.h"

#define TEXT(literal) literal, sizeof(literal) - 1

typedef struct {
    xiaotai_err_t result;
    char type[64];
    char channel[64];
    bool has_channel;
} judged_t;

static judged_t judge(const char *text, size_t length)
{
    judged_t judged = {0};
    char *copy = platform_test_exact(text, length);
    platform_envelope_t envelope;
    memset(&envelope, 0x5A, sizeof(envelope));
    judged.result = platform_json_envelope(copy, length, &envelope);
    if (judged.result == XIAOTAI_OK) {
        CHECK_TEXT(envelope.type.offset + envelope.type.length < length, text);
        CHECK_TEXT(platform_json_span_text(copy, envelope.type, judged.type, sizeof(judged.type), NULL) ==
                       XIAOTAI_OK,
                   text);
        judged.has_channel = envelope.has_channel;
        if (envelope.has_channel) {
            CHECK_TEXT(platform_json_span_text(copy, envelope.channel, judged.channel, sizeof(judged.channel),
                                               NULL) == XIAOTAI_OK,
                       text);
        }
    } else {
        /* Untouched on refusal. */
        platform_envelope_t pattern;
        memset(&pattern, 0x5A, sizeof(pattern));
        CHECK_TEXT(memcmp(&envelope, &pattern, sizeof(pattern)) == 0, text);
    }
    free(copy);
    return judged;
}

static void well_formed(void)
{
    judged_t j = judge(TEXT("{\"type\":\"call_incoming\",\"channel\":\"wx\",\"payload\":{\"wx_room_id\":\"r\"}}"));
    CHECK(j.result == XIAOTAI_OK && strcmp(j.type, "call_incoming") == 0);
    CHECK(j.has_channel && strcmp(j.channel, "wx") == 0);

    j = judge(TEXT("{\"payload\":{\"type\":\"inner\"},\"type\":\"unbind\"}"));
    CHECK(j.result == XIAOTAI_OK && strcmp(j.type, "unbind") == 0 && !j.has_channel);

    /* channel only as a string; other values read as absent. */
    j = judge(TEXT("{\"type\":\"a\",\"channel\":7}"));
    CHECK(j.result == XIAOTAI_OK && !j.has_channel);
    j = judge(TEXT("{\"type\":\"a\",\"channel\":null}"));
    CHECK(j.result == XIAOTAI_OK && !j.has_channel);
    j = judge(TEXT("{\"type\":\"a\",\"channel\":{\"name\":\"wx\"}}"));
    CHECK(j.result == XIAOTAI_OK && !j.has_channel);
    j = judge(TEXT("{\"type\":\"a\",\"channel\":\"\"}"));
    CHECK(j.result == XIAOTAI_OK && j.has_channel && j.channel[0] == '\0');

    /* The first member of a name counts. */
    j = judge(TEXT("{\"type\":\"first\",\"type\":\"second\",\"channel\":\"wx\",\"channel\":\"voip\"}"));
    CHECK(j.result == XIAOTAI_OK && strcmp(j.type, "first") == 0 && strcmp(j.channel, "wx") == 0);

    /* Escapes decode, in names and values. */
    j = judge(TEXT("{\"t\\u0079pe\":\"\\u0063all_cancel\",\"channel\":\"w\\u0078\"}"));
    CHECK(j.result == XIAOTAI_OK && strcmp(j.type, "call_cancel") == 0 && strcmp(j.channel, "wx") == 0);

    /* An empty type is a string: well-formed, nobody subscribes to it. */
    j = judge(TEXT("{\"type\":\"\"}"));
    CHECK(j.result == XIAOTAI_OK && j.type[0] == '\0');

    /* Framing of the strict parse. */
    j = judge(TEXT("\xEF\xBB\xBF \r\n{\"type\":\"a\"}\r\n\x01\x1f"));
    CHECK(j.result == XIAOTAI_OK && strcmp(j.type, "a") == 0);

    /* A lone surrogate elsewhere does not matter here (only what is read
     * must decode). */
    j = judge(TEXT("{\"type\":\"a\",\"payload\":{\"s\":\"\\ud800\"}}"));
    CHECK(j.result == XIAOTAI_OK);
}

static void malformed(void)
{
    static const struct {
        const char *text;
        size_t length;
    } cases[] = {
        {TEXT("{}")},
        {TEXT("{\"channel\":\"wx\"}")},
        {TEXT("{\"type\":7}")},
        {TEXT("{\"type\":null}")},
        {TEXT("{\"type\":[\"a\"]}")},
        {TEXT("{\"type\":{\"a\":1}}")},
        {TEXT("{\"type\":1,\"type\":\"a\"}")},
        {TEXT("{\"Type\":\"a\"}")},
        {TEXT("{\"payload\":{\"type\":\"a\"}}")},
        {TEXT("[{\"type\":\"a\"}]")},
        {TEXT("\"type\"")},
        {TEXT("{\"type\":\"a\"}x")},
        {TEXT("{\"type\":\"a\"")},
        {TEXT("{\"type\":\"a\",}")},
        {TEXT("{\"type\":\"a\"}\0")},
        {TEXT("{\"type\":\"a\0b\"}")},
        {TEXT("")},
        {TEXT(" \r\n")},
        /* type and channel must be C strings. */
        {TEXT("{\"type\":\"\\ud800\"}")},
        {TEXT("{\"type\":\"a\\u0000b\"}")},
        {TEXT("{\"type\":\"a\",\"channel\":\"\\udc00\"}")},
        /* Strict inside the value (xiaotai/json.h): raw control characters. */
        {TEXT("{\"type\":\"a\x01\"}")},
        {TEXT("{\"type\":\"a\",\v\"b\":1}")},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        judged_t j = judge(cases[i].text, cases[i].length);
        CHECK_TEXT(j.result == XIAOTAI_ERR_INVALID_ARG, cases[i].text);
    }
    platform_envelope_t envelope;
    CHECK(platform_json_envelope(NULL, 10, &envelope) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(platform_json_envelope("{\"type\":\"a\"}", 12, NULL) == XIAOTAI_ERR_INVALID_ARG);
}

static char *nested(size_t depth, size_t *length)
{
    size_t size = 32 + 2 * depth;
    char *text = (char *)malloc(size);
    size_t n = (size_t)snprintf(text, size, "{\"type\":\"a\",\"n\":");
    for (size_t i = 1; i < depth; ++i) text[n++] = '[';
    for (size_t i = 1; i < depth; ++i) text[n++] = ']';
    text[n++] = '}';
    *length = n;
    return text;
}

static void depth(void)
{
    size_t length = 0;
    char *text = nested(32, &length);
    CHECK(judge(text, length).result == XIAOTAI_OK);
    free(text);
    text = nested(33, &length);
    CHECK(judge(text, length).result == XIAOTAI_ERR_INVALID_ARG);
    free(text);
}

static void spans(void)
{
    static const char text[] = "{\"type\":\"room_\\u0061ssignment_changed\",\"channel\":\"device\"}";
    platform_envelope_t envelope;
    CHECK(platform_json_envelope(text, sizeof(text) - 1, &envelope) == XIAOTAI_OK);
    /* Raw span: between the quotes, escapes as written. */
    CHECK(envelope.type.offset == 9 && envelope.type.length == 28);
    CHECK(platform_json_span_equals(text, envelope.type, "room_assignment_changed"));
    CHECK(!platform_json_span_equals(text, envelope.type, "room_assignment"));
    CHECK(!platform_json_span_equals(text, envelope.type, NULL));
    CHECK(platform_json_span_equals(text, envelope.channel, "device"));
    size_t length = 0;
    CHECK(platform_json_span_text(text, envelope.type, NULL, 0, &length) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(length == 23);
    char small[23];
    CHECK(platform_json_span_text(text, envelope.type, small, sizeof(small), &length) == XIAOTAI_ERR_INVALID_SIZE);
    char fits[24];
    CHECK(platform_json_span_text(text, envelope.type, fits, sizeof(fits), &length) == XIAOTAI_OK);
    CHECK(length == 23 && strcmp(fits, "room_assignment_changed") == 0);
    /* A span that is not a string of this text. */
    platform_json_span_t wrong = {.offset = 1, .length = 3};
    CHECK(platform_json_span_text(text, wrong, fits, sizeof(fits), NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(!platform_json_span_equals(text, wrong, "type"));
}

static void large(void)
{
    /* The largest message the default ceiling admits (8191 bytes), type last. */
    char *text = (char *)malloc(8191);
    size_t n = (size_t)snprintf(text, 8191, "{\"payload\":\"");
    const char tail[] = "\",\"type\":\"callers_update\"}";
    while (n < 8191 - (sizeof(tail) - 1)) text[n++] = 'p';
    memcpy(text + n, tail, sizeof(tail) - 1);
    n += sizeof(tail) - 1;
    CHECK(n == 8191);
    judged_t j = judge(text, n);
    CHECK(j.result == XIAOTAI_OK && strcmp(j.type, "callers_update") == 0);
    free(text);
}

int main(void)
{
    xiaotai_mem_host_clear();
    well_formed();
    malformed();
    depth();
    spans();
    large();
    CHECK(xiaotai_mem_host_allocations() == 0);
    return platform_test_result("platform_envelope");
}
