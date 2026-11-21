// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_RECOGNITION_CONFIG_PATH "/v1/recognition/config"
#define XIAOTAI_RECOGNITION_CONFIG_MAX_BYTES 8191U
#define XIAOTAI_RECOGNITION_CONFIG_LINE_MAX 352U
#define XIAOTAI_RECOGNITION_CONFIG_CODE_MISSING (-1)
#define XIAOTAI_RECOGNITION_CONFIG_INVALID_RESPONSE ((xiaotai_err_t)0x108)

typedef struct {
    bool enabled, allowed, effective_enabled;
    uint64_t revision;
    uint8_t scheme_count;
    uint32_t max_file_bytes, max_audio_seconds, monthly_limit;
    uint64_t used, input_tokens, output_tokens, tts_characters, unknown_calls;
} xiaotai_recognition_config_policy_t;

typedef struct {
    /* Report ESP_FAIL for a nonzero, non-200 business code in a 2xx
     * JSON object, as the direct HTTP client did (-1 means no code). */
    bool legacy_http_business_error_as_fail;
} xiaotai_recognition_config_options_t;

/* [before init] A null options pointer selects the default. */
xiaotai_err_t xiaotai_recognition_config_configure(const xiaotai_recognition_config_options_t *options);

/* Borrow one reply body. On failure the policy is unchanged, but *code
 * holds the business code if readable. reserve_bytes is the minimum BULK
 * water level to keep free while parsing; zero is valid without PSRAM. */
xiaotai_err_t xiaotai_recognition_config_decode(const char *body, size_t length, int http, int *code,
        size_t max_body_bytes, size_t reserve_bytes, bool legacy_http_business_error_as_fail,
        xiaotai_recognition_config_policy_t *policy);

/* Format one legacy CONFIG notification, without CRLF. A nonnegative result
 * is its length; negative means it did not fit or the arguments were invalid. */
int xiaotai_recognition_config_format(char *out, size_t capacity, xiaotai_err_t result, int http, int code,
        const xiaotai_recognition_config_policy_t *policy);

/* [init] Register only the read-only AT+RECOGNITION CONFIG verb. */
xiaotai_err_t xiaotai_module_recognition_config_init(void);

#ifdef __cplusplus
}
#endif
