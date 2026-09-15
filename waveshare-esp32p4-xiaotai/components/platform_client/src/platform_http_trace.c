#include "platform_http_trace.h"

#include <errno.h>
#include <stddef.h>
#include <stdatomic.h>
#include "esp_timer.h"
#include "esp_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

_Static_assert(sizeof(platform_http_trace_t) == 64, "HTTP trace stack budget changed");

/* Two 32-bit pointers in internal RAM; no new task, mutex, TLS slot or heap.
 * Other tasks only read s_owner. Only its owner dereferences the stack pointer. */
static _Atomic(TaskHandle_t) s_owner;
static platform_http_trace_t *s_trace;

static uint32_t trace_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static platform_http_trace_t *current_trace(void)
{
    TaskHandle_t owner = atomic_load_explicit(&s_owner, memory_order_acquire);
    return owner != NULL && owner == xTaskGetCurrentTaskHandle() ? s_trace : NULL;
}

void platform_http_trace_begin(platform_http_trace_t *trace)
{
    *trace = (platform_http_trace_t){
        .started_ms = trace_now_ms(),
        .write_ms = UINT32_MAX,
        .first_rx_ms = UINT32_MAX,
        .pending_socket = -1,
        .tcp_wait_rc = -2,
    };
    TaskHandle_t expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&s_owner, &expected,
            xTaskGetCurrentTaskHandle(), memory_order_acq_rel, memory_order_acquire)) {
        s_trace = trace;
        trace->enabled = true;
    }
}

void platform_http_trace_end(platform_http_trace_t *trace)
{
    if (trace->enabled && current_trace() == trace) {
        s_trace = NULL;
        atomic_store_explicit(&s_owner, NULL, memory_order_release);
    }
}

int __real_lwip_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int __real_esp_transport_connect(esp_transport_handle_t, const char *, int, int);
int __real_esp_transport_write(esp_transport_handle_t, const char *, int, int);
int __real_esp_transport_read(esp_transport_handle_t, char *, int, int);
int __real_lwip_connect(int, const struct sockaddr *, socklen_t);
int __real_lwip_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);

int __wrap_lwip_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    platform_http_trace_t *trace = current_trace();
    if (trace == NULL) return __real_lwip_connect(fd, address, length);
    uint32_t start = trace_now_ms();
    int rc = __real_lwip_connect(fd, address, length);
    int saved_errno = errno;
    trace->socket_ms += trace_now_ms() - start;
    trace->pending_socket = rc < 0 && saved_errno == EINPROGRESS ? fd : -1;
    errno = saved_errno;
    return rc;
}

int __wrap_lwip_select(int count, fd_set *readfds, fd_set *writefds,
                       fd_set *exceptfds, struct timeval *timeout)
{
    platform_http_trace_t *trace = current_trace();
    /* Only the first write-ready wait after nonblocking connect is TCP setup.
     * HTTP reads and TLS handshakes must not be charged to this interval. */
    bool connecting = trace != NULL && trace->pending_socket >= 0 &&
                      trace->pending_socket < count && writefds != NULL &&
                      FD_ISSET(trace->pending_socket, writefds);
    uint32_t start = connecting ? trace_now_ms() : 0;
    int rc = __real_lwip_select(count, readfds, writefds, exceptfds, timeout);
    int saved_errno = errno;
    if (connecting) {
        trace->tcp_wait_ms += trace_now_ms() - start;
        trace->tcp_wait_rc = rc;
        trace->pending_socket = -1;
    }
    errno = saved_errno;
    return rc;
}

int __wrap_lwip_getaddrinfo(const char *node, const char *service,
                          const struct addrinfo *hints, struct addrinfo **result)
{
    platform_http_trace_t *trace = current_trace();
    if (trace == NULL) return __real_lwip_getaddrinfo(node, service, hints, result);
    uint32_t start = trace_now_ms();
    int rc = __real_lwip_getaddrinfo(node, service, hints, result);
    int saved_errno = errno;
    trace->dns_ms += trace_now_ms() - start;
    trace->dns_calls++;
    trace->dns_rc = rc;
    errno = saved_errno;
    return rc;
}

int __wrap_esp_transport_connect(esp_transport_handle_t transport, const char *host,
                               int port, int timeout_ms)
{
    platform_http_trace_t *trace = current_trace();
    if (trace == NULL) return __real_esp_transport_connect(transport, host, port, timeout_ms);
    uint32_t start = trace_now_ms();
    int rc = __real_esp_transport_connect(transport, host, port, timeout_ms);
    int saved_errno = errno;
    trace->connect_ms += trace_now_ms() - start;
    trace->connect_calls++;
    errno = saved_errno;
    return rc;
}

int __wrap_esp_transport_write(esp_transport_handle_t transport, const char *buffer,
                             int length, int timeout_ms)
{
    platform_http_trace_t *trace = current_trace();
    if (trace == NULL) return __real_esp_transport_write(transport, buffer, length, timeout_ms);
    int rc = __real_esp_transport_write(transport, buffer, length, timeout_ms);
    int saved_errno = errno;
    if (rc > 0) {
        trace->write_ms = trace_now_ms() - trace->started_ms;
        trace->write_calls++;
        trace->tx_bytes += (uint32_t)rc;
    }
    errno = saved_errno;
    return rc;
}

int __wrap_esp_transport_read(esp_transport_handle_t transport, char *buffer,
                            int length, int timeout_ms)
{
    platform_http_trace_t *trace = current_trace();
    if (trace == NULL) return __real_esp_transport_read(transport, buffer, length, timeout_ms);
    int rc = __real_esp_transport_read(transport, buffer, length, timeout_ms);
    int saved_errno = errno;
    if (rc > 0) {
        if (trace->first_rx_ms == UINT32_MAX) {
            trace->first_rx_ms = trace_now_ms() - trace->started_ms;
        }
        trace->rx_bytes += (uint32_t)rc;
    }
    errno = saved_errno;
    return rc;
}
