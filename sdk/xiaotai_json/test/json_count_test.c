// SPDX-License-Identifier: MIT
/* Counting the elements of one array without a token table: 10 000 elements
 * at the top level and under a path where a table of 2048 tokens is refused,
 * agreement with the filled table's child counts, paths through members and
 * positions (escaped keys, the first of duplicate keys), brackets inside
 * strings, the not-found and parse error paths, and no port allocation. */
#include <stdbool.h>
#include <stdint.h>
#include "json_test.h"
#include "xiaotai/json.h"
#include "xiaotai_mem_host.h"

static xiaotai_err_t count_text(const char *text, const char *path, size_t *count)
{
    char *copy = json_test_exact(text, strlen(text));
    *count = 777;
    xiaotai_err_t err = xiaotai_json_count(copy, strlen(text), path, count);
    free(copy);
    return err;
}

static void test_ten_thousand(void)
{
    /* Top level: 10 000 numbers. */
    json_test_builder_t b = {0};
    json_test_append(&b, "%s", " [");
    for (size_t i = 0; i < 10000; ++i) {
        json_test_append(&b, "%s%zu", i ? ", " : "", i);
    }
    json_test_append(&b, "%s", "] ");
    char *top = json_test_exact(b.text, b.length);
    size_t count = 0;
    xiaotai_mem_host_clear();
    CHECK(xiaotai_json_count(top, b.length, "", &count) == XIAOTAI_OK && count == 10000);
    CHECK(xiaotai_json_count(top, b.length, NULL, &count) == XIAOTAI_OK && count == 10000);
    CHECK(xiaotai_mem_host_allocations() == 0);
    free(top);
    free(b.text);

    /* Under a platform-style envelope: 10 000 member objects, each with a
     * nested array and a string holding brackets. */
    b = (json_test_builder_t){0};
    json_test_append(&b, "%s", "{\"code\":200,\"data\":{\"total\":10000,\"list\":[");
    for (size_t i = 0; i < 10000; ++i) {
        json_test_append(&b, "%s{\"id\":%zu,\"tags\":[1,[2]],\"name\":\"[x]\\\"]\"}", i ? "," : "", i);
    }
    json_test_append(&b, "%s", "]}}");
    char *envelope = json_test_exact(b.text, b.length);
    xiaotai_mem_host_clear();
    CHECK(xiaotai_json_count(envelope, b.length, "data.list", &count) == XIAOTAI_OK && count == 10000);
    CHECK(xiaotai_json_count(envelope, b.length, "data.list[9999].tags", &count) == XIAOTAI_OK && count == 2);
    CHECK(xiaotai_json_count(envelope, b.length, "data.list[9999].tags[1]", &count) == XIAOTAI_OK && count == 1);
    CHECK(xiaotai_mem_host_allocations() == 0);

    /* A table a device could hold is refused, and the parse says how many
     * tokens the whole text would need. */
    static xiaotai_json_token_t tokens[2048];
    size_t needed = 0;
    CHECK(xiaotai_json_parse(envelope, b.length, tokens, 2048, &needed, NULL) == XIAOTAI_ERR_INVALID_SIZE);
    /* root, code, data, total, list (key and value each); per member: the
     * object, id, tags (key, array, 1, [2] as two tokens), name. */
    CHECK(needed == 1 + 2 + 2 + 2 + 2 + 10000 * (1 + 2 + 5 + 2));
    free(envelope);
    free(b.text);
}

static void test_agrees_with_table(void)
{
    static const char *const texts[] = {
        "[]", "[1]", "[[],[]]", "[{\"a\":[1,2]},\"]\",[[[]]],null,true,false,-1.5e3]",
        "[ \"\\\\\" , \"\\\"\" , \"\\u005d\" ]", "[\"\xe4\xb8\xad]\"]",
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        xiaotai_json_token_t tokens[64];
        xiaotai_json_doc_t doc;
        size_t n = 0;
        CHECK_TEXT(xiaotai_json_parse(texts[i], strlen(texts[i]), tokens, 64, &n, &doc) == XIAOTAI_OK, texts[i]);
        size_t count = 0;
        CHECK_TEXT(count_text(texts[i], "", &count) == XIAOTAI_OK, texts[i]);
        CHECK_TEXT(count == tokens[0].children, texts[i]);
    }
}

static void test_paths(void)
{
    size_t count = 0;
    static const char text[] =
        "{ \"a\" : [ [1], [2, 3], [] ], \"b\": {\"c\": [true]}, \"\\u0064\\\"e\": [1,2,3,4],"
        " \"dup\": [1], \"dup\": [1, 2], \"s\": \"[1,2]\", \"o\": {}, \"n\": 5, \"x.y\": [1] }";
    CHECK(count_text(text, "a", &count) == XIAOTAI_OK && count == 3);
    CHECK(count_text(text, "a[0]", &count) == XIAOTAI_OK && count == 1);
    CHECK(count_text(text, "a[1]", &count) == XIAOTAI_OK && count == 2);
    CHECK(count_text(text, "a[2]", &count) == XIAOTAI_OK && count == 0);
    CHECK(count_text(text, "b.c", &count) == XIAOTAI_OK && count == 1);
    /* Keys are compared after decoding their escapes. */
    CHECK(count_text(text, "d\"e", &count) == XIAOTAI_OK && count == 4);
    /* Duplicate keys: the first. */
    CHECK(count_text(text, "dup", &count) == XIAOTAI_OK && count == 1);
    /* Not an array, absent, past the end, malformed: NOT_FOUND and 0. */
    static const char *const absent[] = {
        "", "b", "s", "o", "n", "a[3]", "a[0][0]", "zz", "a.b", "b.c[1]", "x.y",
        "a..b", ".a", "a.", "a[", "a[x]", "a[1", "a[1]x", "a[-1]", "[0]", "a[99999999999999999999999]",
    };
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); ++i) {
        CHECK_TEXT(count_text(text, absent[i], &count) == XIAOTAI_ERR_NOT_FOUND && count == 0, absent[i]);
    }
    /* Positions at the top level. */
    CHECK(count_text("[[1,2],[3]]", "[0]", &count) == XIAOTAI_OK && count == 2);
    CHECK(count_text("[[1,2],[[3,4,5]]]", "[1][0]", &count) == XIAOTAI_OK && count == 3);
    CHECK(count_text("[{\"k\":[0]}]", "[0].k", &count) == XIAOTAI_OK && count == 1);
}

static void test_errors(void)
{
    size_t count = 0;
    /* The parse errors, before any path is followed. */
    CHECK(count_text("[1,2,]", "", &count) == XIAOTAI_ERR_INVALID_ARG && count == 0);
    CHECK(count_text("{\"a\":[1]} x", "a", &count) == XIAOTAI_ERR_INVALID_ARG && count == 0);
    CHECK(count_text("", "", &count) == XIAOTAI_ERR_INVALID_ARG);
    json_test_builder_t b = {0};
    json_test_repeat(&b, "[", XIAOTAI_JSON_DEPTH_MAX + 1);
    json_test_repeat(&b, "]", XIAOTAI_JSON_DEPTH_MAX + 1);
    char *deep = json_test_exact(b.text, b.length);
    CHECK(xiaotai_json_count(deep, b.length, "", &count) == XIAOTAI_ERR_INVALID_SIZE && count == 0);
    /* 32 levels are fine, and the path reaches the innermost array. */
    CHECK(xiaotai_json_count(deep + 1, b.length - 2, "", &count) == XIAOTAI_OK && count == 1);
    free(deep);
    free(b.text);
    CHECK(xiaotai_json_count("[1]", 3, "", NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_json_count(NULL, 3, "", &count) == XIAOTAI_ERR_INVALID_ARG);
}

static void test_table_free_string(void)
{
    const char literal[] = "{\"data\":[{\"m\\u0065thod\":\"room_\\u0073napshot\",\"method\":\"other\"}],\"n\":3}";
    char *text = json_test_exact(literal, sizeof(literal) - 1U);
    char value[14];
    size_t decoded = 0;
    CHECK(xiaotai_json_text_string(text, sizeof(literal) - 1U, "data[0].method", value, sizeof(value), &decoded) == XIAOTAI_OK);
    CHECK(!strcmp(value, "room_snapshot") && decoded == sizeof("room_snapshot") - 1U);
    CHECK(xiaotai_json_text_string(text, sizeof(literal) - 1U, "data[1].method", value, sizeof(value), NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_json_text_string(text, sizeof(literal) - 1U, "n", value, sizeof(value), NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_json_text_string(text, sizeof(literal) - 2U, "data[0].method", value, sizeof(value), NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_json_text_string(text, sizeof(literal) - 1U, "data[0].method", value, 13U, NULL) == XIAOTAI_ERR_INVALID_SIZE && !value[0]);
    CHECK(xiaotai_json_text_string("\"a\"", 3U, NULL, value, sizeof(value), NULL) == XIAOTAI_OK && !strcmp(value, "a"));
    free(text);
}

int main(void)
{
    test_ten_thousand();
    test_agrees_with_table();
    test_paths();
    test_errors();
    test_table_free_string();
    if (json_test_failures) {
        fprintf(stderr, "json_count: %d checks failed\n", json_test_failures);
        return 1;
    }
    printf("json_count: ok\n");
    return 0;
}
