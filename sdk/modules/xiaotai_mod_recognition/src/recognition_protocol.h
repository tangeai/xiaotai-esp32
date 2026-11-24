// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/modules/recognition.h"

/* Private protocol results preserve the deployed INVALID_RESPONSE value. */
#define RC_INVALID_RESPONSE ((xiaotai_err_t)0x108)
#define RC_POLICY_BYTES 2048U
#define RC_RECEIPT_BYTES 8192U
#define RC_TASK_BYTES (1024U * 1024U)
#define RC_PCM_FILE_BYTES (5U * 1024U * 1024U)
#define RC_STREAM_CHUNK 1024U

bool rc_id_valid(const char *id);
xiaotai_err_t rc_pcm_check(const xiaotai_recognition_policy_t *policy, size_t bytes,
                           uint32_t sample_rate, uint8_t channels);
/* Body is borrowed, length excludes any terminator. A valid business code
 * is reported even on HTTP error, separately from whether the body is usable. */
xiaotai_err_t rc_policy_decode(const char *body, size_t length, int http, int *code,
                               xiaotai_recognition_policy_t *policy);

typedef struct {
    const uint8_t *pcm;
    size_t pcm_bytes, prefix_bytes, suffix_bytes, total_bytes, offset;
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;
    char content_type[96], prefix[1280], suffix[80];
} rc_multipart_t;

/* Caller allocates the builder in BULK. PCM stays borrowed until the transfer
 * ends. Bind the optional cancellation predicate after prepare and before use. */
xiaotai_err_t rc_multipart_prepare(rc_multipart_t *form, const char *id,
                                   const xiaotai_recognition_policy_t *policy,
                                   const uint8_t *pcm, size_t bytes, uint32_t rate,
                                   uint8_t channels, const uint8_t random[16]);
xiaotai_err_t rc_multipart_read(void *context, uint8_t *data, size_t capacity, size_t *size);

typedef struct {
    uint64_t revision;
    xiaotai_recognition_task_state_t state;
} rc_receipt_t;

xiaotai_err_t rc_receipt_decode(const char *body, size_t length, int http,
                                const char *device_id, const char *request_id, int *code,
                                rc_receipt_t *receipt);

/* Raw offsets refer to the retained HTTP envelope, including on notifications
 * (where only payload is kept). ID and summary spans exclude JSON quotes. */
typedef struct { uint32_t start, size; } rc_span_t;
typedef struct {
    rc_span_t id, summary, result;
    int32_t error_code, speech_error_code;
    uint8_t state, output, speech;
    bool alarm;
} rc_item_t;

typedef struct {
    xiaotai_recognition_result_info_t info;
    rc_item_t items[XIAOTAI_RECOGNITION_ITEMS_MAX];
    char *body;
    size_t length;
} rc_task_t;

xiaotai_err_t rc_task_decode(const char *body, size_t length, int http,
                             const char *device_id, const char *request_id, int *code,
                             rc_task_t **task);
/* Unrelated identities return NOT_FOUND. Config notices return a revision
 * and no task; result notices return a task and revision zero. */
xiaotai_err_t rc_notification_decode(const char *body, size_t length, const char *device_id,
                                     const char *request_id, uint64_t *revision, rc_task_t **task);
void rc_task_free(rc_task_t *task);
xiaotai_err_t rc_task_string(const rc_task_t *task, rc_span_t span, char *out,
                             size_t capacity, size_t *size);
