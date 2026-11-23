// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
#include "product_ai.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/modules/wechat.h"
#include "xiaotai/port/os.h"
#include "xiaotai_transport_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { WAIT_TRIES = 1000, STEP_MS = 5, RTC_REMOTE_CLOSE = -40008 };
static const uintptr_t call_peer = 0xb023U, h5_peer = 0xb024U;
static atomic_uint request_id;
static uintptr_t ai_peer;

void product_peer_close_room_prepare(void);
uintptr_t product_peer_close_room_open(void);
void product_peer_close_room_check(xiaotai_err_t wanted);
uintptr_t product_peer_close_wechat_open(void);
void product_peer_close_prepare(const char *scene);
void product_peer_close_run(const char *scene);

static void call_http(void *ctx, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    static const char info[] = "{\"code\":200,\"data\":{\"device_id\":\"peer\",\"token\":\"fixture-token\"}}";
    static const char wanted[] = "{\"device_id\":\"peer\",\"room_id\":\"d_room\",\"purpose\":\"call\"}";
    if (request->method != XIAOTAI_HTTP_POST ||
        strcmp(request->url, "http://fixture.invalid/call/v1/call/device/info")) return;
    assert(request->body_size == sizeof(wanted) - 1 && !memcmp(request->body, wanted, request->body_size));
    reply->status = 200;
    reply->body = (const uint8_t *)info;
    reply->body_size = sizeof(info) - 1;
}

void product_peer_close_prepare(const char *scene)
{
    product_platform_prepare(true);
    if (!strcmp(scene, "room")) product_peer_close_room_prepare();
    else if (!strcmp(scene, "call")) {
        product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
            "{\"type\":\"device\",\"device_id\":\"peer\",\"remark\":\"Peer\",\"online\":true}]}}");
        product_platform_http_responder(call_http, NULL);
    }
}

static void call_connect(void *ctx, xiaotai_link_kind_t kind, uint32_t request,
                         const char *peer, const char *token)
{
    (void)ctx;
    assert(kind == XIAOTAI_LINK_P2P && request && !strcmp(peer, "peer") && !strcmp(token, "fixture-token"));
    assert(!atomic_exchange(&request_id, request));
}

static void accept_call(void *ctx) { assert(xiaotai_call_accept(*(const uint32_t *)ctx) == XIAOTAI_OK); }
static void read_call(void *ctx) { xiaotai_call_status(ctx); }
static void read_wechat(void *ctx) { xiaotai_wechat_status(ctx); }
static void read_ai(void *ctx) { xiaotai_ai_talk_status(ctx); }
static void record_ai_peer(uintptr_t peer) { assert(peer && !ai_peer); ai_peer = peer; }

static xiaotai_call_status_t call_wait(xiaotai_call_state_t wanted)
{
    xiaotai_call_status_t status;
    for (unsigned i = 0; i < WAIT_TRIES; ++i) {
        product_on_owner(read_call, &status);
        if (status.state == wanted) return status;
        xiaotai_delay_ms(STEP_MS);
    }
    assert(false && "device call phase not reached");
    return (xiaotai_call_status_t){0};
}

static void check_call(void)
{
    xiaotai_transport_host_connect_hook(call_connect, NULL);
    product_platform_message("cmd", "{\"type\":\"call_incoming\",\"payload\":{"
        "\"room_id\":\"d_room\",\"caller_id\":\"peer\"}}");
    uint32_t session = call_wait(XIAOTAI_CALL_INCOMING).session;
    assert(session);
    product_on_owner(accept_call, &session);
    for (unsigned i = 0; i < WAIT_TRIES && !atomic_load(&request_id); ++i) xiaotai_delay_ms(STEP_MS);
    const uint32_t request = atomic_load(&request_id);
    assert(request);
    xiaotai_transport_host_connected(request, call_peer, XIAOTAI_LINK_END_PEER, 0);
    assert(call_wait(XIAOTAI_CALL_ACTIVE).link == request);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    xiaotai_transport_host_error(call_peer, XIAOTAI_LINK_END_PEER, RTC_REMOTE_CLOSE);
    const xiaotai_call_status_t result = call_wait(XIAOTAI_CALL_IDLE);
    printf("PEER_CLOSE call end=%u error=%d link=%u\n", (unsigned)result.end, result.error, result.link);
    fflush(stdout);
    assert(result.end == XIAOTAI_CALL_END_NETWORK && result.error == RTC_REMOTE_CLOSE && !result.link);
}

static void check_wechat(bool reported, bool expects_fault)
{
    const uintptr_t peer = product_peer_close_wechat_open();
    if (reported) xiaotai_transport_host_error(peer, XIAOTAI_LINK_END_PEER, RTC_REMOTE_CLOSE);
    else xiaotai_transport_host_closed(peer);
    xiaotai_wechat_status_t status;
    for (unsigned i = 0; i < WAIT_TRIES; ++i) {
        product_on_owner(read_wechat, &status);
        if (status.state == XIAOTAI_WECHAT_IDLE) break;
        xiaotai_delay_ms(STEP_MS);
    }
    printf("PEER_CLOSE wechat reported=%u end=%u error=%d link=%u\n",
           (unsigned)reported, (unsigned)status.end, status.error, status.link);
    fflush(stdout);
    assert(status.state == XIAOTAI_WECHAT_IDLE && !status.link &&
           status.end == (expects_fault ? XIAOTAI_WECHAT_END_NETWORK : XIAOTAI_WECHAT_END_PEER_HANGUP) &&
           status.error == (expects_fault ? RTC_REMOTE_CLOSE : XIAOTAI_OK));
}

static void check_ai(void)
{
    product_ai_prepare();
    product_ai_connect_armed(product_ai_request(), record_ai_peer);
    assert(ai_peer);
    xiaotai_transport_host_error(ai_peer, XIAOTAI_LINK_END_PEER, RTC_REMOTE_CLOSE);
    xiaotai_ai_talk_status_t status;
    for (unsigned i = 0; i < WAIT_TRIES; ++i) {
        product_on_owner(read_ai, &status);
        if (status.phase == XIAOTAI_AI_TALK_IDLE) break;
        xiaotai_delay_ms(STEP_MS);
    }
    printf("PEER_CLOSE ai end=%u error=%d session=%u\n", (unsigned)status.end, status.error, status.session);
    fflush(stdout);
    assert(status.phase == XIAOTAI_AI_TALK_IDLE && !status.session &&
           status.end == XIAOTAI_AI_TALK_END_NETWORK && status.error == RTC_REMOTE_CLOSE);
}

static xiaotai_stream_status_t stream_wait(xiaotai_stream_phase_t wanted)
{
    xiaotai_stream_status_t status;
    for (unsigned i = 0; i < WAIT_TRIES; ++i) {
        xiaotai_stream_status(&status);
        if (status.phase == wanted) return status;
        xiaotai_delay_ms(STEP_MS);
    }
    assert(false && "H5 phase not reached");
    return (xiaotai_stream_status_t){0};
}

static void check_h5(bool peer_report)
{
    assert(!stream_wait(XIAOTAI_STREAM_LISTENING).generation);
    xiaotai_transport_host_accepted(h5_peer);
    assert(stream_wait(XIAOTAI_STREAM_ACTIVE).generation);
    xiaotai_transport_host_error(h5_peer, peer_report ? XIAOTAI_LINK_END_PEER : XIAOTAI_LINK_END_NETWORK,
                                RTC_REMOTE_CLOSE);
    const xiaotai_stream_status_t status = stream_wait(XIAOTAI_STREAM_LISTENING);
    printf("PEER_CLOSE h5 peer_report=%u end=%u error=%d generation=%u\n",
           (unsigned)peer_report, (unsigned)status.last_end, status.last_error, status.generation);
    fflush(stdout);
    assert(!status.generation && status.last_end == (peer_report ? XIAOTAI_STREAM_END_PEER : XIAOTAI_STREAM_END_LINK) &&
           status.last_error == (peer_report ? XIAOTAI_OK : XIAOTAI_FAIL));
}

void product_peer_close_run(const char *scene)
{
    product_platform_online();
    if (!strcmp(scene, "ai")) check_ai();
    else if (!strcmp(scene, "call")) check_call();
    else if (!strcmp(scene, "wechat")) check_wechat(true, true);
    else if (!strcmp(scene, "wechat-default")) check_wechat(true, false);
    else if (!strcmp(scene, "wechat-orderly")) check_wechat(false, false);
    else if (!strcmp(scene, "room")) {
        const uintptr_t peer = product_peer_close_room_open();
        xiaotai_transport_host_error(peer, XIAOTAI_LINK_END_PEER, RTC_REMOTE_CLOSE);
        product_peer_close_room_check(RTC_REMOTE_CLOSE);
    } else {
        assert(!strcmp(scene, "h5") || !strcmp(scene, "h5-network"));
        check_h5(!strcmp(scene, "h5"));
    }
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    assert(!media.active);
}
