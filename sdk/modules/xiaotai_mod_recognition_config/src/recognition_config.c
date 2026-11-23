// SPDX-License-Identifier: MIT
#include "xiaotai/detail/recognition_json.h"
#include "xiaotai/modules/recognition_config.h"

#define POLICY_SCHEMES_MAX 8U
#define POLICY_REVISION_MAX UINT64_C(9007199254740991)
#define POLICY_FILE_MAX (5U * 1024U * 1024U)
#define POLICY_SECONDS_MAX 60U

static bool scheme_list(rc_json_t *json, int array, uint8_t *size)
{
    if (!rc_json_is(json, array, XIAOTAI_JSON_ARRAY) ||
        json->tokens[array].children > POLICY_SCHEMES_MAX)
        return false;
    *size = (uint8_t)json->tokens[array].children;
    int ids[POLICY_SCHEMES_MAX];
    for (size_t i = 0; i < *size; ++i) {
        const int scheme = xiaotai_json_at(&json->doc, array, i);
        ids[i] = rc_get(json, scheme, "scheme_id");
        uint64_t version;
        if (!rc_json_is(json, ids[i], XIAOTAI_JSON_STRING) ||
            json->tokens[ids[i]].start == json->tokens[ids[i]].end ||
            !rc_json_uint(json, rc_get(json, scheme, "scheme_version"), POLICY_REVISION_MAX, &version) ||
            !version)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (rc_json_same(json, ids[i], ids[j]) || json->error != XIAOTAI_OK) return false;
    }
    return true;
}

static bool policy_read(rc_json_t *json, int data, xiaotai_recognition_config_policy_t *policy)
{
    const int rule = rc_get(json, data, "policy");
    const int limits = rc_get(json, data, "upload_limits");
    const int usage = rc_get(json, data, "usage");
    uint64_t monthly, bytes, seconds;
    if (xiaotai_json_bool(&json->doc, rc_get(json, rule, "enabled"), &policy->enabled) != XIAOTAI_OK ||
        xiaotai_json_bool(&json->doc, rc_get(json, rule, "allowed"), &policy->allowed) != XIAOTAI_OK ||
        xiaotai_json_bool(&json->doc, rc_get(json, data, "effective_enabled"), &policy->effective_enabled) != XIAOTAI_OK ||
        !rc_json_uint(json, rc_get(json, rule, "revision"), POLICY_REVISION_MAX, &policy->revision) ||
        !rc_json_uint(json, rc_get(json, rule, "monthly_limit"), 1000000, &monthly) ||
        !scheme_list(json, rc_get(json, rule, "schemes"), &policy->scheme_count))
        return false;
    policy->monthly_limit = (uint32_t)monthly;
    if (limits >= 0 && !rc_json_is(json, limits, XIAOTAI_JSON_NULL)) {
        if (!rc_json_uint(json, rc_get(json, limits, "max_file_bytes"), POLICY_FILE_MAX, &bytes) ||
            !rc_json_uint(json, rc_get(json, limits, "max_audio_seconds"), POLICY_SECONDS_MAX, &seconds))
            return false;
        policy->max_file_bytes = (uint32_t)bytes;
        policy->max_audio_seconds = (uint32_t)seconds;
    }
    if (policy->effective_enabled && (!policy->enabled || !policy->allowed || !policy->monthly_limit ||
        !policy->scheme_count || !policy->max_file_bytes || !policy->max_audio_seconds))
        return false;
    return rc_json_uint(json, rc_get(json, usage, "used"), POLICY_REVISION_MAX, &policy->used) &&
        rc_json_uint(json, rc_get(json, usage, "input_tokens"), POLICY_REVISION_MAX, &policy->input_tokens) &&
        rc_json_uint(json, rc_get(json, usage, "output_tokens"), POLICY_REVISION_MAX, &policy->output_tokens) &&
        rc_json_uint(json, rc_get(json, usage, "tts_characters"), POLICY_REVISION_MAX, &policy->tts_characters) &&
        rc_json_uint(json, rc_get(json, usage, "unknown_calls"), POLICY_REVISION_MAX, &policy->unknown_calls);
}

xiaotai_err_t xiaotai_recognition_config_decode(const char *body, size_t length, int http, int *code,
        size_t max_body_bytes, size_t reserve_bytes, bool legacy_http_business_error_as_fail,
        xiaotai_recognition_config_policy_t *policy)
{
    if (!code || !policy || !max_body_bytes || max_body_bytes > XIAOTAI_RECOGNITION_CONFIG_MAX_BYTES)
        return XIAOTAI_ERR_INVALID_ARG;
    *code = XIAOTAI_RECOGNITION_CONFIG_CODE_MISSING;
    rc_json_t json;
    xiaotai_err_t error = rc_json_open_reserved(&json, body, length, max_body_bytes, reserve_bytes);
    if (error != XIAOTAI_OK) return error;
    (void)rc_json_code(&json, 0, false, code);
    xiaotai_recognition_config_policy_t value = {0};
    if (http == 200 && *code == 200 && policy_read(&json, rc_get(&json, 0, "data"), &value)) {
        *policy = value;
    } else if (json.error != XIAOTAI_OK) {
        error = json.error;
    } else if (legacy_http_business_error_as_fail && http >= 200 && http < 300 &&
               rc_json_is(&json, 0, XIAOTAI_JSON_OBJECT) && *code != 0 && *code != 200) {
        error = XIAOTAI_FAIL;
    } else {
        error = XIAOTAI_RECOGNITION_CONFIG_INVALID_RESPONSE;
    }
    rc_json_close(&json);
    return error;
}
