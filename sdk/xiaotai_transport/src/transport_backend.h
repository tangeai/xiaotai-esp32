// SPDX-License-Identifier: MIT
#pragma once
/* RTC backend: the boundary between the portable transport core (src/) and the
 * one piece of code that names an RTC package. Internal to this component.
 *
 *   esp-idf/  the TiRTC backend: the only place that includes the package's
 *             headers, reads its build macros and knows its option order
 *   host/     a scripted peer for host tests
 *
 * The core owns links, generations, profiles, gates, pools, events and every
 * rule of xiaotai/link.h and xiaotai/transport.h. The backend translates: it
 * holds no link state beyond the stack's own handles, decides nothing, and
 * never calls a module.
 *
 * Tags.
 *   [worker]   the transport worker task only; calls never overlap
 *   [control]  any task, under the core's control lock; calls never overlap
 *   [uplink]   a media sending task, concurrently (xiaotai/link.h): the backend
 *              itself never blocks, allocates or logs; the package call it
 *              makes may take the stack's locks and allocate inside its send
 *              buffer. The core times each call (send_call_max_us)
 *   [stack]    upcalls: a thread of the RTC stack (or, for system events, the
 *              worker inside xiaotai_rtc_start/_stop). The core's upcalls
 *              never block, allocate, log or call back into the backend. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/link.h"
#include "xiaotai/port/err.h"
#include "xiaotai/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A connection of the RTC stack. An identity: only the backend dereferences
 * it, and the core never passes one after xiaotai_rtc_disconnect() returned
 * for it or xiaotai_rtc_on_closed() reported it. 0: none. */
typedef uintptr_t xiaotai_rtc_conn_t;

/* What the linked package can do; constant for the life of the program. */
typedef struct {
    const char *version;      /* static */
    bool https;               /* its service requests can use TLS */
    /* Pending outbound requests can be cancelled and their release observed
     * (xiaotai_rtc_cancel, xiaotai_rtc_pending). Without it the core waits for
     * the late result and disconnects it. */
    bool cancel_pending;
    /* A tagged published close ACK followed by pending()==0 proves the
     * object has been freed, rather than only that a callback was delivered. */
    bool published_retirement_barrier;
    bool media_transport;     /* the optional media transport exists */
    bool stack_log;           /* log level and filter are available */
    bool video;               /* video frames, key-frame requests and bitrate feedback */
    bool statistics;          /* per-connection transport statistics can be switched on */
    /* Stack one xiaotai_rtc_send of an audio frame uses below its caller, as
     * measured on the bench for this package; XIAOTAI_RTC_SEND_STACK_UNMEASURED
     * until measured, never 0. Copied into the media sink's send_stack_bytes
     * (xiaotai/link.h). */
    uint32_t send_stack_bytes;
} xiaotai_rtc_caps_t;

/* send_stack_bytes of a package whose send has not been measured on the
 * bench. The only reading of a task that sends audio through the RTC stack
 * today is a 10240-byte control task that peaked at 6596 bytes over its
 * walked paths, the send included; that peak, rounded up to 256, bounds the
 * send of that package and stands in for the others until each is measured.
 * A measured package reports its own value; this one is never lowered by
 * guess. */
#define XIAOTAI_RTC_SEND_STACK_UNMEASURED 6656U

/* Everything a start needs. Strings are valid during the call only. */
typedef struct {
    const char *device_id;
    const char *device_secret;
    const char *client_id;    /* "" or NULL: none */
    /* "" or NULL: the stack's built-in entry (the core never starts without
     * one where the product sets endpoint_required). The backend applies
     * endpoint_http_rewrite to it when the package has no TLS. */
    const char *endpoint;
    uint32_t send_buffer_bytes;
    uint32_t vendor_idle_stack_bytes;
    uint8_t links;
    uint8_t network_kind;     /* xiaotai_network_kind_t of the network in use */
    uint8_t poll_timeout_ms;  /* 0: leave the stack's own */
    xiaotai_transport_switch_t connect_cache;
    xiaotai_transport_switch_t media_transport;
    uint8_t stack_log_level;
    /* The product's rules of xiaotai_transport_config_t, as given. */
    bool media_transport_before_init;
    bool option_strings_without_nul;
    bool endpoint_http_rewrite;
    bool endpoint_default_unset;
} xiaotai_rtc_start_t;

typedef enum {
    XIAOTAI_RTC_STARTED = 0,
    /* Also unsolicited, while started: the core then ends every link,
     * releases the stack and recovers as unsolicited_stop says
     * (xiaotai/transport.h). */
    XIAOTAI_RTC_STOPPED,
    XIAOTAI_RTC_ALERT,        /* the stack saw its service requests redirected */
} xiaotai_rtc_system_t;

/* Counters the backend keeps for diagnostics. */
typedef struct {
    uint32_t log_bytes;       /* stack log text counted, not printed */
    uint32_t log_facts;       /* known lines turned into facts */
    /* In-stream messages of the stack (not commands). Nothing consumes them,
     * so they are counted and discarded; a count above 0 shows that a peer
     * started using them. */
    uint32_t messages;
    uint32_t statistics;          /* statistics records turned into samples */
    uint32_t statistics_skipped;  /* statistics records in a format the backend does not know */
    int service_status;       /* HTTP status or service code of the last failed start; 0: none */
} xiaotai_rtc_diag_t;

/* ------------------------------------------------------- backend, downcalls */

/* [any] */
const xiaotai_rtc_caps_t *xiaotai_rtc_caps(void);
void xiaotai_rtc_diag(xiaotai_rtc_diag_t *diag);

/* [any] The stack's own log threshold, changed at run time from the console
 * (`AT+LOGLEVEL TIRTC <n>`, the BK7258 line's `log-level tirtc <n>`): 0 off,
 * 1..5 error to verbose, above 10 its lower layers as well (much output).
 * Never saved: a start applies the product's stack_log_level again. While a
 * level set here is above 0 the stack's lines are also printed to the system
 * log under the tag "tirtc"; they can hold credentials, so they are printed
 * only on request. XIAOTAI_ERR_INVALID_ARG above XIAOTAI_RTC_LOG_LEVEL_MAX,
 * XIAOTAI_ERR_NOT_SUPPORTED without caps.stack_log. */
#define XIAOTAI_RTC_LOG_LEVEL_MAX 17U
xiaotai_err_t xiaotai_rtc_set_log_level(uint8_t level);
/* [any] The current threshold; *printed: lines go to the system log. */
uint8_t xiaotai_rtc_log_level(bool *printed);

/* [worker] Initializes the stack, applies the options in the order the
 * package requires and starts it. XIAOTAI_OK means accepted: STARTED follows
 * as an upcall, possibly before this returns. On failure the stack is left
 * uninitialized, so the call can simply be repeated; *detail is its code. */
xiaotai_err_t xiaotai_rtc_start(const xiaotai_rtc_start_t *options, int *detail);
/* [worker] Asks the stack to stop; STOPPED follows as an upcall, possibly
 * before this returns. INVALID_STATE when it is not started. Also the first
 * step of every fault (xiaotai/transport.h). */
xiaotai_err_t xiaotai_rtc_stop(int *detail);
/* [worker] Releases the stack. Only after STOPPED, never from an upcall. */
void xiaotai_rtc_uninit(void);

/* [worker] Switches the stack's per-connection transport statistics on or
 * off (off after start). While on, the backend turns each statistics record
 * into one xiaotai_net_trace_transport() sample on the stack's thread,
 * without allocating or logging; a record it does not recognise exactly is
 * counted and skipped. Called only with what xiaotai_net_trace_transport_active()
 * says; nothing without the statistics capability. */
void xiaotai_rtc_set_statistics(bool enabled);

/* [worker] Submits an outbound request of kind XIAOTAI_LINK_WHIP or _P2P.
 * request (never 0) comes back in xiaotai_rtc_on_connected exactly once on
 * XIAOTAI_OK, never otherwise. peer and token are valid during the call and
 * the core keeps its copies until the request has ended. */
xiaotai_err_t xiaotai_rtc_connect(xiaotai_link_kind_t kind, const char *peer, const char *token, uint32_t request,
                                  int *detail);
/* [worker] Withdraws a pending request. NOT_SUPPORTED without cancel_pending. */
xiaotai_err_t xiaotai_rtc_cancel(uint32_t request);
/* [worker] Requests whose resources the stack still holds; 0 without
 * cancel_pending. */
uint32_t xiaotai_rtc_pending(void);
/* [worker] Releases a connection. After it returns the handle is dead for the
 * core whatever the result; a failure is a fault of the stack unless the
 * product sets release_failures_tolerated. */
xiaotai_err_t xiaotai_rtc_disconnect(xiaotai_rtc_conn_t conn, int *detail);

/* [worker] One request to the RTC service as this device; blocks until
 * answered. json NULL: no body. On XIAOTAI_OK *status is 0 and body holds the
 * terminated answer (*size without the terminator, 0 when none or when it
 * did not fit). On XIAOTAI_FAIL *status is the HTTP status, the service's
 * code, or the stack's code. */
xiaotai_err_t xiaotai_rtc_service_request(const char *path, const char *json, int *status, char *body,
                                          size_t capacity, size_t *size);

/* [uplink] Sends one frame; frame->stream and format are final. XIAOTAI_OK:
 * taken. BUSY: the stack's buffer is full. INVALID_STATE: the handle is not a
 * connection. FAIL: anything else. *detail: the stack's code when not OK. */
xiaotai_err_t xiaotai_rtc_send(xiaotai_rtc_conn_t conn, const xiaotai_link_frame_t *frame, const uint8_t *data,
                               size_t size, int *detail);
/* [uplink] Bytes accepted and not yet sent; 0 for a handle that is not a
 * connection. */
size_t xiaotai_rtc_backlog(xiaotai_rtc_conn_t conn);
/* [any] False when the stack cannot frame this format (a codec, rate or
 * channel count it has no wire value for); the core then refuses the profile
 * with XIAOTAI_ERR_NOT_SUPPORTED. */
bool xiaotai_rtc_format_supported(xiaotai_link_media_kind_t kind, const xiaotai_link_format_t *format);

/* [control] */
xiaotai_err_t xiaotai_rtc_command(xiaotai_rtc_conn_t conn, uint32_t word, const void *data, size_t size);
xiaotai_err_t xiaotai_rtc_subscribe(xiaotai_rtc_conn_t conn, xiaotai_link_media_kind_t kind, uint8_t stream,
                                    bool subscribed);
xiaotai_err_t xiaotai_rtc_request_key_frame(xiaotai_rtc_conn_t conn, uint8_t stream);
xiaotai_err_t xiaotai_rtc_set_video_bitrate(xiaotai_rtc_conn_t conn, uint8_t stream, uint32_t min_bps,
                                            uint32_t start_bps, uint32_t max_bps);

/* ------------------------------------------------------------ core, upcalls */

/* [stack] detail: the stack's code, 0 when none. */
void xiaotai_rtc_on_system(xiaotai_rtc_system_t event, int detail);
/* [stack] A peer dialled in. The stack names neither the peer nor a service
 * here, so the core admits by listener mode alone (xiaotai/link.h) or records
 * a refusal; a refused connection is released later by the worker, never
 * inside the upcall. */
void xiaotai_rtc_on_accepted(xiaotai_rtc_conn_t conn);
/* An audited backend may attach a generation during its live peer callback.
 * The tagger runs outside the core lock, only for a matching opted-in link. */
typedef bool (*xiaotai_rtc_peer_tag_fn)(xiaotai_rtc_conn_t conn, uint32_t generation);
void xiaotai_rtc_on_accepted_peer(xiaotai_rtc_conn_t conn, xiaotai_rtc_peer_tag_fn tag);
/* [stack] Result of xiaotai_rtc_connect. conn 0: failed, reason and detail say
 * why (reason is the backend's classification of the stack's code); events
 * bound early to this request are then dropped and counted. */
void xiaotai_rtc_on_connected(uint32_t request, xiaotai_rtc_conn_t conn, xiaotai_link_end_t reason, int detail);
void xiaotai_rtc_on_connected_peer(uint32_t request, xiaotai_rtc_conn_t conn, xiaotai_link_end_t reason, int detail,
                                   xiaotai_rtc_peer_tag_fn tag);
/* [stack] A connection broke; it is still to be released by the worker. */
void xiaotai_rtc_on_error(xiaotai_rtc_conn_t conn, xiaotai_link_end_t reason, int detail);
/* [stack] The stack finished releasing a connection. */
void xiaotai_rtc_on_closed(xiaotai_rtc_conn_t conn);
/* Tagged ACK from a backend that provides published_retirement_barrier. */
void xiaotai_rtc_on_closed_for(xiaotai_rtc_conn_t conn, uint32_t generation);
/* [stack] One received frame; data is valid during the call. A frame in a
 * format xiaotai_link_codec_t cannot name arrives with codec NONE. */
void xiaotai_rtc_on_frame(xiaotai_rtc_conn_t conn, const xiaotai_link_frame_t *frame, const uint8_t *data,
                          size_t size);
/* Upcalls that may precede the connect result. request is the number the
 * stack carries for this connection (the outbound request or an admitted
 * inbound retirement tag), 0 when it carries none. A connection the core has
 * not seen yet (its xiaotai_rtc_on_connected has not arrived) is bound to the
 * request named here, or, when request is 0, to the only outbound request
 * still waiting for its result; with none or several, the event is dropped
 * and counted. Bound events wait in the link's place and are delivered after
 * opened(). */
/* [stack] data may be NULL when size is 0; valid during the call. */
void xiaotai_rtc_on_command(xiaotai_rtc_conn_t conn, uint32_t request, uint32_t word, const uint8_t *data,
                            size_t size);
/* [stack] The peer subscribes to or leaves a stream of this side. XIAOTAI_OK
 * accepts; anything else refuses towards the peer (only meaningful when
 * subscribed is true). */
xiaotai_err_t xiaotai_rtc_on_subscribe(xiaotai_rtc_conn_t conn, uint32_t request, xiaotai_link_media_kind_t kind,
                                       uint8_t stream, bool subscribed);
/* [stack] */
void xiaotai_rtc_on_key_frame_request(xiaotai_rtc_conn_t conn, uint8_t stream);
void xiaotai_rtc_on_video_bitrate(xiaotai_rtc_conn_t conn, uint8_t stream, uint32_t bps);

#ifdef __cplusplus
}
#endif
