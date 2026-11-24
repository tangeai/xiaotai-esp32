// SPDX-License-Identifier: MIT
/* Binding module core: phases and status, the attempt's lifetime, the guards
 * and preparation steps of other modules, the automatic start on the platform
 * core's report, the cancel conditions, the framework quiesce guard, the
 * storage hand-off of the identity component's jobs, intents and lifecycle.
 * The report, the window and the grant are in the sibling sources.
 *
 * Every phase change happens on the framework owner task. Other tasks read
 * only the published status and the counters (both under s_lock) and the
 * phase (one atomic). */
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "binding_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/net.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

static const char *TAG = "binding";

typedef struct {
    const char *name;
    xiaotai_binding_guard_fn_t guard;
    void *ctx;
} guard_entry_t;

typedef struct {
    const char *name;
    xiaotai_binding_prepare_fn_t prepare;
    void *ctx;
} prepare_entry_t;

/* What the owner task decides; published into s_status by publish(). */
typedef struct {
    xiaotai_binding_phase_t phase;
    bool signed_rebinding;
    bool environment_boot_failed;
    xiaotai_err_t error;
    int32_t code;
    char verification[XIAOTAI_BINDING_CODE_MAX + 1U];
    int64_t deadline_us;
} owner_status_t;

static struct {
    bool configured;
    xiaotai_binding_config_t requested;   /* as given to xiaotai_binding_configure */
    xiaotai_binding_config_t config;      /* defaults applied at init */
    bool initialized;
    bool started;
    guard_entry_t *guards;
    size_t guard_count;
    prepare_entry_t *prepares;
    size_t prepare_count;
    binding_attempt_t *attempt;
    uint32_t serial;                      /* last attempt serial handed out */
    uint32_t prepare_call;                /* last preparation call handed out */
    uint32_t prepare_current;             /* the call whose done is awaited; 0 none */
    uint32_t timer;
    xiaotai_timer_fn_t timer_handler;
    uint32_t report;                     /* reports of the platform core, never 0 once one came */
    uint32_t auto_waiting;                /* report whose automatic start awaits the guards */
    bool reload_pending;                  /* xiaotai_binding_retry() waits for the identity reload */
    const binding_voice_t *voice;
    uint32_t prepare_ms;
    owner_status_t now;
} s;

static xiaotai_critical_t s_lock;
static atomic_bool s_lock_ready;
static xiaotai_binding_status_t s_status;   /* under s_lock; seconds_left and identity fields filled at snapshot */
static int64_t s_status_deadline_us;        /* under s_lock */
static uint32_t s_status_guest;             /* under s_lock; optional live-code projection */
static xiaotai_binding_stats_t s_stats;     /* under s_lock */
static atomic_int s_phase;

/* ------------------------------------------------------------------ counters */

static const size_t STAT_OFFSETS[] = {
    [BINDING_STAT_NEEDED_FIRST] = offsetof(xiaotai_binding_stats_t, needed_first),
    [BINDING_STAT_NEEDED_REBINDING] = offsetof(xiaotai_binding_stats_t, needed_rebinding),
    [BINDING_STAT_ATTEMPTS] = offsetof(xiaotai_binding_stats_t, attempts),
    [BINDING_STAT_ATTEMPTS_SIGNED] = offsetof(xiaotai_binding_stats_t, attempts_signed),
    [BINDING_STAT_REPORTS_REFUSED] = offsetof(xiaotai_binding_stats_t, reports_refused),
    [BINDING_STAT_STALE_CODE_RETRIES] = offsetof(xiaotai_binding_stats_t, stale_code_retries),
    [BINDING_STAT_CONNECT_TIMEOUTS] = offsetof(xiaotai_binding_stats_t, connect_timeouts),
    [BINDING_STAT_GRANTS] = offsetof(xiaotai_binding_stats_t, grants),
    [BINDING_STAT_GRANTS_IGNORED] = offsetof(xiaotai_binding_stats_t, grants_ignored),
    [BINDING_STAT_GRANTS_MALFORMED] = offsetof(xiaotai_binding_stats_t, grants_malformed),
    [BINDING_STAT_GRANTS_MISMATCHED] = offsetof(xiaotai_binding_stats_t, grants_mismatched),
    [BINDING_STAT_STORES] = offsetof(xiaotai_binding_stats_t, stores),
    [BINDING_STAT_STORE_FAILURES] = offsetof(xiaotai_binding_stats_t, store_failures),
    [BINDING_STAT_CONFIRMATIONS] = offsetof(xiaotai_binding_stats_t, confirmations),
    [BINDING_STAT_CONFIRM_TIMEOUTS] = offsetof(xiaotai_binding_stats_t, confirm_timeouts),
    [BINDING_STAT_BOUND] = offsetof(xiaotai_binding_stats_t, bound),
    [BINDING_STAT_FAILED] = offsetof(xiaotai_binding_stats_t, failed),
    [BINDING_STAT_EXPIRED] = offsetof(xiaotai_binding_stats_t, expired),
    [BINDING_STAT_CANCELLED] = offsetof(xiaotai_binding_stats_t, cancelled),
    [BINDING_STAT_GUARD_REFUSALS] = offsetof(xiaotai_binding_stats_t, guard_refusals),
    [BINDING_STAT_AUTO_RESUMED] = offsetof(xiaotai_binding_stats_t, auto_resumed),
    [BINDING_STAT_PREPARE_FAILURES] = offsetof(xiaotai_binding_stats_t, prepare_failures),
    [BINDING_STAT_PREPARE_TIMEOUTS] = offsetof(xiaotai_binding_stats_t, prepare_timeouts),
    [BINDING_STAT_PREPARE_LATE] = offsetof(xiaotai_binding_stats_t, prepare_late),
    [BINDING_STAT_ANNOUNCEMENTS] = offsetof(xiaotai_binding_stats_t, announcements),
    [BINDING_STAT_ANNOUNCEMENTS_SERVER] = offsetof(xiaotai_binding_stats_t, announcements_server),
    [BINDING_STAT_ANNOUNCEMENTS_FAILED] = offsetof(xiaotai_binding_stats_t, announcements_failed),
    [BINDING_STAT_ANNOUNCEMENTS_BUSY] = offsetof(xiaotai_binding_stats_t, announcements_busy),
    [BINDING_STAT_FORGETS] = offsetof(xiaotai_binding_stats_t, forgets),
    [BINDING_STAT_FORGET_FAILURES] = offsetof(xiaotai_binding_stats_t, forget_failures),
};

void binding_count(binding_stat_t stat)
{
    if (!atomic_load(&s_lock_ready) || (size_t)stat >= sizeof(STAT_OFFSETS) / sizeof(STAT_OFFSETS[0])) return;
    xiaotai_critical_enter(&s_lock);
    uint32_t *field = (uint32_t *)((char *)&s_stats + STAT_OFFSETS[stat]);
    if (*field != UINT32_MAX) ++*field;
    xiaotai_critical_exit(&s_lock);
}

void xiaotai_binding_stats(xiaotai_binding_stats_t *stats)
{
    if (stats == NULL) return;
    memset(stats, 0, sizeof(*stats));
    if (!atomic_load(&s_lock_ready)) return;
    xiaotai_critical_enter(&s_lock);
    *stats = s_stats;
    xiaotai_critical_exit(&s_lock);
}

/* -------------------------------------------------------------------- status */

static bool voice_active(void) { return s.voice != NULL && s.voice->active != NULL && s.voice->active(); }

static void lock_prepare(void)
{
    if (atomic_load(&s_lock_ready)) return;
    xiaotai_critical_init(&s_lock);
    atomic_store(&s_lock_ready, true);
}

/* The identity part of a snapshot; never the key. */
static void fill_identity(xiaotai_binding_status_t *status)
{
    status->generation = xiaotai_identity_generation();
    if (xiaotai_identity_device_id(status->device_id, sizeof(status->device_id)) != XIAOTAI_OK) {
        status->device_id[0] = '\0';
        status->client_id[0] = '\0';
        return;
    }
    xiaotai_identity_t identity;
    if (xiaotai_identity_get(&identity) == XIAOTAI_OK && identity.client_id[0] != '\0') {
        memcpy(status->client_id, identity.client_id, sizeof(status->client_id));
    } else {
        xiaotai_net_status_t net;
        xiaotai_net_status(&net);
        if (!net.mac_valid ||
            xiaotai_identity_client_id(net.mac, status->client_id, sizeof(status->client_id)) != XIAOTAI_OK) {
            status->client_id[0] = '\0';
        }
    }
    xiaotai_identity_wipe(&identity, sizeof(identity));
}

void xiaotai_binding_status(xiaotai_binding_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    status->code = XIAOTAI_PLATFORM_NO_CODE;
    if (!atomic_load(&s_lock_ready)) return;
    int64_t deadline_us;
    xiaotai_critical_enter(&s_lock);
    *status = s_status;
    deadline_us = s_status_deadline_us;
    const uint32_t guest = s_status_guest;
    xiaotai_critical_exit(&s_lock);
    status->seconds_left = status->phase == XIAOTAI_BINDING_WAITING ? binding_window_seconds(deadline_us) : 0U;
    if (guest && status->verification[0]) {
        xiaotai_net_status_t net;
        xiaotai_net_status(&net);
        if (!status->seconds_left || net.state != XIAOTAI_NET_UP || net.user_disconnected ||
            !xiaotai_platform_guest_subscribed(guest)) {
            memset(status->verification, 0, sizeof(status->verification));
            status->seconds_left = 0;
        }
    }
    fill_identity(status);
}

bool xiaotai_binding_bound(void) { return atomic_load(&s_phase) == (int)XIAOTAI_BINDING_BOUND; }

/* Copy the owner's decisions into the published status; on a change advance
 * the revision and emit XIAOTAI_EVENT_BINDING_CHANGED with a snapshot. */
static void publish(void)
{
    if (!atomic_load(&s_lock_ready)) return;
    bool announcing = voice_active();
    bool changed;
    xiaotai_binding_phase_t previous;
    const uint32_t guest = s.config.resume_unbound_window_on_reconnect && s.attempt ? s.attempt->guest : 0;
    xiaotai_critical_enter(&s_lock);
    previous = s_status.phase;
    changed = s_status.phase != s.now.phase || s_status.signed_rebinding != s.now.signed_rebinding ||
              s_status.environment_boot_failed != s.now.environment_boot_failed ||
              s_status.error != s.now.error || s_status.code != s.now.code ||
              strcmp(s_status.verification, s.now.verification) != 0 || s_status.announcing != announcing ||
              s_status_deadline_us != s.now.deadline_us || s_status_guest != guest;
    if (changed) {
        s_status.phase = s.now.phase;
        s_status.signed_rebinding = s.now.signed_rebinding;
        s_status.environment_boot_failed = s.now.environment_boot_failed;
        s_status.error = s.now.error;
        s_status.code = s.now.code;
        memcpy(s_status.verification, s.now.verification, sizeof(s_status.verification));
        s_status.announcing = announcing;
        s_status_deadline_us = s.now.deadline_us;
        s_status_guest = guest;
        ++s_status.revision;
    }
    xiaotai_critical_exit(&s_lock);
    atomic_store(&s_phase, (int)s.now.phase);
    if (!changed) return;
    if (previous != s.now.phase) xiaotai_eventlog_debug(BINDING_LOG_SOURCE, "phase", 0, (int32_t)s.now.phase);
    xiaotai_binding_status_t snapshot;
    xiaotai_binding_status(&snapshot);
    xiaotai_args_t args = {.value = (int32_t)snapshot.phase};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_BINDING_CHANGED, &args, &snapshot, sizeof(snapshot));
}

/* For the voice part: the announcing flag may have changed. */
void binding_publish(void) { publish(); }

const xiaotai_binding_config_t *binding_config(void) { return &s.config; }
binding_attempt_t *binding_attempt(void) { return s.attempt; }
xiaotai_binding_phase_t binding_phase(void) { return s.now.phase; }

bool binding_serial_current(void *ctx)
{
    return s.attempt != NULL && (uint32_t)(uintptr_t)ctx == s.attempt->serial;
}

const char *binding_step_name(xiaotai_binding_phase_t phase)
{
    switch (phase) {
    case XIAOTAI_BINDING_PREPARING: return "prepare";
    case XIAOTAI_BINDING_REPORTING: return "report";
    case XIAOTAI_BINDING_CONNECTING: return "connect";
    case XIAOTAI_BINDING_WAITING: return "grant";
    case XIAOTAI_BINDING_STORING: return "store";
    case XIAOTAI_BINDING_CONFIRMING: return "confirm";
    default: return "report";
    }
}

/* -------------------------------------------------------------------- timers */

void binding_timer_stop(void)
{
    xiaotai_timer_cancel(s.timer);
    s.timer = 0;
    s.timer_handler = NULL;
}

/* Forget the id of the timer that is running, so it is never cancelled later. */
static void timer_fired(void *ctx)
{
    xiaotai_timer_fn_t handler = s.timer_handler;
    s.timer = 0;
    s.timer_handler = NULL;
    if (handler != NULL) handler(ctx);
}

void binding_timer_arm(uint32_t delay_ms, xiaotai_timer_fn_t handler)
{
    binding_timer_stop();
    if (s.attempt == NULL) return;
    s.timer_handler = handler;
    if (xiaotai_timer_start(delay_ms, timer_fired, binding_serial_ctx(s.attempt->serial), &s.timer) != XIAOTAI_OK) {
        s.timer_handler = NULL;
        /* Every framework timer slot is taken: without its deadline the attempt
         * could wait forever, so it ends now. */
        s.timer = 0;
        binding_fail(XIAOTAI_ERR_NO_MEM, binding_step_name(s.now.phase), XIAOTAI_PLATFORM_NO_CODE);
    }
}

/* ------------------------------------------------------------------- storage */

/* One identity job (save, reload or erase) at a time. Its done runs on a
 * storage task: it records the result and hands it to the owner task; a poll
 * on the owner task picks the result up when that hand-off found the owner
 * queue full. */
enum { JOB_IDLE = 0, JOB_PENDING, JOB_DONE };
static struct {
    atomic_uint serial;
    atomic_int state;
    atomic_int result;
    binding_storage_fn_t deliver;
    uint32_t poll;
} s_job;

static void job_consume(void)
{
    int expected = JOB_DONE;
    if (!atomic_compare_exchange_strong(&s_job.state, &expected, JOB_IDLE)) return;
    xiaotai_timer_cancel(s_job.poll);
    s_job.poll = 0;
    binding_storage_fn_t deliver = s_job.deliver;
    s_job.deliver = NULL;
    if (deliver != NULL) deliver((xiaotai_err_t)atomic_load(&s_job.result));
}

static void job_poll(void *ctx)
{
    s_job.poll = 0;
    if ((unsigned)(uintptr_t)ctx != atomic_load(&s_job.serial)) return;
    int state = atomic_load(&s_job.state);
    if (state == JOB_DONE) {
        job_consume();
    } else if (state == JOB_PENDING &&
               xiaotai_timer_start(BINDING_STORAGE_POLL_MS, job_poll, ctx, &s_job.poll) != XIAOTAI_OK) {
        s_job.poll = 0;
    }
}

static void job_deliver(void *ctx)
{
    if ((unsigned)(uintptr_t)ctx != atomic_load(&s_job.serial)) return;
    job_consume();
}

xiaotai_err_t binding_storage_begin(binding_storage_fn_t deliver, void **ctx)
{
    if (atomic_load(&s_job.state) != JOB_IDLE) return XIAOTAI_ERR_BUSY;
    unsigned serial = atomic_load(&s_job.serial) + 1U;
    if (serial == 0U) serial = 1U;
    void *job_ctx = (void *)(uintptr_t)serial;
    /* An identity completion may find the queue full. Do not start storage
     * unless its owner poll is already reserved. */
    const xiaotai_err_t err = xiaotai_timer_start(BINDING_STORAGE_POLL_MS, job_poll, job_ctx, &s_job.poll);
    if (err != XIAOTAI_OK) return err;
    atomic_store(&s_job.serial, serial);
    atomic_store(&s_job.result, XIAOTAI_OK);
    atomic_store(&s_job.state, JOB_PENDING);
    s_job.deliver = deliver;
    *ctx = job_ctx;
    return XIAOTAI_OK;
}

void binding_storage_cancel(void)
{
    atomic_store(&s_job.state, JOB_IDLE);
    atomic_fetch_add(&s_job.serial, 1U);
    s_job.deliver = NULL;
    xiaotai_timer_cancel(s_job.poll);
    s_job.poll = 0;
}

/* [storage] */
void binding_storage_done(xiaotai_err_t result, void *ctx)
{
    if ((unsigned)(uintptr_t)ctx != atomic_load(&s_job.serial)) return;
    atomic_store(&s_job.result, result);
    int expected = JOB_PENDING;
    if (!atomic_compare_exchange_strong(&s_job.state, &expected, JOB_DONE)) return;
    /* A full owner queue is left to the poll. */
    (void)xiaotai_run(job_deliver, ctx);
}

/* ------------------------------------------------------------- attempt's end */

void binding_voice_halt(void)
{
    if (s.voice != NULL && s.voice->halt != NULL) s.voice->halt();
}

/* Close everything the attempt holds, then wipe and free it. */
static void end_attempt(void)
{
    binding_attempt_t *attempt = s.attempt;
    if (attempt == NULL) return;
    binding_timer_stop();
    binding_window_stop();
    s.prepare_current = 0;
    binding_guest_close(attempt);
    s.attempt = NULL;
    binding_voice_halt();
    xiaotai_identity_wipe(attempt, sizeof(*attempt));
    xiaotai_mem_free(attempt);
    s.now.verification[0] = '\0';
    s.now.deadline_us = 0;
}

static void enter_failed(xiaotai_err_t error, const char *step, int32_t code)
{
    s.now.phase = XIAOTAI_BINDING_FAILED;
    s.now.error = error;
    s.now.code = code;
    s.now.verification[0] = '\0';
    s.now.deadline_us = 0;
    binding_count(BINDING_STAT_FAILED);
    publish();
    xiaotai_eventlog_event_t record = {.source = BINDING_LOG_SOURCE, .label = "failed", .value = error,
                                       .level = (uint8_t)XIAOTAI_EVENTLOG_ERROR,
                                       .kind = (uint8_t)XIAOTAI_EVENTLOG_ERR};
    xiaotai_eventlog_emit(&record);
    XIAOTAI_LOGW(TAG, "attempt failed: step=%s error=0x%x code=%ld", step, (unsigned)error, (long)code);
    xiaotai_args_t args = {.value = error};
    (void)snprintf(args.text, sizeof(args.text), "%s", step);
    (void)xiaotai_event_emit(XIAOTAI_EVENT_BINDING_FAILED, &args, NULL, 0);
}

void binding_fail(xiaotai_err_t error, const char *step, int32_t code)
{
    if (s.attempt == NULL) return;
    end_attempt();
    enter_failed(error, step, code);
}

void binding_expire(void)
{
    if (s.attempt == NULL) return;
    end_attempt();
    s.now.phase = XIAOTAI_BINDING_EXPIRED;
    s.now.error = XIAOTAI_ERR_TIMEOUT;
    s.now.code = XIAOTAI_PLATFORM_NO_CODE;
    binding_count(BINDING_STAT_EXPIRED);
    publish();
    xiaotai_eventlog_note(BINDING_LOG_SOURCE, "expired", 0, 0);
    xiaotai_args_t args = {.value = XIAOTAI_ERR_TIMEOUT, .text = "expired"};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_BINDING_FAILED, &args, NULL, 0);
}

void binding_succeed(bool new_identity)
{
    if (s.attempt == NULL) return;
    end_attempt();
    /* The guest session is closed above, before the platform logs in. */
    xiaotai_err_t retried = xiaotai_platform_retry();
    if (retried != XIAOTAI_OK) XIAOTAI_LOGW(TAG, "platform retry after binding: 0x%x", (unsigned)retried);
    s.now.phase = XIAOTAI_BINDING_BOUND;
    s.now.error = XIAOTAI_OK;
    s.now.code = XIAOTAI_PLATFORM_NO_CODE;
    binding_count(BINDING_STAT_BOUND);
    publish();
    xiaotai_eventlog_note(BINDING_LOG_SOURCE, "bound", xiaotai_identity_generation(), new_identity ? 1 : 0);
    xiaotai_args_t args = {.value = new_identity ? 1 : 0};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_BINDING_BOUND, &args, NULL, 0);
}

void binding_enter(xiaotai_binding_phase_t phase)
{
    s.now.phase = phase;
    if (s.config.resume_unbound_window_on_reconnect && phase == XIAOTAI_BINDING_STORING) {
        memset(s.now.verification, 0, sizeof(s.now.verification));
        s.now.deadline_us = 0;
    }
    publish();
}

void binding_restore_code(void)
{
    binding_attempt_t *attempt = s.attempt;
    if (attempt == NULL) return;
    memcpy(s.now.verification, attempt->code, sizeof(s.now.verification));
    s.now.deadline_us = attempt->deadline_us;
    publish();
}

void binding_hide_code(void)
{
    memset(s.now.verification, 0, sizeof(s.now.verification));
    s.now.deadline_us = 0;
    binding_voice_halt();
    publish();
}

bool binding_code_visible(void) { return s.now.verification[0] != '\0'; }

bool binding_can_resume_window(const binding_attempt_t *attempt)
{
    return s.config.resume_unbound_window_on_reconnect && attempt != NULL &&
        s.now.phase == XIAOTAI_BINDING_WAITING && !attempt->signed_report && !attempt->grant_received &&
        xiaotai_identity_state() == XIAOTAI_IDENTITY_ABSENT &&
        attempt->identity_generation == xiaotai_identity_generation();
}

void binding_publish_code(uint32_t seconds_left)
{
    binding_attempt_t *attempt = s.attempt;
    if (attempt == NULL) return;
    binding_restore_code();
    xiaotai_args_t args = {.value = (int32_t)seconds_left};
    memcpy(args.text, attempt->code, sizeof(attempt->code));
    (void)xiaotai_event_emit(XIAOTAI_EVENT_BINDING_CODE, &args, NULL, 0);
}

/* An explicit stop: back to NEEDED. */
static void stop_to_needed(void)
{
    end_attempt();
    binding_count(BINDING_STAT_CANCELLED);
    s.now.phase = XIAOTAI_BINDING_NEEDED;
    s.now.error = XIAOTAI_OK;
    s.now.code = XIAOTAI_PLATFORM_NO_CODE;
    publish();
    xiaotai_eventlog_note(BINDING_LOG_SOURCE, "cancelled", 0, 0);
}

static bool in_commit(void)
{
    return s.now.phase == XIAOTAI_BINDING_STORING || s.now.phase == XIAOTAI_BINDING_CONFIRMING;
}

/* A cancel condition (network down, the user left the network, the platform
 * core left the phases a guest session lives in). What was granted is
 * finished: in STORING and CONFIRMING it is only remembered. */
static void condition_cancel(void)
{
    if (s.attempt == NULL) return;
    if (in_commit()) {
        s.attempt->cancel_latched = true;
        return;
    }
    binding_count(BINDING_STAT_CANCELLED);
    binding_fail(XIAOTAI_ERR_INVALID_STATE, binding_step_name(s.now.phase), XIAOTAI_PLATFORM_NO_CODE);
}

xiaotai_err_t binding_attempt_abort(void)
{
    if (s.reload_pending) {
        binding_storage_cancel();
        s.reload_pending = false;
    }
    if (s.attempt == NULL) return XIAOTAI_OK;
    if (in_commit()) return XIAOTAI_ERR_INVALID_STATE;
    stop_to_needed();
    return XIAOTAI_OK;
}

xiaotai_err_t binding_mutation_ready(void)
{
    if (!s.started || !xiaotai_on_owner() || in_commit()) return XIAOTAI_ERR_INVALID_STATE;
    return (s.reload_pending || atomic_load(&s_job.state) != JOB_IDLE ||
            xiaotai_quiesced() || xiaotai_restart_pending()) ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
}

void binding_voice_attach(const binding_voice_t *voice) { s.voice = voice; }

void binding_test_set_prepare_ms(uint32_t ms) { s.prepare_ms = ms != 0U ? ms : XIAOTAI_BINDING_PREPARE_MS; }

/* ----------------------------------------------------------- preparation */

static void prepare_next(void);

static void prepare_timeout(void *ctx)
{
    if (!binding_serial_current(ctx) || s.now.phase != XIAOTAI_BINDING_PREPARING) return;
    s.prepare_current = 0;
    binding_count(BINDING_STAT_PREPARE_TIMEOUTS);
    binding_fail(XIAOTAI_ERR_TIMEOUT, "prepare", XIAOTAI_PLATFORM_NO_CODE);
}

/* [owner] The step's completion. */
static void prepare_done(xiaotai_err_t result, void *done_ctx)
{
    uint32_t call = (uint32_t)(uintptr_t)done_ctx;
    binding_attempt_t *attempt = s.attempt;
    if (attempt == NULL || call == 0U || call != s.prepare_current || s.now.phase != XIAOTAI_BINDING_PREPARING) {
        binding_count(BINDING_STAT_PREPARE_LATE);
        xiaotai_eventlog_warn(BINDING_LOG_SOURCE, "prepare.late", 0, result);
        return;
    }
    s.prepare_current = 0;
    if (attempt->prepare_in_call) {
        attempt->prepare_done_early = true;
        attempt->prepare_early_result = result;
        return;
    }
    binding_timer_stop();
    if (result != XIAOTAI_OK) {
        binding_count(BINDING_STAT_PREPARE_FAILURES);
        binding_fail(result, "prepare", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    ++attempt->prepare_index;
    prepare_next();
}

static void prepare_next(void)
{
    for (;;) {
        binding_attempt_t *attempt = s.attempt;
        if (attempt == NULL || s.now.phase != XIAOTAI_BINDING_PREPARING) return;
        if (attempt->prepare_index >= s.prepare_count) break;
        const prepare_entry_t *step = &s.prepares[attempt->prepare_index];
        uint32_t serial = attempt->serial;
        if (++s.prepare_call == 0U) s.prepare_call = 1U;
        s.prepare_current = s.prepare_call;
        attempt->prepare_in_call = true;
        attempt->prepare_done_early = false;
        binding_timer_arm(s.prepare_ms, prepare_timeout);
        if (s.attempt != attempt) return;
        xiaotai_err_t result = step->prepare(prepare_done, (void *)(uintptr_t)s.prepare_call, step->ctx);
        if (s.attempt != attempt || attempt->serial != serial) return;
        attempt->prepare_in_call = false;
        if (result != XIAOTAI_OK) {
            /* done is never called after a refusal; one that came anyway is void. */
            s.prepare_current = 0;
            binding_count(BINDING_STAT_PREPARE_FAILURES);
            XIAOTAI_LOGW(TAG, "preparation %s refused: 0x%x", step->name, (unsigned)result);
            binding_fail(result, "prepare", XIAOTAI_PLATFORM_NO_CODE);
            return;
        }
        if (!attempt->prepare_done_early) return;   /* its done follows */
        binding_timer_stop();
        if (attempt->prepare_early_result != XIAOTAI_OK) {
            binding_count(BINDING_STAT_PREPARE_FAILURES);
            binding_fail(attempt->prepare_early_result, "prepare", XIAOTAI_PLATFORM_NO_CODE);
            return;
        }
        ++attempt->prepare_index;
    }
    binding_timer_stop();
    binding_enter(XIAOTAI_BINDING_REPORTING);
    binding_report_begin();
}

/* ------------------------------------------------------------- beginning */

/* Everything an attempt needs besides the guards. */
static xiaotai_err_t check_ready(bool reloading)
{
    if (xiaotai_quiesced()) return XIAOTAI_ERR_BUSY;
    if (xiaotai_net_provisioning_boot()) return XIAOTAI_ERR_INVALID_STATE;
    /* Nothing may overwrite what could not be read; a retry reloads first. */
    if (!reloading && xiaotai_identity_state() == XIAOTAI_IDENTITY_UNREADABLE) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (!platform.discovered || !platform.clock_synced) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    if (net.state != XIAOTAI_NET_UP || !net.mac_valid) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    return XIAOTAI_OK;
}

/* Ask every guard in registration order; the first refusal is returned and
 * *busy tells whether it was a guard's XIAOTAI_ERR_BUSY. */
static xiaotai_err_t ask_guards(bool *busy)
{
    for (size_t i = 0; i < s.guard_count; ++i) {
        xiaotai_err_t result = s.guards[i].guard(s.guards[i].ctx);
        if (result != XIAOTAI_OK) {
            binding_count(BINDING_STAT_GUARD_REFUSALS);
            xiaotai_eventlog_warn(BINDING_LOG_SOURCE, "guard.refused", 0, result);
            XIAOTAI_LOGI(TAG, "guard %s refused: 0x%x", s.guards[i].name, (unsigned)result);
            if (busy != NULL) *busy = result == XIAOTAI_ERR_BUSY;
            return result;
        }
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t begin_attempt(bool *guard_busy)
{
    if (guard_busy != NULL) *guard_busy = false;
    xiaotai_err_t result = check_ready(false);
    if (result != XIAOTAI_OK) return result;
    result = ask_guards(guard_busy);
    if (result != XIAOTAI_OK) return result;
    binding_attempt_t *attempt = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*attempt));
    if (attempt == NULL) return XIAOTAI_ERR_NO_MEM;
    if (++s.serial == 0U) s.serial = 1U;
    attempt->serial = s.serial;
    attempt->identity_generation = xiaotai_identity_generation();
    attempt->signed_report = xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID;
    attempt->guest_closed = XIAOTAI_OK;
    s.attempt = attempt;
    s.auto_waiting = 0;
    binding_count(BINDING_STAT_ATTEMPTS);
    if (attempt->signed_report) binding_count(BINDING_STAT_ATTEMPTS_SIGNED);
    xiaotai_eventlog_debug(BINDING_LOG_SOURCE, "attempt", xiaotai_identity_generation(), attempt->signed_report);
    s.now.phase = XIAOTAI_BINDING_PREPARING;
    s.now.signed_rebinding = attempt->signed_report;
    s.now.error = XIAOTAI_OK;
    s.now.code = XIAOTAI_PLATFORM_NO_CODE;
    s.now.verification[0] = '\0';
    s.now.deadline_us = 0;
    publish();
    prepare_next();
    return XIAOTAI_OK;
}

static bool may_start(void)
{
    return s.started && s.attempt == NULL && !s.reload_pending && atomic_load(&s_job.state) == JOB_IDLE &&
           (s.now.phase == XIAOTAI_BINDING_NEEDED || s.now.phase == XIAOTAI_BINDING_FAILED ||
            s.now.phase == XIAOTAI_BINDING_EXPIRED);
}

xiaotai_err_t xiaotai_binding_start(void)
{
    if (!may_start()) return XIAOTAI_ERR_INVALID_STATE;
    return begin_attempt(NULL);
}

/* The identity was read again for xiaotai_binding_retry(). */
static void retry_reloaded(xiaotai_err_t result)
{
    if (!s.reload_pending) return;
    s.reload_pending = false;
    if (result != XIAOTAI_OK) {
        enter_failed(result, "store", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    if (!may_start()) return;
    result = begin_attempt(NULL);
    if (result != XIAOTAI_OK) {
        s.now.error = result;
        publish();
    }
}

xiaotai_err_t xiaotai_binding_retry(void)
{
    if (!s.started || s.attempt != NULL || s.reload_pending) return XIAOTAI_ERR_INVALID_STATE;
    if (atomic_load(&s_job.state) != JOB_IDLE) return XIAOTAI_ERR_BUSY;
    if (s.now.environment_boot_failed) {
        if (xiaotai_quiesced()) return XIAOTAI_ERR_BUSY;
        if (s.now.phase != XIAOTAI_BINDING_FAILED) return XIAOTAI_ERR_PLATFORM_OFFLINE;
        xiaotai_net_status_t net;
        xiaotai_net_status(&net);
        if (net.state != XIAOTAI_NET_UP) return XIAOTAI_ERR_PLATFORM_OFFLINE;
        /* The legacy refresh acknowledged the view, but a failed boot did
         * not consume the retry or start any network or storage work. */
        binding_enter(XIAOTAI_BINDING_NEEDED);
        return XIAOTAI_OK;
    }
    if (s.now.phase == XIAOTAI_BINDING_UNKNOWN || s.now.phase == XIAOTAI_BINDING_BOUND) {
        return xiaotai_platform_retry();
    }
    xiaotai_err_t result = check_ready(true);
    if (result != XIAOTAI_OK) return result;
    result = ask_guards(NULL);
    if (result != XIAOTAI_OK) return result;
    /* An earlier save may have reached storage although its attempt failed:
     * read it before deciding between a first and a signed report. */
    void *ctx = NULL;
    result = binding_storage_begin(retry_reloaded, &ctx);
    if (result != XIAOTAI_OK) return result;
    result = xiaotai_identity_reload(binding_storage_done, ctx);
    if (result != XIAOTAI_OK) {
        binding_storage_cancel();
        return result;
    }
    s.reload_pending = true;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_binding_cancel(void)
{
    if (s.reload_pending) {
        binding_storage_cancel();
        s.reload_pending = false;
        binding_count(BINDING_STAT_CANCELLED);
        s.now.phase = XIAOTAI_BINDING_NEEDED;
        s.now.error = XIAOTAI_OK;
        publish();
        return XIAOTAI_OK;
    }
    if (s.attempt == NULL || in_commit()) return XIAOTAI_ERR_INVALID_STATE;
    stop_to_needed();
    return XIAOTAI_OK;
}

void xiaotai_binding_guards_changed(void)
{
    if (s.auto_waiting == 0U || s.auto_waiting != s.report || !may_start() ||
        s.now.phase != XIAOTAI_BINDING_NEEDED) {
        return;
    }
    bool busy = false;
    xiaotai_err_t result = begin_attempt(&busy);
    if (result == XIAOTAI_OK) {
        binding_count(BINDING_STAT_AUTO_RESUMED);
    } else if (!busy) {
        s.auto_waiting = 0;
    }
}

/* ------------------------------------------------------------- registries */

xiaotai_err_t xiaotai_binding_guard_register(const char *name, xiaotai_binding_guard_fn_t guard, void *ctx)
{
    if (name == NULL || guard == NULL) return XIAOTAI_ERR_INVALID_ARG;
    if (s.guards == NULL) return XIAOTAI_ERR_INVALID_STATE;
    if (s.guard_count >= XIAOTAI_BINDING_GUARDS_MAX) return XIAOTAI_ERR_NO_MEM;
    s.guards[s.guard_count++] = (guard_entry_t){.name = name, .guard = guard, .ctx = ctx};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_binding_prepare_register(const char *name, xiaotai_binding_prepare_fn_t prepare, void *ctx)
{
    if (name == NULL || prepare == NULL) return XIAOTAI_ERR_INVALID_ARG;
    if (s.prepares == NULL) return XIAOTAI_ERR_INVALID_STATE;
    if (s.prepare_count >= XIAOTAI_BINDING_PREPARES_MAX) return XIAOTAI_ERR_NO_MEM;
    s.prepares[s.prepare_count++] = (prepare_entry_t){.name = name, .prepare = prepare, .ctx = ctx};
    return XIAOTAI_OK;
}

/* ----------------------------------------------------------------- events */

/* The platform core reported that a binding is needed. */
static void on_binding_needed(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    int32_t rebinding = event->args.value != 0 ? 1 : 0;
    if (!rebinding && binding_can_resume_window(s.attempt) && s.attempt->network_suspended) {
        binding_resume_window();
        return;
    }
    binding_count(rebinding ? BINDING_STAT_NEEDED_REBINDING : BINDING_STAT_NEEDED_FIRST);
    xiaotai_eventlog_note(BINDING_LOG_SOURCE, "needed", xiaotai_identity_generation(), rebinding);
    if (++s.report == 0U) s.report = 1U;
    s.auto_waiting = 0;
    if (s.attempt == NULL && !s.reload_pending) {
        s.now.phase = XIAOTAI_BINDING_NEEDED;
        s.now.error = XIAOTAI_OK;
        s.now.code = XIAOTAI_PLATFORM_NO_CODE;
        publish();
    }
    xiaotai_args_t args = {.value = rebinding};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_BINDING_NEEDED, &args, NULL, 0);
    if (s.config.manual_start || !may_start() || s.now.phase != XIAOTAI_BINDING_NEEDED) return;
    bool busy = false;
    xiaotai_err_t result = begin_attempt(&busy);
    if (result == XIAOTAI_ERR_BUSY && busy) {
        s.auto_waiting = s.report;
    } else if (result != XIAOTAI_OK) {
        XIAOTAI_LOGW(TAG, "automatic start refused: 0x%x", (unsigned)result);
    }
}

/* A guest session lives only while the platform core has no session of its own. */
static void check_platform(void)
{
    if (s.attempt == NULL) return;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (binding_can_resume_window(s.attempt) && s.attempt->network_suspended && !platform.authorized &&
        platform.phase == XIAOTAI_PLATFORM_STOPPED) return;
    if (platform.phase == XIAOTAI_PLATFORM_NO_IDENTITY || platform.phase == XIAOTAI_PLATFORM_UNBOUND ||
        platform.phase == XIAOTAI_PLATFORM_PAUSED) {
        return;
    }
    condition_cancel();
}

static void on_platform(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    check_platform();
    if (strcmp(event->name, XIAOTAI_EVENT_PLATFORM_ONLINE) != 0 || s.attempt != NULL) return;
    if (s.reload_pending) {
        binding_storage_cancel();
        s.reload_pending = false;
    }
    s.now.phase = XIAOTAI_BINDING_BOUND;
    s.now.error = XIAOTAI_OK;
    s.now.code = XIAOTAI_PLATFORM_NO_CODE;
    publish();
}

static void on_net(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (strcmp(event->name, XIAOTAI_EVENT_NET_DOWN) == 0) {
        if (binding_can_resume_window(s.attempt)) {
            xiaotai_net_status_t net;
            xiaotai_net_status(&net);
            if (!net.user_disconnected) {
                s.attempt->network_suspended = true;
                binding_hide_code();
                return;
            }
        }
        condition_cancel();
        return;
    }
    if (event->data != NULL && event->size >= sizeof(xiaotai_net_status_t)) {
        xiaotai_net_status_t net;
        memcpy(&net, event->data, sizeof(net));
        if (net.user_disconnected) condition_cancel();
    }
}

static void on_quiesce(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    /* STORING and CONFIRMING run to their end under any quiesce. */
    if (event->args.value != 0 && s.attempt != NULL && !in_commit()) {
        binding_count(BINDING_STAT_CANCELLED);
        binding_fail(XIAOTAI_ERR_INVALID_STATE, binding_step_name(s.now.phase), XIAOTAI_PLATFORM_NO_CODE);
    }
}

/* A direct reset owns the imminent restart. Otherwise only an environment
 * switch cannot interrupt an attempt; other quiesces may cancel it. */
static xiaotai_err_t quiesce_guard(const char *reason, void *ctx)
{
    (void)ctx;
    if (binding_direct_reset_pending()) return XIAOTAI_ERR_BUSY;
    if (reason != NULL && strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0 && s.attempt != NULL) {
        return XIAOTAI_ERR_BUSY;
    }
    return XIAOTAI_OK;
}

static void intent_start(const xiaotai_args_t *args, void *ctx)
{
    (void)args; (void)ctx;
    xiaotai_err_t result = xiaotai_binding_start();
    if (result != XIAOTAI_OK) XIAOTAI_LOGI(TAG, "start: 0x%x", (unsigned)result);
}

static void intent_retry(const xiaotai_args_t *args, void *ctx)
{
    (void)args; (void)ctx;
    xiaotai_err_t result = xiaotai_binding_retry();
    if (result != XIAOTAI_OK) XIAOTAI_LOGI(TAG, "retry: 0x%x", (unsigned)result);
}

static void intent_cancel(const xiaotai_args_t *args, void *ctx)
{
    (void)args; (void)ctx;
    (void)xiaotai_binding_cancel();
}

static void intent_announce(const xiaotai_args_t *args, void *ctx)
{
    (void)args; (void)ctx;
    (void)xiaotai_binding_announce();
}

/* -------------------------------------------------------------- lifecycle */

static bool config_valid(const xiaotai_binding_config_t *config)
{
    return config->wait_s <= 3600U && config->connect_s <= 120U && config->announce_rounds <= 10U &&
           config->guest_subscribe_timeout_s <= 120U &&
           config->announce_gap_ms <= 5000U && (unsigned)config->voice <= (unsigned)XIAOTAI_BINDING_VOICE_NONE;
}

xiaotai_err_t xiaotai_binding_configure(const xiaotai_binding_config_t *config)
{
    if (s.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (config == NULL) {
        s.configured = false;
        memset(&s.requested, 0, sizeof(s.requested));
        return XIAOTAI_OK;
    }
    if (!config_valid(config)) return XIAOTAI_ERR_INVALID_ARG;
    s.requested = *config;
    s.configured = true;
    return XIAOTAI_OK;
}

static void apply_defaults(void)
{
    s.config = s.requested;
    if (s.config.wait_s == 0U) s.config.wait_s = BINDING_DEFAULT_WAIT_S;
    if (s.config.connect_s == 0U) s.config.connect_s = BINDING_DEFAULT_CONNECT_S;
    if (s.config.stale_code_retries == 0U) s.config.stale_code_retries = BINDING_DEFAULT_STALE_RETRIES;
    if (s.config.announce_rounds == 0U) s.config.announce_rounds = BINDING_DEFAULT_ANNOUNCE_ROUNDS;
    if (s.config.announce_gap_ms == 0U) s.config.announce_gap_ms = BINDING_DEFAULT_ANNOUNCE_GAP_MS;
}

static void release_tables(void)
{
    xiaotai_mem_free(s.guards);
    xiaotai_mem_free(s.prepares);
    s.guards = NULL;
    s.prepares = NULL;
    s.guard_count = 0;
    s.prepare_count = 0;
}

xiaotai_err_t xiaotai_module_binding_init(void)
{
    if (s.initialized) return XIAOTAI_ERR_INVALID_STATE;
    lock_prepare();
    apply_defaults();
    if (s.prepare_ms == 0U) s.prepare_ms = XIAOTAI_BINDING_PREPARE_MS;
    s.now.phase = XIAOTAI_BINDING_UNKNOWN;
    s.now.code = XIAOTAI_PLATFORM_NO_CODE;
    s_status.code = XIAOTAI_PLATFORM_NO_CODE;

    const xiaotai_platform_requirement_t requirement = {
        .request_body_bytes = BINDING_REQUEST_BODY_BYTES,
        .response_bytes = BINDING_RESPONSE_BYTES,
        .signal_bytes = BINDING_SIGNAL_BYTES,
        .concurrent_requests = 1,
    };
    xiaotai_err_t result = xiaotai_platform_require(&requirement);
    if (result != XIAOTAI_OK) return result;

    s.guards = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_BINDING_GUARDS_MAX, sizeof(*s.guards));
    s.prepares = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_BINDING_PREPARES_MAX, sizeof(*s.prepares));
    if (s.guards == NULL || s.prepares == NULL) {
        release_tables();
        return XIAOTAI_ERR_NO_MEM;
    }

    static const xiaotai_subscriber_descriptor_t PLATFORM_EVENTS[] = {
        {XIAOTAI_EVENT_PLATFORM_ONLINE, on_platform, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, on_platform, NULL},
        {XIAOTAI_EVENT_PLATFORM_PAUSED, on_platform, NULL},
    };
    result = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, on_binding_needed, NULL);
    for (size_t i = 0; result == XIAOTAI_OK && i < sizeof(PLATFORM_EVENTS) / sizeof(PLATFORM_EVENTS[0]); ++i) {
        result = xiaotai_event_subscribe_static(&PLATFORM_EVENTS[i]);
    }
    if (result == XIAOTAI_OK) result = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_NET_DOWN, on_net, NULL);
    if (result == XIAOTAI_OK) result = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_NET_STATE, on_net, NULL);
    if (result == XIAOTAI_OK) result = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_QUIESCE, on_quiesce, NULL);
    if (result == XIAOTAI_OK) result = xiaotai_intent_handle(XIAOTAI_INTENT_BINDING_START, intent_start, NULL);
    if (result == XIAOTAI_OK) result = xiaotai_intent_handle(XIAOTAI_INTENT_BINDING_RETRY, intent_retry, NULL);
    if (result == XIAOTAI_OK) result = xiaotai_intent_handle(XIAOTAI_INTENT_BINDING_CANCEL, intent_cancel, NULL);
    if (result == XIAOTAI_OK) result = xiaotai_intent_handle(XIAOTAI_INTENT_BINDING_ANNOUNCE, intent_announce, NULL);
    if (result == XIAOTAI_OK) result = xiaotai_quiesce_guard_register("binding", quiesce_guard, NULL);
    if (result == XIAOTAI_OK) result = binding_voice_init();
    if (result == XIAOTAI_OK) result = binding_reset_register();
    if (result == XIAOTAI_OK) result = binding_at_register();
    if (result != XIAOTAI_OK) {
        release_tables();
        return result;
    }
    s.initialized = true;
    return XIAOTAI_OK;
}

void xiaotai_module_binding_start(void)
{
    s.started = true;
    s.now.environment_boot_failed = s.config.present_environment_boot_failure &&
                                    !xiaotai_net_provisioning_boot() &&
                                    xiaotai_platform_environment_boot_failed();
    if (s.now.environment_boot_failed) {
        xiaotai_platform_status_t platform;
        xiaotai_platform_status(&platform);
        s.now.phase = XIAOTAI_BINDING_FAILED;
        s.now.error = platform.last_error;
    } else {
        /* Until the platform core concludes, a stored identity counts as bound. */
        s.now.phase = xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID ? XIAOTAI_BINDING_BOUND
                                                                       : XIAOTAI_BINDING_UNKNOWN;
        s.now.error = XIAOTAI_OK;
    }
    s.now.code = XIAOTAI_PLATFORM_NO_CODE;
    publish();
}

void xiaotai_binding_reset(void)
{
    binding_voice_halt();
    binding_attempt_t *attempt = s.attempt;
    if (attempt != NULL) {
        xiaotai_identity_wipe(attempt, sizeof(*attempt));
        xiaotai_mem_free(attempt);
    }
    release_tables();
    binding_window_forget();
    atomic_store(&s_job.state, JOB_IDLE);
    atomic_fetch_add(&s_job.serial, 1U);
    s_job.deliver = NULL;
    s_job.poll = 0;
    memset(&s, 0, sizeof(s));
    s.now.code = XIAOTAI_PLATFORM_NO_CODE;
    lock_prepare();
    xiaotai_critical_enter(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.code = XIAOTAI_PLATFORM_NO_CODE;
    s_status_deadline_us = 0;
    s_status_guest = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    xiaotai_critical_exit(&s_lock);
    atomic_store(&s_phase, (int)XIAOTAI_BINDING_UNKNOWN);
}
