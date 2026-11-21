// SPDX-License-Identifier: MIT
#pragma once
/* Binding module: gives the device its identity.
 *
 * A device is bound when a user confirmed it in the XiaoTai portal: the device
 * reports itself, receives a short verification code, shows and speaks it,
 * waits for the grant on a temporary signaling session, stores the identity
 * the grant carries and only then acknowledges it. A device the server
 * unbound keeps its stored identity and uses it to sign the next report
 * (a signed re-binding).
 *
 * The platform core (xiaotai/platform.h) says when a binding is needed and
 * carries every exchange (requests without a session, the transfer for the
 * spoken code, the guest signaling session); the identity component
 * (xiaotai/identity.h) stores the result. This module is the flow in between
 * and nothing else: it shows no page (applications read the status and the
 * events) and decides no interaction. In particular it never speaks the code
 * on its own: when and how often the code is heard, and whether voice
 * guidance is on, is the application's (it calls xiaotai_binding_announce(),
 * typically on XIAOTAI_EVENT_BINDING_CODE and XIAOTAI_EVENT_BINDING_ANNOUNCED).
 *
 * One attempt at a time, all of it on the framework owner task:
 *   PREPARING   the registered preparation steps run (another module's
 *               storage write that must succeed before the server is asked)
 *   REPORTING   POST <device>/v1/device/report {"mac":"<fingerprint>"}, signed
 *               when an identity is stored
 *   CONNECTING  guest signaling session with the temporary credentials
 *   WAITING     the code is visible; the grant may arrive until the window ends
 *   STORING     the granted identity is written and read back (or, for a
 *               re-binding, compared with the stored one)
 *   CONFIRMING  the acknowledgement is published and its confirmation awaited
 * then BOUND and xiaotai_platform_retry(). Nothing is acknowledged before it
 * is stored. A failed attempt is never repeated by the module on its own.
 *
 * Manifest name "binding". Depends on the module provisioning and on the
 * engines net, platform, identity, eventlog and at. Hardware need: network.
 * It registers the framework quiesce guard "binding" (xiaotai/framework.h):
 * XIAOTAI_ERR_BUSY for the reason XIAOTAI_QUIESCE_ENVIRONMENT from PREPARING
 * to CONFIRMING, so no environment switch happens while an attempt runs and a
 * code being read stays valid; every other reason is agreed to (the attempt
 * is then cancelled by the quiesce). This needs no dependency on the
 * environment module.
 * Optional (used when the product selects them, never pulled in):
 *   media, sounds speaking the code; src/binding_announce.c. Without the media
 *                 engine, or without an audio output, the announcement
 *                 functions return XIAOTAI_ERR_NOT_SUPPORTED and everything
 *                 else works.
 *
 * Task tags as in xiaotai/framework.h; [guard] is defined below.
 * Secrets: the device key and the temporary token never appear in a status,
 * an event, a log line or a command reply. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/identity.h"
#include "xiaotai/modules/binding_config.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- status */

/* Longest verification code without its terminator. 16: the largest code any
 * deployed firmware accepts; a shorter limit would refuse codes the server
 * may issue. */
#define XIAOTAI_BINDING_CODE_MAX 16U

typedef enum {
    /* The platform has not said yet whether this device is bound (no network,
     * no discovery, login still running). */
    XIAOTAI_BINDING_UNKNOWN = 0,
    /* An identity is stored and the platform accepted it, or has not refused it. */
    XIAOTAI_BINDING_BOUND,
    /* A binding is needed and no attempt is running. */
    XIAOTAI_BINDING_NEEDED,
    /* The registered preparation steps run (xiaotai_binding_prepare_register). */
    XIAOTAI_BINDING_PREPARING,
    XIAOTAI_BINDING_REPORTING,
    XIAOTAI_BINDING_CONNECTING,
    XIAOTAI_BINDING_WAITING,
    XIAOTAI_BINDING_STORING,
    XIAOTAI_BINDING_CONFIRMING,
    /* The attempt failed (error, code); waits for xiaotai_binding_retry(). */
    XIAOTAI_BINDING_FAILED,
    /* The code was shown and its window ended without a grant; waits for
     * xiaotai_binding_retry(). */
    XIAOTAI_BINDING_EXPIRED,
} xiaotai_binding_phase_t;

typedef struct {
    xiaotai_binding_phase_t phase;
    uint32_t revision;         /* increases whenever any field below changes, seconds_left excepted */
    /* The attempt signs with the stored identity: the server unbound this
     * device, or an earlier grant was stored but never confirmed. */
    bool signed_rebinding;
    /* A boot-time environment read prevents any binding attempt this boot. */
    bool environment_boot_failed;
    /* FAILED: why. XIAOTAI_ERR_TIMEOUT (no session, no grant confirmation in
     * time), XIAOTAI_ERR_INVALID_STATE (cancelled: the user left the network,
     * the system was quiesced), XIAOTAI_FAIL (the server refused or sent
     * something malformed), XIAOTAI_ERR_INVALID_ARG (a grant that does not
     * match the stored identity), a storage or platform result otherwise. */
    xiaotai_err_t error;
    /* Business code of the refused report, XIAOTAI_PLATFORM_NO_CODE otherwise
     * (40901: the previous code is still valid, try again later). */
    int32_t code;
    /* WAITING: the code and the seconds until its window ends, computed when
     * the snapshot is taken. "" and 0 in every other phase. */
    char verification[XIAOTAI_BINDING_CODE_MAX + 1U];
    uint32_t seconds_left;
    bool announcing;           /* the code is being spoken now */
    /* The stored device id, and the client id in use (stored, or derived with
     * xiaotai_identity_client_id when the identity layout stores none); both
     * "" when no identity is stored. */
    char device_id[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U];
    char client_id[XIAOTAI_IDENTITY_CLIENT_ID_MAX + 1U];
    uint32_t generation;       /* xiaotai_identity_generation() at the snapshot */
} xiaotai_binding_status_t;

/* [any] Snapshot. */
void xiaotai_binding_status(xiaotai_binding_status_t *status);

/* [any] True in phase BOUND. One atomic read. */
bool xiaotai_binding_bound(void);

/* ---------------------------------------------------------------- operations */

/* [owner] Begin an attempt. Accepted in NEEDED, FAILED and EXPIRED. The guards
 * are asked first; then the attempt enters PREPARING and reports only after
 * every preparation step reported XIAOTAI_OK.
 *
 * XIAOTAI_OK                    accepted; progress arrives as events (a failed
 *                               preparation ends it in FAILED, text "prepare")
 * XIAOTAI_ERR_INVALID_STATE     another phase, a provisioning boot, the stored
 *                               identity is UNREADABLE (nothing may overwrite
 *                               it: xiaotai_binding_retry() reloads it first),
 *                               or a guard refused (its own result is returned
 *                               when it is not XIAOTAI_ERR_INVALID_STATE)
 * XIAOTAI_ERR_BUSY              the system is quiesced
 * XIAOTAI_ERR_PLATFORM_OFFLINE  no network, no discovery or no synchronized
 *                               clock yet (xiaotai/platform.h) */
xiaotai_err_t xiaotai_binding_start(void);

/* [owner] The user's "try again":
 *   FAILED, EXPIRED, NEEDED   reloads the identity from storage (an earlier
 *                             save may have been written although it timed
 *                             out), then starts an attempt, signed when an
 *                             identity is there;
 *   UNKNOWN, BOUND            xiaotai_platform_retry(): the platform session
 *                             leaves PAUSED and tries its failed step again.
 * When configured to present a boot-time environment error, the first retry
 * acknowledges the existing failure without starting an attempt. Repairing
 * the stored environment takes effect on the next boot.
 * XIAOTAI_ERR_INVALID_STATE while an attempt is running. */
xiaotai_err_t xiaotai_binding_retry(void);

/* [owner] Stop the running attempt: the guest session is closed, the code is
 * wiped, an announcement is stopped, the phase becomes NEEDED (in PREPARING
 * the step that is running finishes on its own and its result is ignored;
 * what it stored stays valid for the next attempt). Refused with
 * XIAOTAI_ERR_INVALID_STATE in STORING and CONFIRMING (what was granted is
 * finished) and when no attempt is running. */
xiaotai_err_t xiaotai_binding_cancel(void);

/* [owner] Speak the current code once: the configured voice, announce_rounds
 * rounds with announce_gap_ms between them. The module never calls this by
 * itself; repeating, and skipping while voice guidance is off, is the
 * caller's decision. The end is reported with XIAOTAI_EVENT_BINDING_ANNOUNCED.
 *
 * Arbitration with other sounds: the code is played at the media engine's
 * urgent priority, so a cue or a prompt (normal priority) cannot cut a digit;
 * it does replace a normal sound that is playing, so a caller with its own
 * prompts (a feedback engine) calls this only when they are out
 * (xiaotai_feedback_sound_pending() false).
 *
 * XIAOTAI_ERR_INVALID_STATE  not WAITING
 * XIAOTAI_ERR_BUSY           an announcement is in progress (it is never
 *                            replaced: one replay at a time), a foreground
 *                            activity owns the speaker, or the media engine
 *                            refused (a sound of the same urgency plays)
 * XIAOTAI_ERR_NOT_SUPPORTED  the media engine is not in the build, no audio
 *                            output, XIAOTAI_BINDING_VOICE_NONE, or a code
 *                            the configured voice cannot speak (LOCAL: not
 *                            all digits, a digit sound missing, more than 11
 *                            digits; SERVER: not exactly 6 digits) */
xiaotai_err_t xiaotai_binding_announce(void);

/* [owner] Stop speaking the code; XIAOTAI_EVENT_BINDING_ANNOUNCED follows with
 * XIAOTAI_ERR_INVALID_STATE. Nothing to stop is not an error. */
void xiaotai_binding_announce_stop(void);

/* [owner] Local reset: forget the stored identity and restart. The server is
 * not told (the device shows up as never bound; network credentials stay).
 * Steps: quiesce the system (XIAOTAI_QUIESCE_BINDING), stop a running
 * attempt, erase the identity (xiaotai_identity_erase), emit
 * XIAOTAI_EVENT_BINDING_FORGOTTEN, then hand the quiesce to the restart
 * (xiaotai_restart_request(XIAOTAI_QUIESCE_BINDING, ticket) in
 * xiaotai/framework.h): nothing can begin between the erase and the
 * restart, and the restart holds apply, so an application can finish a
 * confirmation sound.
 * When erasing fails nothing restarts, the quiesce ends and the event carries
 * the error.
 * XIAOTAI_ERR_BUSY when the system is quiesced by someone else;
 * XIAOTAI_ERR_INVALID_STATE in STORING and CONFIRMING and while one is running.
 * A current foreground activity is ended by the quiesce. */
xiaotai_err_t xiaotai_binding_forget(void);

typedef void (*xiaotai_binding_forget_done_fn_t)(xiaotai_err_t result, void *ctx);
/* [owner, local only] As forget(), with completion after the committed erase
 * and restart handoff, exactly once when accepted. Take a confirmation hold
 * before calling. With direct=true the restart does not quiesce or end an
 * activity (for a product whose local factory key has that policy). Binding
 * remains the owner of the identity job; Wi-Fi and the server are untouched.
 * An external intent cannot select either policy through this API. */
xiaotai_err_t xiaotai_binding_forget_local(bool direct, xiaotai_binding_forget_done_fn_t done, void *ctx);

/* -------------------------------------------------------------------- guards */

/* A guard is another module's veto against starting an attempt right now
 * (something it is doing must end first). Data that must be settled before a
 * binding is a preparation step, below, not a guard.
 * [guard] Runs on the owner task inside xiaotai_binding_start(), the
 * automatic start and xiaotai_binding_retry() (not in retry's UNKNOWN / BOUND
 * branch), answers from memory, changes nothing. XIAOTAI_OK lets the attempt
 * begin. A guard that refused with XIAOTAI_ERR_BUSY calls
 * xiaotai_binding_guards_changed() once it would agree. */
typedef xiaotai_err_t (*xiaotai_binding_guard_fn_t)(void *ctx);

/* [init] name: fixed word for diagnostics, static storage. Asked in
 * registration order. XIAOTAI_ERR_NO_MEM beyond XIAOTAI_BINDING_GUARDS_MAX
 * (a build option, xiaotai/modules/binding_config.h). */
xiaotai_err_t xiaotai_binding_guard_register(const char *name, xiaotai_binding_guard_fn_t guard, void *ctx);

/* [owner] A guard that answered XIAOTAI_ERR_BUSY would now agree (the work it
 * waited for ended). Only an automatic start is remembered: when the start the
 * module made on its own for a report (manual_start false) was refused with
 * XIAOTAI_ERR_BUSY and the phase is still NEEDED for that report, the guards
 * are asked again and the attempt begins when all agree; otherwise nothing
 * happens. Any other refusal, and a refused xiaotai_binding_start() of an
 * application, is not remembered (the next report or the user starts again).
 * Calling it when nothing waits is harmless. */
void xiaotai_binding_guards_changed(void);

/* ------------------------------------------------------- preparation steps */

/* A preparation step is work another module must finish, successfully, before
 * a binding report reaches the server: a storage write that moves its data
 * into the past (the recognition module advances its stored scope). It runs
 * for every attempt, first binding and signed re-binding alike, after the
 * guards agreed and before REPORTING; never for an ordinary login and never in
 * retry's UNKNOWN / BOUND branch. */

/* [owner] Completion of one step; called exactly once, on the owner task,
 * after the step returned XIAOTAI_OK. done_ctx is the module's. */
typedef void (*xiaotai_binding_prepared_fn_t)(xiaotai_err_t result, void *done_ctx);

/* [owner] Start the step and return. XIAOTAI_OK: done(result, done_ctx)
 * follows; any other result ends the attempt at once (FAILED, that error) and
 * done is never called. A step may finish synchronously by calling done
 * before returning. ctx is the registrant's. */
typedef xiaotai_err_t (*xiaotai_binding_prepare_fn_t)(xiaotai_binding_prepared_fn_t done, void *done_ctx,
                                                      void *ctx);

/* A step whose done has not come within this time ends the attempt with
 * XIAOTAI_ERR_TIMEOUT; a late done is counted and ignored. */
#define XIAOTAI_BINDING_PREPARE_MS 10000U

/* [init] Steps run one after another in registration order; the attempt
 * reports when the last one reported XIAOTAI_OK, and is FAILED (error: the
 * step's result, XIAOTAI_EVENT_BINDING_FAILED text "prepare") at the first one
 * that did not. name: fixed word for diagnostics, static storage.
 * XIAOTAI_ERR_NO_MEM beyond XIAOTAI_BINDING_PREPARES_MAX (a build option,
 * xiaotai/modules/binding_config.h);
 * XIAOTAI_ERR_INVALID_ARG without a name or function. */
xiaotai_err_t xiaotai_binding_prepare_register(const char *name, xiaotai_binding_prepare_fn_t prepare, void *ctx);

/* ------------------------------------------------------------- configuration */

/* How the code is spoken (by xiaotai_binding_announce). */
typedef enum {
    /* The digits of the sound packs (xiaotai_sounds_digit) after the guidance
     * sound "guide.binding", as one play; when a digit is missing, the code is
     * not all digits or has more than 11, the server's rendering instead. */
    XIAOTAI_BINDING_VOICE_LOCAL_FIRST = 0,
    /* Always the server's rendering (GET <device>/v1/device/tts?code=...,
     * 8 kHz mono 16-bit PCM, streamed to the media engine). A rendering whose
     * total length is an odd number of bytes is a failed announcement (the
     * last half sample is never played). */
    XIAOTAI_BINDING_VOICE_SERVER,
    /* Only the local digits; a code that cannot be spoken locally is not spoken. */
    XIAOTAI_BINDING_VOICE_LOCAL,
    XIAOTAI_BINDING_VOICE_NONE,
} xiaotai_binding_voice_t;

/* Product configuration. Every field: 0 (false) selects the default, and the
 * defaults are what deployed firmware does. */
typedef struct {
    /* false (default): an attempt begins without being asked whenever the
     * platform reports that a binding is needed (once per report; a guard's
     * XIAOTAI_ERR_BUSY is waited out, see xiaotai_binding_guards_changed),
     * which also makes the signed re-binding after a server-side unbind
     * automatic. true: nothing begins until the application calls
     * xiaotai_binding_start(). */
    bool manual_start;
    /* Preserve the failed setup display after the stored environment fails
     * to load. Other products keep their independent identity presentation. */
    bool present_environment_boot_failure;
    /* Require an unexpired exp claim on the temporary Report token before
     * opening guest MQTT. Default false; independent of formal login. */
    bool guest_token_needs_expiry;
    /* Longest wait for the grant after the code became visible, seconds;
     * default 190. The window also ends when the temporary token expires
     * (its expiry read with xiaotai_platform_token_expiry; by default a
     * token without one leaves wait_s alone). */
    uint16_t wait_s;
    /* Longest wait for the guest session to be subscribed, seconds; default 20. */
    uint16_t connect_s;
    /* Longest wait for the acknowledgement's confirmation, milliseconds;
     * default 0: until the window ends. */
    uint32_t confirm_ms;
    /* A report whose temporary token has less than a second to live is an old
     * code replayed by the server: report again after 1000 ms, at most this
     * many times; default 3. */
    uint8_t stale_code_retries;
    /* What a grant the attempt cannot use does (malformed: one credential
     * member without the other, an empty or over-long value, a payload of
     * another type; or, with an identity stored, credentials naming another
     * identity). false (default): the attempt ends FAILED (text "grant"),
     * nothing is acknowledged; "keep the stored identity" is a payload that
     * is absent, null, "" or an object without either credential member.
     * true: the grant is counted (grants_malformed, grants_mismatched) and
     * ignored, and the attempt keeps waiting for a usable one until its
     * window ends; "keep" is a payload that is absent, null or an empty
     * object. Each value is what deployed firmware of a product family does. */
    bool ignore_unusable_grants;
    /* Require time in the verification window when starting identity storage.
     * Default false: a received grant may begin storage after the window. */
    bool require_active_presave_window;
    /* Check the authoritative manual-disconnect flag before storing and
     * before acknowledgement. Default false; network events still apply. */
    bool reject_manual_disconnect_during_commit;

    /* How one announcement sounds. When and how often it is heard is the
     * application's configuration, not this module's. */
    xiaotai_binding_voice_t voice;
    /* Times the code is spoken in one announcement; default 1. */
    uint8_t announce_rounds;
    /* Pause between rounds, milliseconds; default 350. */
    uint16_t announce_gap_ms;
    /* Guest SUBACK timeout in seconds; zero keeps using the platform timeout.
     * A nonzero value still cannot extend connect_s or the first code window. */
    uint16_t guest_subscribe_timeout_s;
    /* Keep a displayed first-binding window across an involuntary network
     * interruption. The original guest subscription and expiry still gate
     * the code. Default false. */
    bool resume_unbound_window_on_reconnect;
} xiaotai_binding_config_t;

/* [glue] Before xiaotai_module_binding_init(), from the product glue; copied. NULL
 * restores the defaults. XIAOTAI_ERR_INVALID_ARG: wait_s above 3600,
 * connect_s or guest_subscribe_timeout_s above 120, announce_rounds above 10,
 * announce_gap_ms above 5000, or an unknown voice. XIAOTAI_ERR_INVALID_STATE
 * once initialized. */
xiaotai_err_t xiaotai_binding_configure(const xiaotai_binding_config_t *config);

/* -------------------------------------------------------------------- events */

/* The status changed. args.value: xiaotai_binding_phase_t; data: a copy of
 * xiaotai_binding_status_t, valid during the handler. */
#define XIAOTAI_EVENT_BINDING_CHANGED "binding.changed"
/* A binding is needed. args.value: 0 the device was never bound, 1 the server
 * unbound it (the binding generation has advanced: whoever caches data of the
 * old binding drops it now). Emitted for every report of the platform core. */
#define XIAOTAI_EVENT_BINDING_NEEDED "binding.needed"
/* The code became visible. args.text: the code; args.value: seconds until its
 * window ends. */
#define XIAOTAI_EVENT_BINDING_CODE "binding.code"
/* The device is bound and the platform session was told to continue.
 * args.value: 1 a new identity was stored, 0 the stored one was confirmed. */
#define XIAOTAI_EVENT_BINDING_BOUND "binding.bound"
/* An attempt ended without a binding. args.value: the xiaotai_err_t;
 * args.text: "expired" for phase EXPIRED, otherwise the step that failed:
 * "prepare", "report", "connect", "grant", "store" or "confirm". */
#define XIAOTAI_EVENT_BINDING_FAILED "binding.failed"
/* An announcement ended. args.value: XIAOTAI_OK every round was played to the
 * end; XIAOTAI_ERR_INVALID_STATE stopped (xiaotai_binding_announce_stop, the
 * grant arrived, the window ended, cancel, quiesce) or cut by another sound or
 * a session; otherwise the failure (download, odd-length rendering, decoder).
 * An application that repeats the code counts its interval from here (end to
 * start) or from its own call (start to start). */
#define XIAOTAI_EVENT_BINDING_ANNOUNCED "binding.announced"
/* The local reset's erase result. args.value: XIAOTAI_OK (durably erased;
 * restart still has to run) or the erase error. */
#define XIAOTAI_EVENT_BINDING_FORGOTTEN "binding.forgotten"

/* ------------------------------------------------------------------- intents */

/* None of them carries arguments. */
#define XIAOTAI_INTENT_BINDING_START "binding.start"       /* xiaotai_binding_start() */
#define XIAOTAI_INTENT_BINDING_RETRY "binding.retry"       /* xiaotai_binding_retry() */
#define XIAOTAI_INTENT_BINDING_CANCEL "binding.cancel"     /* xiaotai_binding_cancel() */
#define XIAOTAI_INTENT_BINDING_ANNOUNCE "binding.announce" /* xiaotai_binding_announce() */
/* xiaotai_binding_forget(). Registered with xiaotai_intent_handle_ex(...,
 * XIAOTAI_INTENT_LOCAL): inputs, touch and applications may post it, the
 * console's AT+INTENT may not (XIAOTAI_ERR_NOT_ALLOWED). Erasing the identity
 * from the console is AT+BIND CLEAR, which exists only with
 * XIAOTAI_BINDING_DEV_COMMANDS. */
#define XIAOTAI_INTENT_BINDING_FORGET "binding.forget"

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t needed_first, needed_rebinding;   /* reports of the platform core */
    uint32_t attempts, attempts_signed;
    uint32_t reports_refused;      /* the report was answered with a refusal */
    uint32_t stale_code_retries;
    uint32_t connect_timeouts;
    uint32_t grants;               /* first grant of an attempt */
    uint32_t grants_ignored;       /* further messages after the first grant, or of another type */
    uint32_t grants_malformed;
    uint32_t grants_mismatched;    /* re-binding grant naming another identity */
    uint32_t stores, store_failures;
    uint32_t confirmations, confirm_timeouts;
    uint32_t bound, failed, expired, cancelled;
    uint32_t guard_refusals;
    uint32_t auto_resumed;         /* automatic starts begun by xiaotai_binding_guards_changed */
    uint32_t prepare_failures, prepare_timeouts, prepare_late;
    uint32_t announcements, announcements_server, announcements_failed;
    uint32_t announcements_busy;   /* refused: one in progress, speaker owned, media refused */
    uint32_t forgets, forget_failures;
} xiaotai_binding_stats_t;

/* [any] */
void xiaotai_binding_stats(xiaotai_binding_stats_t *stats);

/* ----------------------------------------------------------------- lifecycle */

/* Called by the framework through the generated tables. init: declares its
 * platform needs (xiaotai_platform_require), subscribes to the platform's and
 * the network engine's events and to XIAOTAI_EVENT_QUIESCE, registers the five
 * intents, the quiesce guard "binding" (see the top of this header), the
 * command AT+BIND and the diagnostics provider "binding".
 *
 * AT+BIND (xiaotai/at_command.h conventions):
 *   AT+BIND          +BIND: phase=<n> signed=<0|1> error=<n> code=<n>
 *                    verification=<code|-> left=<s> announcing=<0|1>
 *                    device=<id|-> client=<id|-> gen=<n>
 *   AT+BIND RETRY    xiaotai_binding_retry()
 *   AT+BIND REPLAY   xiaotai_binding_announce()
 * Only with the build option XIAOTAI_BINDING_DEV_COMMANDS:
 *   AT+BIND SET <device-id> <key-hex> [<client-id>]
 *                    device id 1..XIAOTAI_IDENTITY_DEVICE_ID_MAX characters,
 *                    the key as hexadecimal of 1..XIAOTAI_IDENTITY_DEVICE_KEY_MAX
 *                    bytes, client id 1..XIAOTAI_IDENTITY_CLIENT_ID_MAX
 *                    characters (absent: the identity component's default);
 *                    checked before anything happens, never echoed; the
 *                    decoded key is wiped (xiaotai_identity_wipe) as soon as it
 *                    was handed over. Same path as xiaotai_binding_forget():
 *                    quiesce (XIAOTAI_QUIESCE_BINDING), stop an attempt,
 *                    save, then the quiesce is handed to
 *                    xiaotai_restart_request(XIAOTAI_QUIESCE_BINDING, ticket);
 *                    a failed save restarts nothing and ends the quiesce.
 *                    INVALID_STATE in STORING and CONFIRMING.
 *   AT+BIND CLEAR    xiaotai_binding_forget()
 * The handlers of SET and CLEAR are compiled only with the option, under the
 * prefix xiaotai_binding_at_dev_ (the link check of xiaotai_at.md 4.6).
 * start: derives the first phase from the identity state. */
xiaotai_err_t xiaotai_module_binding_init(void);
void xiaotai_module_binding_start(void);

/* Host tests: forget the attempt, the guards, the configuration and the counters. */
void xiaotai_binding_reset(void);

#ifdef __cplusplus
}
#endif
