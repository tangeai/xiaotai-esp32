// SPDX-License-Identifier: MIT
#pragma once
/* Internal interfaces of the network engine (private to the component).
 *
 * Structure. One worker task ("xiaotai_net", internal stack) owns the link
 * state machine, every driver call, every part hook and every storage call of
 * the engine. Other tasks reach it through one queue of 64-byte messages:
 * driver events (copied by the driver's emit), jobs (a function and up to
 * NET_JOB_DATA_MAX bytes of arguments, how the public request functions hand
 * work to the worker) and blocking calls (one call slot, for the few public
 * functions that wait for their result). The worker waits on the queue with a
 * timeout that is the nearest armed deadline (net_deadline_set): no timers.
 *
 * Snapshots. The link status, the provisioning status and the scan status are
 * written by the worker only, under the snapshot lock, and copied by readers
 * on any task. A writer edits in place between net_*_edit() and the matching
 * commit, which increases the revision and emits the event that belongs to
 * the change after the lock is released.
 *
 * Units. The engine's parts (link, store, scan, provisioning, gate, serial
 * commands, coprocessor, diagnostics) are units: each fills a net_unit_t and
 * appears once in NET_UNIT_LIST below, which the lifecycle walks at init,
 * start, after the driver started, for every driver event and at reset.
 * The interfaces units offer each other are declared at the end of this
 * file, each with the work package of the component's specification that
 * implements it; a declaration is implemented only when its package lands.
 *
 * Memory. Kernel objects (queue object, snapshot lock, gate lock, counters
 * read by real-time tasks) are static, internal RAM. The engine state (net_t:
 * configuration, credentials, snapshots, counters, the call slot) and the
 * queue's item storage are XIAOTAI_MEM_BULK. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/net.h"
#include "xiaotai/net_config.h"
#include "xiaotai/net_driver.h"
#include "xiaotai/port/os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ messages */

#define NET_MESSAGE_BYTES 64U
/* Argument bytes a job carries inline. */
#define NET_JOB_DATA_MAX 32U

typedef enum {
    NET_MSG_NONE = 0,
    NET_MSG_DRIVER,    /* a driver event, packed (net_event_pack) */
    NET_MSG_JOB,       /* run u.job.fn on the worker */
    NET_MSG_CALL,      /* run the call slot; token is the call's sequence number */
    NET_MSG_STARTED,   /* XIAOTAI_EVENT_STARTED: begin bring-up */
    NET_MSG_STOP,      /* host tests: the worker exits */
} net_message_kind_t;

/* Flags of a packed driver event. */
enum {
    NET_EVENT_SIGNAL_VALID = 1u << 0,
    NET_EVENT_IPV6_VALID = 1u << 1,
};

typedef struct net_message net_message_t;
/* [worker] A job: the message is valid during the call only. */
typedef void (*net_job_fn_t)(const net_message_t *message);

struct net_message {
    uint8_t kind;         /* net_message_kind_t */
    uint8_t event;        /* NET_MSG_DRIVER: xiaotai_net_driver_event_kind_t */
    uint8_t flags;        /* NET_MSG_DRIVER: NET_EVENT_* */
    int8_t signal_dbm;    /* NET_MSG_DRIVER, LOST */
    uint32_t token;       /* NET_MSG_DRIVER: the event's token; NET_MSG_CALL: the call's sequence */
    union {
        uint8_t raw[NET_MESSAGE_BYTES - 8U];
        struct {
            int32_t error;    /* SCAN_DONE */
            int32_t loss;     /* LOST: xiaotai_net_loss_t */
            int32_t code;     /* LOST */
        } result;
        struct {
            char ssid[XIAOTAI_NET_SSID_MAX + 1];
            uint8_t channel;
        } joined;                              /* JOINED */
        struct {
            uint32_t ipv4;
            uint32_t gateway_ipv4;
            uint8_t ipv6[16];
        } address;                             /* ADDRESS */
        struct {
            net_job_fn_t fn;
            uint32_t arg;
            uint8_t size;
            uint8_t data[NET_JOB_DATA_MAX];
        } job;
    } u;
};

/* [any, rt] Driver event <-> queue item. Every field of every kind survives
 * the round trip; fields a kind does not carry come back zero. */
void net_event_pack(const xiaotai_net_driver_event_t *event, net_message_t *message);
void net_event_unpack(const net_message_t *message, xiaotai_net_driver_event_t *event);

/* [any] Queue a job for the worker; size bytes of data are copied inline.
 * XIAOTAI_ERR_INVALID_STATE before init (or after reset),
 * XIAOTAI_ERR_INVALID_SIZE above NET_JOB_DATA_MAX, XIAOTAI_ERR_TIMEOUT when
 * the queue is full (nothing queued; counted in dropped_events). Never
 * waits. */
xiaotai_err_t net_post(net_job_fn_t fn, uint32_t arg, const void *data, size_t size);

/* The driver's emit (xiaotai_net_driver_start_t.emit): copies the event into
 * the queue without waiting. A full queue drops it, counts it in
 * dropped_events and raises the "events lost" mark (net_events_lost_take). */
void net_driver_emit(void *ctx, const xiaotai_net_driver_event_t *event);

/* [worker] True once since a driver event was dropped: the link's state is
 * unknown, the link unit recovers (leave, wait for LOST, attempt again). */
bool net_events_lost_take(void);

/* [any, rt] True on the engine's worker task. */
bool net_on_worker(void);

/* ---------------------------------------------------------- blocking calls */

/* The full coprocessor identity includes the 65-byte digest (with NUL). */
#define NET_CALL_DATA_MAX (sizeof(xiaotai_coprocessor_info_t))

/* The one call slot. fn runs on the worker with the caller's input in data
 * (size bytes) and leaves its output there (size bytes); its result is the
 * call's result. */
typedef struct net_call {
    xiaotai_err_t (*fn)(struct net_call *call);
    bool (*active)(uint32_t token);
    uint32_t arg;
    size_t size;
    uint8_t data[NET_CALL_DATA_MAX];
} net_call_t;

/* [any task that may block, not the worker] Runs fn on the worker and waits
 * at most timeout_ms in all (for the slot, which one caller holds at a time,
 * and for the result). Input and output are copied through the slot, so a
 * caller that timed out leaves nothing of its own behind: the worker still
 * runs the call (a timeout does not mean "not done") and frees the slot
 * after it. out may be NULL; *out_size (optional) receives the output size,
 * cut to out_capacity.
 * XIAOTAI_ERR_INVALID_STATE: before init, or on the worker itself.
 * XIAOTAI_ERR_INVALID_SIZE: in_size above NET_CALL_DATA_MAX.
 * XIAOTAI_ERR_TIMEOUT: the slot stayed taken, the queue was full or the
 * result did not arrive in time. Otherwise fn's result. */
xiaotai_err_t net_call(xiaotai_err_t (*fn)(net_call_t *call), uint32_t arg, const void *in, size_t in_size,
                       void *out, size_t out_capacity, size_t *out_size, uint32_t timeout_ms);
/* Like net_call, except a retired admission token releases the caller even
 * while the worker is joining the caller's server. An inactive request is
 * also rejected before its callback runs. The predicate may not block. */
xiaotai_err_t net_call_cancellable(xiaotai_err_t (*fn)(net_call_t *call), uint32_t token,
                                  bool (*active)(uint32_t), const void *in, size_t in_size,
                                  void *out, size_t out_capacity, uint32_t timeout_ms);

/* ---------------------------------------------------------------- deadlines */

/* Each deadline is one slot; the worker's queue wait ends at the nearest. */
typedef enum {
    NET_DEADLINE_BACKOFF = 0,   /* next attempt (NW2) */
    NET_DEADLINE_ADDRESS,       /* joined without an address (NW2) */
    NET_DEADLINE_LEAVE,         /* waiting for LOST after leave() (NW2) */
    NET_DEADLINE_SIGNAL,        /* background signal sample (NW2) */
    NET_DEADLINE_MAC,           /* station MAC read again (NW2) */
    NET_DEADLINE_CONTROL,       /* delivery of an already-completed control request (NW2) */
    NET_DEADLINE_SCAN,          /* scan watchdog (NW4) */
    NET_DEADLINE_VERIFY,        /* verification time limit (NW5) */
    NET_DEADLINE_PROVISION,     /* begin or resume an accepted submission (NW5) */
    NET_DEADLINE_MAINTENANCE,   /* delivery of a promised restart (NW6) */
    NET_DEADLINE_RESTART,       /* coprocessor reply wait before the restart (NW10) */
    NET_DEADLINE_COPROCESSOR_RELEASE, /* owner delivery of a failed AT quiesce release */
    NET_DEADLINE_COUNT,
} net_deadline_t;

/* [worker] Arms (or re-arms) a slot: fn runs on the worker once
 * xiaotai_time_us() >= due_us, never earlier. due_us 0 clears it. */
void net_deadline_set(net_deadline_t which, int64_t due_us, void (*fn)(void));
/* [worker] */
void net_deadline_clear(net_deadline_t which);
/* [worker] The armed time, 0 when the slot is clear. */
int64_t net_deadline_due(net_deadline_t which);

/* ------------------------------------------------------------------- state */

typedef struct {
    int64_t due_us;          /* 0: clear */
    void (*fn)(void);
} net_deadline_slot_t;

typedef struct {
    void (*joined)(const char *ssid, void *ctx);
    void (*address)(void *ctx);
    void (*lost)(xiaotai_net_loss_t loss, int32_t code, void *ctx);
    void *ctx;
} net_link_observer_t;

typedef enum {
    NET_DRAIN_NONE, NET_DRAIN_HOLD, NET_DRAIN_RETRY, NET_DRAIN_CONNECT,
    NET_DRAIN_VERIFY, NET_DRAIN_VERIFY_FAILED,
} net_drain_t;

typedef struct {
    uint32_t token;
    uint32_t retry_base_ms;
    int64_t started_us, joined_us, drain_until_us;
    net_drain_t drain;
    unsigned leave_calls;
    xiaotai_err_t leave_error;
    xiaotai_net_loss_t verification_loss;
    int32_t verification_code;
    bool radio_ready, joined, deferred_join, drain_exhausted, control_pending;
    bool verifying, first_loss_present;
    uint8_t channel;
    bool retry_waits_for_radio, retry_credentials;
    uint16_t retry_outage_failures;
    net_link_observer_t observer;
    xiaotai_net_credentials_t attempt_credentials;
    xiaotai_net_credentials_t candidate;
    xiaotai_net_driver_event_t verified_address;
} net_link_t;

/* The engine's state, XIAOTAI_MEM_BULK, allocated by init and released by
 * reset. Fields without a note belong to the worker. */
typedef struct {
    xiaotai_net_config_t config;            /* effective: defaults resolved (net_config_resolve) */
    const xiaotai_network_t *part;          /* the part in use; set by start, NULL when none has a driver */
    const xiaotai_net_driver_t *driver;     /* its driver */
    bool driver_running;                    /* the driver's start() succeeded */
    bool restarting;                        /* the immediate restart was called */
    bool coprocessor_power_cycle;            /* worker writes under net_gate_lock; latch after transfer starts */
    bool provisioning_boot;                 /* this boot is a provisioning boot (NW5 decides at init) */
    /* Credentials the link uses (NW2, NW3); never logged. */
    bool has_credentials;
    xiaotai_net_credentials_t credentials;
    net_link_t link;
    /* Accepted candidate retained on the worker until durable save or rejection. */
    xiaotai_net_credentials_t provision_candidate;
    /* Snapshots: the snapshot lock guards these four. */
    xiaotai_net_status_t status;
    xiaotai_net_provision_status_t provision;
    xiaotai_net_scan_status_t scan;
    xiaotai_net_network_t *networks;        /* lazy BULK snapshot, guarded by scan lock */
    size_t network_count;
    xiaotai_net_diag_t diag;                /* dropped_events is kept apart (net_dropped_events) */
    net_deadline_slot_t deadlines[NET_DEADLINE_COUNT];
    net_call_t call;                        /* the call slot; owned by the slot protocol in net.c */
} net_t;

/* NULL before init and after reset. */
extern net_t *net_state;

/* Init: the snapshot lock and the gate lock, then snapshots may be read
 * (zero until the state exists). Close: snapshots read as zero again.
 * Reset: generation, counters and the gate back to zero. */
xiaotai_err_t net_state_open(void);
void net_state_close(void);
void net_state_reset(void);

/* [any] Driver events and requests lost to a full queue (atomic). */
uint32_t net_dropped_events(void);
void net_dropped_count(void);

/* ---------------------------------------------------------------- snapshots */

/* What a commit emits besides increasing the revision. */
typedef enum {
    NET_EMIT_NONE = 0,
    NET_EMIT_STATE,     /* XIAOTAI_EVENT_NET_STATE, value: state */
    NET_EMIT_UP,        /* XIAOTAI_EVENT_NET_UP, value: low 31 bits of the generation */
    NET_EMIT_DOWN,      /* XIAOTAI_EVENT_NET_DOWN, value 0 */
    NET_EMIT_SIGNAL,    /* XIAOTAI_EVENT_NET_SIGNAL, value: dBm, 0 when unknown */
    NET_EMIT_PREPARE,   /* XIAOTAI_EVENT_NET_PREPARE, value: percent, data: the prepare struct */
} net_emit_t;

/* [worker] Lock the snapshots and return the field to change. Every edit
 * ends with exactly one commit or cancel; nothing in between may block. */
xiaotai_net_status_t *net_status_edit(void);
/* [worker] Revision + 1, unlock, then emit (data: a copy of the status,
 * the prepare struct for NET_EMIT_PREPARE). */
void net_status_commit(net_emit_t emit);
/* [worker] Same revision for the optional state event and the link event. */
void net_status_commit_link(bool state_changed, net_emit_t emit);
/* [worker] Unlock without a change. */
void net_snapshot_cancel(void);
/* [worker] Sets the state; a change is committed with NET_EMIT_STATE, the
 * same state again changes nothing. */
void net_set_state(xiaotai_net_state_t state);
/* [any] The state from the snapshot. */
xiaotai_net_state_t net_state_now(void);
/* [init, worker] Publish confirmed storage availability, not the live link. */
void net_saved_credentials_update(bool available);

xiaotai_net_provision_status_t *net_provision_edit(void);
/* [worker] Revision + 1, unlock, emit XIAOTAI_EVENT_NET_PROVISION when asked. */
void net_provision_commit(bool emit);
/* [worker] Retry delivery of the current snapshot without changing its revision. */
xiaotai_err_t net_provision_publish(void);

xiaotai_net_scan_status_t *net_scan_edit(void);
/* [worker] Revision + 1, unlock (the scan status has no event). */
void net_scan_commit(void);

/* [any] The counters under the snapshot lock; done() unlocks. */
xiaotai_net_diag_t *net_diag_edit(void);
void net_diag_done(void);
void net_diag_count_dns(void);
bool net_diag_snapshot(xiaotai_net_diag_t *diag);
xiaotai_err_t net_serial_register(void);
xiaotai_err_t net_serial_write_result(const xiaotai_reply_t *reply, xiaotai_err_t err, bool submit);
#if XIAOTAI_NET_CONSOLE_FORGET
xiaotai_err_t xiaotai_net_console_forget_register(void);
xiaotai_err_t net_store_forget_restart(void);
#endif
xiaotai_err_t net_diagnostics_register(void);
xiaotai_err_t net_diagnostics_run(int argc, const char *const *argv, const xiaotai_reply_t *reply);

/* [any, rt] Address generation: issue gives a new non-zero value never used
 * before in this boot and publishes it; clear publishes 0. [worker] */
uint32_t net_generation_issue(void);
void net_generation_clear(void);
/* [worker] A new non-zero token for join() and scan(). */
uint32_t net_token_next(void);

/* --------------------------------------------------------------- lifecycle */

/* [worker] Bring-up of the part in use (4.4 of the specification): the part's
 * prepare() hook when it has one, then the driver's start(). Outcomes:
 * driver running (units' driver_started; UNCONFIGURED unless a unit moved
 * the state on), BLOCKED with prepare.fault, or the immediate restart.
 * manual_retry is passed to prepare() (xiaotai_net_retry from BLOCKED). */
void net_bringup(bool manual_retry);

/* [worker] The immediate restart (after a coprocessor restarted with other
 * firmware): xiaotai_restart() on the worker, whose stack is internal RAM.
 * Host tests replace the function; where it returns, the engine does
 * nothing more. */
void net_restart_now(void);
void net_set_restart(void (*restart)(void));   /* NULL: xiaotai_restart */
const xiaotai_network_t *net_part_in_use(size_t *index);
void net_report_prepare(xiaotai_network_stage_t stage, uint8_t percent, xiaotai_err_t result);

/* What every driver start() receives (driver_storage, the fallback rule). */
void net_driver_start_args(xiaotai_net_driver_start_t *args);

/* ------------------------------------------------------------------- units */

typedef struct {
    const char *name;
    /* [init] Engine init, after the state is allocated and the built-in
     * drivers are registered, before the worker exists. NULL: nothing. */
    xiaotai_err_t (*init)(void);
    /* [owner] Engine start, after the driver is bound (net_state->part may be
     * NULL). */
    void (*bind)(void);
    /* [worker] The driver's start() succeeded. */
    void (*driver_started)(void);
    /* [worker] Every driver event, in arrival order. */
    void (*driver_event)(const xiaotai_net_driver_event_t *event);
    /* [worker] Optional integration owned by history and provisioning units. */
    void (*link_address)(const xiaotai_net_credentials_t *credentials);
    void (*link_provision)(bool open, xiaotai_net_provision_reason_t reason);
    void (*maintenance_released)(void);
    /* [worker] Undo open runtime resources before a host-test stop. */
    void (*worker_stop)(void);
    /* Host tests: after the worker stopped, before the state is released. */
    void (*reset)(void);
} net_unit_t;

/* Units in dispatch order: X(<the unit's net_unit_t>) per unit. A work
 * package that adds a unit adds its line here. */
#define NET_UNIT_LIST(X) X(net_store_unit) X(net_history_unit) X(net_link_unit) X(net_scan_unit) \
                         X(net_maintenance_unit) X(net_provision_unit) X(net_coprocessor_unit)

/* Host tests: every dispatched driver event is shown to this function first
 * (on the worker). NULL: none. */
void net_set_driver_tap(void (*tap)(const xiaotai_net_driver_event_t *event, void *ctx), void *ctx);

/* ------------------------------------------------------------ configuration */

/* [any] The rejections of xiaotai_net_configure (xiaotai/net.h). */
xiaotai_err_t net_config_check(const xiaotai_net_config_t *config);
/* [any] Every default applied: space, history capacity, AP prefix, the
 * retry policy's first_ms, max_ms and max_credentials_ms. */
void net_config_resolve(const xiaotai_net_config_t *configured, xiaotai_net_config_t *effective);
/* [any] As given to xiaotai_net_configure (all zero without a call). */
void net_config_configured(xiaotai_net_config_t *configured);
/* Init closes configuration (xiaotai_net_configure answers
 * XIAOTAI_ERR_INVALID_STATE); reset forgets it and opens it again. */
void net_config_close(void);
void net_config_reset(void);

/* ----------------------------------------------------------------- drivers */

/* [any] The registered driver of a kind, NULL when none. */
const xiaotai_net_driver_t *net_driver_for(xiaotai_network_kind_t kind);
/* Engine start: binding ends registration (XIAOTAI_ERR_INVALID_STATE). */
void net_drivers_bind(const xiaotai_net_driver_t *bound);
void net_drivers_reset(void);

/* --------------------------------------------------------------------- gate */

/* The gate's state (4.2 of the specification), static internal RAM, guarded
 * by net_gate_lock. Admission is decided inside one critical section and
 * never waits (NW6, src/maintenance.c). */
typedef struct {
    uint32_t maintenance_ticket;   /* 0: free */
    uint32_t next_ticket;
    bool maintenance_restart;      /* held until the restart; never released */
    uint16_t config_ops;           /* ordinary configuration operations running */
    bool coprocessor_op;           /* a coprocessor update or command running */
    bool coprocessor_write;        /* update or rollback, excludes scans */
    bool scan_pending;             /* accepted scan, queued or owning the radio */
    bool radio_ready;              /* worker's radio readiness for write admission */
    /* Bring-up counts as a coprocessor operation: from init (when the part in
     * use has prepare()) until the driver's start() returned, and during every
     * repeated prepare() and start(); not while BLOCKED. Set by the lifecycle. */
    bool bringup;
    bool restart_pending;          /* restart_required, or maintenance ended in a restart */
    bool credential_retire_pending; /* the rejected trial's association is draining */
} net_gate_t;

extern net_gate_t net_gate;
extern xiaotai_critical_t net_gate_lock;

/* [any] Bring-up flag of the gate (see net_gate_t.bringup). */
void net_gate_set_bringup(bool active);
bool net_gate_bringup(void);

/* ================================================================
 * Interfaces of the units that follow. Implemented by the work package named
 * at each group; until it lands nothing calls them.
 * ================================================================ */

/* ---- link and backoff (NW2: src/link.c, src/backoff.c) ---- */

void net_link_queue_recover(void);
void net_units_link_address(const xiaotai_net_credentials_t *credentials);
void net_units_link_provision(bool open, xiaotai_net_provision_reason_t reason);
xiaotai_err_t net_link_control_enqueue(net_job_fn_t fn, uint32_t arg, bool retry);
void net_link_control_finish(xiaotai_err_t result);

/* [worker] Attempt with net_state->credentials now (UNCONFIGURED without). */
void net_link_connect(void);
/* [worker] Leave the network and stay off it (HELD). user: a requested
 * disconnect (status.user_disconnected). */
void net_link_hold(bool user);
void net_link_coprocessor_power_cycle(void);
/* [worker] From WAITING, HELD or BLOCKED: forget the failures and go on. */
void net_link_resume(void);
/* [worker] A join is in flight or a link exists (a scan must wait). */
bool net_link_attempting(void);
/* [worker] The scan ended: an attempt that fell due meanwhile goes now. */
void net_link_scan_finished(void);

/* Verification of submitted credentials: the attempt's events go to the
 * observer instead of the backoff. */
/* [worker] Leaves the present link (its LOST is not reported) and joins with
 * credentials (copied). */
xiaotai_err_t net_link_try(const xiaotai_net_credentials_t *credentials, const net_link_observer_t *observer);
/* [worker] End of a verification: keep the link it brought up (keep_running)
 * or leave it and return to net_state->credentials. */
void net_link_try_end(bool keep);
/* [worker] Keep the rejected trial visible until its outstanding leave ends. */
void net_link_mark_candidate_retiring(void);
/* End a candidate without joining stored credentials (exclusive or restart). */
void net_link_try_discard(void);
/* Take over completion of a queued provisioning control. */
void net_link_control_provision(bool open);

/* Compute the next wait from the latest loss and a 32-bit jitter sample.
 * The stateful policy updates the caller's unjittered base when supplied. */
uint32_t net_backoff_wait_ms(const xiaotai_net_retry_policy_t *policy, uint32_t failures,
                             xiaotai_net_loss_t latest, uint32_t random, uint32_t *previous_base_ms);
/* [any] Pure: the failures-th consecutive failure ends the attempts
 * (retries_max reached). */
bool net_backoff_give_up(const xiaotai_net_retry_policy_t *policy, uint32_t failures);

/* ---- credentials and history (NW3: src/store.c, src/history.c) ---- */

/* [worker, init] XIAOTAI_OK, XIAOTAI_ERR_NOT_FOUND (none stored), or the
 * read error. Applies the layout's read rule. */
xiaotai_err_t net_store_load(xiaotai_net_credentials_t *credentials);
/* [worker] Credentials and the cleared "provision at next boot" mark in one
 * storage job. */
xiaotai_err_t net_store_save(const xiaotai_net_credentials_t *credentials);
/* [worker] Record layout: the whole namespace; plain layout: the entry. */
xiaotai_err_t net_store_forget(void);
/* [worker, init] "Provision at next boot". */
xiaotai_err_t net_store_boot_flag(bool *set);
/* [worker] Sets the mark (with forget: erases the credentials in the same job). */
xiaotai_err_t net_store_request_boot(bool forget);
xiaotai_err_t net_store_clear_boot(void);
/* [any] Length rules: new credentials of method (XIAOTAI_PROVISION_*, 0 for
 * xiaotai_net_set_credentials), with serial_short_passwords applied. */
bool net_credentials_valid_for(const xiaotai_net_credentials_t *credentials, uint32_t method);
/* [worker] The credentials in use obtained an address: remember them (an
 * asynchronous write; deferred while the maintenance gate is held). */
void net_history_remember(const xiaotai_net_credentials_t *credentials);
/* [worker] Maintenance ended: the deferred write goes now. */
void net_history_flush_deferred(void);
/* [worker] Stored password of a network, by name and class. */
xiaotai_err_t net_history_password(const char *ssid, bool secure,
                                   char password[XIAOTAI_NET_PASSWORD_MAX + 1]);
/* [worker] A password for it is stored and the class matches. */
bool net_history_saved(const char *ssid, bool secure);

/* ---- scan (NW4: src/scan.c) ---- */

/* [worker] */
xiaotai_err_t net_scan_start(void);
xiaotai_err_t net_scan_request(uint32_t token, bool (*active)(uint32_t));
/* [worker] Publish saved-only names when opening provisioning before any scan. */
void net_scan_prepare(void);
bool net_scan_running(void);
void net_scan_cancel(void);

/* ---- provisioning (NW5: src/provision.c) ---- */

/* [worker] Concurrent mode: open the methods for reason (no-op when open). */
void net_provision_open(xiaotai_net_provision_reason_t reason);
/* [worker] Close every open method (an address arrived, or on request). */
void net_provision_close_all(void);
bool net_provision_submission_pending(void);

/* Portal's server and DNS lifetime hooks: provided by NW7/NW8 before bind.
 * The provisioning worker owns AP -> server -> DNS ordering. */
typedef struct {
    xiaotai_err_t (*server_start)(uint32_t ipv4);
    xiaotai_err_t (*server_stop)(void);
    xiaotai_err_t (*dns_start)(uint32_t ipv4);
    xiaotai_err_t (*dns_stop)(void);
} net_provision_portal_io_t;
xiaotai_err_t net_provision_portal_register(const net_provision_portal_io_t *io);

/* ---- gate and the promised restart (NW6: src/maintenance.c) ---- */

/* [any] An ordinary configuration operation (store, forget, disconnect,
 * submission). XIAOTAI_ERR_INVALID_STATE while maintenance, a coprocessor
 * operation or a restart is pending. */
xiaotai_err_t net_gate_config_begin(void);
void net_gate_config_end(void);
/* [any] A coprocessor operation. */
xiaotai_err_t net_gate_coprocessor_begin(void);
xiaotai_err_t net_gate_coprocessor_info_begin(void);
xiaotai_err_t net_gate_coprocessor_write_begin(void);
void net_gate_coprocessor_end(void);
/* [any] Coalesces another accepted scan without taking its reservation. */
xiaotai_err_t net_gate_scan_begin(bool *acquired);
void net_gate_scan_end(void);
/* [any] */
bool net_gate_maintenance_held(void);
bool net_gate_restart_pending(void);
/* [worker] Retains history work despite a busy gate or a lost wake. */
void net_maintenance_defer_history(void);
/* [worker] Service a retained maintenance release or restart notification. */
void net_maintenance_work(void);
void net_units_maintenance_released(void);
/* [worker] Companion cache and restart handling (NW10). */
void net_coprocessor_cache_prepare(void);
void net_coprocessor_publish_prepare_identity(const xiaotai_coprocessor_info_t *info, void *ctx);
void net_coprocessor_invalidate(void);
void net_coprocessor_work(void);
/* [any] Transfers an already acquired coprocessor write lease and borrows the
 * image on success. On rejected queue admission, the lease is released. */
xiaotai_err_t net_coprocessor_update_reserved(const uint8_t *image, size_t size,
                                             xiaotai_net_done_fn_t done, void *ctx);
xiaotai_err_t net_companion_at_register(const xiaotai_network_t *part);
void net_companion_at_work(void);
void net_companion_at_reset(void);
/* [worker] Reserve the restart while still holding a configuration lease.
 * On true, commit the final snapshot with restart_required, then publish it
 * before the generic framework restart request is queued. */
bool net_restart_prepare(void);
void net_restart_publish(void);
/* [worker] Promise a restart without changing the provisioning phase. */
void net_restart_required(void);

#ifdef __cplusplus
}
#endif
