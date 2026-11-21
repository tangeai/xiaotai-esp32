// SPDX-License-Identifier: MIT
/* Text helpers of xiaotai/at_command.h (package AT-1), built from source with
 * a fake of the reply seam (internal.h) so the test sees exactly which faults
 * xiaotai_at_printf reports. Every function at its edges: empty, too long,
 * signs, prefixes, odd hexadecimal, zero bytes, "-", short capacity. The
 * cases of the earlier consoles' parser tests are ported here: hexadecimal
 * remarks and Wi-Fi texts, the 64-character identifier limit, digit counts of
 * room and pass codes, page numbers, tap coordinates, device identifiers and
 * recognition revisions. */
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

#include "at_test.h"
#include "internal.h"
#include "xiaotai/at_command.h"

/* ------------------------------------------------------- fake of the seam */

static struct {
    unsigned calls;
    const xiaotai_reply_t *reply;
    xiaotai_at_output_fault_t fault;
} faults;

void xiaotai_at_reply_output_fault(const xiaotai_reply_t *reply, xiaotai_at_output_fault_t fault)
{
    ++faults.calls;
    faults.reply = reply;
    faults.fault = fault;
}

static void faults_clear(void)
{
    memset(&faults, 0, sizeof(faults));
}

/* ------------------------------------------------------------ error names */

static void test_error_names(void)
{
    static const struct {
        xiaotai_err_t code;
        const char *name;
    } names[] = {
        {XIAOTAI_FAIL, "FAIL"},
        {XIAOTAI_ERR_NO_MEM, "NO_MEM"},
        {XIAOTAI_ERR_INVALID_ARG, "INVALID_ARG"},
        {XIAOTAI_ERR_INVALID_STATE, "INVALID_STATE"},
        {XIAOTAI_ERR_INVALID_SIZE, "INVALID_SIZE"},
        {XIAOTAI_ERR_NOT_FOUND, "NOT_FOUND"},
        {XIAOTAI_ERR_NOT_SUPPORTED, "NOT_SUPPORTED"},
        {XIAOTAI_ERR_TIMEOUT, "TIMEOUT"},
        {XIAOTAI_ERR_BUSY, "BUSY"},
        {XIAOTAI_ERR_NOT_ALLOWED, "NOT_ALLOWED"},
    };
    char scratch[12];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        memset(scratch, 'x', sizeof(scratch));
        const char *name = xiaotai_at_error_name(names[i].code, scratch, sizeof(scratch));
        CHECK_TEXT(!strcmp(name, names[i].name), names[i].name);
        CHECK_TEXT(name != scratch && scratch[0] == 'x', names[i].name); /* constant, scratch untouched */
        /* Named codes need no scratch at all. */
        CHECK_TEXT(!strcmp(xiaotai_at_error_name(names[i].code, NULL, 0), names[i].name), names[i].name);
    }

    /* Any other code is its bit pattern in upper-case hexadecimal, in scratch. */
    static const struct {
        xiaotai_err_t code;
        const char *text;
    } others[] = {
        {0x3001, "0x3001"},              /* a platform code passed through */
        {XIAOTAI_AT_PASS, "0x7201"},     /* never meant to be printed, still well formed */
        {0x10A, "0x10A"},
        {XIAOTAI_OK, "0x0"},
        {-2, "0xFFFFFFFE"},
        {INT32_MIN, "0x80000000"},
    };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
        const char *name = xiaotai_at_error_name(others[i].code, scratch, sizeof(scratch));
        CHECK_TEXT(name == scratch && !strcmp(name, others[i].text), others[i].text);
    }

    /* No scratch, or one too short for the text and its terminator: FAIL. */
    CHECK(!strcmp(xiaotai_at_error_name(0x3001, NULL, 12), "FAIL"));
    CHECK(!strcmp(xiaotai_at_error_name(0x3001, scratch, 0), "FAIL"));
    memset(scratch, 'x', sizeof(scratch));
    CHECK(!strcmp(xiaotai_at_error_name(0x3001, scratch, 6), "FAIL") && scratch[0] == '\0');
    CHECK(xiaotai_at_error_name(0x3001, scratch, 7) == scratch && !strcmp(scratch, "0x3001"));
    /* The widest code fits the documented minimum capacity of 12. */
    CHECK(!strcmp(xiaotai_at_error_name(-2, scratch, 11), "0xFFFFFFFE"));
    CHECK(!strcmp(xiaotai_at_error_name(-2, scratch, 10), "FAIL"));
}

/* ------------------------------------------------------------ reply lines */

static void test_printf(void)
{
    at_test_sink_t sink;
    xiaotai_reply_t reply = at_test_reply(&sink);

    faults_clear();
    xiaotai_at_printf(&reply, "+CONSOLE: lines=%u ok=%" PRIu32 " name_hex=%s", 3U, (uint32_t)7, "4142");
    CHECK(sink.writes == 1 && !strcmp(sink.text, "+CONSOLE: lines=3 ok=7 name_hex=4142"));
    CHECK(faults.calls == 0);

    /* Exactly XIAOTAI_AT_OUT_LINE_MAX characters is one line, written whole. */
    char wide[XIAOTAI_AT_OUT_LINE_MAX + 2];
    memset(wide, 'a', sizeof(wide));
    wide[XIAOTAI_AT_OUT_LINE_MAX - 3] = '\0';
    reply = at_test_reply(&sink);
    xiaotai_at_printf(&reply, "+X:%s", wide);
    CHECK(sink.writes == 1 && strlen(sink.text) == XIAOTAI_AT_OUT_LINE_MAX && faults.calls == 0);

    /* One character more: nothing of it is written, the seam hears TOO_LONG
     * for this very reply, once. */
    wide[XIAOTAI_AT_OUT_LINE_MAX - 3] = 'a';
    wide[XIAOTAI_AT_OUT_LINE_MAX - 2] = '\0';
    reply = at_test_reply(&sink);
    xiaotai_at_printf(&reply, "+X:%s", wide);
    CHECK(sink.writes == 0 && sink.text[0] == '\0');
    CHECK(faults.calls == 1 && faults.reply == &reply && faults.fault == XIAOTAI_AT_OUTPUT_TOO_LONG);

    /* Far longer than the stack buffer: still not written, still one fault. */
    char *huge = (char *)malloc(4096);
    if (huge) {
        memset(huge, 'b', 4095);
        huge[4095] = '\0';
        faults_clear();
        reply = at_test_reply(&sink);
        xiaotai_at_printf(&reply, "%s", huge);
        CHECK(sink.writes == 0 && faults.calls == 1 && faults.fault == XIAOTAI_AT_OUTPUT_TOO_LONG);
        free(huge);
    }

    /* A line break in the result would end the record early and inject a
     * second line: not written, reported as malformed. */
    static const char *const broken[] = {"a\nb", "a\rb", "tail\n", "\r"};
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        faults_clear();
        reply = at_test_reply(&sink);
        xiaotai_at_printf(&reply, "+X: value=%s", broken[i]);
        CHECK(sink.writes == 0);
        CHECK(faults.calls == 1 && faults.reply == &reply && faults.fault == XIAOTAI_AT_OUTPUT_MALFORMED);
    }

    /* NULL reply, NULL write function or NULL format: no effect at all. */
    faults_clear();
    xiaotai_at_printf(NULL, "+X: a=%d", 1);
    reply = at_test_reply(&sink);
    xiaotai_reply_t silent = {NULL, &sink};
    xiaotai_at_printf(&silent, "+X: a=%d", 1);
    const char *volatile no_format = NULL; /* hidden from the compiler's format check */
    xiaotai_at_printf(&reply, no_format, 1);
    CHECK(sink.writes == 0 && faults.calls == 0);

    /* The reply context reaches the write function unchanged. */
    at_test_sink_t other;
    xiaotai_reply_t second = at_test_reply(&other);
    reply = at_test_reply(&sink);
    xiaotai_at_printf(&second, "+Y: b=%d", 2);
    CHECK(other.writes == 1 && !strcmp(other.text, "+Y: b=2") && sink.writes == 0);
}

/* ---------------------------------------------------------------- numbers */

static void test_parse_u32(void)
{
    uint32_t value = 77;
    CHECK(xiaotai_at_parse_u32("0", 0, 10, &value) && value == 0);
    CHECK(xiaotai_at_parse_u32("10", 0, 10, &value) && value == 10);
    CHECK(xiaotai_at_parse_u32("4294967295", 0, UINT32_MAX, &value) && value == UINT32_MAX);
    CHECK(xiaotai_at_parse_u32("0000000007", 0, 10, &value) && value == 7); /* ten digits */
    CHECK(xiaotai_at_parse_u32("5", 5, 5, &value) && value == 5);
    CHECK(xiaotai_at_parse_u32("12", 0, 100, NULL)); /* a pure check */

    static const struct {
        const char *text;
        uint32_t min, max;
    } bad[] = {
        {"", 0, 10}, {"11", 0, 10}, {"4", 5, 10}, {"4294967296", 0, UINT32_MAX},
        {"00000000007", 0, 10}, {"99999999999", 0, UINT32_MAX}, {"-1", 0, 10}, {"+1", 0, 10},
        {" 1", 0, 10}, {"1 ", 0, 10}, {"0x10", 0, 100}, {"1e2", 0, 1000}, {"1.0", 0, 10},
        {"1-", 0, 10}, {"5", 6, 4}, /* min above max: nothing is in range */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        value = 77;
        CHECK_TEXT(!xiaotai_at_parse_u32(bad[i].text, bad[i].min, bad[i].max, &value) && value == 77, bad[i].text);
    }
    CHECK(!xiaotai_at_parse_u32(NULL, 0, 10, &value) && value == 77);

    /* Tap coordinates: decimal, 0..65535, nothing else. */
    static const char *const coordinates[] = {"", "-1", "+0", "1e2", " 3", "3 ", "65536", "99999999999999"};
    for (size_t i = 0; i < sizeof(coordinates) / sizeof(coordinates[0]); ++i) {
        CHECK_TEXT(!xiaotai_at_parse_u32(coordinates[i], 0, UINT16_MAX, &value), coordinates[i]);
    }
    CHECK(!xiaotai_at_parse_u32(NULL, 0, UINT16_MAX, &value));
    CHECK(xiaotai_at_parse_u32("0", 0, UINT16_MAX, &value) && value == 0);
    CHECK(xiaotai_at_parse_u32("319", 0, UINT16_MAX, &value) && value == 319);
    CHECK(xiaotai_at_parse_u32("65535", 0, UINT16_MAX, &value) && value == 65535);

    /* Page numbers 0..65535, leading zeros allowed. */
    CHECK(xiaotai_at_parse_u32("00002", 0, UINT16_MAX, &value) && value == 2);
    static const char *const pages[] = {"-1", "+1", "65536", "12345678901234567890"};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        CHECK_TEXT(!xiaotai_at_parse_u32(pages[i], 0, UINT16_MAX, &value), pages[i]);
    }

    /* Volume 0..100, gain 0..36, recording seconds 1..60. */
    CHECK(xiaotai_at_parse_u32("100", 0, 100, &value) && value == 100);
    CHECK(!xiaotai_at_parse_u32("101", 0, 100, &value));
    CHECK(!xiaotai_at_parse_u32("99999999999999999999", 0, 100, &value));
    CHECK(xiaotai_at_parse_u32("36", 0, 36, &value) && !xiaotai_at_parse_u32("37", 0, 36, &value));
    CHECK(!xiaotai_at_parse_u32("12x", 0, 36, &value));
    CHECK(!xiaotai_at_parse_u32("0", 1, 60, &value) && !xiaotai_at_parse_u32("61", 1, 60, &value));
    CHECK(xiaotai_at_parse_u32("60", 1, 60, &value) && value == 60);
    CHECK(!xiaotai_at_parse_u32("9999999999999999", 0, 12, &value));
}

static void test_parse_i32(void)
{
    int32_t value = 77;
    CHECK(xiaotai_at_parse_i32("-1", -10, 10, &value) && value == -1);
    CHECK(xiaotai_at_parse_i32("-0", -10, 10, &value) && value == 0);
    CHECK(xiaotai_at_parse_i32("7", -10, 10, &value) && value == 7);
    CHECK(xiaotai_at_parse_i32("-2147483648", INT32_MIN, INT32_MAX, &value) && value == INT32_MIN);
    CHECK(xiaotai_at_parse_i32("2147483647", INT32_MIN, INT32_MAX, &value) && value == INT32_MAX);
    CHECK(xiaotai_at_parse_i32("-12", -12, -12, NULL));

    static const struct {
        const char *text;
        int32_t min, max;
    } bad[] = {
        {"", -10, 10}, {"-", -10, 10}, {"--1", -10, 10}, {"+1", -10, 10}, {" -1", -10, 10},
        {"- 1", -10, 10}, {"1-", -10, 10}, {"-11", -10, 10}, {"11", -10, 10},
        {"-2147483649", INT32_MIN, INT32_MAX}, {"2147483648", INT32_MIN, INT32_MAX},
        {"-00000000001", INT32_MIN, INT32_MAX}, {"0x1", -10, 10}, {"1", 2, -2},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        value = 77;
        CHECK_TEXT(!xiaotai_at_parse_i32(bad[i].text, bad[i].min, bad[i].max, &value) && value == 77, bad[i].text);
    }
    CHECK(!xiaotai_at_parse_i32(NULL, -10, 10, &value) && value == 77);
}

static void test_parse_u64(void)
{
    /* Recognition revisions: 0..2^53-1, the largest integer a JSON number
     * keeps exactly. Revision 0 is valid (a record the server never made). */
    const uint64_t revision_max = UINT64_C(9007199254740991);
    uint64_t value = 77;
    CHECK(xiaotai_at_parse_u64("9007199254740991", revision_max, &value) && value == revision_max);
    CHECK(xiaotai_at_parse_u64("0", revision_max, &value) && value == 0);
    static const char *const revisions[] = {
        "-1", "1.0", "+1", " 1", "", "9007199254740992", "18446744073709551616",
        "1000000000000000000000000000",
    };
    for (size_t i = 0; i < sizeof(revisions) / sizeof(revisions[0]); ++i) {
        value = 77;
        CHECK_TEXT(!xiaotai_at_parse_u64(revisions[i], revision_max, &value) && value == 77, revisions[i]);
    }
    CHECK(!xiaotai_at_parse_u64(NULL, revision_max, &value));

    /* The full range: twenty digits, no wrap past UINT64_MAX. */
    CHECK(xiaotai_at_parse_u64("18446744073709551615", UINT64_MAX, &value) && value == UINT64_MAX);
    CHECK(!xiaotai_at_parse_u64("18446744073709551616", UINT64_MAX, &value));
    CHECK(!xiaotai_at_parse_u64("99999999999999999999", UINT64_MAX, &value));
    CHECK(xiaotai_at_parse_u64("00000000000000000001", UINT64_MAX, &value) && value == 1);
    CHECK(!xiaotai_at_parse_u64("000000000000000000001", UINT64_MAX, &value));
    /* A limit below a single digit. */
    CHECK(xiaotai_at_parse_u64("0", 0, &value) && value == 0);
    CHECK(!xiaotai_at_parse_u64("5", 0, &value) && !xiaotai_at_parse_u64("5", 4, &value));
    CHECK(xiaotai_at_parse_u64("5", 5, NULL));
}

static void test_flags(void)
{
    bool value = true;
    CHECK(xiaotai_at_parse_flag("0", &value) && value == false);
    CHECK(xiaotai_at_parse_flag("1", &value) && value == true);
    CHECK(xiaotai_at_parse_flag("1", NULL));
    static const char *const bad[] = {"", "2", "01", "10", "-1", "+1", " 1", "1 ", "ON", "0.0", "=1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        value = true;
        CHECK_TEXT(!xiaotai_at_parse_flag(bad[i], &value) && value == true, bad[i]);
    }
    CHECK(!xiaotai_at_parse_flag(NULL, &value));
}

/* ------------------------------------------------- identifiers and digits */

static void test_identifiers(void)
{
    char id[XIAOTAI_AT_ID_MAX + 2];
    memset(id, 'A', XIAOTAI_AT_ID_MAX);
    id[XIAOTAI_AT_ID_MAX] = '\0';
    CHECK(xiaotai_at_is_id(id, XIAOTAI_AT_ID_MAX));
    id[XIAOTAI_AT_ID_MAX] = 'A';
    id[XIAOTAI_AT_ID_MAX + 1] = '\0';
    CHECK(!xiaotai_at_is_id(id, XIAOTAI_AT_ID_MAX));
    /* A larger max_length is held to the 64-character limit. */
    CHECK(!xiaotai_at_is_id(id, 1000));

    static const char *const good[] = {"TIR_test-01", "TIRX1234ABCD", "fixture-Device_1", "a", "-", "_", "0"};
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        CHECK_TEXT(xiaotai_at_is_id(good[i], XIAOTAI_AT_ID_MAX), good[i]);
    }
    static const char *const bad[] = {
        "", "one two", "id\"", "id/other", "bad\nnext", "bad/id", "a.b", "a:b", "a+b", "id\t", " id",
        "bad/item", "\xe5\xb0\x8f",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK_TEXT(!xiaotai_at_is_id(bad[i], XIAOTAI_AT_ID_MAX), bad[i]);
    }
    CHECK(!xiaotai_at_is_id(NULL, XIAOTAI_AT_ID_MAX));
    CHECK(xiaotai_at_is_id("abcdef", 6) && !xiaotai_at_is_id("abcdefg", 6));
    CHECK(!xiaotai_at_is_id("a", 0));
}

static void test_digits(void)
{
    CHECK(xiaotai_at_is_digits("012345", 6));
    CHECK(xiaotai_at_is_digits("0000", 4));
    static const struct {
        const char *text;
        size_t count;
    } bad[] = {
        {"12345", 6}, {"01234a", 6}, {"1234567", 6}, {"123", 4}, {"12345", 4}, {"12x4", 4},
        {"-123", 4}, {"+123", 4}, {" 123", 4}, {"123 ", 4}, {"", 4},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK_TEXT(!xiaotai_at_is_digits(bad[i].text, bad[i].count), bad[i].text);
    }
    CHECK(!xiaotai_at_is_digits(NULL, 6));
    /* Zero digits: only the empty text. */
    CHECK(xiaotai_at_is_digits("", 0) && !xiaotai_at_is_digits("1", 0));
}

static void test_keywords(void)
{
    static const char *const filters[] = {"ALL", "DEV", "WX", NULL};
    CHECK(xiaotai_at_keyword("ALL", filters) == 0);
    CHECK(xiaotai_at_keyword("DEV", filters) == 1);
    CHECK(xiaotai_at_keyword("WX", filters) == 2);
    static const char *const bad[] = {"wx", "DEVICE", "DE", "", " ALL", "ALL "};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK_TEXT(xiaotai_at_keyword(bad[i], filters) == -1, bad[i]);
    }
    CHECK(xiaotai_at_keyword(NULL, filters) == -1);
    CHECK(xiaotai_at_keyword("ALL", NULL) == -1);
    static const char *const none[] = {NULL};
    CHECK(xiaotai_at_keyword("ALL", none) == -1);
    /* The first of two equal entries wins. */
    static const char *const twice[] = {"ON", "OFF", "ON", NULL};
    CHECK(xiaotai_at_keyword("ON", twice) == 0 && xiaotai_at_keyword("OFF", twice) == 1);
}

/* ------------------------------------------------------------ hexadecimal */

static void test_hex_decode(void)
{
    uint8_t out[8];
    size_t size = 99;
    memset(out, 0xEE, sizeof(out));
    CHECK(xiaotai_at_hex_decode("00ff10", out, sizeof(out), &size) == XIAOTAI_OK && size == 3);
    CHECK(out[0] == 0x00 && out[1] == 0xFF && out[2] == 0x10 && out[3] == 0xEE); /* zero bytes are data */
    CHECK(xiaotai_at_hex_decode("aBcD", out, sizeof(out), &size) == XIAOTAI_OK && size == 2);
    CHECK(out[0] == 0xAB && out[1] == 0xCD);
    /* Exact capacity, then one byte short: nothing usable is written. */
    memset(out, 0xEE, sizeof(out));
    CHECK(xiaotai_at_hex_decode("0102030405060708", out, 8, &size) == XIAOTAI_OK && size == 8 && out[7] == 0x08);
    memset(out, 0xEE, sizeof(out));
    CHECK(xiaotai_at_hex_decode("0102030405060708", out, 7, &size) == XIAOTAI_ERR_INVALID_SIZE && size == 0);
    CHECK(out[0] == 0xEE);
    CHECK(xiaotai_at_hex_decode("01", NULL, 0, &size) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_at_hex_decode("01", out, sizeof(out), NULL) == XIAOTAI_OK && out[0] == 0x01);

    static const char *const bad[] = {"", "6", "6g", "g6", "abc", "0x10", " 10", "10 ", "-", "--", "1-"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(out, 0xEE, sizeof(out));
        size = 99;
        CHECK_TEXT(xiaotai_at_hex_decode(bad[i], out, sizeof(out), &size) == XIAOTAI_ERR_INVALID_ARG, bad[i]);
        CHECK_TEXT(size == 0 && out[0] == 0xEE, bad[i]);
    }
    CHECK(xiaotai_at_hex_decode(NULL, out, sizeof(out), &size) == XIAOTAI_ERR_INVALID_ARG);
    /* A malformed text is INVALID_ARG even when it would not fit either. */
    CHECK(xiaotai_at_hex_decode("zz", out, 0, &size) == XIAOTAI_ERR_INVALID_ARG);
}

static void test_text_decode(void)
{
    char out[300];

    /* Remarks: lower and upper case, "-" for empty, a space inside. */
    CHECK(xiaotai_at_text_decode("e5b08fe9929b", out, sizeof(out)) == XIAOTAI_OK);
    CHECK(!strcmp(out, "\xe5\xb0\x8f\xe9\x92\x9b"));
    CHECK(xiaotai_at_text_decode("E5B08FE9929B", out, sizeof(out)) == XIAOTAI_OK);
    CHECK(!strcmp(out, "\xe5\xb0\x8f\xe9\x92\x9b"));
    strcpy(out, "stale");
    CHECK(xiaotai_at_text_decode("-", out, sizeof(out)) == XIAOTAI_OK && out[0] == '\0');
    CHECK(xiaotai_at_text_decode("412042", out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, "A B"));

    /* 256 bytes, the longest remark, fit 257 with the terminator; 257 do not. */
    char hex[2 * 257 + 1];
    for (size_t i = 0; i < 256; ++i) {
        memcpy(hex + 2 * i, "61", 2);
    }
    hex[512] = '\0';
    CHECK(xiaotai_at_text_decode(hex, out, 257) == XIAOTAI_OK && strlen(out) == 256 && out[255] == 'a');
    CHECK(xiaotai_at_text_decode(hex, out, 256) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');
    memcpy(hex + 512, "61", 3);
    CHECK(xiaotai_at_text_decode(hex, out, 257) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');

    /* Malformed: nothing is decoded, out is the empty text (no part of a
     * secret is left behind). */
    static const char *const bad[] = {"", "6", "6g", "00", "410042", "4100", "0041", "--", "- ", " -", "-61"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(out, 'x', 8);
        CHECK_TEXT(xiaotai_at_text_decode(bad[i], out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG, bad[i]);
        CHECK_TEXT(out[0] == '\0' && out[1] == 'x', bad[i]);
    }
    CHECK(xiaotai_at_text_decode(NULL, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG && out[0] == '\0');

    /* Bytes are not checked to be UTF-8: that is the value owner's call. */
    static const struct {
        const char *hex;
        const char *text;
    } raw[] = {
        {"ff", "\xff"}, {"c0af", "\xc0\xaf"}, {"eda080", "\xed\xa0\x80"},
        {"f4908080", "\xf4\x90\x80\x80"}, {"e5b0", "\xe5\xb0"},
    };
    for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); ++i) {
        CHECK_TEXT(xiaotai_at_text_decode(raw[i].hex, out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, raw[i].text),
                   raw[i].hex);
    }
    for (size_t i = 0; i < 64; ++i) {
        memcpy(hex + 8 * i, "f09f9880", 8);
    }
    hex[512] = '\0';
    CHECK(xiaotai_at_text_decode(hex, out, 257) == XIAOTAI_OK && strlen(out) == 256);
    for (size_t i = 0; i < 64; ++i) {
        CHECK(!memcmp(out + 4 * i, "\xf0\x9f\x98\x80", 4));
    }

    /* Wi-Fi texts: SSID up to 32 bytes, password up to 64. */
    char ssid[33], password[65];
    CHECK(xiaotai_at_text_decode("5469", ssid, sizeof(ssid)) == XIAOTAI_OK && !strcmp(ssid, "Ti"));
    CHECK(xiaotai_at_text_decode("3132333435363738", password, sizeof(password)) == XIAOTAI_OK &&
          !strcmp(password, "12345678"));
    memset(hex, 'a', 64);
    hex[64] = '\0';
    CHECK(xiaotai_at_text_decode(hex, ssid, sizeof(ssid)) == XIAOTAI_OK && strlen(ssid) == 32);
    memset(hex, 'b', 128);
    hex[128] = '\0';
    CHECK(xiaotai_at_text_decode(hex, password, sizeof(password)) == XIAOTAI_OK && strlen(password) == 64);
    memcpy(hex + 128, "bb", 3);
    CHECK(xiaotai_at_text_decode(hex, password, sizeof(password)) == XIAOTAI_ERR_INVALID_SIZE && !password[0]);

    /* No room at all, even for the empty text's terminator. */
    CHECK(xiaotai_at_text_decode("-", out, 0) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_at_text_decode("-", NULL, 8) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_at_text_decode("41", NULL, 8) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_at_text_decode("41", out, 1) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');
    CHECK(xiaotai_at_text_decode("-", out, 1) == XIAOTAI_OK && out[0] == '\0');
    CHECK(xiaotai_at_text_decode("41", out, 2) == XIAOTAI_OK && !strcmp(out, "A"));
}

static void test_encode(void)
{
    char out[32];
    static const uint8_t bytes[] = {0x00, 0xAB, 0x7F};
    CHECK(xiaotai_at_hex_encode(bytes, 3, out, sizeof(out)) == 6 && !strcmp(out, "00AB7F"));
    CHECK(xiaotai_at_hex_encode(bytes, 3, out, 7) == 6 && !strcmp(out, "00AB7F"));
    strcpy(out, "stale");
    CHECK(xiaotai_at_hex_encode(bytes, 3, out, 6) == 0 && out[0] == '\0');
    strcpy(out, "stale");
    CHECK(xiaotai_at_hex_encode(bytes, 3, out, 0) == 0 && out[0] == 's'); /* nothing to write into */
    CHECK(xiaotai_at_hex_encode(bytes, 3, NULL, 7) == 0);
    strcpy(out, "stale");
    CHECK(xiaotai_at_hex_encode(bytes, 0, out, sizeof(out)) == 0 && out[0] == '\0');
    CHECK(xiaotai_at_hex_encode(NULL, 0, out, 1) == 0 && out[0] == '\0');
    CHECK(xiaotai_at_hex_encode(NULL, 1, out, sizeof(out)) == 0 && out[0] == '\0');
    /* A size whose text length would wrap size_t is refused, not wrapped. */
    CHECK(xiaotai_at_hex_encode(bytes, SIZE_MAX, out, sizeof(out)) == 0 && out[0] == '\0');
    CHECK(xiaotai_at_hex_encode(bytes, SIZE_MAX / 2 + 1, out, sizeof(out)) == 0);

    CHECK(xiaotai_at_text_encode("A B", out, sizeof(out)) == 6 && !strcmp(out, "412042"));
    CHECK(xiaotai_at_text_encode("\xe5\xb0\x8f\xe9\x92\x9b", out, sizeof(out)) == 12 &&
          !strcmp(out, "E5B08FE9929B"));
    CHECK(xiaotai_at_text_encode("", out, sizeof(out)) == 1 && !strcmp(out, "-"));
    CHECK(xiaotai_at_text_encode(NULL, out, sizeof(out)) == 1 && !strcmp(out, "-"));
    CHECK(xiaotai_at_text_encode("", out, 2) == 1 && !strcmp(out, "-"));
    strcpy(out, "stale");
    CHECK(xiaotai_at_text_encode("", out, 1) == 0 && out[0] == '\0');
    strcpy(out, "stale");
    CHECK(xiaotai_at_text_encode("A B", out, 6) == 0 && out[0] == '\0');
    CHECK(xiaotai_at_text_encode("A B", out, 7) == 6);
    CHECK(xiaotai_at_text_encode("A", NULL, 8) == 0);

    /* Round trips: every byte value but zero survives text encode/decode,
     * and every byte value hex encode/decode. */
    char text[256], hex[2 * 256 + 1], back[256];
    for (size_t i = 0; i < 255; ++i) {
        text[i] = (char)(i + 1);
    }
    text[255] = '\0';
    CHECK(xiaotai_at_text_encode(text, hex, sizeof(hex)) == 510);
    CHECK(xiaotai_at_text_decode(hex, back, sizeof(back)) == XIAOTAI_OK && !memcmp(back, text, 256));
    uint8_t all[256], decoded[256];
    for (size_t i = 0; i < 256; ++i) {
        all[i] = (uint8_t)i;
    }
    size_t size = 0;
    CHECK(xiaotai_at_hex_encode(all, sizeof(all), hex, sizeof(hex)) == 512);
    CHECK(xiaotai_at_hex_decode(hex, decoded, sizeof(decoded), &size) == XIAOTAI_OK && size == 256 &&
          !memcmp(decoded, all, 256));
}

int main(void)
{
    test_error_names();
    test_printf();
    test_parse_u32();
    test_parse_i32();
    test_parse_u64();
    test_flags();
    test_identifiers();
    test_digits();
    test_keywords();
    test_hex_decode();
    test_text_decode();
    test_encode();
    return at_test_result("at_text");
}
