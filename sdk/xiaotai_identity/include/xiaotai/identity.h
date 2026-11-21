// SPDX-License-Identifier: MIT
#pragma once
/* Device identity and the binding generation.
 *
 * The identity is what the platform issued when the device was bound: a device
 * id and a device key, plus an optional client id. It lives in persistent
 * storage in the layout the deployed firmware of this product already uses
 * (xiaotai_identity_config_t); this component reads and writes that layout
 * unchanged, so a device keeps its binding across the firmware change and can
 * still be rolled back.
 *
 * The binding generation is a counter of "which binding is this": it advances
 * whenever the binding stops being the one callers knew (the server unbound the
 * device, another identity was stored, the identity was erased). Platform
 * requests and signals carry it; whoever caches data that belongs to a binding
 * (contacts, room assignment, tokens) compares generations and drops what is
 * stale. It is not persistent: it starts at 1 on every boot and is never 0.
 *
 * Dependencies: the port layer, and the runtime framework's header for the
 * setting store its engine init installs (preferences.h); the framework never
 * links this component. The identity engine is part of every product (the
 * selection's base engines: eventlog, at, identity).
 *
 * Task tags: [glue], [init], [any] as in xiaotai/framework.h ("[glue, init]":
 * the glue or an init()); [storage] a storage task, callbacks only: the
 * storage port's worker (xiaotai/port/kv.h) for a save, an erase and
 * preference writes, this component's reload task for a reload. Either way:
 * return quickly, call no storage function (of this header or of the storage
 * port) and hand work to the owner task with xiaotai_run().
 *
 * Tasks. The identity is read on the init task at engine init. A save and an
 * erase (a binding or a local reset) are one asynchronous job each of the
 * storage port's worker, the one storage task of the system, and so are
 * preference writes (xiaotai/preferences.h): a binding or a settings change
 * creates no task, which a board without external RAM could not afford
 * beside a real-time session. A reload is the one exception (decided before
 * the storage port gained xiaotai_kv_read_async; work package I-2 may move
 * the reload onto it and drop the task, which changes no call here): it
 * creates a short-lived task
 * (XIAOTAI_IDENTITY_RELOAD_STACK_BYTES of stack from XIAOTAI_MEM_BULK,
 * priority 1) that reads through the storage port's synchronous calls, runs
 * done and exits. A reload happens only before a binding is retried after a
 * failed save and to leave UNREADABLE, never during a real-time session.
 * Besides that stack, nothing is allocated except the copy of an identity
 * being saved (XIAOTAI_MEM_BULK, until its job ended), so a provisioning
 * boot costs this component its static data only.
 *
 * Secrets: the device key is never logged. Callers that copy an identity out
 * wipe their copy with xiaotai_identity_wipe() when done. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- limits */

/* Text lengths without the terminator; they equal what deployed firmware stores. */
#define XIAOTAI_IDENTITY_DEVICE_ID_MAX 64U
#define XIAOTAI_IDENTITY_DEVICE_KEY_MAX 256U
#define XIAOTAI_IDENTITY_CLIENT_ID_MAX 64U
/* "AA:BB:CC:DD:EE:FF". */
#define XIAOTAI_IDENTITY_FINGERPRINT_MAX 17U
#define XIAOTAI_IDENTITY_MAC_BYTES 6U
/* Stack of the reload task (see the file comment): one identity copy and the
 * storage port's synchronous calls. */
#define XIAOTAI_IDENTITY_RELOAD_STACK_BYTES 3072U

/* -------------------------------------------------------------------- types */

typedef struct {
    char device_id[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U];   /* 1..64 characters */
    char device_key[XIAOTAI_IDENTITY_DEVICE_KEY_MAX + 1U]; /* 1..256 characters; secret */
    /* Stored client id, "" when the device has none (then the default applies,
     * see xiaotai_identity_client_id). Layouts without this field leave it "". */
    char client_id[XIAOTAI_IDENTITY_CLIENT_ID_MAX + 1U];
} xiaotai_identity_t;

typedef enum {
    /* Nothing stored: the device was never bound. Binding may store an identity. */
    XIAOTAI_IDENTITY_ABSENT = 0,
    /* A complete identity is loaded. */
    XIAOTAI_IDENTITY_VALID,
    /* Separate-entry layout only: some entries are missing or out of range, as
     * after power loss in the middle of a multi-entry save. Treated as unbound;
     * binding may overwrite. */
    XIAOTAI_IDENTITY_INCOMPLETE,
    /* Storage failed, or a single-record layout holds something of the wrong
     * shape. The stored data may be a valid identity this firmware cannot see:
     * nothing may overwrite it. Retry with xiaotai_identity_reload(). */
    XIAOTAI_IDENTITY_UNREADABLE,
} xiaotai_identity_state_t;

/* How deployed firmware of this product stores the identity. A further layout
 * is one more value here and one more codec in the implementation; no function
 * of this header changes. */
typedef enum {
    /* Three string entries: "device_id", "secret", "client_id" (optional when
     * read, always written). Saving also removes the obsolete entry "endpoint". */
    XIAOTAI_IDENTITY_LAYOUT_STRINGS = 0,
    /* One blob entry "identity" of exactly 322 bytes:
     * char device_id[65]; char device_key[257]; both terminated and non-empty.
     * It has no client id. */
    XIAOTAI_IDENTITY_LAYOUT_RECORD,
} xiaotai_identity_layout_t;

/* The server compares the fingerprint as text, so every product keeps the
 * spelling its devices were bound with. */
typedef enum {
    XIAOTAI_FINGERPRINT_UPPER_COLON = 0, /* AA:BB:CC:DD:EE:FF */
    XIAOTAI_FINGERPRINT_LOWER_COLON,     /* aa:bb:cc:dd:ee:ff */
} xiaotai_fingerprint_format_t;

typedef struct {
    xiaotai_identity_layout_t layout;
    /* Storage namespace (at most 15 characters, static storage). NULL selects
     * "tirtc_cfg" for XIAOTAI_IDENTITY_LAYOUT_STRINGS; the record layout has no
     * default and must name the namespace its deployed devices use. */
    const char *space;
    xiaotai_fingerprint_format_t fingerprint;
    /* Default client id is "<client_prefix>-<12 lowercase hex digits of the MAC>".
     * NULL: the chip name reported by the system port. Static storage, 1 to
     * XIAOTAI_IDENTITY_CLIENT_ID_MAX - 13 characters. */
    const char *client_prefix;
    /* A storage failure while reading at engine init (a timeout, a flash
     * error, as opposed to an entry that is absent or of the wrong shape)
     * yields INCOMPLETE, so binding may store over it, instead of UNREADABLE.
     * Deployed firmware of some products treats every failed read as "not
     * bound" and binds again; true keeps that behaviour for them. false (the
     * default) refuses to overwrite what could not be read. */
    bool storage_error_unbound;
} xiaotai_identity_config_t;

typedef struct {
    uint32_t loads, load_failures;
    uint32_t saves, save_failures;
    uint32_t erases;
    uint32_t generation_advances;
    /* A save, reload or erase was refused with XIAOTAI_ERR_NO_MEM (see the
     * storage functions). */
    uint32_t rejected_jobs;
} xiaotai_identity_stats_t;

/* [storage] Completion of an identity job: on the storage port's worker for a
 * save or an erase, on the reload task for a reload. */
typedef void (*xiaotai_identity_done_fn_t)(xiaotai_err_t result, void *ctx);

/* ---------------------------------------------------------------- lifecycle */

/* [glue] Product configuration, before the framework starts. NULL or never
 * called: strings layout in "tirtc_cfg", upper-case fingerprint, chip-name
 * prefix. XIAOTAI_ERR_INVALID_ARG for a bad namespace, a record layout
 * without one, an unknown layout or fingerprint format, or a client prefix
 * that is empty or too long; XIAOTAI_ERR_INVALID_STATE once the engine is
 * initialized. */
xiaotai_err_t xiaotai_identity_configure(const xiaotai_identity_config_t *config);

/* Engine lifecycle (called by the framework through the generated tables).
 * Installs the preferences service as the framework's setting store
 * (xiaotai_setting_store_set, before any module registers a setting: the
 * engines initialize first) and takes the preferences listener for it, then
 * loads the stored identity on the calling task, waiting for storage at most
 * XIAOTAI_KV_WAIT_MAX_MS per entry. A missing or unreadable identity is a
 * state, not an init failure: the result is XIAOTAI_OK unless the component's
 * own lock cannot be created or the store cannot be installed. Creates no
 * task and allocates nothing. */
xiaotai_err_t xiaotai_engine_identity_init(void);

/* ------------------------------------------------------------------ reading */

/* [any] State of the loaded identity; ABSENT before the engine is initialized. */
xiaotai_identity_state_t xiaotai_identity_state(void);

/* [any] Copy of the loaded identity. XIAOTAI_ERR_NOT_FOUND unless the state is
 * VALID (*out is then zeroed). The caller owns the copy and wipes it. */
xiaotai_err_t xiaotai_identity_get(xiaotai_identity_t *out);

/* [any] The device id alone, for topics, logs and diagnostics.
 * XIAOTAI_ERR_NOT_FOUND unless VALID; XIAOTAI_ERR_INVALID_SIZE when it does not fit. */
xiaotai_err_t xiaotai_identity_device_id(char *out, size_t capacity);

/* [any] True when the loaded identity has exactly this device id and key. The
 * comparison takes the same time whatever differs. Used to check that a grant
 * for a signed re-binding names the identity the device already has. */
bool xiaotai_identity_matches(const char *device_id, const char *device_key);

/* [any] True when both required fields are present, within their limits, and
 * every field is terminated. Does not touch storage. */
bool xiaotai_identity_valid(const xiaotai_identity_t *identity);

/* ------------------------------------------------------------------ storage */

/* Save and erase hand one asynchronous job to the storage port
 * (xiaotai_kv_commit_async); reload starts the reload task. All three return
 * at once and never wait for flash. XIAOTAI_OK means handed over, and done
 * (may be NULL) is then called exactly once, [storage], after the job ran:
 * its result is final (a handed-over save or erase is never abandoned, so
 * there is no "timed out but maybe written"). Refused, with nothing handed
 * over and done not called:
 *   XIAOTAI_ERR_NO_MEM         the storage port has no free job slot; four
 *                              identity jobs are already in flight; a save
 *                              while an earlier save has not finished or
 *                              without memory for the copy; a reload while a
 *                              reload runs or without memory for its task;
 *                              a save or an erase while a reload runs (it
 *                              reads the entries one by one, and a write
 *                              between them would tear what it reads)
 *   XIAOTAI_ERR_TIMEOUT        the storage port's queue stayed locked for a
 *                              few milliseconds
 *   XIAOTAI_ERR_INVALID_STATE  before the engine is initialized
 * Order: jobs take effect in the order they were handed over. The storage
 * port runs its jobs in order, each one's done before the next starts, and
 * the reads of a reload queue behind every save and erase handed over before
 * it; so a reload right after a save reads what the save stored, and the
 * loaded identity is already updated when the save's done runs. Callers rely
 * only on the [storage] rules for done, not on which task runs it. */

/* [any] Store this identity in the configured layout, in one storage job.
 * The loaded identity (and the state VALID) change only when the job
 * reported XIAOTAI_OK, before done runs; on any failure the previous loaded
 * identity stays, and storage may hold a partial write (the state after the
 * next reload tells). The binding generation advances when the stored device
 * id or key differs from the one loaded before. identity is copied (a
 * XIAOTAI_MEM_BULK copy until the job ended, then wiped);
 * XIAOTAI_ERR_INVALID_ARG when it is not valid. Refused with
 * XIAOTAI_ERR_INVALID_STATE while the state is UNREADABLE. */
xiaotai_err_t xiaotai_identity_save(const xiaotai_identity_t *identity, xiaotai_identity_done_fn_t done,
                                    void *ctx);

/* [any] Read storage again and replace the loaded identity and state, before
 * done runs. Used before retrying a binding whose save failed (storage may
 * hold part of it), and to leave UNREADABLE. Runs on the reload task, each
 * entry read waiting at most XIAOTAI_KV_WAIT_MAX_MS. done receives XIAOTAI_OK
 * when the reads themselves worked (whatever the state), otherwise the
 * storage error, XIAOTAI_ERR_TIMEOUT included (the loaded identity and state
 * then stay). The generation advances when a VALID identity is replaced by a
 * different one or disappears. The task's stack is freed after done
 * returned. */
xiaotai_err_t xiaotai_identity_reload(xiaotai_identity_done_fn_t done, void *ctx);

/* [any] Remove the stored identity (a local reset; never done on a server-side
 * unbind, which keeps the identity for the signed re-binding). Strings layout:
 * its three entries and the obsolete one, which is everything deployed
 * firmware keeps in that namespace. Record layout: only the "identity" entry,
 * because the namespace also holds other owners' data. Afterwards the state
 * is ABSENT and the generation has advanced. */
xiaotai_err_t xiaotai_identity_erase(xiaotai_identity_done_fn_t done, void *ctx);

/* Persist the same erase, but retain the current runtime identity and
 * generation for a caller committing to an immediate local restart. The
 * callback is required and confirms durable storage. After success all other
 * identity jobs are refused until restart. If restart handoff is refused,
 * the caller must publish the committed absence with the second function. */
xiaotai_err_t xiaotai_identity_erase_for_restart(xiaotai_identity_done_fn_t done, void *ctx);
void xiaotai_identity_publish_restart_erase(void);

/* --------------------------------------------------- fingerprint, client id */

/* [any] Fingerprint text for the station MAC, as the platform knows this device:
 * the "mac" of the binding report and the X-Mac header of every login, always
 * from this one function. Normally the configured format. Compatibility rule: when the stored client
 * id is exactly the MAC as 12 upper-case hex digits without separators (how
 * early firmware bound devices), that compact text is the fingerprint.
 * XIAOTAI_ERR_INVALID_SIZE when capacity is below the text and its terminator. */
xiaotai_err_t xiaotai_identity_fingerprint(const uint8_t mac[XIAOTAI_IDENTITY_MAC_BYTES], char *out,
                                           size_t capacity);

/* [any] Client id for the transport: the stored one when not empty, otherwise
 * "<client_prefix>-<12 lowercase hex digits>". */
xiaotai_err_t xiaotai_identity_client_id(const uint8_t mac[XIAOTAI_IDENTITY_MAC_BYTES], char *out,
                                         size_t capacity);

/* ------------------------------------------------------- binding generation */

/* [any] Current binding generation; lock-free, usable on real-time paths.
 * Never 0, so 0 can mean "no generation" in callers' structures. */
uint32_t xiaotai_identity_generation(void);

/* [any] The binding known so far is no longer valid: advance and return the new
 * generation (skips 0 on wrap-around). Called by the platform core when the
 * server reports the device unbound or a lost signal makes the binding
 * uncertain; storage functions above advance it themselves. Nothing is erased. */
uint32_t xiaotai_identity_generation_advance(void);

/* -------------------------------------------------------------- diagnostics */

/* [any] Overwrite with zeros in a way the compiler may not remove. */
void xiaotai_identity_wipe(void *data, size_t size);

/* [any] */
void xiaotai_identity_stats(xiaotai_identity_stats_t *stats);

/* Host tests: forget configuration, identity and counters. Jobs still with
 * the storage port, and a running reload, complete without calling back;
 * tests flush the port (xiaotai_kv_flush) and let a reload finish first. */
void xiaotai_identity_reset(void);

#ifdef __cplusplus
}
#endif
