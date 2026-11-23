// SPDX-License-Identifier: MIT
/* Queries and strict reads: lookups by key (escapes decoded, first of
 * duplicates, not into nested objects), positions, paths ("data.list[2].id",
 * malformed paths), integers at +-(2^53 - 1) and refused just outside, with
 * fractions, exponents and strings refused, booleans, string decoding (simple
 * escapes, \uXXXX to UTF-8, surrogate pairs, lone surrogates and \u0000
 * refused), exact and short capacities, equality and raw text, and the error
 * paths leaving the outputs untouched. */
#include <stdbool.h>
#include <stdint.h>
#include "json_test.h"
#include "xiaotai/json.h"

static xiaotai_json_token_t table[128];
static char *text_copy;

/* Parses text from an exact-size copy that stays until the next call. */
static xiaotai_json_doc_t parse(const char *text)
{
    free(text_copy);
    text_copy = json_test_exact(text, strlen(text));
    xiaotai_json_doc_t doc;
    size_t count = 0;
    xiaotai_err_t err = xiaotai_json_parse(text_copy, strlen(text), table, 128, &count, &doc);
    CHECK_TEXT(err == XIAOTAI_OK, text);
    return doc;
}

static void test_lookup(void)
{
    xiaotai_json_doc_t doc = parse(
        "{\"code\":200,\"data\":{\"list\":[{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"c\",\"n\":{\"id\":\"inner\"}}],"
        "\"total\":3},\"a.b\":1,\"c[0]\":2,\"\\u0065sc\\n\":3,\"dup\":1,\"dup\":2,\"\":4}");
    int data = xiaotai_json_get(&doc, 0, "data");
    CHECK(data > 0 && doc.tokens[data].type == XIAOTAI_JSON_OBJECT);
    CHECK(xiaotai_json_get(&doc, 0, "missing") == -1);
    CHECK(xiaotai_json_get(&doc, 0, NULL) == -1);
    /* get looks at one object's own keys, not into nested ones. */
    CHECK(xiaotai_json_get(&doc, 0, "list") == -1);
    CHECK(xiaotai_json_get(&doc, 0, "total") == -1);
    CHECK(xiaotai_json_get(&doc, data, "total") == xiaotai_json_find(&doc, 0, "data.total"));
    /* Not an object: -1. A key token is a string. */
    CHECK(xiaotai_json_get(&doc, xiaotai_json_find(&doc, 0, "data.list"), "id") == -1);
    CHECK(xiaotai_json_get(&doc, 1, "code") == -1);
    CHECK(xiaotai_json_get(&doc, -1, "code") == -1);
    CHECK(xiaotai_json_get(&doc, 1000, "code") == -1);
    CHECK(xiaotai_json_get(NULL, 0, "code") == -1);

    int64_t value = 0;
    /* Keys that a path cannot name are reached with get; escapes decoded. */
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "a.b"), &value) == XIAOTAI_OK && value == 1);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "c[0]"), &value) == XIAOTAI_OK && value == 2);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "esc\n"), &value) == XIAOTAI_OK && value == 3);
    CHECK(xiaotai_json_get(&doc, 0, "\\u0065sc\\n") == -1);
    CHECK(xiaotai_json_get(&doc, 0, "esc") == -1);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "dup"), &value) == XIAOTAI_OK && value == 1);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, ""), &value) == XIAOTAI_OK && value == 4);

    /* Positions. */
    int list = xiaotai_json_find(&doc, 0, "data.list");
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, xiaotai_json_at(&doc, list, 0), "id"), "a"));
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, xiaotai_json_at(&doc, list, 2), "id"), "c"));
    CHECK(xiaotai_json_at(&doc, list, 3) == -1);
    CHECK(xiaotai_json_at(&doc, list, SIZE_MAX) == -1);
    CHECK(xiaotai_json_at(&doc, 0, 0) == -1);
    CHECK(xiaotai_json_at(&doc, -1, 0) == -1);

    /* Paths. */
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "data.list[2].id"), "c"));
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "data.list[2].n.id"), "inner"));
    CHECK(xiaotai_json_find(&doc, 0, "") == 0);
    CHECK(xiaotai_json_find(&doc, data, "") == data);
    CHECK(xiaotai_json_find(&doc, data, "list[1].id") == xiaotai_json_find(&doc, 0, "data.list[1].id"));
    static const char *const absent[] = {
        "data.list[3].id", "data.list[0].n", "data.total.x", "code[0]", "data.list.id", "nope",
        "data..list", ".data", "data.", "data.list[", "data.list[]", "data.list[a]", "data.list[0]id",
        "data.list[-1]", "data.list[0].", "data.list[99999999999999999999999]", "[0]", "data.[0]",
    };
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); ++i) {
        CHECK_TEXT(xiaotai_json_find(&doc, 0, absent[i]) == -1, absent[i]);
    }
    CHECK(xiaotai_json_find(&doc, 0, NULL) == -1);
    CHECK(xiaotai_json_find(&doc, -1, "data") == -1);
    CHECK(xiaotai_json_find(&doc, 500, "") == -1);

    doc = parse("[[10,[20,21]],{\"k\":[30]}]");
    CHECK(xiaotai_json_int(&doc, xiaotai_json_find(&doc, 0, "[0][1][1]"), &value) == XIAOTAI_OK && value == 21);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_find(&doc, 0, "[1].k[0]"), &value) == XIAOTAI_OK && value == 30);
    CHECK(xiaotai_json_find(&doc, 0, "[1]k") == -1);
    CHECK(xiaotai_json_find(&doc, 0, "k") == -1);
}

static void test_integers(void)
{
    xiaotai_json_doc_t doc = parse(
        "{\"max\":9007199254740991,\"min\":-9007199254740991,\"over\":9007199254740992,"
        "\"under\":-9007199254740992,\"zero\":0,\"neg0\":-0,\"frac\":1.0,\"exp\":1e2,\"Exp\":1E0,"
        "\"str\":\"5\",\"huge\":123456789012345678901234567890,\"t\":true,\"null\":null,\"o\":{},\"a\":[]}");
    int64_t value = 0;
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "max"), &value) == XIAOTAI_OK &&
          value == XIAOTAI_JSON_INT_MAX);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "min"), &value) == XIAOTAI_OK &&
          value == -XIAOTAI_JSON_INT_MAX);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "zero"), &value) == XIAOTAI_OK && value == 0);
    value = 7;
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "neg0"), &value) == XIAOTAI_OK && value == 0);
    static const char *const refused[] = {"over", "under", "frac", "exp", "Exp", "str", "huge", "t", "null", "o", "a"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        value = 4242;
        CHECK_TEXT(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, refused[i]), &value) == XIAOTAI_ERR_INVALID_ARG,
                   refused[i]);
        CHECK_TEXT(value == 4242, refused[i]);
    }
    value = 4242;
    CHECK(xiaotai_json_int(&doc, -1, &value) == XIAOTAI_ERR_NOT_FOUND && value == 4242);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "absent"), &value) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_json_int(&doc, 1000, &value) == XIAOTAI_ERR_INVALID_ARG && value == 4242);
    CHECK(xiaotai_json_int(&doc, -2, &value) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_json_int(NULL, 0, &value) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "max"), NULL) == XIAOTAI_ERR_INVALID_ARG);

    bool flag = false;
    CHECK(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "t"), &flag) == XIAOTAI_OK && flag);
    flag = true;
    doc = parse("[false,0,\"true\",null]");
    CHECK(xiaotai_json_bool(&doc, xiaotai_json_at(&doc, 0, 0), &flag) == XIAOTAI_OK && !flag);
    for (size_t i = 1; i < 4; ++i) {
        flag = true;
        CHECK(xiaotai_json_bool(&doc, xiaotai_json_at(&doc, 0, i), &flag) == XIAOTAI_ERR_INVALID_ARG && flag);
    }
    CHECK(xiaotai_json_bool(&doc, -1, &flag) == XIAOTAI_ERR_NOT_FOUND && flag);
    CHECK(xiaotai_json_bool(&doc, xiaotai_json_at(&doc, 0, 0), NULL) == XIAOTAI_ERR_INVALID_ARG);
}

static void test_strings(void)
{
    xiaotai_json_doc_t doc = parse(
        "[\"plain\",\"\\\"\\\\\\/\\b\\f\\n\\r\\t\",\"\\u0041\\u00e9\\u4E2D\",\"\\ud83d\\ude00\",\"\xe4\xb8\xad\","
        "\"\\uD800\",\"\\uDC00\",\"\\ud83dx\",\"\\ud83d\\u0041\",\"\\u0000\",\"a\\u0000b\",\"\",\"\\u007f\\u0080\\u07ff\\u0800\\uffff\","
        "1,{\"k\":\"v\"}]");
    char out[64];
    size_t length = 0;
    CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, 0), out, sizeof(out), &length) == XIAOTAI_OK);
    CHECK(strcmp(out, "plain") == 0 && length == 5);
    CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, 1), out, sizeof(out), &length) == XIAOTAI_OK);
    CHECK(strcmp(out, "\"\\/\b\f\n\r\t") == 0 && length == 8);
    /* \uXXXX to UTF-8: one, two and three bytes; a surrogate pair to four. */
    CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, 2), out, sizeof(out), &length) == XIAOTAI_OK);
    CHECK(strcmp(out, "A\xc3\xa9\xe4\xb8\xad") == 0 && length == 6);
    CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, 3), out, sizeof(out), &length) == XIAOTAI_OK);
    CHECK(strcmp(out, "\xf0\x9f\x98\x80") == 0 && length == 4);
    /* Raw UTF-8 passes unchanged. */
    CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, 4), out, sizeof(out), &length) == XIAOTAI_OK);
    CHECK(strcmp(out, "\xe4\xb8\xad") == 0);
    CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, 12), out, sizeof(out), &length) == XIAOTAI_OK);
    CHECK(strcmp(out, "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf") == 0 && length == 11);
    CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, 11), out, sizeof(out), &length) == XIAOTAI_OK);
    CHECK(out[0] == '\0' && length == 0);

    /* Lone surrogates, a high surrogate without its low half, and NUL are
     * refused; nothing is written. */
    for (size_t i = 5; i <= 10; ++i) {
        memset(out, 'Z', sizeof(out));
        length = 999;
        CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, i), out, sizeof(out), &length) ==
              XIAOTAI_ERR_INVALID_ARG);
        CHECK(out[0] == 'Z' && length == 999);
        CHECK(!xiaotai_json_equals(&doc, xiaotai_json_at(&doc, 0, i), ""));
    }

    /* Capacity: exactly the decoded bytes plus the terminator fits; one less
     * reports the length and leaves an empty string, never a truncated one. */
    int index = xiaotai_json_at(&doc, 0, 2);
    char *exact = (char *)malloc(7);
    CHECK(xiaotai_json_string(&doc, index, exact, 7, &length) == XIAOTAI_OK && length == 6);
    CHECK(memcmp(exact, "A\xc3\xa9\xe4\xb8\xad", 7) == 0);
    memset(exact, 'Z', 7);
    CHECK(xiaotai_json_string(&doc, index, exact, 6, &length) == XIAOTAI_ERR_INVALID_SIZE && length == 6);
    CHECK(exact[0] == '\0' && exact[1] == 'Z');
    free(exact);
    /* Measuring without a buffer. */
    length = 0;
    CHECK(xiaotai_json_string(&doc, index, NULL, 0, &length) == XIAOTAI_ERR_INVALID_SIZE && length == 6);
    CHECK(xiaotai_json_string(&doc, index, NULL, 5, &length) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_json_string(&doc, index, out, sizeof(out), NULL) == XIAOTAI_OK);

    /* Other types and indexes. */
    memset(out, 'Z', sizeof(out));
    CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, 13), out, sizeof(out), &length) ==
          XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_json_string(&doc, xiaotai_json_at(&doc, 0, 14), out, sizeof(out), &length) ==
          XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_json_string(&doc, -1, out, sizeof(out), &length) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_json_string(&doc, 999, out, sizeof(out), &length) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(out[0] == 'Z');

    /* Equality after decoding. */
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_at(&doc, 0, 2), "A\xc3\xa9\xe4\xb8\xad"));
    CHECK(!xiaotai_json_equals(&doc, xiaotai_json_at(&doc, 0, 2), "A\xc3\xa9"));
    CHECK(!xiaotai_json_equals(&doc, xiaotai_json_at(&doc, 0, 2), "A\xc3\xa9\xe4\xb8\xad!"));
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_at(&doc, 0, 11), ""));
    CHECK(!xiaotai_json_equals(&doc, xiaotai_json_at(&doc, 0, 13), "1"));
    CHECK(!xiaotai_json_equals(&doc, xiaotai_json_at(&doc, 0, 0), NULL));
    CHECK(!xiaotai_json_equals(&doc, -1, "plain"));
}

static void test_raw(void)
{
    xiaotai_json_doc_t doc = parse("{\"sub\": {\"a\": [1, \"x\"]} , \"s\":\"q\\\"z\",\"n\":-1.5e3}");
    size_t length = 0;
    const char *raw = xiaotai_json_raw(&doc, xiaotai_json_get(&doc, 0, "sub"), &length);
    CHECK(raw && length == 15 && memcmp(raw, "{\"a\": [1, \"x\"]}", 15) == 0);
    /* Strings without their quotes, escapes as written. */
    raw = xiaotai_json_raw(&doc, xiaotai_json_get(&doc, 0, "s"), &length);
    CHECK(raw && length == 4 && memcmp(raw, "q\\\"z", 4) == 0);
    raw = xiaotai_json_raw(&doc, xiaotai_json_get(&doc, 0, "n"), &length);
    CHECK(raw && length == 6 && memcmp(raw, "-1.5e3", 6) == 0);
    length = 9;
    CHECK(xiaotai_json_raw(&doc, -1, &length) == NULL && length == 0);
    CHECK(xiaotai_json_raw(&doc, 0, NULL) == text_copy);
    CHECK(xiaotai_json_raw(NULL, 0, &length) == NULL);
}

int main(void)
{
    test_lookup();
    test_integers();
    test_strings();
    test_raw();
    free(text_copy);
    if (json_test_failures) {
        fprintf(stderr, "json_query: %d checks failed\n", json_test_failures);
        return 1;
    }
    printf("json_query: ok\n");
    return 0;
}
