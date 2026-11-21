// SPDX-License-Identifier: MIT
/* Starting over (specification 2.3) and the restart hold after stored
 * credentials (2.4). Owner task only; nothing here blocks: waits are polls on
 * framework timers.
 *
 * A begin: quiesce the system, close the platform gate where a restart
 * follows, wait for settings (and the gate) with one bound, hand
 * xiaotai_net_reprovision() to the engine, then either end the quiesce when
 * the engine finished (concurrent provisioning) or pass the quiesce to the
 * framework's restart once the engine reports restart_required. The direct
 * local policies hand off immediately, without disturbing the activity. */
#include <string.h>
#include "internal.h"
#include "xiaotai/net.h"
#include "xiaotai/port/os.h"
#include "xiaotai/preferences.h"

bool prov_restart_follows(void) { return xiaotai_net_provisioning_exclusive(); }

bool prov_direct_store(void)
{
    return XIAOTAI_PROVISIONING_LOCAL_POLICY == XIAOTAI_PROVISIONING_LOCAL_DIRECT_RESTART &&
           prov_restart_follows() && xiaotai_net_provisioning_boot();
}

static int64_t deadline_after(uint32_t ms)
{
    return ms ? xiaotai_time_us() + (int64_t)ms * 1000 : 0;
}

static bool expired(int64_t deadline_us)
{
    return deadline_us && xiaotai_time_us() >= deadline_us;
}

/* -------------------------------------------------------------- the begin */

/* Releases what a begin took, in reverse order. */
static void release_taken(void)
{
    xiaotai_timer_cancel(prov.settle_timer);
    prov.settle_timer = 0;
    if (prov.gate) {
        prov_gate_open(prov.gate);
        prov.gate = 0;
    }
    if (prov.quiesce) {
        xiaotai_quiesce_end(prov.quiesce);
        prov.quiesce = 0;
    }
    prov.begin = PROV_BEGIN_IDLE;
    prov.direct_begin = false;
    prov.deadline_us = 0;
    prov.settle_failure = XIAOTAI_OK;
    prov.restart_handed = false;
}

void prov_begin_drop(void) { release_taken(); }

static void fail(xiaotai_err_t err, const char *label)
{
    release_taken();
    prov.error = err;
    prov_log(label, err, true);
    prov_emit_failed(err, "begin");
}

static void hand(void)
{
    xiaotai_timer_cancel(prov.settle_timer);
    prov.settle_timer = 0;
    const xiaotai_err_t err = xiaotai_net_reprovision((prov.flags & XIAOTAI_PROVISIONING_FORGET) != 0);
    if (err != XIAOTAI_OK) {
        prov_count(&prov.stats.begins_failed);
        fail(err, "begin.failed");
        prov_refresh(true);
        return;
    }
    prov.begin = PROV_BEGIN_HANDED;
    prov.error = XIAOTAI_OK;
    prov_log("handed", (xiaotai_err_t)prov.flags, false);
    /* The engine may already have finished (or promised its restart). */
    xiaotai_net_provision_status_t provision;
    xiaotai_net_status_t link;
    xiaotai_net_provision_status(&provision);
    xiaotai_net_status(&link);
    prov_begin_update(&provision, &link);
    prov_refresh(true);
}

/* A setting failed or the bound ran out during the wait. */
static void settle_failed(xiaotai_err_t err, const char *label)
{
    if (!prov.exclusive_mode || XIAOTAI_PROVISIONING_BEGIN_FAILURE == XIAOTAI_PROVISIONING_BEGIN_FAILURE_CONTINUE) {
        prov_log(label, err, false);
        hand();
        return;
    }
    if (XIAOTAI_PROVISIONING_BEGIN_FAILURE == XIAOTAI_PROVISIONING_BEGIN_FAILURE_HOLD) {
        xiaotai_timer_cancel(prov.settle_timer);
        prov.settle_timer = 0;
        prov.begin = PROV_BEGIN_HELD;
        prov.error = err;
        prov_log(label, err, true);
        prov_emit_failed(err, "begin");
    } else {
        fail(err, label);
    }
    prov_refresh(true);
}

static void settle_check(void *ctx)
{
    (void)ctx;
    prov.settle_timer = 0;
    if (prov.begin != PROV_BEGIN_SETTLING) return;
    if (prov.settle_failure != XIAOTAI_OK) {
        settle_failed(prov.settle_failure, "settle.failed");
        return;
    }
    const bool written = !xiaotai_preferences_pending();
    const bool drained = !prov.gate || prov_gate_drained(prov.gate);
    if (written && drained) {
        xiaotai_err_t stored = XIAOTAI_OK;
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY && \
    XIAOTAI_PROVISIONING_BEGIN_FAILURE == XIAOTAI_PROVISIONING_BEGIN_FAILURE_HOLD
        /* The storage callback can finish before the owner dispatches its
         * failure notice. Check the packed record before committing the marker. */
        if (prov.exclusive_mode) stored = xiaotai_preferences_packed_maintenance(false);
#endif
        if (stored == XIAOTAI_OK) {
            hand();
            return;
        }
        if (stored != XIAOTAI_ERR_BUSY) {
            prov_count(&prov.stats.flush_failures);
            settle_failed(stored, "settle.failed");
            return;
        }
    }
    if (expired(prov.deadline_us)) {
        prov_count(&prov.stats.flush_timeouts);
        settle_failed(XIAOTAI_ERR_TIMEOUT, "settle.timeout");
        return;
    }
    if (xiaotai_timer_start(PROV_POLL_MS, settle_check, NULL, &prov.settle_timer) != XIAOTAI_OK) {
        /* Every timer slot is taken: give the begin up rather than wait on
         * nothing, with what was taken released. */
        prov.settle_timer = 0;
        fail(XIAOTAI_ERR_NO_MEM, "begin.failed");
        prov_refresh(true);
    }
}

static xiaotai_err_t refuse(xiaotai_err_t err)
{
    prov_count(&prov.stats.begins_refused);
    prov_log("begin.refused", err, false);
    return err;
}

xiaotai_err_t prov_begin(uint32_t flags, bool local)
{
    if (!prov.started || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (flags & ~(uint32_t)(XIAOTAI_PROVISIONING_FORGET | XIAOTAI_PROVISIONING_PREEMPT)) {
        return refuse(XIAOTAI_ERR_INVALID_ARG);
    }
    xiaotai_net_status_t link;
    xiaotai_net_status(&link);
    /* Only Wi-Fi takes credentials; a link without them cannot start over. */
    if (link.state != XIAOTAI_NET_STOPPED && link.kind != XIAOTAI_NETWORK_WIFI &&
        link.kind != XIAOTAI_NETWORK_WIFI_COPROCESSOR) {
        return refuse(XIAOTAI_ERR_NOT_SUPPORTED);
    }
    prov_refresh(true);
    (void)xiaotai_mutex_lock(&prov.lock, XIAOTAI_WAIT_FOREVER);
    const xiaotai_provisioning_phase_t phase = prov.status.phase;
    xiaotai_mutex_unlock(&prov.lock);
    if (prov.begin != PROV_BEGIN_IDLE || (phase != XIAOTAI_PROVISIONING_NONE && phase != XIAOTAI_PROVISIONING_WAITING &&
                                          phase != XIAOTAI_PROVISIONING_REJECTED) || prov.restart_handoff_failed) {
        return refuse(XIAOTAI_ERR_INVALID_STATE);
    }
    if (xiaotai_activity_current(NULL) && !(flags & XIAOTAI_PROVISIONING_PREEMPT)) {
        return refuse(XIAOTAI_ERR_BUSY);
    }
    const bool exclusive = prov_restart_follows();
    const bool direct = local && XIAOTAI_PROVISIONING_LOCAL_POLICY != XIAOTAI_PROVISIONING_LOCAL_ORDERLY;
    if (direct && ((XIAOTAI_PROVISIONING_LOCAL_POLICY == XIAOTAI_PROVISIONING_LOCAL_DIRECT_RESTART) != exclusive)) {
        return refuse(XIAOTAI_ERR_INVALID_STATE);
    }
    uint32_t quiesce = 0;
    uint32_t gate = 0;
    if (direct) {
        if (xiaotai_quiesced() || xiaotai_restart_pending()) return refuse(XIAOTAI_ERR_BUSY);
    } else {
        /* Revokes the current activity (PREEMPT) and freezes setting changes. */
        xiaotai_err_t err = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_PROVISIONING, &quiesce);
        if (err != XIAOTAI_OK) return refuse(err);
        if (exclusive && prov_gate_present()) {
            err = prov_gate_close(&gate);
            if (err != XIAOTAI_OK) {
                xiaotai_quiesce_end(quiesce);
                return refuse(XIAOTAI_ERR_BUSY);
            }
        }
    }
    prov.begin = PROV_BEGIN_SETTLING;
    prov.flags = flags;
    prov.exclusive_mode = exclusive;
    prov.direct_begin = direct;
    prov.quiesce = quiesce;
    prov.gate = gate;
    prov.settle_failure = XIAOTAI_OK;
    prov.restart_handed = false;
    prov.deadline_us = direct ? 0 : deadline_after(XIAOTAI_PROVISIONING_SETTLE_WAIT_MS);
    prov.error = XIAOTAI_OK;
    prov_count(&prov.stats.begins);
    prov_log("begin", (xiaotai_err_t)flags, false);
    if (direct) {
        hand();
        return XIAOTAI_OK;
    }
    /* The first look comes from the owner loop, after this call returned. */
    const xiaotai_err_t err = xiaotai_timer_start(0, settle_check, NULL, &prov.settle_timer);
    if (err != XIAOTAI_OK) {
        prov.settle_timer = 0;
        release_taken();
        prov_refresh(true);
        return refuse(err);
    }
    prov_refresh(true);
    return XIAOTAI_OK;
}

void prov_setting_not_saved(xiaotai_err_t error)
{
    if (prov.begin == PROV_BEGIN_SETTLING) {
        prov_count(&prov.stats.flush_failures);
        if (prov.settle_failure == XIAOTAI_OK) prov.settle_failure = error;
        /* Concurrent provisioning, or the continue option: counted only. */
        if (prov.exclusive_mode &&
            XIAOTAI_PROVISIONING_BEGIN_FAILURE != XIAOTAI_PROVISIONING_BEGIN_FAILURE_CONTINUE) {
            settle_failed(error, "settle.failed");
        } else {
            prov.settle_failure = XIAOTAI_OK;
            prov_log("settle.failed", error, false);
        }
        return;
    }
    if (prov.holding && prov.hold_settings) {
        prov_count(&prov.stats.flush_failures);
        prov_log("hold.failed", error, true);
        if (XIAOTAI_PROVISIONING_STORE_SETTINGS_HOLD && !prov.hold_failed) {
            prov.hold_failed = true;
            xiaotai_timer_cancel(prov.hold_timer);
            prov.hold_timer = 0;
            prov.error = error;
            prov_emit_failed(error, "store");
            prov_refresh(true);
        }
    }
}

void prov_begin_update(const xiaotai_net_provision_status_t *provision, const xiaotai_net_status_t *link)
{
    if (prov.begin != PROV_BEGIN_HANDED) return;
    if (prov.exclusive_mode) {
        if (provision->restart_required && !prov.restart_handed && !prov.restart_handoff_failed) {
            /* With orderly begin, the quiesce goes to the restart and the
             * gate stays closed. Direct begin already committed the network
             * data; the application can still hold for its confirmation. */
            const xiaotai_err_t err = prov.direct_begin
                ? xiaotai_restart_request_direct(XIAOTAI_QUIESCE_PROVISIONING)
                : xiaotai_restart_request(XIAOTAI_QUIESCE_PROVISIONING, prov.quiesce);
            prov_log("restart.handed", err, err != XIAOTAI_OK);
            if (err == XIAOTAI_OK) {
                prov.restart_handed = true;
                prov.quiesce = 0;
            } else if (prov.direct_begin) {
                prov_count(&prov.stats.begins_failed);
                prov_restart_refused(err);
            } else {
                prov_count(&prov.stats.begins_failed);
                fail(err, "begin.failed");
            }
            return;
        }
        if (!prov.restart_handed && !prov.restart_handoff_failed && !link->control_pending &&
            link->control_error != XIAOTAI_OK) {
            prov_count(&prov.stats.begins_failed);
            fail(link->control_error, "begin.failed");
        }
        return;
    }
    if (link->control_pending) return;
    /* Concurrent: the link is HELD and the methods are open (or the request
     * failed); either way the device may run again. */
    const xiaotai_err_t result = link->control_error;
    if (prov.quiesce) xiaotai_quiesce_end(prov.quiesce);
    prov.quiesce = 0;
    prov.begin = PROV_BEGIN_IDLE;
    prov.error = result;
    if (result != XIAOTAI_OK) {
        prov_count(&prov.stats.begins_failed);
        prov_log("begin.failed", result, true);
        prov_emit_failed(result, "begin");
    }
}

/* ------------------------------------------------- the hold after storing */

void prov_hold_take(bool settings)
{
    if (prov.holding) return;
    prov.holding = true;
    prov.hold_settings = settings;
    prov.hold_failed = false;
    prov.hold_deadline_us = 0;
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
    if (++prov.hold_epoch == 0) ++prov.hold_epoch;
#endif
    xiaotai_restart_hold();
}

void prov_hold_release(void)
{
    xiaotai_timer_cancel(prov.hold_timer);
    prov.hold_timer = 0;
    if (!prov.holding) return;
    prov.holding = false;
    prov.hold_settings = false;
    prov.hold_failed = false;
    prov.hold_deadline_us = 0;
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
    prov.hold_check_queued = false;
#endif
    xiaotai_restart_release();
    prov_log("hold.released", XIAOTAI_OK, false);
}

#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
bool prov_hold_packed(void)
{
    return prov.holding && prov.hold_settings && xiaotai_net_provisioning_boot();
}

static void hold_packed_failed(xiaotai_err_t error)
{
    prov.hold_failed = true;
    prov.error = error;
    prov_log("hold.packed.failed", error, true);
    prov_emit_failed(error, "store");
    prov_refresh(true);
}

xiaotai_err_t prov_hold_retry_packed(void)
{
    if (!prov.hold_failed || !xiaotai_restart_pending() || !xiaotai_quiesced()) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = xiaotai_preferences_packed_maintenance(true);
    if (err == XIAOTAI_OK) {
        prov.hold_failed = false;
        prov.error = XIAOTAI_OK;
        prov_hold_wait();
    }
    return err;
}
#endif

static void hold_check(void *ctx)
{
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
    if ((uintptr_t)ctx != prov.hold_epoch) return;
#endif
    prov.hold_timer = 0;
    prov.hold_check_queued = false;
    if (!prov.holding || prov.hold_failed) return;
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
    if (prov_hold_packed()) {
        const xiaotai_err_t state = xiaotai_quiesced() && xiaotai_restart_pending() ?
            xiaotai_preferences_packed_maintenance(false) : XIAOTAI_ERR_BUSY;
        if (state == XIAOTAI_OK) {
            prov_hold_release();
            prov_refresh(true);
        } else if (state != XIAOTAI_ERR_BUSY) {
            hold_packed_failed(state);
        } else {
            if (expired(prov.hold_deadline_us)) {
                prov_count(&prov.stats.flush_timeouts);
                prov_log("hold.packed.timeout", XIAOTAI_ERR_TIMEOUT, true);
                prov.hold_deadline_us = 0;
            }
            const xiaotai_err_t err = xiaotai_timer_start(PROV_POLL_MS, hold_check, ctx, &prov.hold_timer);
            if (err != XIAOTAI_OK) hold_packed_failed(err);
        }
        return;
    }
#endif
    if (!prov.hold_settings || !xiaotai_preferences_pending()) {
        prov_hold_release();
        prov_refresh(true);
        return;
    }
    if (expired(prov.hold_deadline_us)) {
        prov_count(&prov.stats.flush_timeouts);
        prov_log("hold.timeout", XIAOTAI_ERR_TIMEOUT, false);
        prov_hold_release();
        prov_refresh(true);
        return;
    }
    if (xiaotai_timer_start(PROV_POLL_MS, hold_check, ctx, &prov.hold_timer) != XIAOTAI_OK) {
        /* No timer: never keep the device from restarting on nothing. */
        prov.hold_timer = 0;
        prov_hold_release();
        prov_refresh(true);
    }
}

static void hold_check_job(void *ctx) { hold_check(ctx); }

void prov_hold_wait(void)
{
    if (!prov.holding || prov.hold_failed || prov.hold_check_queued || prov.hold_timer) return;
    prov.hold_deadline_us = deadline_after(XIAOTAI_PROVISIONING_SETTLE_WAIT_MS);
    void *ctx = NULL;
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
    ctx = (void *)(uintptr_t)prov.hold_epoch;
#endif
    /* Behind every event emitted so far, so DONE is handled first. */
    if (xiaotai_run(hold_check_job, ctx) == XIAOTAI_OK) {
        prov.hold_check_queued = true;
    } else if (xiaotai_timer_start(PROV_POLL_MS, hold_check, ctx, &prov.hold_timer) != XIAOTAI_OK) {
        prov.hold_timer = 0;
#if XIAOTAI_PROVISIONING_STORE_PACKED_RETRY
        if (prov_hold_packed()) {
            hold_packed_failed(XIAOTAI_ERR_NO_MEM);
            return;
        }
#endif
        prov_hold_release();
    }
}

bool prov_hold_rearm(void)
{
    if (!prov.holding || !prov.hold_failed) return false;
    prov.hold_failed = false;
    prov.error = XIAOTAI_OK;
    prov_hold_wait();
    return true;
}

/* ------------------------------------------ store again on a fixed period */

static void store_retry(void *ctx)
{
    (void)ctx;
    prov.store_retry_timer = 0;
    xiaotai_net_provision_status_t provision;
    xiaotai_net_provision_status(&provision);
    if (provision.state != XIAOTAI_NET_PROVISION_STORE_FAILED) return;
    const xiaotai_err_t err = xiaotai_net_provision_store_retry();
    prov_log("store.retry", err, err != XIAOTAI_OK);
    if (err == XIAOTAI_OK) {
        prov_count(&prov.stats.retries);
    } else {
        /* The engine refused this time (a request pending): try again. */
        prov_store_retry_schedule();
    }
}

void prov_store_retry_schedule(void)
{
    if (XIAOTAI_PROVISIONING_STORE_RETRY_MS == 0 || prov.store_retry_timer) return;
    if (xiaotai_timer_start(XIAOTAI_PROVISIONING_STORE_RETRY_MS, store_retry, NULL, &prov.store_retry_timer) !=
        XIAOTAI_OK) {
        prov.store_retry_timer = 0;
        prov_log("store.retry", XIAOTAI_ERR_NO_MEM, true);
    }
}

void prov_begin_reset(void)
{
    /* Host tests reset after the framework stopped: its timers, quiesce and
     * holds are gone with it, so only the module's own fields are cleared. */
    prov.begin = PROV_BEGIN_IDLE;
    prov.settle_timer = prov.hold_timer = prov.store_retry_timer = prov.restart_notice_timer = 0;
    prov.quiesce = prov.gate = 0;
    prov.holding = prov.hold_failed = prov.hold_check_queued = false;
}
