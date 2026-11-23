// SPDX-License-Identifier: MIT
/* Tokenizer: the RFC 8259 examples and token layout, strict acceptance and
 * refusal of every grammar rule, the inputs where it is stricter than the
 * cJSON parsing it replaces, counting mode agreeing with the filled table,
 * capacity and the count it reports, 32 levels accepted and 33 refused, the
 * saturating child count, the text-length limit, and no port allocation. Every
 * text is parsed from an exact-size copy without a terminator. */
#include <stdbool.h>
#include <stdint.h>
#include "json_test.h"
#include "xiaotai/json.h"
#include "xiaotai_mem_host.h"

#define TABLE 256
static xiaotai_json_token_t table[TABLE];

/* Parses text in counting mode and into the table; checks that both agree on
 * the result and the count. Returns the table result. */
static xiaotai_err_t parse_both(const char *text, size_t length, size_t capacity, size_t *count,
                                xiaotai_json_doc_t *doc)
{
    char *copy = json_test_exact(text, length);
    size_t counted = 12345;
    xiaotai_err_t counting = xiaotai_json_parse(copy, length, NULL, 0, &counted, NULL);
    size_t filled = 12345;
    xiaotai_err_t result = xiaotai_json_parse(copy, length, table, capacity, &filled, doc);
    if (result != XIAOTAI_ERR_INVALID_SIZE || counting == XIAOTAI_ERR_INVALID_SIZE) {
        /* A table too small is the only difference between the two modes. */
        CHECK_TEXT(counting == result, text);
    }
    CHECK_TEXT(counted == filled, text);
    *count = filled;
    if (doc && result == XIAOTAI_OK) {
        /* The queries read the caller's text; point them at a copy that stays. */
        free(copy);
        doc->text = text;
    } else {
        free(copy);
    }
    return result;
}

static xiaotai_err_t parse_text(const char *text, size_t *count, xiaotai_json_doc_t *doc)
{
    return parse_both(text, strlen(text), TABLE, count, doc);
}

static void check_token(const xiaotai_json_doc_t *doc, int index, int type, int parent, int children,
                        const char *raw)
{
    const xiaotai_json_token_t *token = &doc->tokens[index];
    CHECK_TEXT(token->type == type, raw);
    CHECK_TEXT(token->parent == parent, raw);
    CHECK_TEXT(token->children == children, raw);
    CHECK_TEXT(token->reserved == 0, raw);
    CHECK_TEXT(token->end - token->start == strlen(raw), raw);
    CHECK_TEXT(memcmp(doc->text + token->start, raw, strlen(raw)) == 0, raw);
}

static void test_rfc_examples(void)
{
    /* RFC 8259 section 13, first example. */
    static const char image[] =
        "{\n"
        "  \"Image\": {\n"
        "    \"Width\":  800,\n"
        "    \"Height\": 600,\n"
        "    \"Title\":  \"View from 15th Floor\",\n"
        "    \"Thumbnail\": {\n"
        "      \"Url\":    \"http://www.example.com/image/481989943\",\n"
        "      \"Height\": 125,\n"
        "      \"Width\":  100\n"
        "    },\n"
        "    \"Animated\" : false,\n"
        "    \"IDs\": [116, 943, 234, 38793]\n"
        "  }\n"
        "}\n";
    size_t count = 0;
    xiaotai_json_doc_t doc;
    CHECK(parse_text(image, &count, &doc) == XIAOTAI_OK);
    CHECK(count == 25 && doc.count == 25 && doc.tokens == table && doc.length == strlen(image));
    int64_t value = 0;
    CHECK(xiaotai_json_int(&doc, xiaotai_json_find(&doc, 0, "Image.Thumbnail.Width"), &value) == XIAOTAI_OK &&
          value == 100);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_find(&doc, 0, "Image.IDs[3]"), &value) == XIAOTAI_OK &&
          value == 38793);
    bool animated = true;
    CHECK(xiaotai_json_bool(&doc, xiaotai_json_find(&doc, 0, "Image.Animated"), &animated) == XIAOTAI_OK &&
          !animated);
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "Image.Title"), "View from 15th Floor"));
    CHECK(doc.tokens[xiaotai_json_find(&doc, 0, "Image")].children == 6);
    CHECK(doc.tokens[xiaotai_json_find(&doc, 0, "Image.IDs")].children == 4);

    /* Second example: an array of two objects with fractions. */
    static const char places[] =
        "[\n"
        "  {\n"
        "     \"precision\": \"zip\",\n"
        "     \"Latitude\":  37.7668,\n"
        "     \"Longitude\": -122.3959,\n"
        "     \"Address\":   \"\",\n"
        "     \"City\":      \"SAN FRANCISCO\",\n"
        "     \"State\":     \"CA\",\n"
        "     \"Zip\":       \"94107\",\n"
        "     \"Country\":   \"US\"\n"
        "  },\n"
        "  {\n"
        "     \"precision\": \"zip\",\n"
        "     \"Latitude\":  37.371991,\n"
        "     \"Longitude\": -122.026020,\n"
        "     \"Address\":   \"\",\n"
        "     \"City\":      \"SUNNYVALE\",\n"
        "     \"State\":     \"CA\",\n"
        "     \"Zip\":       \"94085\",\n"
        "     \"Country\":   \"US\"\n"
        "  }\n"
        "]\n";
    CHECK(parse_text(places, &count, &doc) == XIAOTAI_OK);
    CHECK(count == 1 + 2 * (1 + 16));
    CHECK(doc.tokens[0].children == 2 && doc.tokens[1].children == 8);
    size_t length = 0;
    const char *raw = xiaotai_json_raw(&doc, xiaotai_json_find(&doc, 0, "[1].Longitude"), &length);
    CHECK(raw && length == 11 && memcmp(raw, "-122.026020", 11) == 0);
    CHECK(xiaotai_json_int(&doc, xiaotai_json_find(&doc, 0, "[0].Latitude"), &value) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "[1].City"), "SUNNYVALE"));
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "[0].Address"), ""));

    /* A text may be any single value (RFC 8259 section 2). */
    CHECK(parse_text("\"Hello world!\"", &count, &doc) == XIAOTAI_OK && count == 1);
    check_token(&doc, 0, XIAOTAI_JSON_STRING, -1, 0, "Hello world!");
    CHECK(parse_text("42", &count, &doc) == XIAOTAI_OK && count == 1);
    check_token(&doc, 0, XIAOTAI_JSON_NUMBER, -1, 0, "42");
    CHECK(parse_text("true", &count, &doc) == XIAOTAI_OK && count == 1);
    check_token(&doc, 0, XIAOTAI_JSON_TRUE, -1, 0, "true");
}

static void test_token_layout(void)
{
    static const char text[] = " {\"a\":[1,{\"b\":null}],\"c\":\"x\\ny\",\"d\":[],\"e\":{}} ";
    size_t count = 0;
    xiaotai_json_doc_t doc;
    CHECK(parse_text(text, &count, &doc) == XIAOTAI_OK);
    CHECK(count == 13);
    /* Text order; a key's parent is its object, a value's parent its key, an
     * element's parent its array; strings exclude their quotes. */
    check_token(&doc, 0, XIAOTAI_JSON_OBJECT, -1, 4, "{\"a\":[1,{\"b\":null}],\"c\":\"x\\ny\",\"d\":[],\"e\":{}}");
    check_token(&doc, 1, XIAOTAI_JSON_STRING, 0, 0, "a");
    check_token(&doc, 2, XIAOTAI_JSON_ARRAY, 1, 2, "[1,{\"b\":null}]");
    check_token(&doc, 3, XIAOTAI_JSON_NUMBER, 2, 0, "1");
    check_token(&doc, 4, XIAOTAI_JSON_OBJECT, 2, 1, "{\"b\":null}");
    check_token(&doc, 5, XIAOTAI_JSON_STRING, 4, 0, "b");
    check_token(&doc, 6, XIAOTAI_JSON_NULL, 5, 0, "null");
    check_token(&doc, 7, XIAOTAI_JSON_STRING, 0, 0, "c");
    check_token(&doc, 8, XIAOTAI_JSON_STRING, 7, 0, "x\\ny");
    check_token(&doc, 9, XIAOTAI_JSON_STRING, 0, 0, "d");
    check_token(&doc, 10, XIAOTAI_JSON_ARRAY, 9, 0, "[]");
    check_token(&doc, 11, XIAOTAI_JSON_STRING, 0, 0, "e");
    check_token(&doc, 12, XIAOTAI_JSON_OBJECT, 11, 0, "{}");
    CHECK(doc.tokens[0].start == 1);

    /* Duplicate keys are kept; lookups return the first. */
    CHECK(parse_text("{\"a\":1,\"a\":2}", &count, &doc) == XIAOTAI_OK && count == 5);
    CHECK(doc.tokens[0].children == 2);
    int64_t value = 0;
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "a"), &value) == XIAOTAI_OK && value == 1);
}

static void test_accepted(void)
{
    static const char *const texts[] = {
        "0", "-0", "1", "-1", "10", "123456789012345678901234567890", "0.5", "-0.5", "1e5", "1E5", "1e+5",
        "1e-5", "1.25E-10", "0e0", "-0.0e-0", "true", "false", "null", "\"\"", "\"a\"",
        "\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t\"", "\"\\u0041\\u00e9\\u4E2D\"", "\"\\ud83d\\ude00\"",
        "\"\\uDEAD\"",           /* grammar allows lone surrogates; xiaotai_json_string refuses them */
        "\"\\u0000\"",           /* likewise NUL */
        "\"\xe4\xb8\xad\xe6\x96\x87\"", "\"\x7f\"", "[]", "{}", "[ ]", "{ }", " \t\r\n[1] \t\r\n",
        "[1,2,3]", "[[],[[]],{}]", "{\"\":0}", "{\"a\":{\"b\":{\"c\":[true,false,null]}}}",
        "[\"]\",\"}\",\"[\",\"{\",\",\",\":\"]", "{\"a\" : 1 , \"b\" : [ 2 , 3 ] }",
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        size_t count = 0;
        xiaotai_json_doc_t doc;
        CHECK_TEXT(parse_text(texts[i], &count, &doc) == XIAOTAI_OK, texts[i]);
        CHECK_TEXT(count > 0 && doc.count == count, texts[i]);
    }
}

typedef struct {
    const char *text;
    size_t length; /* 0: strlen(text) */
} sample_t;

static void test_refused(void)
{
    static const sample_t samples[] = {
        {"", 0}, {" ", 0}, {"\t\r\n", 0},
        /* structure */
        {"{", 0}, {"}", 0}, {"[", 0}, {"]", 0}, {"[1", 0}, {"[1,", 0}, {"{\"a\"", 0}, {"{\"a\":", 0},
        {"{\"a\":1", 0}, {"[1,]", 0}, {"[,1]", 0}, {"[,]", 0}, {"{,}", 0}, {"{\"a\":1,}", 0}, {"[1 2]", 0},
        {"{\"a\" 1}", 0}, {"{\"a\":}", 0}, {"{\"a\":1 \"b\":2}", 0}, {"{a:1}", 0}, {"{1:1}", 0},
        {"{\"a\"::1}", 0}, {"[}", 0}, {"{]", 0}, {"[1}", 0}, {"{\"a\":1]", 0}, {"[]]", 0}, {"{}}", 0},
        /* one top-level value, no trailing data */
        {"1 2", 0}, {"[] []", 0}, {"{}x", 0}, {"[1]\0", 4}, {"\"a\"\"b\"", 0}, {"1,", 0},
        /* no comments, no byte-order mark, no single quotes */
        {"// c\n1", 0}, {"/* c */ 1", 0}, {"[1] // c", 0}, {"\xef\xbb\xbf" "1", 0}, {"'a'", 0},
        /* numbers as RFC 8259 writes them */
        {"01", 0}, {"-01", 0}, {"00", 0}, {"1.", 0}, {".5", 0}, {"-", 0}, {"+1", 0}, {"1e", 0}, {"1e+", 0},
        {"1E-", 0}, {"1.e5", 0}, {"0x10", 0}, {"1_000", 0}, {"- 1", 0}, {"NaN", 0}, {"Infinity", 0},
        {"-Infinity", 0}, {"1.5.5", 0},
        /* literals */
        {"tru", 0}, {"nul", 0}, {"fals", 0}, {"True", 0}, {"NULL", 0}, {"truex", 0}, {"[nulll]", 0},
        /* strings */
        {"\"abc", 0}, {"\"\\\"", 0}, {"\"\\", 0}, {"\"\t\"", 0}, {"\"\n\"", 0}, {"\"\x01\"", 0},
        {"\"a\0b\"", 5}, {"\"\\x\"", 0}, {"\"\\a\"", 0}, {"\"\\U0041\"", 0}, {"\"\\u12\"", 0},
        {"\"\\u12G4\"", 0}, {"\"\\u\"", 0}, {"\"\\u004", 0}, {"\"\\'\"", 0}, {"\"\\0\"", 0},
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        const char *text = samples[i].text;
        size_t length = samples[i].length ? samples[i].length : strlen(text);
        size_t count = 0;
        xiaotai_json_doc_t doc = {.tokens = table, .count = 7};
        CHECK_TEXT(parse_both(text, length, TABLE, &count, &doc) == XIAOTAI_ERR_INVALID_ARG, text);
        /* A failed parse leaves no table for the queries. */
        CHECK_TEXT(doc.tokens == NULL && doc.count == 0, text);
        CHECK_TEXT(xiaotai_json_find(&doc, 0, "") == -1, text);
    }
}

/* Where this reader is stricter than the cJSON 1.7.19 parsing the existing
 * firmware does (table "cJSON 差异" in the J1 notes): each difference is pinned
 * here so a module's difference table can point at it. Numbering follows that
 * table. */
static void test_cjson_differences(void)
{
    size_t count = 0;
    xiaotai_json_doc_t doc;
    static const sample_t refused[] = {
        /* 1: trailing data; cJSON_ParseWithLength ignores what follows the value. */
        {"{\"code\":200}\0", 13}, {"{\"code\":200}\0\0", 14}, {"{\"code\":200}x", 0}, {"[1] ]", 0},
        /* 2: UTF-8 byte-order mark; cJSON skips it. */
        {"\xef\xbb\xbf{\"a\":1}", 0},
        /* 3: bytes up to 0x20 other than space, tab, CR, LF; cJSON skips all of them. */
        {"\v1", 0}, {"1\f", 0}, {"[1,\x01 2]", 0}, {"\0[1]", 4},
        /* 4: unescaped control characters in strings; cJSON copies them. */
        {"\"a\x1f\"", 0}, {"{\"k\":\"\r\"}", 0},
        /* 7: number spellings cJSON's strtod reads. */
        {"[01]", 0}, {"[-01]", 0}, {"[1.]", 0}, {"[1.e2]", 0},
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        const char *text = refused[i].text;
        size_t length = refused[i].length ? refused[i].length : strlen(text);
        CHECK_TEXT(parse_both(text, length, TABLE, &count, &doc) == XIAOTAI_ERR_INVALID_ARG, text);
    }

    /* 1, caller side: a peer length that counts the sender's terminator parses
     * once the trailing NUL bytes are stripped, as the module notes say. */
    static const char sent[] = "{\"code\":200}\0";
    size_t length = sizeof(sent) - 1U; /* the 13 bytes the peer reports */
    while (length > 0 && sent[length - 1U] == '\0') {
        --length;
    }
    CHECK(length == 12 && parse_both(sent, length, TABLE, &count, &doc) == XIAOTAI_OK && count == 3);

    /* 5: keys compare case-sensitively; cJSON_GetObjectItem ignores case. */
    CHECK(parse_text("{\"Code\":200}", &count, &doc) == XIAOTAI_OK);
    CHECK(xiaotai_json_get(&doc, 0, "code") == -1 && xiaotai_json_get(&doc, 0, "Code") == 2);

    /* 6: integers are exact; cJSON's valueint truncates fractions and reads
     * exponents (all three are 200 there). */
    CHECK(parse_text("{\"a\":200.0,\"b\":2e2,\"c\":200.5}", &count, &doc) == XIAOTAI_OK);
    static const char *const keys[] = {"a", "b", "c"};
    for (size_t i = 0; i < 3; ++i) {
        int64_t value = 5;
        CHECK_TEXT(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, keys[i]), &value) == XIAOTAI_ERR_INVALID_ARG &&
                       value == 5,
                   keys[i]);
    }

    /* 9: \u0000 is refused when the string is read; cJSON ends the string there. */
    CHECK(parse_text("{\"s\":\"ab\\u0000cd\"}", &count, &doc) == XIAOTAI_OK);
    char out[8];
    CHECK(xiaotai_json_string(&doc, xiaotai_json_get(&doc, 0, "s"), out, sizeof(out), NULL) ==
          XIAOTAI_ERR_INVALID_ARG);

    /* 10 (looser, not stricter): a lone surrogate fails the whole cJSON parse;
     * here the document parses and only reading that string fails. */
    CHECK(parse_text("{\"s\":\"\\ud800\",\"n\":1}", &count, &doc) == XIAOTAI_OK);
    int64_t n = 0;
    CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "n"), &n) == XIAOTAI_OK && n == 1);
    CHECK(xiaotai_json_string(&doc, xiaotai_json_get(&doc, 0, "s"), out, sizeof(out), NULL) ==
          XIAOTAI_ERR_INVALID_ARG);
}

static void test_error_count(void)
{
    /* *count: the tokens up to the error. */
    size_t count = 99;
    CHECK(parse_text("[1,2,x]", &count, NULL) == XIAOTAI_ERR_INVALID_ARG && count == 3);
    CHECK(parse_text("{\"a\":1,\"b\"}", &count, NULL) == XIAOTAI_ERR_INVALID_ARG && count == 4);
    CHECK(parse_text("x", &count, NULL) == XIAOTAI_ERR_INVALID_ARG && count == 0);
    CHECK(parse_text("[[[]]] 1", &count, NULL) == XIAOTAI_ERR_INVALID_ARG && count == 3);
}

static void test_capacity(void)
{
    static const char text[] = "{\"a\":[1,2,3],\"b\":{\"c\":null}}";
    const size_t needed = 10;
    char *copy = json_test_exact(text, strlen(text));
    size_t count = 0;
    xiaotai_json_doc_t doc;

    CHECK(xiaotai_json_parse(copy, strlen(text), NULL, 0, &count, NULL) == XIAOTAI_OK && count == needed);
    /* tokens NULL means capacity 0 whatever capacity says: counting. */
    CHECK(xiaotai_json_parse(copy, strlen(text), NULL, 50, &count, &doc) == XIAOTAI_OK && count == needed);
    CHECK(doc.tokens == NULL && doc.count == 0);

    for (size_t capacity = 0; capacity < needed; ++capacity) {
        /* One short: INVALID_SIZE and how many the whole text needs; no
         * token is written past the capacity (the table is exact-size). */
        xiaotai_json_token_t *small = (xiaotai_json_token_t *)malloc((capacity ? capacity : 1) * sizeof(*small));
        count = 0;
        CHECK(xiaotai_json_parse(copy, strlen(text), small, capacity, &count, &doc) == XIAOTAI_ERR_INVALID_SIZE);
        CHECK(count == needed);
        CHECK(doc.tokens == NULL && doc.count == 0);
        free(small);
    }
    xiaotai_json_token_t *exact = (xiaotai_json_token_t *)malloc(needed * sizeof(*exact));
    CHECK(xiaotai_json_parse(copy, strlen(text), exact, needed, &count, &doc) == XIAOTAI_OK && count == needed);
    CHECK(doc.tokens == exact && doc.count == needed);
    /* The capacity-limited table equals the large one. */
    xiaotai_json_doc_t large;
    CHECK(xiaotai_json_parse(copy, strlen(text), table, TABLE, &count, &large) == XIAOTAI_OK);
    CHECK(memcmp(exact, table, needed * sizeof(*exact)) == 0);
    free(exact);

    /* A table too small and a syntax error later: the syntax error wins. */
    xiaotai_json_token_t two[2];
    CHECK(xiaotai_json_parse("[1,2,3,]", 8, two, 2, &count, NULL) == XIAOTAI_ERR_INVALID_ARG && count == 4);
    /* count may be NULL. */
    CHECK(xiaotai_json_parse(copy, strlen(text), table, TABLE, NULL, &doc) == XIAOTAI_OK && doc.count == needed);
    free(copy);
}

/* depth nested containers of one kind around a 0 (arrays) or {} (objects). */
static char *nested(size_t depth, bool objects, size_t *length)
{
    json_test_builder_t b = {0};
    for (size_t i = 0; i < depth; ++i) {
        json_test_append(&b, "%s", objects ? (i + 1 == depth ? "{" : "{\"k\":") : "[");
    }
    if (!objects) {
        json_test_append(&b, "%s", "0");
    }
    json_test_repeat(&b, objects ? "}" : "]", depth);
    *length = b.length;
    return b.text;
}

static void test_depth(void)
{
    for (int objects = 0; objects < 2; ++objects) {
        size_t length = 0;
        char *ok = nested(XIAOTAI_JSON_DEPTH_MAX, objects, &length);
        size_t count = 0;
        xiaotai_json_doc_t doc;
        CHECK(parse_both(ok, length, TABLE, &count, &doc) == XIAOTAI_OK);
        CHECK(count == (objects ? 2 * XIAOTAI_JSON_DEPTH_MAX - 1 : XIAOTAI_JSON_DEPTH_MAX + 1));
        free(ok);

        char *deep = nested(XIAOTAI_JSON_DEPTH_MAX + 1, objects, &length);
        CHECK(parse_both(deep, length, TABLE, &count, &doc) == XIAOTAI_ERR_INVALID_SIZE);
        /* The tokens up to the 33rd opening bracket. */
        CHECK(count == (objects ? 2 * XIAOTAI_JSON_DEPTH_MAX : XIAOTAI_JSON_DEPTH_MAX));
        size_t counted = 0;
        CHECK(xiaotai_json_parse(deep, length, NULL, 0, &counted, NULL) == XIAOTAI_ERR_INVALID_SIZE);
        free(deep);
    }
    /* The limit is on open containers, not on how many there are. */
    json_test_builder_t b = {0};
    json_test_append(&b, "%s", "[");
    for (int i = 0; i < 100; ++i) {
        json_test_append(&b, "%s", i ? "," : "");
        json_test_repeat(&b, "[", XIAOTAI_JSON_DEPTH_MAX - 1);
        json_test_repeat(&b, "]", XIAOTAI_JSON_DEPTH_MAX - 1);
    }
    json_test_append(&b, "%s", "]");
    size_t count = 0;
    CHECK(xiaotai_json_parse(b.text, b.length, NULL, 0, &count, NULL) == XIAOTAI_OK &&
          count == 1 + 100 * (XIAOTAI_JSON_DEPTH_MAX - 1));
    free(b.text);
}

static void test_children_saturate(void)
{
    json_test_builder_t b = {0};
    json_test_append(&b, "%s", "[");
    for (size_t i = 0; i < 70000; ++i) {
        json_test_append(&b, "%s%zu", i ? "," : "", i);
    }
    json_test_append(&b, "%s", "]");
    xiaotai_json_token_t *tokens = (xiaotai_json_token_t *)malloc(70001 * sizeof(*tokens));
    size_t count = 0;
    xiaotai_json_doc_t doc;
    CHECK(xiaotai_json_parse(b.text, b.length, tokens, 70001, &count, &doc) == XIAOTAI_OK && count == 70001);
    CHECK(tokens[0].children == UINT16_MAX);
    /* Positions past the saturated count still resolve. */
    int64_t value = 0;
    CHECK(xiaotai_json_int(&doc, xiaotai_json_at(&doc, 0, 69999), &value) == XIAOTAI_OK && value == 69999);
    CHECK(xiaotai_json_at(&doc, 0, 70000) == -1);
    free(tokens);
    free(b.text);
}

static void test_arguments(void)
{
    size_t count = 7;
    xiaotai_json_doc_t doc;
    CHECK(xiaotai_json_parse(NULL, 5, table, TABLE, &count, &doc) == XIAOTAI_ERR_INVALID_ARG && count == 0);
    CHECK(xiaotai_json_parse(NULL, 0, table, TABLE, &count, &doc) == XIAOTAI_ERR_INVALID_ARG);
#if SIZE_MAX > 0x7FFFFFFFU
    /* Longer than the limit: refused before a byte is read (the buffer is
     * one byte; the sanitizer would report any read). */
    char *one = json_test_exact("1", 1);
    CHECK(xiaotai_json_parse(one, (size_t)XIAOTAI_JSON_TEXT_MAX + 1, table, TABLE, &count, &doc) ==
          XIAOTAI_ERR_INVALID_SIZE);
    CHECK(count == 0 && doc.tokens == NULL);
    free(one);
#endif
}

static void test_no_allocation(void)
{
    /* Every function is [rt]: nothing goes through the port's allocator. */
    xiaotai_mem_host_clear();
    size_t count = 0;
    xiaotai_json_doc_t doc;
    static const char text[] = "{\"a\":[1,2,{\"b\":\"\\u00e9\"}]}";
    CHECK(xiaotai_json_parse(text, strlen(text), table, TABLE, &count, &doc) == XIAOTAI_OK);
    CHECK(xiaotai_json_parse(text, strlen(text), NULL, 0, &count, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_json_count(text, strlen(text), "a", &count) == XIAOTAI_OK && count == 3);
    char out[8];
    CHECK(xiaotai_json_string(&doc, xiaotai_json_find(&doc, 0, "a[2].b"), out, sizeof(out), NULL) == XIAOTAI_OK);
    char buffer[64];
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, "b", out);
    xiaotai_json_object_end(&writer);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_allocations() == 0 && xiaotai_mem_host_outstanding() == 0);
}

int main(void)
{
    test_rfc_examples();
    test_token_layout();
    test_accepted();
    test_refused();
    test_cjson_differences();
    test_error_count();
    test_capacity();
    test_depth();
    test_children_saturate();
    test_arguments();
    test_no_allocation();
    if (json_test_failures) {
        fprintf(stderr, "json_parse: %d checks failed\n", json_test_failures);
        return 1;
    }
    printf("json_parse: ok\n");
    return 0;
}
