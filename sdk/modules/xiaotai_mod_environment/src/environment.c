// SPDX-License-Identifier: MIT
/* Environment module: the switch coordinator (phase machine, events, intents).
 *
 * Everything below runs on the framework owner task except status(),
 * stats() and the host reset. The few status fields other tasks read are
 * written under s_lock; the rest of the state is the owner's alone.
 *
 * Order of a switch (xiaotai/modules/environment.h): admission, quiesce
 * (the framework asks every quiesce guard), network maintenance gate,
 * platform gate, wait for the gate to drain and the settings to settle, the
 * storage barrier, the point of no return, store and read back, then either
 * the restart (the quiesce is handed to the framework's orderly restart) or
 * the release in reverse order. A store that timed out may still be running
 * on the storage worker, so its release starts with a key-value barrier and
 * stops in ERROR_HELD, nothing released, when that barrier fails. */
#include <stdatomic.h>
#include <string.h>
#include "environment_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/environment_config.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/preferences.h"

#define EVENTLOG_SOURCE "environment"

typedef struct {
    /* Status (written under s_lock, read by status() from any task). */
    xiaotai_environment_phase_t phase;
    uint32_t revision;
    xiaotai_err_t error;
    bool cancel_requested;
    bool write_attempted;
    xiaotai_environment_stats_t stats;
    uint32_t stale;

    /* Owner task only. */
    bool started;
    bool reset_base;                              /* the switch stores NULL (built-in address) */
    char base[XIAOTAI_PLATFORM_BASE_MAX + 1U];    /* normalized base of the switch */
    uint32_t quiesce_ticket, net_ticket, gate_ticket;
    bool barrier_busy;
    bool store_unconfirmed;                       /* a timed-out store: release barrier still owed */
    bool unlimited;                               /* step 4 has no deadline */
    int64_t deadline_us;
    uint32_t timer;
    uint32_t sequence;                            /* switch and step whose results are awaited */
} environment_state_t;

static environment_state_t *s_env;
static xiaotai_environment_config_t s_config;
static xiaotai_critical_t s_lock;
static atomic_bool s_lock_ready;

/* ----------------------------------------------------------------- helpers */

static void lock(void) { xiaotai_critical_enter(&s_lock); }
static void unlock(void) { xiaotai_critical_exit(&s_lock); }

#define COUNT(field)                  \
    do {                              \
        lock();                       \
        ++s_env->stats.field;         \
        unlock();                     \
    } while (0)

static uint32_t wait_ms(void) { return s_config.wait_ms ? s_config.wait_ms : ENVIRONMENT_WAIT_DEFAULT_MS; }
static uint32_t release_ms(void) { return s_config.release_ms ? s_config.release_ms : ENVIRONMENT_RELEASE_DEFAULT_MS; }

void environment_effective_config(xiaotai_environment_config_t *config)
{
    if (!config) return;
    config->wait_ms = wait_ms();
    config->release_ms = release_ms();
}

uint32_t environment_stale_results(void)
{
    if (!s_env || !atomic_load(&s_lock_ready)) return 0;
    lock();
    const uint32_t stale = s_env->stale;
    unlock();
    return stale;
}

void xiaotai_environment_status(xiaotai_environment_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (s_env && atomic_load(&s_lock_ready)) {
        lock();
        status->phase = s_env->phase;
        status->revision = s_env->revision;
        status->error = s_env->error;
        status->cancel_requested = s_env->cancel_requested;
        status->write_attempted = s_env->write_attempted;
        unlock();
    }
    /* Read on demand: the platform keeps it in memory, so the module stores
     * no copy of the three addresses. */
    xiaotai_platform_environment(&status->environment);
}

void xiaotai_environment_stats(xiaotai_environment_stats_t *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (!s_env || !atomic_load(&s_lock_ready)) return;
    lock();
    *stats = s_env->stats;
    unlock();
}

/* The status fields change together, and every change is announced. */
static void publish(xiaotai_environment_phase_t phase, xiaotai_err_t error, bool cancel, bool write_attempted)
{
    lock();
    s_env->phase = phase;
    s_env->error = error;
    s_env->cancel_requested = cancel;
    s_env->write_attempted = write_attempted;
    ++s_env->revision;
    unlock();
    xiaotai_environment_status_t status;
    xiaotai_environment_status(&status);
    const xiaotai_args_t args = {.value = (int32_t)phase};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_ENVIRONMENT_CHANGED, &args, &status, sizeof(status));
}

static void announce_result(xiaotai_environment_result_t result)
{
    xiaotai_environment_status_t status;
    xiaotai_environment_status(&status);
    const xiaotai_args_t args = {.value = (int32_t)result};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_ENVIRONMENT_RESULT, &args, &status, sizeof(status));
}

static void stop_timer(void)
{
    if (s_env->timer) {
        xiaotai_timer_cancel(s_env->timer);
        s_env->timer = 0;
    }
}

static void poll(void *ctx);

static xiaotai_err_t arm(uint32_t delay_ms)
{
    stop_timer();
    const xiaotai_err_t err = xiaotai_timer_start(delay_ms, poll, NULL, &s_env->timer);
    if (err != XIAOTAI_OK) {
        s_env->timer = 0;
        XIAOTAI_LOGE(ENVIRONMENT_TAG, "no timer for the wait: 0x%x", (unsigned)err);
    }
    return err;
}

static uint32_t next_sequence(void)
{
    if (++s_env->sequence == 0) s_env->sequence = 1;
    return s_env->sequence;
}

static void count_stale(void)
{
    lock();
    ++s_env->stale;
    unlock();
}

/* --------------------------------------------------------------- releasing */

/* Reverse order of acquisition; each step is forgotten once it succeeded, so
 * a release that is tried again continues where it stopped. */
static xiaotai_err_t release_gates(void)
{
    if (s_env->gate_ticket) {
        const xiaotai_err_t err = xiaotai_platform_gate_open(s_env->gate_ticket);
        if (err != XIAOTAI_OK) return err;
        s_env->gate_ticket = 0;
    }
    if (s_env->net_ticket) {
        xiaotai_net_maintenance_end(s_env->net_ticket);
        s_env->net_ticket = 0;
    }
    if (s_env->quiesce_ticket) {
        xiaotai_quiesce_end(s_env->quiesce_ticket);
        s_env->quiesce_ticket = 0;
    }
    return XIAOTAI_OK;
}

/* A release step failed (or the switch must not let go): everything still
 * held stays held until a cancel. */
static void hold(xiaotai_err_t err)
{
    stop_timer();
    COUNT(release_failures);
    publish(XIAOTAI_ENVIRONMENT_ERROR_HELD, err, false, s_env->write_attempted);
    xiaotai_eventlog_error(EVENTLOG_SOURCE, "held", 0, err);
    XIAOTAI_LOGE(ENVIRONMENT_TAG, "switch held (0x%x); cancel releases", (unsigned)err);
}

static void finish(void)
{
    const xiaotai_err_t error = s_env->error;
    const xiaotai_environment_result_t result =
        error == XIAOTAI_OK ? XIAOTAI_ENVIRONMENT_CANCELLED : XIAOTAI_ENVIRONMENT_FAILED;
    if (result == XIAOTAI_ENVIRONMENT_CANCELLED) {
        COUNT(cancelled);
        xiaotai_eventlog_note(EVENTLOG_SOURCE, "cancelled", 0, 0);
    } else {
        COUNT(failed);
        xiaotai_eventlog_error(EVENTLOG_SOURCE, "failed", 0, error);
    }
    publish(XIAOTAI_ENVIRONMENT_IDLE, error, false, s_env->write_attempted);
    announce_result(result);
    XIAOTAI_LOGI(ENVIRONMENT_TAG, "released; %s (0x%x)", result == XIAOTAI_ENVIRONMENT_CANCELLED ? "cancelled" : "failed",
                 (unsigned)error);
}

static void release_step(void)
{
    if (s_env->store_unconfirmed) {
        if (s_env->barrier_busy) return;
        const uint32_t sequence = next_sequence();
        const xiaotai_err_t err = environment_barrier_start(ENVIRONMENT_BARRIER_RELEASE, release_ms(), sequence);
        if (err != XIAOTAI_OK) {
            hold(err);
            return;
        }
        s_env->barrier_busy = true;
        return;
    }
    const xiaotai_err_t err = release_gates();
    if (err != XIAOTAI_OK) {
        hold(err);
        return;
    }
    finish();
}

/* Begins the release with the switch's error as it stands (XIAOTAI_OK for a
 * cancel). */
static void release_begin(void)
{
    stop_timer();
    publish(XIAOTAI_ENVIRONMENT_RELEASING, s_env->error, false, s_env->write_attempted);
    release_step();
}

static void fail(xiaotai_err_t err)
{
    lock();
    s_env->error = err;
    unlock();
    release_begin();
}

static void wait_again(void)
{
    const xiaotai_err_t err = arm(ENVIRONMENT_POLL_MS);
    if (err != XIAOTAI_OK) fail(err);
}

/* ------------------------------------------------------------ the switch */

static void store_done(xiaotai_err_t result, void *ctx);

static void write_now(void)
{
    stop_timer();
    publish(XIAOTAI_ENVIRONMENT_WRITING, XIAOTAI_OK, false, true);
    const uint32_t sequence = next_sequence();
    const xiaotai_err_t err = xiaotai_platform_environment_store(s_env->reset_base ? NULL : s_env->base, store_done,
                                                                 (void *)(uintptr_t)sequence);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE(ENVIRONMENT_TAG, "store not started: 0x%x; no restart", (unsigned)err);
        fail(err);
    }
}

static void restart(void)
{
    COUNT(saved);
    publish(XIAOTAI_ENVIRONMENT_RESTARTING, XIAOTAI_OK, false, true);
    announce_result(XIAOTAI_ENVIRONMENT_SAVED);
    xiaotai_eventlog_note(EVENTLOG_SOURCE, "saved", 0, 0);
    XIAOTAI_LOGI(ENVIRONMENT_TAG, "stored and read back; restarting, network credentials and identity kept");
    xiaotai_err_t err = xiaotai_net_maintenance_restart(s_env->net_ticket);
    if (err == XIAOTAI_OK) {
        /* The quiesce now belongs to the orderly restart and is never
         * released: nothing begins between the store and the restart. */
        err = xiaotai_restart_request(XIAOTAI_QUIESCE_ENVIRONMENT, s_env->quiesce_ticket);
    }
    if (err != XIAOTAI_OK) hold(err);
}

static void store_done(xiaotai_err_t result, void *ctx)
{
    const uint32_t sequence = (uint32_t)(uintptr_t)ctx;
    if (!s_env || s_env->phase != XIAOTAI_ENVIRONMENT_WRITING || sequence != s_env->sequence) {
        if (s_env) count_stale();
        return;
    }
    if (result == XIAOTAI_OK) {
        restart();
        return;
    }
    if (result == XIAOTAI_ERR_TIMEOUT) {
        /* The accepted write may still run on the storage worker. */
        COUNT(store_timeouts);
        s_env->store_unconfirmed = true;
    }
    XIAOTAI_LOGE(ENVIRONMENT_TAG, "store failed: 0x%x; stored value may have changed; no restart", (unsigned)result);
    fail(result);
}

static bool deadline_passed(void) { return !s_env->unlimited && xiaotai_time_us() >= s_env->deadline_us; }

static uint32_t remaining_ms(void)
{
    if (s_env->unlimited) return ENVIRONMENT_BARRIER_SLICE_MS;
    const int64_t left = (s_env->deadline_us - xiaotai_time_us() + 999) / 1000;
    return left <= 0 ? 0U : left >= (int64_t)UINT32_MAX ? UINT32_MAX - 1U : (uint32_t)left;
}

static bool settled(void)
{
    return xiaotai_platform_gate_drained(s_env->gate_ticket) && !xiaotai_preferences_pending();
}

static void wait_timed_out(void)
{
    COUNT(wait_timeouts);
    XIAOTAI_LOGW(ENVIRONMENT_TAG, "requests or settings did not settle in time");
    fail(XIAOTAI_ERR_TIMEOUT);
}

static bool held_gates_ready(void)
{
    bool ready;
    const xiaotai_err_t err = xiaotai_quiesce_check(s_env->quiesce_ticket, &ready);
    if (err == XIAOTAI_OK && ready) return true;
    if (err != XIAOTAI_OK) {
        fail(err);
    } else if (deadline_passed()) {
        wait_timed_out();
    } else {
        wait_again();
    }
    return false;
}

/* Step 4, every ENVIRONMENT_POLL_MS while no barrier runs. */
static void poll(void *ctx)
{
    (void)ctx;
    s_env->timer = 0;
    if (s_env->phase != XIAOTAI_ENVIRONMENT_WAITING || s_env->barrier_busy) return;
    if (s_env->cancel_requested) {
        release_begin();
        return;
    }
    if (!held_gates_ready()) return;
    if (deadline_passed()) {
        wait_timed_out();
        return;
    }
    if (!settled()) {
        wait_again();
        return;
    }
    const uint32_t sequence = next_sequence();
    const xiaotai_err_t err = environment_barrier_start(ENVIRONMENT_BARRIER_BEFORE_WRITE, remaining_ms(), sequence);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE(ENVIRONMENT_TAG, "no barrier task: 0x%x", (unsigned)err);
        fail(err);
        return;
    }
    s_env->barrier_busy = true;
}

static void barrier_before_write(xiaotai_err_t result)
{
    if (s_env->cancel_requested) {
        release_begin();
        return;
    }
    if (result == XIAOTAI_OK) {
        /* The point of no return. Settings are frozen and the gate is
         * closed, so this holds unless something broke a rule; then keep
         * waiting instead of writing behind it. */
        if (!settled()) {
            if (deadline_passed()) {
                wait_timed_out();
            } else {
                wait_again();
            }
            return;
        }
        if (held_gates_ready()) write_now();
        return;
    }
    if (result == XIAOTAI_ERR_TIMEOUT) {
        if (s_env->unlimited) {
            wait_again();
            return;
        }
        wait_timed_out();
        return;
    }
    XIAOTAI_LOGE(ENVIRONMENT_TAG, "storage barrier failed: 0x%x", (unsigned)result);
    if (XIAOTAI_ENVIRONMENT_HOLD_ON_SETTINGS_ERROR) {
        hold(result);
        return;
    }
    fail(result);
}

void environment_barrier_done(environment_barrier_kind_t kind, uint32_t sequence, xiaotai_err_t result)
{
    if (!s_env) return;
    if (!s_env->barrier_busy || sequence != s_env->sequence) {
        count_stale();
        return;
    }
    s_env->barrier_busy = false;
    if (kind == ENVIRONMENT_BARRIER_BEFORE_WRITE) {
        if (s_env->phase == XIAOTAI_ENVIRONMENT_WAITING) barrier_before_write(result);
        return;
    }
    if (s_env->phase != XIAOTAI_ENVIRONMENT_RELEASING) return;
    if (result != XIAOTAI_OK) {
        hold(result);
        return;
    }
    s_env->store_unconfirmed = false;
    release_step();
}

/* -------------------------------------------------------------- operations */

/* A refusal after the base was accepted, with everything taken released. */
static xiaotai_err_t refuse(xiaotai_err_t err)
{
    publish(XIAOTAI_ENVIRONMENT_IDLE, err, false, false);
    xiaotai_eventlog_error(EVENTLOG_SOURCE, "refused", 0, err);
    XIAOTAI_LOGW(ENVIRONMENT_TAG, "switch refused: 0x%x", (unsigned)err);
    return err;
}

/* Gives back what an unfinished acquisition took. A release failure here is
 * held like any other (the refusal is still returned). */
static bool unwind(void)
{
    const xiaotai_err_t err = release_gates();
    if (err == XIAOTAI_OK) return true;
    hold(err);
    return false;
}

xiaotai_err_t xiaotai_environment_set(const char *base)
{
    if (!s_env || !s_env->started) return XIAOTAI_ERR_INVALID_STATE;
    char normalized[XIAOTAI_PLATFORM_BASE_MAX + 1U];
    if (base) {
        const xiaotai_err_t err = xiaotai_platform_environment_normalize(base, normalized, sizeof(normalized));
        if (err != XIAOTAI_OK) {
            /* Override only the error priority, without taking either gate. */
            if (err == XIAOTAI_ERR_INVALID_ARG && s_config.occupied_before_invalid_base &&
                (s_env->phase != XIAOTAI_ENVIRONMENT_IDLE || xiaotai_net_provisioning_held()))
                return XIAOTAI_ERR_INVALID_STATE;
            return err;
        }
    }
    if (s_env->phase != XIAOTAI_ENVIRONMENT_IDLE) return XIAOTAI_ERR_INVALID_STATE;

    /* A switch never ends a session and never steals a quiesce or a restart. */
    if (xiaotai_activity_current(NULL) != 0 || xiaotai_quiesced() || xiaotai_restart_pending()) {
        COUNT(refused_busy);
        return refuse(XIAOTAI_ERR_BUSY);
    }

    if (base) {
        memcpy(s_env->base, normalized, sizeof(s_env->base));
    } else {
        s_env->base[0] = '\0';
    }
    s_env->reset_base = base == NULL;
    s_env->store_unconfirmed = false;
    /* ACQUIRING lasts only for this call and is not announced on its own:
     * the request ends it either in WAITING or in a refusal, and each of
     * those is one change of the status (one revision, one event). */
    lock();
    s_env->phase = XIAOTAI_ENVIRONMENT_ACQUIRING;
    s_env->error = XIAOTAI_OK;
    s_env->cancel_requested = false;
    s_env->write_attempted = false;
    unlock();

    /* Already quiesced and a pending restart were refused above, so a
     * refusal here is a guard's, returned as the guard gave it. */
    xiaotai_err_t err = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &s_env->quiesce_ticket);
    if (err != XIAOTAI_OK) {
        s_env->quiesce_ticket = 0;
        COUNT(refused_guard);
        return refuse(err);
    }
    err = xiaotai_net_maintenance_begin(&s_env->net_ticket);
    if (err != XIAOTAI_OK) {
        s_env->net_ticket = 0;
    } else {
        err = s_config.require_platform_binding_idle ?
            xiaotai_platform_gate_close_binding_idle(&s_env->gate_ticket) :
            xiaotai_platform_gate_close(&s_env->gate_ticket);
        if (err != XIAOTAI_OK) s_env->gate_ticket = 0;
    }
    if (err != XIAOTAI_OK) {
        COUNT(refused_busy);
        if (unwind()) refuse(err);
        return err;
    }

    /* An accepted switch must be able to advance. The callback runs on
     * the owner task, so it cannot see the state before WAITING is set. */
    if (!s_env->cancel_requested) {
        err = arm(0);
        if (err != XIAOTAI_OK) {
            if (unwind()) refuse(err);
            return err;
        }
    }
    COUNT(requests);
    const uint32_t limit = wait_ms();
    s_env->unlimited = limit == XIAOTAI_WAIT_FOREVER;
    s_env->deadline_us = xiaotai_time_us() + (int64_t)limit * 1000;
    (void)next_sequence();
    XIAOTAI_LOGI(ENVIRONMENT_TAG, "switch accepted (%s)", s_env->reset_base ? "built-in address" : "new base");
    publish(XIAOTAI_ENVIRONMENT_WAITING, XIAOTAI_OK, s_env->cancel_requested, false);
    if (s_env->cancel_requested) {
        /* Cancelled while the gates were being taken. */
        release_begin();
        return XIAOTAI_OK;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_environment_cancel(void)
{
    if (!s_env || !s_env->started) return XIAOTAI_ERR_INVALID_STATE;
    switch (s_env->phase) {
    case XIAOTAI_ENVIRONMENT_ACQUIRING:
        /* Taken into account once the gates are held (xiaotai_environment_set). */
        publish(XIAOTAI_ENVIRONMENT_ACQUIRING, XIAOTAI_OK, true, false);
        return XIAOTAI_OK;
    case XIAOTAI_ENVIRONMENT_WAITING:
        if (s_env->barrier_busy) {
            /* The barrier's result ends the switch. */
            publish(XIAOTAI_ENVIRONMENT_WAITING, s_env->error, true, s_env->write_attempted);
            return XIAOTAI_OK;
        }
        release_begin();
        return XIAOTAI_OK;
    case XIAOTAI_ENVIRONMENT_ERROR_HELD:
        release_begin();
        return XIAOTAI_OK;
    default:
        return XIAOTAI_ERR_INVALID_STATE;
    }
}

xiaotai_err_t xiaotai_environment_configure(const xiaotai_environment_config_t *config)
{
    if (s_env) return XIAOTAI_ERR_INVALID_STATE;
    if (config) {
        s_config = *config;
    } else {
        memset(&s_config, 0, sizeof(s_config));
    }
    return XIAOTAI_OK;
}

/* ----------------------------------------------------------------- intents */

static void on_reset_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    const xiaotai_err_t err = xiaotai_environment_set(NULL);
    if (err != XIAOTAI_OK) XIAOTAI_LOGW(ENVIRONMENT_TAG, "reset intent refused: 0x%x", (unsigned)err);
}

static void on_cancel_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    const xiaotai_err_t err = xiaotai_environment_cancel();
    if (err != XIAOTAI_OK) XIAOTAI_LOGW(ENVIRONMENT_TAG, "cancel intent refused: 0x%x", (unsigned)err);
}

/* --------------------------------------------------------------- lifecycle */

xiaotai_err_t xiaotai_module_environment_init(void)
{
    if (!atomic_load(&s_lock_ready)) {
        xiaotai_critical_init(&s_lock);
        atomic_store(&s_lock_ready, true);
    }
    /* A framework start that failed forgets its registrations but not this
     * block, so a later start reuses it. */
    if (!s_env) {
        s_env = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s_env));
        if (!s_env) return XIAOTAI_ERR_NO_MEM;
    } else {
        lock();
        memset(s_env, 0, sizeof(*s_env));
        unlock();
    }
    xiaotai_err_t err = xiaotai_intent_handle_ex(XIAOTAI_INTENT_ENVIRONMENT_RESET, on_reset_intent, NULL,
                                                XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK)
        err = xiaotai_intent_handle_ex(XIAOTAI_INTENT_ENVIRONMENT_CANCEL, on_cancel_intent, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = environment_at_register();
    return err;
}

void xiaotai_module_environment_start(void)
{
    if (!s_env) return;
    s_env->started = true;
    xiaotai_platform_environment_t environment;
    xiaotai_platform_environment(&environment);
    if (environment.boot_error != XIAOTAI_OK) {
        /* The platform stays stopped; AT+EP SET / RESET repairs the base. */
        XIAOTAI_LOGW(ENVIRONMENT_TAG, "stored environment unreadable at boot (0x%x); a switch repairs it",
                     (unsigned)environment.boot_error);
    }
}

void xiaotai_environment_reset(void)
{
    environment_barrier_wait_idle();
    if (s_env) {
        environment_state_t *state = s_env;
        s_env = NULL;
        xiaotai_mem_free(state);
    }
    memset(&s_config, 0, sizeof(s_config));
}
