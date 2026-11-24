// SPDX-License-Identifier: MIT
#include "../src/trace_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include "esp_netif.h"
#include "lwip/sockets.h"

/* SO_TYPE would take the TCP/IP core lock or wait for its task on every
 * packet. Track socket type when descriptors are created and released. */
enum { SOCKET_OTHER, SOCKET_UDP, SOCKET_CLOSING };
static atomic_uint s_socket_kind[MEMP_NUM_NETCONN];

static atomic_uint *socket_kind(int fd)
{
    return fd >= LWIP_SOCKET_OFFSET && fd - LWIP_SOCKET_OFFSET < MEMP_NUM_NETCONN ?
           &s_socket_kind[fd - LWIP_SOCKET_OFFSET] : NULL;
}

int __real_lwip_socket(int domain, int type, int protocol);
int __wrap_lwip_socket(int domain, int type, int protocol)
{
    const int fd = __real_lwip_socket(domain, type, protocol);
    atomic_uint *kind = socket_kind(fd);
    if (kind) atomic_store(kind, type == SOCK_DGRAM && protocol != IPPROTO_UDPLITE ? SOCKET_UDP : SOCKET_OTHER);
    return fd;
}

int __real_lwip_close(int fd);
int __wrap_lwip_close(int fd)
{
    atomic_uint *kind = socket_kind(fd);
    unsigned previous = kind ? atomic_exchange(kind, SOCKET_CLOSING) : SOCKET_OTHER;
    const int result = __real_lwip_close(fd);
    if (result != 0 && kind) {
        unsigned closing = SOCKET_CLOSING;
        (void)atomic_compare_exchange_strong(kind, &closing, previous);
    }
    return result;
}

esp_err_t __real_esp_netif_receive(esp_netif_t *netif, void *buffer, size_t size, void *owner);
esp_err_t __wrap_esp_netif_receive(esp_netif_t *netif, void *buffer, size_t size, void *owner)
{
    net_trace_frame(buffer, size);
    return __real_esp_netif_receive(netif, buffer, size, owner);
}

ssize_t __real_lwip_recvfrom(int fd, void *data, size_t size, int flags,
                            struct sockaddr *from, socklen_t *from_len);
ssize_t __wrap_lwip_recvfrom(int fd, void *data, size_t size, int flags,
                            struct sockaddr *from, socklen_t *from_len)
{
    const socklen_t capacity = from && from_len ? *from_len : 0;
    const ssize_t count = __real_lwip_recvfrom(fd, data, size, flags, from, from_len);
    const int saved_errno = errno;
    if (count > 0 && capacity >= sizeof(struct sockaddr_in) && *from_len >= sizeof(struct sockaddr_in) &&
        from->sa_family == AF_INET) {
        atomic_uint *kind = socket_kind(fd);
        if (kind && atomic_load(kind) == SOCKET_UDP) {
            const struct sockaddr_in *source = (const struct sockaddr_in *)from;
            net_trace_socket((const uint8_t *)&source->sin_addr.s_addr, (size_t)count);
        }
    }
    errno = saved_errno;
    return count;
}
