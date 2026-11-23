// SPDX-License-Identifier: MIT
#include "../src/trace_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include "lwip/sockets.h"
#include "xiaotai/port/os.h"

typedef struct {
    uint32_t ipv4;
    net_probe_ready_fn_t ready;
    net_probe_done_fn_t done;
} probe_config_t;

static atomic_bool s_active;
static probe_config_t s_config;
static xiaotai_task_t s_task;

static void receive_probe(void *arg)
{
    const probe_config_t config = *(const probe_config_t *)arg;
    xiaotai_err_t error = XIAOTAI_FAIL;
    net_probe_rx_t rx = {0};
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) goto done;

    const struct sockaddr_in local = {
        .sin_family = AF_INET, .sin_port = htons(NET_PROBE_PORT), .sin_addr.s_addr = config.ipv4,
    };
    if (fd >= LWIP_SELECT_MAXNFDS || bind(fd, (const struct sockaddr *)&local, sizeof(local)) != 0) goto done;
    net_probe_rx_begin(&rx, (uint64_t)xiaotai_time_us());
    if (config.ready) config.ready();
    error = XIAOTAI_OK;
    for (;;) {
        const uint32_t remaining = net_probe_rx_remaining_ms(&rx, (uint64_t)xiaotai_time_us());
        if (!remaining) break;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        struct timeval timeout = {.tv_sec = remaining / 1000U,
                                  .tv_usec = (suseconds_t)((remaining % 1000U) * 1000U)};
        const int waiting = lwip_select(fd + 1, &readable, NULL, NULL, &timeout);
        if (waiting < 0 && errno == EINTR) continue;
        if (waiting < 0) {
            error = XIAOTAI_FAIL;
            break;
        }
        if (!waiting) continue;
        if (!net_probe_rx_remaining_ms(&rx, (uint64_t)xiaotai_time_us())) break;
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        uint8_t header[NET_PROBE_HEADER];
        const ssize_t length = recvfrom(fd, header, sizeof(header), MSG_DONTWAIT,
                                        (struct sockaddr *)&from, &from_len);
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (length < 0) {
            error = XIAOTAI_FAIL;
            break;
        }
        if (from_len >= sizeof(from) && from.sin_family == AF_INET) {
            net_probe_rx_packet(&rx, (uint64_t)xiaotai_time_us(),
                                (const uint8_t *)&from.sin_addr.s_addr, header, (size_t)length);
        }
    }
done:
    if (fd >= 0) (void)closesocket(fd);
    atomic_store(&s_active, false);
    config.done(error, &rx.result);
    xiaotai_task_exit();
}

xiaotai_err_t net_probe_platform_start(uint32_t ipv4, net_probe_ready_fn_t ready,
                                      net_probe_done_fn_t done)
{
    if (!done || !ipv4) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_exchange(&s_active, true)) return XIAOTAI_ERR_BUSY;
    s_config = (probe_config_t){.ipv4 = ipv4, .ready = ready, .done = done};
    const xiaotai_task_config_t task = {
        .name = "xiaotai_probe", .entry = receive_probe, .arg = &s_config,
        .stack_bytes = 4096, .stack = XIAOTAI_MEM_INTERNAL, .priority = 8, .core = XIAOTAI_CORE_ANY,
    };
    const xiaotai_err_t error = xiaotai_task_create(&task, &s_task);
    if (error != XIAOTAI_OK) atomic_store(&s_active, false);
    return error;
}

#if XIAOTAI_NET_PATTERN_PROBE
enum { NET_PROBE_V2_MAX_EXAMINED = 4000 };
_Static_assert(NET_PROBE_V2_MAX_US == NET_PROBE_LIMIT_MS * 1000U, "probe lease mismatch");

typedef struct {
    uint32_t ipv4, peer_ipv4;
    uint16_t peer_port;
    net_probe_v2_plan_t plan;
    net_probe_v2_ready_fn_t ready;
    net_probe_v2_done_fn_t done;
} probe_v2_config_t;

static probe_v2_config_t s_config_v2;

static void receive_probe_v2(void *arg)
{
    const probe_v2_config_t config = *(const probe_v2_config_t *)arg;
    net_probe_v2_rx_t rx = {0};
    xiaotai_err_t error = XIAOTAI_FAIL;
    uint32_t examined = 0, peer_rejects = 0;
    const uint16_t peer_port = htons(config.peer_port);
    /* One extra byte distinguishes a valid datagram from a truncated prefix. */
    uint8_t data[NET_PROBE_V2_HEADER + NET_PROBE_V2_MAX_PAYLOAD + 1U];
    const size_t capacity = NET_PROBE_V2_HEADER + (size_t)config.plan.payload_bytes + 1U;
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) goto done;

    const struct sockaddr_in local = {
        .sin_family = AF_INET, .sin_port = htons(NET_PROBE_PORT), .sin_addr.s_addr = config.ipv4,
    };
    if (fd >= LWIP_SELECT_MAXNFDS || bind(fd, (const struct sockaddr *)&local, sizeof(local)) != 0) goto done;
    if (!net_probe_v2_rx_begin(&rx, &config.plan)) goto done;
    const uint64_t started = (uint64_t)xiaotai_time_us();
    if (config.ready) config.ready(config.plan.flow);
    error = XIAOTAI_OK;
    for (;;) {
        const uint64_t elapsed = (uint64_t)xiaotai_time_us() - started;
        if (elapsed >= NET_PROBE_V2_MAX_US) break;
        const uint32_t remaining = NET_PROBE_V2_MAX_US - (uint32_t)elapsed;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        struct timeval timeout = {.tv_sec = remaining / 1000000U,
                                  .tv_usec = (suseconds_t)(remaining % 1000000U)};
        const int waiting = lwip_select(fd + 1, &readable, NULL, NULL, &timeout);
        if (waiting < 0 && errno == EINTR) continue;
        if (waiting < 0) {
            error = XIAOTAI_FAIL;
            break;
        }
        if (!waiting) continue;
        if ((uint64_t)xiaotai_time_us() - started >= NET_PROBE_V2_MAX_US) break;
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        const ssize_t length = recvfrom(fd, data, capacity, MSG_DONTWAIT,
                                        (struct sockaddr *)&from, &from_len);
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            error = XIAOTAI_FAIL;
            break;
        }
        const uint64_t arrival = (uint64_t)xiaotai_time_us() - started;
        if (arrival >= NET_PROBE_V2_MAX_US) break;
        if (++examined > NET_PROBE_V2_MAX_EXAMINED) {
            error = XIAOTAI_ERR_INVALID_SIZE;
            break;
        }
        if (from_len != sizeof(from) || from.sin_family != AF_INET ||
            from.sin_addr.s_addr != config.peer_ipv4 || from.sin_port != peer_port) {
            peer_rejects++;
            continue;
        }
        const bool full = (size_t)length >= capacity;
        net_probe_v2_rx_packet(&rx, (uint32_t)arrival, data, full ? capacity : (size_t)length, full);
    }
done:
    if (fd >= 0 && closesocket(fd) != 0 && error == XIAOTAI_OK) error = XIAOTAI_FAIL;
    const net_probe_v2_stats_t *stats = net_probe_v2_rx_finish(&rx, error == XIAOTAI_OK);
    atomic_store(&s_active, false);
    config.done(error, config.plan.flow, stats, peer_rejects);
    xiaotai_task_exit();
}

xiaotai_err_t net_probe_v2_platform_start(uint32_t ipv4, uint32_t peer_ipv4, uint16_t peer_port,
                                         const net_probe_v2_plan_t *plan, net_probe_v2_ready_fn_t ready,
                                         net_probe_v2_done_fn_t done)
{
    if (!done || !ipv4 || ipv4 == peer_ipv4 || peer_port < 1024 ||
        !net_probe_v2_private_peer((const uint8_t *)&peer_ipv4) ||
        !net_probe_v2_plan_valid(plan)) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_exchange(&s_active, true)) return XIAOTAI_ERR_BUSY;
    s_config_v2 = (probe_v2_config_t){.ipv4 = ipv4, .peer_ipv4 = peer_ipv4, .peer_port = peer_port,
                                   .plan = *plan, .ready = ready, .done = done};
    const xiaotai_task_config_t task = {
        .name = "xiaotai_probe", .entry = receive_probe_v2, .arg = &s_config_v2,
        .stack_bytes = 4096, .stack = XIAOTAI_MEM_INTERNAL, .priority = 8, .core = XIAOTAI_CORE_ANY,
    };
    const xiaotai_err_t error = xiaotai_task_create(&task, &s_task);
    if (error != XIAOTAI_OK) atomic_store(&s_active, false);
    return error;
}
#endif
