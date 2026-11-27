// SPDX-License-Identifier: MIT
#pragma once
/* Internal platform seam of the network engine (private to the component).
 *
 * The portable engine (src/) reaches the platform through three things only:
 * the driver interface (xiaotai/net_driver.h), the port layer (the
 * xiaotai/port headers) and this seam. Each platform implements every function
 * below: the files of esp-idf/ on the device, host/net_host.c for host tests (an
 * in-memory server and socket, a fixed small page, the scripted driver).
 * It is not a public header and may change inside the component, but both
 * implementations change together. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- setup page server: one request at a time, on the platform's server task ---- */

typedef struct net_exchange net_exchange_t;          /* one request, owned by the platform */

typedef struct {
    const char *method;      /* "GET" or "POST" */
    const char *path;        /* without the query */
    const char *origin;      /* Origin header, NULL when absent */
    size_t content_length;
    bool local_is_ap;        /* the connection's local address is the access point's */
} net_request_t;

typedef void (*net_request_fn_t)(const net_request_t *request, net_exchange_t *exchange, void *ctx);

xiaotai_err_t net_platform_server_start(uint32_t ap_ipv4, uint32_t stack_bytes, net_request_fn_t handler,
                                        void *ctx);
/* Returns when no handler runs. */
xiaotai_err_t net_platform_server_stop(void);
/* Up to capacity body bytes; *size 0 at the end; XIAOTAI_ERR_TIMEOUT per receive timeout. */
xiaotai_err_t net_platform_body_read(net_exchange_t *exchange, uint8_t *data, size_t capacity, size_t *size);
/* Status line and headers ("Name: value" strings); then any number of chunks; then end. */
xiaotai_err_t net_platform_reply_begin(net_exchange_t *exchange, int status, const char *content_type,
                                       const char *const *headers, size_t header_count);
xiaotai_err_t net_platform_reply_chunk(net_exchange_t *exchange, const void *data, size_t size);
xiaotai_err_t net_platform_reply_end(net_exchange_t *exchange);

/* ---- one UDP socket bound to an address (captive name server, probe) ---- */

typedef struct {
    uint32_t ipv4;
    uint16_t port;
} net_peer_t;

xiaotai_err_t net_platform_udp_open(uint32_t ipv4, uint16_t port, uint32_t receive_timeout_ms, int *socket);
/* >0 bytes received, 0 timeout, <0 the socket failed. */
int net_platform_udp_receive(int socket, uint8_t *data, size_t capacity, net_peer_t *peer);
xiaotai_err_t net_platform_udp_send(int socket, const net_peer_t *peer, const uint8_t *data, size_t size);
void net_platform_udp_close(int socket);

/* ---- the rest ---- */

/* The embedded setup page. */
const char *net_platform_page(size_t *size);
/* Built-in drivers for kinds without one (xiaotai_net_driver_register refuses
 * a kind that is taken, so a driver registered earlier wins). Called from the
 * engine's init, on the owner task. */
void net_platform_register_drivers(void);
/* Diagnostic builds: receive-path counters. */
xiaotai_err_t net_platform_trace_install(void);

#ifdef __cplusplus
}
#endif
