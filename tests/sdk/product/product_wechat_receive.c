// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
// Replaces legacy: tests/xmini/xmini_rtc/test_rx_repacketize.c::rx_repacketize
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/media.h"
#include "xiaotai/media_rx_ingress.h"
#include "xiaotai/modules/wechat.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_transport_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define PEER ((uintptr_t)0x7401U)
static atomic_uint request;
static atomic_uint subscriptions;

static void submitted(void *ctx, xiaotai_link_kind_t kind, uint32_t id, const char *peer, const char *token)
{
    (void)ctx;
    assert(kind == XIAOTAI_LINK_WHIP && id && !strcmp(peer, "fixture-wx-peer") && !strcmp(token, "fixture-wx-token"));
    assert(!atomic_exchange(&request, id));
}

static void requested(void *ctx, xiaotai_transport_host_call_kind_t kind, uintptr_t conn, int64_t val)
{
    (void)ctx;
    (void)conn;
    (void)val;
    if (kind == XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) atomic_fetch_add(&subscriptions, 1U);
}

static void status(void *ctx) { xiaotai_wechat_status(ctx); }

static xiaotai_wechat_status_t wait_for(xiaotai_wechat_state_t state, bool ready)
{
    for (unsigned i = 0; i < 1000U; ++i) {
        xiaotai_wechat_status_t s;
        product_on_owner(status, &s);
        if (s.state == state && (!ready || s.ready)) return s;
        xiaotai_delay_ms(5U);
    }
    assert(false && "WeChat scenario phase not reached");
    return (xiaotai_wechat_status_t){0};
}

static void accept(void *ctx)
{
    assert(xiaotai_wechat_accept(*(const uint32_t *)ctx) == XIAOTAI_OK);
}

static void inject(unsigned stream, uint32_t pts, const uint8_t *data, size_t size)
{
    const xiaotai_link_frame_t frame = {.kind = XIAOTAI_LINK_AUDIO, .stream = (uint8_t)stream, .timestamp_ms = pts,
                                       .format = {XIAOTAI_LINK_CODEC_G711A, 8000U, 1U}};
    xiaotai_transport_host_frame(PEER, &frame, data, size);
}

static xiaotai_media_stats_t media_stats(void)
{
    xiaotai_media_stats_t stats;
    xiaotai_media_stats(&stats);
    return stats;
}

static xiaotai_link_media_stats_t link_stats(uint32_t generation)
{
    xiaotai_link_stats_t stats;
    assert(xiaotai_link_stats(generation, &stats));
    return stats.audio;
}

static xiaotai_wechat_status_t open_for_answer(void)
{
    product_platform_online();
    (void)wait_for(XIAOTAI_WECHAT_IDLE, true);
    xiaotai_transport_host_connect_hook(submitted, NULL);
    xiaotai_transport_host_media_hook(requested, NULL);
    product_platform_message("cmd", "{\"channel\":\"wx\",\"type\":\"call_incoming\",\"payload\":{"
        "\"wx_room_id\":\"fixture-wx-room\",\"wx_room_type\":\"voice\",\"wx_from\":\"fixture-phone\","
        "\"peer_id\":\"fixture-wx-peer\",\"token\":\"fixture-wx-token\"}}");
    const xiaotai_wechat_status_t incoming = wait_for(XIAOTAI_WECHAT_INCOMING, true);
    assert(incoming.session && !incoming.outgoing);
    uint32_t session = incoming.session;
    product_on_owner(accept, &session);
    for (unsigned i = 0; i < 1000U && !atomic_load(&request); ++i) xiaotai_delay_ms(5U);
    assert(atomic_load(&request));
    xiaotai_transport_host_connected(atomic_load(&request), PEER, XIAOTAI_LINK_END_PEER, 0);
    assert(xiaotai_transport_host_settle(5000));
    const xiaotai_wechat_status_t waiting = wait_for(XIAOTAI_WECHAT_CONNECTING, true);
    assert(waiting.session == session && waiting.link);
    return waiting;
}

uintptr_t product_peer_close_wechat_open(void);
uintptr_t product_peer_close_wechat_open(void)
{
    const xiaotai_wechat_status_t waiting = open_for_answer();
    xiaotai_transport_host_command(PEER, 0, 0x2000U, NULL, 0);
    const xiaotai_wechat_status_t active = wait_for(XIAOTAI_WECHAT_ACTIVE, true);
    assert(active.link == waiting.link && active.session == waiting.session);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    xiaotai_transport_host_media_hook(NULL, NULL);
    return PEER;
}

void product_wechat_receive(void)
{
    const xiaotai_wechat_status_t waiting = open_for_answer();
    xiaotai_transport_host_command(PEER, 0, 0x2000U, NULL, 0);
    const xiaotai_wechat_status_t active = wait_for(XIAOTAI_WECHAT_ACTIVE, true);
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    assert(active.session == waiting.session && active.link && media.active && media.generation == active.link);

    static const uint8_t audio[160] = {[0] = 42U, [80] = 120U, [159] = 33U};
    const xiaotai_media_stats_t before = media_stats();
    const xiaotai_link_media_stats_t link = link_stats(active.link);
#if CONFIG_XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    assert(xiaotai_media_ingress_epoch(active.link) != 0U);
    const uint32_t allocs = xiaotai_mem_host_allocations();
    inject(0, 1000U, audio, 80U);
    assert(media_stats().rx_received == before.rx_received);
    inject(7, 1010U, audio, sizeof(audio));
    assert(media_stats().rx_received == before.rx_received && link_stats(active.link).receive_filtered == link.receive_filtered + 1U);
    inject(0, 9000U, audio + 80U, 80U);
    assert(media_stats().rx_received == before.rx_received + 1U);
    assert(media_stats().rx_overflow == before.rx_overflow && xiaotai_mem_host_allocations() == allocs);
    puts("WeChat local stream=0: 80+80 -> one fixed A-law block, stream 7 rejected, synchronous heap unchanged");
#else
    inject(0, 1000U, audio, sizeof(audio));
    inject(7, 1020U, audio, sizeof(audio));
    assert(media_stats().rx_received == before.rx_received + 2U);
    assert(link_stats(active.link).receive_filtered == link.receive_filtered);
    static const uint8_t oversized[1520];
    inject(0, 1040U, oversized, sizeof(oversized));
    const xiaotai_media_stats_t after = media_stats();
    assert(after.rx_received == before.rx_received + 2U && after.rx_invalid == before.rx_invalid + 1U);
    puts("WeChat default ANY: streams 0 and 7 accepted, oversized packet rejected");
#endif
    assert(atomic_load(&subscriptions) == 0U);
    xiaotai_transport_host_closed(PEER);
    assert(xiaotai_transport_host_settle(5000));
    assert(!wait_for(XIAOTAI_WECHAT_IDLE, false).session && atomic_load(&subscriptions) == 0U);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    xiaotai_transport_host_media_hook(NULL, NULL);
}

void product_wechat_answer_payload(bool strict);

static unsigned received_commands(uint32_t link)
{
    xiaotai_link_stats_t counts;
    assert(xiaotai_link_stats(link, &counts));
    return counts.commands_received;
}

static xiaotai_wechat_status_t deliver_answer(uint32_t link, uint32_t sequence, uint32_t word, const char *body)
{
    const unsigned before = received_commands(link);
    xiaotai_transport_host_command(PEER, sequence, word, (const uint8_t *)body, body ? strlen(body) : 0U);
    for (unsigned i = 0; i < 500U && received_commands(link) == before; ++i) xiaotai_delay_ms(2U);
    assert(received_commands(link) == before + 1U);
    xiaotai_wechat_status_t now;
    product_on_owner(status, &now);
    return now;
}

void product_wechat_answer_payload(bool strict)
{
    const xiaotai_wechat_status_t waiting = open_for_answer();
    const uint32_t link = waiting.link;
    xiaotai_wechat_status_t now;
    if (strict) {
        static const char *const bad[] = {"not-json", "null", "[]", "1", "\"string\"", "{", "{} trailing"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            now = deliver_answer(link, 0U, 0x2000U, bad[i]);
            xiaotai_media_status_t media;
            xiaotai_media_status(&media);
            printf("RTC answered body=%s state=%u media_active=%u answered=%u\n", bad[i], now.state, media.active, now.answered);
            fflush(stdout);
            assert(now.state == XIAOTAI_WECHAT_CONNECTING && now.session == waiting.session && !media.active &&
                   now.answered == waiting.answered);
        }
        now = deliver_answer(link, 0U, 0x12000U, "{}");
        assert(now.state == XIAOTAI_WECHAT_CONNECTING && now.answered == waiting.answered);
    }

    /* A transport sequence is independent of the raw command word. */
    now = deliver_answer(link, 7U, 0x2000U, strict ? "{}" : "null");
    const xiaotai_wechat_status_t active = wait_for(XIAOTAI_WECHAT_ACTIVE, true);
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    assert(active.link == link && active.session == waiting.session && media.active && media.generation == link &&
           active.answered == waiting.answered + 1U);
    now = deliver_answer(link, 0U, 0x2000U, "{\"opaque\":true}");
    assert(now.state == XIAOTAI_WECHAT_ACTIVE && now.answered == active.answered);
    printf("RTC answered %s with separate sequence 7, media activated once; duplicate did not activate again\n",
           strict ? "JSON object" : "legacy opaque payload");
    xiaotai_transport_host_closed(PEER);
    assert(xiaotai_transport_host_settle(5000));
    assert(!wait_for(XIAOTAI_WECHAT_IDLE, false).session);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    xiaotai_transport_host_media_hook(NULL, NULL);
}
