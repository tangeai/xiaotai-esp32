// SPDX-License-Identifier: MIT
/* Writer: exact output of a nested document (commas, keys, every value
 * kind), escaping of quotes, backslashes and every control character with raw
 * UTF-8 passed unchanged, the whole int64 range as exact decimal text, raw
 * sub-objects copied unchanged, a round trip through the reader, overflow at
 * every buffer size (the buffer keeps complete items and its terminator, later
 * calls are no-ops, nothing is written past the capacity), 32 levels written
 * and the 33rd refused, the end states (open container, nothing written,
 * misuse that would not be JSON), and against a model of correct use: over
 * many generated call sequences writer_end returns XIAOTAI_OK exactly for
 * correct ones, with the model's text, and never for one with misuse. */
#include <stdbool.h>
#include <stdint.h>
#include "json_test.h"
#include "xiaotai/json.h"

/* The sample document; returns what writer_end says. */
static xiaotai_err_t build(char *buffer, size_t capacity, size_t *length)
{
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, buffer, capacity);
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, "type", "signal");
    xiaotai_json_put_int(&writer, "seq", 42);
    xiaotai_json_put_bool(&writer, "ok", true);
    xiaotai_json_put_bool(&writer, "no", false);
    xiaotai_json_put_null(&writer, "none");
    xiaotai_json_put_string(&writer, "nil", NULL);
    xiaotai_json_object_begin(&writer, "data");
    xiaotai_json_array_begin(&writer, "list");
    xiaotai_json_put_int(&writer, NULL, -1);
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_object_end(&writer);
    xiaotai_json_array_begin(&writer, NULL);
    xiaotai_json_array_end(&writer);
    xiaotai_json_put_string_n(&writer, NULL, "abcdef", 3);
    xiaotai_json_array_end(&writer);
    xiaotai_json_put_raw(&writer, "raw", "{\"x\": [1, 2]}", 13);
    xiaotai_json_object_end(&writer);
    xiaotai_json_object_end(&writer);
    return xiaotai_json_writer_end(&writer, length);
}

static const char expected[] =
    "{\"type\":\"signal\",\"seq\":42,\"ok\":true,\"no\":false,\"none\":null,\"nil\":null,"
    "\"data\":{\"list\":[-1,{},[],\"abc\"],\"raw\":{\"x\": [1, 2]}}}";

static void test_document(void)
{
    char buffer[256];
    size_t length = 0;
    CHECK(build(buffer, sizeof(buffer), &length) == XIAOTAI_OK);
    CHECK(length == strlen(expected) && strcmp(buffer, expected) == 0);

    /* Round trip through the reader. */
    xiaotai_json_token_t tokens[32];
    xiaotai_json_doc_t doc;
    size_t count = 0;
    CHECK(xiaotai_json_parse(buffer, length, tokens, 32, &count, &doc) == XIAOTAI_OK);
    int64_t value = 0;
    CHECK(xiaotai_json_int(&doc, xiaotai_json_find(&doc, 0, "data.list[0]"), &value) == XIAOTAI_OK && value == -1);
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_find(&doc, 0, "data.list[3]"), "abc"));
    CHECK(xiaotai_json_int(&doc, xiaotai_json_find(&doc, 0, "data.raw.x[1]"), &value) == XIAOTAI_OK && value == 2);

    /* Top-level values other than objects. */
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_put_int(&writer, NULL, 7);
    CHECK(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_OK && strcmp(buffer, "7") == 0 && length == 1);
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_array_begin(&writer, NULL);
    xiaotai_json_array_end(&writer);
    CHECK(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_OK && strcmp(buffer, "[]") == 0);
}

static void test_escaping(void)
{
    char input[40];
    size_t n = 0;
    for (int c = 1; c < 0x20; ++c) {
        input[n++] = (char)c;
    }
    input[n++] = '"';
    input[n++] = '\\';
    input[n++] = '/';
    input[n++] = 0x7f;
    char buffer[512];
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_array_begin(&writer, NULL);
    xiaotai_json_put_string_n(&writer, NULL, input, n);
    /* Exactly length bytes: an embedded NUL is written as \u0000. */
    xiaotai_json_put_string_n(&writer, NULL, "a\0b", 3);
    xiaotai_json_put_string(&writer, NULL, "\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80");
    xiaotai_json_put_string_n(&writer, NULL, NULL, 0);
    xiaotai_json_array_end(&writer);
    size_t length = 0;
    CHECK(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_OK);
    const char *want =
        "[\"\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f"
        "\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d"
        "\\u001e\\u001f\\\"\\\\/\x7f\",\"a\\u0000b\",\"\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80\",\"\"]";
    CHECK(strcmp(buffer, want) == 0 && length == strlen(want));

    /* The reader decodes it back to the same bytes. */
    xiaotai_json_token_t tokens[8];
    xiaotai_json_doc_t doc;
    size_t count = 0;
    CHECK(xiaotai_json_parse(buffer, length, tokens, 8, &count, &doc) == XIAOTAI_OK && count == 5);
    char out[64];
    size_t decoded = 0;
    CHECK(xiaotai_json_string(&doc, 1, out, sizeof(out), &decoded) == XIAOTAI_OK);
    CHECK(decoded == n && memcmp(out, input, n) == 0);

    /* Keys are escaped like values. */
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_int(&writer, "a\"b\n", 1);
    xiaotai_json_object_end(&writer);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_OK && strcmp(buffer, "{\"a\\\"b\\n\":1}") == 0);
}

static void test_integers(void)
{
    static const struct {
        int64_t value;
        const char *text;
    } cases[] = {
        {0, "0"},
        {-1, "-1"},
        {9, "9"},
        {10, "10"},
        {XIAOTAI_JSON_INT_MAX, "9007199254740991"},
        {-XIAOTAI_JSON_INT_MAX, "-9007199254740991"},
        {INT64_MAX, "9223372036854775807"},
        {INT64_MIN, "-9223372036854775808"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char buffer[32];
        xiaotai_json_writer_t writer;
        xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
        xiaotai_json_put_int(&writer, NULL, cases[i].value);
        CHECK_TEXT(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_OK && strcmp(buffer, cases[i].text) == 0,
                   cases[i].text);
    }
}

static void test_overflow(void)
{
    const size_t needed = strlen(expected) + 1;
    for (size_t capacity = 0; capacity <= needed; ++capacity) {
        /* Exact-size buffer: the sanitizer reports any byte past capacity. */
        char *buffer = (char *)malloc(capacity ? capacity : 1);
        size_t length = 99;
        xiaotai_err_t err = build(capacity ? buffer : NULL, capacity, &length);
        if (capacity < needed) {
            CHECK(err == XIAOTAI_ERR_INVALID_SIZE && length == 0);
            if (capacity > 0) {
                /* A terminated prefix of the expected text, ending after a
                 * complete item. */
                size_t kept = strlen(buffer);
                CHECK(kept < capacity && memcmp(buffer, expected, kept) == 0);
                CHECK(kept == 0 || strchr("{[,", expected[kept]) || strchr("}]", expected[kept]) ||
                      expected[kept - 1] == '{' || expected[kept - 1] == '[');
            }
        } else {
            CHECK(err == XIAOTAI_OK && length == strlen(expected) && strcmp(buffer, expected) == 0);
        }
        free(buffer);
    }

    /* After an overflow every call is a no-op, even one that would fit. */
    char buffer[8];
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_array_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, NULL, "too long for it");
    CHECK(writer.overflow && strcmp(buffer, "[") == 0 && writer.length == 1);
    xiaotai_json_put_int(&writer, NULL, 1);
    xiaotai_json_array_end(&writer);
    CHECK(strcmp(buffer, "[") == 0);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE);

    /* The terminator always has its byte: 2 bytes hold "1", not "12". */
    xiaotai_json_writer_init(&writer, buffer, 2);
    xiaotai_json_put_int(&writer, NULL, 12);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE && buffer[0] == '\0');
    xiaotai_json_writer_init(&writer, buffer, 2);
    xiaotai_json_put_int(&writer, NULL, 1);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_OK && strcmp(buffer, "1") == 0);
}

static void test_depth(void)
{
    /* Room for 33 levels, so only the nesting limit can stop the writer. */
    char buffer[256];
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    for (unsigned i = 0; i < XIAOTAI_JSON_DEPTH_MAX; ++i) {
        xiaotai_json_array_begin(&writer, NULL);
    }
    xiaotai_json_put_int(&writer, NULL, 0);
    xiaotai_json_put_int(&writer, NULL, 1);
    for (unsigned i = 0; i < XIAOTAI_JSON_DEPTH_MAX; ++i) {
        xiaotai_json_array_end(&writer);
    }
    size_t length = 0;
    CHECK(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_OK && length == 2 * XIAOTAI_JSON_DEPTH_MAX + 3);
    /* The reader accepts what the writer wrote at the deepest level. */
    size_t count = 0;
    CHECK(xiaotai_json_parse(buffer, length, NULL, 0, &count, NULL) == XIAOTAI_OK &&
          count == XIAOTAI_JSON_DEPTH_MAX + 2);

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    for (unsigned i = 0; i < XIAOTAI_JSON_DEPTH_MAX; ++i) {
        xiaotai_json_object_begin(&writer, i ? "k" : NULL);
    }
    CHECK(!writer.overflow);
    xiaotai_json_object_begin(&writer, "k");
    CHECK(writer.overflow);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE);
}

static void test_end_states(void)
{
    char buffer[64];
    xiaotai_json_writer_t writer;
    size_t length = 5;

    /* Something still open. */
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_int(&writer, "a", 1);
    CHECK(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_ERR_INVALID_STATE && length == 0);
    xiaotai_json_object_end(&writer);
    CHECK(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_OK && strcmp(buffer, "{\"a\":1}") == 0);

    /* Nothing written: not a JSON text. */
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    CHECK(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_ERR_INVALID_STATE && buffer[0] == '\0');

    /* What would not be JSON is refused like an overflow, keeping what was
     * written: a second top-level value, a key at the top level, an end with
     * nothing open, an empty raw value. */
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_put_int(&writer, NULL, 1);
    xiaotai_json_put_int(&writer, NULL, 2);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE && strcmp(buffer, "1") == 0);

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_put_int(&writer, "k", 1);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE && buffer[0] == '\0');

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_object_end(&writer);
    xiaotai_json_object_end(&writer);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE && strcmp(buffer, "{}") == 0);

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_array_begin(&writer, NULL);
    xiaotai_json_put_raw(&writer, NULL, "", 0);
    CHECK(writer.overflow && strcmp(buffer, "[") == 0);

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_array_begin(&writer, NULL);
    xiaotai_json_put_string_n(&writer, NULL, NULL, 3);
    CHECK(writer.overflow);

    /* No buffer: nothing can be written; NULL writers are ignored. */
    xiaotai_json_writer_init(&writer, NULL, 10);
    xiaotai_json_put_int(&writer, NULL, 1);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE);
    xiaotai_json_writer_init(NULL, buffer, sizeof(buffer));
    xiaotai_json_put_int(NULL, NULL, 1);
    xiaotai_json_object_end(NULL);
    CHECK(xiaotai_json_writer_end(NULL, &length) == XIAOTAI_ERR_INVALID_ARG);
}

/* Key and kind misuse. While the innermost level is empty its first item and
 * its end are checked at the call (the writer is refused and keeps what was
 * written); after items, writer_end finds it by reading the text back. Either
 * way writer_end never returns XIAOTAI_OK for text that is not JSON. */
static void test_kind_misuse(void)
{
    char buffer[128];
    xiaotai_json_writer_t writer;

    /* Refused at the call. */
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_int(&writer, NULL, 1); /* {1} */
    CHECK(writer.overflow && strcmp(buffer, "{") == 0);
    xiaotai_json_object_end(&writer);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE && strcmp(buffer, "{") == 0);

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_array_begin(&writer, NULL);
    xiaotai_json_put_int(&writer, "k", 1); /* ["k":1] */
    CHECK(writer.overflow && strcmp(buffer, "[") == 0);

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_array_begin(&writer, NULL);
    xiaotai_json_object_end(&writer); /* [} */
    CHECK(writer.overflow && strcmp(buffer, "[") == 0);
    CHECK(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE);

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_array_end(&writer); /* {] */
    CHECK(writer.overflow && strcmp(buffer, "{") == 0);

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_array_begin(&writer, NULL); /* {[ : a member without a key */
    CHECK(writer.overflow && strcmp(buffer, "{") == 0);

    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_array_begin(&writer, NULL);
    xiaotai_json_object_begin(&writer, "k"); /* ["k":{ */
    CHECK(writer.overflow && strcmp(buffer, "[") == 0);

    /* Found by writer_end: the level already had an item. */
    static const char *const late[] = {"{\"a\":1,2}", "[1,\"k\":2]", "[1}", "{\"a\":1]", "[[],{}}"};
    for (size_t i = 0; i < sizeof(late) / sizeof(late[0]); ++i) {
        xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
        switch (i) {
        case 0:
            xiaotai_json_object_begin(&writer, NULL);
            xiaotai_json_put_int(&writer, "a", 1);
            xiaotai_json_put_int(&writer, NULL, 2);
            xiaotai_json_object_end(&writer);
            break;
        case 1:
            xiaotai_json_array_begin(&writer, NULL);
            xiaotai_json_put_int(&writer, NULL, 1);
            xiaotai_json_put_int(&writer, "k", 2);
            xiaotai_json_array_end(&writer);
            break;
        case 2:
            xiaotai_json_array_begin(&writer, NULL);
            xiaotai_json_put_int(&writer, NULL, 1);
            xiaotai_json_object_end(&writer);
            break;
        case 3:
            xiaotai_json_object_begin(&writer, NULL);
            xiaotai_json_put_int(&writer, "a", 1);
            xiaotai_json_array_end(&writer);
            break;
        default:
            xiaotai_json_array_begin(&writer, NULL);
            xiaotai_json_array_begin(&writer, NULL);
            xiaotai_json_array_end(&writer);
            xiaotai_json_object_begin(&writer, NULL);
            xiaotai_json_object_end(&writer);
            xiaotai_json_object_end(&writer);
            break;
        }
        /* The calls themselves could not see it. */
        CHECK_TEXT(!writer.overflow && strcmp(buffer, late[i]) == 0, late[i]);
        size_t length = 77;
        CHECK_TEXT(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_ERR_INVALID_SIZE && length == 0, late[i]);
        CHECK_TEXT(writer.overflow, late[i]);
        /* Refused for good: a later writer_end says the same. */
        CHECK_TEXT(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE, late[i]);
    }

    /* A raw value is checked with the rest: text that is not JSON, or that
     * nests past XIAOTAI_JSON_DEPTH_MAX where it is placed. */
    static const char *const bad_raw[] = {"{", "1 2", "[1,]", "{\"a\"}", "01", "\"\x01\""};
    for (size_t i = 0; i < sizeof(bad_raw) / sizeof(bad_raw[0]); ++i) {
        xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
        xiaotai_json_array_begin(&writer, NULL);
        xiaotai_json_put_raw(&writer, NULL, bad_raw[i], strlen(bad_raw[i]));
        xiaotai_json_array_end(&writer);
        CHECK_TEXT(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_ERR_INVALID_SIZE, bad_raw[i]);
    }
    for (unsigned extra = 1; extra <= 2; ++extra) {
        xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
        for (unsigned i = 0; i < XIAOTAI_JSON_DEPTH_MAX - 1U; ++i) {
            xiaotai_json_array_begin(&writer, NULL);
        }
        /* 31 levels open: "[1]" makes 32, "[[1]]" makes 33. */
        const char *raw = extra == 1 ? "[1]" : "[[1]]";
        xiaotai_json_put_raw(&writer, NULL, raw, strlen(raw));
        for (unsigned i = 0; i < XIAOTAI_JSON_DEPTH_MAX - 1U; ++i) {
            xiaotai_json_array_end(&writer);
        }
        CHECK(xiaotai_json_writer_end(&writer, NULL) ==
              (extra == 1 ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_SIZE));
    }

    /* Checking the text at the end does not change it or the writer. */
    xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
    xiaotai_json_put_int(&writer, NULL, 1);
    size_t length = 0;
    CHECK(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_OK && length == 1);
    CHECK(!writer.overflow && strcmp(buffer, "1") == 0);
    CHECK(xiaotai_json_writer_end(&writer, &length) == XIAOTAI_OK && length == 1);
}

/* ------------------------------------------------- model of correct use */

#define MODEL_OPS 48U

typedef struct {
    char kinds[XIAOTAI_JSON_DEPTH_MAX + 1]; /* '{' or '[' per open level */
    bool has_items[XIAOTAI_JSON_DEPTH_MAX + 1];
    unsigned depth;
    bool top_written;
    bool misuse;
    char text[2048];
    size_t length;
} model_t;

typedef enum { OP_OBJECT, OP_ARRAY, OP_INT, OP_RAW, OP_END_OBJECT, OP_END_ARRAY, OP_KINDS } op_t;

static uint32_t rng_state;

static uint32_t rng(void)
{
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state >> 8;
}

static void model_append(model_t *m, const char *piece)
{
    size_t n = strlen(piece);
    if (m->length + n >= sizeof(m->text)) {
        abort(); /* the sequences are sized to fit */
    }
    memcpy(m->text + m->length, piece, n + 1);
    m->length += n;
}

/* Applies one call to the model; the item is what correct use allows or
 * misuse, after which the model stops rendering. */
static void model_step(model_t *m, op_t op, bool key)
{
    if (m->misuse) {
        return;
    }
    bool closing = op == OP_END_OBJECT || op == OP_END_ARRAY;
    if (closing) {
        char open = op == OP_END_OBJECT ? '{' : '[';
        if (m->depth == 0 || m->kinds[m->depth - 1] != open) {
            m->misuse = true;
            return;
        }
        --m->depth;
        model_append(m, op == OP_END_OBJECT ? "}" : "]");
        return;
    }
    /* An item: a key exactly inside an object, one top-level value. */
    if (m->depth == 0 ? (key || m->top_written) : (key != (m->kinds[m->depth - 1] == '{'))) {
        m->misuse = true;
        return;
    }
    /* Nesting: a container adds one level, the raw value "[1]" one too. */
    if ((op == OP_OBJECT || op == OP_ARRAY || op == OP_RAW) && m->depth + 1U > XIAOTAI_JSON_DEPTH_MAX) {
        m->misuse = true;
        return;
    }
    if (m->depth == 0) {
        m->top_written = true;
    } else {
        if (m->has_items[m->depth - 1]) {
            model_append(m, ",");
        }
        m->has_items[m->depth - 1] = true;
    }
    if (key) {
        model_append(m, "\"k\":");
    }
    switch (op) {
    case OP_OBJECT:
    case OP_ARRAY:
        model_append(m, op == OP_OBJECT ? "{" : "[");
        m->kinds[m->depth] = op == OP_OBJECT ? '{' : '[';
        m->has_items[m->depth] = false;
        ++m->depth;
        break;
    case OP_INT:
        model_append(m, "7");
        break;
    default:
        model_append(m, "[1]");
        break;
    }
}

static void writer_step(xiaotai_json_writer_t *writer, op_t op, bool key)
{
    const char *k = key ? "k" : NULL;
    switch (op) {
    case OP_OBJECT:
        xiaotai_json_object_begin(writer, k);
        break;
    case OP_ARRAY:
        xiaotai_json_array_begin(writer, k);
        break;
    case OP_INT:
        xiaotai_json_put_int(writer, k, 7);
        break;
    case OP_RAW:
        xiaotai_json_put_raw(writer, k, "[1]", 3);
        break;
    case OP_END_OBJECT:
        xiaotai_json_object_end(writer);
        break;
    default:
        xiaotai_json_array_end(writer);
        break;
    }
}

/* A call correct use allows in the model's state. */
static void correct_op(const model_t *m, op_t *op, bool *key)
{
    /* At the top level mostly a container, so that most texts nest. */
    unsigned pick = m->depth > 0 ? rng() % 6U : (rng() % 8U == 0 ? OP_INT : rng() % 2U);
    if (pick >= 4U) {
        *op = m->kinds[m->depth - 1] == '{' ? OP_END_OBJECT : OP_END_ARRAY;
        *key = false;
        return;
    }
    *op = (op_t)pick;
    *key = m->depth > 0 && m->kinds[m->depth - 1] == '{';
}

static void test_against_model(void)
{
    rng_state = 0x5EEDU;
    unsigned ok = 0, open = 0, refused = 0, found_at_end = 0;
    for (unsigned run = 0; run < 20000U; ++run) {
        static model_t m;
        memset(&m, 0, sizeof(m));
        char buffer[sizeof(m.text)];
        xiaotai_json_writer_t writer;
        xiaotai_json_writer_init(&writer, buffer, sizeof(buffer));
        /* Mostly correct calls with a misuse now and then, so that both
         * correct texts and every kind of misuse are reached. */
        unsigned misuse_one_in = 2U + run % 64U;
        unsigned ops = 1U + rng() % MODEL_OPS;
        /* Half the runs stop at the first misuse, half go on calling. */
        bool stop_at_misuse = rng() & 1U;
        for (unsigned i = 0; i < ops && !(m.depth == 0 && m.top_written && !m.misuse); ++i) {
            if (m.misuse && stop_at_misuse) {
                break;
            }
            op_t op;
            bool key;
            if (rng() % misuse_one_in == 0) {
                op = (op_t)(rng() % OP_KINDS);
                key = rng() & 1U;
            } else {
                correct_op(&m, &op, &key);
            }
            model_step(&m, op, key);
            writer_step(&writer, op, key);
        }
        if (rng() & 1U) {
            /* Close what the writer has open with the kinds the model
             * recorded. Without misuse the two are in step; after a run that
             * stopped at a misuse the calls did not see, the writer's open
             * levels are the model's (one fewer after an end of the wrong
             * kind), so the text is complete and only writer_end can tell. */
            while (!writer.overflow && writer.depth > 0 && writer.depth <= m.depth) {
                op_t op = m.kinds[writer.depth - 1] == '{' ? OP_END_OBJECT : OP_END_ARRAY;
                model_step(&m, op, false);
                writer_step(&writer, op, false);
            }
        }
        size_t length = 99;
        bool refused_by_calls = writer.overflow;
        xiaotai_err_t err = xiaotai_json_writer_end(&writer, &length);
        if (m.misuse) {
            CHECK_TEXT(err != XIAOTAI_OK && length == 0, buffer);
            ++refused;
            if (!refused_by_calls && err == XIAOTAI_ERR_INVALID_SIZE) {
                ++found_at_end;
            }
        } else if (m.depth > 0 || !m.top_written) {
            CHECK_TEXT(err == XIAOTAI_ERR_INVALID_STATE && length == 0, m.text);
            ++open;
        } else {
            CHECK_TEXT(err == XIAOTAI_OK && length == m.length && strcmp(buffer, m.text) == 0, m.text);
            CHECK_TEXT(xiaotai_json_parse(buffer, length, NULL, 0, NULL, NULL) == XIAOTAI_OK, m.text);
            ++ok;
        }
    }
    /* The generator reached every outcome in quantity, including misuse that
     * only the check in writer_end finds. */
    if (!(ok > 1000U && open > 1000U && refused > 1000U && found_at_end > 200U)) {
        fprintf(stderr, "model outcomes: ok=%u open=%u refused=%u found_at_end=%u\n", ok, open, refused,
                found_at_end);
        CHECK(!"every outcome reached");
    }
}

int main(void)
{
    test_document();
    test_escaping();
    test_integers();
    test_overflow();
    test_depth();
    test_end_states();
    test_kind_misuse();
    test_against_model();
    if (json_test_failures) {
        fprintf(stderr, "json_write: %d checks failed\n", json_test_failures);
        return 1;
    }
    printf("json_write: ok\n");
    return 0;
}
