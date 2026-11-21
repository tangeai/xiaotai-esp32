// SPDX-License-Identifier: MIT
#pragma once
/* Internals shared by the sources of the binding module. Everything here runs
 * on the framework owner task unless a comment says otherwise; the only state
 * read from other tasks is the status snapshot and the counters, both behind
 * the module's critical section (binding.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Event-log source of the module (a fixed word, xiaotai/eventlog.h). */
#define BINDING_LOG_SOURCE "binding"

/* Defaults of xiaotai_binding_config_t (0 in a field selects them). */
#define BINDING_DEFAULT_WAIT_S 190U
#define BINDING_DEFAULT_CONNECT_S 20U
#define BINDING_DEFAULT_STALE_RETRIES 3U
#define BINDING_DEFAULT_ANNOUNCE_ROUNDS 1U
#define BINDING_DEFAULT_ANNOUNCE_GAP_MS 350U

/* The report: path, timeout, and the wait before an old code is reported
 * again (deployed firmware: 15 s, 1000 ms). */
#define BINDING_REPORT_PATH "/v1/device/report"
#define BINDING_REPORT_TIMEOUT_MS 15000U
#define BINDING_STALE_DELAY_MS 1000U
/* A temporary token with less than this to live is an old code replayed. */
#define BINDING_STALE_BELOW_MS 1000
/* Business code of a successful report. */
#define BINDING_REPORT_CODE_OK 200

/* What the module asks of the platform core's pools (specification 2.7):
 * the report body {"mac":"AA:BB:CC:DD:EE:FF"}; the report reply carries a
 * temporary token of up to 1 KiB; a grant message. */
#define BINDING_REQUEST_BODY_BYTES 64U
#define BINDING_RESPONSE_BYTES 2048U
#define BINDING_SIGNAL_BYTES 1024U

/* Most JSON tokens a report reply or a grant may need. A reply needs about 10;
 * anything far larger is not a reply of this service. */
#define BINDING_JSON_TOKENS_MAX 128U

/* While a storage job of the identity component runs, the owner task also
 * looks for its result at this interval, so a result whose hand-off to the
 * owner queue failed (queue full) is still picked up. */
#define BINDING_STORAGE_POLL_MS 200U

/* One attempt, from PREPARING to its end. XIAOTAI_MEM_BULK, wiped and freed
 * when the attempt ends (it holds the temporary token and the granted key). */
typedef struct {
    uint32_t serial;                 /* stamps every callback of this attempt */
    uint32_t identity_generation;    /* a retained first-binding window belongs to this generation */
    bool signed_report;              /* an identity was VALID at the start */
    bool network_suspended;          /* the old guest must retire before reopening on Wi-Fi recovery */
    uint8_t stale_reports;           /* reports answered with an old code */
    uint8_t prepare_index;           /* step running in PREPARING */
    uint32_t prepare_call;           /* stamps the running step's done */
    bool prepare_in_call;            /* inside the step's prepare function */
    bool prepare_done_early;         /* done arrived before prepare returned */
    xiaotai_err_t prepare_early_result;
    char fingerprint[XIAOTAI_IDENTITY_FINGERPRINT_MAX + 1U];
    char code[XIAOTAI_BINDING_CODE_MAX + 1U];
    char temp_token[XIAOTAI_PLATFORM_BEARER_MAX];
    char temp_client_id[XIAOTAI_IDENTITY_CLIENT_ID_MAX + 1U];
    /* Window: fixed when CONNECTING begins (binding_window.c). */
    int64_t deadline_us;
    /* The granted identity; has_identity false: the grant keeps the stored one. */
    xiaotai_identity_t granted;
    bool grant_has_identity;
    bool grant_received;
    bool identity_saved;             /* a new identity was stored by this attempt */
    uint32_t guest;                  /* guest session id, 0 when none */
    xiaotai_err_t guest_closed;      /* why the session ended by itself; XIAOTAI_OK while open */
    /* A cancel condition seen in STORING or CONFIRMING, which run to their
     * end; it ends the attempt once the store has finished. */
    bool cancel_latched;
} binding_attempt_t;

/* What a grant message turned out to be (binding_grant_parse). */
typedef enum {
    BINDING_GRANT_OTHER = 0,     /* not a grant: ignored */
    BINDING_GRANT_IDENTITY,      /* carries both credentials */
    BINDING_GRANT_KEEP,          /* keeps the stored identity */
    BINDING_GRANT_MALFORMED,
} binding_grant_kind_t;

/* Voice part (src/binding_announce.c or src/announce_none.c, work package
 * MB-2) attaches itself from the module's init; without it nothing is
 * spoken and nothing needs stopping. */
typedef struct {
    /* [owner] Stop a running announcement; the voice part reports its end
     * (XIAOTAI_EVENT_BINDING_ANNOUNCED with XIAOTAI_ERR_INVALID_STATE). */
    void (*halt)(void);
    /* [any] An announcement is running. */
    bool (*active)(void);
} binding_voice_t;

/* ----------------------------------------------------------- binding.c */

/* The effective configuration (defaults applied). */
const xiaotai_binding_config_t *binding_config(void);
/* The running attempt, NULL outside PREPARING..CONFIRMING. */
binding_attempt_t *binding_attempt(void);
/* Current phase (owner task). */
xiaotai_binding_phase_t binding_phase(void);
/* Enter a phase of the running attempt (REPORTING..CONFIRMING). */
void binding_enter(xiaotai_binding_phase_t phase);
/* End the running attempt in FAILED with this error; step: the text of
 * XIAOTAI_EVENT_BINDING_FAILED. code: business code of a refused report, or
 * XIAOTAI_PLATFORM_NO_CODE. */
void binding_fail(xiaotai_err_t error, const char *step, int32_t code);
/* End the running attempt in EXPIRED (the code's window ended). */
void binding_expire(void);
/* End the running attempt in BOUND and let the platform session continue. */
void binding_succeed(bool new_identity);
/* The text of XIAOTAI_EVENT_BINDING_FAILED for a failure in this phase. */
const char *binding_step_name(xiaotai_binding_phase_t phase);
/* WAITING: publish the code (status and XIAOTAI_EVENT_BINDING_CODE). */
void binding_publish_code(uint32_t seconds_left);
/* Hide or restore a code without minting a new code or extending the window. */
void binding_hide_code(void);
void binding_restore_code(void);
bool binding_code_visible(void);
bool binding_can_resume_window(const binding_attempt_t *attempt);
void binding_resume_window(void);
/* A grant was accepted: stop speaking the code. */
void binding_voice_halt(void);

/* Counters, behind the module's lock. */
typedef enum {
    BINDING_STAT_NEEDED_FIRST = 0,
    BINDING_STAT_NEEDED_REBINDING,
    BINDING_STAT_ATTEMPTS,
    BINDING_STAT_ATTEMPTS_SIGNED,
    BINDING_STAT_REPORTS_REFUSED,
    BINDING_STAT_STALE_CODE_RETRIES,
    BINDING_STAT_CONNECT_TIMEOUTS,
    BINDING_STAT_GRANTS,
    BINDING_STAT_GRANTS_IGNORED,
    BINDING_STAT_GRANTS_MALFORMED,
    BINDING_STAT_GRANTS_MISMATCHED,
    BINDING_STAT_STORES,
    BINDING_STAT_STORE_FAILURES,
    BINDING_STAT_CONFIRMATIONS,
    BINDING_STAT_CONFIRM_TIMEOUTS,
    BINDING_STAT_BOUND,
    BINDING_STAT_FAILED,
    BINDING_STAT_EXPIRED,
    BINDING_STAT_CANCELLED,
    BINDING_STAT_GUARD_REFUSALS,
    BINDING_STAT_AUTO_RESUMED,
    BINDING_STAT_PREPARE_FAILURES,
    BINDING_STAT_PREPARE_TIMEOUTS,
    BINDING_STAT_PREPARE_LATE,
    BINDING_STAT_ANNOUNCEMENTS,
    BINDING_STAT_ANNOUNCEMENTS_SERVER,
    BINDING_STAT_ANNOUNCEMENTS_FAILED,
    BINDING_STAT_ANNOUNCEMENTS_BUSY,
    BINDING_STAT_FORGETS,
    BINDING_STAT_FORGET_FAILURES,
} binding_stat_t;
void binding_count(binding_stat_t stat);

/* The module's phase timer (one at a time: preparation deadline, stale-code
 * delay, connection deadline). Arming cancels the previous one. */
void binding_timer_arm(uint32_t delay_ms, xiaotai_timer_fn_t handler);
void binding_timer_stop(void);
/* The attempt serial a timer or callback context carries. */
static inline void *binding_serial_ctx(uint32_t serial) { return (void *)(uintptr_t)serial; }
/* True when ctx names the running attempt. */
bool binding_serial_current(void *ctx);

/* Re-publish the status (the voice part's announcing flag changed). */
void binding_publish(void);

/* Identity jobs (save, reload, erase), one at a time. Only hand the job to
 * the identity component after begin() reserves its completion poll and gives
 * the ctx to pass with binding_storage_done. deliver then runs on the owner
 * task. cancel() forgets the job (a late done is ignored). */
typedef void (*binding_storage_fn_t)(xiaotai_err_t result);
xiaotai_err_t binding_storage_begin(binding_storage_fn_t deliver, void **ctx);
void binding_storage_cancel(void);
/* [storage] xiaotai_identity_done_fn_t. */
void binding_storage_done(xiaotai_err_t result, void *ctx);

/* Voice part (MB-2): attach once from init. */
void binding_voice_attach(const binding_voice_t *voice);
xiaotai_err_t binding_voice_init(void);
/* Reset part (MB-3): stop a running attempt before the identity is
 * replaced or erased. XIAOTAI_ERR_INVALID_STATE in STORING and CONFIRMING. */
xiaotai_err_t binding_attempt_abort(void);

/* [owner] Admission before opening a maintenance transaction. */
xiaotai_err_t binding_mutation_ready(void);
/* An admitted mutation completes once on the owner; an immediate refusal
 * never calls done. NULL identity erases, otherwise the store copies it. */
typedef void (*binding_mutation_fn_t)(xiaotai_err_t result, void *ctx);
xiaotai_err_t binding_mutate(const xiaotai_identity_t *identity, binding_mutation_fn_t done, void *ctx);
xiaotai_err_t binding_reset_register(void);
bool binding_direct_reset_pending(void);
xiaotai_err_t binding_at_register(void);
xiaotai_err_t xiaotai_binding_at_dev_register(void);

/* Host tests: the preparation deadline (XIAOTAI_BINDING_PREPARE_MS otherwise). */
void binding_test_set_prepare_ms(uint32_t ms);

/* ---------------------------------------------------- binding_report.c */

/* REPORTING: send the device report of the running attempt. */
void binding_report_begin(void);
/* Read a report reply body: data.code (1..16), data.temp_token (1..1023),
 * data.temp_client_id (1..64), all non-empty strings, into the attempt.
 * XIAOTAI_OK, or XIAOTAI_FAIL when a field is missing, of another type, empty
 * or too long, or the body is not JSON. */
xiaotai_err_t binding_report_parse(const char *body, size_t size, binding_attempt_t *attempt);

/* ---------------------------------------------------- binding_window.c */

/* Fix the window of the attempt: min(wait_s, token expiry - now), from now.
 * *stale is set if this Report should be retried without fixing the window.
 * With required expiry: INVALID_RESPONSE for no usable exp, TIMEOUT if expired. */
xiaotai_err_t binding_window_open(binding_attempt_t *attempt, bool *stale);
/* Milliseconds until the window ends, 0 when it has ended or is not fixed. */
uint32_t binding_window_left_ms(const binding_attempt_t *attempt);
/* Whole seconds until the window ends, rounded up; for the status. */
uint32_t binding_window_seconds(int64_t deadline_us);
/* WAITING: arm the window's end (EXPIRED). */
void binding_window_arm(void);
void binding_window_stop(void);
/* Host tests, after the framework was reset: the timer id is gone with it. */
void binding_window_forget(void);

/* ----------------------------------------------------- binding_grant.c */

/* CONNECTING: open the guest session of the running attempt. */
void binding_connect_begin(void);
/* Close the attempt's guest session, if open. */
void binding_guest_close(binding_attempt_t *attempt);
/* Classify a grant message (the envelope's type and its whole JSON text);
 * for BINDING_GRANT_IDENTITY the credentials are copied to *granted. */
binding_grant_kind_t binding_grant_parse(const char *type, const char *json, size_t size, bool tolerant,
                                         xiaotai_identity_t *granted);

#ifdef __cplusplus
}
#endif
