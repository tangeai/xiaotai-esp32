// SPDX-License-Identifier: MIT
#include "product_ai.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/json.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/os.h"
#include "xiaotai_transport_host.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AI_WORD 0x2100U
#define AI_PEER_CONN ((uintptr_t)0xb1a1U)
#define WAIT_US INT64_C(5000000)
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s AI peer: %s:%d: %s\n", XIAOTAI_PRODUCT_NAME, __FILE__, __LINE__, #condition); abort(); \
} } while (0)

static atomic_uint request_id;
static atomic_size_t received_size;
static char received[512];

void product_ai_prepare_retirement_backend(void)
{
    const xiaotai_transport_host_caps_t caps = {.https = true, .cancel_pending = true,
        .media_transport = true, .stack_log = true, .video = true, .published_retirement_barrier = true};
    xiaotai_transport_host_set_caps(&caps);
}

static void connected_request(void *ctx, xiaotai_link_kind_t kind, uint32_t request,
                              const char *peer, const char *token)
{
    (void)ctx;
    CHECK(kind == XIAOTAI_LINK_WHIP && request && !strcmp(peer, "fixture-ai-peer"));
    CHECK(!strcmp(token, "fixture-ai-token"));
    CHECK(!atomic_exchange(&request_id, request));
}

static void peer_command(void *ctx, uintptr_t conn, uint32_t word, const void *data, size_t size)
{
    (void)ctx;
    CHECK(conn == AI_PEER_CONN && xiaotai_link_command_is(word, AI_WORD));
    CHECK(data && size && size < sizeof(received) && !atomic_load(&received_size));
    memcpy(received, data, size);
    atomic_store(&received_size, size);
}

void product_ai_prepare(void)
{
    atomic_store(&request_id, 0U);
    atomic_store(&received_size, 0U);
    xiaotai_transport_host_connect_hook(connected_request, NULL);
    xiaotai_transport_host_command_hook(peer_command, NULL);
}

static void start(void *ctx)
{
    uint32_t *session = ctx;
    const xiaotai_err_t result = xiaotai_ai_talk_start(NULL, session);
    if (result != XIAOTAI_OK) fprintf(stderr, "AI start rejected: %d\n", (int)result);
    CHECK(result == XIAOTAI_OK && *session);
}

uint32_t product_ai_request(void)
{
    uint32_t session = 0;
    product_on_owner(start, &session);
    return session;
}

static void read_status(void *ctx) { xiaotai_ai_talk_status(ctx); }

uint32_t product_ai_link(uint32_t session)
{
    xiaotai_ai_talk_status_t talk;
    product_on_owner(read_status, &talk);
    CHECK(talk.session == session && talk.link);
    return talk.link;
}

void product_ai_wait_disconnect(uint32_t previous)
{
    const int64_t until = xiaotai_time_us() + WAIT_US;
    while (xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == previous &&
           xiaotai_time_us() < until) xiaotai_delay_ms(5);
    CHECK(xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == previous + 1U);
}

static void answer(size_t size)
{
    xiaotai_json_token_t tokens[48];
    xiaotai_json_doc_t doc;
    size_t count;
    CHECK(xiaotai_json_parse(received, size, tokens, sizeof(tokens) / sizeof(*tokens), &count, &doc) == XIAOTAI_OK);
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, 0, "method"), "start_session"));
    const int params = xiaotai_json_get(&doc, 0, "params");
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, params, "device_id"), PRODUCT_FIXTURE_DEVICE));
    CHECK(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, params, "role_id"), "fixture-role"));
    const char *const directions[] = {"input_audio", "output_audio"};
    for (size_t i = 0; i < sizeof(directions) / sizeof(*directions); ++i) {
        const int audio = xiaotai_json_get(&doc, params, directions[i]);
        int64_t rate = 0, channels = 0;
        CHECK(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, audio, "codec"), "g711a"));
        CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, audio, "sample_rate"), &rate) == XIAOTAI_OK);
        CHECK(xiaotai_json_int(&doc, xiaotai_json_get(&doc, audio, "channels"), &channels) == XIAOTAI_OK);
        CHECK(rate == 8000 && channels == 1);
    }
    char id[17];
    CHECK(xiaotai_json_string(&doc, xiaotai_json_get(&doc, 0, "id"), id, sizeof(id), &count) == XIAOTAI_OK);
    CHECK(count == 16U && strspn(id, "0123456789abcdef") == count);
    char reply[288];
    const int written = snprintf(reply, sizeof(reply),
        "{\"id\":\"%s\",\"result\":{\"session_id\":\"fixture-session\","
        "\"input_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1},"
        "\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}}}", id);
    CHECK(written > 0 && (size_t)written < sizeof(reply));
    xiaotai_transport_host_command(AI_PEER_CONN, 0, AI_WORD, (const uint8_t *)reply, (size_t)written);
}

uint32_t product_ai_pending_request(void)
{
    const int64_t until = xiaotai_time_us() + WAIT_US;
    while (!atomic_load(&request_id) && xiaotai_time_us() < until) xiaotai_delay_ms(2);
    const uint32_t request = atomic_load(&request_id);
    CHECK(request != 0U);
    return request;
}

void product_ai_connect_armed(uint32_t session, void (*observe)(uintptr_t peer))
{
    const uint32_t request = product_ai_pending_request();
    xiaotai_transport_host_connected(request, AI_PEER_CONN, XIAOTAI_LINK_END_PEER, 0);

    int64_t until = xiaotai_time_us() + WAIT_US;
    while (!atomic_load(&received_size) && xiaotai_time_us() < until) xiaotai_delay_ms(2);
    const size_t size = atomic_load(&received_size);
    CHECK(size != 0U);
    xiaotai_transport_host_command_hook(NULL, NULL);
    xiaotai_transport_host_connect_hook(NULL, NULL);
    if (observe) observe(AI_PEER_CONN);
    answer(size);

    xiaotai_ai_talk_status_t talk;
    xiaotai_media_status_t media;
    until = xiaotai_time_us() + WAIT_US;
    do {
        product_on_owner(read_status, &talk);
        xiaotai_media_status(&media);
        if (talk.session == session && talk.phase == XIAOTAI_AI_TALK_LISTENING &&
            talk.link && media.generation == talk.link && media.active) {
            printf("%s: AI listening; media generation=%u active\n", XIAOTAI_PRODUCT_NAME, (unsigned)media.generation);
            return;
        }
        if (!talk.session) break;
        xiaotai_delay_ms(2);
    } while (xiaotai_time_us() < until);
    fprintf(stderr, "AI session=%u phase=%u link=%u media=%u active=%u\n", (unsigned)talk.session,
            (unsigned)talk.phase, (unsigned)talk.link, (unsigned)media.generation, (unsigned)media.active);
    CHECK(false);
}

void product_ai_connect(uint32_t session) { product_ai_connect_armed(session, NULL); }

void product_ai_peer_message(const char *json)
{
    xiaotai_transport_host_command(AI_PEER_CONN, 0, AI_WORD, (const uint8_t *)json, strlen(json));
}

static void stop(void *ctx)
{
    const uint32_t session = *(const uint32_t *)ctx;
    CHECK(xiaotai_ai_talk_stop(session) == XIAOTAI_OK);
}

void product_ai_stop(uint32_t session)
{
    product_on_owner(stop, &session);
    const int64_t until = xiaotai_time_us() + WAIT_US;
    do {
        xiaotai_ai_talk_status_t talk;
        xiaotai_media_status_t media;
        product_on_owner(read_status, &talk);
        xiaotai_media_status(&media);
        if (talk.phase == XIAOTAI_AI_TALK_IDLE && !talk.session && !media.active) return;
        xiaotai_delay_ms(2);
    } while (xiaotai_time_us() < until);
    CHECK(false);
}
