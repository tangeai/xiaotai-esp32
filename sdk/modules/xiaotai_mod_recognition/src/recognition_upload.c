// SPDX-License-Identifier: MIT
#include "recognition_json.h"
#include "xiaotai/platform.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool boundary_collision(const uint8_t *pcm, size_t size, const char *boundary)
{
    const size_t length = strlen(boundary);
    for (size_t i = 0; i + length + 2U <= size; ++i)
        if (pcm[i] == '-' && pcm[i + 1] == '-' &&
            (!i || (i >= 2 && pcm[i - 2] == '\r' && pcm[i - 1] == '\n')) &&
            !memcmp(pcm + i + 2U, boundary, length))
            return true;
    return false;
}

xiaotai_err_t rc_multipart_prepare(rc_multipart_t *form, const char *id,
                                   const xiaotai_recognition_policy_t *policy,
                                   const uint8_t *pcm, size_t bytes, uint32_t rate,
                                   uint8_t channels, const uint8_t random[16])
{
    if (!form) return XIAOTAI_ERR_INVALID_ARG;
    memset(form, 0, sizeof(*form));
    if (!rc_id_valid(id) || !pcm || !random) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_err_t error = rc_pcm_check(policy, bytes, rate, channels);
    if (error != XIAOTAI_OK) return error;
    char boundary[41] = "xiaotai-";
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        boundary[8U + 2U * i] = hex[random[i] >> 4];
        boundary[9U + 2U * i] = hex[random[i] & 15U];
    }
    if (boundary_collision(pcm, bytes, boundary)) return XIAOTAI_ERR_INVALID_ARG;

    int prefix = snprintf(form->prefix, sizeof(form->prefix),
        "--%s\r\nContent-Disposition: form-data; name=\"request_id\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"config_revision\"\r\n\r\n%" PRIu64 "\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"media_type\"\r\n\r\naudio\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"audio_format\"\r\n\r\npcm_s16le\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"sample_rate\"\r\n\r\n%" PRIu32 "\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"channels\"\r\n\r\n%u\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"files\"; filename=\"recording.pcm\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        boundary, id, boundary, policy->revision, boundary, boundary, boundary, rate, boundary,
        (unsigned)channels, boundary);
    int suffix = snprintf(form->suffix, sizeof(form->suffix), "\r\n--%s--\r\n", boundary);
    int content = snprintf(form->content_type, sizeof(form->content_type), "multipart/form-data; boundary=%s", boundary);
    if (prefix <= 0 || (size_t)prefix >= sizeof(form->prefix) ||
        suffix <= 0 || (size_t)suffix >= sizeof(form->suffix) ||
        content <= 0 || (size_t)content >= sizeof(form->content_type))
        return XIAOTAI_ERR_INVALID_SIZE;
    size_t total = (size_t)prefix + bytes + (size_t)suffix;
    if (total > 6U * 1024U * 1024U) return XIAOTAI_ERR_INVALID_SIZE;
    form->pcm = pcm;
    form->pcm_bytes = bytes;
    form->prefix_bytes = (size_t)prefix;
    form->suffix_bytes = (size_t)suffix;
    form->total_bytes = total;
    return XIAOTAI_OK;
}

xiaotai_err_t rc_multipart_read(void *context, uint8_t *data, size_t capacity, size_t *size)
{
    if (!size) return XIAOTAI_ERR_INVALID_ARG;
    *size = 0;
    rc_multipart_t *form = context;
    if (!form || !form->pcm || !data || !capacity) return XIAOTAI_ERR_INVALID_ARG;
    if (form->cancelled && form->cancelled(form->cancel_ctx)) return XIAOTAI_ERR_PLATFORM_CANCELLED;
    if (capacity > RC_STREAM_CHUNK) capacity = RC_STREAM_CHUNK;
    const uint8_t *segments[] = {(const uint8_t *)form->prefix, form->pcm, (const uint8_t *)form->suffix};
    const size_t sizes[] = {form->prefix_bytes, form->pcm_bytes, form->suffix_bytes};
    size_t skip = form->offset;
    for (size_t i = 0; i < 3 && *size < capacity; ++i) {
        if (skip >= sizes[i]) { skip -= sizes[i]; continue; }
        const size_t left = sizes[i] - skip;
        const size_t room = capacity - *size;
        const size_t copy = left < room ? left : room;
        memcpy(data + *size, segments[i] + skip, copy);
        *size += copy;
        skip = 0;
    }
    form->offset += *size;
    return XIAOTAI_OK;
}

xiaotai_err_t rc_receipt_decode(const char *body, size_t length, int http,
                                const char *device_id, const char *request_id, int *code,
                                rc_receipt_t *receipt)
{
    if (!code || !receipt || !device_id || !device_id[0] || !rc_id_valid(request_id))
        return XIAOTAI_ERR_INVALID_ARG;
    *code = -1;
    rc_json_t json;
    xiaotai_err_t error = rc_json_open(&json, body, length, RC_RECEIPT_BYTES);
    if (error != XIAOTAI_OK) return error;
    (void)rc_json_code(&json, 0, true, code);
    const int data = rc_get(&json, 0, "data");
    const int state = rc_task_state(&json, rc_get(&json, data, "status"));
    uint64_t revision;
    if ((http == 200 || http == 202) && *code == 200 && state >= 0 &&
        (http != 202 || state == XIAOTAI_RECOGNITION_TASK_QUEUED) &&
        rc_equals(&json, rc_get(&json, data, "device_id"), device_id) &&
        rc_equals(&json, rc_get(&json, data, "request_id"), request_id) &&
        rc_json_is(&json, rc_get(&json, data, "results"), XIAOTAI_JSON_ARRAY) &&
        rc_json_uint(&json, rc_get(&json, data, "status_revision"), XIAOTAI_RECOGNITION_REVISION_MAX, &revision) &&
        revision) {
        *receipt = (rc_receipt_t){.state = (xiaotai_recognition_task_state_t)state, .revision = revision};
    } else {
        error = RC_INVALID_RESPONSE;
    }
    rc_json_close(&json);
    return error;
}
