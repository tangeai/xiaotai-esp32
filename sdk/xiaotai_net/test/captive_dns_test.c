// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_captive_portal_contract.py
#include "captive_dns.h"
#include "net_platform.h"
#include "xiaotai/net_config.h"
#include "xiaotai/port/os.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#define TEST_SOCKET 71
#define WAIT_MS 2000U

static const uint8_t query[] = {
    0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 1,
    1, 'a', 3, 'l', 'a', 'n', 0, 0, 1, 0, 1,
    0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t expected[] = {
    0x12, 0x34, 0x85, 0x00, 0, 1, 0, 1, 0, 0, 0, 0,
    1, 'a', 3, 'l', 'a', 'n', 0, 0, 1, 0, 1,
    0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 6, 1,
};

static uint32_t ap;
static atomic_uint opens, creates, closes, sends;
static atomic_bool bound, hold_receive, send_query;
static bool fail_open, fail_task;
static xiaotai_signal_t listening, release_receive, sent;

xiaotai_err_t net_platform_udp_open(uint32_t ipv4, uint16_t port, uint32_t timeout_ms, int *socket)
{
    assert(ipv4 == ap && port == 53 && timeout_ms == 100);
    assert(!atomic_load(&bound));
    atomic_fetch_add(&opens, 1);
    if (fail_open) return XIAOTAI_FAIL;
    atomic_store(&bound, true);
    *socket = TEST_SOCKET;
    return XIAOTAI_OK;
}

void net_platform_udp_close(int socket)
{
    assert(socket == TEST_SOCKET && atomic_exchange(&bound, false));
    atomic_fetch_add(&closes, 1);
}

int net_platform_udp_receive(int socket, uint8_t *data, size_t capacity, net_peer_t *peer)
{
    assert(socket == TEST_SOCKET && capacity == 512 && atomic_load(&bound));
    if (atomic_load(&hold_receive)) {
        xiaotai_signal_give(&listening);
        assert(xiaotai_signal_take(&release_receive, WAIT_MS));
    } else if (!atomic_exchange(&send_query, false)) {
        xiaotai_delay_ms(25);
        return 0;
    }
    memcpy(data, query, sizeof(query));
    *peer = (net_peer_t){.ipv4 = ap, .port = 12345};
    return sizeof(query);
}

xiaotai_err_t net_platform_udp_send(int socket, const net_peer_t *peer, const uint8_t *data, size_t size)
{
    assert(socket == TEST_SOCKET && peer->port == 12345 && peer->ipv4 == ap && atomic_load(&bound));
    assert(size == sizeof(expected) && !memcmp(data, expected, size));
    atomic_fetch_add(&sends, 1);
    xiaotai_signal_give(&sent);
    return XIAOTAI_OK;
}

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    assert(atomic_load(&bound) && !strcmp(config->name, "xiaotai_dns") && config->priority == 4 &&
           config->stack == XIAOTAI_MEM_BULK && config->stack_bytes == XIAOTAI_NET_DNS_STACK_BYTES);
    atomic_fetch_add(&creates, 1);
    return fail_task ? XIAOTAI_ERR_NO_MEM : __real_xiaotai_task_create(config, task);
}

static void bytes(void)
{
    uint8_t packet[513], reply[513];
    assert(net_captive_dns_reply(ap, query, sizeof(query), reply, sizeof(reply)) == sizeof(expected));
    assert(!memcmp(reply, expected, sizeof(expected)));
    assert(!net_captive_dns_reply(ap, query, sizeof(query), reply, sizeof(expected) - 1));
    assert(!net_captive_dns_reply(ap, NULL, sizeof(query), reply, sizeof(reply)));
    assert(!net_captive_dns_reply(ap, query, sizeof(query), NULL, sizeof(reply)));
    assert(!net_captive_dns_reply(ap, query, 11, reply, sizeof(reply)));
    assert(!net_captive_dns_reply(ap, query, 22, reply, sizeof(reply)));
    for (unsigned count = 0; count <= 2; count += 2) {
        memcpy(packet, query, sizeof(query));
        packet[5] = (uint8_t)count;
        assert(!net_captive_dns_reply(ap, packet, sizeof(query), reply, sizeof(reply)));
    }
    memcpy(packet, query, sizeof(query));
    packet[12] = 0xc0;
    assert(!net_captive_dns_reply(ap, packet, sizeof(query), reply, sizeof(reply)));
    packet[12] = 64;
    assert(!net_captive_dns_reply(ap, packet, sizeof(query), reply, sizeof(reply)));
    packet[12] = 63;
    assert(!net_captive_dns_reply(ap, packet, sizeof(query), reply, sizeof(reply)));
    for (unsigned unsupported = 0; unsupported < 2; ++unsupported) {
        memcpy(packet, query, sizeof(query));
        packet[2] = 0; /* RD off. */
        packet[unsupported ? 22 : 20] = unsupported ? 3U : 28U; /* CH or AAAA. */
        assert(net_captive_dns_reply(ap, packet, sizeof(query), packet, sizeof(packet)) == 23);
        assert(packet[2] == 0x84 && packet[3] == 0 && packet[7] == 0 && packet[11] == 0);
        assert(packet[unsupported ? 22 : 20] == (unsupported ? 3U : 28U));
    }

    memset(packet, 'x', sizeof(packet));
    memcpy(packet, query, sizeof(query));
    assert(net_captive_dns_reply(ap, packet, 512, reply, sizeof(reply)) == sizeof(expected));
    assert(!memcmp(reply, expected, sizeof(expected)));
    assert(!net_captive_dns_reply(ap, packet, 513, reply, sizeof(reply)));
    memset(packet, 'x', sizeof(packet));
    memcpy(packet, query, 12);
    packet[12] = 63;
    packet[76] = 0;
    packet[77] = packet[79] = 0;
    packet[78] = packet[80] = 1;
    assert(net_captive_dns_reply(ap, packet, 81, reply, sizeof(reply)) == 97);
    assert(!memcmp(packet + 12, reply + 12, 69));
}

static void lifecycle(void)
{
    assert(xiaotai_signal_init(&listening) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&release_receive) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&sent) == XIAOTAI_OK);
    assert(net_captive_dns_stop() == XIAOTAI_OK);
    assert(net_captive_dns_start(0) == XIAOTAI_ERR_INVALID_ARG);
    fail_open = true;
    assert(net_captive_dns_start(ap) == XIAOTAI_FAIL);
    assert(atomic_load(&opens) == 1 && !atomic_load(&creates) && !atomic_load(&closes));
    fail_open = false;
    fail_task = true;
    assert(net_captive_dns_start(ap) == XIAOTAI_ERR_NO_MEM);
    assert(atomic_load(&opens) == 2 && atomic_load(&creates) == 1 && atomic_load(&closes) == 1);
    fail_task = false;
    atomic_store(&send_query, true);
    assert(net_captive_dns_start(ap) == XIAOTAI_OK);
    assert(net_captive_dns_start(ap) == XIAOTAI_OK && atomic_load(&opens) == 3);
    assert(net_captive_dns_start(ap ^ 1U) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_signal_take(&sent, WAIT_MS));
    const int64_t first = xiaotai_time_us();
    assert(net_captive_dns_stop() == XIAOTAI_OK && xiaotai_time_us() - first < 500000);
    assert(atomic_load(&closes) == 2 && atomic_load(&sends) == 1);

    atomic_store(&hold_receive, true);
    assert(net_captive_dns_start(ap) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&listening, WAIT_MS));
    assert(net_captive_dns_stop() == XIAOTAI_ERR_TIMEOUT);
    assert(net_captive_dns_start(ap) == XIAOTAI_ERR_INVALID_STATE && atomic_load(&closes) == 2);
    xiaotai_signal_give(&release_receive);
    assert(net_captive_dns_stop() == XIAOTAI_OK);
    assert(atomic_load(&closes) == 3 && atomic_load(&sends) == 1);
    xiaotai_signal_deinit(&sent);
    xiaotai_signal_deinit(&release_receive);
    xiaotai_signal_deinit(&listening);
}

int main(void)
{
    memcpy(&ap, expected + sizeof(expected) - sizeof(ap), sizeof(ap));
    bytes();
    lifecycle();
    return 0;
}
