// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_device_profile_response.py
/* xiaotai_platform_json_code: the strict business-code read of deployed
 * firmware (main/xiaotai/services/shared/platform_json_code.h on a parse
 * that required the terminator): one top-level "code" (a second one, also
 * one written with escapes, makes the reply codeless), only top-level
 * members count, an integral number in int32 range (fractions, exponents,
 * strings, booleans refused), one JSON object nested at most 32 deep, a NUL
 * byte anywhere refused, a byte-order mark and bytes 0x01..0x20 around the
 * object skipped. Every text is read from an exact-size copy without a
 * terminator; the output is untouched on failure; no port allocation. */
#include <stdbool.h>
#include <stdint.h>
#include "platform_test.h"
#include "xiaotai/platform.h"
#include "xiaotai_mem_host.h"

static xiaotai_err_t read_code(const char *text, size_t length, int32_t *code)
{
    char *copy = platform_test_exact(text, length);
    xiaotai_err_t err = xiaotai_platform_json_code(copy, length, code);
    free(copy);
    return err;
}

static void expect_code(const char *text, size_t length, int32_t expected)
{
    int32_t code = 12345;
    CHECK_TEXT(read_code(text, length, &code) == XIAOTAI_OK, text);
    CHECK_TEXT(code == expected, text);
}

static void expect_none(const char *text, size_t length)
{
    int32_t code = 12345;
    CHECK_TEXT(read_code(text, length, &code) == XIAOTAI_ERR_NOT_FOUND, text);
    CHECK_TEXT(code == 12345, text);
}

#define TEXT(literal) literal, sizeof(literal) - 1

static void accepted(void)
{
    expect_code(TEXT("{\"code\":200}"), 200);
    expect_code(TEXT("{\"code\":6006,\"msg\":\"device unbound\",\"data\":null}"), 6006);
    expect_code(TEXT("{\"msg\":\"ok\",\"data\":{\"code\":5,\"list\":[{\"code\":7}]},\"code\":0}"), 0);
    expect_code(TEXT("{\"code\":-1}"), -1);
    expect_code(TEXT("{\"code\":50000}"), 50000);
    expect_code(TEXT("{ \"code\" : 410 , \"x\" : [ ] }"), 410);
    expect_code(TEXT("{\"co\\u0064e\":200}"), 200);
    expect_code(TEXT("{\"code\":2147483647}"), INT32_MAX);
    expect_code(TEXT("{\"code\":-2147483647}"), -INT32_MAX);
    /* Framing of the strict cJSON parse: blanks of 0x01..0x20 around, a BOM. */
    expect_code(TEXT("\r\n\t {\"code\":200}\r\n"), 200);
    expect_code(TEXT("\x01\x1f{\"code\":200}\v\f\x01"), 200);
    expect_code(TEXT("\xEF\xBB\xBF{\"code\":200}"), 200);
    expect_code(TEXT("{\"code\":200,\"msg\":\"\\\"code\\\":1\"}"), 200);
}

static void refused(void)
{
    /* Duplicate code, also when written with an escape (keys compare decoded). */
    expect_none(TEXT("{\"code\":200,\"code\":200}"));
    expect_none(TEXT("{\"code\":200,\"co\\u0064e\":1}"));
    /* Only the top level counts; names compare with case. */
    expect_none(TEXT("{\"data\":{\"code\":200}}"));
    expect_none(TEXT("{\"Code\":200}"));
    expect_none(TEXT("{\"msg\":\"ok\"}"));
    expect_none(TEXT("{}"));
    /* Not an integral number. */
    expect_none(TEXT("{\"code\":\"200\"}"));
    expect_none(TEXT("{\"code\":200.0}"));
    expect_none(TEXT("{\"code\":2e2}"));
    expect_none(TEXT("{\"code\":200.5}"));
    expect_none(TEXT("{\"code\":true}"));
    expect_none(TEXT("{\"code\":null}"));
    expect_none(TEXT("{\"code\":[200]}"));
    expect_none(TEXT("{\"code\":{\"value\":200}}"));
    /* Outside int32 (INT32_MIN is XIAOTAI_PLATFORM_NO_CODE). */
    expect_none(TEXT("{\"code\":2147483648}"));
    expect_none(TEXT("{\"code\":-2147483648}"));
    expect_none(TEXT("{\"code\":9007199254740993}"));
    /* Not one JSON object. */
    expect_none(TEXT("[{\"code\":200}]"));
    expect_none(TEXT("\"code\""));
    expect_none(TEXT("200"));
    expect_none(TEXT("{\"code\":200}x"));
    expect_none(TEXT("{\"code\":200}{}"));
    expect_none(TEXT("{\"code\":200"));
    expect_none(TEXT("{\"code\" 200}"));
    expect_none(TEXT("{\"code\":200,}"));
    expect_none(TEXT("{'code':200}"));
    expect_none(TEXT("\xEF\xBB{\"code\":200}"));
    expect_none(TEXT(""));
    expect_none(TEXT("   "));
    /* A NUL byte anywhere, a trailing one included. */
    expect_none(TEXT("{\"code\":200}\0"));
    expect_none(TEXT("{\"code\":200,\"m\":\"a\0b\"}"));
    expect_none(TEXT("\0{\"code\":200}"));
    /* Arguments. */
    int32_t code = 7;
    CHECK(xiaotai_platform_json_code(NULL, 4, &code) == XIAOTAI_ERR_NOT_FOUND && code == 7);
    CHECK(xiaotai_platform_json_code("{\"code\":1}", 10, NULL) == XIAOTAI_ERR_INVALID_ARG);
    /* The size bounds the text: a longer buffer is not read past size. */
    CHECK(xiaotai_platform_json_code("{\"code\":200}garbage", 12, &code) == XIAOTAI_OK && code == 200);
}

/* {"code":1,"n": + (depth - 1) levels of arrays + 1 + closings + } */
static char *nested(size_t depth, size_t *length)
{
    size_t size = 16 + 2 * depth + 4;
    char *text = (char *)malloc(size);
    size_t n = (size_t)snprintf(text, size, "{\"code\":1,\"n\":");
    for (size_t i = 1; i < depth; ++i) text[n++] = '[';
    text[n++] = '1';
    for (size_t i = 1; i < depth; ++i) text[n++] = ']';
    text[n++] = '}';
    *length = n;
    return text;
}

static void depth(void)
{
    size_t length = 0;
    char *text = nested(32, &length);
    int32_t code = 0;
    CHECK(read_code(text, length, &code) == XIAOTAI_OK && code == 1);
    free(text);
    text = nested(33, &length);
    code = 0;
    CHECK(read_code(text, length, &code) == XIAOTAI_ERR_NOT_FOUND && code == 0);
    free(text);
}

static void large(void)
{
    /* A reply filling the 8191-byte pool body, code last. */
    char *text = (char *)malloc(8192);
    size_t n = (size_t)snprintf(text, 8192, "{\"data\":\"");
    while (n < 8191 - 13) text[n++] = 'a';
    n += (size_t)snprintf(text + n, 8192 - n, "\",\"code\":200}");
    CHECK(n == 8191);
    int32_t code = 0;
    CHECK(read_code(text, n, &code) == XIAOTAI_OK && code == 200);
    free(text);
}

int main(void)
{
    xiaotai_mem_host_clear();
    accepted();
    refused();
    depth();
    large();
    CHECK(xiaotai_mem_host_allocations() == 0);
    return platform_test_result("platform_json_code");
}
