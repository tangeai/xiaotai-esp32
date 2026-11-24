// SPDX-License-Identifier: MIT
#pragma once
/* Scripted RTC peer for host tests of the transport and of the modules that
 * link the real transport core (host/script_backend.c).
 *
 * The backend records every call the core makes, answers as the script says
 * and lets the test play the stack's side (STARTED, STOPPED, alerts). It also
 * owns the engine's clock on the host: by default the real monotonic clock;
 * in manual mode time moves only when the test advances it, so timers are
 * checked at absolute instants.
 *
 * Typical use: xiaotai_transport_host_reset(); _clock_manual(0); start the
 * framework with the engine; drive events; xiaotai_transport_host_settle();
 * then read the calls. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/transport.h"
#include "xiaotai/link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Calls the core made into the backend. */
typedef enum {
    XIAOTAI_TRANSPORT_HOST_START = 1,
    XIAOTAI_TRANSPORT_HOST_STOP,
    XIAOTAI_TRANSPORT_HOST_UNINIT,
    XIAOTAI_TRANSPORT_HOST_STATISTICS,   /* value: 1 on, 0 off */
    XIAOTAI_TRANSPORT_HOST_DISCONNECT,   /* value: the connection */
    XIAOTAI_TRANSPORT_HOST_CONNECT,      /* value: the request */
    XIAOTAI_TRANSPORT_HOST_CANCEL,       /* value: the request */
    XIAOTAI_TRANSPORT_HOST_PENDING,      /* value: the outstanding count */
    XIAOTAI_TRANSPORT_HOST_SEND,         /* value: the frame size */
    XIAOTAI_TRANSPORT_HOST_BACKLOG,      /* value: outstanding bytes */
    XIAOTAI_TRANSPORT_HOST_COMMAND,      /* value: the original command word */
    XIAOTAI_TRANSPORT_HOST_SUBSCRIBE,    /* value: kind << 16 | stream << 8 | on */
    XIAOTAI_TRANSPORT_HOST_KEY_FRAME,    /* value: the stream */
    XIAOTAI_TRANSPORT_HOST_BITRATE,      /* value: the starting bit rate */
} xiaotai_transport_host_call_kind_t;

typedef struct {
    xiaotai_transport_host_call_kind_t kind;
    int64_t at_ms;        /* engine clock when the call was made */
    int64_t value;
    xiaotai_err_t result; /* what the backend answered */
    uintptr_t connection; /* 0 for a downcall without a connection */
} xiaotai_transport_host_call_t;

/* Copy of the options of the latest start (test values only). */
typedef struct {
    char device_id[72];
    char device_secret[264];
    char client_id[72];
    char endpoint[260];
    uint32_t send_buffer_bytes;
    uint8_t links;
    uint8_t network_kind;
    uint8_t poll_timeout_ms;
    uint8_t stack_log_level;
    xiaotai_transport_switch_t connect_cache, media_transport;
    bool media_transport_before_init, option_strings_without_nul, endpoint_http_rewrite, endpoint_default_unset;
} xiaotai_transport_host_start_t;

/* What the scripted package can do (xiaotai_rtc_caps). */
typedef struct {
    bool https, cancel_pending, media_transport, stack_log, video, statistics;
    bool published_retirement_barrier;
    uint32_t send_stack_bytes;   /* 0: the unmeasured placeholder */
} xiaotai_transport_host_caps_t;

/* Forgets the script, the records and the stack's state; real clock; the
 * default capabilities (everything but cancel_pending and statistics). */
void xiaotai_transport_host_reset(void);
void xiaotai_transport_host_set_caps(const xiaotai_transport_host_caps_t *caps);

/* ---- clock ---- */
/* From now on the engine clock reads start_ms until advanced. */
void xiaotai_transport_host_clock_manual(int64_t start_ms);
/* Moves the manual clock forward and wakes the engine's worker. */
void xiaotai_transport_host_clock_advance(uint32_t ms);
int64_t xiaotai_transport_host_clock(void);

/* Waits up to timeout_ms (real time) until the worker has nothing left to do
 * at the present engine time. False on timeout. */
bool xiaotai_transport_host_settle(uint32_t timeout_ms);

/* ---- script ---- */
/* The next `times` starts (stops) fail with result and detail. */
void xiaotai_transport_host_fail_start(xiaotai_err_t result, int detail, uint32_t times);
void xiaotai_transport_host_fail_stop(xiaotai_err_t result, int detail, uint32_t times);
/* Report STARTED (STOPPED) from inside a successful start (stop), before it
 * returns, as some packages do. Off by default: the test reports them. */
void xiaotai_transport_host_auto_started(bool in_call);
void xiaotai_transport_host_auto_stopped(bool in_call);
void xiaotai_transport_host_fail_connect(xiaotai_err_t result, int detail, uint32_t times);
void xiaotai_transport_host_fail_disconnect(xiaotai_err_t result, int detail, uint32_t times);
void xiaotai_transport_host_pending(uint32_t count);
/* Sets the fallback backlog for all connections, clearing any overrides. */
void xiaotai_transport_host_backlog_set(size_t bytes);
/* Sets a connection-specific backlog; at most XIAOTAI_TRANSPORT_LINKS_MAX
 * overrides until the next fallback reset. */
xiaotai_err_t xiaotai_transport_host_backlog_for(uintptr_t conn, size_t bytes);
void xiaotai_transport_host_media_fail(xiaotai_transport_host_call_kind_t kind, xiaotai_err_t result,
                                      int detail, uint32_t times);
/* For deterministic tests, invoke from inside a media downcall after it is
 * recorded, with no backend lock held. NULL clears the hook. */
typedef void (*xiaotai_transport_host_media_hook_t)(void *ctx, xiaotai_transport_host_call_kind_t kind,
                                                    uintptr_t conn, int64_t value);
void xiaotai_transport_host_media_hook(xiaotai_transport_host_media_hook_t fn, void *ctx);
/* Called for an accepted submit with no backend lock held. A hook may inject
 * an inline completion or retain the borrowed inputs while the engine owns
 * the request; they must not be read after its release. NULL clears it. */
typedef void (*xiaotai_transport_host_connect_hook_t)(void *ctx, xiaotai_link_kind_t kind, uint32_t request,
                                                       const char *peer, const char *token);
void xiaotai_transport_host_connect_hook(xiaotai_transport_host_connect_hook_t fn, void *ctx);
/* A scripted peer receives accepted command bytes without the backend lock.
 * The data is borrowed for the call only; the hook cannot change its result. */
typedef void (*xiaotai_transport_host_command_hook_t)(void *ctx, uintptr_t conn, uint32_t word,
                                                       const void *data, size_t size);
void xiaotai_transport_host_command_hook(xiaotai_transport_host_command_hook_t fn, void *ctx);
typedef xiaotai_err_t (*xiaotai_transport_host_service_fn)(void *ctx, const char *path, const char *json,
                                    int *status, char *body, size_t capacity, size_t *size);
void xiaotai_transport_host_service(xiaotai_transport_host_service_fn fn, void *ctx);
bool xiaotai_transport_host_last_frame(xiaotai_link_frame_t *frame);

/* ---- the stack's side ---- */
void xiaotai_transport_host_started(void);
/* STOPPED, asked for or not (detail: the stack's code). */
void xiaotai_transport_host_stopped(int detail);
void xiaotai_transport_host_alert(void);
void xiaotai_transport_host_accepted(uintptr_t conn);
void xiaotai_transport_host_connected(uint32_t request, uintptr_t conn, xiaotai_link_end_t reason, int detail);
void xiaotai_transport_host_closed(uintptr_t conn);
/* A close tagged with the original request for a retirement-capable backend. */
void xiaotai_transport_host_closed_for(uintptr_t conn, uint32_t request);
void xiaotai_transport_host_error(uintptr_t conn, xiaotai_link_end_t reason, int detail);
void xiaotai_transport_host_frame(uintptr_t conn, const xiaotai_link_frame_t *frame, const uint8_t *data, size_t size);
void xiaotai_transport_host_command(uintptr_t conn, uint32_t request, uint32_t word, const uint8_t *data, size_t size);
xiaotai_err_t xiaotai_transport_host_peer_subscribe(uintptr_t conn, uint32_t request,
                                                  xiaotai_link_media_kind_t kind, uint8_t stream, bool on);
void xiaotai_transport_host_key_frame(uintptr_t conn, uint8_t stream);
void xiaotai_transport_host_bitrate(uintptr_t conn, uint8_t stream, uint32_t bps);

/* ---- inspection ---- */
/* Copies up to capacity recorded calls, oldest first; returns how many were
 * recorded (at most 256 are kept). */
size_t xiaotai_transport_host_calls(xiaotai_transport_host_call_t *out, size_t capacity);
/* Calls of one kind since the reset. */
uint32_t xiaotai_transport_host_count(xiaotai_transport_host_call_kind_t kind);
/* Engine clock of the latest call of one kind; -1 when none. */
int64_t xiaotai_transport_host_last_at(xiaotai_transport_host_call_kind_t kind);
void xiaotai_transport_host_last_start(xiaotai_transport_host_start_t *out);
/* The stack is initialized (a start succeeded and no uninit followed). */
bool xiaotai_transport_host_initialized(void);
/* Calls that break the backend contract: a start while initialized, an
 * uninit before STOPPED, a stop or uninit while not initialized. */
uint32_t xiaotai_transport_host_violations(void);

#ifdef __cplusplus
}
#endif
