// SPDX-License-Identifier: MIT
#include "recognition_protocol.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    xiaotai_recognition_policy_t policy = {
        .enabled = true, .allowed = true, .effective_enabled = true,
        .revision = XIAOTAI_RECOGNITION_REVISION_MAX, .monthly_limit = 1000000,
        .scheme_count = 8, .used = 1000000 - 8,
        .max_file_bytes = 320044, .max_audio_seconds = 10,
    };
    const xiaotai_recognition_policy_t baseline = policy;
    assert(rc_pcm_check(&policy, 32000, 16000, 1) == XIAOTAI_OK);
    assert(rc_pcm_check(&policy, 320000, 16000, 1) == XIAOTAI_OK);
    assert(rc_pcm_check(&policy, 192000, 48000, 2) == XIAOTAI_OK);
    assert(rc_pcm_check(&policy, 16000, 8000, 1) == XIAOTAI_OK);
    assert(rc_pcm_check(NULL, 32000, 16000, 1) == XIAOTAI_ERR_INVALID_ARG);
    assert(rc_pcm_check(&policy, 32000, 7999, 1) == XIAOTAI_ERR_INVALID_ARG);
    assert(rc_pcm_check(&policy, 32000, 48001, 1) == XIAOTAI_ERR_INVALID_ARG);
    assert(rc_pcm_check(&policy, 32000, 16000, 0) == XIAOTAI_ERR_INVALID_ARG);
    assert(rc_pcm_check(&policy, 32000, 16000, 3) == XIAOTAI_ERR_INVALID_ARG);
    assert(rc_pcm_check(&policy, 31998, 16000, 1) == XIAOTAI_ERR_INVALID_SIZE);
    assert(rc_pcm_check(&policy, 32001, 16000, 1) == XIAOTAI_ERR_INVALID_SIZE);
    assert(rc_pcm_check(&policy, 320002, 16000, 1) == XIAOTAI_ERR_INVALID_SIZE);
    assert(rc_pcm_check(&policy, SIZE_MAX, 16000, 1) == XIAOTAI_ERR_INVALID_SIZE);
#define BAD(member, value, err) do { \
    policy.member = value; \
    assert(rc_pcm_check(&policy, 32000, 16000, 1) == (err)); \
    policy = baseline; \
} while (0)
    BAD(revision, XIAOTAI_RECOGNITION_REVISION_MAX + 1, XIAOTAI_ERR_INVALID_ARG);
    BAD(enabled, false, XIAOTAI_ERR_INVALID_STATE);
    BAD(allowed, false, XIAOTAI_ERR_INVALID_STATE);
    BAD(effective_enabled, false, XIAOTAI_ERR_INVALID_STATE);
    BAD(monthly_limit, 0, XIAOTAI_ERR_INVALID_STATE);
    BAD(monthly_limit, 1000001, XIAOTAI_ERR_INVALID_STATE);
    BAD(scheme_count, 0, XIAOTAI_ERR_INVALID_STATE);
    BAD(scheme_count, 9, XIAOTAI_ERR_INVALID_STATE);
    BAD(used, 1000001, XIAOTAI_ERR_INVALID_STATE);
    BAD(used, 1000000 - 7, XIAOTAI_ERR_INVALID_STATE);
    BAD(max_audio_seconds, 0, XIAOTAI_ERR_INVALID_SIZE);
    BAD(max_audio_seconds, 61, XIAOTAI_ERR_INVALID_SIZE);
    BAD(max_file_bytes, 43, XIAOTAI_ERR_INVALID_SIZE);
    BAD(max_file_bytes, 32043, XIAOTAI_ERR_INVALID_SIZE);
    BAD(max_file_bytes, RC_PCM_FILE_BYTES + 1, XIAOTAI_ERR_INVALID_SIZE);
    assert(rc_id_valid("Ab9_-") && !rc_id_valid(NULL) && !rc_id_valid("") && !rc_id_valid("a/b"));
    char max_id[XIAOTAI_RECOGNITION_ID_MAX + 1];
    memset(max_id, 'a', sizeof(max_id));
    max_id[64] = 0;
    assert(rc_id_valid(max_id));
    max_id[64] = 'a';
    max_id[65] = 0;
    assert(!rc_id_valid(max_id));
}
