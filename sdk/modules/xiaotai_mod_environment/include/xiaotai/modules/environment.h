// SPDX-License-Identifier: MIT
#pragma once
/* Environment module: switches the service environment the device talks to.
 *
 * The environment is the base address the discovery URL is made from. The
 * platform core (xiaotai/platform.h) validates, stores and reads it; this
 * module is the coordinator that makes a switch safe: nothing may be in
 * flight, nothing may still be on its way to storage, and nothing new may
 * begin between the decision and the restart. It is the only caller of
 * xiaotai_platform_environment_store().
 *
 * One switch at a time. Its steps, all driven from the framework owner task
 * and none of them blocking it:
 *   1. the base is normalized; no foreground activity is current, the system
 *      is not quiesced and no restart is pending;
 *   2. xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT), which first asks
 *      every quiesce guard (xiaotai_quiesce_guard_register in
 *      xiaotai/framework.h); a refusal ends the request here (see
 *      xiaotai_environment_set). While the system is quiesced the framework
 *      refuses setting changes (xiaotai_setting_set_*), so no new preference
 *      write begins after this point;
 *   3. xiaotai_net_maintenance_begin(), then close the platform gate
 *      (requiring idle platform binding if selected by the product);
 *   4. wait (bounded) until the platform gate is drained and no setting change
 *      is pending, then pass a storage barrier (preferences flush and key-value
 *      flush, on a short-lived task of this module);
 *   5. the point of no return: a cancel is honoured before it, never after;
 *   6. store and read back (xiaotai_platform_environment_store);
 *   7. success: xiaotai_net_maintenance_restart(), then the quiesce of step 2
 *      is handed to xiaotai_restart_request(XIAOTAI_QUIESCE_ENVIRONMENT, its
 *      ticket), so nothing begins between the store and the restart (restart
 *      holds apply); the device restarts with its network credentials and
 *      identity untouched. Failure: everything is released in reverse order
 *      and the device keeps running; nothing is retried or rolled back
 *      (environment.unconfirmed tells whether the stored base is known).
 *      When the store ended with XIAOTAI_ERR_TIMEOUT
 *      the accepted write may still be running on the storage worker: the
 *      release then first passes a key-value barrier (xiaotai_kv_flush within
 *      release_ms) and enters ERROR_HELD, nothing released, when that fails.
 * A release that cannot complete leaves the switch in ERROR_HELD with the
 * gates it could not open still held; xiaotai_environment_cancel() tries the
 * release again (the barrier first, when it is the step that failed).
 *
 * Vetoes. A module that owns something a switch would lose or orphan (a
 * recognition record that was not archived, a binding attempt that runs)
 * registers a framework quiesce guard and refuses when the reason is
 * XIAOTAI_QUIESCE_ENVIRONMENT (compared with strcmp: the reason is a text).
 * This module has no guard table of its own, so no module depends on it in
 * order to veto, and it stays optional for every product.
 *
 * Manifest name "environment". Depends on the engines net, platform, identity
 * (preferences), eventlog and at. No hardware need beyond what those have.
 *
 * Task tags as in xiaotai/framework.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/platform.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- status */

typedef enum {
    XIAOTAI_ENVIRONMENT_IDLE = 0,
    XIAOTAI_ENVIRONMENT_ACQUIRING,   /* steps 2 and 3 (quiesce guards, gates) */
    XIAOTAI_ENVIRONMENT_WAITING,     /* step 4 */
    XIAOTAI_ENVIRONMENT_WRITING,     /* step 6; cannot be cancelled */
    XIAOTAI_ENVIRONMENT_RESTARTING,  /* step 7, stored; cannot be cancelled */
    XIAOTAI_ENVIRONMENT_RELEASING,   /* storage barrier after a timed-out store, then
                                      * giving the gates back; cannot be cancelled */
    XIAOTAI_ENVIRONMENT_ERROR_HELD,  /* a release step failed; cancel releases again */
} xiaotai_environment_phase_t;

/* Outcome of a switch, carried by XIAOTAI_EVENT_ENVIRONMENT_RESULT. */
typedef enum {
    XIAOTAI_ENVIRONMENT_SAVED = 0,   /* stored and read back; the restart follows */
    XIAOTAI_ENVIRONMENT_FAILED,      /* status.error says why; nothing restarts */
    XIAOTAI_ENVIRONMENT_CANCELLED,   /* cancelled before anything was written */
} xiaotai_environment_result_t;

typedef struct {
    xiaotai_environment_phase_t phase;
    uint32_t revision;        /* increases whenever any field below changes */
    /* Error of the switch in progress or of the last request that ended;
     * XIAOTAI_OK when it was saved or cancelled, and before the first one.
     * A request refused after its base was accepted (by the activity,
     * quiesce or restart check, by a quiesce guard, or at step 3, with
     * everything taken released again) also ends here with that result (and
     * write_attempted false), as a switch that failed before it wrote. A
     * malformed base, and a request while a switch is in progress or before
     * the module started, leave the status alone: they are only returned. */
    xiaotai_err_t error;
    bool cancel_requested;    /* a cancel was accepted and has not taken effect yet */
    /* The store was started for the switch in progress or the last one: the
     * stored base may have changed even when error is not XIAOTAI_OK (a
     * storage timeout does not mean "not written"). */
    bool write_attempted;
    /* Active, stored and next discovery addresses and the boot-time read
     * error, as the platform core holds them (never read from storage here).
     * environment.unconfirmed: a store failed after it reached storage (a
     * timeout included), so saved and next are not known and are shown as
     * unknown ("?" in AT+EP). It stays set after the release barrier passed
     * (the write has settled, but nobody read it back) until a later store is
     * confirmed or the device restarts. */
    xiaotai_platform_environment_t environment;
} xiaotai_environment_status_t;

/* [any] Snapshot. */
void xiaotai_environment_status(xiaotai_environment_status_t *status);

/* ---------------------------------------------------------------- operations */

/* [owner] Begin a switch to base, or back to the product's built-in discovery
 * address with NULL. base is copied.
 *
 * XIAOTAI_OK                 accepted; progress and outcome arrive as events
 * XIAOTAI_ERR_INVALID_ARG    base is not an acceptable address
 *                            (xiaotai_platform_environment_normalize)
 * XIAOTAI_ERR_INVALID_STATE  a switch is in progress (any phase but IDLE), or
 *                            the module has not started
 * XIAOTAI_ERR_BUSY           a foreground activity is current (a switch never
 *                            ends a session), the system is already quiesced,
 *                            a restart is pending, or the product requires
 *                            idle platform binding and it is busy
 * A quiesce guard's refusal is returned exactly as the guard gave it
 * (XIAOTAI_ERR_BUSY, XIAOTAI_ERR_INVALID_STATE, a storage error, ...; counted
 * as refused_guard), never translated. The results of step 3
 * (network maintenance and platform gate close) are returned
 * as they are; everything taken so far has been released by then. Every
 * refusal after the base was accepted and while no switch was in progress
 * (BUSY, a guard's result, step 3) is also recorded in status.error and
 * reported by AT+EP (error=), and XIAOTAI_EVENT_ENVIRONMENT_CHANGED is
 * emitted; no XIAOTAI_EVENT_ENVIRONMENT_RESULT follows (no switch began). */
xiaotai_err_t xiaotai_environment_set(const char *base);

/* [owner] Cancel. Accepted in ACQUIRING and WAITING (the switch ends with
 * XIAOTAI_ENVIRONMENT_CANCELLED once the gates are released) and in
 * ERROR_HELD (the release is tried again). XIAOTAI_ERR_INVALID_STATE in every
 * other phase: a store that began cannot be undone. */
xiaotai_err_t xiaotai_environment_cancel(void);

/* ------------------------------------------------------------- configuration */

/* Every field: 0 selects the default. wait_ms XIAOTAI_WAIT_FOREVER
 * (xiaotai/port/os.h): step 4 has no time limit (a cancel ends it); each
 * storage barrier is then bounded on its own and tried again after a timeout. */
typedef struct {
    uint32_t wait_ms;      /* longest step 4, the storage barrier included; default 10000 */
    uint32_t release_ms;   /* longest release; default 5000 */
    bool require_platform_binding_idle; /* refuse a switch during active platform binding reconciliation */
    bool occupied_before_invalid_base; /* an already held environment or network operation wins over a bad base */
} xiaotai_environment_config_t;

/* [glue] Before xiaotai_module_environment_init(), from the product glue; copied.
 * NULL restores the defaults. XIAOTAI_ERR_INVALID_STATE once initialized. */
xiaotai_err_t xiaotai_environment_configure(const xiaotai_environment_config_t *config);

/* -------------------------------------------------------------------- events */

/* The status changed. args.value: xiaotai_environment_phase_t; data: a copy of
 * xiaotai_environment_status_t, valid during the handler. */
#define XIAOTAI_EVENT_ENVIRONMENT_CHANGED "environment.changed"
/* A switch ended. args.value: xiaotai_environment_result_t; data: a copy of
 * xiaotai_environment_status_t (its error field says why it failed). After
 * SAVED the framework restarts the device (xiaotai_restart_request with the
 * reason XIAOTAI_QUIESCE_ENVIRONMENT: XIAOTAI_EVENT_RESTARTING follows once
 * every restart hold was released). */
#define XIAOTAI_EVENT_ENVIRONMENT_RESULT "environment.result"

/* ------------------------------------------------------------------- intents */

/* xiaotai_environment_set(NULL): back to the built-in address. No arguments.
 * (Setting an address has no intent: an address does not fit intent
 * arguments. Use the function or AT+EP SET.) */
#define XIAOTAI_INTENT_ENVIRONMENT_RESET "environment.reset"
/* xiaotai_environment_cancel(). No arguments. */
#define XIAOTAI_INTENT_ENVIRONMENT_CANCEL "environment.cancel"

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t requests;         /* accepted */
    uint32_t refused_guard;    /* a quiesce guard refused (step 2) */
    uint32_t refused_busy;     /* activity current, quiesced, restart pending, or a gate not available */
    uint32_t saved;
    uint32_t failed;
    uint32_t cancelled;
    uint32_t wait_timeouts;    /* step 4 ran out of time */
    uint32_t store_timeouts;   /* the store ended with XIAOTAI_ERR_TIMEOUT: a release barrier ran */
    uint32_t release_failures; /* entered ERROR_HELD */
} xiaotai_environment_stats_t;

/* [any] */
void xiaotai_environment_stats(xiaotai_environment_stats_t *stats);

/* ----------------------------------------------------------------- lifecycle */

/* Called by the framework through the generated tables. init: registers the
 * command AT+EP, the two intents and the diagnostics provider "environment".
 *
 * AT+EP (xiaotai/at_command.h conventions), all lines from one snapshot:
 *   AT+EP            +EP: active=<url|-> phase=<n> error=<n> boot_error=<n>
 *                    cancel=<0|1> write_attempted=<0|1>
 *                    +EP-SAVED: saved=<base|default|?> next=<url|-|?>
 *                    ("?" while environment.unconfirmed)
 *                    Only if that second line would exceed the AT line limit,
 *                    emit +EP-SAVED: saved=<base> then +EP-NEXT: next=<url>.
 *   AT+EP SET <base> xiaotai_environment_set(base)
 *   AT+EP RESET      xiaotai_environment_set(NULL)
 *   AT+EP CANCEL     xiaotai_environment_cancel()
 * OK means accepted; a refusal is ERROR with the returned result. The end of
 * a switch is the unsolicited line +EP: result=<saved|failed|cancelled>
 * error=<n>. The diagnostics provider follows the same size rule with
 * +ENVIRONMENT-SAVED: and +ENVIRONMENT-NEXT: instead of the AT prefixes.
 * start: takes the first snapshot. The
 * module works while the platform is stopped by a boot-time read error
 * (environment.boot_error): that is when it is needed to repair the stored
 * base. */
xiaotai_err_t xiaotai_module_environment_init(void);
void xiaotai_module_environment_start(void);

/* Host tests: end the helper task and forget the switch in progress, the
 * configuration and the counters. */
void xiaotai_environment_reset(void);

#ifdef __cplusplus
}
#endif
