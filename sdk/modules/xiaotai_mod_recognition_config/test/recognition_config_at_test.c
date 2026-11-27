// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_command.c::command
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/at.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/recognition_config.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

static const char good[] =
    "{\"code\":200,\"requestId\":\"not-visible\",\"data\":{\"effective_enabled\":true,"
    "\"policy\":{\"enabled\":true,\"allowed\":true,\"revision\":9,\"monthly_limit\":33,"
    "\"schemes\":[{\"scheme_id\":\"private-scheme\",\"scheme_version\":1}]},"
    "\"upload_limits\":{\"max_file_bytes\":512345,\"max_audio_seconds\":60},"
    "\"usage\":{\"used\":7,\"input_tokens\":87,\"output_tokens\":6,\"tts_characters\":23,\"unknown_calls\":5}}}";
static const char expect_ok[] =
    "OK\r\n+RECOGNITION: http=200 code=200 enabled=1 allowed=1 effective=1 revision=9 schemes=1"
    " max-bytes=512345 max-seconds=60 monthly-limit=33 used=7 input-tokens=87 output-tokens=6"
    " tts-characters=23 unknown-calls=5\r\n";
static const char *input;
static char output[1024];
static size_t used;
static unsigned requests;
static xiaotai_err_t admission;
static xiaotai_platform_reply_fn_t pending;
static void *pending_context;
static xiaotai_signal_t delivered;

static size_t read_input(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    if (!input) return 0;
    const size_t count = strlen(input);
    assert(count <= capacity);
    memcpy(data, input, count);
    input = NULL;
    return count;
}

static xiaotai_err_t write_output(void *ctx, const char *data, size_t size)
{
    (void)ctx;
    assert(size + used < sizeof(output));
    memcpy(output + used, data, size);
    used += size;
    output[used] = '\0';
    return XIAOTAI_OK;
}

void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    (void)kind;
    *stats = (xiaotai_mem_stats_t){.total = 96U * 1024U, .free = 12U * 1024U, .largest_block = 5U * 1024U};
}

xiaotai_err_t __wrap_xiaotai_platform_request(const xiaotai_platform_request_t *request,
        xiaotai_platform_reply_fn_t reply, void *ctx)
{
    assert(xiaotai_on_owner());
    assert(request->method == XIAOTAI_PLATFORM_GET && request->auth == XIAOTAI_PLATFORM_AUTH_SESSION);
    assert(!strcmp(request->service, "recognition") && !strcmp(request->path, "/v1/recognition/config"));
    assert(request->body == NULL && request->bearer == NULL && request->flags == 0);
    assert(reply && !pending);
    ++requests;
    if (admission == XIAOTAI_OK) {
        pending = reply;
        pending_context = ctx;
    }
    return admission;
}

static void deliver(void *data)
{
    assert(xiaotai_on_owner() && pending);
    const xiaotai_platform_reply_fn_t reply = pending;
    pending = NULL;
    reply(data, pending_context);
    xiaotai_signal_give(&delivered);
}

static void command(const char *line, const char *expected)
{
    input = line;
    used = 0;
    output[0] = '\0';
    assert(xiaotai_at_service(1) == XIAOTAI_OK && !input);
    if (strcmp(output, expected)) fprintf(stderr, "For [%s], got [%s], wanted [%s]\n", line, output, expected);
    assert(!strcmp(output, expected));
}

static void response(xiaotai_err_t result, int status, int32_t code, const char *body, const char *expected)
{
    xiaotai_platform_reply_t reply = {
        .result = result, .http_status = status, .code = code,
        .body = body, .size = body ? strlen(body) : 0U,
    };
    assert(xiaotai_run(deliver, &reply) == XIAOTAI_OK && xiaotai_signal_take(&delivered, 2000));
    assert(xiaotai_at_service(1) == XIAOTAI_OK);
    assert(!strcmp(output, expected));
}

static void max_line(void)
{
    const xiaotai_recognition_config_policy_t max = {
        .enabled = true, .allowed = true, .effective_enabled = true,
        .revision = UINT64_C(9007199254740991), .scheme_count = UINT8_MAX,
        .max_file_bytes = UINT32_MAX, .max_audio_seconds = UINT32_MAX, .monthly_limit = UINT32_MAX,
        .used = UINT64_MAX, .input_tokens = UINT64_MAX, .output_tokens = UINT64_MAX,
        .tts_characters = UINT64_MAX, .unknown_calls = UINT64_MAX,
    };
    static const char golden[] =
        "+RECOGNITION: http=-2147483648 code=-2147483648 enabled=1 allowed=1 effective=1 revision=9007199254740991"
        " schemes=255 max-bytes=4294967295 max-seconds=4294967295 monthly-limit=4294967295 used=18446744073709551615"
        " input-tokens=18446744073709551615 output-tokens=18446744073709551615"
        " tts-characters=18446744073709551615 unknown-calls=18446744073709551615";
    char line[sizeof(golden)];
    assert(sizeof(golden) - 1U == 352U && XIAOTAI_RECOGNITION_CONFIG_LINE_MAX == 352U);
    assert(xiaotai_recognition_config_format(line, sizeof(line), XIAOTAI_OK, INT32_MIN, INT32_MIN, &max) == 352);
    assert(!strcmp(line, golden));
    assert(xiaotai_recognition_config_format(line, sizeof(line) - 1U, XIAOTAI_OK, INT32_MIN, INT32_MIN, &max) < 0);
}

static void thin_policy(void)
{
    int code;
    xiaotai_recognition_config_policy_t policy = {.revision = 123};
    /* Only the full recorder keeps a 256 KiB external-RAM reserve. */
    assert(xiaotai_recognition_config_decode(good, sizeof(good) - 1U, 200, &code, 2048U,
           256U * 1024U, false, &policy) == XIAOTAI_ERR_NO_MEM);
    assert(policy.revision == 123 && code == XIAOTAI_RECOGNITION_CONFIG_CODE_MISSING);
    assert(xiaotai_recognition_config_decode(good, sizeof(good) - 1U, 200, &code,
           XIAOTAI_RECOGNITION_CONFIG_MAX_BYTES, 0, false, &policy) == XIAOTAI_OK);
    assert(policy.revision == 9 && policy.effective_enabled && code == 200);
}

int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "xmini")));
    const bool xmini = argc == 2;
    if (xmini) assert(xiaotai_recognition_config_configure(
        &(xiaotai_recognition_config_options_t){.legacy_http_business_error_as_fail = true}) == XIAOTAI_OK);
    max_line();
    thin_policy();
    static const xiaotai_module_t module = {
        .name = "recognition_config", .init = xiaotai_module_recognition_config_init, .start = xiaotai_engine_at_start,
    };
    const xiaotai_module_t *modules[] = {&module};
    static const xiaotai_at_io_t io = {.name = "fixture", .read = read_input, .write = write_output};
    assert(xiaotai_signal_init(&delivered) == XIAOTAI_OK);
    assert(xiaotai_at_configure(&(xiaotai_at_config_t){.task_stack_bytes = XIAOTAI_AT_NO_TASK,
                                                     .no_platform_console = true}) == XIAOTAI_OK);
    assert(xiaotai_at_attach(&io) == XIAOTAI_OK);
    assert(xiaotai_at_open() == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 1}) == XIAOTAI_OK);
    assert(xiaotai_at_service(5) == XIAOTAI_OK && strstr(output, "+READY:"));

    command("AT+RECOGNITION\r", "ERROR: INVALID_ARG MISSING_VERB\r\n");
    command("AT+RECOGNITION STATE\r", "ERROR: INVALID_ARG UNKNOWN_VERB\r\n");
    command("AT+RECOGNITION ENABLE\r", "ERROR: INVALID_ARG UNKNOWN_VERB\r\n");
    command("AT+RECOGNITION RECORD 1\r", "ERROR: INVALID_ARG UNKNOWN_VERB\r\n");
    command("AT+RECOGNITION QUERY\r", "ERROR: INVALID_ARG UNKNOWN_VERB\r\n");
    command("AT+RECOGNITION CONFIG extra\r", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
    assert(requests == 0);
    admission = XIAOTAI_ERR_PLATFORM_OFFLINE;
    command("AT+RECOGNITION CONFIG\r", "ERROR: INVALID_STATE\r\n");
    admission = XIAOTAI_ERR_NOT_SUPPORTED;
    command("AT+RECOGNITION CONFIG\r", "ERROR: NOT_SUPPORTED\r\n");
    admission = XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
    command("AT+RECOGNITION CONFIG\r", "ERROR: 0x108\r\n");
    admission = XIAOTAI_ERR_NO_MEM;
    command("AT+RECOGNITION CONFIG\r", "ERROR: NO_MEM\r\n");
    admission = XIAOTAI_OK;
    char at_pool_limit[XIAOTAI_RECOGNITION_CONFIG_MAX_BYTES + 1U];
    memset(at_pool_limit, ' ', sizeof(at_pool_limit) - sizeof(good));
    memcpy(at_pool_limit + sizeof(at_pool_limit) - sizeof(good), good, sizeof(good));
    command("AT+RECOGNITION CONFIG\r", "OK\r\n");
    response(XIAOTAI_OK, 200, 200, at_pool_limit, expect_ok);
    command("AT+RECOGNITION CONFIG\r", "OK\r\n");
    response(XIAOTAI_OK, 403, 40303, "{\"code\":40303,\"msg\":\"private-response\"}",
        "OK\r\n+RECOGNITION: error=ESP_ERR_INVALID_RESPONSE http=403 code=40303\r\n");
    command("AT+RECOGNITION CONFIG\r", "OK\r\n");
    response(XIAOTAI_OK, 200, 40303, "{\"code\":40303,\"msg\":\"private-response\"}", xmini
        ? "OK\r\n+RECOGNITION: error=ESP_FAIL http=200 code=40303\r\n"
        : "OK\r\n+RECOGNITION: error=ESP_ERR_INVALID_RESPONSE http=200 code=40303\r\n");
    command("AT+RECOGNITION CONFIG\r", "OK\r\n");
    response(XIAOTAI_OK, 200, XIAOTAI_PLATFORM_NO_CODE, "{}", xmini
        ? "OK\r\n+RECOGNITION: error=ESP_FAIL http=200 code=-1\r\n"
        : "OK\r\n+RECOGNITION: error=ESP_ERR_INVALID_RESPONSE http=200 code=-1\r\n");
    command("AT+RECOGNITION CONFIG\r", "OK\r\n");
    response(XIAOTAI_OK, 200, 0, "{\"code\":0}",
        "OK\r\n+RECOGNITION: error=ESP_ERR_INVALID_RESPONSE http=200 code=0\r\n");
    command("AT+RECOGNITION CONFIG\r", "OK\r\n");
    response(XIAOTAI_OK, 200, XIAOTAI_PLATFORM_NO_CODE, "{",
        "OK\r\n+RECOGNITION: error=ESP_ERR_INVALID_RESPONSE http=200 code=-1\r\n");
    command("AT+RECOGNITION CONFIG\r", "OK\r\n");
    response(XIAOTAI_ERR_PLATFORM_STALE, 0, XIAOTAI_PLATFORM_NO_CODE, NULL,
        "OK\r\n+RECOGNITION: error=ESP_ERR_INVALID_STATE http=0 code=-1\r\n");
    command("AT+RECOGNITION CONFIG\r", "OK\r\n");
    response(XIAOTAI_ERR_TIMEOUT, 0, XIAOTAI_PLATFORM_NO_CODE, NULL,
        "OK\r\n+RECOGNITION: error=ESP_ERR_TIMEOUT http=0 code=-1\r\n");
    assert(requests == 12 && !pending);
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_signal_deinit(&delivered);
    return 0;
}
