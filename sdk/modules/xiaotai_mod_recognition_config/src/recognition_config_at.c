// SPDX-License-Identifier: MIT
#include "xiaotai/modules/recognition_config.h"
#include "xiaotai/at_command.h"
#include "xiaotai/platform.h"

#include <inttypes.h>
#include <stdio.h>

_Static_assert(XIAOTAI_RECOGNITION_CONFIG_LINE_MAX <= XIAOTAI_AT_NOTIFY_MAX, "CONFIG must fit in one notice");

static xiaotai_recognition_config_options_t s_options;
static bool s_initialized;

xiaotai_err_t xiaotai_recognition_config_configure(const xiaotai_recognition_config_options_t *options)
{
    if (s_initialized) return XIAOTAI_ERR_INVALID_STATE;
    s_options = options ? *options : (xiaotai_recognition_config_options_t){0};
    return XIAOTAI_OK;
}

static xiaotai_err_t legacy_result(xiaotai_err_t error)
{
    switch (error) {
    case XIAOTAI_ERR_PLATFORM_OFFLINE:
    case XIAOTAI_ERR_PLATFORM_STALE:
    case XIAOTAI_ERR_PLATFORM_GATED: return XIAOTAI_ERR_INVALID_STATE;
    case XIAOTAI_ERR_PLATFORM_BAD_SERVICE: return XIAOTAI_RECOGNITION_CONFIG_INVALID_RESPONSE;
    default: return error;
    }
}

static const char *legacy_error_name(xiaotai_err_t error)
{
    switch (error) {
    case XIAOTAI_FAIL: return "ESP_FAIL";
    case XIAOTAI_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case XIAOTAI_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case XIAOTAI_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case XIAOTAI_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case XIAOTAI_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case XIAOTAI_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case XIAOTAI_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case XIAOTAI_RECOGNITION_CONFIG_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    default: return "UNKNOWN ERROR";
    }
}

int xiaotai_recognition_config_format(char *out, size_t capacity, xiaotai_err_t result, int http, int code,
        const xiaotai_recognition_config_policy_t *policy)
{
    if (!out || !capacity || (result == XIAOTAI_OK && !policy)) return -1;
    int length;
    if (result != XIAOTAI_OK) {
        length = snprintf(out, capacity, "+RECOGNITION: error=%s http=%d code=%d",
                          legacy_error_name(legacy_result(result)), http, code);
    } else {
        length = snprintf(out, capacity,
            "+RECOGNITION: http=%d code=%d enabled=%d allowed=%d effective=%d revision=%" PRIu64
            " schemes=%u max-bytes=%" PRIu32 " max-seconds=%" PRIu32 " monthly-limit=%" PRIu32
            " used=%" PRIu64 " input-tokens=%" PRIu64 " output-tokens=%" PRIu64
            " tts-characters=%" PRIu64 " unknown-calls=%" PRIu64,
            http, code, policy->enabled, policy->allowed, policy->effective_enabled, policy->revision,
            (unsigned)policy->scheme_count, policy->max_file_bytes, policy->max_audio_seconds,
            policy->monthly_limit, policy->used, policy->input_tokens, policy->output_tokens,
            policy->tts_characters, policy->unknown_calls);
    }
    return length >= 0 && (size_t)length < capacity ? length : -1;
}

static void config_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    (void)ctx;
    int code = XIAOTAI_RECOGNITION_CONFIG_CODE_MISSING;
    xiaotai_recognition_config_policy_t policy;
    xiaotai_err_t error = reply->result;
    if (error == XIAOTAI_OK)
        error = xiaotai_recognition_config_decode(reply->body, reply->size, reply->http_status, &code,
                  XIAOTAI_RECOGNITION_CONFIG_MAX_BYTES, 0, s_options.legacy_http_business_error_as_fail, &policy);
    char line[XIAOTAI_RECOGNITION_CONFIG_LINE_MAX + 1U];
    if (xiaotai_recognition_config_format(line, sizeof(line), error, reply->http_status, code, &policy) >= 0)
        (void)xiaotai_at_notify(line);
}

static xiaotai_err_t config_run(int count, const char *const *args, const xiaotai_reply_t *reply)
{
    (void)count;
    (void)args;
    (void)reply;
    static const xiaotai_platform_request_t request = {
        .method = XIAOTAI_PLATFORM_GET,
        .service = XIAOTAI_PLATFORM_SERVICE_RECOGNITION,
        .path = XIAOTAI_RECOGNITION_CONFIG_PATH,
        .auth = XIAOTAI_PLATFORM_AUTH_SESSION,
    };
    return legacy_result(xiaotai_platform_request(&request, config_reply, NULL));
}

xiaotai_err_t xiaotai_module_recognition_config_init(void)
{
    static const xiaotai_at_verb_t config[] = { { .word = "CONFIG", .run = config_run } };
    xiaotai_err_t error = xiaotai_at_verbs_register("AT+RECOGNITION", config, sizeof(config) / sizeof(config[0]));
    if (error == XIAOTAI_OK) s_initialized = true;
    return error;
}
