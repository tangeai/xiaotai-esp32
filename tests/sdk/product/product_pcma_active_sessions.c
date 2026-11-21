// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_pcma.py
#include "opus_host_fixture.h"
#ifdef XIAOTAI_TEST_OPUS_SCENES
#include "product_opus_scene.h"
#endif
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/modules/wechat.h"
#include "xiaotai/port/os.h"
#include "xiaotai/xiaotai.h"
#include "xiaotai_transport_host.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PCMA_BYTES = 160, TX_STREAM = 10, H5_RX_STREAM = 14, OTHER_RX_STREAM = 7 };
typedef enum { H5, DEVICE, WECHAT } scene_t;
static scene_t scene;
static const char *scene_name;
static const uintptr_t peer = 0xb025U;
static atomic_uint request_id;
static uint8_t tx_payload[PCMA_BYTES + 1];
static unsigned tx_count;
static unsigned subscribe_on, subscribe_off;

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
xiaotai_err_t __real_xiaotai_rtc_subscribe(uintptr_t conn, xiaotai_link_media_kind_t kind, uint8_t stream, bool on);
xiaotai_err_t __wrap_xiaotai_rtc_subscribe(uintptr_t conn, xiaotai_link_media_kind_t kind, uint8_t stream, bool on);

xiaotai_err_t __wrap_xiaotai_engine_media_init(void)
{
    const xiaotai_err_t result = __real_xiaotai_engine_media_init();
    return result ? result : xiaotai_media_decoder_register(&media_opus_decoder);
}

xiaotai_err_t __wrap_xiaotai_rtc_send(uintptr_t conn, const xiaotai_link_frame_t *frame,
                                     const uint8_t *data, size_t size, int *detail)
{
    if (data == tx_payload) {
        assert(conn == peer && size == PCMA_BYTES && frame->kind == XIAOTAI_LINK_AUDIO);
        assert(frame->stream == TX_STREAM && frame->format.codec == XIAOTAI_LINK_CODEC_G711A &&
               frame->format.rate_hz == 8000U && frame->format.channels == 1U);
        assert(frame->timestamp_ms == 20U * (++tx_count));
    }
    return __real_xiaotai_rtc_send(conn, frame, data, size, detail);
}

xiaotai_err_t __wrap_xiaotai_rtc_subscribe(uintptr_t conn, xiaotai_link_media_kind_t kind, uint8_t stream, bool on)
{
    if (scene == DEVICE && conn == peer && kind == XIAOTAI_LINK_AUDIO) {
        assert(stream == TX_STREAM);
        if (on) ++subscribe_on;
        else ++subscribe_off;
    }
    return __real_xiaotai_rtc_subscribe(conn, kind, stream, on);
}

static void call_http(void *unused, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)unused;
    static const char body[] = "{\"code\":200,\"data\":{\"device_id\":\"peer\",\"token\":\"fixture-token\"}}";
    reply->status = 404;
    if (request->method != XIAOTAI_HTTP_POST ||
        strcmp(request->url, "http://fixture.invalid/call/v1/call/device/info")) return;
    static const char wanted[] = "{\"device_id\":\"peer\",\"room_id\":\"d_room\",\"purpose\":\"call\"}";
    assert(request->body_size == sizeof(wanted) - 1U && !memcmp(request->body, wanted, request->body_size));
    reply->status = 200;
    reply->body = (const uint8_t *)body;
    reply->body_size = sizeof(body) - 1U;
}

static void connected_request(void *unused, xiaotai_link_kind_t kind, uint32_t request,
                              const char *peer_id, const char *token)
{
    (void)unused;
    assert(scene != H5 && kind == (scene == DEVICE ? XIAOTAI_LINK_P2P : XIAOTAI_LINK_WHIP));
    assert(!strcmp(peer_id, scene == DEVICE ? "peer" : "fixture-wx-peer"));
    assert(!strcmp(token, scene == DEVICE ? "fixture-token" : "fixture-wx-token"));
    assert(request && !atomic_exchange(&request_id, request));
}

static void device_status(void *status) { xiaotai_call_status(status); }
static void wechat_status(void *status) { xiaotai_wechat_status(status); }

static bool wechat_ready(void)
{
    for (unsigned i = 0; i < 1000U; ++i) {
        xiaotai_wechat_status_t status;
        product_on_owner(wechat_status, &status);
        if (status.ready) return true;
        xiaotai_delay_ms(5U);
    }
    return false;
}

static void accept(void *ctx)
{
    const uint32_t session = *(const uint32_t *)ctx;
    const xiaotai_err_t result = scene == DEVICE ? xiaotai_call_accept(session) : xiaotai_wechat_accept(session);
    assert(result == XIAOTAI_OK);
}

static uint32_t wait_call(bool active)
{
    for (unsigned i = 0; i < 1000U; ++i) {
        if (scene == DEVICE) {
            xiaotai_call_status_t call;
            product_on_owner(device_status, &call);
            if (call.state == (active ? XIAOTAI_CALL_ACTIVE : XIAOTAI_CALL_INCOMING))
                return active ? call.link : call.session;
        } else {
            xiaotai_wechat_status_t call;
            product_on_owner(wechat_status, &call);
            if (call.state == (active ? XIAOTAI_WECHAT_ACTIVE : XIAOTAI_WECHAT_INCOMING) && call.ready)
                return active ? call.link : call.session;
        }
        xiaotai_delay_ms(5U);
    }
    fprintf(stderr, "%s: %s call did not reach %s\n", XIAOTAI_PRODUCT_NAME, scene_name,
            active ? "active" : "incoming");
    return 0;
}

static uint32_t open_call(void)
{
    xiaotai_transport_host_connect_hook(connected_request, NULL);
    if (scene == DEVICE) {
        product_platform_message("cmd", "{\"type\":\"call_incoming\",\"payload\":{"
            "\"room_id\":\"d_room\",\"caller_id\":\"peer\"}}");
    } else {
        /* MQTT can become online before the profile makes WeChat ready. */
        assert(wechat_ready());
        product_platform_message("cmd", "{\"channel\":\"wx\",\"type\":\"call_incoming\",\"payload\":{"
            "\"wx_room_id\":\"fixture-wx-room\",\"wx_room_type\":\"voice\",\"wx_from\":\"fixture-phone\","
            "\"peer_id\":\"fixture-wx-peer\",\"token\":\"fixture-wx-token\"}}");
    }
    uint32_t session = wait_call(false);
    assert(session);
    product_on_owner(accept, &session);
    for (unsigned i = 0; i < 1000U && !atomic_load(&request_id); ++i) xiaotai_delay_ms(5U);
    const uint32_t request = atomic_load(&request_id);
    assert(request);
    xiaotai_transport_host_connected(request, peer, XIAOTAI_LINK_END_PEER, 0);
    if (scene == WECHAT) {
        assert(xiaotai_transport_host_settle(5000));
        xiaotai_transport_host_command(peer, 0, 0x2000U, NULL, 0);
    }
    const uint32_t link = wait_call(true);
    assert(link);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    return link;
}

static uint32_t open_stream(void)
{
    xiaotai_stream_status_t stream;
    unsigned tries = 0;
    do {
        xiaotai_stream_status(&stream);
        if (stream.phase == XIAOTAI_STREAM_LISTENING) break;
        xiaotai_delay_ms(5U);
    } while (++tries < 1000U);
    assert(stream.phase == XIAOTAI_STREAM_LISTENING);
    xiaotai_transport_host_accepted(peer);
    tries = 0;
    do {
        xiaotai_stream_status(&stream);
        if (stream.phase == XIAOTAI_STREAM_ACTIVE) break;
        xiaotai_delay_ms(5U);
    } while (++tries < 1000U);
    assert(stream.phase == XIAOTAI_STREAM_ACTIVE && stream.generation && !stream.peer_audio);
    assert(xiaotai_link_send_audio(stream.generation, 20, tx_payload, PCMA_BYTES) == XIAOTAI_ERR_INVALID_STATE);
    assert(!tx_count);
    assert(xiaotai_transport_host_peer_subscribe(peer, 0, XIAOTAI_LINK_AUDIO, TX_STREAM, true) == XIAOTAI_OK);
    assert(xiaotai_transport_host_settle(5000));
    return stream.generation;
}

static xiaotai_link_media_stats_t receive_counts(uint32_t link)
{
    xiaotai_link_stats_t stats;
    assert(xiaotai_link_stats(link, &stats));
    return stats.audio;
}

static void check_wire(uint32_t link)
{
    xiaotai_media_status_t media;
    for (unsigned i = 0; i < 1000U; ++i) {
        xiaotai_media_status(&media);
        if (media.active && media.generation == link) break;
        xiaotai_delay_ms(5U);
    }
    assert(media.active && media.generation == link);
    if (scene == DEVICE) assert(subscribe_on == 1U && !subscribe_off);
    assert(xiaotai_link_send_audio(link, 20, tx_payload, 159) == XIAOTAI_ERR_INVALID_SIZE);
    assert(xiaotai_link_send_audio(link, 20, tx_payload, 161) == XIAOTAI_ERR_INVALID_SIZE && !tx_count);
    for (unsigned pts = 20; pts <= 40; pts += 20) {
        xiaotai_err_t result = XIAOTAI_ERR_BUSY;
        for (unsigned i = 0; i < 20U && result == XIAOTAI_ERR_BUSY; ++i) {
            result = xiaotai_link_send_audio(link, pts, tx_payload, PCMA_BYTES);
            if (result == XIAOTAI_ERR_BUSY) xiaotai_delay_ms(1);
        }
        assert(result == XIAOTAI_OK);
    }
    assert(tx_count == 2U);

    static const uint8_t pcma[PCMA_BYTES];
    uint8_t opus[PCMA_BYTES];
    size_t bytes = host_opus_packet(320, opus, sizeof(opus));
    assert(opus_packet_pad(opus, (int32_t)bytes, sizeof(opus)) == 0);
    xiaotai_link_frame_t frame = {.kind = XIAOTAI_LINK_AUDIO, .stream = scene == H5 ? H5_RX_STREAM : TX_STREAM,
        .timestamp_ms = 20, .format = {XIAOTAI_LINK_CODEC_G711A, 8000U, 1U}};
    const xiaotai_link_media_stats_t before = receive_counts(link);
    xiaotai_transport_host_frame(peer + 1U, &frame, pcma, sizeof(pcma));
    frame.format.rate_hz = 16000U;
    xiaotai_transport_host_frame(peer, &frame, pcma, sizeof(pcma));
    frame.format.rate_hz = 8000U;
    xiaotai_transport_host_frame(peer, &frame, pcma, sizeof(pcma));
    frame.timestamp_ms += 20U;
    frame.format = (xiaotai_link_format_t){XIAOTAI_LINK_CODEC_OPUS, 16000U, 1U};
    xiaotai_transport_host_frame(peer, &frame, opus, sizeof(opus));
    const xiaotai_link_media_stats_t primary = receive_counts(link);
    assert(primary.received == before.received + 2U && primary.receive_filtered == before.receive_filtered + 1U);
    assert(primary.receive_refused == before.receive_refused);
    printf("%s %s nominal rx=%u PCMA8+Opus16 and TX=%u 160B stream=10 PTS=20/40\n",
           XIAOTAI_PRODUCT_NAME, scene_name, primary.received - before.received, tx_count);

    frame.stream = OTHER_RX_STREAM;
    frame.timestamp_ms += 20U;
    xiaotai_transport_host_frame(peer, &frame, opus, sizeof(opus));
    frame.timestamp_ms += 20U;
    frame.format = (xiaotai_link_format_t){XIAOTAI_LINK_CODEC_G711A, 8000U, 1U};
    xiaotai_transport_host_frame(peer, &frame, pcma, sizeof(pcma));
    const xiaotai_link_media_stats_t alternate = receive_counts(link);
    const uint32_t accepted = alternate.received - primary.received;
    const uint32_t filtered = alternate.receive_filtered - primary.receive_filtered;
    assert(alternate.receive_refused == primary.receive_refused && accepted + filtered == 2U);
    printf("%s %s stream=7 received=%u filtered=%u (old expected=%u)\n",
           XIAOTAI_PRODUCT_NAME, scene_name, accepted, filtered, scene == H5 ? 0U : 2U);
    fflush(stdout);
    assert(accepted == (scene == H5 ? 0U : 2U));

    if (scene == DEVICE) {
        /* The peer may number its audio independently of our subscription. */
        frame.stream = 13U;
        frame.timestamp_ms += 20U;
        xiaotai_transport_host_frame(peer, &frame, pcma, sizeof(pcma));
        assert(receive_counts(link).received == alternate.received + 1U);
        xiaotai_link_receive_stop(link, XIAOTAI_LINK_AUDIO);
        assert(subscribe_on == 1U && subscribe_off == 1U);
        xiaotai_transport_host_frame(peer, &frame, pcma, sizeof(pcma));
        const xiaotai_link_media_stats_t closed = receive_counts(link);
        assert(closed.received == alternate.received + 1U && closed.receive_closed == alternate.receive_closed + 1U);
        puts("device receive: peer streams 7/13 accepted, subscription and TX stayed on 10; stop closed receive");
    }
}

xiaotai_err_t __wrap_xiaotai_start(const xiaotai_config_t *config)
{
    scene_name = getenv("XIAOTAI_HOST_PCMA_ACTIVE_SCENE");
    if (!scene_name) return __real_xiaotai_start(config);
    if (!strcmp(scene_name, "h5")) scene = H5;
    else if (!strcmp(scene_name, "device")) scene = DEVICE;
    else { assert(!strcmp(scene_name, "wechat")); scene = WECHAT; }
    product_platform_prepare(true);
    if (scene == DEVICE) product_platform_http_responder(call_http, NULL);
    const xiaotai_err_t result = __real_xiaotai_start(config);
    if (result != XIAOTAI_OK) return result;
    product_platform_online();
#ifdef XIAOTAI_TEST_OPUS_SCENES
    if (getenv("XIAOTAI_HOST_OPUS_SCENE")) {
        const uint32_t link = scene == H5 ? open_stream() : open_call();
        product_opus_legacy_scene(peer, link, scene_name, scene == H5 ? H5_RX_STREAM : TX_STREAM, scene != H5);
    } else
#endif
    check_wire(scene == H5 ? open_stream() : open_call());
    xiaotai_transport_host_closed(peer);
    assert(xiaotai_transport_host_settle(5000));
    return result;
}
