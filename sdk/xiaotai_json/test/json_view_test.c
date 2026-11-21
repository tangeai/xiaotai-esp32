// SPDX-License-Identifier: MIT
/* Compare borrowed views with the original tokenizer and typed readers, using
 * exact-size inputs so sanitizer checks also cover the view's boundaries. */
#include "json_test.h"
#include "xiaotai/json_view.h"
#include "xiaotai_mem_host.h"

static xiaotai_json_token_t table[256];

static bool absent(const xiaotai_json_view_t *view)
{
    return !view->text && !view->length && !view->type;
}

static void compare_span(const xiaotai_json_doc_t *doc, size_t at, const xiaotai_json_view_t *view)
{
    const xiaotai_json_token_t *token = &doc->tokens[at];
    const size_t quotes = token->type == XIAOTAI_JSON_STRING ? 1U : 0U;
    CHECK(view->type == token->type);
    CHECK(view->text == doc->text + token->start - quotes);
    CHECK(view->length == token->end - token->start + quotes * 2U);
}

static void compare_reads(const xiaotai_json_doc_t *doc, int at, const xiaotai_json_view_t *view)
{
    int64_t old_int = INT64_MIN, new_int = INT64_MIN;
    bool old_bool = true, new_bool = true;
    CHECK(xiaotai_json_int(doc, at, &old_int) == xiaotai_json_view_int(view, &new_int));
    CHECK(old_int == new_int);
    CHECK(xiaotai_json_bool(doc, at, &old_bool) == xiaotai_json_view_bool(view, &new_bool));
    CHECK(old_bool == new_bool);
    CHECK(xiaotai_json_int(doc, at, NULL) == xiaotai_json_view_int(view, NULL));
    CHECK(xiaotai_json_bool(doc, at, NULL) == xiaotai_json_view_bool(view, NULL));
    static const size_t capacities[] = {0, 1, 4, 5, 12, 64};
    for (size_t i = 0; i < sizeof(capacities) / sizeof(*capacities); ++i) {
        size_t old_len = SIZE_MAX, new_len = SIZE_MAX;
        char old_text[64], new_text[64];
        memset(old_text, 'X', sizeof(old_text));
        memset(new_text, 'X', sizeof(new_text));
        CHECK(xiaotai_json_string(doc, at, old_text, capacities[i], &old_len) ==
              xiaotai_json_view_string(view, new_text, capacities[i], &new_len));
        CHECK(old_len == new_len && !memcmp(old_text, new_text, sizeof(old_text)));
    }
    size_t old_len = SIZE_MAX, new_len = SIZE_MAX;
    CHECK(xiaotai_json_string(doc, at, NULL, 0, &old_len) ==
          xiaotai_json_view_string(view, NULL, 0, &new_len));
    CHECK(old_len == new_len);
    CHECK(xiaotai_json_string(doc, at, NULL, 1, NULL) == xiaotai_json_view_string(view, NULL, 1, NULL));

    static const char *const words[] = {"", "code", "dup", "\xf0\x9f\x98\x80", "a\nb", NULL};
    for (size_t i = 0; i < sizeof(words) / sizeof(*words); ++i) {
        CHECK(xiaotai_json_equals(doc, at, words[i]) == xiaotai_json_view_equals(view, words[i]));
    }
}

/* Return the first token past the value in the reference walk. The original
 * token parents, not the new offsets, determine the expected children. */
static size_t compare_node(const xiaotai_json_doc_t *doc, size_t at, const xiaotai_json_view_t *view)
{
    compare_span(doc, at, view);
    compare_reads(doc, (int)at, view);
    const bool object = doc->tokens[at].type == XIAOTAI_JSON_OBJECT;
    const bool container = object || doc->tokens[at].type == XIAOTAI_JSON_ARRAY;
    size_t next = at + 1U;
    xiaotai_json_view_iter_t iter;
    xiaotai_json_view_iter(view, &iter);
    xiaotai_json_view_t key, child;
    while (container && next < doc->count && doc->tokens[next].parent == (int32_t)at) {
        if (!xiaotai_json_view_next(&iter, &key, &child)) {
            CHECK(false);
            return doc->count;
        }
        if (object) {
            compare_span(doc, next, &key);
            compare_reads(doc, (int)next, &key);
            char name[128];
            if (xiaotai_json_string(doc, (int)next, name, sizeof(name), NULL) == XIAOTAI_OK) {
                const int expected = xiaotai_json_get(doc, (int)at, name);
                xiaotai_json_view_t first;
                CHECK(expected >= 0);
                CHECK(xiaotai_json_view_get(view, name, &first));
                if (expected >= 0) compare_span(doc, (size_t)expected, &first);
            }
            ++next;
        } else {
            CHECK(absent(&key));
        }
        next = compare_node(doc, next, &child);
    }
    CHECK(!xiaotai_json_view_next(&iter, &key, &child));
    CHECK(absent(&key) && absent(&child));
    CHECK(!xiaotai_json_view_next(&iter, NULL, &child));
    return next;
}

static xiaotai_err_t compare(const char *text, size_t length)
{
    char *exact = json_test_exact(text, length);
    size_t old_count = 99U, new_count = 101U;
    xiaotai_json_doc_t doc;
    xiaotai_json_view_t root = {.text = exact, .length = 17, .type = XIAOTAI_JSON_NULL};
    const xiaotai_err_t old_rc = xiaotai_json_parse(exact, length, table, sizeof(table) / sizeof(*table),
                                                   &old_count, &doc);
    const xiaotai_err_t new_rc = xiaotai_json_view_parse(exact, length, &new_count, &root);
    CHECK(old_count == new_count && old_rc == new_rc);
    if (new_rc != XIAOTAI_OK) {
        CHECK(absent(&root));
    } else if (old_rc == XIAOTAI_OK) {
        CHECK(compare_node(&doc, 0, &root) == old_count);
        xiaotai_json_view_t missing = root;
        CHECK(!xiaotai_json_view_get(&root, "not an existing field", &missing));
        CHECK(absent(&missing));
        compare_reads(&doc, -1, &missing);
    }
    free(exact);
    return new_rc;
}

static void corpus(void)
{
    static const char *const accepted[] = {
        " \t \r\n{} ", " { \"\":[] , \"empty\":{  } } \t",
        "{\"d\\u0075p\":1,\"dup\":2,\"\\u0063ode\":3,\"code\":4,\"\\uD800\":5,\"a.b[0]\":[0]}",
        " [null,true,false,{\"nested\":[\"{[,:]}\",\"\\\"\\\\\\/\\b\\f\\n\\r\\t\",[{}],3]}]\r\n",
        "[0,-0,1,9007199254740991,-9007199254740991,9007199254740992,-9007199254740992,1.0,3e0,"
        "123456789012345678901234567890,1.25e+4]",
        "[\"\",\"\\u0000\",\"\\uDEAD\",\"\\ud83d\\ude00\",\"\\ud83d\\u0041\",\"\\u00e9\","
        "\"a\\nb\",\"\xe4\xb8\xad\xe6\x96\x87\",\"\xc0\x80\"]",
        " \r true ", " \t \"\\u0061\" \n", "\n -8.5e-2  \t", "[]",
    };
    for (size_t i = 0; i < sizeof(accepted) / sizeof(*accepted); ++i) {
        size_t length = strlen(accepted[i]);
        CHECK(compare(accepted[i], length) == XIAOTAI_OK);
        /* A prefix can itself be a valid scalar. The tokenizer decides. */
        for (size_t prefix = 0; prefix < length; ++prefix) (void)compare(accepted[i], prefix);
    }
    static const char *const refused[] = {
        "", " \t", "[] true", "[ {}, ]", "{\"first\":{\"code\":1},\"next\"",
        "{\"id\":\"a\",\"remark\":\"\\u00xz\"}", "[false,null,\"open]", "{\"x\":-01}", "[1, 3e+]",
        "{\"\\u0063ode\":200,\"items\":[1,2,]}", "[1 /* c */,2]", "{\"a\":1,}",
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(*refused); ++i) {
        CHECK(compare(refused[i], strlen(refused[i])) == XIAOTAI_ERR_INVALID_ARG);
    }
    static const char trailing_nul[] = {'[', '1', ']', '\0'};
    CHECK(compare(trailing_nul, sizeof(trailing_nul)) == XIAOTAI_ERR_INVALID_ARG);
    char depth[2U * (XIAOTAI_JSON_DEPTH_MAX + 1U) + 1U];
    for (size_t i = XIAOTAI_JSON_DEPTH_MAX; i <= XIAOTAI_JSON_DEPTH_MAX + 1U; ++i) {
        memset(depth, '[', i);
        depth[i] = '0';
        memset(depth + i + 1U, ']', i);
        CHECK(compare(depth, 2U * i + 1U) == (i == XIAOTAI_JSON_DEPTH_MAX ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_SIZE));
    }
}

static void guards_and_no_allocation(void)
{
    xiaotai_mem_host_clear();
    xiaotai_json_view_t root, view, copy;
    size_t count = 7U;
    CHECK(xiaotai_json_view_parse(NULL, 0, &count, &root) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(count == 0 && absent(&root));
    CHECK(xiaotai_json_view_parse(NULL, 3, &count, &root) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(count == 0 && absent(&root));
    CHECK(xiaotai_json_view_parse("1", (size_t)XIAOTAI_JSON_TEXT_MAX + 1U, &count, &root) ==
          XIAOTAI_ERR_INVALID_SIZE);
    CHECK(count == 0 && absent(&root));
    static const char text[] = "{ \"\\u006b\":[\"escaped\", false],\"k\":1 }";
    CHECK(xiaotai_json_view_parse(text, sizeof(text) - 1U, NULL, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_json_view_parse(text, sizeof(text) - 1U, &count, &root) == XIAOTAI_OK && count == 7U);
    CHECK(xiaotai_json_view_get(&root, "k", &view) && view.type == XIAOTAI_JSON_ARRAY);
    copy = view;
    CHECK(!xiaotai_json_view_get(&copy, "k", &view) && absent(&view));
    xiaotai_json_view_iter_t iter;
    xiaotai_json_view_iter(&copy, &iter);
    CHECK(xiaotai_json_view_next(&iter, NULL, &view) && xiaotai_json_view_equals(&view, "escaped"));
    bool flag = true;
    CHECK(xiaotai_json_view_next(&iter, NULL, &view) && xiaotai_json_view_bool(&view, &flag) == XIAOTAI_OK && !flag);
    CHECK(!xiaotai_json_view_next(&iter, NULL, &view));
    CHECK(xiaotai_mem_host_allocations() == 0 && xiaotai_mem_host_outstanding() == 0);
}

int main(void)
{
    corpus();
    guards_and_no_allocation();
    if (json_test_failures) {
        fprintf(stderr, "json_view: %d checks failed\n", json_test_failures);
        return 1;
    }
    printf("json_view: ok\n");
    return 0;
}
