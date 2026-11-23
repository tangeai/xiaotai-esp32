// SPDX-License-Identifier: MIT
#pragma once
/* HTTP port: an HTTP/1.1 client that performs one request at a time.
 *
 * A client object owns at most one connection. Policy stays above the port: the
 * caller decides which requests may share a connection (keep_alive), when an
 * idle connection is dropped (xiaotai_http_close), whether a failed request is
 * repeated, what is logged, and how URLs are admitted or rewritten. The port
 * resolves, connects, sends, receives, enforces the limits it is given and
 * reports what happened, including where the time went.
 *
 * The port never follows a redirect, never retries, never logs a URL, a header
 * or a body, and never allocates per request beyond what the platform's HTTP
 * stack needs for its own connection. Bodies live in buffers the caller owns
 * (place them in XIAOTAI_MEM_BULK) or pass through the caller's callbacks.
 *
 * https:// is verified against the platform's certificate store; a build
 * without TLS answers XIAOTAI_ERR_NOT_SUPPORTED for it.
 *
 * Threading: xiaotai_http_perform blocks. Call it from a worker task with room
 * for the platform's TLS and HTTP stack frames (the port documents the amount),
 * never from the framework owner task or a real-time task. One client is used
 * by one task at a time; xiaotai_http_cancel is the only call another task may
 * make while a request runs. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xiaotai_http xiaotai_http_t;

typedef enum {
    XIAOTAI_HTTP_GET = 0,
    XIAOTAI_HTTP_POST,
    XIAOTAI_HTTP_PUT,
    XIAOTAI_HTTP_DELETE,
} xiaotai_http_method_t;

typedef struct {
    /* Receive and transmit buffers of the platform's HTTP stack; 0: the port's
     * default. They bound header size, not body size. */
    uint16_t rx_buffer_bytes;
    uint16_t tx_buffer_bytes;
    /* Disable Nagle's algorithm on connections this client opens. */
    bool no_delay;
} xiaotai_http_options_t;

/* [any task that may block] Creates a client without connecting. options may be
 * NULL for all defaults. *client is NULL on failure (XIAOTAI_ERR_NO_MEM). */
xiaotai_err_t xiaotai_http_create(const xiaotai_http_options_t *options, xiaotai_http_t **client);
/* [the task that uses the client] Closes the connection and frees the client.
 * Not while a request runs. NULL is ignored. */
void xiaotai_http_destroy(xiaotai_http_t *client);

typedef struct {
    const char *name;
    const char *value;
} xiaotai_http_header_t;

/* A response header the caller wants to read. value receives the terminated
 * text of its first occurrence, "" when the response has no such header. A
 * value that does not fit is cut at capacity - 1 bytes and sets truncated.
 * count tells how often the header appeared, so a caller that must refuse a
 * repeated header (two Location or two Content-Type lines) can. */
typedef struct {
    const char *name;       /* compared without regard to case */
    char *value;
    size_t capacity;
    bool truncated;         /* out */
    uint32_t count;         /* out: occurrences in the response, 0 when absent */
} xiaotai_http_capture_t;

/* Produces a streamed request body. Fill at most capacity bytes and set *size;
 * the port calls again until it has source_size bytes. A result other than
 * XIAOTAI_OK, or *size 0 before source_size bytes were produced, aborts the
 * request with that result (XIAOTAI_FAIL for the short body). Runs on the task
 * inside xiaotai_http_perform. */
typedef xiaotai_err_t (*xiaotai_http_source_fn_t)(void *ctx, uint8_t *data, size_t capacity, size_t *size);

/* Consumes a streamed response body, in order, in pieces of the port's choosing.
 * A result other than XIAOTAI_OK aborts the request with that result. Called
 * for the body of every response whatever its status: response->status,
 * content_length and the captures are set before the first call, so the
 * callback can refuse an error page or an unexpected content type. Runs on the
 * task inside xiaotai_http_perform. */
typedef xiaotai_err_t (*xiaotai_http_sink_fn_t)(void *ctx, const uint8_t *data, size_t size);

typedef struct {
    xiaotai_http_method_t method;
    /* Absolute URL, http:// or https://. Borrowed for the call. */
    const char *url;
    /* Extra request headers. Borrowed for the call; the port removes them from
     * a kept connection afterwards, so nothing of this request is replayed. */
    const xiaotai_http_header_t *headers;
    size_t header_count;
    /* Fixed body. With POST, PUT or DELETE a NULL body sends an empty one (for
     * DELETE: no body and no content length); with GET the body must be NULL.
     * Borrowed for the call. */
    const void *body;
    size_t body_size;
    /* Streamed body of exactly source_size bytes instead of body (body NULL). */
    xiaotai_http_source_fn_t source;
    void *source_ctx;
    size_t source_size;
    /* Limit for the whole request: resolve, connect, handshake, send, receive.
     * 1..600000. */
    uint32_t timeout_ms;
    /* Limit for one socket operation; 0: the remaining part of timeout_ms. */
    uint32_t io_timeout_ms;
    /* Leave the connection open after a complete response so the next request
     * to the same scheme, host and port can reuse it. The port still closes it
     * when the response carried a Location header, asked to close, or failed. */
    bool keep_alive;
    /* Optional cancellation already latched by the request's owner. Called
     * once after claiming the client and before transport setup. It must not
     * call the client. A true result aborts with INVALID_STATE and stage NONE.
     * Subsequent cancellation still uses xiaotai_http_cancel(). */
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;
} xiaotai_http_request_t;

/* Where a failed request stopped. */
typedef enum {
    XIAOTAI_HTTP_STAGE_NONE = 0,   /* arguments rejected before any I/O */
    XIAOTAI_HTTP_STAGE_RESOLVE,
    XIAOTAI_HTTP_STAGE_CONNECT,    /* TCP connect and TLS handshake */
    XIAOTAI_HTTP_STAGE_SEND,
    XIAOTAI_HTTP_STAGE_RECEIVE,    /* waiting for or reading the response */
    XIAOTAI_HTTP_STAGE_DONE,       /* a complete response was received */
} xiaotai_http_stage_t;

/* An HTTP receive fault after request write, separate from policy errors,
 * allocation failures, cancellation and expiry of the whole exchange. */
typedef uint8_t xiaotai_http_receive_fault_t;
enum {
    XIAOTAI_HTTP_RECEIVE_FAULT_NONE = 0,
    XIAOTAI_HTTP_RECEIVE_FAULT_HEAD,
    XIAOTAI_HTTP_RECEIVE_FAULT_BODY,
    XIAOTAI_HTTP_RECEIVE_FAULT_BODY_TRUNCATED, /* EOF after a body prefix, before the framing completed */
};

/* Durations in milliseconds, 0 for a stage that did not run (a reused
 * connection has no resolve or connect time). */
typedef struct {
    uint32_t resolve_ms;
    uint32_t connect_ms;
    uint32_t send_ms;
    uint32_t wait_ms;      /* request sent until the first response byte */
    uint32_t receive_ms;   /* first response byte until the end of the body */
    uint32_t total_ms;
} xiaotai_http_timing_t;

typedef struct {
    /* ---- set by the caller before the call ---- */
    /* Body buffer, or NULL to discard the body or when sink is set. The stored
     * body is not terminated; bytes are stored as received, zeros included. */
    uint8_t *body;
    size_t capacity;
    /* Streamed body instead of the buffer; limit is the largest body accepted. */
    xiaotai_http_sink_fn_t sink;
    void *sink_ctx;
    size_t sink_limit;
    /* Headers to read from the response; may be NULL. */
    xiaotai_http_capture_t *captures;
    size_t capture_count;

    /* ---- set by the port ---- */
    /* HTTP status code; 0 when no status line arrived. Kept even when the call
     * fails later (for example a body that is too long). */
    int status;
    /* Body bytes stored in body or handed to sink. */
    size_t size;
    /* Content-Length of the response; -1 when not announced. */
    int64_t content_length;
    /* The response carried a Location header. Never followed. */
    bool redirect;
    /* The request went out on a connection kept from an earlier request. When a
     * request on a reused connection fails before any response byte, the peer
     * may simply have closed it: the caller decides whether repeating the
     * request on a new connection is safe (it is for GET). */
    bool reused;
    /* The connection is still open for the next request. */
    bool kept;
    xiaotai_http_receive_fault_t receive_fault;
    xiaotai_http_stage_t stage;
    /* Platform error behind a failure (socket errno or TLS code), 0 when none.
     * For diagnostics only. */
    int32_t platform_error;
    xiaotai_http_timing_t timing;
} xiaotai_http_response_t;

/* [worker task, blocking] Performs one request.
 *
 * XIAOTAI_OK: a complete response was received, whatever its status code.
 * XIAOTAI_ERR_INVALID_ARG: bad request (URL form, method with body, limits).
 * XIAOTAI_ERR_NOT_SUPPORTED: scheme not available in this build.
 * XIAOTAI_ERR_TIMEOUT: timeout_ms or io_timeout_ms expired.
 * XIAOTAI_ERR_INVALID_SIZE: the body is longer than capacity or sink_limit;
 *   size holds what was accepted, and the connection is closed.
 * XIAOTAI_ERR_INVALID_STATE: cancelled, or the client is already in a request.
 * XIAOTAI_ERR_NO_MEM: the platform stack could not allocate.
 * XIAOTAI_FAIL: resolve, connect, handshake or transfer failed; see stage.
 * Any other value came from the caller's source or sink.
 *
 * After every result other than XIAOTAI_OK the connection is closed. A request
 * is never sent twice by the port. */
xiaotai_err_t xiaotai_http_perform(xiaotai_http_t *client, const xiaotai_http_request_t *request,
                                   xiaotai_http_response_t *response);

/* [the task that uses the client] True when a kept connection exists and is
 * still usable: open, with no unread data and no error pending. A connection
 * that fails this check is closed by the call. */
bool xiaotai_http_idle_connection(xiaotai_http_t *client);
/* [the task that uses the client] Closes the kept connection, if any. */
void xiaotai_http_close(xiaotai_http_t *client);

/* [any] Makes the request running on this client end with
 * XIAOTAI_ERR_INVALID_STATE at its next socket operation or within
 * io_timeout_ms, whichever comes first. Without a running request the next
 * xiaotai_http_perform is not affected. */
void xiaotai_http_cancel(xiaotai_http_t *client);

typedef struct {
    uint32_t requests;      /* xiaotai_http_perform calls that reached the network */
    uint32_t completed;     /* complete responses */
    uint32_t failed;
    uint32_t timeouts;
    uint32_t oversize;      /* bodies longer than the caller allowed */
    uint32_t cancelled;
    uint32_t connections;   /* connections opened */
    uint32_t reused;        /* requests sent on a kept connection */
} xiaotai_http_stats_t;

/* [any] Totals over every client since boot. */
void xiaotai_http_stats(xiaotai_http_stats_t *stats);

#ifdef __cplusplus
}
#endif
