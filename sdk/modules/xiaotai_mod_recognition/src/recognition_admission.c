// SPDX-License-Identifier: MIT
#include "recognition_protocol.h"

bool rc_id_valid(const char *id)
{
    if (!id || !id[0]) return false;
    for (size_t i = 0; i < XIAOTAI_RECOGNITION_ID_MAX; ++i) {
        const char c = id[i];
        if (!c) return true;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return false;
}

xiaotai_err_t rc_pcm_check(const xiaotai_recognition_policy_t *policy, size_t bytes,
                           uint32_t sample_rate, uint8_t channels)
{
    if (!policy || sample_rate < 8000 || sample_rate > 48000 ||
        channels < 1 || channels > 2 || policy->revision > XIAOTAI_RECOGNITION_REVISION_MAX)
        return XIAOTAI_ERR_INVALID_ARG;
    if (!policy->enabled || !policy->allowed || !policy->effective_enabled ||
        !policy->scheme_count || policy->scheme_count > XIAOTAI_RECOGNITION_ITEMS_MAX ||
        !policy->monthly_limit || policy->monthly_limit > 1000000 ||
        policy->used > policy->monthly_limit ||
        policy->scheme_count > policy->monthly_limit - policy->used)
        return XIAOTAI_ERR_INVALID_STATE;

    const size_t frame = 2U * channels;
    const size_t second = sample_rate * frame;
    if (!policy->max_audio_seconds || policy->max_audio_seconds > XIAOTAI_RECOGNITION_SECONDS_MAX ||
        policy->max_file_bytes < 44 || policy->max_file_bytes > RC_PCM_FILE_BYTES ||
        bytes % frame || bytes < second || bytes > second * policy->max_audio_seconds ||
        bytes > (size_t)policy->max_file_bytes - 44U)
        return XIAOTAI_ERR_INVALID_SIZE;
    return XIAOTAI_OK;
}
