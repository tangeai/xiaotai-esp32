#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Caller-owned, normally on the platform worker's PSRAM stack. No payload,
 * hostname, token or address is retained. All intervals are milliseconds. */
typedef struct {
    uint32_t started_ms;
    uint32_t dns_ms;
    uint32_t dns_calls;
    uint32_t connect_ms;
    uint32_t connect_calls;
    uint32_t write_ms;
    uint32_t first_rx_ms;
    uint32_t write_calls;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t socket_ms;
    uint32_t tcp_wait_ms;
    int pending_socket;
    int tcp_wait_rc;
    int dns_rc;
    bool enabled;
} platform_http_trace_t;

/* Only synchronous calls made by this task are observed. A nested/concurrent
 * scope remains disabled and does not displace the existing owner. */
void platform_http_trace_begin(platform_http_trace_t *trace);
void platform_http_trace_end(platform_http_trace_t *trace);
