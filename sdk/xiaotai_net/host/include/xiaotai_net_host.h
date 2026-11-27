// SPDX-License-Identifier: MIT
#pragma once
/* Test controls of the network engine on the host.
 *
 * The scripted driver. On the host the engine's built-in drivers for the two
 * Wi-Fi kinds are one scripted driver (two driver objects, one script). Every
 * call is recorded; its result and the events it emits come from steps a
 * test queues per function, so a test states "the next join() returns OK
 * and the network then reports JOINED and ADDRESS" before it happens. A
 * call with no step queued returns XIAOTAI_OK and emits nothing (mac() and
 * signal() then answer with the values set below). Events of a step are
 * emitted before the call returns, through the emit function the engine
 * gave the last start(); a token 0 in an event of a join() or scan() step
 * is replaced by that call's token. A step may hold its call until the test
 * releases it, which keeps the engine's worker busy for as long as a test
 * needs (a full queue, a slow coprocessor).
 *
 * The rest reaches into the engine for what a test must see or replace: the
 * configuration as given and as resolved, whether the caller runs on the
 * worker, the immediate restart, a barrier behind everything queued so far,
 * and a tap on the driver events the worker dispatches.
 *
 * The functions are [any] unless noted; none may be called from the
 * engine's worker except xiaotai_net_host_on_worker. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/net.h"
#include "xiaotai/net_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XIAOTAI_NET_HOST_START = 0,
    XIAOTAI_NET_HOST_JOIN,
    XIAOTAI_NET_HOST_LEAVE,
    XIAOTAI_NET_HOST_SCAN,
    XIAOTAI_NET_HOST_SCAN_CANCEL,
    XIAOTAI_NET_HOST_SCAN_READ,
    XIAOTAI_NET_HOST_AP_START,
    XIAOTAI_NET_HOST_AP_STOP,
    XIAOTAI_NET_HOST_SIGNAL,
    XIAOTAI_NET_HOST_MAC,
    XIAOTAI_NET_HOST_REPORT,
    XIAOTAI_NET_HOST_CALLS,
} xiaotai_net_host_call_t;

/* One driver call as the driver saw it. */
typedef struct {
    xiaotai_net_host_call_t call;
    xiaotai_network_kind_t kind;          /* which driver object was called */
    bool on_worker;                       /* called on the engine's worker task */
    int64_t at_us;                        /* xiaotai_time_us() when the call began */
    uint32_t token;                       /* JOIN, SCAN */
    uint32_t flags;                       /* JOIN */
    bool has_credentials;                 /* JOIN: credentials were given (copied) */
    xiaotai_net_credentials_t credentials;
    xiaotai_net_ap_config_t ap;           /* AP_START */
    const xiaotai_network_t *part;        /* START */
    xiaotai_net_driver_start_t start;     /* START: the request as given */
} xiaotai_net_host_record_t;

#define XIAOTAI_NET_HOST_STEP_EVENTS 2U
#define XIAOTAI_NET_HOST_STEPS_MAX 16U
#define XIAOTAI_NET_HOST_RECORDS_MAX 128U

typedef struct {
    xiaotai_err_t result;
    uint8_t event_count;                  /* 0..XIAOTAI_NET_HOST_STEP_EVENTS */
    xiaotai_net_driver_event_t events[XIAOTAI_NET_HOST_STEP_EVENTS];
    bool hold;                            /* block in the call until xiaotai_net_host_release */
} xiaotai_net_host_step_t;

/* Forgets steps, records, the emit function and the values below, and
 * releases a held call. */
void xiaotai_net_host_reset(void);
/* The scripted driver object of a Wi-Fi kind (NULL for other kinds), for a
 * test that registers it itself or compares xiaotai_net_driver_bound(). */
const xiaotai_net_driver_t *xiaotai_net_host_driver(xiaotai_network_kind_t kind);
/* Queues the step of the next call of that function. XIAOTAI_ERR_NO_MEM when
 * XIAOTAI_NET_HOST_STEPS_MAX are queued, XIAOTAI_ERR_INVALID_ARG for a bad
 * call or event count. */
xiaotai_err_t xiaotai_net_host_step(xiaotai_net_host_call_t call, const xiaotai_net_host_step_t *step);
/* True once a call is held (within timeout_ms). */
bool xiaotai_net_host_wait_held(uint32_t timeout_ms);
/* Lets the held call return. */
void xiaotai_net_host_release(void);
/* Copies up to capacity records, oldest first; returns how many there are
 * (at most XIAOTAI_NET_HOST_RECORDS_MAX are kept). */
size_t xiaotai_net_host_records(xiaotai_net_host_record_t *records, size_t capacity);
/* Waits until `count` calls of that function were made (within timeout_ms). */
bool xiaotai_net_host_wait_calls(xiaotai_net_host_call_t call, size_t count, uint32_t timeout_ms);
/* Through the emit function of the last start(). XIAOTAI_ERR_INVALID_STATE
 * before a start. */
xiaotai_err_t xiaotai_net_host_emit(const xiaotai_net_driver_event_t *event);
/* What mac(), signal() and scan_read() answer without a step. */
void xiaotai_net_host_set_mac(xiaotai_err_t result, const uint8_t mac[6]);
void xiaotai_net_host_set_signal(xiaotai_err_t result, int8_t dbm);
void xiaotai_net_host_set_scan(const xiaotai_net_scan_record_t *records, size_t count, bool truncated);

/* The configuration as given to xiaotai_net_configure and as the engine uses
 * it (defaults applied; after init, the engine's own copy). Either may be
 * NULL. */
void xiaotai_net_host_config(xiaotai_net_config_t *configured, xiaotai_net_config_t *effective);
/* [any, the worker too] True on the engine's worker task. */
bool xiaotai_net_host_on_worker(void);
/* Replaces the immediate restart (NULL: xiaotai_restart). Where the
 * replacement returns, the engine stays quiet until reset. */
void xiaotai_net_host_set_restart(void (*restart)(void));
/* Returns XIAOTAI_OK once the worker has handled everything queued before
 * the call; XIAOTAI_ERR_TIMEOUT when the queue is full or the worker does
 * not get there in time; XIAOTAI_ERR_INVALID_STATE before init. */
xiaotai_err_t xiaotai_net_host_sync(uint32_t timeout_ms);
/* Shows every driver event the worker dispatches to tap (on the worker),
 * before the engine's units see it. NULL: none. Forgotten by
 * xiaotai_net_reset. */
void xiaotai_net_host_tap(void (*tap)(const xiaotai_net_driver_event_t *event, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
