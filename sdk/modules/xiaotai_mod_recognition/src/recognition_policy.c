// SPDX-License-Identifier: MIT
#include "recognition_json.h"
#include "xiaotai/modules/recognition_config.h"

xiaotai_err_t rc_policy_decode(const char *body, size_t length, int http, int *code,
                               xiaotai_recognition_policy_t *policy)
{
    if (!policy) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_recognition_config_policy_t value;
    xiaotai_err_t error = xiaotai_recognition_config_decode(body, length, http, code,
        RC_POLICY_BYTES, RC_BULK_RESERVE, false, &value);
    if (error != XIAOTAI_OK) return error;
    *policy = (xiaotai_recognition_policy_t){
        .valid = true, .enabled = value.enabled, .allowed = value.allowed,
        .effective_enabled = value.effective_enabled, .revision = value.revision,
        .monthly_limit = value.monthly_limit, .scheme_count = value.scheme_count,
        .max_file_bytes = value.max_file_bytes, .max_audio_seconds = value.max_audio_seconds,
        .used = value.used, .input_tokens = value.input_tokens, .output_tokens = value.output_tokens,
        .tts_characters = value.tts_characters, .unknown_calls = value.unknown_calls,
    };
    return XIAOTAI_OK;
}
