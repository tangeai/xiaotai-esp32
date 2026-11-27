// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recognition_config.py
#include "recognition_protocol.h"
#include "xiaotai_mem_host.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCHEME "{\"scheme_id\":\"detect\",\"scheme_version\":1}"
#define LIMITS "{\"max_file_bytes\":320044,\"max_audio_seconds\":10}"
#define USAGE "{\"used\":3,\"input_tokens\":8,\"output_tokens\":9,\"tts_characters\":7,\"unknown_calls\":1}"
#define VALID "{\"code\":200,\"data\":{\"policy\":{\"enabled\":true,\"allowed\":true,\"revision\":9007199254740991," \
              "\"monthly_limit\":100,\"schemes\":[" SCHEME "]},\"effective_enabled\":true," \
              "\"upload_limits\":" LIMITS ",\"usage\":" USAGE "}}"

static void replace(const char *field, const char *with)
{
    char text[RC_POLICY_BYTES + 1];
    const char *at = strstr(VALID, field);
    assert(at);
    int n = snprintf(text, sizeof(text), "%.*s%s%s", (int)(at - VALID), VALID, with, at + strlen(field));
    assert(n > 0 && (size_t)n < sizeof(text));
    xiaotai_recognition_policy_t value = {.revision = 123};
    int code;
    assert(rc_policy_decode(text, (size_t)n, 200, &code, &value) == RC_INVALID_RESPONSE);
    assert(value.revision == 123);
}

int main(void)
{
    xiaotai_recognition_policy_t value = {0};
    int code;
    assert(rc_policy_decode(VALID, strlen(VALID), 200, &code, &value) == XIAOTAI_OK);
    assert(value.valid && value.enabled && value.allowed && value.effective_enabled && code == 200);
    assert(value.revision == XIAOTAI_RECOGNITION_REVISION_MAX && value.scheme_count == 1 &&
           value.monthly_limit == 100 && value.max_file_bytes == 320044 && value.max_audio_seconds == 10);
    assert(value.used == 3 && value.input_tokens == 8 && value.output_tokens == 9 &&
           value.tts_characters == 7 && value.unknown_calls == 1);
    const int other_statuses[] = {201, 202, 204, 302, 400, 401, 403, 404, 409, 410, 429, 503};
    for (size_t i = 0; i < sizeof(other_statuses) / sizeof(other_statuses[0]); ++i) {
        value.revision = 123;
        assert(rc_policy_decode(VALID, strlen(VALID), other_statuses[i], &code, &value) == RC_INVALID_RESPONSE);
        assert(code == 200 && value.revision == 123);
    }
    replace("\"enabled\":true", "\"enabled\":1");
    replace("\"allowed\":true", "\"allowed\":false");
    replace("\"effective_enabled\":true", "\"effective_enabled\":-1");
    replace("\"revision\":9007199254740991", "\"revision\":-1");
    replace("\"revision\":9007199254740991", "\"revision\":9007199254740992");
    replace("\"revision\":9007199254740991", "\"revision\":0.5");
    replace("\"revision\":9007199254740991", "\"revision\":01");
    replace("\"revision\":9007199254740991", "\"revision\":1e30");
    replace("\"monthly_limit\":100", "\"monthly_limit\":-1");
    replace("\"monthly_limit\":100", "\"monthly_limit\":1000001");
    replace("\"schemes\":[" SCHEME "]", "\"schemes\":[]");
    replace("\"scheme_version\":1", "\"scheme_version\":0");
    replace("\"scheme_id\":\"detect\"", "\"scheme_id\":\"detect\\u0000other\"");
    replace(SCHEME, SCHEME ",{\"scheme_id\":\"de\\u0074ect\",\"scheme_version\":2}");
    replace("\"max_file_bytes\":320044", "\"max_file_bytes\":5242881");
    replace("\"max_audio_seconds\":10", "\"max_audio_seconds\":61");
    replace("\"usage\":" USAGE, "\"usage\":null");
    replace("\"used\":3", "\"used\":true");
    replace("\"unknown_calls\":1", "\"unknown_calls\":-1");
    replace("\"code\":200", "\"code\":200.5");
    const char disabled[] = "{\"code\":200,\"data\":{\"policy\":{\"enabled\":false,\"allowed\":false,"
        "\"revision\":0,\"monthly_limit\":0,\"schemes\":[]},\"effective_enabled\":false,"
        "\"upload_limits\":null,\"usage\":" USAGE "}}";
    assert(rc_policy_decode(disabled, strlen(disabled), 200, &code, &value) == XIAOTAI_OK);
    assert(value.valid && !value.effective_enabled && !value.revision && !value.max_audio_seconds);
    const char denied[] = "{\"code\":200,\"data\":{\"policy\":{\"enabled\":true,\"allowed\":false,"
        "\"revision\":4,\"monthly_limit\":100,\"schemes\":[" SCHEME "]},\"effective_enabled\":false,"
        "\"upload_limits\":" LIMITS ",\"usage\":" USAGE "}}";
    assert(rc_policy_decode(denied, strlen(denied), 200, &code, &value) == XIAOTAI_OK);
    assert(value.enabled && !value.allowed && !value.effective_enabled);
    const char xmini[] = "{\"code\":200,\"data\":{\"policy\":{\"enabled\":false,\"allowed\":true,"
        "\"revision\":0,\"monthly_limit\":100,\"schemes\":[]},\"effective_enabled\":false,"
        "\"upload_limits\":{\"max_file_bytes\":5242880,\"max_audio_seconds\":60},\"usage\":" USAGE "}}";
    assert(rc_policy_decode(xmini, strlen(xmini), 200, &code, &value) == XIAOTAI_OK);
    assert(!value.enabled && value.allowed && !value.effective_enabled && value.monthly_limit == 100);
    assert(value.max_file_bytes == 5242880 && value.max_audio_seconds == 60);
    const char unbound[] = "{\"code\":6006}";
    assert(rc_policy_decode(unbound, strlen(unbound), 410, &code, &value) == RC_INVALID_RESPONSE && code == 6006);
    assert(rc_policy_decode(NULL, 0, 200, &code, &value) == XIAOTAI_FAIL && code == -1);
    const char trailing[] = "{} garbage";
    assert(rc_policy_decode(trailing, strlen(trailing), 200, &code, &value) == RC_INVALID_RESPONSE && code == -1);
    assert(rc_policy_decode(VALID, sizeof(VALID), 200, &code, &value) == RC_INVALID_RESPONSE);
    assert(rc_policy_decode(VALID, RC_POLICY_BYTES + 1, 200, &code, &value) == XIAOTAI_ERR_INVALID_SIZE);
    xiaotai_mem_host_fail_nth(1);
    assert(rc_policy_decode(VALID, strlen(VALID), 200, &code, &value) == XIAOTAI_ERR_NO_MEM);
    assert(!xiaotai_mem_host_outstanding());
}
