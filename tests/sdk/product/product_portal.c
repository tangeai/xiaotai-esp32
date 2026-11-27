// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include <stdlib.h>
#include "captive_dns.h"
#include "internal.h"
#include "net_platform.h"
#include "portal.h"
#include "product_portal.h"

static atomic_uint portal_stack, dns_wait;

void product_portal_prepare(void)
{
    static const net_provision_portal_io_t io = {
        net_portal_start, net_portal_stop, net_captive_dns_start, net_captive_dns_stop,
    };
    if (net_provision_portal_register(&io) != XIAOTAI_OK) abort();
}

uint32_t product_portal_stack(void) { return atomic_load(&portal_stack); }

/* Keep the production admission and DNS task lifetimes. The native HTTP
 * server stand-in records its stack request without opening a socket. */
xiaotai_err_t net_platform_server_start(uint32_t ipv4, uint32_t stack_bytes, net_request_fn_t handler, void *ctx)
{
    (void)ctx;
    if (!ipv4 || !handler || !stack_bytes || atomic_exchange(&portal_stack, stack_bytes)) abort();
    return XIAOTAI_OK;
}

xiaotai_err_t net_platform_server_stop(void)
{
    atomic_store(&portal_stack, 0);
    return XIAOTAI_OK;
}

xiaotai_err_t net_platform_body_read(net_exchange_t *exchange, uint8_t *data, size_t capacity, size_t *size)
{
    (void)exchange; (void)data; (void)capacity; (void)size;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t net_platform_reply_begin(net_exchange_t *exchange, int status, const char *type,
                                       const char *const *headers, size_t count)
{
    (void)exchange; (void)status; (void)type; (void)headers; (void)count;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t net_platform_reply_chunk(net_exchange_t *exchange, const void *data, size_t size)
{
    (void)exchange; (void)data; (void)size;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t net_platform_reply_end(net_exchange_t *exchange)
{
    (void)exchange;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t net_platform_udp_open(uint32_t ipv4, uint16_t port, uint32_t wait, int *socket)
{
    if (!ipv4 || port != 53 || !wait || !socket) abort();
    atomic_store(&dns_wait, wait);
    *socket = 1;
    return XIAOTAI_OK;
}

int net_platform_udp_receive(int socket, uint8_t *data, size_t capacity, net_peer_t *peer)
{
    const uint32_t wait = atomic_load(&dns_wait);
    if (socket != 1 || !data || !capacity || !peer || !wait) abort();
    xiaotai_delay_ms(wait);
    return 0;
}

xiaotai_err_t net_platform_udp_send(int socket, const net_peer_t *peer, const uint8_t *data, size_t size)
{
    (void)socket; (void)peer; (void)data; (void)size;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

void net_platform_udp_close(int socket)
{
    if (socket != 1) abort();
    atomic_store(&dns_wait, 0);
}
