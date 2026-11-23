// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/port/http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The simulated peer receives its own immutable request snapshot; pointers
 * into it may only be used until the current perform returns. It never
 * receives the caller's source, sink or operating-system socket. */
typedef struct {
    xiaotai_http_method_t method;
    const char *url;
    const xiaotai_http_header_t *headers;
    size_t header_count;
    const uint8_t *body;
    size_t body_size;            /* Fixed bytes or announced length for stream. */
    bool streamed;             /* On metadata callback, a stream's body is NULL. */
    bool has_content_length;   /* False on GET and bodyless DELETE. */
    bool reused;
    xiaotai_http_options_t options;
} xiaotai_http_host_request_t;

typedef void (*xiaotai_http_host_upload_fn)(void *ctx, const uint8_t *body, size_t size);

typedef struct {
    /* Data supplied by the fixture must outlive perform. One response only:
     * default status 200, content length unknown. No follow or retry. */
    int status;
    int64_t content_length;
    const xiaotai_http_header_t *headers;
    size_t header_count;
    const uint8_t *body;
    size_t body_size;
    bool close;
    /* NONE disables faults / blocking. RECEIVE faults are before headers
     * unless fail_after_bytes is nonzero (also supports errors after a prefix). */
    xiaotai_http_stage_t fail_stage, block_stage;
    xiaotai_err_t error;
    int32_t platform_error;
    size_t fail_after_bytes;
    /* Base durations per stage; total is computed by the port. Source/receive
     * chunk durations are extra I/O operations, not part of base durations. */
    xiaotai_http_timing_t timing;
    uint32_t send_chunk_ms, read_chunk_ms;
    size_t read_chunk_bytes;
    /* Optional copy of all streamed bytes. Called only if all were sent. */
    xiaotai_http_host_upload_fn uploaded;
    void *upload_ctx;
} xiaotai_http_host_reply_t;

typedef void (*xiaotai_http_host_responder_fn)(void *ctx, const xiaotai_http_host_request_t *request,
                                               xiaotai_http_host_reply_t *reply);

/* Set between requests; mock_tls models whether certificate verification is
 * available on the simulated stack, not a switch to bypass verification. */
void xiaotai_http_host_set_responder(xiaotai_http_host_responder_fn respond, void *ctx, bool mock_tls);
/* Inject EOF, unread bytes or socket error on a kept connection. */
void xiaotai_http_host_invalidate_idle(xiaotai_http_t *client);

#ifdef __cplusplus
}
#endif
