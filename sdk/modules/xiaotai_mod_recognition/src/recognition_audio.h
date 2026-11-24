// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/platform.h"

typedef struct {
    uint8_t *data; /* transferred to the owner callback on success */
    size_t size;
    int http_status, code;
    xiaotai_err_t error;
    uint32_t generation;
} rc_audio_reply_t;

typedef void (*rc_audio_reply_fn_t)(const rc_audio_reply_t *reply, void *ctx);

/* Accepted transfers call done on the owner, including after cancellation.
 * The predicate and ctx remain valid through done; bytes remain borrowed by
 * the platform until that callback. */
xiaotai_err_t rc_audio_transfer(const char *request_id, const char *item_id,
                                bool (*cancelled)(void *), rc_audio_reply_fn_t done,
                                void *ctx, uint32_t *transfer_id);

/* Owner's download stage. The scheduler supplies a selected, ready item.
 * On success ready takes ownership of the candidate WAV bytes and keeps the
 * same operation/activity. Until playback owns the operation, finish returns
 * the activity; the caller still owns and must release the WAV bytes. */
typedef void (*rc_audio_ready_fn_t)(uint32_t token, uint8_t *data, size_t size, void *ctx);
xiaotai_err_t rc_recognition_audio_start(const char *item_id, rc_audio_ready_fn_t ready, void *ctx);
xiaotai_err_t rc_recognition_audio_finish(uint32_t token, xiaotai_err_t result);
