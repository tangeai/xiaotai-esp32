// SPDX-License-Identifier: MIT
#include "trace_internal.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "xiaotai/port/os.h"

#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); abort(); } } while (0)

static enum { NORMAL, BAD_BIND, BAD_RECV, BAD_TASK, BAD_SOCKET, BAD_SELECT, BURST_LIMIT, LATE_READ, INTERRUPTED } s_mode;
static int s_fd = 29;
static bool s_close_fails;
static unsigned s_closed;
static bool s_in_probe;
static unsigned s_phase;
static unsigned s_frame_samples;
static unsigned s_socket_samples;
static uint64_t s_now = 1000000;
static const uint8_t s_lan[4] = {192, 168, 1, 2};
static const uint8_t s_wan[4] = {1, 2, 3, 4};
static uint32_t s_ip;
static struct sockaddr_in s_from = {.sin_family = AF_INET};
static xiaotai_task_config_t s_task;
static unsigned s_ready;
static unsigned s_done;
static xiaotai_err_t s_error;
static net_probe_result_t s_result;
static jmp_buf s_exit;
#if XIAOTAI_NET_PATTERN_PROBE
static bool s_in_probe_v2;
static unsigned s_v2_reads;
static unsigned s_peer_rejects;
static uint32_t s_peer_ip;
static const uint16_t s_peer_port = 41321;
static const net_probe_v2_plan_t s_plan = {.flow = 4217, .seed = 9527, .packets = 4, .payload_bytes = 32};
static net_probe_v2_stats_t s_stats;
#endif

int64_t xiaotai_time_us(void) { return (int64_t)s_now; }

void net_trace_frame(const void *ethernet, size_t size)
{
    CHECK(ethernet && size == 4);
    s_frame_samples++;
}

void net_trace_socket(const uint8_t from[4], size_t received)
{
    CHECK(from);
#if XIAOTAI_NET_PATTERN_PROBE
    if (s_in_probe_v2) {
        CHECK(memcmp(from, &s_peer_ip, 4) == 0 || memcmp(from, s_lan, 4) == 0);
        CHECK(received == NET_PROBE_V2_HEADER + (size_t)s_plan.payload_bytes ||
              received == NET_PROBE_V2_HEADER + (size_t)s_plan.payload_bytes + 1);
    } else
#endif
    {
        CHECK(memcmp(from, s_lan, 4) == 0 || memcmp(from, s_wan, 4) == 0);
        CHECK(received == 8);
    }
    s_socket_samples++;
    errno = EDOM; /* the native wrapper must restore the underlying result's errno */
}

esp_err_t __wrap_esp_netif_receive(esp_netif_t *netif, void *buffer, size_t size, void *owner);

esp_err_t __real_esp_netif_receive(esp_netif_t *netif, void *buffer, size_t size, void *owner)
{
    CHECK(!netif && buffer && size == 4 && !owner && s_frame_samples == 1);
    return -17;
}

int __real_lwip_socket(int domain, int type, int protocol)
{
    CHECK(domain == AF_INET && (type == SOCK_DGRAM || type == SOCK_STREAM));
    CHECK(protocol == IPPROTO_UDP || protocol == IPPROTO_UDPLITE || protocol == IPPROTO_TCP);
    return s_mode == BAD_SOCKET ? -1 : s_fd;
}

int __real_lwip_close(int fd)
{
    CHECK(fd == s_fd);
    s_closed++;
    return s_close_fails ? -1 : 0;
}

#if XIAOTAI_NET_PATTERN_PROBE
static ssize_t recv_v2(void *data, size_t capacity, int flags, struct sockaddr *from, socklen_t *length)
{
    CHECK(capacity == NET_PROBE_V2_HEADER + (size_t)s_plan.payload_bytes + 1);
    CHECK(flags == MSG_DONTWAIT && from && length && *length == sizeof(struct sockaddr_in));
    s_v2_reads++;
    if (s_mode == BAD_RECV || (s_mode == NORMAL && s_phase == 1)) {
        errno = s_mode == BAD_RECV ? EIO : EWOULDBLOCK;
        return -1;
    }
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(s_peer_port),
                               .sin_addr.s_addr = s_peer_ip};
    if (s_mode == BURST_LIMIT || s_phase == 4) peer.sin_port++;
    if (s_phase == 5) peer.sin_addr.s_addr = s_ip;
    const uint32_t sequence = s_phase == 7 ? 0 : s_phase == 3 ? 2 : 1;
    size_t size = net_probe_v2_packet(&s_plan, sequence, 29, data, capacity);
    CHECK(size == capacity - 1);
    if (s_phase == 3) {
        /* Model recvfrom with a larger datagram: only capacity bytes are returned. */
        ((uint8_t *)data)[size++] = 0x5a;
    }
    if (s_phase == 6) (*length)--;
    memcpy(from, &peer, *length);
    if (s_mode == LATE_READ) s_now += 100000;
    return (ssize_t)size;
}
#endif

ssize_t __real_lwip_recvfrom(int fd, void *data, size_t size, int flags,
                            struct sockaddr *from, socklen_t *from_len)
{
    CHECK(fd == s_fd);
#if XIAOTAI_NET_PATTERN_PROBE
    if (s_in_probe_v2) return recv_v2(data, size, flags, from, from_len);
#endif
    if (s_in_probe) {
        CHECK(flags == MSG_DONTWAIT && size == NET_PROBE_HEADER && from && from_len);
        if (s_mode == BAD_RECV) {
            errno = EIO;
            return -1;
        }
        if (s_phase == 1) {
            errno = EWOULDBLOCK;
            return -1;
        }
        memcpy(&s_from.sin_addr, s_phase == 4 ? s_wan : s_lan, 4);
        memcpy(data, "XTP1\x00\x00\x01\xF3", NET_PROBE_HEADER);
    }
    if (from && from_len) {
        memcpy(from, &s_from, *from_len < sizeof(s_from) ? *from_len : sizeof(s_from));
        *from_len = sizeof(s_from);
    }
    errno = EBUSY;
    return 8;
}

int trace_test_bind(int fd, const struct sockaddr *address, socklen_t length)
{
    CHECK(fd == s_fd && length == sizeof(struct sockaddr_in));
    const struct sockaddr_in *local = (const struct sockaddr_in *)address;
    CHECK(local->sin_family == AF_INET && local->sin_port == htons(49161));
    CHECK(local->sin_addr.s_addr == s_ip);
    return s_mode == BAD_BIND ? -1 : 0;
}

int trace_test_select(int nfds, fd_set *readable, fd_set *writable, fd_set *except, struct timeval *timeout)
{
    CHECK(nfds == s_fd + 1 && FD_ISSET(s_fd, readable) && !writable && !except);
    if (s_phase++ == 0) CHECK(timeout->tv_sec == 12 && timeout->tv_usec == 0);
#if XIAOTAI_NET_PATTERN_PROBE
    if (s_in_probe_v2) {
        if (s_mode == BAD_SELECT) {
            errno = EIO;
            return -1;
        }
        if (s_mode == BURST_LIMIT) {
            s_now++;
            return 1;
        }
        if (s_mode == INTERRUPTED) {
            if (s_phase == 1) {
                s_now += 100000;
                errno = EINTR;
                return -1;
            }
            CHECK(s_phase == 2 && timeout->tv_sec == 11 && timeout->tv_usec == 900000);
            s_now += (uint64_t)timeout->tv_sec * 1000000 + (uint64_t)timeout->tv_usec;
            return 0;
        }
        if (s_mode == LATE_READ || s_phase == 8) {
            if (s_mode != LATE_READ) CHECK(timeout->tv_sec == 11 && timeout->tv_usec == 300000);
            s_now += (uint64_t)timeout->tv_sec * 1000000 + (uint64_t)timeout->tv_usec;
            if (s_mode == LATE_READ) s_now -= 1;
            return 1;
        }
        s_now += 100000;
        return 1;
    }
#endif
    if (s_phase == 6) {
        CHECK(timeout->tv_sec == 11 && timeout->tv_usec == 500000);
        s_now += (uint64_t)timeout->tv_sec * 1000000 + (uint64_t)timeout->tv_usec;
        return 1; /* Ready at the deadline is still excluded. */
    }
    s_now += 100000;
    return 1;
}

xiaotai_err_t xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    CHECK(task && config && config->entry && config->stack == XIAOTAI_MEM_INTERNAL && config->stack_bytes >= 4096);
    if (s_mode == BAD_TASK) return XIAOTAI_ERR_NO_MEM;
    s_task = *config;
    return XIAOTAI_OK;
}

void xiaotai_task_exit(void) { longjmp(s_exit, 1); }

static void ready(void) { s_ready++; }
static void done(xiaotai_err_t error, const net_probe_result_t *result)
{
    s_done++;
    s_error = error;
    s_result = *result;
}

static void run_probe(void)
{
    CHECK(net_probe_platform_start(s_ip, ready, done) == XIAOTAI_OK);
    CHECK(net_probe_platform_start(s_ip, ready, done) == XIAOTAI_ERR_BUSY);
    s_in_probe = true;
    s_phase = 0;
    if (setjmp(s_exit) == 0) s_task.entry(s_task.arg);
    s_in_probe = false;
}

#if XIAOTAI_NET_PATTERN_PROBE
static void ready_v2(uint32_t flow)
{
    CHECK(flow == s_plan.flow);
    s_ready++;
}

static void done_v2(xiaotai_err_t error, uint32_t flow, const net_probe_v2_stats_t *stats,
                    uint32_t peer_rejects)
{
    CHECK(flow == s_plan.flow && stats);
    s_done++;
    s_error = error;
    s_stats = *stats;
    s_peer_rejects = peer_rejects;
}

static xiaotai_err_t start_v2(void)
{
    return net_probe_v2_platform_start(s_ip, s_peer_ip, s_peer_port, &s_plan, ready_v2, done_v2);
}

static void run_v2(void)
{
    const unsigned ready_before = s_ready, done_before = s_done;
    CHECK(start_v2() == XIAOTAI_OK);
    CHECK(start_v2() == XIAOTAI_ERR_BUSY);
    CHECK(net_probe_platform_start(s_ip, ready, done) == XIAOTAI_ERR_BUSY);
    s_in_probe_v2 = true;
    s_v2_reads = s_phase = 0;
    if (setjmp(s_exit) == 0) s_task.entry(s_task.arg);
    s_in_probe_v2 = false;
    CHECK(s_ready == ready_before + (s_mode != BAD_SOCKET && s_mode != BAD_BIND));
    CHECK(s_done == done_before + 1);
}

static void test_v2(void)
{
    const unsigned closed_before = s_closed;
    const uint8_t peer[4] = {192, 168, 1, 88};
    memcpy(&s_peer_ip, peer, 4);
    uint32_t external, link_local, broadcast;
    const uint8_t local_only[4] = {169, 254, 4, 4};
    const uint8_t broadcast_addr[4] = {192, 168, 1, 255};
    memcpy(&external, s_wan, 4);
    memcpy(&link_local, local_only, 4);
    memcpy(&broadcast, broadcast_addr, 4);
    const net_probe_v2_plan_t invalid = {0};
    CHECK(net_probe_v2_platform_start(s_ip, external, s_peer_port, &s_plan, ready_v2, done_v2) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(net_probe_v2_platform_start(s_ip, link_local, s_peer_port, &s_plan, ready_v2, done_v2) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(net_probe_v2_platform_start(s_ip, broadcast, s_peer_port, &s_plan, ready_v2, done_v2) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(net_probe_v2_platform_start(s_ip, s_ip, s_peer_port, &s_plan, ready_v2, done_v2) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(net_probe_v2_platform_start(s_ip, s_peer_ip, 80, &s_plan, ready_v2, done_v2) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(net_probe_v2_platform_start(s_ip, s_peer_ip, s_peer_port, &invalid, ready_v2, done_v2) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(net_probe_v2_platform_start(s_ip, s_peer_ip, s_peer_port, &s_plan, ready_v2, NULL) == XIAOTAI_ERR_INVALID_ARG);

    s_mode = BAD_TASK;
    CHECK(start_v2() == XIAOTAI_ERR_NO_MEM);
    s_mode = BAD_SOCKET;
    run_v2();
    CHECK(s_error == XIAOTAI_FAIL && !s_stats.complete && s_stats.received == 0 && s_closed == closed_before);
    s_mode = BAD_BIND;
    run_v2();
    CHECK(s_error == XIAOTAI_FAIL && !s_stats.complete && s_stats.received == 0 && s_closed == closed_before + 1);
    s_mode = BAD_SELECT;
    run_v2();
    CHECK(s_error == XIAOTAI_FAIL && !s_stats.complete && s_v2_reads == 0);
    s_mode = BAD_RECV;
    run_v2();
    CHECK(s_error == XIAOTAI_FAIL && !s_stats.complete && s_v2_reads == 1);

    s_mode = BURST_LIMIT;
    run_v2();
    CHECK(s_error == XIAOTAI_ERR_INVALID_SIZE && !s_stats.complete && s_v2_reads == 4001);
    CHECK(s_peer_rejects == 4000 && s_stats.received == 0 && s_stats.missing == 0);
    s_mode = LATE_READ;
    run_v2();
    CHECK(s_error == XIAOTAI_OK && s_stats.complete && s_v2_reads == 1 && s_stats.received == 0 && s_stats.missing == 4);

    s_mode = INTERRUPTED;
    s_close_fails = true;
    const uint64_t interrupted_start = s_now;
    run_v2();
    CHECK(s_now - interrupted_start == NET_PROBE_V2_MAX_US && s_phase == 2 && s_v2_reads == 0);
    CHECK(s_error == XIAOTAI_FAIL && !s_stats.complete && s_stats.missing == 0);
    s_close_fails = false;
    s_mode = NORMAL;
    const uint64_t start = s_now;
    run_v2();
    CHECK(s_now - start == NET_PROBE_V2_MAX_US && s_phase == 8 && s_v2_reads == 7);
    CHECK(s_error == XIAOTAI_OK && s_stats.complete && s_peer_rejects == 3);
    CHECK(s_stats.received == 3 && s_stats.valid == 2 && s_stats.reordered == 1 && s_stats.bad_length == 1);
    CHECK(s_stats.missing == 1 && s_closed == closed_before + 7);

    CHECK(net_probe_platform_start(s_ip, ready, done) == XIAOTAI_OK);
    CHECK(start_v2() == XIAOTAI_ERR_BUSY);
    s_in_probe = true;
    s_phase = 0;
    if (setjmp(s_exit) == 0) s_task.entry(s_task.arg);
    s_in_probe = false;
    CHECK(s_error == XIAOTAI_OK && s_result.unique == 1);
}
#endif

static void run_wrap(void)
{
    char packet[8];
    struct sockaddr_in from;
    memmove(&s_from.sin_addr, s_lan, 4);
    socklen_t len = sizeof(from);
    CHECK(__wrap_esp_netif_receive(NULL, packet, 4, NULL) == -17);
    CHECK(recvfrom(s_fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &len) == 8);
    CHECK(s_socket_samples == 0);
    CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) == s_fd);
    CHECK(recvfrom(s_fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &len) == 8);
    CHECK(s_socket_samples == 0);
    CHECK(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDPLITE) == s_fd);
    CHECK(recvfrom(s_fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &len) == 8);
    CHECK(s_socket_samples == 0);
    CHECK(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) == s_fd);
    CHECK(recvfrom(s_fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &len) == 8);
    CHECK(s_socket_samples == 1 && errno == EBUSY);
    len = 2;
    CHECK(recvfrom(s_fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &len) == 8);
    CHECK(len == sizeof(from) && s_socket_samples == 1);
    s_from.sin_family = AF_INET6;
    CHECK(recvfrom(s_fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &len) == 8);
    CHECK(s_socket_samples == 1);
    s_from.sin_family = AF_INET;
    s_close_fails = true;
    CHECK(closesocket(s_fd) == -1);
    CHECK(recvfrom(s_fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &len) == 8);
    CHECK(s_socket_samples == 2);
    s_close_fails = false;
    CHECK(closesocket(s_fd) == 0);
    CHECK(recvfrom(s_fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &len) == 8);
    CHECK(s_socket_samples == 2);
}

int main(void)
{
    run_wrap();
    memcpy(&s_ip, s_lan, 4);
    const unsigned closed_before = s_closed;
    CHECK(net_probe_platform_start(0, ready, done) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(net_probe_platform_start(s_ip, ready, NULL) == XIAOTAI_ERR_INVALID_ARG);
    s_mode = BAD_TASK;
    CHECK(net_probe_platform_start(s_ip, ready, done) == XIAOTAI_ERR_NO_MEM);
    s_mode = BAD_SOCKET;
    run_probe();
    CHECK(s_error == XIAOTAI_FAIL && s_ready == 0 && s_done == 1 && s_closed == closed_before);
    s_mode = BAD_BIND;
    run_probe();
    CHECK(s_error == XIAOTAI_FAIL && s_ready == 0 && s_done == 2 && s_closed == closed_before + 1);
    s_mode = BAD_RECV;
    run_probe();
    CHECK(s_error == XIAOTAI_FAIL && s_ready == 1 && s_done == 3 && s_closed == closed_before + 2);
    s_mode = NORMAL;
    run_probe();
    CHECK(s_error == XIAOTAI_OK && s_ready == 2 && s_done == 4 && s_closed == closed_before + 3);
    CHECK(s_result.unique == 1 && s_result.duplicate == 2 && s_result.invalid == 0 && s_result.max_seq == 499);
    CHECK(s_result.phases[0] == 0 && s_result.phases[4] == 1);
#if XIAOTAI_NET_PATTERN_PROBE
    test_v2();
#endif
    return 0;
}
