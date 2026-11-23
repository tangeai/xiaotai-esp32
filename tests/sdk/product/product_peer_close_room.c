// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
#include "product_platform.h"
#include "xiaotai/json.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/room.h"
#include "xiaotai/port/os.h"
#include "xiaotai_transport_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { ROOM_WORD = 0x2200U, ROOM_PEER = 0xb022U, WAIT_MS = 6000U };
static const char room_id[] = "group_room_exact";
static atomic_uint room_request, join_id;

void product_peer_close_room_prepare(void);
uintptr_t product_peer_close_room_open(void);
void product_peer_close_room_check(xiaotai_err_t wanted);

static void room_http(void *unused, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)unused;
    const char *body = NULL;
    if (request->method == XIAOTAI_HTTP_GET &&
        !strcmp(request->url, "http://fixture.invalid/call/v1/call/group/device/assignment")) {
        body = "{\"code\":200,\"data\":{\"device_id\":\"" PRODUCT_FIXTURE_DEVICE
               "\",\"desired_state\":\"joined\",\"room_id\":\"group_room_exact\","
               "\"room_code\":\"001234\",\"assignment_version\":3,\"device_name\":\"fixture\"}}";
    } else if (request->method == XIAOTAI_HTTP_POST &&
               !strcmp(request->url, "http://fixture.invalid/call/v1/call/group/device/connect-token")) {
        body = "{\"code\":200,\"data\":{\"peer_id\":\"room-peer\",\"token\":\"room-token\","
               "\"heartbeat_seconds\":10,\"lease_seconds\":90}}";
    } else if (request->method == XIAOTAI_HTTP_POST &&
               !strcmp(request->url, "http://fixture.invalid/call/v1/call/group/device/presence")) {
        body = "{\"code\":200,\"data\":{}}";
    }
    reply->status = body ? 200 : 404;
    reply->body = (const uint8_t *)body;
    reply->body_size = body ? strlen(body) : 0U;
}

static void connected(void *unused, xiaotai_link_kind_t kind, uint32_t request,
                      const char *peer, const char *token)
{
    (void)unused;
    assert(kind == XIAOTAI_LINK_WHIP && request && !strcmp(peer, "room-peer") && !strcmp(token, "room-token"));
    assert(!atomic_exchange(&room_request, request));
}

static void command(void *unused, uintptr_t conn, uint32_t word, const void *data, size_t bytes)
{
    (void)unused;
    assert(conn == ROOM_PEER && xiaotai_link_command_is(word, ROOM_WORD));
    xiaotai_json_token_t tokens[64];
    xiaotai_json_doc_t doc;
    size_t used;
    assert(xiaotai_json_parse(data, bytes, tokens, sizeof(tokens) / sizeof(*tokens), &used, &doc) == XIAOTAI_OK);
    if (!xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "method"), "join_room")) return;
    int64_t id;
    assert(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "id"), &id) == XIAOTAI_OK);
    assert(id > 0 && id <= UINT32_MAX && !atomic_exchange(&join_id, (uint32_t)id));
}

static uint32_t wait_value(const atomic_uint *value)
{
    uint32_t current = 0;
    for (unsigned i = 0; i < WAIT_MS; i += 5U) {
        current = atomic_load(value);
        if (current) break;
        xiaotai_delay_ms(5);
    }
    assert(current);
    return current;
}

void product_peer_close_room_prepare(void)
{
    product_platform_http_responder(room_http, NULL);
}

uintptr_t product_peer_close_room_open(void)
{
    xiaotai_transport_host_connect_hook(connected, NULL);
    xiaotai_transport_host_command_hook(command, NULL);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    const uint32_t generation = wait_value(&room_request);
    xiaotai_transport_host_connected(generation, ROOM_PEER, XIAOTAI_LINK_END_PEER, 0);
    const uint32_t id = wait_value(&join_id);
    char response[256];
    const int bytes = snprintf(response, sizeof(response),
        "{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":{\"session_id\":\"j\","
        "\"input_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1},"
        "\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}}}", id);
    assert(bytes > 0 && (size_t)bytes < sizeof(response));
    xiaotai_transport_host_command(ROOM_PEER, 0, ROOM_WORD, (const uint8_t *)response, (size_t)bytes);

    xiaotai_room_status_t room = {0};
    xiaotai_media_status_t media = {0};
    for (unsigned i = 0; i < WAIT_MS; i += 5U) {
        xiaotai_room_status(&room);
        xiaotai_media_status(&media);
        if (room.phase == XIAOTAI_ROOM_LISTENING && media.active && media.generation == generation) break;
        xiaotai_delay_ms(5);
    }
    assert(room.phase == XIAOTAI_ROOM_LISTENING && room.assigned && !strcmp(room.room_id, room_id) &&
           room.generation == generation && room.error == XIAOTAI_OK && !room.transmitting &&
           media.active && media.generation == generation && !media.uplink_open);
    xiaotai_transport_host_command_hook(NULL, NULL);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    return ROOM_PEER;
}

void product_peer_close_room_check(xiaotai_err_t wanted)
{
    xiaotai_room_status_t room = {0};
    xiaotai_media_status_t media = {0};
    for (unsigned i = 0; i < WAIT_MS; i += 5U) {
        xiaotai_room_status(&room);
        xiaotai_media_status(&media);
        if ((room.phase == XIAOTAI_ROOM_WAITING || room.phase == XIAOTAI_ROOM_BLOCKED) &&
            !room.generation && !media.active && !media.generation) break;
        xiaotai_delay_ms(5);
    }
    printf("PEER_CLOSE room phase=%u wait=%u error=%d want=%d code=%d generation=%u media=%u\n",
           (unsigned)room.phase, (unsigned)room.wait, room.error, wanted, (int)room.code,
           (unsigned)room.generation, (unsigned)media.active);
    fflush(stdout);
    assert((room.phase == XIAOTAI_ROOM_WAITING || room.phase == XIAOTAI_ROOM_BLOCKED) &&
           room.error == wanted && !room.code && room.assigned && !strcmp(room.room_id, room_id) &&
           !room.generation && !room.transmitting && !media.active && !media.generation && !media.uplink_open);
}
