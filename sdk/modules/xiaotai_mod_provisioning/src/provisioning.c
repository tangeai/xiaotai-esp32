// SPDX-License-Identifier: MIT
/* Provisioning module: the snapshot a setup page reads, the derived phase, the
 * module events made from the network engine's, the intents, cancel, retry
 * and the lifecycle. The begin sequence and the restart hold are in
 * provisioning_begin.c, the console verbs and diagnostics in
 * provisioning_at.c. Everything runs on the framework owner task except the
 * [any] functions, which copy under prov.lock. */
#include <string.h>
#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/net.h"

prov_state_t prov;

/* ------------------------------------------------------------------ helpers */

void prov_count(uint32_t *counter)
{
    (void)xiaotai_mutex_lock(&prov.lock, XIAOTAI_WAIT_FOREVER);
    ++*counter;
    xiaotai_mutex_unlock(&prov.lock);
}

void prov_log(const char *label, xiaotai_err_t value, bool failure)
{
    const xiaotai_eventlog_event_t event = {
        .source = PROV_SOURCE, .label = label, .value = value,
        .level = (uint8_t)(failure ? XIAOTAI_EVENTLOG_ERROR : XIAOTAI_EVENTLOG_INFO),
        .kind = (uint8_t)XIAOTAI_EVENTLOG_ERR,
    };
    xiaotai_eventlog_emit(&event);
}

xiaotai_err_t prov_emit_failed(xiaotai_err_t error, const char *what)
{
    xiaotai_args_t args = {.value = error};
    strncpy(args.text, what, sizeof(args.text) - 1);
    return xiaotai_event_emit(XIAOTAI_EVENT_PROVISIONING_FAILED, &args, NULL, 0);
}

static void restart_failure_notice(void *ctx)
{
    (void)ctx;
    prov.restart_notice_timer = 0;
    const xiaotai_err_t err = prov_emit_failed(prov.error, "restart");
    if (err == XIAOTAI_OK) return;
    /* The owner queue may be full. Retain the durable-failure indication in
     * the snapshot even if the timer table cannot also retain the notice. */
    if (xiaotai_timer_start(PROV_POLL_MS, restart_failure_notice, NULL, &prov.restart_notice_timer) != XIAOTAI_OK) {
        prov_log("restart.notice", err, true);
    }
}

void prov_restart_refused(xiaotai_err_t error)
{
    prov.restart_handoff_failed = true;
    prov.error = error;
    restart_failure_notice(NULL);
}

/* --------------------------------------------------------------- the phase */

/* Rows of the derivation table, first match wins (specification 2.2). A begin
 * that reached the engine still counts as STARTING while the engine works on
 * it, and where a restart follows until the restart is announced: the
 * engine's methods open only in the provisioning boot. A request somebody
 * else made (a retry, a close) does not make this module's phase STARTING. */
static xiaotai_provisioning_phase_t derive(const xiaotai_net_provision_status_t *provision,
                                           const xiaotai_net_status_t *link)
{
    if (prov.restarting) return XIAOTAI_PROVISIONING_RESTARTING;
    if (prov.begin == PROV_BEGIN_SETTLING || prov.begin == PROV_BEGIN_HELD) return XIAOTAI_PROVISIONING_STARTING;
    if (prov.begin == PROV_BEGIN_HANDED && (link->control_pending || prov.exclusive_mode)) {
        return XIAOTAI_PROVISIONING_STARTING;
    }
    switch (provision->state) {
    case XIAOTAI_NET_PROVISION_OPENING: return XIAOTAI_PROVISIONING_STARTING;
    case XIAOTAI_NET_PROVISION_OPEN: return XIAOTAI_PROVISIONING_WAITING;
    case XIAOTAI_NET_PROVISION_VERIFYING: return XIAOTAI_PROVISIONING_VERIFYING;
    case XIAOTAI_NET_PROVISION_REJECTED: return XIAOTAI_PROVISIONING_REJECTED;
    case XIAOTAI_NET_PROVISION_STORING: return XIAOTAI_PROVISIONING_STORING;
    case XIAOTAI_NET_PROVISION_STORE_FAILED: return XIAOTAI_PROVISIONING_STORE_FAILED;
    case XIAOTAI_NET_PROVISION_DONE: return XIAOTAI_PROVISIONING_DONE;
    case XIAOTAI_NET_PROVISION_CLOSING:
    case XIAOTAI_NET_PROVISION_CLOSED:
    default: return XIAOTAI_PROVISIONING_NONE;
    }
}

void prov_refresh(bool emit)
{
    /* One copy on the owner's stack (about 500 bytes, inside the handler
     * budget); the published one is replaced only when something changed. */
    xiaotai_provisioning_status_t next;
    memset(&next, 0, sizeof(next));
    xiaotai_net_provision_status(&next.provision);
    xiaotai_net_status(&next.link);
    next.phase = derive(&next.provision, &next.link);
    next.exclusive = xiaotai_net_provisioning_boot();
    next.restart_pending = prov.restart_handoff_failed ? xiaotai_restart_pending()
        : next.provision.restart_required || (prov.begin == PROV_BEGIN_HANDED && prov.exclusive_mode);
    next.holding_restart = prov.holding;
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
    next.packed_retry_required = next.phase == XIAOTAI_PROVISIONING_DONE && prov_hold_packed() && prov.hold_failed;
#endif
    next.error = prov.error;

    (void)xiaotai_mutex_lock(&prov.lock, XIAOTAI_WAIT_FOREVER);
    const xiaotai_provisioning_status_t *old = &prov.status;
    /* The engine's revisions stand for every field of its structs, so padding
     * never decides whether something changed. */
    const bool changed = old->phase != next.phase || old->exclusive != next.exclusive ||
                         old->restart_pending != next.restart_pending ||
                         old->holding_restart != next.holding_restart || old->error != next.error ||
                         old->packed_retry_required != next.packed_retry_required ||
                         old->provision.revision != next.provision.revision ||
                         old->provision.state != next.provision.state ||
                         old->link.revision != next.link.revision || old->link.state != next.link.state;
    next.revision = old->revision + (changed ? 1U : 0U);
    if (changed) prov.status = next;
    xiaotai_mutex_unlock(&prov.lock);

    if (changed && emit) {
        const xiaotai_args_t args = {.value = (int32_t)next.phase};
        (void)xiaotai_event_emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, &args, &next, sizeof(next));
    }
}

/* ------------------------------------------------------------ engine events */

static void emit_opened(const xiaotai_net_provision_status_t *provision)
{
    xiaotai_args_t args = {.value = (int32_t)provision->reason};
    if (provision->methods & XIAOTAI_PROVISION_PORTAL) {
        strncpy(args.text, provision->ap_ssid, sizeof(args.text) - 1);
    }
    (void)xiaotai_event_emit(XIAOTAI_EVENT_PROVISIONING_OPENED, &args, NULL, 0);
}

static bool methods_closed(xiaotai_net_provision_state_t state)
{
    return state == XIAOTAI_NET_PROVISION_CLOSED || state == XIAOTAI_NET_PROVISION_OPENING ||
           state == XIAOTAI_NET_PROVISION_CLOSING;
}

/* One transition of the engine's provisioning, as the event announced it
 * (its value and data are the state at that moment, so a quick sequence is
 * seen step by step even when the snapshot has moved on). */
static void transition(xiaotai_net_provision_state_t state, const xiaotai_net_provision_status_t *at)
{
    const xiaotai_net_provision_state_t previous = prov.seen_state ? prov.last_state : XIAOTAI_NET_PROVISION_CLOSED;
    prov.last_state = state;
    prov.seen_state = true;
    switch (state) {
    case XIAOTAI_NET_PROVISION_OPEN:
        if (methods_closed(previous)) emit_opened(at);
        break;
    case XIAOTAI_NET_PROVISION_REJECTED:
        prov_count(&prov.stats.rejected);
        prov_log("rejected", at->error, false);
        prov_emit_failed(at->error, "verify");
        break;
    case XIAOTAI_NET_PROVISION_STORING:
        prov_hold_take(!prov_direct_store());
        break;
    case XIAOTAI_NET_PROVISION_STORE_FAILED:
        prov_count(&prov.stats.store_failures);
        prov_log("store.failed", at->error, true);
        /* No restart can follow before DONE; a hold kept here would block
         * the restart that closing provisioning may ask for. */
        prov_hold_release();
        if (prov.store_observer) prov.store_observer(at->error, false, prov.store_observer_ctx);
        prov_emit_failed(at->error, "store");
        prov_store_retry_schedule();
        break;
    case XIAOTAI_NET_PROVISION_DONE: {
        if (previous == XIAOTAI_NET_PROVISION_DONE) break;
        prov_hold_take(!prov_direct_store());
        const bool restart = at->restart_required && !prov.restart_handoff_failed;
        prov_count(&prov.stats.stored);
        prov_log("stored", restart ? 1 : 0, false);
        if (prov.store_observer) prov.store_observer(XIAOTAI_OK, restart, prov.store_observer_ctx);
        const xiaotai_args_t args = {.value = restart ? 1 : 0};
        (void)xiaotai_event_emit(XIAOTAI_EVENT_PROVISIONING_DONE, &args, NULL, 0);
        /* Queued behind the event: the handlers of DONE run before the
         * module may release its hold. */
        prov_hold_wait();
        break;
    }
    case XIAOTAI_NET_PROVISION_CLOSING:
    case XIAOTAI_NET_PROVISION_CLOSED:
        if (previous != XIAOTAI_NET_PROVISION_DONE) prov_hold_release();
        break;
    default:
        break;
    }
}

static void on_net(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!prov.started) return;
    if (strcmp(event->name, XIAOTAI_EVENT_NET_PROVISION) == 0) {
        xiaotai_net_provision_status_t at;
        if (event->data && event->size == sizeof(at)) {
            memcpy(&at, event->data, sizeof(at));
        } else {
            xiaotai_net_provision_status(&at);
        }
        /* A durable DONE still needs a framework acceptance. Hold through
         * event delivery so an application can take its confirmation hold. */
        if (event->args.value == XIAOTAI_NET_PROVISION_DONE && at.restart_required &&
            prov_direct_store() && !prov.restart_handoff_failed && !xiaotai_restart_pending()) {
            prov_hold_take(false);
            const xiaotai_err_t direct_err = xiaotai_restart_request_direct(XIAOTAI_QUIESCE_PROVISIONING);
            prov_log("store.restart", direct_err, direct_err != XIAOTAI_OK);
            if (direct_err != XIAOTAI_OK) prov_restart_refused(direct_err);
        }
        transition((xiaotai_net_provision_state_t)event->args.value, &at);
    }
    xiaotai_net_provision_status_t provision;
    xiaotai_net_status_t link;
    xiaotai_net_provision_status(&provision);
    xiaotai_net_status(&link);
    prov_begin_update(&provision, &link);
    prov_refresh(true);
}

static void on_restarting(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    if (!prov.started) return;
    prov.restarting = true;
    prov_refresh(true);
}

static void on_setting_not_saved(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!prov.started) return;
    prov_setting_not_saved(event->args.value ? (xiaotai_err_t)event->args.value : XIAOTAI_FAIL);
}

/* ----------------------------------------------------------------- intents */

static void report(xiaotai_err_t err, const char *what)
{
    if (err != XIAOTAI_OK) prov_emit_failed(err, what);
}

static void on_intent_begin(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    /* PREEMPT only: the stored credentials are kept, so anyone may post it. */
    const uint32_t value = (uint32_t)args->value;
    if (value & ~(uint32_t)XIAOTAI_PROVISIONING_PREEMPT) {
        prov_count(&prov.stats.begins_refused);
        report(XIAOTAI_ERR_INVALID_ARG, "begin");
        return;
    }
    report(prov_begin(value, false), "begin");
}

static void on_intent_forget(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    const uint32_t preempt = (uint32_t)args->value & (uint32_t)XIAOTAI_PROVISIONING_PREEMPT;
    report(xiaotai_provisioning_begin(XIAOTAI_PROVISIONING_FORGET | preempt), "begin");
}

static void on_intent_cancel(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    (void)xiaotai_provisioning_cancel();
}

static void on_intent_retry(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    (void)xiaotai_provisioning_retry();
}

/* --------------------------------------------------------------- operations */

xiaotai_err_t xiaotai_provisioning_begin(uint32_t flags)
{
    return prov_begin(flags, true);
}

bool xiaotai_provisioning_begin_cancellable(void)
{
    return xiaotai_on_owner() && prov.started &&
           (prov.begin == PROV_BEGIN_SETTLING || prov.begin == PROV_BEGIN_HELD);
}

static xiaotai_provisioning_phase_t phase_now(void)
{
    (void)xiaotai_mutex_lock(&prov.lock, XIAOTAI_WAIT_FOREVER);
    const xiaotai_provisioning_phase_t phase = prov.status.phase;
    xiaotai_mutex_unlock(&prov.lock);
    return phase;
}

xiaotai_err_t xiaotai_provisioning_cancel(void)
{
    if (!prov.started || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    prov_refresh(true);
    if (xiaotai_provisioning_begin_cancellable()) {
        prov_begin_drop();
        prov.error = XIAOTAI_OK;
        prov_count(&prov.stats.cancels);
        prov_log("cancel", XIAOTAI_OK, false);
        prov_refresh(true);
        return XIAOTAI_OK;
    }
    const xiaotai_provisioning_phase_t phase = phase_now();
    /* A begin whose restart is already requested cannot be taken back. */
    if (phase == XIAOTAI_PROVISIONING_DONE || phase == XIAOTAI_PROVISIONING_RESTARTING ||
        prov.restart_handed || prov.restart_handoff_failed) {
        return XIAOTAI_ERR_INVALID_STATE;
    }
    const xiaotai_err_t err = xiaotai_net_provision_close();
    prov.error = err;
    if (err == XIAOTAI_OK) prov_count(&prov.stats.cancels);
    prov_log("cancel", err, err != XIAOTAI_OK);
    prov_refresh(true);
    return err;
}

xiaotai_err_t xiaotai_provisioning_retry(void)
{
    if (!prov.started || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    /* A begin on its way to the engine owns the link until it is handed. */
    if (prov.begin == PROV_BEGIN_SETTLING || prov.begin == PROV_BEGIN_HELD || prov.restart_handoff_failed) {
        return XIAOTAI_ERR_INVALID_STATE;
    }
    xiaotai_err_t err;
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
    if (prov_hold_packed()) {
        err = prov_hold_retry_packed();
        if (err == XIAOTAI_OK) prov_count(&prov.stats.retries);
        prov_log("retry", err, err != XIAOTAI_OK);
        prov_refresh(true);
        return err;
    }
#endif
    if (prov_hold_rearm()) {
        err = XIAOTAI_OK;
    } else {
        xiaotai_net_provision_status_t provision;
        xiaotai_net_status_t link;
        xiaotai_net_provision_status(&provision);
        xiaotai_net_status(&link);
        if (provision.state == XIAOTAI_NET_PROVISION_STORE_FAILED) {
            err = xiaotai_net_provision_store_retry();
        } else if (link.state == XIAOTAI_NET_BLOCKED || link.state == XIAOTAI_NET_WAITING ||
                   link.state == XIAOTAI_NET_HELD) {
            err = xiaotai_net_retry();
        } else {
            return XIAOTAI_ERR_INVALID_STATE;
        }
    }
    prov.error = err;
    if (err == XIAOTAI_OK) prov_count(&prov.stats.retries);
    prov_log("retry", err, err != XIAOTAI_OK);
    prov_refresh(true);
    return err;
}

xiaotai_err_t xiaotai_provisioning_scan(void)
{
    return xiaotai_net_scan();
}

size_t xiaotai_provisioning_networks(xiaotai_net_network_t *networks, size_t capacity,
                                     xiaotai_net_scan_status_t *status)
{
    return xiaotai_net_networks(networks, capacity, status);
}

void xiaotai_provisioning_restart_hold(void)
{
    if (prov.initialized) prov_count(&prov.stats.restart_holds);
    xiaotai_restart_hold();
}

void xiaotai_provisioning_restart_release(void)
{
    xiaotai_restart_release();
}

/* ------------------------------------------------------------------- status */

void xiaotai_provisioning_status(xiaotai_provisioning_status_t *status)
{
    if (!status) return;
    if (!prov.initialized) {
        memset(status, 0, sizeof(*status));
        return;
    }
    (void)xiaotai_mutex_lock(&prov.lock, XIAOTAI_WAIT_FOREVER);
    *status = prov.status;
    xiaotai_mutex_unlock(&prov.lock);
}

bool xiaotai_provisioning_active(void)
{
    if (!prov.initialized) return false;
    return phase_now() != XIAOTAI_PROVISIONING_NONE;
}

xiaotai_err_t xiaotai_provisioning_observe_store(xiaotai_provisioning_store_fn_t fn, void *ctx)
{
    if (!fn) return XIAOTAI_ERR_INVALID_ARG;
    if (!prov.initialized || prov.started || prov.store_observer) return XIAOTAI_ERR_INVALID_STATE;
    prov.store_observer = fn;
    prov.store_observer_ctx = ctx;
    return XIAOTAI_OK;
}

void xiaotai_provisioning_stats(xiaotai_provisioning_stats_t *stats)
{
    if (!stats) return;
    if (!prov.initialized) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    (void)xiaotai_mutex_lock(&prov.lock, XIAOTAI_WAIT_FOREVER);
    *stats = prov.stats;
    xiaotai_mutex_unlock(&prov.lock);
}

/* ---------------------------------------------------------------- lifecycle */

xiaotai_err_t xiaotai_module_provisioning_init(void)
{
    if (prov.initialized) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = xiaotai_mutex_init(&prov.lock);
    if (err != XIAOTAI_OK) return err;
    prov.initialized = true;
    static const xiaotai_subscriber_descriptor_t engine_events[] = {
        {XIAOTAI_EVENT_NET_STATE, on_net, NULL},
        {XIAOTAI_EVENT_NET_PROVISION, on_net, NULL},
        {XIAOTAI_EVENT_NET_UP, on_net, NULL},
        {XIAOTAI_EVENT_NET_DOWN, on_net, NULL},
    };
    for (size_t i = 0; i < sizeof(engine_events) / sizeof(engine_events[0]) && err == XIAOTAI_OK; ++i) {
        err = xiaotai_event_subscribe_static(&engine_events[i]);
    }
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_RESTARTING, on_restarting, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_SETTING_NOT_SAVED, on_setting_not_saved,
                                                           NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_PROVISIONING_BEGIN, on_intent_begin, NULL);
    if (err == XIAOTAI_OK) {
        err = xiaotai_intent_handle_ex(XIAOTAI_INTENT_PROVISIONING_FORGET, on_intent_forget, NULL,
                                       XIAOTAI_INTENT_LOCAL);
    }
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_PROVISIONING_CANCEL, on_intent_cancel, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_PROVISIONING_RETRY, on_intent_retry, NULL);
    if (err == XIAOTAI_OK) err = prov_at_register();
    if (err == XIAOTAI_OK) err = xiaotai_diag_register(PROV_SOURCE, prov_diag);
    return err;
}

void xiaotai_module_provisioning_start(void)
{
    if (!prov.initialized || prov.started) return;
    prov.started = true;
    xiaotai_net_provision_status_t provision;
    xiaotai_net_provision_status(&provision);
    prov.last_state = provision.state;
    prov.seen_state = true;
    prov_refresh(false);
    /* No credentials at boot, or a provisioning boot: the methods are open
     * before anybody could hear the engine's own event. */
    if (provision.state == XIAOTAI_NET_PROVISION_OPEN || provision.state == XIAOTAI_NET_PROVISION_REJECTED) {
        emit_opened(&provision);
    }
}

void xiaotai_provisioning_reset(void)
{
    prov_begin_reset();
    if (prov.initialized) xiaotai_mutex_deinit(&prov.lock);
    memset(&prov, 0, sizeof(prov));
}
