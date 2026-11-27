// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "signal_scan.h"

static void split(const char *text, size_t size)
{
    platform_envelope_t envelope;
    const bool expected = platform_json_envelope(text, size, &envelope) == XIAOTAI_OK;
    const bool unbind = expected && platform_json_span_equals(text, envelope.type, "unbind");
    for (size_t cut = 0; cut <= size; ++cut) {
        platform_signal_scan_t scan = {0};
        platform_signal_scan_feed(&scan, (const uint8_t *)text, cut);
        platform_signal_scan_feed(&scan, (const uint8_t *)text + cut, size - cut);
        assert(platform_signal_scan_valid(&scan) == expected);
        assert(platform_signal_scan_unbind(&scan) == unbind);
    }
    platform_signal_scan_t scan = {0};
    for (size_t i = 0; i < size; ++i) platform_signal_scan_feed(&scan, (const uint8_t *)text + i, 1);
    assert(platform_signal_scan_valid(&scan) == expected);
    assert(platform_signal_scan_unbind(&scan) == unbind);
}

int main(void)
{
    static const char *const cases[] = {
        "", "{", "{}", "[]", "false", "{\"type\":\"\"}", "{\"type\":5,\"type\":\"x\"}",
        "{\"type\":\"ok\",\"type\":{},\"channel\":false,\"channel\":\"\\u0000\"}",
        "{\"ty\\u0070e\":\"a\",\"ch\\u0061nnel\":\"\\ud8ab\\udc90\"}",
        "{\"type\":\"x\",\"channel\":\"\\uD800a\"}", "{\"type\":\"x\",\"channel\":\"\\uD800\\n\"}",
        "{\"type\":\"x\",\"channel\":\"\\uD800\\u1111\"}", "{\"type\":\"\\udcff\"}",
        "{\"type\":\"x\",\"channel\":\"\\u0000\"}", "{\"type\":\"x\\u0000\"}",
        "{\"type\":\"x\",\"other\":\"\\u0000\\udaaa\"}", "{\"type\":\"\\u123z\"}",
        "{\"type\":\"x\",\"x\":[3,-1.23e+5,0.0,2E-9,true,null,false,{},[]]}",
        "{\"type\":\"x\",\"x\":[01]}", "{\"type\":\"x\",\"x\":[1.]}", "{\"type\":\"x\",\"x\":[1E-]}",
        "{\"type\":\"x\",\"x\":[1,]}", "{\"type\":\"x\",}", "{\"type\":\"x\"} {}",
        "\xef\xbb\xbf\x01\t{\"type\":\"x\"}\x1f", " \xef\xbb\xbf{\"type\":\"x\"}",
        "{\x01\"type\":\"x\"}", "{\"type\":\"x\"} \x07 ", "{\"type\":\"x\"}\\",
        "{\"type\":\"x\",\"longkeythatkeepsgoinganddoesnotmatch\":false}",
        "{\"type\":\"unbind\"}", "{\"ty\\u0070e\":\"\\u0075nbin\\u0064\"}",
        "{\"type\":\"other\",\"nested\":{\"type\":\"unbind\"}}",
        "{\"type\":\"unbind\",\"type\":\"other\"}", "{\"type\":\"other\",\"type\":\"unbind\"}",
        "{\"type\":\"unbind\",\"channel\":\"\\u0000\"}", "{\"type\":\"unbind\"} junk",
        "{\"type\":\"unbinde\"}", "{\"type\":\"unbin\"}", "{\"type\":\"unbin\\nd\"}",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) split(cases[i], strlen(cases[i]));

    char deep[190] = "{\"type\":\"x\",\"nested\":";
    const size_t base = strlen(deep);
    for (size_t count = 30; count <= 34; ++count) {
        memset(deep + base, '[', count);
        memset(deep + base + count, ']', count);
        deep[base + 2U * count] = '}';
        split(deep, base + 2U * count + 1U);
    }
    char varied[] = "{\"type\":\"\\uD800\\udf01\",\"channel\":\"hello\",\"nest\":[true,0e2,{}]}";
    for (size_t i = 0; i < sizeof(varied) - 1U; ++i) {
        const char original = varied[i];
        /* Every byte substituted, including a literal NUL; exercise all cuts. */
        for (unsigned j = 0; j <= 255; j += 7) {
            varied[i] = (char)j;
            split(varied, sizeof(varied) - 1U);
        }
        varied[i] = original;
    }
    puts("Receive-only envelope classifier matches the retained-buffer parser");
    return 0;
}
