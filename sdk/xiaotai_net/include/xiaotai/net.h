// SPDX-License-Identifier: MIT
#pragma once
/* Network engine: the device's IP link and how it gets its credentials.
 *
 * The board lists network parts (xiaotai/hal.h); a driver per network kind
 * (xiaotai/net_driver.h) operates the hardware. This engine is everything in
 * between and is the same for every product: the link state machine with its
 * retry backoff, the stored credentials and the history of networks that
 * worked, provisioning (one setup access point with one page and one captive
 * DNS, one serial path, further methods by registration), the maintenance gate
 * and the diagnostics. It replaces the former network port.
 *
 * Ownership of work: one engine worker task runs the state machine, every
 * driver call and every storage call the engine makes. Functions below copy
 * their arguments, queue a request to that worker and return; results arrive
 * as status changes and events. Status functions copy a snapshot and never
 * touch hardware (on a co-processor every radio query is a remote call).
 *
 * Files: this header is what engines, modules and applications use.
 * xiaotai/net_driver.h is the interface of a network kind's driver (one more
 * kind is one more driver). xiaotai/net_config.h holds the build options.
 *
 * Callers: [glue], [init], [owner], [any] and [rt] as in xiaotai/framework.h
 * (an engine calling an [init] function lists "net" in its manifest's
 * depends, so it is initialized after this engine); [blocking] any task that
 * may wait, never the framework owner task and never a real-time task.
 * Nothing here may be called from an interrupt.
 *
 * Lifecycle: init() reads the stored state and creates the worker; start()
 * binds a driver to the network part and allocates nothing on behalf of a
 * driver. Driver bring-up (radio, coprocessor link), which takes internal and
 * DMA memory, begins on XIAOTAI_EVENT_STARTED, after every engine's start()
 * has made its reservations (xiaotai/framework.h).
 *
 * Generations: every time the link gets an address the engine issues a new
 * non-zero address generation. Whoever starts work on the link (platform
 * session, transport) records it and drops results that carry another one.
 * It is not a session generation: links and media sessions take theirs from
 * xiaotai_generation_next() (xiaotai/framework.h), and the two counters are
 * never mixed.
 *
 * Restarts: the orderly restart is the framework's (xiaotai_restart_request
 * in xiaotai/framework.h: quiesce, restart holds, XIAOTAI_EVENT_RESTARTING,
 * delay, flush). This engine has no restart of its own, no restart hold, no
 * delay and no restart event. For the one restart it promises
 * (provision.restart_required) it hands xiaotai_restart_request("network", 0)
 * to the owner task (xiaotai_run). When the system is quiesced by another
 * holder (XIAOTAI_ERR_BUSY) it asks again each time a quiesce ends
 * (XIAOTAI_EVENT_QUIESCE, value 0) until the restart is pending; a holder
 * that caused restart_required (it quiesced before xiaotai_net_reprovision)
 * passes its own ticket to xiaotai_restart_request once it sees
 * restart_required, so nothing begins in between. Whoever has something to
 * finish first takes a framework restart hold; whoever must know that the
 * restart is under way subscribes to XIAOTAI_EVENT_RESTARTING. Only the
 * restart after a finished coprocessor update or rollback (or a bring-up that
 * ends in XIAOTAI_NET_PREPARE_RESTARTING) is immediate (xiaotai_restart in
 * xiaotai/port/sys.h), because the coprocessor is already restarting. The
 * engine's worker task calls it, never the owner task or a caller's task, and
 * only after the caller had its chance to report the outcome (the done of an
 * update returns first; after a rollback command the caller releases the
 * restart, see xiaotai_net_coprocessor_restart_now). The worker's stack is
 * internal RAM (XIAOTAI_NET_WORKER_STACK_BYTES in xiaotai/net_config.h), the
 * one placement that is safe on every board: deployed firmware restarted
 * from an internal stack after an environment switch, while its coprocessor
 * update and rollback restarted from a task with an external-RAM stack; the
 * engine unifies both on the internal stack, as the framework's orderly
 * restart does. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------- credentials */

#define XIAOTAI_NET_SSID_MAX 32U
#define XIAOTAI_NET_PASSWORD_MAX 64U

/* Both fields are terminated byte strings. An SSID is 1..32 bytes of any value
 * except zero (not necessarily UTF-8); a password is empty (open network) or
 * 8..64 bytes. Stored credentials read under XIAOTAI_NET_STORE_PLAIN may have
 * a password of 1..7 bytes (see there); they are used as they are, and a
 * product with xiaotai_net_config_t.serial_short_passwords also accepts such
 * a password from the serial path. */
typedef struct {
    char ssid[XIAOTAI_NET_SSID_MAX + 1];
    char password[XIAOTAI_NET_PASSWORD_MAX + 1];
} xiaotai_net_credentials_t;

/* [any] The length rules above, including termination inside each field: the
 * rule for new credentials (submissions, xiaotai_net_set_credentials), except
 * for the serial path of a product with serial_short_passwords. */
bool xiaotai_net_credentials_valid(const xiaotai_net_credentials_t *credentials);

/* --------------------------------------------------------------- link state */

typedef enum {
    XIAOTAI_NET_STOPPED = 0,   /* engine not started, or the board has no network part with a driver */
    XIAOTAI_NET_PREPARING,     /* the driver brings its hardware up (co-processor check or update) */
    XIAOTAI_NET_BLOCKED,       /* bring-up or start failed; waits for an allowed retry or restart */
    XIAOTAI_NET_UNCONFIGURED,  /* running without usable credentials */
    XIAOTAI_NET_CONNECTING,    /* an attempt is in flight (joining, or waiting for an address) */
    XIAOTAI_NET_UP,            /* has an address */
    XIAOTAI_NET_WAITING,       /* lost or failed; the next attempt follows after the backoff */
    /* No attempts until retry, new credentials or restart: the network was
     * left on request (user_disconnected), or the retry policy gave up after
     * its retries_max (failing, not user_disconnected). */
    XIAOTAI_NET_HELD,
} xiaotai_net_state_t;

/* Bring-up of a network that needs one before it can start. */
typedef enum {
    XIAOTAI_NET_PREPARE_NONE = 0,    /* not needed, or not begun */
    XIAOTAI_NET_PREPARE_CHECKING,    /* waiting for the hardware and reading what it runs */
    XIAOTAI_NET_PREPARE_VERIFYING,   /* checking the firmware image that would be installed */
    XIAOTAI_NET_PREPARE_WRITING,
    XIAOTAI_NET_PREPARE_ACTIVATING,
    XIAOTAI_NET_PREPARE_RESTARTING,  /* the device restarts to meet the new firmware */
    XIAOTAI_NET_PREPARE_READY,
    XIAOTAI_NET_PREPARE_FAILED,
} xiaotai_net_prepare_stage_t;

typedef enum {
    XIAOTAI_NET_FAULT_NONE = 0,
    XIAOTAI_NET_FAULT_NO_LINK,       /* the hardware did not answer; retry may help */
    XIAOTAI_NET_FAULT_UNSUPPORTED,   /* wrong hardware, or firmware this build neither accepts nor may replace */
    XIAOTAI_NET_FAULT_BAD_IMAGE,     /* the image to install is damaged */
    XIAOTAI_NET_FAULT_INTERRUPTED,   /* an earlier update was never confirmed; continues only on request */
    XIAOTAI_NET_FAULT_POWER_CYCLE,   /* an update began during this boot; only a power cycle helps */
    XIAOTAI_NET_FAULT_START,         /* bring-up passed but the network did not start */
    XIAOTAI_NET_FAULT_ERROR,         /* bring-up ended with an error and none of the above; see error */
} xiaotai_net_fault_t;

#define XIAOTAI_NET_DETAIL_MAX 96U

typedef struct {
    xiaotai_net_prepare_stage_t stage;
    xiaotai_net_fault_t fault;                 /* meaningful when stage is FAILED */
    xiaotai_err_t error;                       /* the result behind fault */
    uint8_t percent;                           /* 0..100 over the whole bring-up */
    char detail[XIAOTAI_NET_DETAIL_MAX];       /* what was found, for a service screen; may be "" and may hold '\n' */
    bool retry_blocked;                       /* product fault policy only; not transient gate availability */
} xiaotai_net_prepare_t;

typedef struct {
    xiaotai_net_state_t state;
    /* Increases whenever any field of this struct changes. */
    uint32_t revision;
    /* Identifies the present address: non-zero while UP, 0 otherwise, never
     * reused during one boot. */
    uint32_t generation;
    uint8_t part;                    /* index of the network part in use */
    xiaotai_network_kind_t kind;
    /* The stored network failed XIAOTAI_NET_FAILURES_NOTICE times in a row;
     * attempts continue. Cleared by the next address. */
    bool failing;
    /* xiaotai_net_disconnect was accepted during this boot and no address has
     * been obtained since. */
    bool user_disconnected;
    /* A requested operation (disconnect, retry, reprovision, provisioning
     * close) has not finished; control_error is the result of the last one that
     * finished. Only one may be pending. */
    bool control_pending;
    xiaotai_err_t control_error;
    /* Signal strength, sampled in the background; false when there is no
     * recent sample. */
    bool signal_valid;
    int8_t signal_dbm;
    /* Hardware address of the station side. On a co-processor it is known only
     * after the network started and is read through a remote call. The engine
     * reads it before it publishes UP; when that read fails, UP is published
     * with mac_valid false and the worker reads again every
     * XIAOTAI_NET_MAC_RETRY_MS while UP; once it succeeds the status changes
     * and XIAOTAI_EVENT_NET_UP is emitted again with the same generation. */
    bool mac_valid;
    uint8_t mac[6];
    uint32_t ipv4;                   /* the four address bytes in memory order; 0 unless UP */
    /* A global IPv6 address of the link, where the driver reports one; UP
     * still means an IPv4 address unless the driver's network offers IPv6
     * only (then ipv4 is 0 and ipv6_valid true). */
    bool ipv6_valid;
    uint8_t ipv6[16];
    uint16_t failures;               /* consecutive failed attempts */
    uint32_t retry_in_ms;            /* WAITING: time to the next attempt when this snapshot was taken */
    char ssid[XIAOTAI_NET_SSID_MAX + 1];   /* the network being used or tried; "" without credentials */
    xiaotai_net_prepare_t prepare;
    uint32_t gateway_ipv4;           /* route from the current address event, 0 when not reported */
    uint8_t channel;                 /* primary channel at association, 0 when not reported */
} xiaotai_net_status_t;

/* [any] Snapshot. Before the engine starts: state STOPPED, everything else zero. */
void xiaotai_net_status(xiaotai_net_status_t *status);
/* [any, rt] The generation of the present address, 0 when the link is not UP.
 * One atomic read. */
uint32_t xiaotai_net_generation(void);

/* ------------------------------------------------------------- link control */

/* Common results of the requests below:
 *   XIAOTAI_OK                 queued; the outcome arrives as status and events
 *   XIAOTAI_ERR_INVALID_STATE  engine not started, another request pending, the
 *                              maintenance gate is held, or a restart is pending
 *   XIAOTAI_ERR_NOT_SUPPORTED  the network in use cannot do this
 *   XIAOTAI_ERR_TIMEOUT        the engine's queue is full (dropped, counted) */

/* [any] Try now. WAITING: skips the remaining backoff and forgets the failure
 * count. HELD: resumes with the stored credentials. BLOCKED: repeats the
 * bring-up as a user-requested retry, which may continue an interrupted
 * update. A fault the product excludes from manual retry returns
 * XIAOTAI_ERR_INVALID_STATE. CONNECTING and UP: nothing to do, XIAOTAI_OK. */
xiaotai_err_t xiaotai_net_retry(void);

/* [any] Leave the network and stay off it (state HELD) until xiaotai_net_retry,
 * accepted new credentials or a restart. Stored credentials are kept. Opens
 * provisioning (reason DISCONNECTED) unless provisioning is exclusive on this
 * product. An address that arrives after the request is discarded. */
xiaotai_err_t xiaotai_net_disconnect(void);

/* ------------------------------------------------------ provisioning: status */

typedef enum {
    XIAOTAI_NET_PROVISION_CLOSED = 0,
    XIAOTAI_NET_PROVISION_OPENING,
    XIAOTAI_NET_PROVISION_OPEN,          /* waiting for credentials */
    XIAOTAI_NET_PROVISION_VERIFYING,     /* trying the submitted credentials */
    XIAOTAI_NET_PROVISION_REJECTED,      /* the last submission failed (see error); still accepting */
    XIAOTAI_NET_PROVISION_STORING,       /* accepted, being stored; no further submission is taken */
    XIAOTAI_NET_PROVISION_STORE_FAILED,  /* accepted but not stored; xiaotai_net_provision_store_retry */
    XIAOTAI_NET_PROVISION_DONE,          /* stored; restart_required tells whether a restart follows */
    XIAOTAI_NET_PROVISION_CLOSING,
} xiaotai_net_provision_state_t;

/* Why provisioning is open. Wording for the user belongs to the application. */
typedef enum {
    XIAOTAI_NET_PROVISION_REASON_NONE = 0,
    XIAOTAI_NET_PROVISION_REASON_UNCONFIGURED,  /* no usable stored credentials */
    XIAOTAI_NET_PROVISION_REASON_FAILING,       /* the stored network keeps failing; attempts continue */
    XIAOTAI_NET_PROVISION_REASON_DISCONNECTED,  /* xiaotai_net_disconnect */
    XIAOTAI_NET_PROVISION_REASON_REQUESTED,     /* xiaotai_net_reprovision */
    XIAOTAI_NET_PROVISION_REASON_SUBMISSION,    /* a submission arrived while nothing was open (serial) */
} xiaotai_net_provision_reason_t;

#define XIAOTAI_NET_URL_MAX 48U

typedef struct {
    xiaotai_net_provision_state_t state;
    /* Increases whenever any field of this struct changes. */
    uint32_t revision;
    xiaotai_net_provision_reason_t reason;
    /* XIAOTAI_PROVISION_* methods (xiaotai/hal.h) open now. */
    uint32_t methods;
    /* This boot is a provisioning boot: the product gives provisioning the
     * device to itself, nothing else goes online, and leaving needs a restart.
     * Fixed from engine init until restart. */
    bool exclusive;
    /* Stored state takes effect only after a restart; the engine asks the
     * framework for it (reason "network", see Restarts at the top), which
     * restarts when no restart hold is left. */
    bool restart_required;
    /* REJECTED and STORE_FAILED: XIAOTAI_ERR_TIMEOUT (no address in time),
     * XIAOTAI_ERR_NOT_FOUND (network not found), XIAOTAI_FAIL (join refused,
     * wrong password), or the storage result. */
    xiaotai_err_t error;
    /* The setup access point; "" while the portal method is not open.
     * ap_password "" means an open access point. */
    char ap_ssid[XIAOTAI_NET_SSID_MAX + 1];
    char ap_password[XIAOTAI_NET_PASSWORD_MAX + 1];
    char url[XIAOTAI_NET_URL_MAX];   /* address of the setup page */
    /* The network of the submission being verified or stored; "" otherwise. */
    char ssid[XIAOTAI_NET_SSID_MAX + 1];
} xiaotai_net_provision_status_t;

/* [any] Snapshot. */
void xiaotai_net_provision_status(xiaotai_net_provision_status_t *status);

/* [any; valid after the engine's init] True when this boot is a provisioning
 * boot (see exclusive above). Engines and modules that would go online check
 * it at their start and stay idle. */
bool xiaotai_net_provisioning_boot(void);

/* [any; valid after the engine's init] True when the product switches
 * between station and provisioning only across a restart. Unlike
 * xiaotai_net_provisioning_boot(), this does not report the current boot. */
bool xiaotai_net_provisioning_exclusive(void);

/* ----------------------------------------------------- provisioning: control */

/* [any] Start over with new credentials (reason REQUESTED).
 *   Not exclusive: same as xiaotai_net_disconnect; with forget the stored
 *   credentials and history are erased first.
 *   Exclusive: stores "provision at next boot" (with forget, erases the stored
 *   credentials in the same storage job) and sets restart_required; the old
 *   credentials otherwise stay stored but unused until new ones are stored or
 *   xiaotai_net_provision_close is called in the provisioning boot.
 * The caller makes sure the device is idle first. */
xiaotai_err_t xiaotai_net_reprovision(bool forget);

/* [any] Give up provisioning and return to the stored credentials.
 *   Not exclusive: closes every open method, leaves HELD and tries the stored
 *   credentials again. XIAOTAI_OK when nothing was open.
 *   Exclusive, in a provisioning boot: clears "provision at next boot" and sets
 *   restart_required. XIAOTAI_ERR_NOT_FOUND when there are no stored
 *   credentials to return to.
 * XIAOTAI_ERR_INVALID_STATE while a submission is being verified or stored. */
xiaotai_err_t xiaotai_net_provision_close(void);

typedef struct {
    /* With use_saved the password field must be empty: the engine takes the
     * password stored for this SSID from the history, on the device. */
    xiaotai_net_credentials_t credentials;
    bool use_saved;
    /* The XIAOTAI_PROVISION_* bit of the method that produced it. */
    uint32_t method;
} xiaotai_net_submission_t;

/* [any] Hands credentials to the engine, from any provisioning method. The
 * submission is copied and the caller's copy may be wiped at once.
 *
 * With verification configured (the default) the engine tries them first:
 * VERIFYING, then STORING and DONE, or REJECTED with the previous credentials
 * back in use. Without it they are stored and take effect at restart.
 *
 * Methods listed in unverified_methods (xiaotai_net_config_t) are stored
 * without verification even when the others are verified.
 *
 * XIAOTAI_OK                 accepted for verification or storing
 * XIAOTAI_ERR_INVALID_ARG    length rules (the serial method of a product
 *                            with serial_short_passwords takes any password
 *                            of at most 64 bytes), a zero byte, password with
 *                            use_saved
 * XIAOTAI_ERR_NOT_FOUND      use_saved without a stored password for the SSID
 * XIAOTAI_ERR_BUSY           another submission is being verified or stored,
 *                            or one was accepted and awaits the restart, or
 *                            (with verification only, which must join) a
 *                            scan is running; the page answers 409
 * XIAOTAI_ERR_INVALID_STATE  network not started (still PREPARING or BLOCKED),
 *                            maintenance gate held, restart pending, or
 *                            provisioning is exclusive on this product and
 *                            this is not a provisioning boot; the page
 *                            answers 503
 * XIAOTAI_ERR_NOT_SUPPORTED  the network in use takes no credentials
 * Storing without verification (store_unverified, unverified_methods) never
 * looks at a scan. */
xiaotai_err_t xiaotai_net_provision_submit(const xiaotai_net_submission_t *submission);

/* [any] STORE_FAILED only: store the accepted credentials again. They are kept
 * in memory until stored or until restart. */
xiaotai_err_t xiaotai_net_provision_store_retry(void);

/* A provisioning method: a way for credentials to reach the device. The portal
 * and the serial path are built in; another (for example over a short-range
 * radio) registers itself and calls xiaotai_net_provision_submit. */
typedef struct {
    /* One XIAOTAI_PROVISION_* bit (xiaotai/hal.h); opened only when the
     * network part lists it. The bits are board facts (a method may need a
     * radio), so they live where boards can see them: a new method appends a
     * bit there and a word to the board vocabulary, and this engine needs no
     * change. */
    uint32_t method;
    const char *name;      /* static */
    /* [engine worker] Start accepting. status is the snapshot being published
     * (access point name, URL). May block for a short time. */
    xiaotai_err_t (*open)(void *ctx, const xiaotai_net_provision_status_t *status);
    /* [engine worker] Stop accepting; after it returns the method submits
     * nothing more. */
    xiaotai_err_t (*close)(void *ctx);
    void *ctx;
} xiaotai_net_provision_method_t;

/* [init] The struct must stay valid for the life of the program.
 * XIAOTAI_ERR_INVALID_ARG: no bit, several bits, or a bit already registered. */
xiaotai_err_t xiaotai_net_provision_method_register(const xiaotai_net_provision_method_t *method);

/* ------------------------------------------------------------------ networks */

typedef enum {
    XIAOTAI_NET_SECURITY_OPEN = 0,
    XIAOTAI_NET_SECURITY_WEP,
    XIAOTAI_NET_SECURITY_WPA,
    XIAOTAI_NET_SECURITY_WPA2,
    XIAOTAI_NET_SECURITY_WPA_WPA2,
    XIAOTAI_NET_SECURITY_WPA3,
    XIAOTAI_NET_SECURITY_WPA2_WPA3,
    XIAOTAI_NET_SECURITY_ENTERPRISE,
    XIAOTAI_NET_SECURITY_OTHER,
} xiaotai_net_security_t;

typedef enum {
    XIAOTAI_NET_SCAN_IDLE = 0,   /* nothing scanned yet */
    XIAOTAI_NET_SCAN_QUEUED,
    XIAOTAI_NET_SCAN_RUNNING,
    XIAOTAI_NET_SCAN_READY,
    XIAOTAI_NET_SCAN_BUSY,       /* refused: the station is joining or joined; ask again later */
    XIAOTAI_NET_SCAN_ERROR,      /* see error; XIAOTAI_ERR_TIMEOUT when the scan did not finish */
} xiaotai_net_scan_state_t;

typedef struct {
    char ssid[XIAOTAI_NET_SSID_MAX + 1];   /* bytes; hidden networks are never listed */
    bool available;            /* seen by the last scan */
    bool signal_valid;
    int8_t signal_dbm;
    xiaotai_net_security_t security;
    bool secure;               /* needs a password */
    bool supported;            /* this device can join it: open, WPA2, WPA/WPA2, WPA3, WPA2/WPA3 */
    /* A password for it is stored on the device and the security class matches
     * (a stored password is never offered to an open network of the same name). */
    bool saved;
} xiaotai_net_network_t;

typedef struct {
    xiaotai_net_scan_state_t state;
    uint32_t revision;         /* increases whenever the list or the state changes */
    xiaotai_err_t error;
    bool truncated;            /* more networks were seen than the engine keeps */
    bool history_error;        /* the saved networks could not be read */
} xiaotai_net_scan_status_t;

/* [any] Asks for a scan. Scans are spaced XIAOTAI_NET_SCAN_INTERVAL_MS apart: a
 * request that comes sooner returns XIAOTAI_OK and keeps the present list.
 * A join attempt always goes first (state BUSY). */
xiaotai_err_t xiaotai_net_scan(void);

/* [any] Copies up to capacity networks and returns how many: those seen by the
 * last scan, strongest first, one per name and class (secured or open: an open
 * network never hides a secured one of the same name), the engine's own access
 * point left out, followed by saved networks that were not seen. status may be
 * NULL. The engine keeps at most XIAOTAI_NET_SCAN_MAX seen networks. No
 * password ever leaves the engine through this call. */
size_t xiaotai_net_networks(xiaotai_net_network_t *networks, size_t capacity,
                            xiaotai_net_scan_status_t *status);

/* ------------------------------------------------------- stored credentials */

/* [any] Whether the engine has confirmed a usable saved station record.
 * A live SSID or an in-flight candidate does not imply a saved record.
 * False before init and after an ambiguous credential write or erase until
 * the engine validates storage again. Does not read flash or expose secrets. */
bool xiaotai_net_has_saved_credentials(void);

/* The two calls below wait for storage (at most XIAOTAI_KV_WAIT_MAX_MS): any
 * task that may block, the owner task included for a console command, never a
 * real-time task. XIAOTAI_ERR_INVALID_STATE before the network started (still
 * PREPARING or BLOCKED), while the maintenance gate is held, while a
 * submission is being verified or stored, or while a restart is pending. A
 * timeout does not mean "not written". */

/* [blocking, owner] Stores credentials without trying them and without restarting; they are used
 * by the next attempt (xiaotai_net_retry) or the next boot. Clears "provision
 * at next boot" in the same storage job. The stored record keeps the layout
 * earlier firmware wrote. Credentials follow xiaotai_net_credentials_valid,
 * or with serial_short_passwords the plain layout's read rule (this is the
 * console's AT+NET STORE). XIAOTAI_ERR_INVALID_ARG otherwise. */
xiaotai_err_t xiaotai_net_set_credentials(const xiaotai_net_credentials_t *credentials);
/* [blocking, owner] Erases the stored credentials, the keys of earlier firmware and the history
 * (record layout: the whole namespace in one operation; plain layout: only the
 * credentials entry, because the namespace also holds data of other owners).
 * History is not written again during this boot. Does not disconnect.
 * Its console command AT+NET FORGET exists only in builds with
 * XIAOTAI_NET_CONSOLE_FORGET (xiaotai/net_config.h, off by default), like
 * every other console path that erases stored state. */
xiaotai_err_t xiaotai_net_forget_credentials(void);

/* ------------------------------------------------- maintenance and restart */

/* Maintenance gate: a caller that is about to change what the device is
 * (switching the service environment, a factory operation) closes the door on
 * new network configuration first. It guards the network engine's own state
 * only: winding down sessions and refusing new ones is the framework's
 * quiesce (xiaotai_quiesce_begin in xiaotai/framework.h), which such a caller
 * takes first. While it is held, submissions, stored
 * credential changes, disconnect, reprovision and coprocessor operations
 * are refused; the link itself, attempts in flight and a restart that was
 * already promised are not disturbed, and history writes wait for the end.
 *
 * [any] XIAOTAI_ERR_INVALID_STATE when it cannot be taken now: already held, a
 * request pending, a submission being verified or stored, a restart pending, or
 * a coprocessor operation running. Bring-up counts as a coprocessor operation:
 * from the engine's init, when the part in use has a prepare() hook, until
 * the driver's start() has returned, and again during every repeated prepare()
 * and start() (xiaotai_net_retry); state BLOCKED does not count. Never waits.
 * *ticket is non-zero on success. */
xiaotai_err_t xiaotai_net_maintenance_begin(uint32_t *ticket);
/* [any] Read-only snapshot of an already held maintenance gate, a submitted
 * credential still being processed, or a promised network restart. An idle
 * portal and unrelated network work do not count. This takes no ticket and
 * does not replace the normal maintenance admission check. */
bool xiaotai_net_provisioning_held(void);
/* [any] Releases the gate. A ticket that is not the holder's is ignored. */
void xiaotai_net_maintenance_end(uint32_t ticket);
/* [any] The holder's maintenance ends in a restart: the gate stays held until
 * the device restarts and can no longer be released. The engine does not ask
 * for that restart: the holder, which also holds the framework's quiesce (it
 * took it first), hands that quiesce to xiaotai_restart_request(reason, its
 * quiesce ticket), so nothing begins in between.
 * XIAOTAI_ERR_INVALID_STATE for a wrong ticket. */
xiaotai_err_t xiaotai_net_maintenance_restart(uint32_t ticket);

/* Restart holds, the restart delay, XIAOTAI_EVENT_RESTARTING and the intent
 * "system.restart" are the framework's (xiaotai/framework.h); this engine
 * registers none of them. The intent is refused while the system is quiesced
 * by another holder, which is the case while the maintenance gate is held,
 * since its holder quiesces first. */

/* -------------------------------------------------------------------- events */

/* Emitted on the framework's event bus. args.value carries the number named
 * below; data is a copy of the status struct named below, valid during the
 * handler. */
/* A state transition or completed link control (the state may be unchanged).
 * value: xiaotai_net_state_t; data: xiaotai_net_status_t. */
#define XIAOTAI_EVENT_NET_STATE "net.state"
/* An address was obtained, or (same generation again) the status of the
 * present address changed: the station MAC became known. value: low 31 bits
 * of generation; data: xiaotai_net_status_t. */
#define XIAOTAI_EVENT_NET_UP "net.up"
#define XIAOTAI_EVENT_NET_DOWN "net.down"            /* the address is gone; data: xiaotai_net_status_t */
#define XIAOTAI_EVENT_NET_SIGNAL "net.signal"        /* value: dBm, 0 when unknown; data: xiaotai_net_status_t */
#define XIAOTAI_EVENT_NET_PREPARE "net.prepare"      /* value: percent; data: xiaotai_net_prepare_t */
#define XIAOTAI_EVENT_NET_PROVISION "net.provision"  /* value: xiaotai_net_provision_state_t; data: xiaotai_net_provision_status_t */
/* Saved station availability changed. value: 0 or 1; no data payload. */
#define XIAOTAI_EVENT_NET_CREDENTIALS_CHANGED "net.credentials_changed"
/* A packet path capture started (value 1) or ended (0), diagnostic builds
 * only. The transport reads xiaotai_net_trace_transport_active() on it. */
#define XIAOTAI_EVENT_NET_TRACE "net.trace"

/* --------------------------------------------------------------- diagnostics */

typedef enum {
    XIAOTAI_NET_LOSS_UNKNOWN = 0,
    XIAOTAI_NET_LOSS_REQUESTED,     /* the engine asked the driver to leave */
    XIAOTAI_NET_LOSS_NOT_FOUND,     /* no network of that name was seen at all */
    XIAOTAI_NET_LOSS_CREDENTIALS,   /* refused in a way wrong credentials produce (authentication, handshake) */
    XIAOTAI_NET_LOSS_SIGNAL,        /* the access point stopped being heard */
    XIAOTAI_NET_LOSS_REFUSED,       /* the driver refused to start the attempt */
    XIAOTAI_NET_LOSS_ADDRESS,       /* joined but no address in time */
    /* A network of that name was seen, but not with a security this attempt
     * accepts (no compatible security, or below XIAOTAI_NET_JOIN_REQUIRE_WPA2). */
    XIAOTAI_NET_LOSS_SECURITY,
} xiaotai_net_loss_t;

typedef struct {
    xiaotai_net_loss_t loss;
    int32_t code;            /* the platform's own reason number */
    bool signal_valid;
    int8_t signal_dbm;
    uint32_t uptime_s;       /* when it happened, seconds since boot */
} xiaotai_net_loss_record_t;

#define XIAOTAI_NET_LOSS_HISTORY 8U

typedef struct {
    uint32_t attempts;           /* join attempts started */
    uint32_t addresses;          /* addresses obtained */
    uint32_t losses;
    uint32_t signal_losses;      /* of which XIAOTAI_NET_LOSS_SIGNAL */
    uint32_t stale_events;       /* driver events dropped for an old attempt or scan */
    uint32_t dropped_events;     /* driver events or requests lost to a full queue */
    uint32_t scans;
    uint32_t scan_timeouts;
    uint32_t submissions;
    uint32_t submissions_rejected;
    uint32_t store_failures;
    uint32_t history_deferred;   /* history writes that waited for maintenance to end */
    uint32_t portal_requests;
    uint32_t portal_refused;     /* requests not from the access point side, or after closing */
    uint32_t dns_queries;
    uint32_t backoff_ms;         /* the wait chosen after the latest failure */
    uint32_t join_ms;            /* latest attempt: start to joined */
    uint32_t address_ms;         /* latest attempt: joined to address */
    xiaotai_net_loss_record_t first;                       /* first loss since the last address */
    xiaotai_net_loss_record_t recent[XIAOTAI_NET_LOSS_HISTORY];  /* oldest first */
    uint8_t recent_count;
} xiaotai_net_diag_t;

/* [any] */
void xiaotai_net_diag(xiaotai_net_diag_t *diag);

/* Packet path capture, a diagnostic build option (XIAOTAI_NET_PATH_TRACE): one
 * row of counters per second, no packet contents. The transport feeds the two
 * points only it can see. Without the option, or while no capture runs, the
 * calls return at once. They never block, allocate or log. */
typedef struct {
    /* Which stream the sample belongs to: 0 none, 1 audio, 2 video; each
     * sample counts one in the row's tp_audio or tp_video column. */
    uint32_t media;
    uint32_t rtt_ms;
    uint32_t nack;
    uint32_t retransmitted;
    uint32_t send_loss;
    uint32_t receive_loss;
} xiaotai_net_trace_transport_t;

/* [any, rt] True while a capture is recording. */
bool xiaotai_net_trace_active(void);
/* [any, rt] True while a capture is recording and the build also wants the
 * transport's statistics samples (XIAOTAI_NET_PATH_TRACE_TRANSPORT). The
 * transport's own statistics logging perturbs weak-network timing, so it
 * turns it on only then; XIAOTAI_EVENT_NET_TRACE tells it when to look. */
bool xiaotai_net_trace_transport_active(void);
/* [rt] One media packet reached the application. */
void xiaotai_net_trace_media(bool video);
/* [any, rt] A transport statistics sample (copied). */
void xiaotai_net_trace_transport(const xiaotai_net_trace_transport_t *sample);

/* -------------------------------------------------------------- coprocessor */

/* Link hardware with firmware of its own (a coprocessor) is reached through
 * the hooks of its network part (xiaotai/hal.h). The engine is the only caller
 * of those hooks and never runs two at once. The three calls below return
 * XIAOTAI_ERR_NOT_SUPPORTED when the part in use lacks the hook, and
 * XIAOTAI_ERR_INVALID_STATE before the engine started.
 *
 * Admission of the engine's own console commands. The coprocessor update and
 * rollback commands (spelled AT+COMPANION UPDATE and AT+COMPANION ROLLBACK in
 * the console vocabulary, whose words the console specification owns), like
 * AT+NET TRACE START and the trace dump, refuse (ERROR: BUSY) while a
 * foreground activity exists (xiaotai_activity_current on the owner task,
 * where command handlers run) or the system is quiesced: a call or an AI
 * session is never revoked by a maintenance command. Links and media sessions
 * exist only inside activities, so that is the idle check of deployed
 * firmware (no runtime session, no media, no RTC connection). The two
 * coprocessor commands then take
 * xiaotai_quiesce_begin(XIAOTAI_QUIESCE_COPROCESSOR, ...) (the reason word of
 * xiaotai/framework.h) for the operation, so nothing starts meanwhile; success
 * ends in a restart, failure ends the quiesce. They do not stop the RTC stack,
 * as deployed firmware did not. Other callers of the functions below follow
 * the order of the call's description themselves and quiesce with the same
 * reason. */

/* [blocking; with live false also the owner task] Identity of the coprocessor
 * firmware. live false: what bring-up read, from memory. live true: asks the
 * coprocessor now on the engine's worker and waits (seconds).
 * XIAOTAI_ERR_NOT_FOUND when it is not known or not complete (also once an
 * update began a transfer during this boot). */
xiaotai_err_t xiaotai_net_coprocessor_info(bool live, xiaotai_coprocessor_info_t *info);

/* [engine worker] Outcome of xiaotai_net_coprocessor_update. */
typedef void (*xiaotai_net_done_fn_t)(xiaotai_err_t result, void *ctx);

/* [any] Writes a coprocessor firmware image the caller obtained and verified
 * against its digest. Queued to the engine's worker; image is borrowed until
 * done is called. Progress is published as status.prepare and
 * XIAOTAI_EVENT_NET_PREPARE. On success the engine restarts the device as soon
 * as the part's hook returns (xiaotai_restart on the engine's worker, whose
 * stack is internal RAM: restart holds do not apply, the coprocessor is
 * already restarting) and done is called just before, on the same task: the
 * restart follows done's return, so done writes any report it owes (a
 * console line, which returns once the device took it) before it returns,
 * and returns promptly. On failure the link continues unless prepare.fault is
 * XIAOTAI_NET_FAULT_POWER_CYCLE. In that state retry, further writes and
 * commands other than the exact read-only INFO require a power cycle.
 * The caller makes sure no real-time link or media session is up and none
 * starts until done: it quiesces the system (xiaotai_quiesce_begin in
 * xiaotai/framework.h) and holds the transport down where one is built.
 * XIAOTAI_ERR_INVALID_STATE: network not started, the maintenance gate is
 * held, a request, submission, scan or another coprocessor operation is
 * pending, or a restart is pending. XIAOTAI_ERR_INVALID_ARG: no image, no done. */
xiaotai_err_t xiaotai_net_coprocessor_update(const uint8_t *image, size_t size,
                                             xiaotai_net_done_fn_t done, void *ctx);

/* [blocking] Sends one command of the coprocessor firmware's own vocabulary
 * and copies its text reply (terminated, cut to capacity). restart true marks
 * a command after which the coprocessor restarts with other firmware (a
 * rollback): it is admitted like xiaotai_net_coprocessor_update. The part's
 * hook has already waited for as long as the coprocessor needs (a fact of the
 * board's part, xiaotai/hal.h), so the engine adds no wait for the
 * coprocessor. After XIAOTAI_OK the device restarts (xiaotai_restart on the
 * engine's worker, as for an update; the caller's task never restarts), but
 * only once the caller has reported the outcome: the worker restarts when the
 * caller calls xiaotai_net_coprocessor_restart_now(), or
 * XIAOTAI_NET_COPROCESSOR_REPLY_WAIT_MS (xiaotai/net_config.h) after this
 * call returned if it never does. A caller that reports (the console's
 * rollback reply line) writes its report first and then releases the
 * restart, so the report is out before the device restarts, as deployed
 * firmware printed its rollback line before restarting. Until the restart
 * the engine counts a restart as pending. */
xiaotai_err_t xiaotai_net_coprocessor_command(const char *command, bool restart, char *reply,
                                              size_t capacity);

/* [any] After xiaotai_net_coprocessor_command(..., true, ...) returned
 * XIAOTAI_OK: the caller has reported the outcome; the engine's worker
 * restarts the device now. Returns at once. No effect when no such restart is
 * waiting (the bounded wait already ended it, or no rollback ran). */
void xiaotai_net_coprocessor_restart_now(void);

/* ------------------------------------------------------------- configuration */

/* How deployed firmware of this product stores the credentials. The engine
 * reads and writes that layout unchanged, so a device keeps its network across
 * the firmware change and can still be rolled back. A further layout is one
 * more value here and one more codec in the implementation. */
typedef enum {
    /* Entry "credentials": a blob of exactly 99 bytes {uint8 version = 1;
     * char ssid[33]; char password[65]}. Only when that entry is absent, the
     * string entries "ssid" and "password" of earlier firmware are read (never
     * written). Entry "known": the history of networks that worked, a blob
     * {uint8 version = 1; uint8 count; N x {char ssid[33]; char password[65]}}
     * with N = 5 (492 bytes) or N = 4 (394 bytes), most recent first; both
     * sizes are read and a stored history is rewritten in its own size. A
     * stored history that cannot be read as such (another size, an unknown
     * version, too many entries, an entry failing the credential rules) is
     * never overwritten during this boot: history_error is set and new
     * networks are not remembered, so firmware rolled back and forward again
     * never loses a newer history.
     * Entry "provision": uint8, 1 = provision at next boot (exclusive mode
     * only; absent on devices of earlier firmware). */
    XIAOTAI_NET_STORE_RECORD = 0,
    /* Entry "wifi": a blob of exactly 98 bytes {char ssid[33];
     * char password[65]}. Entry "wifi_provision": uint8, 1 = provision at
     * next boot, erased in the storage job that stores credentials. No
     * history: saved is never set and use_saved finds nothing.
     * Read rule, the one earlier firmware applied: SSID 1..32 bytes, both
     * fields terminated, password at most 64 bytes. Earlier firmware stored
     * passwords of 1..7 bytes too (a WEP key through its serial command), so
     * such a record stays usable; new credentials follow
     * xiaotai_net_credentials_valid. */
    XIAOTAI_NET_STORE_PLAIN,
} xiaotai_net_store_layout_t;

typedef enum {
    /* The setup methods open while the device runs, next to the station side:
     * when there are no credentials, when the stored network keeps failing and
     * on request. Nothing restarts to enter or leave provisioning. */
    XIAOTAI_NET_PROVISIONING_CONCURRENT = 0,
    /* Provisioning has a boot to itself (xiaotai_net_provisioning_boot): chosen
     * at init when there are no usable credentials or "provision at next boot"
     * is stored. For products whose memory cannot hold the access point and
     * the cloud session together. */
    XIAOTAI_NET_PROVISIONING_EXCLUSIVE,
} xiaotai_net_provisioning_mode_t;

/* Most entries of xiaotai_net_retry_policy_t.steps_ms. */
#define XIAOTAI_NET_RETRY_STEPS_MAX 16U

/* Wait before the next attempt after the n-th consecutive failure (n from 1):
 * none for n <= fast_retries, otherwise first_ms doubled for every further
 * failure and capped, plus a random 0..jitter_percent of itself. The cap is
 * max_credentials_ms when the latest loss looks like wrong credentials or an
 * unacceptable security (XIAOTAI_NET_LOSS_CREDENTIALS,
 * XIAOTAI_NET_LOSS_SECURITY), max_ms otherwise: a network that is simply not
 * seen (a router restarting) keeps the short cap. A failure is a failed
 * attempt or the loss of an established link; the count restarts only when
 * an address is obtained (and on xiaotai_net_retry). A field left 0 takes
 * its default: first_ms 1000, max_ms 30000, max_credentials_ms the value of
 * max_ms; fast_retries, jitter_percent, step_count and retries_max default
 * to 0. */
typedef struct {
    uint8_t fast_retries;
    uint8_t jitter_percent;        /* 0..100 */
    /* Double the last capped base. Only link losses consume fast retries;
     * a synchronous join refusal waits using the latest link loss's cap. */
    bool grow_from_previous_base;
    /* Discard an interrupted wait. A stopped-radio join error waits for
     * STARTED or an explicit retry rather than scheduling another join. */
    bool join_immediately_on_radio_start;
    uint32_t first_ms;
    uint32_t max_ms;
    uint32_t max_credentials_ms;
    /* A fixed table instead of the doubling (step_count 1..
     * XIAOTAI_NET_RETRY_STEPS_MAX, static storage): after the n-th
     * consecutive failure beyond fast_retries the wait is
     * steps_ms[min(n - fast_retries, step_count) - 1] plus the jitter, and
     * first_ms, max_ms and max_credentials_ms are not used. A product whose
     * deployed firmware waited by a table that is not a doubling keeps it
     * exactly ({1000, 2000, 4000, 8000, 15000, 30000}). */
    const uint32_t *steps_ms;
    uint8_t step_count;
    /* 0: attempts never stop. Otherwise at most this many attempts follow a
     * run of consecutive failures: the failure after the last of them ends
     * the attempts and the link state becomes HELD (status.failing set,
     * user_disconnected clear) until xiaotai_net_retry, accepted new
     * credentials or a restart. A product whose deployed firmware stopped
     * reconnecting after a number of tries keeps that number here. */
    uint16_t retries_max;
} xiaotai_net_retry_policy_t;

/* Where the driver may put the fallback name server
 * (XIAOTAI_NET_FALLBACK_DNS_IPV4, "" disables it in every rule) before the
 * address is announced. Name servers the network assigned are never replaced.
 * A slot the rule needs that cannot be read: nothing is written, the error is
 * counted, and the address is announced as usual. */
typedef enum {
    /* Fill the last (fallback) slot when it is empty; only that slot is read. */
    XIAOTAI_NET_DNS_FALLBACK_SLOT_EMPTY = 0,
    /* Fill the last slot only when every slot is empty; every slot is read. */
    XIAOTAI_NET_DNS_FALLBACK_ALL_EMPTY,
    /* Never write a name server. */
    XIAOTAI_NET_DNS_FALLBACK_NEVER,
} xiaotai_net_dns_fallback_t;

/* Product configuration. Every field left zero selects the default, and the
 * all-zero struct is what a product gets without calling xiaotai_net_configure. */
typedef struct {
    xiaotai_net_store_layout_t layout;
    /* Storage namespace (at most XIAOTAI_KV_NAME_MAX characters, static
     * storage). NULL selects "wifi_cfg" for XIAOTAI_NET_STORE_RECORD; the
     * plain layout has no default and must name the namespace its deployed
     * devices use. */
    const char *space;
    /* XIAOTAI_NET_STORE_RECORD: entries of a history written where none is
     * stored yet, 4 or 5; 0 selects 5. */
    uint8_t history_capacity;
    xiaotai_net_provisioning_mode_t provisioning;
    /* false: submitted credentials are tried before they are stored. true:
     * they are stored untried and take effect at restart. */
    bool store_unverified;
    /* Concurrent mode with verification only. false: the device restarts after
     * storing (restart_required). true: it keeps running on the link the
     * verification brought up. */
    bool keep_running;
    /* false: a network with a password must offer WPA2 or better to be joined.
     * true: any security the access point offers is accepted. */
    bool accept_weak_security;
    /* Name of the setup access point is this prefix followed by the last two
     * bytes of the station hardware address in upper-case hexadecimal. NULL
     * selects "XiaoTai-"; at most 27 characters, static storage. */
    const char *ap_prefix;
    xiaotai_net_retry_policy_t retry;
    /* XIAOTAI_PROVISION_* methods whose submissions are stored without being
     * tried, as with store_unverified, while the other methods verify. For a
     * product whose deployed firmware verified on its setup page but stored
     * straight from its serial command. Ignored with store_unverified. */
    uint32_t unverified_methods;
    /* true: submissions of the serial method (XIAOTAI_PROVISION_SERIAL) and
     * xiaotai_net_set_credentials accept any password of at most 64 bytes
     * (1..7 included), the read rule of XIAOTAI_NET_STORE_PLAIN, as a
     * deployed serial command did (a WEP key); every other method keeps empty
     * or 8..64. Plain layout only. */
    bool serial_short_passwords;
    xiaotai_net_dns_fallback_t dns_fallback;
    /* false: the platform driver keeps no persistent copy of its own of the
     * station configuration (credentials live only in this engine's storage).
     * true: the driver's own storage stays as the platform sets it by
     * default, for a product whose deployed firmware never turned it off. */
    bool driver_storage;
    /* A driver start failure requires a restart. */
    bool block_retry_after_driver_start_failure;
    /* A blocked companion can be retried manually only after NO_LINK or
     * INTERRUPTED. Other link states and automatic bring-up are unchanged. */
    bool restrict_blocked_retry_to_recoverable;
    /* The exact peer diagnostic command INFO may share a maintenance ticket.
     * It still excludes any live coprocessor operation, bring-up or restart. */
    bool allow_coprocessor_info_during_maintenance;
} xiaotai_net_config_t;

/* [glue] Before xiaotai_engine_net_init(), from the product glue
 * (xiaotai_selected_configure, generated from the "configure" entry of this
 * engine's manifest and the product's parameters); config is copied (the
 * strings and the retry table are not). NULL restores every default.
 * XIAOTAI_ERR_INVALID_ARG: a bad namespace or prefix, a plain layout without
 * a namespace, a history capacity other than 0, 4 and 5, jitter above 100,
 * max_ms below first_ms, steps_ms and step_count not given together or
 * step_count above XIAOTAI_NET_RETRY_STEPS_MAX, serial_short_passwords with
 * the record layout,
 * or an unknown dns_fallback. XIAOTAI_ERR_INVALID_STATE once the engine is
 * initialized. */
xiaotai_err_t xiaotai_net_configure(const xiaotai_net_config_t *config);

/* -------------------------------------------------------------------- engine */

/* Engine lifecycle, called by the framework through the generated tables.
 * init: reads the stored credentials and the boot flag (so
 * xiaotai_net_provisioning_boot is valid afterwards), creates the worker,
 * registers the platform's drivers for kinds nobody registered, the built-in
 * provisioning methods, the commands and the diagnostics provider. start:
 * binds a driver to the first network part that has one; on
 * XIAOTAI_EVENT_STARTED (after every engine's start() made its reservations)
 * the worker begins bring-up and the first attempt; start() returns at once. Parts after it
 * are not used in this version: a fallback between parts (Wi-Fi, then
 * Ethernet) is a later addition to xiaotai_net_config_t whose zero value keeps
 * this behaviour, and status.part already names the part in use. */
xiaotai_err_t xiaotai_engine_net_init(void);
void xiaotai_engine_net_start(void);

/* Host tests: stop the worker, close every method and forget the
 * configuration, the registered drivers and methods, and every counter. */
void xiaotai_net_reset(void);

#ifdef __cplusplus
}
#endif
