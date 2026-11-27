// SPDX-License-Identifier: MIT
#pragma once

#include "recognition_audio.h"
#include "recognition_journal.h"
#include "recognition_protocol.h"
#include "recognition_spill.h"
#include "xiaotai/sounds.h"

#include <stdatomic.h>

typedef enum {
    RC_HTTP_NONE, RC_HTTP_POLICY, RC_HTTP_RECHECK, RC_HTTP_SUBMIT, RC_HTTP_QUERY, RC_HTTP_REFRESH, RC_HTTP_DOWNLOAD,
} rc_http_stage_t;

typedef enum {RC_BACKGROUND_NONE, RC_BACKGROUND_QUERY, RC_BACKGROUND_REFRESH} rc_background_t;

typedef struct {
    rc_multipart_t multipart;
    size_t received;
    char receipt[RC_RECEIPT_BYTES + 1U];
} rc_upload_t;

/* Except upload bytes and the atomic cancel flag, the owner alone uses this.
 * A cancelled operation stays resident until every accepted lender completes. */
typedef struct {
    uint32_t generation, seconds, capture_rate, activity_id, recording_id, play_id;
    uint32_t transfer_id, timer_id;
    int64_t deadline_us, prepare_deadline_us;
    int64_t played_since_us;
    uint64_t initial_revision, notice_revision;
    int64_t query_at_us, refresh_at_us;
    rc_http_stage_t http;
    rc_background_t background;
    xiaotai_recognition_phase_t before_refresh;
    bool journal_pending, journal_prepared, prepare_timed_out, post_queued;
    bool unbound, conflict, clearing;
    bool query_loading, query_explicit, query_conflict;
    bool play_requested, archive_pending;
    bool download_active, play_handoff;
    uint8_t releases;
    atomic_bool cancelled;
    int16_t *pcm;
    rc_spill_t *spill;          /* instead of pcm: a recording too long for memory, in flash */
    bool flush_wait;            /* the upload waits for the last of it to reach flash */
    rc_upload_t *upload;
    rc_audio_ready_fn_t audio_ready;
    void *audio_ctx;
    uint8_t *play_data;
    xiaotai_sound_t play_sound;
    rc_task_t *task;
    xiaotai_recognition_journal_record_t record;
    char old_request_id[XIAOTAI_RECOGNITION_ID_MAX];
    uint32_t request_scope, old_request_scope;
    bool request_scope_known, old_request_scope_known;
    xiaotai_recognition_status_t status;
    xiaotai_recognition_stats_t stats;
    xiaotai_recognition_policy_t policy;
} rc_operation_t;

xiaotai_err_t rc_at_init(void);
