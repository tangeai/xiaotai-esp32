// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_connection_lifecycle
// Replaces legacy: tests/regression/test_voice_reliability.py::test_finish_pending_connect
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "product_ai.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/modules/room.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/port/os.h"
#include "xiaotai_transport_host.h"

#define WAIT_MS 5000U

typedef struct {
    xiaotai_ai_talk_status_t ai;
    xiaotai_stream_status_t stream;
    xiaotai_stream_stats_t stream_stats;
    xiaotai_media_status_t media;
    xiaotai_transport_status_t transport;
    uint32_t activity, activity_generation;
} observed_t;

static uintptr_t ai_peer;

static void read_owner(void *ctx)
{
    observed_t *out = ctx;
    xiaotai_ai_talk_status(&out->ai);
    xiaotai_stream_status(&out->stream);
    xiaotai_stream_stats(&out->stream_stats);
    xiaotai_media_status(&out->media);
    xiaotai_transport_status(&out->transport);
    out->activity = xiaotai_activity_current(NULL);
    out->activity_generation = out->activity ? xiaotai_activity_generation(out->activity) : 0;
}

static observed_t view(void)
{
    observed_t out;
    product_on_owner(read_owner, &out);
    return out;
}

static void settle(void)
{
    assert(xiaotai_transport_host_settle(WAIT_MS));
    product_on_owner(NULL, NULL);
}

static uint32_t calls(xiaotai_transport_host_call_kind_t kind)
{
    return xiaotai_transport_host_count(kind);
}

static unsigned peer_calls(xiaotai_transport_host_call_kind_t kind, uintptr_t conn)
{
    xiaotai_transport_host_call_t recorded[256];
    const size_t count = xiaotai_transport_host_calls(recorded, sizeof(recorded) / sizeof(*recorded));
    unsigned matched = 0;
    for (size_t i = 0; i < count; ++i)
        matched += recorded[i].kind == kind && recorded[i].connection == conn;
    return matched;
}

static observed_t wait_stream(xiaotai_stream_phase_t phase)
{
    observed_t out;
    const int64_t deadline = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
    do {
        out = view();
        if (out.stream.phase == phase &&
            (phase != XIAOTAI_STREAM_ACTIVE || (out.media.active && out.media.generation == out.stream.generation)))
            return out;
        xiaotai_delay_ms(2);
    } while (xiaotai_time_us() < deadline);
    fprintf(stderr, "stream wanted=%u got=%u closed=%u gen=%u media=%u\n", (unsigned)phase,
            (unsigned)out.stream.phase, out.stream.closed, out.stream.generation, out.media.generation);
    assert(false);
    return out;
}

static observed_t open_stream(uintptr_t conn)
{
    (void)wait_stream(XIAOTAI_STREAM_LISTENING);
    xiaotai_transport_host_accepted(conn);
    const observed_t active = wait_stream(XIAOTAI_STREAM_ACTIVE);
    assert(active.stream.generation && active.activity_generation == active.stream.generation);
    assert(peer_calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE, conn) == 1U);
    return active;
}

static uint32_t pending(uint32_t *session)
{
    product_platform_online();
    product_ai_prepare();
    *session = product_ai_request();
    const uint32_t request = product_ai_pending_request();
    const observed_t current = view();
    assert(current.ai.phase == XIAOTAI_AI_TALK_CONNECTING && current.ai.session == *session);
    assert(current.ai.link == request && current.activity && !current.activity_generation &&
           !current.media.active && !current.transport.connect_reserve_held);
    return request;
}

static void retired(uint32_t request, uintptr_t late)
{
    xiaotai_transport_host_connected(request, late, XIAOTAI_LINK_END_PEER, 0);
    settle();
    assert(peer_calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT, late) == 1);
    assert(!peer_calls(XIAOTAI_TRANSPORT_HOST_COMMAND, late));
    xiaotai_transport_host_closed(late);
    settle();
}

static void duplicate_open(uintptr_t peer)
{
    ai_peer = peer;
    const observed_t armed = view();
    assert(armed.ai.phase == XIAOTAI_AI_TALK_CONNECTING && armed.ai.link);
    assert(armed.media.generation == armed.ai.link && !armed.media.active && !armed.activity_generation);
    const uint32_t commands = calls(XIAOTAI_TRANSPORT_HOST_COMMAND);
    const uint32_t released = calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
    xiaotai_transport_host_connected(armed.ai.link, peer, XIAOTAI_LINK_END_PEER, 0);
    settle();
    const observed_t after = view();
    assert(after.ai.link == armed.ai.link && after.ai.session == armed.ai.session &&
           after.ai.phase == armed.ai.phase && !after.media.active);
    assert(calls(XIAOTAI_TRANSPORT_HOST_COMMAND) == commands &&
           calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == released);
}

static void check_lifecycle(void)
{
    const uintptr_t old_h5 = 0x1321U, old_link = 0x1322U, current_h5 = 0x1323U, refused_h5 = 0x1324U;
    const uintptr_t stale_result = 0x1325U;
    const observed_t stream = open_stream(old_h5);
    xiaotai_transport_host_closed(old_h5);
    settle();
    assert(wait_stream(XIAOTAI_STREAM_LISTENING).stream.last_end == XIAOTAI_STREAM_END_PEER);

    uint32_t failed_session;
    const uint32_t failed = pending(&failed_session);
    xiaotai_transport_host_closed(old_h5);
    settle();
    assert(view().ai.session == failed_session && !calls(XIAOTAI_TRANSPORT_HOST_COMMAND));
    xiaotai_transport_host_connected(failed, 0, XIAOTAI_LINK_END_CONNECT_FAILED, 5);
    settle();
    const observed_t failure = view();
    assert(failure.ai.phase == XIAOTAI_AI_TALK_IDLE && failure.ai.end == XIAOTAI_AI_TALK_END_NETWORK &&
           !failure.ai.session && !failure.activity && !failure.media.generation &&
           failure.transport.connect_reserve_held);

    uint32_t cancelled_session;
    const uint32_t cancelled = pending(&cancelled_session);
    product_ai_stop(cancelled_session);
    settle();
    const observed_t stopped = view();
    assert(stopped.ai.end == XIAOTAI_AI_TALK_END_LOCAL && !stopped.ai.link && !stopped.activity &&
           stopped.transport.links_releasing == 1 && !calls(XIAOTAI_TRANSPORT_HOST_COMMAND));
    retired(cancelled, old_link);
    uint32_t session;
    const uint32_t request = pending(&session);
    const observed_t awaiting = view();
    assert(request != cancelled && session != cancelled_session && awaiting.ai.session == session &&
           awaiting.ai.link == request && !awaiting.transport.connect_reserve_held &&
           !calls(XIAOTAI_TRANSPORT_HOST_COMMAND));
    retired(failed, stale_result);
    const observed_t ignored = view();
    assert(ignored.ai.session == session && ignored.ai.link == request && !calls(XIAOTAI_TRANSPORT_HOST_COMMAND));
    printf("unrelated connect result: new link=%u, reserve held before=%u after=%u\n",
           request, (unsigned)awaiting.transport.connect_reserve_held, (unsigned)ignored.transport.connect_reserve_held);
    fflush(stdout);
    assert(ignored.transport.connect_reserve_held == awaiting.transport.connect_reserve_held);
    product_ai_connect_armed(session, duplicate_open);
    const observed_t listening = view();
    assert(listening.activity_generation == request && listening.media.active);
    xiaotai_transport_host_closed(old_h5);
    xiaotai_transport_host_error(old_link, XIAOTAI_LINK_END_NETWORK, 5);
    settle();
    const observed_t still = view();
    assert(still.ai.session == session && still.ai.link == request && still.media.generation == request &&
           still.media.active && calls(XIAOTAI_TRANSPORT_HOST_COMMAND) == 1U);
    product_ai_stop(session);
    settle();
    xiaotai_transport_host_closed(ai_peer);
    settle();

    const observed_t next = open_stream(current_h5);
    assert(next.stream.generation != stream.stream.generation);
    xiaotai_transport_host_closed(old_h5);
    settle();
    assert(view().stream.generation == next.stream.generation && view().media.active);
    xiaotai_transport_host_closed(current_h5);
    settle();
    assert(wait_stream(XIAOTAI_STREAM_LISTENING).stream.last_end == XIAOTAI_STREAM_END_PEER);
    const observed_t before = view();
    xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE, XIAOTAI_FAIL, -7, 1);
    xiaotai_transport_host_accepted(refused_h5);
    settle();
    const observed_t refused = view();
    assert(refused.stream.phase == XIAOTAI_STREAM_LISTENING && !refused.stream.generation &&
           !refused.media.generation && refused.stream_stats.ended_link == before.stream_stats.ended_link + 1U);
    assert(peer_calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE, refused_h5) == 1 &&
           peer_calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT, refused_h5) == 1);
    xiaotai_transport_host_closed(refused_h5);
    settle();
    puts("stale H5/AI completion ignored; current failure retires; one RPC before media activation; H5 subscribe failure unwinds");
}

typedef struct { uint32_t ticket; } maintenance_t;

static void maintenance_ready(void *ctx)
{
    *(bool *)ctx = xiaotai_room_settled() && xiaotai_recognition_maintenance_check() == XIAOTAI_OK;
}

static void wait_maintenance_guards(void)
{
    const int64_t deadline = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
    bool ready;
    do {
        product_on_owner(maintenance_ready, &ready);
        if (ready) return;
        xiaotai_delay_ms(2);
    } while (xiaotai_time_us() < deadline);
    assert(ready);
}

static void enter_maintenance(void *ctx)
{
    maintenance_t *state = ctx;
    const xiaotai_err_t result = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &state->ticket);
    if (result != XIAOTAI_OK) fprintf(stderr, "maintenance guard refused: %d\n", result);
    assert(result == XIAOTAI_OK && state->ticket);
}

static void leave_maintenance(void *ctx)
{
    xiaotai_quiesce_end(((const maintenance_t *)ctx)->ticket);
}

static void check_finish_pending(void)
{
    const uintptr_t refused = 0x1351U, late = 0x1352U, viewer = 0x1353U;
    (void)wait_stream(XIAOTAI_STREAM_LISTENING);
    /* Settle boot-time work first. This injection tests only a pending WHIP,
     * not the environment guards' separate refusal of room/recognition work. */
    wait_maintenance_guards();
    uint32_t session;
    const uint32_t request = pending(&session);
    maintenance_t maintenance = {0};
    product_on_owner(enter_maintenance, &maintenance);
    settle();
    const observed_t held = wait_stream(XIAOTAI_STREAM_CLOSED);
    assert((held.stream.closed & XIAOTAI_STREAM_CLOSED_QUIESCED) && !held.ai.link &&
           held.ai.end == XIAOTAI_AI_TALK_END_SYSTEM && !held.activity && !held.media.generation &&
           held.transport.links_releasing == 1U && !calls(XIAOTAI_TRANSPORT_HOST_COMMAND));
    xiaotai_transport_host_accepted(refused);
    settle();
    assert(!view().stream.generation && peer_calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT, refused) == 1);
    xiaotai_transport_host_closed(refused);
    retired(request, late);
    assert(wait_stream(XIAOTAI_STREAM_CLOSED).transport.links_releasing == 0U);
    product_on_owner(leave_maintenance, &maintenance);
    (void)open_stream(viewer);
    xiaotai_transport_host_closed(viewer);
    settle();
    assert(wait_stream(XIAOTAI_STREAM_LISTENING).stream.last_end == XIAOTAI_STREAM_END_PEER);
    puts("pending AI retires without a handle; maintenance blocks H5 despite late completion and releases to a fresh viewer");
}

void product_ai_stream_lifecycle(bool maintenance_stop)
{
    product_platform_online();
    if (maintenance_stop) check_finish_pending();
    else check_lifecycle();
    xiaotai_transport_host_command_hook(NULL, NULL);
    xiaotai_transport_host_connect_hook(NULL, NULL);
}
