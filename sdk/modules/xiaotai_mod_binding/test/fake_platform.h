// SPDX-License-Identifier: MIT
#pragma once
/* Fakes of what the binding module calls outside the framework: the platform
 * core (requests with scripted replies, the guest session with injectable
 * ready / message / closed and acknowledgement results, token expiry), the
 * identity component (state, storage jobs finished by the test as if by the
 * storage task), the network engine's status and the clocks (fake wall time
 * and optionally manual monotonic time).
 *
 * Everything the module calls runs on the framework owner task and only
 * records; the fake_* drivers below run the recorded callbacks on the owner
 * task (and wait until it is idle again), except the storage completions,
 * which run on the calling test thread like the storage task would. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/identity.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/mem.h"

/* Wait until the owner task handled everything queued so far. */
bool fake_drain(void);
/* Also drain the events published by those jobs; a failed fence aborts. */
void fake_drain_events(void);
/* Run fn(ctx) on the owner task and wait for it. */
void fake_on_owner(void (*fn)(void *ctx), void *ctx);
/* Use before starting the first fixture; advance once per deadline boundary. */
void fake_clock_start(void);
void fake_clock_advance_to(int64_t at_us);

/* ------------------------------------------------------------- platform */

typedef struct {
    unsigned count;                  /* requests accepted */
    xiaotai_err_t refuse;            /* returned instead of accepting (XIAOTAI_OK: accept) */
    char service[32];
    xiaotai_platform_method_t method;
    char path[XIAOTAI_PLATFORM_PATH_MAX];
    char body[256];
    uint32_t timeout_ms;
    xiaotai_platform_auth_t auth;
    uint32_t flags;
    int64_t at_us;                   /* when the last one was accepted */
    xiaotai_platform_reply_fn_t reply;
    void *ctx;
} fake_request_t;
extern fake_request_t g_request;

typedef struct {
    unsigned opens, closes, acks;
    xiaotai_err_t open_refuse;
    xiaotai_err_t ack_refuse;
    bool open;
    uint32_t id;
    char client_id[XIAOTAI_IDENTITY_CLIENT_ID_MAX + 2U];
    char token[XIAOTAI_PLATFORM_BEARER_MAX + 1U];
    xiaotai_platform_guest_t guest;
    uint32_t ack_timeout_ms;
    bool ack_pending;
    bool ack_with_identity;          /* an identity was VALID when the ack was asked for */
    xiaotai_platform_done_fn_t ack_done;
    void *ack_ctx;
} fake_guest_t;
extern fake_guest_t g_guest;

extern xiaotai_platform_status_t g_platform;
extern unsigned g_platform_retries;
extern xiaotai_platform_requirement_t g_requirement;
/* Order of guest closes and platform retries: "close retry ...". */
extern char g_trace[256];

/* Answer the pending request (on the owner task). */
void fake_reply(int http_status, int32_t code, const char *body);
void fake_reply_error(xiaotai_err_t result);
/* A report reply body with these data fields; NULL leaves a field out. */
const char *fake_report_body(const char *code, const char *token, const char *client_id);
void fake_guest_ready(void);
void fake_guest_unready(void);
void fake_guest_ready_at(int64_t at_us);
void fake_guest_message(const char *type, const char *json);
void fake_guest_closed(xiaotai_err_t reason);
void fake_guest_ack_result(xiaotai_err_t result);

/* Tokens "exp.<unix seconds>" carry an expiry; every other token has none. */
extern int64_t g_unix_ms;

/* ------------------------------------------------------------- identity */

typedef struct {
    xiaotai_identity_state_t state;
    xiaotai_identity_t identity;
    uint32_t generation;
    unsigned saves, reloads, erases;
    xiaotai_err_t save_refuse, reload_refuse, erase_refuse;
    bool save_pending, reload_pending, erase_pending, restart_erase, restart_erase_committed;
    xiaotai_identity_t saving;
    xiaotai_identity_done_fn_t done;
    void *done_ctx;
} fake_identity_t;
extern fake_identity_t g_identity;

void fake_identity_set(const char *device_id, const char *device_key);
/* Finish the pending save or reload on the calling thread (the storage task).
 * A reload that works leaves the identity in `state`. */
void fake_identity_finish_save(xiaotai_err_t result);
void fake_identity_finish_erase(xiaotai_err_t result);
void fake_identity_finish_reload(xiaotai_err_t result, xiaotai_identity_state_t state);

/* ------------------------------------------------------------------ net */

extern xiaotai_net_status_t g_net;
extern bool g_provisioning_boot;

/* --------------------------------------------------------------- memory */

/* Allocations of exactly `watch_size` bytes (the attempt context), seen
 * through --wrap=xiaotai_mem_calloc / xiaotai_mem_free. */
typedef struct {
    size_t watch_size;
    unsigned allocations, frees;
    xiaotai_mem_kind_t kind;          /* of the last one */
    bool freed_wiped;                 /* the last freed one was all zeros */
    void *live;
} fake_mem_t;
extern fake_mem_t g_mem;

/* The next xiaotai_run() outside the framework answers XIAOTAI_ERR_NO_MEM, as
 * with a full owner queue (--wrap=xiaotai_run). */
extern bool g_run_fail_once;
extern unsigned g_run_failures;

/* Everything back to: platform NO_IDENTITY (clock, discovery), network up
 * with MAC AA:BB:CC:DD:EE:01, no identity, nothing recorded. */
void fake_reset(void);
