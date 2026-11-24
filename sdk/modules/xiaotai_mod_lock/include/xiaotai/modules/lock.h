// SPDX-License-Identifier: MIT
#pragma once
/* Lock module: the door lock outputs of this device.
 *
 * A lock is a relay output of the board in the role family "door" (role
 * "door" or "door.<name>": the role up to its first ".", xiaotai/hal.h
 * "Output ownership"; "doorbell" is another family). The module claims that
 * family in its init() (xiaotai_hal_output_claim with the static claim
 * {"door", "lock"}), so from then on no other component, the feedback engine
 * included, drives those outputs; a claim the hardware abstraction refuses
 * (another owner already claimed "door") fails the init. The board lists as
 * many door relays as it has and the module numbers them in the board's
 * order; outputs of the family that are not relays are left alone.
 *
 * Opening a lock releases it for a bounded time and then locks it again: the
 * module never
 * leaves a lock released without a deadline, locks every lock when the system
 * is quiesced (a restart, an update) and at start, and spaces releases so a
 * strike coil is not driven continuously.
 *
 * The module decides nothing about who may open: an application calls it
 * after its own confirmation (a button on the indoor station, an accepted
 * request of the peer in a call), and how a request travels between two
 * devices is the application's protocol. The module also draws nothing.
 *
 * Door state. A board may sense whether the door itself is open (a contact on
 * an input part). Inputs belong to the interaction engine, so the sensed
 * state reaches this module as intents that the product's interaction profile
 * maps from that input; without them the door state stays UNKNOWN.
 *
 * Manifest name "lock". Depends on the engines eventlog and at. Hardware
 * need: door_lock, a relay in the role family "door" (XIAOTAI_NEED_DOOR_LOCK
 * of xiaotai/needs.h, derived by the rule of sdk/xiaotai_hal/needs.json).
 * As the claimant this module is the only caller of the door relays'
 * functions (the role of "[engine]" in xiaotai/hal.h for these parts) and
 * calls them from the framework owner task only.
 *
 * Task tags as in xiaotai/framework.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/modules/lock_config.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- limits */

/* XIAOTAI_LOCK_MAX, the number of locks the module manages, is a build option
 * (xiaotai/modules/lock_config.h). */
/* Bounds of a release, milliseconds. */
#define XIAOTAI_LOCK_HOLD_MIN_MS 100U
#define XIAOTAI_LOCK_HOLD_MAX_MS 30000U
/* Role text copied into the status, with terminator. */
#define XIAOTAI_LOCK_ROLE_MAX 16U

/* -------------------------------------------------------------------- status */

typedef enum {
    XIAOTAI_LOCK_LOCKED = 0,
    XIAOTAI_LOCK_RELEASED,     /* the relay is on; it locks again when remaining_ms ran out */
    /* The relay refused the last command (error). The module keeps trying to
     * lock it once a second; opening is refused meanwhile. */
    XIAOTAI_LOCK_FAULT,
} xiaotai_lock_state_t;

/* What a door contact reported last. */
typedef enum {
    XIAOTAI_LOCK_DOOR_UNKNOWN = 0,   /* no sensor, or nothing reported yet */
    XIAOTAI_LOCK_DOOR_CLOSED,
    XIAOTAI_LOCK_DOOR_OPEN,
} xiaotai_lock_door_t;

typedef struct {
    uint8_t index;
    char role[XIAOTAI_LOCK_ROLE_MAX];   /* the relay's role, cut to fit */
    xiaotai_lock_state_t state;
    xiaotai_lock_door_t door;
    uint32_t revision;        /* increases whenever state, door or error changes */
    uint32_t remaining_ms;    /* RELEASED: time left when the snapshot was taken */
    xiaotai_err_t error;      /* FAULT: the relay's result */
    uint32_t opens;           /* accepted releases since boot */
} xiaotai_lock_status_t;

/* [any] Number of locks, 0..XIAOTAI_LOCK_MAX. 0 before the module started. */
size_t xiaotai_lock_count(void);

/* [any] Snapshot of one lock. XIAOTAI_ERR_NOT_FOUND for an index at or beyond
 * xiaotai_lock_count(). */
xiaotai_err_t xiaotai_lock_status(uint8_t index, xiaotai_lock_status_t *status);

/* ---------------------------------------------------------------- operations */

/* [owner] Release the lock for hold_ms (0: the setting "lock.hold_ms"), then
 * lock it again. Releasing a lock that is already released extends the
 * deadline to the later of the two; it never shortens it.
 *
 * XIAOTAI_OK                 released
 * XIAOTAI_ERR_NOT_FOUND      no such lock
 * XIAOTAI_ERR_INVALID_ARG    hold_ms outside XIAOTAI_LOCK_HOLD_MIN_MS..XIAOTAI_LOCK_HOLD_MAX_MS
 * XIAOTAI_ERR_BUSY           the system is quiesced, or the lock was locked
 *                            again less than the configured rest time ago
 * XIAOTAI_ERR_INVALID_STATE  the lock is in FAULT, or the module has not started
 * otherwise the relay's own result: the lock is then in FAULT. */
xiaotai_err_t xiaotai_lock_open(uint8_t index, uint32_t hold_ms);

/* [owner] Lock now, before the deadline. Locking a locked lock is XIAOTAI_OK.
 * XIAOTAI_ERR_NOT_FOUND; the relay's own result when it refuses (FAULT). */
xiaotai_err_t xiaotai_lock_close(uint8_t index);

/* [owner] Lock every lock now (what the module does itself when the system is
 * quiesced). */
void xiaotai_lock_close_all(void);

/* [any] What a door contact senses, for a caller that reads a sensor itself.
 * Queued to the owner task. XIAOTAI_ERR_NOT_FOUND for an unknown index. */
xiaotai_err_t xiaotai_lock_door_report(uint8_t index, xiaotai_lock_door_t door);

/* ------------------------------------------------------------------ settings */

/* Default release time. Int, milliseconds
 * XIAOTAI_LOCK_HOLD_MIN_MS..XIAOTAI_LOCK_HOLD_MAX_MS; default: configuration
 * (1000). Storage key "lock_hold". */
#define XIAOTAI_SETTING_LOCK_HOLD_MS "lock.hold_ms"
#define XIAOTAI_SETTING_KEY_LOCK_HOLD_MS "lock_hold"

/* ------------------------------------------------------------- configuration */

/* Product configuration. Every field: 0 selects the default. */
typedef struct {
    uint32_t hold_default_ms;   /* default of "lock.hold_ms"; default 1000 */
    /* Shortest time between locking and the next release of the same lock;
     * default 500. */
    uint32_t rest_ms;
} xiaotai_lock_config_t;

/* [glue] Before xiaotai_module_lock_init(), from the product glue; copied. NULL
 * restores the defaults. XIAOTAI_ERR_INVALID_ARG: a hold time outside its
 * bounds, a rest time above 60000. XIAOTAI_ERR_INVALID_STATE once initialized. */
xiaotai_err_t xiaotai_lock_configure(const xiaotai_lock_config_t *config);

/* -------------------------------------------------------------------- events */

/* A lock's state or its door state changed. args.value: the lock's index;
 * args.text: its role; data: a copy of xiaotai_lock_status_t. */
#define XIAOTAI_EVENT_LOCK_STATE "lock.state"
/* A release was refused. args.value: the xiaotai_err_t; args.text: the role
 * ("" for an unknown index). Emitted for refused intents only: a caller of
 * xiaotai_lock_open has the result already. */
#define XIAOTAI_EVENT_LOCK_REFUSED "lock.refused"

/* ------------------------------------------------------------------- intents */

/* xiaotai_lock_open(args.value, 0). Registered with xiaotai_intent_handle_ex(...,
 * XIAOTAI_INTENT_LOCAL): inputs, touch and applications may post it, the
 * console's AT+INTENT may not (XIAOTAI_ERR_NOT_ALLOWED), so a console without
 * XIAOTAI_LOCK_AT_OPEN cannot release a door through an intent either. */
#define XIAOTAI_INTENT_LOCK_OPEN "lock.open"
/* xiaotai_lock_close(args.value). */
#define XIAOTAI_INTENT_LOCK_CLOSE "lock.close"
/* A door contact changed; args.value: the lock's index. The interaction
 * profile maps the contact's input events to these two. */
#define XIAOTAI_INTENT_LOCK_DOOR_OPENED "lock.door_opened"
#define XIAOTAI_INTENT_LOCK_DOOR_CLOSED "lock.door_closed"

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t opens;             /* accepted */
    uint32_t extended;          /* of which extended a release in progress */
    uint32_t refused_rest;      /* too soon after locking */
    uint32_t refused_quiesced;
    uint32_t refused_fault;
    uint32_t closes_early;      /* locked before the deadline on request */
    uint32_t closes_quiesce;    /* locked because the system was quiesced */
    uint32_t relay_errors;
    uint32_t late_closes;       /* locked more than 100 ms after the deadline (the owner task was busy) */
    uint32_t ignored_relays;    /* door relays beyond XIAOTAI_LOCK_MAX */
} xiaotai_lock_stats_t;

/* [any] */
void xiaotai_lock_stats(xiaotai_lock_stats_t *stats);

/* ----------------------------------------------------------------- lifecycle */

/* Called by the framework through the generated tables. init: claims the
 * output family "door" (its result is init's when it is not XIAOTAI_OK),
 * finds the door relays (the board's parts are known: xiaotai_hal_init() runs
 * before the framework starts), registers the setting, the intents, the command AT+LOCK (status; and, only
 * with the build option XIAOTAI_LOCK_AT_OPEN, OPEN and CLOSE) and the
 * diagnostics provider "lock"; subscribes to XIAOTAI_EVENT_QUIESCE.
 *   AT+LOCK          one line per lock: +LOCK: index=<n> role=<role>
 *                    state=<locked|released|fault> door=<unknown|closed|open>
 *                    remaining_ms=<n> opens=<n> error=<n>
 *   AT+LOCK OPEN [<index> [<ms>]]   xiaotai_lock_open(index or 0, ms or 0)
 *   AT+LOCK CLOSE [<index>]         xiaotai_lock_close(index), every lock
 *                                   without an index
 * start: locks every lock. A board without a relay in the family "door" (its
 * relays all have other roles, or it has none) lacks door_lock and fails the
 * hardware need check, at build selection and in xiaotai_start(), before this
 * module is initialized, so a product with this module has a door relay. */
xiaotai_err_t xiaotai_module_lock_init(void);
void xiaotai_module_lock_start(void);

/* Host tests: forget locks, configuration and counters. */
void xiaotai_lock_reset(void);

#ifdef __cplusplus
}
#endif
