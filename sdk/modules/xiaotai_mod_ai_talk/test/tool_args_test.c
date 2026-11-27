// SPDX-License-Identifier: MIT
// The lenient reads of a tool call's arguments (xiaotai_ai_talk_tool_arg_*):
// what a language model writes for an integer, a boolean, an enumeration word
// and a label, and what is refused.
#include "xiaotai/modules/ai_talk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

static xiaotai_ai_talk_tool_call_t call_of(const char *json)
{
    return (xiaotai_ai_talk_tool_call_t){.session = 1, .request = 1, .name = "tool", .arguments = json,
        .size = strlen(json), .device_action = true, .has_arguments = true};
}

static xiaotai_err_t read_int(const char *json, int32_t *value)
{
    const xiaotai_ai_talk_tool_call_t call = call_of(json);
    return xiaotai_ai_talk_tool_arg_int(&call, "v", value);
}

static void integers(void)
{
    int32_t v = -7;
    CHECK(read_int("{\"v\":60}", &v) == XIAOTAI_OK && v == 60);
    CHECK(read_int("{\"v\":-5}", &v) == XIAOTAI_OK && v == -5);
    CHECK(read_int("{\"v\":7.0}", &v) == XIAOTAI_OK && v == 7);
    CHECK(read_int("{\"v\":\"60\"}", &v) == XIAOTAI_OK && v == 60);
    CHECK(read_int("{\"v\":\" 45% \"}", &v) == XIAOTAI_OK && v == 45);
    CHECK(read_int("{\"v\":\"+8\"}", &v) == XIAOTAI_OK && v == 8);
    CHECK(read_int("{\"v\":2147483647}", &v) == XIAOTAI_OK && v == INT32_MAX);
    CHECK(read_int("{\"v\":-2147483648}", &v) == XIAOTAI_OK && v == INT32_MIN);
    v = 99;
    CHECK(read_int("{\"other\":1}", &v) == XIAOTAI_ERR_NOT_FOUND && v == 99);
    CHECK(read_int("{}", &v) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(read_int("{\"v\":null}", &v) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(read_int("{\"v\":\"\"}", &v) == XIAOTAI_ERR_NOT_FOUND && v == 99);
    CHECK(read_int("{\"v\":7.5}", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_int("{\"v\":\"seven\"}", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_int("{\"v\":\"6 0\"}", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_int("{\"v\":true}", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_int("{\"v\":2147483648}", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_int("{\"v\":1e3}", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_int("{\"v\":[1]}", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_int("{\"v\":1", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_int("{\"a\":{\"v\":1}}", &v) == XIAOTAI_ERR_NOT_FOUND && v == 99);
    /* Eleven members fit; a twelfth is more than any tool takes. */
    CHECK(read_int("{\"a\":1,\"b\":1,\"c\":1,\"d\":1,\"e\":1,\"f\":1,\"g\":1,\"h\":1,\"i\":1,\"j\":1,\"v\":3}", &v) ==
          XIAOTAI_OK && v == 3);
    CHECK(read_int("{\"a\":1,\"b\":1,\"c\":1,\"d\":1,\"e\":1,\"f\":1,\"g\":1,\"h\":1,\"i\":1,\"j\":1,\"k\":1,\"v\":3}",
                   &v) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_ai_talk_tool_call_t empty = call_of("{}");
    CHECK(xiaotai_ai_talk_tool_arg_int(NULL, "v", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_ai_talk_tool_arg_int(&empty, NULL, &v) == XIAOTAI_ERR_INVALID_ARG);
}

static xiaotai_err_t read_bool(const char *json, bool *value)
{
    const xiaotai_ai_talk_tool_call_t call = call_of(json);
    return xiaotai_ai_talk_tool_arg_bool(&call, "v", value);
}

static void booleans(void)
{
    bool v = false;
    CHECK(read_bool("{\"v\":true}", &v) == XIAOTAI_OK && v);
    CHECK(read_bool("{\"v\":false}", &v) == XIAOTAI_OK && !v);
    CHECK(read_bool("{\"v\":\"True\"}", &v) == XIAOTAI_OK && v);
    CHECK(read_bool("{\"v\":0}", &v) == XIAOTAI_OK && !v);
    CHECK(read_bool("{\"v\":1}", &v) == XIAOTAI_OK && v);
    CHECK(read_bool("{}", &v) == XIAOTAI_ERR_NOT_FOUND && v);
    CHECK(read_bool("{\"v\":\"yes\"}", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_bool("{\"v\":2}", &v) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(read_bool("{\"v\":{}}", &v) == XIAOTAI_ERR_INVALID_ARG);
}

static void words(void)
{
    char out[8] = "x";
    xiaotai_ai_talk_tool_call_t call = call_of("{\"mode\":\" Up \"}");
    CHECK(xiaotai_ai_talk_tool_arg_word(&call, "mode", out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, "up"));
    call = call_of("{\"mode\":7}");
    CHECK(xiaotai_ai_talk_tool_arg_word(&call, "mode", out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, "7"));
    call = call_of("{\"mode\":\"weekdays\"}");
    CHECK(xiaotai_ai_talk_tool_arg_word(&call, "mode", out, sizeof(out)) == XIAOTAI_ERR_INVALID_SIZE && !out[0]);
    call = call_of("{\"mode\":\"  \"}");
    strcpy(out, "x");
    CHECK(xiaotai_ai_talk_tool_arg_word(&call, "mode", out, sizeof(out)) == XIAOTAI_ERR_NOT_FOUND && !out[0]);
    call = call_of("{\"mode\":[\"up\"]}");
    CHECK(xiaotai_ai_talk_tool_arg_word(&call, "mode", out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
    call = call_of("{}");
    strcpy(out, "x");
    CHECK(xiaotai_ai_talk_tool_arg_word(&call, "mode", out, sizeof(out)) == XIAOTAI_ERR_NOT_FOUND && !out[0]);
    CHECK(xiaotai_ai_talk_tool_arg_word(&call, "mode", out, 0) == XIAOTAI_ERR_INVALID_ARG);
}

static void texts(void)
{
    char out[8];
    /* Three bytes per character: two fit in seven bytes, the third is cut whole. */
    xiaotai_ai_talk_tool_call_t call = call_of("{\"label\":\" \xe5\x85\xb3\xe7\x81\xab\xe4\xba\x86 \"}");
    CHECK(xiaotai_ai_talk_tool_arg_text(&call, "label", out, sizeof(out)) == XIAOTAI_OK);
    CHECK(!strcmp(out, "\xe5\x85\xb3\xe7\x81\xab"));
    call = call_of("{\"label\":\"Tea\"}");
    CHECK(xiaotai_ai_talk_tool_arg_text(&call, "label", out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, "Tea"));
    call = call_of("{\"label\":\"a\\\"b\"}");
    CHECK(xiaotai_ai_talk_tool_arg_text(&call, "label", out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, "a\"b"));
    call = call_of("{\"label\":\"\"}");
    CHECK(xiaotai_ai_talk_tool_arg_text(&call, "label", out, sizeof(out)) == XIAOTAI_ERR_NOT_FOUND && !out[0]);
    call = call_of("{\"label\":5}");
    CHECK(xiaotai_ai_talk_tool_arg_text(&call, "label", out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
    char json[400] = "{\"label\":\"";
    memset(json + strlen(json), 'a', 300);
    strcat(json, "\"}");
    call = call_of(json);
    CHECK(xiaotai_ai_talk_tool_arg_text(&call, "label", out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
}

int main(void)
{
    integers();
    booleans();
    words();
    texts();
    puts("PASS: tool arguments are read as a model writes them and refused otherwise");
    return 0;
}
