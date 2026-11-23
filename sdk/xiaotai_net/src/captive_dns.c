// SPDX-License-Identifier: MIT
#include "captive_dns.h"
#include "internal.h"
#include "net_platform.h"
#include "xiaotai/net_config.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"

#include <stdatomic.h>
#include <string.h>

#define DNS_PORT 53U
#define PACKET_MAX 512U
#define HEADER_SIZE 12U
#define ANSWER_SIZE 16U
#define RECEIVE_MS 100U
#define STOP_MS 500U

static struct {
    uint32_t ipv4;
    int socket;
    xiaotai_signal_t done;
    atomic_bool stopping;
    bool started;
} s_dns;

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

size_t net_captive_dns_reply(uint32_t ipv4, const uint8_t *query, size_t size, uint8_t *reply, size_t capacity)
{
    if (!query || !reply || size < HEADER_SIZE || size > PACKET_MAX || read_be16(query + 4) != 1U)
        return 0;

    size_t offset = HEADER_SIZE;
    for (;;) {
        if (offset == size) return 0;
        const uint8_t length = query[offset++];
        if (!length) break;
        if (length > 63U || length > size - offset) return 0;
        offset += length;
    }
    if (size - offset < 4U) return 0;
    const uint16_t flags = read_be16(query + 2);
    const bool answer = read_be16(query + offset) == 1U && read_be16(query + offset + 2U) == 1U;
    const size_t question_end = offset + 4U;
    const size_t reply_size = question_end + (answer ? ANSWER_SIZE : 0U);
    if (reply_size > capacity || reply_size > PACKET_MAX) return 0;

    /* Keep the transaction and question, not any input answer or EDNS data. */
    memmove(reply, query, question_end);
    write_be16(reply + 2, (uint16_t)(0x8400U | (flags & 0x0100U)));
    write_be16(reply + 6, answer ? 1U : 0U);
    write_be16(reply + 8, 0U);
    write_be16(reply + 10, 0U);
    if (answer) {
        static const uint8_t answer_head[] = {
            0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4,
        };
        memcpy(reply + question_end, answer_head, sizeof(answer_head));
        memcpy(reply + question_end + sizeof(answer_head), &ipv4, sizeof(ipv4));
    }
    return reply_size;
}

static void dns_task(void *ctx)
{
    (void)ctx;
    const int socket = s_dns.socket;
    const uint32_t ipv4 = s_dns.ipv4;
    uint8_t query[PACKET_MAX], reply[PACKET_MAX];
    bool receive_failed = false;
    while (!atomic_load(&s_dns.stopping)) {
        net_peer_t peer;
        const int count = net_platform_udp_receive(socket, query, sizeof(query), &peer);
        if (atomic_load(&s_dns.stopping)) break;
        if (count < 0) {
            if (!receive_failed) XIAOTAI_LOGW("xiaotai_dns", "receive failed");
            receive_failed = true;
            xiaotai_delay_ms(RECEIVE_MS);
        } else {
            receive_failed = false;
            if (count > 0) {
                net_diag_count_dns();
                const size_t size = net_captive_dns_reply(ipv4, query, (size_t)count, reply, sizeof(reply));
                if (size) (void)net_platform_udp_send(socket, &peer, reply, size);
            }
        }
    }
    net_platform_udp_close(socket);
    xiaotai_signal_give(&s_dns.done);
    xiaotai_task_exit();
}

xiaotai_err_t net_captive_dns_start(uint32_t ipv4)
{
    if (!ipv4) return XIAOTAI_ERR_INVALID_ARG;
    if (s_dns.started) {
        return s_dns.ipv4 == ipv4 && !atomic_load(&s_dns.stopping) ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
    }
    int socket;
    xiaotai_err_t err = net_platform_udp_open(ipv4, DNS_PORT, RECEIVE_MS, &socket);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_signal_init(&s_dns.done);
    if (err != XIAOTAI_OK) {
        net_platform_udp_close(socket);
        return err;
    }
    s_dns.socket = socket;
    s_dns.ipv4 = ipv4;
    atomic_store(&s_dns.stopping, false);
    const xiaotai_task_config_t config = {
        .name = "xiaotai_dns", .entry = dns_task, .stack_bytes = XIAOTAI_NET_DNS_STACK_BYTES,
        .stack = XIAOTAI_MEM_BULK, .priority = 4, .core = XIAOTAI_CORE_ANY,
    };
    xiaotai_task_t task;
    err = xiaotai_task_create(&config, &task);
    if (err != XIAOTAI_OK) {
        net_platform_udp_close(socket);
        xiaotai_signal_deinit(&s_dns.done);
        return err;
    }
    s_dns.started = true;
    return XIAOTAI_OK;
}

xiaotai_err_t net_captive_dns_stop(void)
{
    if (!s_dns.started) return XIAOTAI_OK;
    atomic_store(&s_dns.stopping, true);
    if (!xiaotai_signal_take(&s_dns.done, STOP_MS)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_signal_deinit(&s_dns.done);
    s_dns.started = false;
    return XIAOTAI_OK;
}
