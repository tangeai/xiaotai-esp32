// SPDX-License-Identifier: MIT
#pragma once
/* Provisioning module: how an application sees and steers the way the device
 * gets its network credentials.
 *
 * The network engine (xiaotai/net.h) does all of the work: the link state
 * machine, the setup access point with its page, the serial path, verification
 * and storage. This module is the thin layer above it that applications use:
 *   - one snapshot that answers "what should a setup page show now" (a derived
 *     phase next to the engine's two status structs, taken at one moment),
 *   - module events in place of the engine's raw ones,
 *   - the operations a user can ask for (start over, give up, try again) as
 *     functions and as intents, with the device wound down first,
 *   - the restart that follows stored credentials held back until what the
 *     user changed has reached storage and the application's prompt has ended.
 * It implements no provisioning method and draws nothing. A further way for
 * credentials to reach the device (an on-screen keyboard, a short-range radio)
 * is a method registered with the engine (xiaotai_net_provision_method_register),
 * never an addition here.
 *
 * Manifest name "provisioning". Depends on the engines net, identity (the
 * preferences service), eventlog and at; optionally on the platform core
 * (used when the product selects it, never pulled in: src/with_platform.c
 * closes its gate before a provisioning restart). Hardware need: network.
 *
 * Task tags as in xiaotai/framework.h. Every handler and event of this module
 * runs on the framework owner task. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/net.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- status */

/* What a setup page shows. Derived from the engine's provisioning state, its
 * pending control request and this module's own begin in progress. */
typedef enum {
    /* No setup method is open and none was asked for: the link is run by the
     * engine with the stored credentials (see link.state). */
    XIAOTAI_PROVISIONING_NONE = 0,
    /* A begin was accepted: the device is being wound down, pending settings
     * are being written, or the engine has not opened its methods yet. */
    XIAOTAI_PROVISIONING_STARTING,
    /* Methods are open and wait for credentials (provision.methods,
     * provision.ap_ssid, provision.url say how; provision.reason says why). */
    XIAOTAI_PROVISIONING_WAITING,
    /* Submitted credentials are being tried (provision.ssid). */
    XIAOTAI_PROVISIONING_VERIFYING,
    /* The last submission failed (provision.error); still accepting. */
    XIAOTAI_PROVISIONING_REJECTED,
    /* Accepted credentials are being stored. */
    XIAOTAI_PROVISIONING_STORING,
    /* Accepted but not stored (provision.error): xiaotai_provisioning_retry(). */
    XIAOTAI_PROVISIONING_STORE_FAILED,
    /* Stored. With restart_pending the device restarts once every restart
     * hold is released; without, it keeps running on the verified link. */
    XIAOTAI_PROVISIONING_DONE,
    /* The framework announced the restart (XIAOTAI_EVENT_RESTARTING). */
    XIAOTAI_PROVISIONING_RESTARTING,
} xiaotai_provisioning_phase_t;

typedef struct {
    xiaotai_provisioning_phase_t phase;
    /* Increases whenever any field of this struct changes, the two engine
     * snapshots included. */
    uint32_t revision;
    /* This boot is a provisioning boot (xiaotai_net_provisioning_boot()). */
    bool exclusive;
    /* A restart will follow (provision.restart_required, or a begin in
     * exclusive mode was stored). */
    bool restart_pending;
    /* This module holds the restart back: settings are still being written. */
    bool holding_restart;
    bool packed_retry_required;
    /* Result of the last begin, cancel or retry that finished; XIAOTAI_OK when
     * none failed. */
    xiaotai_err_t error;
    /* The engine's snapshots, both taken on the owner task at the same time. */
    xiaotai_net_provision_status_t provision;
    xiaotai_net_status_t link;
} xiaotai_provisioning_status_t;

/* [any] Snapshot. Before the module started: phase NONE, everything zero. */
void xiaotai_provisioning_status(xiaotai_provisioning_status_t *status);

/* [any] True while phase is anything but NONE: the application gives its
 * screen (or its lights) to setup guidance. */
bool xiaotai_provisioning_active(void);

/* [init] One product policy may observe the credential store result directly
 * on the owner. Unlike UI events, delivery does not need an owner queue slot.
 * On success, take any confirmation hold here, before the module releases
 * its own. On failure, schedule (do not run inline) any product retry. */
typedef void (*xiaotai_provisioning_store_fn_t)(xiaotai_err_t result, bool restart, void *ctx);
xiaotai_err_t xiaotai_provisioning_observe_store(xiaotai_provisioning_store_fn_t fn, void *ctx);

/* ---------------------------------------------------------------- operations */

enum {
    /* Erase the stored credentials (and the history of networks that worked)
     * first. Without it they are kept, and xiaotai_provisioning_cancel()
     * returns to them. */
    XIAOTAI_PROVISIONING_FORGET = 1u << 0,
    /* Allow a begin while a foreground activity is current. The orderly
     * policy revokes it; a selected direct local policy leaves it running
     * until the link goes down or the device restarts. Without PREEMPT a
     * begin while an activity is current is refused with XIAOTAI_ERR_BUSY. */
    XIAOTAI_PROVISIONING_PREEMPT = 1u << 1,
};

/* [owner] Start over with new credentials: what "change network" and a
 * factory gesture ask for. The default orderly policy has these steps,
 * none of them blocking:
 *   1. quiesce the system (xiaotai_quiesce_begin(XIAOTAI_QUIESCE_PROVISIONING));
 *      while it is quiesced the framework refuses setting changes, so nothing
 *      new starts towards storage;
 *   2. where provisioning has a boot to itself (a restart follows step 3),
 *      with the platform core in the build: close its gate
 *      (xiaotai_platform_gate_close; the binding module cancels a running
 *      attempt on the quiesce) and wait until it is drained, so no request is
 *      in flight when the device restarts;
 *   3. wait until accepted setting changes have reached storage
 *      (xiaotai_preferences_pending()). Steps 2 and 3 share one bound of 5 s.
 *      Where a restart follows, a setting that could not be written
 *      (XIAOTAI_EVENT_SETTING_NOT_SAVED during the wait) or the bound running
 *      out stops the begin with that error (XIAOTAI_ERR_TIMEOUT for the
 *      bound): nothing is handed to the engine, everything taken is released,
 *      the user may begin again. Where provisioning is concurrent (no
 *      restart, the writes go on in the background) the wait only counts a
 *      timeout and continues;
 *   4. xiaotai_net_reprovision(flags & XIAOTAI_PROVISIONING_FORGET).
 * Where provisioning is concurrent the quiesce ends as soon as the engine
 * finished the request (the link is then HELD and the methods are open);
 * where it has a boot to itself the quiesce and the gate are kept until the
 * restart: as soon as the engine reports restart_required this module hands
 * its quiesce to xiaotai_restart_request(XIAOTAI_QUIESCE_PROVISIONING,
 * ticket), so nothing begins in between (restart holds apply). The outcome
 * arrives as status and events; a failure of steps 2 to 4 releases what was
 * taken in reverse order and is reported in status.error and
 * XIAOTAI_EVENT_PROVISIONING_FAILED.
 * The build-time local policies in provisioning_config.h skip steps 1–3:
 * DIRECT_LINK (concurrent only) hands the change directly to the engine;
 * DIRECT_RESTART (exclusive only) also uses the framework's direct restart
 * after the engine commits. The native application may take a restart hold
 * before the call and use provisioning.changed with provision.restart_required,
 * restart_pending and no error to observe the committed restart. The failure
 * event distinguishes "begin" (not committed) from "restart" (committed, but
 * the framework refused ownership). Both let the application release its hold.
 * restart_pending alone only promises a restart if the request succeeds.
 * The local policy never takes over an existing quiesce or restart. A policy
 * and network mode mismatch is XIAOTAI_ERR_INVALID_STATE.
 *
 * XIAOTAI_OK                 accepted
 * XIAOTAI_ERR_BUSY           an activity is current and PREEMPT was not given,
 *                            the system is quiesced by someone else, or the
 *                            platform gate is held by someone else
 * XIAOTAI_ERR_INVALID_STATE  a begin is already in progress, the phase is not
 *                            NONE, WAITING or REJECTED, or the module has not started
 * XIAOTAI_ERR_INVALID_ARG    unknown flag bits
 * XIAOTAI_ERR_NOT_SUPPORTED  the network in use takes no credentials */
xiaotai_err_t xiaotai_provisioning_begin(uint32_t flags);

/* [owner] A begin is still waiting or held before handoff to the engine.
 * A STARTING phase can also mean the engine already owns the request. */
bool xiaotai_provisioning_begin_cancellable(void);

/* [owner] Give up: a begin that has not reached the engine yet is dropped (the
 * platform gate opens, the quiesce ends); otherwise xiaotai_net_provision_close(), which returns to the
 * stored credentials (in a provisioning boot: after a restart).
 * XIAOTAI_ERR_NOT_FOUND when there are no stored credentials to return to;
 * XIAOTAI_ERR_INVALID_STATE while credentials are being verified or stored,
 * and in phases DONE and RESTARTING. XIAOTAI_OK when nothing was open. */
xiaotai_err_t xiaotai_provisioning_cancel(void);

/* [owner] "Try again", whatever is stuck:
 *   STORE_FAILED                     xiaotai_net_provision_store_retry()
 *   otherwise, link BLOCKED, WAITING or HELD   xiaotai_net_retry()
 * XIAOTAI_ERR_INVALID_STATE when neither applies. */
xiaotai_err_t xiaotai_provisioning_retry(void);

/* [any] Ask for a scan and read its result, for an application that lists
 * networks itself. Same contracts as xiaotai_net_scan() and
 * xiaotai_net_networks(); no password leaves the engine. */
xiaotai_err_t xiaotai_provisioning_scan(void);
size_t xiaotai_provisioning_networks(xiaotai_net_network_t *networks, size_t capacity,
                                     xiaotai_net_scan_status_t *status);

/* [any] Restart holds for applications: taken when a prompt ("connected",
 * "saved") starts, released when it ended or was given up. They forward to
 * the framework's holds (xiaotai_restart_hold, xiaotai_restart_release), so
 * the restart waits for the last one of anybody; applications may call the
 * framework directly. A hold that is never released keeps the device from
 * restarting: release on every path. */
void xiaotai_provisioning_restart_hold(void);
void xiaotai_provisioning_restart_release(void);

/* -------------------------------------------------------------------- events */

/* The status changed. args.value: xiaotai_provisioning_phase_t; data: a copy of
 * xiaotai_provisioning_status_t, valid during the handler. */
#define XIAOTAI_EVENT_PROVISIONING_CHANGED "provisioning.changed"
/* Setup methods opened. args.value: xiaotai_net_provision_reason_t; args.text:
 * the setup access point's name ("" when the portal is not among the methods). */
#define XIAOTAI_EVENT_PROVISIONING_OPENED "provisioning.opened"
/* Credentials were stored. args.value: 1 a restart follows, 0 none accepted
 * or expected (check provisioning.failed for a refused restart). Emitted once
 * per stored submission, before the module releases its restart hold. */
#define XIAOTAI_EVENT_PROVISIONING_DONE "provisioning.done"
/* A submission was rejected, storing failed, or a begin failed. args.value:
 * the xiaotai_err_t (XIAOTAI_ERR_TIMEOUT no address in time,
 * XIAOTAI_ERR_NOT_FOUND network not found, XIAOTAI_FAIL join refused, or a
 * storage result); args.text: "verify", "store", "begin", or "restart"
 * (the Wi-Fi commit succeeded; the framework refused the direct restart). */
#define XIAOTAI_EVENT_PROVISIONING_FAILED "provisioning.failed"

/* ------------------------------------------------------------------- intents */

/* Orderly begin: args.value may carry
 * XIAOTAI_PROVISIONING_PREEMPT only; FORGET (or any other bit) is refused with
 * XIAOTAI_ERR_INVALID_ARG. A refusal is reported with
 * XIAOTAI_EVENT_PROVISIONING_FAILED (text "begin"). The stored credentials are
 * kept, so this intent may come from anywhere, the console included. */
#define XIAOTAI_INTENT_PROVISIONING_BEGIN "provisioning.begin"
/* xiaotai_provisioning_begin(XIAOTAI_PROVISIONING_FORGET | (args.value &
 * XIAOTAI_PROVISIONING_PREEMPT)): the factory gesture. Registered with
 * xiaotai_intent_handle_ex(..., XIAOTAI_INTENT_LOCAL): inputs, touch and
 * applications may post it, the console's AT+INTENT may not
 * (XIAOTAI_ERR_NOT_ALLOWED). Erasing the credentials from the console is the
 * explicit command AT+NET FORGET or AT+NET PROVISION FORGET; both exist only
 * in builds with XIAOTAI_NET_CONSOLE_FORGET (xiaotai/net_config.h, off by
 * default: development and bench firmware) and use the orderly policy. */
#define XIAOTAI_INTENT_PROVISIONING_FORGET "provisioning.forget"
/* xiaotai_provisioning_cancel(). No arguments. */
#define XIAOTAI_INTENT_PROVISIONING_CANCEL "provisioning.cancel"
/* xiaotai_provisioning_retry(). No arguments. */
#define XIAOTAI_INTENT_PROVISIONING_RETRY "provisioning.retry"

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t begins;           /* accepted */
    uint32_t begins_refused;   /* busy or wrong state */
    uint32_t begins_failed;    /* the engine refused step 3 */
    uint32_t cancels;
    uint32_t retries;
    uint32_t stored;           /* submissions that reached DONE */
    uint32_t rejected;         /* submissions that reached REJECTED */
    uint32_t store_failures;
    uint32_t flush_timeouts;   /* settings or the platform gate still pending after 5 s */
    uint32_t flush_failures;   /* a setting could not be written during the wait */
    uint32_t restart_holds;    /* holds applications took through this module */
} xiaotai_provisioning_stats_t;

/* [any] */
void xiaotai_provisioning_stats(xiaotai_provisioning_stats_t *stats);

/* ----------------------------------------------------------------- lifecycle */

/* Called by the framework through the generated tables. init: subscribes to
 * the engine's events, registers the four intents, two verbs of the engine's
 * AT+NET command (xiaotai_at_verbs_register) and the diagnostics provider
 * "provisioning":
 *   AT+NET PROVISION [FORCE]            xiaotai_provisioning_begin(), FORCE
 *                                       giving XIAOTAI_PROVISIONING_PREEMPT
 *   AT+NET PROVISION FORGET [FORCE]     the same with
 *                                       XIAOTAI_PROVISIONING_FORGET; the word
 *                                       FORGET is accepted only with
 *                                       XIAOTAI_NET_CONSOLE_FORGET (otherwise
 *                                       ERROR: INVALID_ARG, nothing happens);
 *                                       its handler is compiled only with that
 *                                       option, under the prefix
 *                                       xiaotai_provisioning_at_forget_ (the
 *                                       link check of xiaotai_at.md 4.6)
 *   AT+NET CANCEL                       xiaotai_provisioning_cancel()
 * start: takes the first snapshot and, when the engine already
 * has methods open (no credentials at boot, a provisioning boot), emits
 * XIAOTAI_EVENT_PROVISIONING_OPENED. */
xiaotai_err_t xiaotai_module_provisioning_init(void);
void xiaotai_module_provisioning_start(void);

/* Host tests: forget every registration, counter and the begin in progress. */
void xiaotai_provisioning_reset(void);

#ifdef __cplusplus
}
#endif
