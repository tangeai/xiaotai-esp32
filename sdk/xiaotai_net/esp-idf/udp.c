// SPDX-License-Identifier: MIT
#include "net_platform.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <limits.h>
#include <unistd.h>

xiaotai_err_t net_platform_udp_open(uint32_t ipv4, uint16_t port, uint32_t timeout_ms, int *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = -1;
    if (!timeout_ms) return XIAOTAI_ERR_INVALID_ARG;
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return XIAOTAI_FAIL;
    const int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const struct timeval timeout = {
        .tv_sec = timeout_ms / 1000U, .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
    };
    const struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ipv4,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return XIAOTAI_FAIL;
    }
    *out = fd;
    return XIAOTAI_OK;
}

int net_platform_udp_receive(int fd, uint8_t *data, size_t capacity, net_peer_t *peer)
{
    if (fd < 0 || !data || !capacity || capacity > INT_MAX || !peer) return -1;
    struct sockaddr_in remote = {0};
    struct iovec iov = {.iov_base = data, .iov_len = capacity};
    struct msghdr message = {
        .msg_name = &remote, .msg_namelen = sizeof(remote), .msg_iov = &iov, .msg_iovlen = 1,
    };
    const ssize_t received = recvmsg(fd, &message, 0);
    if (received < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    if (!received || (message.msg_flags & MSG_TRUNC) || (size_t)received > capacity ||
        message.msg_namelen < sizeof(remote) || remote.sin_family != AF_INET) return 0;
    *peer = (net_peer_t){.ipv4 = remote.sin_addr.s_addr, .port = ntohs(remote.sin_port)};
    return (int)received;
}

xiaotai_err_t net_platform_udp_send(int fd, const net_peer_t *peer, const uint8_t *data, size_t size)
{
    if (fd < 0 || !peer || !peer->port || (!data && size) || size > INT_MAX) return XIAOTAI_ERR_INVALID_ARG;
    const struct sockaddr_in remote = {
        .sin_family = AF_INET, .sin_port = htons(peer->port), .sin_addr.s_addr = peer->ipv4,
    };
    const ssize_t sent = sendto(fd, data, size, 0, (const struct sockaddr *)&remote, sizeof(remote));
    if (sent == (ssize_t)size) return XIAOTAI_OK;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return XIAOTAI_ERR_TIMEOUT;
    return XIAOTAI_FAIL;
}

void net_platform_udp_close(int fd)
{
    if (fd >= 0) (void)close(fd);
}
