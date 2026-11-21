// SPDX-License-Identifier: MIT
#include "recognition_internal.h"
#include "recognition_decision.h"
#include "recognition_playback.h"
#include "recognition_result.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/json_view.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/recognition_config.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define RC_SERVICE "recognition"
#define RC_SUBMIT_PATH "/v1/recognition/requests"
#define RC_RESERVE (256U * 1024U)
#define RC_POLICY_MS 20000U
#define RC_POLICY_HTTP_MS 15000U
#define RC_UPLOAD_MS 90000U
#define RC_UPLOAD_HTTP_MS 60000U
#define RC_JOURNAL_MS 5000U
#define RC_RELEASE_MS 100U
#define RC_RELEASE_TRIES 3U
#define RC_FOLLOWUP_US 2000000
#define RC_QUERY_MS 25000U
#define RC_QUERY_HTTP_MS 20000U
#define RC_DOWNLOAD_MS 25000U
#define RC_QUERY_BLOCK 4096U
#define RC_POLL_US 5000000
#define RC_SYNC_MS 250U

#define RC(name) XIAOTAI_RECOGNITION_##name
#define RJ(name) XIAOTAI_RECOGNITION_JOURNAL_##name

static rc_operation_t op;
static xiaotai_recognition_journal_job_t job;
static xiaotai_recognition_config_t config;
static bool initialized;
static atomic_bool started;
static struct {
    uint32_t generation, revision, timer_id;
    unsigned query_failures, refresh_failures;
} sync;
static _Atomic uint32_t active_token;
static atomic_flag snapshot_lock = ATOMIC_FLAG_INIT;
static struct {
    xiaotai_recognition_status_t status;
    xiaotai_recognition_stats_t stats;
    xiaotai_recognition_policy_t policy;
    uint32_t generation, recording_id;
} snapshot;

typedef struct {
    xiaotai_recognition_action_t action;
    uint32_t argument, generation;
} action_t;

static void stage_timeout(void *unused);
static void cancel_operation(xiaotai_err_t error);
static void clear_prepared(xiaotai_err_t error);
static void settle_cancel(void);
static xiaotai_err_t read_policy(rc_http_stage_t stage);
static void background_cancel_done(void);
static void download_end(xiaotai_recognition_phase_t next, xiaotai_err_t error);
static void playback_tick(void);
static void finish_refresh(xiaotai_err_t error, int http, int code, bool retry);
static xiaotai_err_t sync_schedule(uint32_t delay_ms);

static void publish(void)
{
    rc_result_flush();
    atomic_store_explicit(&active_token, op.status.token, memory_order_release);
    if (atomic_flag_test_and_set_explicit(&snapshot_lock, memory_order_acquire)) return;
    snapshot.status = op.status;
    snapshot.policy = op.policy;
    snapshot.stats = op.stats;
    snapshot.generation = op.generation;
    snapshot.recording_id = op.recording_id;
    atomic_flag_clear_explicit(&snapshot_lock, memory_order_release);
}

static bool borrowed(void)
{
    return op.recording_id || op.play_id || op.http != RC_HTTP_NONE || op.journal_pending || op.post_queued ||
        op.query_loading;
}

static void phase(xiaotai_recognition_phase_t next)
{
    const bool changed = next != op.status.phase;
    op.status.phase = next;
    op.status.cleanup_pending = borrowed();
    publish();
    if (changed) {
        xiaotai_eventlog_note(RC_SERVICE, "phase", op.status.token, next);
        xiaotai_args_t args = {.value = (int32_t)next};
        const size_t id = strlen(op.status.request_id);
        if (id < sizeof(args.text)) memcpy(args.text, op.status.request_id, id + 1U);
        (void)xiaotai_event_emit(XIAOTAI_EVENT_RECOGNITION_STATE, &args, NULL, 0);
    }
}

static void decision(xiaotai_recognition_action_t action, xiaotai_err_t error)
{
    if (++op.status.decision_serial == 0) ++op.status.decision_serial;
    op.status.decision_action = action;
    op.status.decision_error = error;
    ++op.stats.decisions;
    if (error != XIAOTAI_OK) {
        ++op.stats.decisions_refused;
        xiaotai_eventlog_warn(RC_SERVICE, "decision.refused", op.status.token, error);
    }
    publish();
    const xiaotai_args_t args = {.value = error};
    const xiaotai_recognition_receipt_t receipt = {op.status.decision_serial, action, error};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_RECOGNITION_DECISION, &args, &receipt, sizeof(receipt));
}

static xiaotai_err_t normal_error(xiaotai_err_t error)
{
    if (error == XIAOTAI_ERR_PLATFORM_BAD_SERVICE) return RC_INVALID_RESPONSE;
    if (error == XIAOTAI_ERR_PLATFORM_STALE) return XIAOTAI_ERR_INVALID_STATE;
    return error;
}

static bool online(void)
{
    xiaotai_platform_status_t status;
    xiaotai_platform_status(&status);
    return status.phase == XIAOTAI_PLATFORM_ONLINE && status.generation == xiaotai_identity_generation();
}

static bool background_ready(void)
{
    return online() && !xiaotai_activity_active() && !xiaotai_quiesced();
}

static xiaotai_err_t endpoint(void)
{
    char address[XIAOTAI_PLATFORM_BASE_MAX + 1U];
    const xiaotai_err_t error = xiaotai_platform_endpoint(RC_SERVICE, address, sizeof(address));
    if (error != XIAOTAI_OK) return normal_error(error);
    return strncmp(address, "http://", 7U) == 0 ? XIAOTAI_OK : RC_INVALID_RESPONSE;
}

static xiaotai_err_t current(void)
{
    if (op.generation != xiaotai_identity_generation() || !online()) return XIAOTAI_ERR_INVALID_STATE;
    if (op.deadline_us && xiaotai_time_us() >= op.deadline_us) return XIAOTAI_ERR_TIMEOUT;
    return XIAOTAI_OK;
}

static void drop_timer(void)
{
    xiaotai_timer_cancel(op.timer_id);
    op.timer_id = 0;
    op.deadline_us = 0;
}

static xiaotai_err_t deadline(uint32_t ms)
{
    drop_timer();
    const xiaotai_err_t error = xiaotai_timer_start(ms, stage_timeout, NULL, &op.timer_id);
    if (error == XIAOTAI_OK) op.deadline_us = xiaotai_time_us() + (int64_t)ms * 1000;
    return error;
}

static void release_activity(void)
{
    if (!op.activity_id) return;
    const uint32_t id = op.activity_id;
    op.activity_id = 0;
    xiaotai_activity_end(id);
}

static void buffers_free(void)
{
    if (op.recording_id || op.http == RC_HTTP_SUBMIT) return;
    xiaotai_mem_free(op.pcm);
    xiaotai_mem_free(op.upload);
    op.pcm = NULL;
    op.upload = NULL;
}

static void finish(xiaotai_recognition_phase_t next, xiaotai_err_t error)
{
    drop_timer();
    buffers_free();
    release_activity();
    op.status.error = error;
    op.status.token = 0;
    phase(next);
}

static void discard_task(void)
{
    rc_result_clear();
    op.task = NULL;
    op.status.task_revision = 0;
    op.status.task_items = op.status.task_state = 0;
    op.play_requested = op.archive_pending = false;
    op.status.item_id[0] = '\0';
    op.status.consumption = RC(UNTOUCHED);
    op.status.playback_paused = false;
    op.status.playback_ms = op.status.playback_total_ms = 0;
}

static void clear_request(void)
{
    op.status.request_id[0] = '\0';
    op.request_scope_known = false;
}

static void show_request(const xiaotai_recognition_journal_record_t *record, bool verified)
{
    /* A generated ID is visible before PREPARE has assigned a scope. */
    op.request_scope_known = verified || (op.request_scope_known && op.request_scope == record->scope &&
                                          !strcmp(op.status.request_id, record->request_id));
    op.request_scope = record->scope;
    memcpy(op.status.request_id, record->request_id, sizeof(op.status.request_id));
}

static bool durable_scope_allowed(const xiaotai_recognition_journal_record_t *record)
{
    if (strcmp(record->request_id, op.old_request_id)) return true;
    /* Bare notifications carry no scope and keep the original ID quarantine. */
    return op.old_request_scope_known && record->scope > op.old_request_scope &&
           !xiaotai_recognition_journal_busy();
}

static void hide_old_binding(void)
{
    discard_task();
    if (op.status.request_id[0]) {
        memcpy(op.old_request_id, op.status.request_id, sizeof(op.old_request_id));
        op.old_request_scope_known = op.request_scope_known;
        op.old_request_scope = op.request_scope;
    }
    clear_request();
    op.status.possibly_sent = op.status.rejected_without_task = false;
    op.status.http_status = op.status.config_http_status = 0;
    op.status.code = op.status.config_code = 0;
    op.status.config_revision = op.notice_revision = 0;
    op.status.config_error = XIAOTAI_OK;
    op.status.config_enabled = false;
    op.policy = (xiaotai_recognition_policy_t){0};
    op.query_at_us = op.refresh_at_us = 0;
    sync.query_failures = sync.refresh_failures = 0;
    if (!borrowed()) {
        memset(&op.record, 0, sizeof(op.record));
        op.journal_prepared = false;
    }
}

static void reflect_record(const xiaotai_recognition_journal_mirror_t *mirror)
{
    if (borrowed() || op.status.token) return;
    const uint32_t generation = xiaotai_identity_generation();
    if (op.generation != generation) {
        hide_old_binding();
        op.generation = generation;
    }
    if (mirror->state == RJ(PRESENT)) {
        if (op.status.phase == RC(ACCEPTED) || op.status.phase == RC(RESULT)) return;
        char device[XIAOTAI_RECOGNITION_ID_MAX];
        if (xiaotai_identity_device_id(device, sizeof(device)) == XIAOTAI_OK &&
            strcmp(mirror->record.device_id, device) == 0 &&
            durable_scope_allowed(&mirror->record))
            show_request(&mirror->record, true);
        else
            clear_request();
        op.status.error = XIAOTAI_ERR_INVALID_STATE;
        phase(RC(RECOVERY));
    } else if (mirror->state == RJ(ERROR)) {
        op.status.error = mirror->error;
        phase(RC(RECOVERY));
    } else if (mirror->state == RJ(ABSENT) && op.status.phase == RC(RECOVERY) && !op.unbound) {
        clear_request();
        op.status.error = XIAOTAI_OK;
        phase(RC(IDLE));
    }
}

static void loaded(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                   const xiaotai_recognition_journal_mirror_t *mirror, void *unused)
{
    (void)error;
    (void)item;
    (void)unused;
    rc_result_update(mirror);
    reflect_record(mirror);
    (void)sync_schedule(1U);
}

static xiaotai_err_t load_record(void)
{
    _Static_assert(sizeof(xiaotai_recognition_journal_job_t) <= 256U, "owner stack budget");
    const xiaotai_recognition_journal_job_t load = {.operation = RJ(LOAD), .done = loaded};
    return xiaotai_recognition_journal_submit(&load);
}

typedef struct {
    xiaotai_recognition_done_fn_t done;
    void *ctx;
} scope_request_t;

static void scope_advanced(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                           const xiaotai_recognition_journal_mirror_t *mirror, void *arg)
{
    (void)item;
    const scope_request_t request = *(const scope_request_t *)arg;
    xiaotai_mem_free(arg);
    if (error == XIAOTAI_OK) {
        ++op.stats.scope_advances;
        xiaotai_eventlog_note(RC_SERVICE, "scope.advanced", 0, 0);
    } else {
        ++op.stats.journal_failures;
        if (error == XIAOTAI_ERR_TIMEOUT) ++op.stats.journal_timeouts;
        xiaotai_eventlog_error(RC_SERVICE, "scope.failed", 0, error);
    }
    rc_result_update(mirror);
    publish();
    request.done(error, request.ctx);
}

xiaotai_err_t xiaotai_recognition_scope_advance(xiaotai_recognition_done_fn_t done, void *ctx)
{
    if (!done) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner() || !atomic_load_explicit(&started, memory_order_acquire))
        return XIAOTAI_ERR_INVALID_STATE;
    scope_request_t *request = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*request));
    if (!request) return XIAOTAI_ERR_NO_MEM;
    *request = (scope_request_t){done, ctx};
    const xiaotai_recognition_journal_job_t advance = {.operation = RJ(ADVANCE_SCOPE),
        .done = scope_advanced, .ctx = request};
    const xiaotai_err_t error = xiaotai_recognition_journal_submit(&advance);
    if (error != XIAOTAI_OK) xiaotai_mem_free(request);
    return error;
}

xiaotai_err_t xiaotai_recognition_maintenance_check(void)
{
    if (!xiaotai_on_owner() || !atomic_load_explicit(&started, memory_order_acquire) ||
        op.status.token || borrowed() || op.archive_pending || xiaotai_recognition_journal_busy())
        return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_recognition_journal_mirror_t *mirror = xiaotai_recognition_journal_mirror();
    if (mirror->state == RJ(ABSENT)) return XIAOTAI_OK;
    return mirror->state == RJ(ERROR) ? mirror->error : XIAOTAI_ERR_INVALID_STATE;
}

static xiaotai_err_t binding_prepare(xiaotai_binding_prepared_fn_t done, void *done_ctx, void *ctx)
{
    (void)ctx;
    return xiaotai_recognition_scope_advance(done, done_ctx);
}

static xiaotai_err_t environment_guard(const char *reason, void *ctx)
{
    (void)ctx;
    return reason && strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0 ?
        xiaotai_recognition_maintenance_check() : XIAOTAI_OK;
}

static void parked(xiaotai_err_t error, xiaotai_recognition_phase_t next)
{
    if (op.generation != xiaotai_identity_generation()) {
        const int status = op.status.http_status;
        const int32_t code = op.status.code;
        hide_old_binding();
        if (op.unbound) {
            op.status.http_status = status;
            op.status.code = code;
        }
        op.generation = xiaotai_identity_generation();
    }
    finish(next, error);
}

static bool report_transfer_unbound(int http, int code)
{
    if (http != 410 || code != XIAOTAI_PLATFORM_CODE_UNBOUND) return false;
    op.unbound = true;
    op.query_at_us = op.refresh_at_us = 0;
    xiaotai_platform_report_unbound(true);
    return true;
}

static void journal_done(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                         const xiaotai_recognition_journal_mirror_t *mirror, void *unused);

static xiaotai_err_t write_record(xiaotai_recognition_journal_operation_t action)
{
    job.operation = action;
    job.record = op.record;
    job.done = journal_done;
    const xiaotai_err_t error = xiaotai_recognition_journal_submit(&job);
    if (error == XIAOTAI_OK) {
        op.journal_pending = true;
        ++op.stats.journal_writes;
    } else {
        ++op.stats.journal_failures;
    }
    return error;
}

static void clear_prepared(xiaotai_err_t error)
{
    drop_timer();
    buffers_free();
    op.status.error = error;
    if (!op.journal_prepared) {
        parked(error, op.unbound ? RC(RECOVERY) :
            atomic_load(&op.cancelled) ? RC(CANCELLED) : RC(FAILED));
        return;
    }
    op.clearing = true;
    const xiaotai_err_t clear = write_record(RJ(CLEAR));
    if (clear != XIAOTAI_OK) {
        op.clearing = false;
        parked(clear, RC(RECOVERY));
    } else {
        phase(op.status.phase);
    }
}

static void journal_done(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                         const xiaotai_recognition_journal_mirror_t *mirror, void *unused)
{
    (void)item;
    (void)unused;
    op.journal_pending = false;
    if (error != XIAOTAI_OK) {
        ++op.stats.journal_failures;
        if (error == XIAOTAI_ERR_TIMEOUT && !op.prepare_timed_out) ++op.stats.journal_timeouts;
    }
    if (op.clearing) {
        op.clearing = false;
        op.journal_prepared = false;
        if (error == XIAOTAI_OK) clear_request();
        parked(error == XIAOTAI_OK ? op.status.error : error,
               error != XIAOTAI_OK || op.unbound ? RC(RECOVERY) :
               atomic_load(&op.cancelled) ? RC(CANCELLED) : RC(FAILED));
        return;
    }
    if (op.conflict) {
        op.conflict = false;
        if (error == XIAOTAI_OK && op.generation == xiaotai_identity_generation())
            op.refresh_at_us = xiaotai_time_us() + RC_FOLLOWUP_US;
        parked(error == XIAOTAI_OK ? op.status.error : error, RC(RECOVERY));
        return;
    }
    if (!op.prepare_timed_out && error == XIAOTAI_OK &&
        xiaotai_time_us() >= op.prepare_deadline_us) {
        op.prepare_timed_out = true;
        ++op.stats.journal_timeouts;
    }
    if (error != XIAOTAI_OK || op.prepare_timed_out) {
        parked(op.prepare_timed_out ? XIAOTAI_ERR_TIMEOUT : error, RC(RECOVERY));
        return;
    }
    if (mirror->state != RJ(PRESENT)) {
        parked(RC_INVALID_RESPONSE, RC(RECOVERY));
        return;
    }
    op.journal_prepared = true;
    op.record = mirror->record;
    if (!strcmp(op.status.request_id, op.record.request_id)) show_request(&op.record, true);
    const xiaotai_err_t condition = current();
    if (condition != XIAOTAI_OK || atomic_load(&op.cancelled)) {
        clear_prepared(atomic_load(&op.cancelled) ? op.status.error : condition);
        return;
    }
    error = deadline(RC_UPLOAD_MS);
    if (error != XIAOTAI_OK) {
        clear_prepared(error);
        return;
    }
    phase(RC(UPLOAD));
    (void)read_policy(RC_HTTP_RECHECK);
}

static xiaotai_err_t pcm_check(void)
{
    return rc_pcm_check(&op.policy, (size_t)op.status.target_samples * sizeof(int16_t), op.capture_rate, 1);
}

static void captured_progress(void)
{
    if (op.capture_rate) {
        publish();
        const xiaotai_args_t args = {.value = (int32_t)(op.status.samples / op.capture_rate)};
        (void)xiaotai_event_emit(XIAOTAI_EVENT_RECOGNITION_PROGRESS, &args, NULL, 0);
    }
}

static void recording_progress(uint32_t id, uint32_t samples, void *unused)
{
    (void)unused;
    if (!id || id != op.recording_id || op.status.phase != RC(RECORDING) ||
        op.generation != xiaotai_identity_generation() || atomic_load(&op.cancelled) ||
        samples <= op.status.samples || samples >= op.status.target_samples) return;
    op.status.samples = samples;
    captured_progress();
}

static void recorded(uint32_t id, xiaotai_err_t error, uint32_t samples, void *unused)
{
    (void)unused;
    if (id != op.recording_id || !id) {
        ++op.stats.stale;
        publish();
        return;
    }
    op.recording_id = 0; /* done is the only carrier that returns this borrow. */
    op.status.samples = samples;
    const xiaotai_err_t condition = current();
    if (atomic_load(&op.cancelled) || condition != XIAOTAI_OK) {
        ++op.stats.recordings_failed;
        if (!atomic_load(&op.cancelled)) cancel_operation(condition);
        else settle_cancel();
        return;
    }
    if (error != XIAOTAI_OK || samples != op.status.target_samples) {
        ++op.stats.recordings_failed;
        finish(RC(FAILED), error != XIAOTAI_OK ? error : XIAOTAI_FAIL);
        return;
    }
    ++op.stats.recordings_completed;
    if ((error = deadline(RC_JOURNAL_MS)) != XIAOTAI_OK) {
        finish(RC(FAILED), error);
        return;
    }
    op.prepare_deadline_us = op.deadline_us;
    op.record.config_revision = op.initial_revision;
    for (unsigned i = 0; i < 4U; ++i)
        (void)snprintf(op.record.request_id + (8U * i), 9U, "%08" PRIx32, xiaotai_random());
    clear_request();
    show_request(&op.record, false);
    error = write_record(RJ(PREPARE));
    if (error != XIAOTAI_OK) finish(RC(RECOVERY), error);
    else {
        // Capture has returned; the ID is being made durable.
        phase(RC(RECORDING));
        captured_progress();
    }
}

static void begin_recording(void)
{
    xiaotai_err_t error = pcm_check();
    if (error == XIAOTAI_OK) {
        const size_t bytes = (size_t)op.status.target_samples * sizeof(int16_t);
        xiaotai_mem_stats_t memory;
        xiaotai_mem_stats(XIAOTAI_MEM_BULK, &memory);
        if (memory.free < RC_RESERVE || bytes > memory.free - RC_RESERVE || bytes > memory.largest_block)
            error = XIAOTAI_ERR_NO_MEM;
        else if (!(op.pcm = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes)))
            error = XIAOTAI_ERR_NO_MEM;
    }
    if (error == XIAOTAI_OK) error = deadline(op.seconds * 1000U + 2000U);
    if (error == XIAOTAI_OK) {
        const xiaotai_media_record_t request = {.pcm = op.pcm, .samples = op.status.target_samples,
            .done = recorded, .progress = recording_progress};
        error = xiaotai_media_record(&request, &op.recording_id);
    }
    if (error != XIAOTAI_OK) finish(RC(FAILED), error);
    else {
        ++op.stats.recordings;
        phase(RC(RECORDING));
    }
}

static bool cancelled(void *unused)
{
    (void)unused;
    return atomic_load_explicit(&op.cancelled, memory_order_acquire);
}

static xiaotai_err_t read_upload(void *ctx, uint8_t *data, size_t capacity, size_t *size)
{
    rc_upload_t *upload = ctx;
    return rc_multipart_read(&upload->multipart, data, capacity, size);
}

static xiaotai_err_t write_upload(void *ctx, const uint8_t *data, size_t size)
{
    rc_upload_t *upload = ctx;
    if (cancelled(NULL)) return XIAOTAI_ERR_PLATFORM_CANCELLED;
    if (size > RC_RECEIPT_BYTES - upload->received) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(upload->receipt + upload->received, data, size);
    upload->received += size;
    upload->receipt[upload->received] = '\0';
    return XIAOTAI_OK;
}

static void uploaded(void *ctx, const xiaotai_platform_transfer_result_t *result)
{
    const rc_upload_t *upload = ctx;
    op.http = RC_HTTP_NONE;
    op.transfer_id = 0;
    const bool same_binding = result->generation == op.generation &&
                              op.generation == xiaotai_identity_generation();
    const bool was_cancelled = cancelled(NULL);
    const xiaotai_err_t condition = current();
    if (!same_binding) ++op.stats.stale;
    xiaotai_err_t error = normal_error(result->result);
    if (error == XIAOTAI_FAIL && result->receive_fault == XIAOTAI_PLATFORM_RECEIVE_FAULT_BODY_TRUNCATED)
        error = RC_INVALID_RESPONSE;
    int code = -1;
    rc_receipt_t receipt;
    if (error == XIAOTAI_OK && same_binding) {
        if (result->received != upload->received) error = RC_INVALID_RESPONSE;
        else error = rc_receipt_decode(upload->receipt, upload->received, result->http_status,
                                      op.record.device_id, op.record.request_id, &code, &receipt);
    }
    if (same_binding) {
        op.status.http_status = result->http_status;
        op.status.code = code == -1 ? 0 : code;
    }
    op.status.possibly_sent = result->sent || op.status.task_revision != 0;
    buffers_free();
    if (same_binding) (void)report_transfer_unbound(result->http_status, code);
    if (!op.unbound && (was_cancelled || !same_binding || condition != XIAOTAI_OK)) {
        atomic_store_explicit(&op.cancelled, true, memory_order_release);
        error = was_cancelled ? op.status.error : !same_binding ? XIAOTAI_ERR_INVALID_STATE : condition;
    }
    if (!op.status.possibly_sent) {
        clear_prepared(error != XIAOTAI_OK || was_cancelled ? error : RC_INVALID_RESPONSE);
    } else if (op.unbound) {
        ++op.stats.submissions_uncertain;
        /* The call above may immediately advance the binding. Preserve the
         * diagnostic, never the old request ID, until the binding event. */
        finish(RC(RECOVERY), error);
    } else if (same_binding && !cancelled(NULL) && condition == XIAOTAI_OK &&
               !op.status.task_revision && op.journal_prepared && error == RC_INVALID_RESPONSE &&
               result->http_status == 409 && code == 40901) {
        ++op.stats.submissions_conflict;
        op.status.error = error;
        op.query_at_us = xiaotai_time_us() + RC_FOLLOWUP_US;
        drop_timer();
        op.conflict = true;
        error = write_record(RJ(MARK_CONFLICT));
        if (error != XIAOTAI_OK) {
            op.conflict = false;
            finish(RC(RECOVERY), error);
        } else {
            phase(RC(RECOVERY));
        }
    } else if (same_binding && op.status.task_revision != 0) {
        finish(RC(RESULT), error);
    } else if (same_binding && !cancelled(NULL) && error == XIAOTAI_OK) {
        ++op.stats.submissions_accepted;
        op.query_at_us = xiaotai_time_us() + RC_FOLLOWUP_US;
        finish(RC(ACCEPTED), XIAOTAI_OK);
    } else {
        ++op.stats.submissions_uncertain;
        if (same_binding) op.query_at_us = xiaotai_time_us() + RC_FOLLOWUP_US;
        parked(error, RC(RECOVERY));
    }
}

static void send_upload(void *ctx)
{
    if ((uint32_t)(uintptr_t)ctx != op.status.token || !op.post_queued) return;
    op.post_queued = false;
    xiaotai_err_t error = current();
    if (cancelled(NULL)) {
        settle_cancel();
        return;
    }
    if (error == XIAOTAI_OK) error = endpoint();
    if (error == XIAOTAI_OK) {
        op.upload = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*op.upload));
        if (!op.upload) error = XIAOTAI_ERR_NO_MEM;
    }
    if (error == XIAOTAI_OK) {
        uint32_t random[4];
        for (size_t i = 0; i < 4U; ++i) random[i] = xiaotai_random();
        error = rc_multipart_prepare(&op.upload->multipart, op.record.request_id, &op.policy,
            (const uint8_t *)op.pcm, (size_t)op.status.target_samples * sizeof(int16_t),
            op.capture_rate, 1, (const uint8_t *)random);
    }
    if (error == XIAOTAI_OK) {
        op.upload->multipart.cancelled = cancelled;
        const xiaotai_platform_transfer_t transfer = {
            .request = {.method = XIAOTAI_PLATFORM_POST, .service = RC_SERVICE, .path = RC_SUBMIT_PATH,
                        .auth = XIAOTAI_PLATFORM_AUTH_SESSION, .timeout_ms = RC_UPLOAD_HTTP_MS},
            .content_type = op.upload->multipart.content_type, .body_size = op.upload->multipart.total_bytes,
            .response_max = RC_RECEIPT_BYTES, .stall_timeout_ms = 5000U,
        };
        const xiaotai_platform_stream_t stream = {
            .read = read_upload, .write = write_upload, .done = uploaded, .ctx = op.upload,
        };
        error = normal_error(xiaotai_platform_transfer(&transfer, &stream, &op.transfer_id));
    }
    if (error != XIAOTAI_OK) clear_prepared(error);
    else {
        op.http = RC_HTTP_SUBMIT;
        ++op.stats.submissions;
        phase(RC(UPLOAD));
    }
}

static void policy_event(xiaotai_err_t error, int http, int code)
{
    op.status.config_error = error;
    op.status.config_http_status = http;
    op.status.config_code = code == -1 || code == XIAOTAI_PLATFORM_NO_CODE ? 0 : code;
    if (error != XIAOTAI_OK) ++op.stats.policy_failures;
    publish();
    const xiaotai_args_t args = {.value = error};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_RECOGNITION_POLICY, &args, NULL, 0);
}

static void policy_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    if ((uint32_t)(uintptr_t)ctx != op.status.token) return;
    const bool recheck = op.http == RC_HTTP_RECHECK;
    const bool refresh = op.http == RC_HTTP_REFRESH;
    op.http = RC_HTTP_NONE;
    const bool own_reply = reply->generation == op.generation;
    const bool unbound = own_reply && reply->result == XIAOTAI_OK &&
                         reply->http_status == 410 && reply->code == XIAOTAI_PLATFORM_CODE_UNBOUND;
    if (unbound) {
        /* P2 already reported it. Its binding hook may have changed the
         * generation before this handler; do not accept any policy from it. */
        op.unbound = true;
        op.query_at_us = op.refresh_at_us = 0;
        op.status.http_status = reply->http_status;
        op.status.code = reply->code;
        policy_event(RC_INVALID_RESPONSE, reply->http_status, reply->code);
        if (recheck) clear_prepared(RC_INVALID_RESPONSE);
        else {
            op.background = RC_BACKGROUND_NONE;
            finish(RC(RECOVERY), RC_INVALID_RESPONSE);
        }
        return;
    }
    xiaotai_err_t condition = own_reply ? current() : XIAOTAI_ERR_INVALID_STATE;
    if (refresh && condition == XIAOTAI_OK && !background_ready()) condition = XIAOTAI_ERR_INVALID_STATE;
    if (condition != XIAOTAI_OK || cancelled(NULL)) {
        if (!own_reply || op.generation != xiaotai_identity_generation()) ++op.stats.stale;
        if (!cancelled(NULL)) cancel_operation(condition);
        else settle_cancel();
        return;
    }
    xiaotai_err_t error = normal_error(reply->result);
    int code = reply->code;
    if (error == XIAOTAI_OK) {
        xiaotai_recognition_policy_t policy;
        error = rc_policy_decode(reply->body, reply->size, reply->http_status, &code, &policy);
        if (error == XIAOTAI_OK && policy.revision < op.notice_revision) error = RC_INVALID_RESPONSE;
        if (error == XIAOTAI_OK) {
            op.policy = policy;
            op.status.config_revision = policy.revision;
            op.status.config_enabled = policy.effective_enabled;
        }
    }
    if (refresh) {
        if (reply->http_status >= 400 && reply->http_status < 500) op.refresh_at_us = 0;
        finish_refresh(error, reply->http_status, code, reply->http_status == 0 || reply->http_status >= 500);
        return;
    }
    policy_event(error, reply->http_status, code);
    if (recheck) {
        if (error == XIAOTAI_OK && op.policy.revision != op.initial_revision) error = RC_INVALID_RESPONSE;
        if (error == XIAOTAI_OK) error = pcm_check();
        if (error == XIAOTAI_OK) {
            /* Finished from the owner's perspective. Let the P2 reply
             * handler return before trying the next transaction. */
            error = xiaotai_run(send_upload, ctx);
            if (error == XIAOTAI_OK) {
                op.post_queued = true;
                phase(RC(UPLOAD));
            }
        }
        if (error != XIAOTAI_OK) clear_prepared(error);
    } else if (error != XIAOTAI_OK) {
        finish(RC(FAILED), error);
    } else {
        op.initial_revision = op.policy.revision;
        begin_recording();
    }
}

static xiaotai_err_t read_policy(rc_http_stage_t stage)
{
    xiaotai_err_t error = endpoint();
    if (error == XIAOTAI_OK) {
        const xiaotai_platform_request_t request = {
            .method = XIAOTAI_PLATFORM_GET, .service = RC_SERVICE, .path = XIAOTAI_RECOGNITION_CONFIG_PATH,
            .auth = XIAOTAI_PLATFORM_AUTH_SESSION, .timeout_ms = RC_POLICY_HTTP_MS,
            .flags = stage == RC_HTTP_REFRESH ? XIAOTAI_PLATFORM_BACKGROUND : 0U,
        };
        error = normal_error(xiaotai_platform_request(&request, policy_reply, (void *)(uintptr_t)op.status.token));
    }
    if (error == XIAOTAI_OK) {
        op.http = stage;
        ++op.stats.policy_reads;
        phase(op.status.phase);
    } else if (stage == RC_HTTP_REFRESH) {
        finish_refresh(error, 0, 0, error == XIAOTAI_ERR_NO_MEM || error == XIAOTAI_ERR_TIMEOUT);
    } else {
        policy_event(error, 0, 0);
        if (stage == RC_HTTP_RECHECK) clear_prepared(error);
        else finish(RC(FAILED), error);
    }
    return error;
}

static void retry_stop(void *unused)
{
    (void)unused;
    op.timer_id = 0;
    if ((!op.recording_id && !op.play_id) || op.releases >= RC_RELEASE_TRIES) return;
    ++op.releases;
    ++op.stats.release_retries;
    if (op.play_id) xiaotai_media_stop(op.play_id);
    else xiaotai_media_record_stop(op.recording_id);
    if ((op.recording_id || op.play_id) && op.releases < RC_RELEASE_TRIES)
        (void)xiaotai_timer_start(RC_RELEASE_MS, retry_stop, NULL, &op.timer_id);
    publish();
}

static void settle_cancel(void)
{
    if (borrowed()) {
        phase(op.prepare_timed_out ? RC(RECOVERY) : RC(CANCELLING));
        return;
    }
    if (op.background != RC_BACKGROUND_NONE) background_cancel_done();
    else if (op.download_active) download_end(RC(CANCELLED), op.status.error);
    else if (op.prepare_timed_out) parked(XIAOTAI_ERR_TIMEOUT, RC(RECOVERY));
    else clear_prepared(op.status.error);
}

static void cancel_operation(xiaotai_err_t error)
{
    if (!op.status.token) return;
    const bool first = !atomic_exchange_explicit(&op.cancelled, true, memory_order_acq_rel);
    if (first) {
        if (op.background == RC_BACKGROUND_REFRESH) op.status.config_error = error;
        else op.status.error = error;
    }
    drop_timer();
    if (op.transfer_id && first) xiaotai_platform_transfer_cancel(op.transfer_id);
    if (op.download_active) {
        op.status.playback_paused = true;
        op.play_requested = false;
    }
    if ((op.recording_id || op.play_id) && (first || error == XIAOTAI_OK)) {
        ++op.releases;
        if (op.play_id) xiaotai_media_stop(op.play_id);
        else xiaotai_media_record_stop(op.recording_id);
        if ((op.recording_id || op.play_id) && op.releases < RC_RELEASE_TRIES)
            (void)xiaotai_timer_start(RC_RELEASE_MS, retry_stop, NULL, &op.timer_id);
    }
    /* Stop work before notifying activity attachments that it has ended. */
    release_activity();
    if (op.status.token) settle_cancel();
}

static void stage_timeout(void *unused)
{
    (void)unused;
    op.timer_id = 0;
    if (op.journal_pending && !op.journal_prepared && !op.download_active) {
        ++op.stats.journal_timeouts;
        op.prepare_timed_out = true;
        buffers_free();
    }
    cancel_operation(XIAOTAI_ERR_TIMEOUT);
}

static void revoked(uint32_t id, xiaotai_revoke_t reason, void *unused)
{
    (void)reason;
    (void)unused;
    if (id != op.activity_id) return;
    op.activity_id = 0;
    ++op.stats.revoked;
    cancel_operation(XIAOTAI_ERR_INVALID_STATE);
}

static bool speech_ready(const char *item_id)
{
    if (!op.task || !item_id[0] || strcmp(op.task->info.request_id, op.record.request_id)) return false;
    return rc_speech_ready(rc_find_item(op.task, item_id));
}

static bool playback_available(void)
{
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    return !media.speaker_muted && !media.generation && !media.recording;
}

static void download_end(xiaotai_recognition_phase_t next, xiaotai_err_t error)
{
    op.download_active = false;
    op.audio_ready = NULL;
    op.audio_ctx = NULL;
    xiaotai_mem_free(op.play_data);
    op.play_data = NULL;
    op.play_sound = (xiaotai_sound_t){0};
    if (next != RC(RESULT)) {
        op.status.playback_paused = true;
        op.play_requested = false;
        if (!op.play_handoff) ++op.stats.download_failures;
    }
    op.play_handoff = false;
    op.played_since_us = 0;
    parked(error, next);
}

static void downloaded(const rc_audio_reply_t *reply, void *unused)
{
    (void)unused;
    op.http = RC_HTTP_NONE;
    op.transfer_id = 0;
    const bool same_binding = reply->generation == op.generation && op.generation == xiaotai_identity_generation();
    if (same_binding) {
        op.status.http_status = reply->http_status;
        op.status.code = reply->code < 0 ? 0 : reply->code;
        if (report_transfer_unbound(reply->http_status, reply->code)) {
            xiaotai_mem_free(reply->data);
            download_end(RC(RECOVERY), RC_INVALID_RESPONSE);
            return;
        }
    } else {
        ++op.stats.stale;
    }
    const xiaotai_err_t condition = same_binding ? current() : XIAOTAI_ERR_INVALID_STATE;
    if (cancelled(NULL) || condition != XIAOTAI_OK || !speech_ready(op.status.item_id)) {
        xiaotai_mem_free(reply->data);
        const xiaotai_err_t error = cancelled(NULL) ? op.status.error : condition == XIAOTAI_OK ?
                                    XIAOTAI_ERR_INVALID_STATE : condition;
        download_end(RC(CANCELLED), error);
        return;
    }
    if (reply->error != XIAOTAI_OK) {
        download_end(RC(FAILED), normal_error(reply->error));
        return;
    }
    if (!playback_available()) {
        xiaotai_mem_free(reply->data);
        download_end(RC(CANCELLED), XIAOTAI_ERR_INVALID_STATE);
        return;
    }
    drop_timer();
    rc_audio_ready_fn_t ready = op.audio_ready;
    void *const ctx = op.audio_ctx;
    op.audio_ready = NULL;
    op.audio_ctx = NULL;
    op.play_handoff = true;
    phase(RC(DOWNLOAD)); /* The bytes changed owners; the activity has not. */
    ready(op.status.token, reply->data, reply->size, ctx);
}

xiaotai_err_t rc_recognition_audio_start(const char *item_id, rc_audio_ready_fn_t ready, void *ctx)
{
    if (!rc_id_valid(item_id) || !ready) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner() || op.status.token || borrowed() || xiaotai_recognition_journal_busy() ||
        !background_ready() || op.generation != xiaotai_identity_generation() || !op.play_requested ||
        op.status.playback_paused || op.archive_pending || !speech_ready(item_id) ||
        !rc_record_matches(xiaotai_recognition_journal_mirror(), &op.record)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    if (!info.playback) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!playback_available()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t error = endpoint();
    if (error != XIAOTAI_OK) return error;
    error = deadline(RC_DOWNLOAD_MS);
    if (error != XIAOTAI_OK) return error;
    const uint32_t token = xiaotai_generation_next();
    const xiaotai_activity_t activity = {
        .name = XIAOTAI_RECOGNITION_ACTIVITY, .resources = XIAOTAI_RESOURCE_AUDIO_OUT, .revoked = revoked,
        .generation = token,
    };
    error = xiaotai_activity_begin(&activity, &op.activity_id);
    if (error != XIAOTAI_OK) {
        drop_timer();
        return normal_error(error);
    }
    atomic_store_explicit(&op.cancelled, false, memory_order_release);
    op.unbound = false;
    op.status.token = token;
    op.status.error = XIAOTAI_OK;
    op.status.http_status = op.status.code = 0;
    memcpy(op.status.item_id, item_id, strlen(item_id) + 1U);
    op.status.consumption = RC(UNTOUCHED);
    op.status.playback_ms = op.status.playback_total_ms = 0;
    op.releases = 0;
    op.download_active = true;
    op.audio_ready = ready;
    op.audio_ctx = ctx;
    error = normal_error(rc_audio_transfer(op.record.request_id, item_id, cancelled, downloaded, NULL, &op.transfer_id));
    if (error != XIAOTAI_OK) download_end(RC(FAILED), error);
    else {
        op.http = RC_HTTP_DOWNLOAD;
        ++op.stats.downloads;
        phase(RC(DOWNLOAD));
    }
    return error;
}

xiaotai_err_t rc_recognition_audio_finish(uint32_t token, xiaotai_err_t result)
{
    if (!xiaotai_on_owner() || !token || token != op.status.token || !op.download_active ||
        op.audio_ready || op.http != RC_HTTP_NONE) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t condition = current();
    if (condition != XIAOTAI_OK) {
        download_end(RC(CANCELLED), condition);
        return condition;
    }
    download_end(result == XIAOTAI_OK ? RC(RESULT) : RC(FAILED), result);
    return XIAOTAI_OK;
}

static void speech_event(bool active)
{
    const xiaotai_args_t args = {.value = active ? 1 : 0};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_RECOGNITION_SPEECH, &args,
                             op.status.item_id, strlen(op.status.item_id) + 1U);
}

static void playback_progress(void)
{
    if (!op.played_since_us) return;
    const int64_t elapsed_ms = (xiaotai_time_us() - op.played_since_us) / 1000;
    const uint32_t duration = op.status.playback_total_ms;
    op.status.playback_ms = elapsed_ms < 0 ? 0 : elapsed_ms > duration ? duration : (uint32_t)elapsed_ms;
}

static xiaotai_err_t submit_disposition_write(const xiaotai_recognition_journal_job_t *job)
{
    const xiaotai_err_t error = xiaotai_recognition_journal_submit(job);
    if (error == XIAOTAI_OK) {
        op.journal_pending = true;
        ++op.stats.journal_writes;
    } else {
        ++op.stats.journal_failures;
    }
    return error;
}

static void disposition_storage_returned(xiaotai_err_t error)
{
    op.journal_pending = false;
    if (error != XIAOTAI_OK) {
        ++op.stats.journal_failures;
        if (error == XIAOTAI_ERR_TIMEOUT) ++op.stats.journal_timeouts;
    }
}

static xiaotai_err_t write_play_item(xiaotai_recognition_journal_item_state_t old,
                                     xiaotai_recognition_journal_item_state_t next,
                                     xiaotai_recognition_journal_done_t done, void *ctx)
{
    xiaotai_recognition_journal_job_t transition = {.operation = RJ(ITEM_TRANSITION),
        .record = op.record, .expected_item = old, .next_item = next, .done = done, .ctx = ctx};
    memcpy(transition.item_id, op.status.item_id, sizeof(transition.item_id));
    return submit_disposition_write(&transition);
}

static void marked_played(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                          const xiaotai_recognition_journal_mirror_t *mirror, void *unused)
{
    (void)item;
    (void)unused;
    disposition_storage_returned(error);
    rc_result_update(mirror);
    if (error == XIAOTAI_OK) op.status.consumption = RC(PLAYED);
    const xiaotai_err_t condition = current();
    if (cancelled(NULL)) settle_cancel();
    else if (condition != XIAOTAI_OK) cancel_operation(condition);
    else if (error != XIAOTAI_OK) download_end(RC(FAILED), error);
    else {
        ++op.stats.playbacks_completed;
        download_end(RC(RESULT), XIAOTAI_OK);
        (void)sync_schedule(1U);
    }
}

static void playback_done(uint32_t id, xiaotai_err_t error, void *unused)
{
    (void)unused;
    if (!id || id != op.play_id) {
        ++op.stats.stale;
        publish();
        return;
    }
    op.play_id = 0; /* the output returned its descriptor and WAV borrow */
    speech_event(false);
    playback_progress();
    xiaotai_mem_free(op.play_data);
    op.play_data = NULL;
    const xiaotai_err_t condition = current();
    if (cancelled(NULL) || error != XIAOTAI_OK || condition != XIAOTAI_OK || !speech_ready(op.status.item_id)) {
        ++op.stats.playbacks_cut;
        if (cancelled(NULL)) settle_cancel();
        else cancel_operation(error != XIAOTAI_OK ? error : condition != XIAOTAI_OK ? condition : XIAOTAI_ERR_INVALID_STATE);
        return;
    }
    op.status.playback_ms = op.status.playback_total_ms;
    op.played_since_us = 0;
    if ((error = deadline(RC_JOURNAL_MS)) == XIAOTAI_OK) {
        error = write_play_item(RJ(PLAY_STARTED), RJ(PLAYED), marked_played, NULL);
    }
    if (error != XIAOTAI_OK) download_end(RC(FAILED), error);
    else phase(RC(PLAYBACK));
}

static void marked_started(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                           const xiaotai_recognition_journal_mirror_t *mirror, void *unused)
{
    (void)item;
    (void)unused;
    disposition_storage_returned(error);
    rc_result_update(mirror);
    if (error == XIAOTAI_OK) op.status.consumption = RC(PLAY_STARTED);
    if (cancelled(NULL)) {
        settle_cancel();
        return;
    }
    const xiaotai_err_t condition = current();
    if (error == XIAOTAI_OK && condition != XIAOTAI_OK) error = condition;
    if (error == XIAOTAI_OK && (!speech_ready(op.status.item_id) || !playback_available()))
        error = XIAOTAI_ERR_INVALID_STATE;
    if (error == XIAOTAI_OK) error = deadline(op.status.playback_total_ms + 2000U);
    if (error == XIAOTAI_OK) {
        _Static_assert(sizeof(xiaotai_media_play_t) <= 256U, "owner stack budget");
        const xiaotai_media_play_t request = {.clips = {{.sound = &op.play_sound}}, .clip_count = 1U,
            .priority = XIAOTAI_PLAY_URGENT, .done = playback_done};
        error = xiaotai_media_play_clips(&request, &op.play_id);
    }
    if (error != XIAOTAI_OK) download_end(RC(FAILED), error);
    else {
        ++op.stats.playbacks;
        op.played_since_us = xiaotai_time_us();
        phase(RC(PLAYBACK));
        speech_event(true);
    }
}

static void play_download(uint32_t token, uint8_t *data, size_t size, void *unused)
{
    (void)unused;
    op.play_data = data;
    xiaotai_err_t error = rc_playback_wav(data, size, &op.play_sound, &op.status.playback_total_ms);
    if (error == XIAOTAI_OK) error = deadline(RC_JOURNAL_MS);
    if (error == XIAOTAI_OK) error = write_play_item(RJ(UNTOUCHED), RJ(PLAY_STARTED), marked_started, NULL);
    if (error != XIAOTAI_OK) (void)rc_recognition_audio_finish(token, error);
    else phase(RC(DOWNLOAD));
}

static void playback_tick(void)
{
    if (op.download_active) {
        if (cancelled(NULL)) return;
        const xiaotai_err_t condition = current();
        if (condition != XIAOTAI_OK || !playback_available() || !speech_ready(op.status.item_id)) {
            cancel_operation(condition == XIAOTAI_OK ? XIAOTAI_ERR_INVALID_STATE : condition);
        } else if (op.play_id) {
            playback_progress();
            publish();
        }
        return;
    }
    if (!op.play_requested || op.status.playback_paused || op.archive_pending || !op.task || op.status.token ||
        borrowed() || xiaotai_recognition_journal_busy() || !background_ready()) return;
    const xiaotai_recognition_journal_mirror_t *mirror = xiaotai_recognition_journal_mirror();
    if (!rc_record_matches(mirror, &op.record)) return;
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    if (!info.playback || !playback_available()) return;

    char id[XIAOTAI_RECOGNITION_ID_MAX];
    const rc_order_t order = rc_choose_speech(op.task, mirror, id);
    if (order == RC_ORDER_WAIT) return;
    if (order == RC_ORDER_NONE) {
        op.play_requested = false;
        publish();
        return;
    }
    const xiaotai_err_t error = order == RC_ORDER_READY ? rc_recognition_audio_start(id, play_download, NULL) :
                                 XIAOTAI_ERR_INVALID_STATE;
    if (error != XIAOTAI_OK && op.play_requested) {
        memcpy(op.status.item_id, id, strlen(id) + 1U);
        op.status.consumption = (xiaotai_recognition_consumption_t)rc_item_consumption(mirror, id);
        download_end(RC(FAILED), error);
    }
}

static void platform_changed(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    if (op.status.token) {
        const xiaotai_err_t condition = current();
        if (condition != XIAOTAI_OK && !cancelled(NULL)) cancel_operation(condition);
    } else if (op.generation != xiaotai_identity_generation()) {
        hide_old_binding();
        op.generation = xiaotai_identity_generation();
        op.unbound = false;
        op.status.error = XIAOTAI_OK;
        phase(RC(IDLE));
        reflect_record(xiaotai_recognition_journal_mirror());
    }
    if (event && strcmp(event->name, XIAOTAI_EVENT_PLATFORM_ONLINE) == 0 &&
        atomic_load_explicit(&started, memory_order_acquire) && !xiaotai_recognition_journal_busy() &&
        xiaotai_recognition_journal_mirror()->state == RJ(UNREAD))
        (void)load_record();
    if (event) (void)sync_schedule(1U);
}

/* The worker owns the borrowed response bytes until transfer done. */
typedef struct {
    char *body;
    size_t capacity, received;
    int64_t declared;
    uint32_t reused_scope; /* Nonzero only after a verified scope advance. */
} query_body_t;

static xiaotai_err_t query_begin(void *ctx, const xiaotai_platform_response_head_t *head)
{
    query_body_t *body = ctx;
    if (cancelled(NULL)) return XIAOTAI_ERR_PLATFORM_CANCELLED;
    if (head->content_length > RC_TASK_BYTES) return XIAOTAI_ERR_INVALID_SIZE;
    body->declared = head->content_length;
    body->capacity = body->declared < 0 ? RC_QUERY_BLOCK : (size_t)body->declared + 1U;
    body->body = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, body->capacity);
    if (!body->body) return XIAOTAI_ERR_NO_MEM;
    body->body[0] = '\0';
    return XIAOTAI_OK;
}

static xiaotai_err_t query_write(void *ctx, const uint8_t *data, size_t size)
{
    query_body_t *body = ctx;
    if (cancelled(NULL)) return XIAOTAI_ERR_PLATFORM_CANCELLED;
    if (!body->body) return RC_INVALID_RESPONSE;
    if (!size) return XIAOTAI_OK;
    if (size > RC_TASK_BYTES - body->received) return XIAOTAI_ERR_INVALID_SIZE;
    if (memchr(data, '\0', size)) return RC_INVALID_RESPONSE;
    const size_t need = body->received + size + 1U;
    if (need > body->capacity) {
        if (body->declared >= 0) return RC_INVALID_RESPONSE;
        size_t capacity = body->capacity;
        while (capacity < need) {
            capacity *= 2U;
            if (capacity > RC_TASK_BYTES + 1U) capacity = RC_TASK_BYTES + 1U;
        }
        char *next = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, capacity);
        if (!next) return XIAOTAI_ERR_NO_MEM;
        memcpy(next, body->body, body->received);
        xiaotai_mem_free(body->body);
        body->body = next;
        body->capacity = capacity;
    }
    memcpy(body->body + body->received, data, size);
    body->received += size;
    body->body[body->received] = '\0';
    return XIAOTAI_OK;
}

static bool task_pending(void)
{
    return op.task && (op.task->info.state == RC(TASK_QUEUED) || op.task->info.state == RC(TASK_PROCESSING));
}

static xiaotai_err_t adopt_task(rc_task_t *task)
{
    op.status.rejected_without_task = false;
    if (op.task && task->info.revision <= op.task->info.revision) {
        ++op.stats.results_stale;
        rc_task_free(task);
        if (!op.query_at_us && task_pending()) op.query_at_us = xiaotai_time_us() + RC_POLL_US;
        publish();
        return XIAOTAI_OK;
    }
    const xiaotai_err_t error = rc_result_adopt(task, op.generation, &op.record, xiaotai_recognition_journal_mirror());
    if (error != XIAOTAI_OK) {
        rc_task_free(task);
        return error;
    }
    op.task = task;
    op.status.task_revision = task->info.revision;
    op.status.task_items = task->info.items;
    op.status.task_state = task->info.state;
    sync.query_failures = 0;
    op.query_at_us = task_pending() ? xiaotai_time_us() + RC_POLL_US : 0;
    if (config.autoplay && !op.status.playback_paused && !op.archive_pending) op.play_requested = true;
    ++op.stats.results_adopted;
    publish();
    const xiaotai_args_t args = {.value = task->info.items};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_RECOGNITION_RESULT, &args, NULL, 0);
    return XIAOTAI_OK;
}

static void retry_query(bool cancellation)
{
    const unsigned failures = ++sync.query_failures;
    if (failures >= 3U) return;
    op.query_at_us = xiaotai_time_us() + RC_POLL_US * (cancellation ? 1U : failures);
    ++op.stats.query_retries;
}

static void query_fail(xiaotai_err_t error, bool retry)
{
    ++op.stats.query_failures;
    if (retry) retry_query(false);
    op.background = RC_BACKGROUND_NONE;
    finish(RC(RECOVERY), error);
}

static void queried(void *ctx, const xiaotai_platform_transfer_result_t *result)
{
    query_body_t *body = ctx;
    op.http = RC_HTTP_NONE;
    op.transfer_id = 0;
    const bool same_binding = result->generation == op.generation &&
                              op.generation == xiaotai_identity_generation();
    /* The journal can advance before the binding generation changes. */
    const bool same_request = op.record.scope == body->reused_scope &&
                              !strcmp(op.record.request_id, op.old_request_id);
    const bool stale_scope = same_binding && body->reused_scope &&
        (!same_request || xiaotai_recognition_journal_busy() ||
         !rc_record_matches(xiaotai_recognition_journal_mirror(), &op.record));
    xiaotai_err_t error = normal_error(result->result);
    if (config.query_legacy_http && (error == XIAOTAI_FAIL || error == XIAOTAI_ERR_TIMEOUT)) {
        if (result->receive_fault == XIAOTAI_PLATFORM_RECEIVE_FAULT_HEAD) error = XIAOTAI_FAIL;
        else if (result->receive_fault == XIAOTAI_PLATFORM_RECEIVE_FAULT_BODY ||
                 result->receive_fault == XIAOTAI_PLATFORM_RECEIVE_FAULT_BODY_TRUNCATED) error = RC_INVALID_RESPONSE;
    }
    int code = -1;
    rc_task_t *task = NULL;
    if (error == XIAOTAI_OK && same_binding && !stale_scope) {
        if (!body->body || body->received != result->received ||
            (body->declared >= 0 && (uint64_t)body->declared != body->received)) error = RC_INVALID_RESPONSE;
        else error = rc_task_decode(body->body, body->received, result->http_status,
                                    op.record.device_id, op.record.request_id, &code, &task);
    }
    xiaotai_mem_free(body->body);
    xiaotai_mem_free(body);
    if (stale_scope) {
        ++op.stats.stale;
        if (cancelled(NULL)) settle_cancel();
        else {
            if (!op.task || same_request) op.query_at_us = 0;
            query_fail(XIAOTAI_ERR_INVALID_STATE, false);
        }
        return;
    }
    if (same_binding) {
        op.status.http_status = result->http_status;
        op.status.code = code == -1 ? 0 : code;
        if (report_transfer_unbound(result->http_status, code)) {
            rc_task_free(task);
            query_fail(error, false);
            return;
        }
    } else {
        ++op.stats.stale;
    }
    xiaotai_err_t condition = same_binding ? current() : XIAOTAI_ERR_INVALID_STATE;
    if (condition == XIAOTAI_OK && !background_ready()) condition = XIAOTAI_ERR_INVALID_STATE;
    if (cancelled(NULL) || condition != XIAOTAI_OK) {
        rc_task_free(task);
        if (!cancelled(NULL)) cancel_operation(condition);
        else settle_cancel();
        return;
    }
    if (error != XIAOTAI_OK) {
        op.status.rejected_without_task = !op.task && !task && op.query_conflict &&
            error == RC_INVALID_RESPONSE && result->http_status == 404 && code == 40400;
        rc_task_free(task);
        /* A result notice received in flight may have scheduled a poll. The
         * failed exchange replaces that plan with its bounded retry policy. */
        op.query_at_us = 0;
        const bool transient = result->http_status == 0 || result->http_status >= 500 ||
            (!config.query_legacy_http && result->http_status < 400 &&
             (error == XIAOTAI_ERR_NO_MEM || error == XIAOTAI_ERR_TIMEOUT));
        query_fail(error, transient && (!op.task || task_pending()));
        return;
    }
    sync.query_failures = 0;
    error = adopt_task(task);
    if (error != XIAOTAI_OK) {
        query_fail(error, !op.task || task_pending());
        return;
    }
    op.background = RC_BACKGROUND_NONE;
    finish(RC(RESULT), XIAOTAI_OK);
}

static xiaotai_err_t query_send(void)
{
    char path[sizeof(RC_SUBMIT_PATH) + XIAOTAI_RECOGNITION_ID_MAX];
    const int written = snprintf(path, sizeof(path), RC_SUBMIT_PATH "/%s", op.record.request_id);
    if (written < 0 || (size_t)written >= sizeof(path)) return XIAOTAI_ERR_INVALID_SIZE;
    query_body_t *body = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, sizeof(*body));
    if (!body) return XIAOTAI_ERR_NO_MEM;
    if (!strcmp(op.record.request_id, op.old_request_id)) body->reused_scope = op.record.scope;
    const xiaotai_platform_transfer_t transfer = {
        .request = {.method = XIAOTAI_PLATFORM_GET, .service = RC_SERVICE, .path = path,
                    .auth = XIAOTAI_PLATFORM_AUTH_SESSION,
                    .timeout_ms = config.query_legacy_http ? RC_QUERY_MS : RC_QUERY_HTTP_MS,
                    .flags = XIAOTAI_PLATFORM_BACKGROUND},
        .response_max = RC_TASK_BYTES,
        .exchange_timeout_ms = config.query_legacy_http ? RC_QUERY_HTTP_MS : 0,
    };
    const xiaotai_platform_stream_t stream = {
        .begin = query_begin, .write = query_write, .done = queried, .ctx = body,
    };
    const xiaotai_err_t error = normal_error(xiaotai_platform_transfer(&transfer, &stream, &op.transfer_id));
    if (error != XIAOTAI_OK) xiaotai_mem_free(body);
    else {
        op.http = RC_HTTP_QUERY;
        ++op.stats.queries;
        phase(RC(QUERY));
    }
    return error;
}

static void query_loaded(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                         const xiaotai_recognition_journal_mirror_t *mirror, void *unused)
{
    (void)item;
    (void)unused;
    op.query_loading = false;
    const bool explicit = op.query_explicit;
    const xiaotai_err_t condition = current();
    if (cancelled(NULL) || condition != XIAOTAI_OK || !background_ready()) {
        if (explicit) decision(RC(ACTION_QUERY), condition == XIAOTAI_OK ? XIAOTAI_ERR_INVALID_STATE : condition);
        if (!cancelled(NULL)) cancel_operation(condition == XIAOTAI_OK ? XIAOTAI_ERR_INVALID_STATE : condition);
        else settle_cancel();
        return;
    }
    char device[XIAOTAI_RECOGNITION_ID_MAX];
    if (error == XIAOTAI_OK && (mirror->state != RJ(PRESENT) ||
        xiaotai_identity_device_id(device, sizeof(device)) != XIAOTAI_OK ||
        strcmp(device, mirror->record.device_id) != 0 ||
        !durable_scope_allowed(&mirror->record))) error = XIAOTAI_ERR_INVALID_STATE;
    if (error == XIAOTAI_OK) {
        if (op.task && (strcmp(op.task->info.request_id, mirror->record.request_id) ||
                       !op.request_scope_known || op.request_scope != mirror->record.scope)) discard_task();
        op.record = mirror->record;
        op.query_conflict = mirror->version == 4U;
        show_request(&op.record, true);
        error = query_send();
    }
    if (error != XIAOTAI_OK) {
        if (!explicit && error == XIAOTAI_ERR_NOT_FOUND && !op.status.request_id[0]) {
            op.background = RC_BACKGROUND_NONE;
            finish(RC(IDLE), XIAOTAI_OK);
        } else {
            /* A missed-notification lookup is independent of the cached
             * task's state; a local admission failure did not check the server. */
            query_fail(error, !explicit && (error == XIAOTAI_ERR_NO_MEM || error == XIAOTAI_ERR_TIMEOUT));
        }
    }
    if (explicit) decision(RC(ACTION_QUERY), error);
}

static xiaotai_err_t owner_query(bool explicit)
{
    if (op.status.token || borrowed() || xiaotai_recognition_journal_busy() || !background_ready() ||
        (op.archive_pending && !op.task)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t available = endpoint();
    if (available != XIAOTAI_OK) return available;
    const xiaotai_recognition_journal_mirror_t *mirror = xiaotai_recognition_journal_mirror();
    if (explicit && mirror->state == RJ(ERROR)) {
        const xiaotai_err_t error = mirror->error;
        (void)load_record();
        return error;
    }
    xiaotai_err_t error = deadline(RC_QUERY_MS);
    if (error != XIAOTAI_OK) return error;
    const xiaotai_recognition_journal_job_t load = {.operation = RJ(LOAD), .done = query_loaded};
    if (mirror->state == RJ(PRESENT)) op.record = mirror->record;
    error = xiaotai_recognition_journal_submit(&load);
    if (error != XIAOTAI_OK) {
        drop_timer();
        return error;
    }
    op.generation = xiaotai_identity_generation();
    op.status.token = xiaotai_generation_next();
    op.status.error = XIAOTAI_OK;
    op.status.http_status = op.status.code = 0;
    op.status.rejected_without_task = op.unbound = false;
    op.query_loading = true;
    op.query_explicit = explicit;
    op.query_at_us = 0;
    op.background = RC_BACKGROUND_QUERY;
    atomic_store_explicit(&op.cancelled, false, memory_order_release);
    phase(RC(QUERY));
    return XIAOTAI_OK;
}

static void finish_refresh(xiaotai_err_t error, int http, int code, bool retry)
{
    if (error == XIAOTAI_OK) sync.refresh_failures = 0;
    else if (retry && ++sync.refresh_failures < 3U)
        op.refresh_at_us = xiaotai_time_us() + RC_POLL_US * sync.refresh_failures;
    op.background = RC_BACKGROUND_NONE;
    const xiaotai_recognition_phase_t next = op.task ? RC(RESULT) : op.before_refresh;
    finish(next, op.status.error);
    policy_event(error, http, code);
}

static void background_cancel_done(void)
{
    const rc_background_t kind = op.background;
    const xiaotai_err_t error = kind == RC_BACKGROUND_QUERY ? op.status.error : op.status.config_error;
    if (op.generation != xiaotai_identity_generation()) {
        op.background = RC_BACKGROUND_NONE;
        op.unbound = false;
        parked(XIAOTAI_OK, RC(IDLE));
        return;
    }
    if (kind == RC_BACKGROUND_QUERY) {
        if (error == XIAOTAI_OK) op.query_at_us = 0;
        else if (error == XIAOTAI_ERR_TIMEOUT) retry_query(true);
        else {
            op.query_at_us = xiaotai_time_us() + RC_POLL_US;
            ++op.stats.query_retries;
        }
        query_fail(error, false);
    } else {
        if (error == XIAOTAI_OK) op.refresh_at_us = 0;
        else if (error != XIAOTAI_ERR_TIMEOUT || ++sync.refresh_failures < 3U)
            op.refresh_at_us = xiaotai_time_us() + RC_POLL_US;
        finish_refresh(error, 0, 0, false);
    }
}

static void owner_refresh(void)
{
    if (op.status.token || borrowed() || !background_ready()) {
        policy_event(XIAOTAI_ERR_INVALID_STATE, 0, 0);
        return;
    }
    op.refresh_at_us = 0;
    op.before_refresh = op.status.phase;
    op.generation = xiaotai_identity_generation();
    op.status.token = xiaotai_generation_next();
    op.unbound = false;
    op.background = RC_BACKGROUND_REFRESH;
    atomic_store_explicit(&op.cancelled, false, memory_order_release);
    const xiaotai_err_t error = deadline(RC_POLICY_MS);
    if (error != XIAOTAI_OK) finish_refresh(error, 0, 0, true);
    else {
        phase(RC(REFRESH));
        (void)read_policy(RC_HTTP_REFRESH);
    }
}

static void sync_tick(void *unused)
{
    (void)unused;
    sync.timer_id = 0;
    /* Rearm in the slot this callback just released. A new request's
     * deadline must not consume it when the owner timer table is full. */
    (void)sync_schedule(RC_SYNC_MS);
    if (op.task && !xiaotai_recognition_journal_busy()) rc_result_update(xiaotai_recognition_journal_mirror());
    if (!op.status.token || op.background != RC_BACKGROUND_NONE) platform_changed(NULL, NULL);
    if (!op.status.token && !borrowed() && !xiaotai_recognition_journal_busy() && background_ready()) {
        xiaotai_platform_status_t platform;
        xiaotai_platform_status(&platform);
        const int64_t now = xiaotai_time_us();
        if (sync.generation != platform.generation || sync.revision != platform.notify_revision) {
            sync.generation = platform.generation;
            sync.revision = platform.notify_revision;
            op.query_at_us = op.refresh_at_us = now;
            sync.query_failures = sync.refresh_failures = 0;
        }
        if (op.query_at_us && op.query_at_us <= now) {
            op.query_at_us = 0;
            const xiaotai_err_t error = owner_query(false);
            if (error == XIAOTAI_ERR_NO_MEM || error == XIAOTAI_ERR_TIMEOUT) retry_query(false);
            else if (error != XIAOTAI_OK) query_fail(error, false);
        } else if (op.refresh_at_us && op.refresh_at_us <= now) {
            owner_refresh();
        }
    }
    playback_tick();
}

static xiaotai_err_t sync_schedule(uint32_t delay_ms)
{
    if (!atomic_load_explicit(&started, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    if (sync.timer_id) xiaotai_timer_cancel(sync.timer_id);
    sync.timer_id = 0;
    return xiaotai_timer_start(delay_ms, sync_tick, NULL, &sync.timer_id);
}

static void activity_changed(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    if (op.background != RC_BACKGROUND_NONE && !background_ready() && !cancelled(NULL))
        cancel_operation(XIAOTAI_ERR_INVALID_STATE);
    (void)sync_schedule(1U);
}

static void notification(const xiaotai_platform_signal_t *signal, void *unused)
{
    (void)unused;
    ++op.stats.notifications;
    if (!online() || signal->generation != xiaotai_identity_generation() ||
        (signal->generation != op.generation && borrowed())) goto ignored;
    platform_changed(NULL, NULL);
    char device[XIAOTAI_RECOGNITION_ID_MAX];
    if (xiaotai_identity_device_id(device, sizeof(device)) != XIAOTAI_OK) goto ignored;
    const xiaotai_recognition_journal_mirror_t *mirror = xiaotai_recognition_journal_mirror();
    /* A query's fresh LOAD temporarily hides the last verified mirror. */
    const xiaotai_recognition_journal_record_t *record = mirror->state == RJ(PRESENT) ? &mirror->record :
        op.query_loading && mirror->state == RJ(READING) ? &op.record : NULL;
    const bool matches = record && !strcmp(device, record->device_id) &&
                         strcmp(record->request_id, op.old_request_id) != 0;
    rc_task_t *task = NULL;
    uint64_t revision = 0;
    const xiaotai_err_t error = rc_notification_decode(signal->json, signal->size, device,
        matches ? record->request_id : NULL, &revision, &task);
    if (error != XIAOTAI_OK) {
        if (error == XIAOTAI_ERR_NO_MEM && matches) retry_query(false);
        goto ignored;
    }
    if (revision) {
        if (revision <= op.notice_revision || revision <= op.status.config_revision) goto ignored;
        op.notice_revision = revision;
        op.refresh_at_us = xiaotai_time_us() + 1000;
        sync.refresh_failures = 0;
        const xiaotai_args_t args = {.value = XIAOTAI_ERR_INVALID_STATE};
        (void)xiaotai_event_emit(XIAOTAI_EVENT_RECOGNITION_POLICY, &args, NULL, 0);
        if (op.status.phase == RC(CONFIG) || op.status.phase == RC(RECORDING))
            cancel_operation(XIAOTAI_ERR_INVALID_STATE);
    } else if (task) {
        if (op.task && strcmp(op.task->info.request_id, task->info.request_id) != 0) discard_task();
        op.record = *record;
        show_request(&op.record, mirror->state == RJ(PRESENT));
        if (adopt_task(task) != XIAOTAI_OK) {
            retry_query(false);
            goto ignored;
        }
        if (!borrowed() && !op.status.token) phase(RC(RESULT));
    }
    publish();
    (void)sync_schedule(1U);
    return;

ignored:
    ++op.stats.notifications_ignored;
    publish();
}

static void legacy_notification(const xiaotai_platform_signal_t *signal, void *context)
{
    xiaotai_json_view_t envelope, channel;
    /* The platform also reports a non-string channel as empty. Only dispatch
     * an omitted channel here, and leave the recognition decoder unchanged. */
    if (xiaotai_json_view_parse(signal->json, signal->size, NULL, &envelope) == XIAOTAI_OK &&
        envelope.type == XIAOTAI_JSON_OBJECT &&
        !xiaotai_json_view_get(&envelope, "channel", &channel))
        notification(signal, context);
}

static xiaotai_err_t owner_record(uint32_t seconds)
{
    if (op.status.token || borrowed() || op.status.task_revision ||
        (op.archive_pending && op.generation == xiaotai_identity_generation())) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_recognition_journal_mirror_t *mirror = xiaotai_recognition_journal_mirror();
    if (xiaotai_recognition_journal_busy()) return XIAOTAI_ERR_INVALID_STATE;
    if (mirror->state != RJ(ABSENT)) {
        reflect_record(mirror);
        if (mirror->state == RJ(ERROR)) {
            const xiaotai_err_t error = mirror->error;
            (void)load_record();
            return error;
        }
        if (mirror->state == RJ(UNREAD)) (void)load_record();
        return XIAOTAI_ERR_INVALID_STATE;
    }
    if (xiaotai_activity_active()) {
        ++op.stats.activity_busy;
        return XIAOTAI_ERR_INVALID_STATE;
    }
    if (!online()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t error = endpoint();
    if (error != XIAOTAI_OK) return error;
    xiaotai_media_info_t info;
    xiaotai_media_status_t media;
    xiaotai_media_info(&info);
    xiaotai_media_status(&media);
    if (!info.capture_pcm) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (media.mic_muted || !media.ready || !info.capture_rate_hz) return XIAOTAI_ERR_INVALID_STATE;
    if (info.capture_rate_hz > UINT32_MAX / seconds ||
        info.capture_rate_hz > SIZE_MAX / sizeof(int16_t) / seconds) return XIAOTAI_ERR_INVALID_SIZE;
    error = xiaotai_identity_device_id(op.record.device_id, sizeof(op.record.device_id));
    if (error != XIAOTAI_OK) return XIAOTAI_ERR_INVALID_STATE;

    const uint32_t token = xiaotai_generation_next();
    const xiaotai_activity_t activity = {
        .name = XIAOTAI_RECOGNITION_ACTIVITY, .resources = XIAOTAI_RESOURCE_AUDIO_IN, .revoked = revoked,
        .generation = token,
    };
    error = deadline(RC_POLICY_MS);
    if (error != XIAOTAI_OK) return error;
    error = xiaotai_activity_begin(&activity, &op.activity_id);
    if (error != XIAOTAI_OK) {
        drop_timer();
        ++op.stats.activity_busy;
        return normal_error(error);
    }
    op.generation = xiaotai_identity_generation();
    op.seconds = seconds;
    op.capture_rate = info.capture_rate_hz;
    op.status.capture_rate_hz = info.capture_rate_hz;
    op.record.seconds = seconds;
    op.record.request_id[0] = '\0';
    op.journal_prepared = op.prepare_timed_out = op.unbound = false;
    op.releases = 0;
    atomic_store_explicit(&op.cancelled, false, memory_order_release);
    op.status.token = token;
    op.status.samples = 0;
    op.status.target_samples = seconds * info.capture_rate_hz;
    op.status.error = XIAOTAI_OK;
    op.status.http_status = op.status.code = 0;
    clear_request();
    op.status.possibly_sent = op.status.rejected_without_task = false;
    phase(RC(CONFIG));
    return read_policy(RC_HTTP_POLICY);
}

static void run_action(void *ctx)
{
    action_t *request = ctx;
    const xiaotai_recognition_action_t action = request->action;
    const uint32_t argument = request->argument;
    const uint32_t generation = request->generation;
    xiaotai_mem_free(request);
    xiaotai_err_t error = XIAOTAI_ERR_INVALID_STATE;
    if (generation != xiaotai_identity_generation()) {
        ++op.stats.stale;
        if (action == RC(ACTION_NONE)) {
            policy_event(error, 0, 0);
            return;
        }
    } else if (action == RC(ACTION_RECORD)) {
        platform_changed(NULL, NULL);
        error = owner_record(argument);
    } else if (action == RC(ACTION_QUERY)) {
        platform_changed(NULL, NULL);
        error = owner_query(true);
        if (error == XIAOTAI_OK) return; /* The journal callback writes the one receipt. */
    } else if (action == RC(ACTION_NONE)) {
        platform_changed(NULL, NULL);
        owner_refresh();
        return;
    } else if (argument && argument == op.status.token) {
        ++op.stats.cancels;
        op.releases = 0;
        cancel_operation(XIAOTAI_OK);
        error = XIAOTAI_OK;
    }
    decision(action, error);
}

static xiaotai_err_t queue_action(xiaotai_recognition_action_t action, uint32_t value)
{
    if (!atomic_load_explicit(&started, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    action_t *request = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*request));
    if (!request) return XIAOTAI_ERR_NO_MEM;
    *request = (action_t){action, value, xiaotai_identity_generation()};
    const xiaotai_err_t error = xiaotai_run(run_action, request);
    if (error != XIAOTAI_OK) xiaotai_mem_free(request);
    return error;
}

xiaotai_err_t xiaotai_recognition_record(uint32_t seconds)
{
    if (!seconds || seconds > XIAOTAI_RECOGNITION_SECONDS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    return queue_action(RC(ACTION_RECORD), seconds);
}

xiaotai_err_t xiaotai_recognition_cancel(uint32_t token)
{
    if (!token) token = atomic_load_explicit(&active_token, memory_order_acquire);
    return queue_action(RC(ACTION_CANCEL), token);
}

xiaotai_err_t xiaotai_recognition_query(void)
{
    return queue_action(RC(ACTION_QUERY), 0);
}

xiaotai_err_t xiaotai_recognition_policy_refresh(void)
{
    return queue_action(RC(ACTION_NONE), 0);
}

typedef struct {
    xiaotai_recognition_action_t action;
    uint32_t generation;
    uint64_t revision;
    char request_id[XIAOTAI_RECOGNITION_ID_MAX], item_id[XIAOTAI_RECOGNITION_ID_MAX];
} disposition_t;

static void archived(void)
{
    discard_task();
    op.record = (xiaotai_recognition_journal_record_t){0};
    op.journal_prepared = op.conflict = false;
    op.query_at_us = 0;
    clear_request();
    op.status.samples = op.status.target_samples = 0;
    op.status.error = XIAOTAI_OK;
    op.status.http_status = op.status.code = 0;
    op.status.possibly_sent = op.status.rejected_without_task = false;
    phase(RC(IDLE));
}

static void finish_disposition(disposition_t *request, xiaotai_err_t error)
{
    decision(request->action, error);
    xiaotai_mem_free(request);
}

static void resume_speech(void)
{
    op.status.playback_paused = false;
    op.play_requested = true;
    op.status.error = XIAOTAI_OK;
    op.status.http_status = op.status.code = 0;
    phase(RC(RESULT));
}

static void disposition_done(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                              const xiaotai_recognition_journal_mirror_t *mirror, void *ctx)
{
    (void)item;
    disposition_t *request = ctx;
    disposition_storage_returned(error);
    if (request->generation != xiaotai_identity_generation()) {
        platform_changed(NULL, NULL);
        finish_disposition(request, XIAOTAI_ERR_INVALID_STATE);
        return;
    }
    if (request->action == RC(ACTION_ARCHIVE)) {
        if (error == XIAOTAI_OK) archived();
        else {
            rc_result_update(mirror);
            op.status.error = error;
            phase(RC(RECOVERY));
        }
    } else {
        rc_result_update(mirror);
        if (error == XIAOTAI_OK) {
            op.status.consumption = RC(DISMISSED);
            if (config.resume_after_dismiss) resume_speech();
            else {
                op.status.error = XIAOTAI_OK;
                op.status.http_status = op.status.code = 0;
                phase(RC(RESULT));
            }
        } else {
            phase(op.status.phase);
        }
    }
    finish_disposition(request, error);
    (void)sync_schedule(1U);
}

static xiaotai_err_t submit_archive(disposition_t *request, bool *pending)
{
    const xiaotai_recognition_journal_job_t clear = {.operation = op.task ? RJ(CLEAR) : RJ(CLEAR_CONFLICT),
        .record = op.record, .done = disposition_done, .ctx = request};
    op.archive_pending = true;
    op.play_requested = false;
    op.status.playback_paused = true;
    const xiaotai_err_t error = submit_disposition_write(&clear);
    *pending = error == XIAOTAI_OK;
    if (error != XIAOTAI_OK) op.status.error = error;
    phase(error == XIAOTAI_OK ? op.status.phase : RC(RECOVERY));
    return error;
}

static void archive_rechecked(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                               const xiaotai_recognition_journal_mirror_t *mirror, void *ctx)
{
    (void)item;
    disposition_t *request = ctx;
    op.journal_pending = false;
    if (request->generation != xiaotai_identity_generation()) {
        platform_changed(NULL, NULL);
        finish_disposition(request, XIAOTAI_ERR_INVALID_STATE);
        return;
    }
    if (!background_ready()) {
        phase(RC(RECOVERY));
        finish_disposition(request, XIAOTAI_ERR_INVALID_STATE);
        return;
    }
    if (error == XIAOTAI_ERR_NOT_FOUND && mirror->state == RJ(ABSENT)) {
        archived();
        finish_disposition(request, XIAOTAI_OK);
        return;
    }
    if (error == XIAOTAI_OK && !rc_record_matches(mirror, &op.record)) error = XIAOTAI_ERR_INVALID_STATE;
    if (error == XIAOTAI_OK) error = rc_allow_archive(op.task, mirror, request->revision, op.status.rejected_without_task);
    bool pending = false;
    if (error == XIAOTAI_OK) error = submit_archive(request, &pending);
    if (!pending) {
        rc_result_update(mirror);
        op.status.error = error;
        phase(RC(RECOVERY));
        finish_disposition(request, error);
    }
}

static xiaotai_err_t owner_disposition(disposition_t *request, bool *pending)
{
    if (request->generation != xiaotai_identity_generation() || request->generation != op.generation ||
        !background_ready() || op.status.token || borrowed() || xiaotai_recognition_journal_busy() ||
        (op.archive_pending && request->action != RC(ACTION_ARCHIVE)) ||
        strcmp(op.record.request_id, request->request_id)) return XIAOTAI_ERR_INVALID_STATE;
    char device[XIAOTAI_RECOGNITION_ID_MAX];
    if (xiaotai_identity_device_id(device, sizeof(device)) != XIAOTAI_OK || strcmp(device, op.record.device_id))
        return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_recognition_journal_mirror_t *mirror = xiaotai_recognition_journal_mirror();
    if (op.archive_pending) {
        const xiaotai_recognition_journal_job_t load = {.operation = RJ(LOAD), .done = archive_rechecked, .ctx = request};
        if ((!op.task && request->revision) || (op.task && request->revision != op.task->info.revision))
            return XIAOTAI_ERR_INVALID_STATE;
        const xiaotai_err_t error = xiaotai_recognition_journal_submit(&load);
        op.journal_pending = *pending = error == XIAOTAI_OK;
        phase(op.status.phase);
        return error;
    }
    if (!rc_record_matches(mirror, &op.record)) return XIAOTAI_ERR_INVALID_STATE;
    if (request->action == RC(ACTION_ARCHIVE)) {
        const xiaotai_err_t error = rc_allow_archive(op.task, mirror, request->revision, op.status.rejected_without_task);
        return error == XIAOTAI_OK ? submit_archive(request, pending) : error;
    }
    if (request->action == RC(ACTION_RESUME)) {
        xiaotai_media_info_t info;
        xiaotai_media_info(&info);
        if (!info.playback) return XIAOTAI_ERR_NOT_SUPPORTED;
        const xiaotai_err_t error = rc_allow_resume(op.task, mirror);
        if (error != XIAOTAI_OK) return error;
        resume_speech();
        (void)sync_schedule(1U);
        return XIAOTAI_OK;
    }
    if (!op.task) return XIAOTAI_ERR_INVALID_STATE;
    if (!rc_find_item(op.task, request->item_id)) return XIAOTAI_ERR_NOT_FOUND;
    if (strcmp(op.status.item_id, request->item_id)) op.status.playback_ms = op.status.playback_total_ms = 0;
    memcpy(op.status.item_id, request->item_id, sizeof(op.status.item_id));
    const xiaotai_recognition_journal_item_state_t consumed = rc_item_consumption(mirror, request->item_id);
    op.status.consumption = (xiaotai_recognition_consumption_t)consumed;
    if (consumed == RJ(PLAYED) || consumed == RJ(DISMISSED)) {
        if (config.resume_after_dismiss) {
            resume_speech();
            (void)sync_schedule(1U);
        }
        return XIAOTAI_OK;
    }
    const xiaotai_err_t error = write_play_item(consumed, RJ(DISMISSED), disposition_done, request);
    if (error == XIAOTAI_OK) *pending = true;
    /* A configured dismiss cannot lift the pause until the write succeeds. */
    phase(op.status.phase);
    return error;
}

static void run_disposition(void *arg)
{
    disposition_t *request = arg;
    bool pending = false;
    const xiaotai_err_t error = owner_disposition(request, &pending);
    if (!pending) finish_disposition(request, error);
}

static xiaotai_err_t queue_disposition(xiaotai_recognition_action_t action, const char *request_id,
                                       const char *item_id, uint64_t revision)
{
    if (!rc_id_valid(request_id) || (action == RC(ACTION_DISMISS) && !rc_id_valid(item_id)) ||
        revision > XIAOTAI_RECOGNITION_REVISION_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load_explicit(&started, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    disposition_t *request = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1U, sizeof(*request));
    if (!request) return XIAOTAI_ERR_NO_MEM;
    request->action = action;
    request->generation = xiaotai_identity_generation();
    request->revision = revision;
    memcpy(request->request_id, request_id, strlen(request_id) + 1U);
    if (item_id) memcpy(request->item_id, item_id, strlen(item_id) + 1U);
    const xiaotai_err_t error = xiaotai_run(run_disposition, request);
    if (error != XIAOTAI_OK) xiaotai_mem_free(request);
    return error;
}

xiaotai_err_t xiaotai_recognition_resume(const char *request_id)
{
    return queue_disposition(RC(ACTION_RESUME), request_id, NULL, 0);
}

xiaotai_err_t xiaotai_recognition_dismiss(const char *request_id, const char *item_id)
{
    return queue_disposition(RC(ACTION_DISMISS), request_id, item_id, 0);
}

xiaotai_err_t xiaotai_recognition_archive(const char *request_id, uint64_t revision)
{
    return queue_disposition(RC(ACTION_ARCHIVE), request_id, NULL, revision);
}

typedef struct {
    const char *name;
    xiaotai_recognition_action_t action;
} recognition_intent_t;

static void intent_action(const xiaotai_args_t *args, void *ctx)
{
    const recognition_intent_t *intent = ctx;
    if (!intent) return;
    const xiaotai_recognition_action_t action = intent->action;
    xiaotai_err_t error = XIAOTAI_ERR_INVALID_ARG;
    if (args->text[0]) goto refused;
    switch (action) {
    case RC(ACTION_RECORD):
        error = xiaotai_recognition_record((uint32_t)args->value);
        break;
    case RC(ACTION_CANCEL):
        if (!args->value) error = xiaotai_recognition_cancel(0);
        break;
    case RC(ACTION_QUERY):
        if (!args->value) error = xiaotai_recognition_query();
        break;
    case RC(ACTION_RESUME):
    case RC(ACTION_DISMISS): {
        if (args->value < 0 || (action == RC(ACTION_RESUME) && args->value)) break;
        error = XIAOTAI_ERR_INVALID_STATE;
        if (!op.task || op.generation != xiaotai_identity_generation()) break;
        if (action == RC(ACTION_RESUME)) {
            error = xiaotai_recognition_resume(op.task->info.request_id);
        } else if ((uint32_t)args->value >= op.task->info.items) {
            error = XIAOTAI_ERR_INVALID_ARG;
        } else {
            char item[XIAOTAI_RECOGNITION_ID_MAX];
            error = rc_task_string(op.task, op.task->items[args->value].id, item, sizeof(item), NULL);
            if (error == XIAOTAI_OK) error = xiaotai_recognition_dismiss(op.task->info.request_id, item);
        }
        break;
    }
    default:
        break;
    }
refused:
    if (error != XIAOTAI_OK) decision(action, error);
}

static xiaotai_err_t register_intents(void)
{
    static recognition_intent_t intents[] = {
        {XIAOTAI_INTENT_RECOGNITION_RECORD, RC(ACTION_RECORD)}, {XIAOTAI_INTENT_RECOGNITION_CANCEL, RC(ACTION_CANCEL)},
        {XIAOTAI_INTENT_RECOGNITION_QUERY, RC(ACTION_QUERY)}, {XIAOTAI_INTENT_RECOGNITION_RESUME, RC(ACTION_RESUME)},
        {XIAOTAI_INTENT_RECOGNITION_DISMISS, RC(ACTION_DISMISS)},
    };
    for (size_t i = 0; i < sizeof(intents) / sizeof(*intents); ++i) {
        const xiaotai_err_t error = xiaotai_intent_handle(intents[i].name, intent_action, &intents[i]);
        if (error != XIAOTAI_OK) return error;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_recognition_status(xiaotai_recognition_status_t *status)
{
    if (!status) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_flag_test_and_set_explicit(&snapshot_lock, memory_order_acquire)) return XIAOTAI_ERR_TIMEOUT;
    *status = snapshot.status;
    const uint32_t generation = snapshot.generation;
    const uint32_t recording_id = snapshot.recording_id;
    atomic_flag_clear_explicit(&snapshot_lock, memory_order_release);
    if (generation != xiaotai_identity_generation()) {
        status->request_id[0] = '\0';
        status->item_id[0] = '\0';
        status->consumption = RC(UNTOUCHED);
        status->playback_ms = status->playback_total_ms = 0;
        status->config_enabled = false;
        status->config_revision = 0;
        status->task_revision = 0;
        status->task_items = status->task_state = 0;
        status->rejected_without_task = false;
    }
    if (recording_id && status->phase == RC(RECORDING) && generation == xiaotai_identity_generation()) {
        xiaotai_media_status_t media;
        xiaotai_media_status(&media);
        if (media.recording == recording_id && media.recorded_samples <= status->target_samples &&
            status->token == atomic_load_explicit(&active_token, memory_order_acquire))
            status->samples = media.recorded_samples;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_recognition_policy(xiaotai_recognition_policy_t *policy)
{
    if (!policy) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_flag_test_and_set_explicit(&snapshot_lock, memory_order_acquire)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_err_t error = XIAOTAI_ERR_NOT_FOUND;
    if (snapshot.generation == xiaotai_identity_generation() && snapshot.policy.valid) {
        *policy = snapshot.policy;
        error = XIAOTAI_OK;
    }
    atomic_flag_clear_explicit(&snapshot_lock, memory_order_release);
    return error;
}

void xiaotai_recognition_stats(xiaotai_recognition_stats_t *stats)
{
    if (!stats) return;
    while (atomic_flag_test_and_set_explicit(&snapshot_lock, memory_order_acquire))
        xiaotai_delay_ms(1U);
    *stats = snapshot.stats;
    atomic_flag_clear_explicit(&snapshot_lock, memory_order_release);
}

xiaotai_err_t xiaotai_recognition_configure(const xiaotai_recognition_config_t *requested)
{
    if (initialized) return XIAOTAI_ERR_INVALID_STATE;
    config = requested ? *requested : (xiaotai_recognition_config_t){0};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_module_recognition_init(void)
{
    const xiaotai_platform_requirement_t requirement = {
        .response_bytes = RC_POLICY_BYTES,
        .concurrent_requests = config.legacy_four_http_slots ? 4 : 2,
    };
    xiaotai_err_t error = xiaotai_platform_require(&requirement);
    if (error == XIAOTAI_OK)
        error = xiaotai_event_subscribe(XIAOTAI_EVENT_PLATFORM_OFFLINE, platform_changed, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_event_subscribe(XIAOTAI_EVENT_PLATFORM_ONLINE, platform_changed, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_event_subscribe(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, platform_changed, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_event_subscribe(XIAOTAI_EVENT_ACTIVITY, activity_changed, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_event_subscribe(XIAOTAI_EVENT_QUIESCE, activity_changed, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_platform_signal_subscribe("recognition_result", RC_SERVICE, notification, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_platform_signal_subscribe("recognition_config_changed", RC_SERVICE, notification, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_platform_signal_subscribe("recognition_result", "", legacy_notification, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_platform_signal_subscribe("recognition_config_changed", "", legacy_notification, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_binding_prepare_register(RC_SERVICE, binding_prepare, NULL);
    if (error == XIAOTAI_OK)
        error = xiaotai_quiesce_guard_register(RC_SERVICE, environment_guard, NULL);
    if (error == XIAOTAI_OK) error = register_intents();
    if (error == XIAOTAI_OK) error = rc_at_init();
    if (error == XIAOTAI_OK) initialized = true;
    return error;
}

void xiaotai_module_recognition_start(void)
{
    op.generation = xiaotai_identity_generation();
    sync.generation = op.generation;
    xiaotai_err_t error = xiaotai_recognition_journal_start();
    if (error == XIAOTAI_OK) {
        atomic_store_explicit(&started, true, memory_order_release);
        error = sync_schedule(RC_SYNC_MS);
        if (error == XIAOTAI_OK) error = load_record();
    }
    if (error != XIAOTAI_OK) {
        atomic_store_explicit(&started, false, memory_order_release);
        xiaotai_timer_cancel(sync.timer_id);
        sync.timer_id = 0;
        op.status.error = error;
        phase(RC(RECOVERY));
    }
}

void xiaotai_recognition_reset(void)
{
#ifdef XIAOTAI_RECOGNITION_JOURNAL_TEST
    xiaotai_recognition_journal_host_stop();
#endif
    /* Host callers stop the owner and return all borrows before reset. */
    atomic_store(&started, false);
    atomic_store(&active_token, 0);
    initialized = false;
    config = (xiaotai_recognition_config_t){0};
    rc_result_clear();
    op = (rc_operation_t){0};
    memset(&sync, 0, sizeof(sync));
    memset(&snapshot, 0, sizeof(snapshot));
    atomic_flag_clear(&snapshot_lock);
}
