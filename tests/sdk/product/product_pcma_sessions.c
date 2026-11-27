// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_pcma.py
#include "opus_host_fixture.h"
#ifdef XIAOTAI_TEST_OPUS_SCENES
#include "product_opus_scene.h"
#endif
#include "product_ai.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/json.h"
#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"
#include "xiaotai/modules/room.h"
#include "xiaotai/port/os.h"
#include "xiaotai/xiaotai.h"
#include "xiaotai_transport_host.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ROOM_WORD = 0x2200U, ROOM_PEER = 0xb022U, PCMA_BYTES = 160U };
static uint8_t tx_payload[PCMA_BYTES + 1U];
static unsigned observed_tx;
static atomic_uint room_request, room_reply_id, observed_profile;
static uintptr_t echo_peer;

extern const xiaotai_audio_decoder_t media_opus_decoder;
extern int opus_packet_pad(unsigned char *data, int32_t len, int32_t new_len);
xiaotai_err_t __real_xiaotai_engine_media_init(void);
xiaotai_err_t __wrap_xiaotai_engine_media_init(void);
xiaotai_err_t __real_xiaotai_start(const xiaotai_config_t *config);
xiaotai_err_t __wrap_xiaotai_start(const xiaotai_config_t *config);
xiaotai_err_t __real_xiaotai_rtc_send(uintptr_t conn, const xiaotai_link_frame_t *frame,
                                     const uint8_t *data, size_t size, int *detail);
xiaotai_err_t __wrap_xiaotai_rtc_send(uintptr_t conn, const xiaotai_link_frame_t *frame,
                                     const uint8_t *data, size_t size, int *detail);

xiaotai_err_t __wrap_xiaotai_engine_media_init(void)
{
    const xiaotai_err_t result = __real_xiaotai_engine_media_init();
    return result ? result : xiaotai_media_decoder_register(&media_opus_decoder);
}

xiaotai_err_t __wrap_xiaotai_rtc_send(uintptr_t conn, const xiaotai_link_frame_t *frame,
                                     const uint8_t *data, size_t size, int *detail)
{
    if (data == tx_payload) {
        assert(conn == echo_peer && frame->kind == XIAOTAI_LINK_AUDIO && size == PCMA_BYTES);
        assert(frame->stream == 1 && frame->format.codec == XIAOTAI_LINK_CODEC_G711A &&
               frame->format.rate_hz == 8000U && frame->format.channels == 1U);
        assert(frame->timestamp_ms == 20U * (++observed_tx));
    }
    return __real_xiaotai_rtc_send(conn, frame, data, size, detail);
}

static int64_t integer(const xiaotai_json_doc_t *doc, int object, const char *key)
{
    int64_t value;
    assert(xiaotai_json_int(doc, xiaotai_json_get(doc, object, key), &value) == XIAOTAI_OK);
    return value;
}

static void rpc_audio(const xiaotai_json_doc_t *doc, int params, const char *key)
{
    const int audio = xiaotai_json_get(doc, params, key);
    assert(integer(doc, audio, "sample_rate") == 8000 && integer(doc, audio, "channels") == 1);
    assert(xiaotai_json_equals(doc, xiaotai_json_get(doc, audio, "codec"), "g711a"));
}

static void profile(const xiaotai_http_host_request_t *request)
{
    xiaotai_json_token_t storage[192];
    xiaotai_json_doc_t doc;
    size_t count;
    assert(request->method == XIAOTAI_HTTP_POST);
    assert(xiaotai_json_parse((const char *)request->body, request->body_size, storage,
                              sizeof(storage) / sizeof(*storage), &count, &doc) == XIAOTAI_OK);
    const int scenes = xiaotai_json_get(&doc, 0, "profiles");
    const char *const names[] = {"stream", "call", "voip"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        const int scene = xiaotai_json_get(&doc, scenes, names[i]);
        assert(integer(&doc, scene, "audio_rate") == 8000 && integer(&doc, scene, "audio_channels") == 1);
        const int down = xiaotai_json_get(&doc, scene, "down_audio_mt");
        assert(xiaotai_json_equals(&doc, i == 2 ? down : xiaotai_json_at(&doc, down, 0), "alaw"));
        if (i < 2) {
            const int up = xiaotai_json_get(&doc, scene, "up_audio_mt");
            assert(xiaotai_json_equals(&doc, xiaotai_json_at(&doc, up, 0), "alaw"));
        }
    }
    ++observed_profile;
}

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
               "\"heartbeat_seconds\":1,\"lease_seconds\":9}}";
    } else if (request->method == XIAOTAI_HTTP_POST &&
               !strcmp(request->url, "http://fixture.invalid/call/v1/call/group/device/presence")) {
        body = "{\"code\":200,\"data\":{}}";
    }
    reply->status = body ? 200 : 404;
    reply->body = (const uint8_t *)body;
    reply->body_size = body ? strlen(body) : 0;
}

static void room_connect(void *unused, xiaotai_link_kind_t kind, uint32_t request,
                         const char *peer, const char *token)
{
    (void)unused;
    assert(kind == XIAOTAI_LINK_WHIP && request && !strcmp(peer, "room-peer") && !strcmp(token, "room-token"));
    assert(!atomic_exchange(&room_request, request));
}

static void room_command(void *unused, uintptr_t conn, uint32_t word, const void *message, size_t size)
{
    (void)unused;
    assert(conn == ROOM_PEER && xiaotai_link_command_is(word, ROOM_WORD));
    xiaotai_json_token_t storage[64];
    xiaotai_json_doc_t doc;
    size_t count;
    assert(xiaotai_json_parse(message, size, storage, sizeof(storage) / sizeof(*storage), &count, &doc) == XIAOTAI_OK);
    if (!xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "method"), "join_room")) return;
    const int params = xiaotai_json_get(&doc, 0, "params");
    rpc_audio(&doc, params, "input_audio");
    rpc_audio(&doc, params, "output_audio");
    const int64_t id = integer(&doc, 0, "id");
    assert(id > 0 && id <= UINT32_MAX && !atomic_exchange(&room_reply_id, (uint32_t)id));
}

static uint32_t wait_value(const atomic_uint *value)
{
    for (unsigned i = 0; i < 1000 && !atomic_load(value); ++i) xiaotai_delay_ms(5);
    const uint32_t result = atomic_load(value);
    assert(result);
    return result;
}

static xiaotai_link_media_stats_t receive_counts(uint32_t generation)
{
    xiaotai_link_stats_t stats;
    assert(xiaotai_link_stats(generation, &stats));
    return stats.audio;
}

static void check_receive(uintptr_t conn, uint32_t generation, bool room)
{
    uint8_t payload[PCMA_BYTES] = {0};
    xiaotai_link_frame_t frame = {.kind = XIAOTAI_LINK_AUDIO, .stream = 1,
        .timestamp_ms = 20, .format = {XIAOTAI_LINK_CODEC_G711A, 8000U, 1U}};
    const xiaotai_link_media_stats_t before = receive_counts(generation);
    xiaotai_transport_host_frame(conn + 1U, &frame, payload, sizeof(payload));
    frame.format.rate_hz = 16000U;
    xiaotai_transport_host_frame(conn, &frame, payload, sizeof(payload));
    frame.format.rate_hz = 8000U;
    xiaotai_transport_host_frame(conn, &frame, payload, sizeof(payload));
    if (room) {
        frame.stream = 10;
        xiaotai_transport_host_frame(conn, &frame, payload, sizeof(payload));
        frame.stream = 1;
    }
    frame.format = (xiaotai_link_format_t){XIAOTAI_LINK_CODEC_OPUS, 16000U, 1U};
    frame.timestamp_ms += 20;
    const size_t bytes = host_opus_packet(320, payload, sizeof(payload));
    assert(opus_packet_pad(payload, (int32_t)bytes, sizeof(payload)) == 0);
    xiaotai_transport_host_frame(conn, &frame, payload, sizeof(payload));
    const xiaotai_link_media_stats_t after = receive_counts(generation);
    assert(after.received == before.received + 2U);
    assert(after.receive_refused == before.receive_refused &&
           after.receive_filtered == before.receive_filtered + (room ? 2U : 1U));
    printf("%s: %s stream=1 received PCMA8+Opus16, rejected PCMA16/foreign%s\n", XIAOTAI_PRODUCT_NAME,
           room ? "room" : "AI", room ? "/wrong-stream" : "");
}

static void check_send(uintptr_t conn, uint32_t generation)
{
    echo_peer = conn;
    observed_tx = 0;
    assert(xiaotai_link_send_audio(generation, 20, tx_payload, 159) == XIAOTAI_ERR_INVALID_SIZE);
    assert(xiaotai_link_send_audio(generation, 20, tx_payload, 161) == XIAOTAI_ERR_INVALID_SIZE);
    assert(!observed_tx);
    for (unsigned pts = 20; pts <= 40; pts += 20) {
        xiaotai_err_t result = XIAOTAI_ERR_BUSY;
        for (unsigned retry = 0; retry < 20 && result == XIAOTAI_ERR_BUSY; ++retry) {
            result = xiaotai_link_send_audio(generation, pts, tx_payload, PCMA_BYTES);
            if (result == XIAOTAI_ERR_BUSY) xiaotai_delay_ms(1);
        }
        assert(result == XIAOTAI_OK);
    }
    assert(observed_tx == 2);
}

static uint32_t ai_generation;

static void ai_peer(uintptr_t peer)
{
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    assert(media.generation && !media.active);
    ai_generation = media.generation;
#ifdef XIAOTAI_TEST_OPUS_SCENES
    if (!getenv("XIAOTAI_HOST_OPUS_SCENE"))
#endif
    check_receive(peer, media.generation, false);
    echo_peer = peer;
}

static void probe_room(void)
{
    xiaotai_transport_host_connect_hook(room_connect, NULL);
    xiaotai_transport_host_command_hook(room_command, NULL);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    xiaotai_transport_host_connected(wait_value(&room_request), ROOM_PEER, XIAOTAI_LINK_END_PEER, 0);
    const uint32_t id = wait_value(&room_reply_id);
    char response[256];
    const int bytes = snprintf(response, sizeof(response),
        "{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":{\"session_id\":\"j\","
        "\"input_audio\":{\"codec\":\"alaw\",\"sample_rate\":8000,\"channels\":1},"
        "\"output_audio\":{\"codec\":\"alaw\",\"sample_rate\":8000,\"channels\":1}}}", id);
    assert(bytes > 0 && (size_t)bytes < sizeof(response));
    xiaotai_transport_host_command(ROOM_PEER, 0, ROOM_WORD, (const uint8_t *)response, (size_t)bytes);
    xiaotai_media_status_t media;
    unsigned tries = 0;
    do {
        xiaotai_media_status(&media);
        if (media.active && media.generation == id) break;
        xiaotai_delay_ms(5);
    } while (++tries < 1000);
    assert(media.active && media.generation == id && !media.uplink_open);
#ifdef XIAOTAI_TEST_OPUS_SCENES
    if (getenv("XIAOTAI_HOST_OPUS_SCENE")) {
        product_opus_legacy_scene(ROOM_PEER, id, "room", 1U, false);
        assert(xiaotai_room_attend(false) == XIAOTAI_OK);
        return;
    }
#endif
    check_receive(ROOM_PEER, id, true);
    assert(xiaotai_room_talk(id, true) == XIAOTAI_OK);
    check_send(ROOM_PEER, id);
    assert(xiaotai_room_talk(id, false) == XIAOTAI_OK);
    xiaotai_media_status(&media);
    assert(!media.uplink_open);
    assert(xiaotai_room_attend(false) == XIAOTAI_OK);
}

xiaotai_err_t __wrap_xiaotai_start(const xiaotai_config_t *config)
{
    product_platform_prepare(true);
    product_platform_profile_observer(profile);
    product_platform_http_responder(room_http, NULL);
    const xiaotai_err_t result = __real_xiaotai_start(config);
    if (result != XIAOTAI_OK) return result;
    product_platform_online();
    assert(wait_value(&observed_profile) == 1);
    printf("%s: stream, call and voip profiles advertise A-law 8k mono\n", XIAOTAI_PRODUCT_NAME);
#ifdef XIAOTAI_TEST_OPUS_SCENES
    const char *const opus_scene = getenv("XIAOTAI_HOST_OPUS_SCENE");
    if (opus_scene && !strcmp(opus_scene, "room")) {
        probe_room();
        return result;
    }
#endif

    product_ai_prepare();
    const uint32_t session = product_ai_request();
    product_ai_connect_armed(session, ai_peer);
#ifdef XIAOTAI_TEST_OPUS_SCENES
    if (opus_scene) {
        assert(!strcmp(opus_scene, "ai"));
        product_opus_legacy_scene(echo_peer, ai_generation, "ai", 1U, true);
        product_ai_stop(session);
        return result;
    }
#endif
    check_send(echo_peer, ai_generation);
    product_ai_stop(session);
    probe_room();
    puts("PASS: AI/Room RPC and 20 ms PCMA wire");
    return result;
}
