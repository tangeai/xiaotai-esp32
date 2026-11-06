#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Lives on the caller's PSRAM stack. No payload, host, token or IP is retained. */
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
    int dns_rc;
    bool enabled;
} platform_http_trace_t;

/* Observe only the synchronous HTTP operation on this task. A concurrent or
 * nested caller runs normally with enabled=false, never borrowing this scope. */
void platform_http_trace_begin(platform_http_trace_t *trace);
void platform_http_trace_end(platform_http_trace_t *trace);
