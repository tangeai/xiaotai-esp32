// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_rtc/test_rtc.c::rtc
#include "support.h"

typedef struct {
    char order[12];
    size_t count;
    uint32_t generation, word;
    xiaotai_link_end_t reason;
    int detail;
    bool dropped;
    size_t size;
    uint8_t byte;
} observation_t;

typedef struct {
    observation_t *observation;
    uint32_t timeout;
    xiaotai_link_kind_t kind;
    bool track_peer_retirement;
    xiaotai_err_t result;
} connect_t;

static void remember(observation_t *observation, uint32_t generation, char which)
{
    CHECK(observation->generation == generation && observation->count < sizeof(observation->order));
    observation->order[observation->count++] = which;
}

static void opened(void *ctx, uint32_t generation) { remember(ctx, generation, 'O'); }

static void closed(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    observation_t *observation = ctx;
    remember(observation, generation, 'X');
    observation->reason = reason;
    observation->detail = detail;
}

static void command(void *ctx, uint32_t generation, const xiaotai_link_command_t *msg)
{
    observation_t *observation = ctx;
    remember(observation, generation, 'C');
    observation->word = msg->word;
    observation->dropped = msg->dropped;
    observation->size = msg->size;
    if (msg->data) {
        observation->byte = msg->data[0];
        CHECK(msg->data[msg->size] == 0);
    }
}

static void subscription(void *ctx, uint32_t generation, xiaotai_link_media_kind_t kind, uint8_t stream,
                         bool subscribed)
{
    CHECK(kind == XIAOTAI_LINK_AUDIO && stream == 1 && subscribed);
    remember(ctx, generation, 'S');
}

static void owner_connect(void *ctx)
{
    connect_t *call = ctx;
    const xiaotai_link_request_t request = {
        .kind = call->kind, .peer = "http://peer.test", .token = "copied-token", .timeout_ms = call->timeout,
        .profile = {.audio = {.send = true, .send_stream = 1, .send_format = {XIAOTAI_LINK_CODEC_G711A, 8000, 1}},
                    .command_max_bytes = 24, .command_pool_bytes = 48,
                    .track_peer_retirement = call->track_peer_retirement},
    };
    const xiaotai_link_handlers_t handlers = {
        .opened = opened, .closed = closed, .command = command, .peer_subscribed = subscription,
        .ctx = call->observation,
    };
    call->result = xiaotai_link_connect(&request, &handlers, &call->observation->generation);
}

static void connect_kind(observation_t *observation, uint32_t timeout, xiaotai_link_kind_t kind, bool track)
{
    memset(observation, 0, sizeof(*observation));
    connect_t call = {.observation = observation, .timeout = timeout, .kind = kind, .track_peer_retirement = track,
                      .result = XIAOTAI_FAIL};
    CHECK(xiaotai_run(owner_connect, &call) == XIAOTAI_OK);
    settle();
    CHECK(call.result == XIAOTAI_OK && observation->generation);
}

static void connect(observation_t *observation, uint32_t timeout)
{
    connect_kind(observation, timeout, XIAOTAI_LINK_WHIP, false);
}

static void owner_disconnect(void *ctx)
{
    xiaotai_link_disconnect(((observation_t *)ctx)->generation);
}

static void disconnect(observation_t *observation)
{
    CHECK(xiaotai_run(owner_disconnect, observation) == XIAOTAI_OK);
    settle();
}

static void answer(const observation_t *observation, uintptr_t conn)
{
    xiaotai_transport_host_connected(observation->generation, conn, XIAOTAI_LINK_END_CONNECT_FAILED, conn ? 0 : -41);
    settle();
}

static void test_late_result_and_timeout(void)
{
    boot_online(NULL);
    observation_t old, next;
    connect(&old, 0);
    CHECK(status_now().links_requested == 1 && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1);
    disconnect(&old);
    CHECK(status_now().links_releasing == 1 && !old.count && status_now().connect_ready);
    connect(&next, 100);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1);
    answer(&old, 101);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1 && status_now().late_links_closed == 1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1 && !old.count);
    xiaotai_transport_host_closed(101);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 2);
    advance(99);
    CHECK(!next.count);
    advance(1);
    CHECK(strcmp(next.order, "X") == 0 && next.reason == XIAOTAI_LINK_END_TIMEOUT);
    advance(34999);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    advance(1);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_FAULT && status_now().release_stalls == 1);
    CHECK(!old.count && strcmp(next.order, "X") == 0);
    shutdown_all();
}

static void test_cancellable_release(void)
{
    boot_online(NULL);
    xiaotai_transport_host_set_caps(&(xiaotai_transport_host_caps_t){.cancel_pending = true});
    xiaotai_transport_host_pending(1);
    observation_t cancelled;
    connect(&cancelled, 0);
    disconnect(&cancelled);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CANCEL) == 1 && calls(XIAOTAI_TRANSPORT_HOST_PENDING) == 0);
    advance(19);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_PENDING) == 0);
    advance(1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_PENDING) == 1 && status_now().links_releasing == 1);
    xiaotai_transport_host_pending(0);
    advance(20);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_PENDING) == 2 && status_now().links_releasing == 0);
    CHECK(!cancelled.count);
    shutdown_all();
}

static void test_release_tolerated(void)
{
    const xiaotai_transport_config_t config = {.release_failures_tolerated = true, .release_timeout_ms = 1000};
    boot_online(&config);
    observation_t old, current, failed;
    connect(&old, 0);
    disconnect(&old);
    connect(&current, 0);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 2);
    xiaotai_transport_host_fail_disconnect(XIAOTAI_FAIL, -53, 1);
    answer(&old, 201);
    CHECK(status_now().release_stalls == 1 && xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    CHECK(status_now().links_releasing == 0 && !old.count);
    answer(&current, 202);
    CHECK(strcmp(current.order, "O") == 0);
    disconnect(&current);
    CHECK(status_now().links_releasing == 1);
    advance(1000);
    CHECK(status_now().release_stalls == 2 && status_now().links_releasing == 0);
    xiaotai_transport_host_fail_connect(XIAOTAI_ERR_NO_MEM, -54, 1);
    connect(&failed, 0);
    CHECK(strcmp(failed.order, "X") == 0 && failed.reason == XIAOTAI_LINK_END_NO_MEMORY && failed.detail == -54);
    shutdown_all();
}

static void test_early_event_order(void)
{
    const xiaotai_transport_config_t config = {.links = 2, .event_slots = 4, .command_pool_bytes = 64};
    boot_online(&config);
    observation_t first, left, right;
    connect(&first, 0);
    uint8_t bytes[25] = {0x42};
    xiaotai_rtc_on_command(91, 0, 0x1234, bytes, 2); /* One pending request, no provided ID. */
    bytes[0] = 0;
    CHECK(xiaotai_rtc_on_subscribe(91, first.generation, XIAOTAI_LINK_AUDIO, 1, true) == XIAOTAI_OK);
    settle();
    CHECK(!first.count);
    answer(&first, 91);
    CHECK(strcmp(first.order, "OCS") == 0 && first.byte == 0x42 && first.word == 0x1234 && !first.dropped);
    xiaotai_rtc_on_command(91, 0, 0xf0, bytes, sizeof(bytes));
    settle();
    CHECK(strcmp(first.order, "OCSC") == 0 && first.size == sizeof(bytes) && first.dropped);
    xiaotai_transport_host_closed(91);
    settle();
    CHECK(strcmp(first.order, "OCSCX") == 0);

    connect(&left, 0);
    connect(&right, 0);
    const uint32_t dropped = status_now().early_dropped;
    xiaotai_rtc_on_command(92, 0, 0xff, bytes, 1); /* No ID with two waiting is ambiguous. */
    xiaotai_rtc_on_command(93, right.generation, 0xa1, bytes, 1);
    settle();
    CHECK(status_now().early_dropped == dropped + 1 && !left.count && !right.count);
    answer(&right, 0);
    CHECK(strcmp(right.order, "X") == 0 && right.detail == -41);
    CHECK(status_now().early_dropped == dropped + 2);
    answer(&left, 92);
    CHECK(strcmp(left.order, "O") == 0);
    shutdown_all();
}

static xiaotai_signal_t owner_entered, owner_resume;
static void block_owner(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&owner_entered);
    CHECK(xiaotai_signal_take(&owner_resume, 3000));
}

typedef struct {
    uint32_t generation;
    bool opened;
    xiaotai_err_t result;
} finish_t;

static void finish_owner(void *ctx)
{
    finish_t *call = ctx;
    call->result = xiaotai_link_finish_opening(call->generation, &call->opened, true,
                                               0x2001, NULL, 0);
}

static void queue_finish(finish_t *call, const observation_t *observation)
{
    *call = (finish_t){.generation = observation->generation};
    CHECK(xiaotai_run(block_owner, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&owner_entered, 3000));
    CHECK(xiaotai_run(finish_owner, call) == XIAOTAI_OK);
}

static void close_during_final_command(void *ctx, xiaotai_transport_host_call_kind_t kind,
                                       uintptr_t conn, int64_t word)
{
    CHECK(kind == XIAOTAI_TRANSPORT_HOST_COMMAND && word == 0x2001);
    xiaotai_transport_host_closed(conn);
    const uint32_t generation = ((const observation_t *)ctx)->generation;
    unsigned pins = 0;
    xiaotai_critical_enter(&xt.lock);
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        const xt_release_t *release = &xt.releases[i];
        if (release->used && release->closed && release->generation == generation) pins += release->uses;
    }
    xiaotai_critical_exit(&xt.lock);
    CHECK(pins == 1);
}

static void test_finish_opening(void)
{
    boot_online(NULL);
    CHECK(xiaotai_signal_init(&owner_entered) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&owner_resume) == XIAOTAI_OK);
    observation_t link;
    finish_t call;

    connect(&link, 0);
    queue_finish(&call, &link);
    xiaotai_transport_host_connected(link.generation, 91, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    CHECK(status_now().links_open == 1 && !link.count);
    CHECK(xiaotai_link_command(link.generation, 0x2001, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_rtc_on_command(91, 0, 0x2000, NULL, 0);
    xiaotai_signal_give(&owner_resume);
    settle();
    CHECK(call.result == XIAOTAI_OK && call.opened && !link.count);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_COMMAND) == 1 && !calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT));
    CHECK(xiaotai_link_command(link.generation, 0x2001, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_rtc_on_command(91, 0, 0x2000, NULL, 0);
    CHECK(xiaotai_run(finish_owner, &call) == XIAOTAI_OK);
    settle();
    CHECK(call.result == XIAOTAI_ERR_INVALID_STATE && !call.opened && !link.count);
    disconnect(&link);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1);
    xiaotai_transport_host_closed(91);
    settle();

    connect(&link, 0);
    queue_finish(&call, &link);
    xiaotai_signal_give(&owner_resume);
    settle();
    CHECK(call.result == XIAOTAI_OK && !call.opened && !link.count);
    answer(&link, 92); /* The request was already withdrawn: retire the late success. */
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_COMMAND) == 1 && calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 2);
    CHECK(status_now().late_links_closed == 1 && !link.count);
    xiaotai_transport_host_closed(92);
    settle();

    connect(&link, 0);
    queue_finish(&call, &link);
    xiaotai_transport_host_connected(link.generation, 93, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    xiaotai_transport_host_closed(93);
    xiaotai_signal_give(&owner_resume);
    settle();
    CHECK(call.result == XIAOTAI_ERR_INVALID_STATE && call.opened && !link.count);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_COMMAND) == 1 && calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 2);

    connect(&link, 0);
    queue_finish(&call, &link);
    xiaotai_transport_host_connected(link.generation, 0, XIAOTAI_LINK_END_CONNECT_FAILED, -41);
    xiaotai_signal_give(&owner_resume);
    settle();
    CHECK(call.result == XIAOTAI_OK && !call.opened && !link.count);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_COMMAND) == 1 && calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 2);

    connect(&link, 0);
    queue_finish(&call, &link);
    xiaotai_transport_host_connected(link.generation, 94, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_COMMAND, XIAOTAI_ERR_BUSY, -42, 1);
    xiaotai_transport_host_media_hook(close_during_final_command, &link);
    xiaotai_signal_give(&owner_resume);
    settle();
    xiaotai_transport_host_media_hook(NULL, NULL);
    CHECK(call.result == XIAOTAI_ERR_BUSY && call.opened);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_COMMAND) == 2 && calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 2);
    CHECK(strcmp(link.order, "X") == 0 && link.reason == XIAOTAI_LINK_END_PEER);

    shutdown_all();
    xiaotai_signal_deinit(&owner_resume);
    xiaotai_signal_deinit(&owner_entered);
}

static void test_event_overflow(bool fault)
{
    const xiaotai_transport_config_t config = {.links = 2, .event_slots = 2, .overflow_fault = fault};
    boot_online(&config);
    observation_t first, second;
    connect(&first, 0);
    answer(&first, 31);
    connect(&second, 0);
    answer(&second, 32);
    CHECK(xiaotai_signal_init(&owner_entered) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&owner_resume) == XIAOTAI_OK);
    CHECK(xiaotai_run(block_owner, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&owner_entered, 3000));
    xiaotai_rtc_on_command(31, 0, 1, NULL, 0);
    xiaotai_rtc_on_command(31, 0, 2, NULL, 0);
    CHECK(xiaotai_rtc_on_subscribe(31, 0, XIAOTAI_LINK_AUDIO, 1, true) == XIAOTAI_ERR_BUSY);
    CHECK(status_now().links_open == 2 && status_now().events_lost == 0);
    xiaotai_rtc_on_command(31, 0, 3, NULL, 0);
    xiaotai_signal_give(&owner_resume);
    settle();
    CHECK(strcmp(first.order, "OCCX") == 0 && first.word == 2 && first.reason == XIAOTAI_LINK_END_OVERFLOW);
    CHECK(status_now().events_lost == 1);
    CHECK(strcmp(second.order, fault ? "OX" : "O") == 0);
    if (fault) CHECK(second.reason == XIAOTAI_LINK_END_STOPPED && xiaotai_transport_state() == XIAOTAI_TRANSPORT_FAULT);
    else CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    shutdown_all();
    xiaotai_signal_deinit(&owner_resume);
    xiaotai_signal_deinit(&owner_entered);
}

static void test_release_watch(void)
{
    const xiaotai_transport_config_t config = {.links = 1};
    boot_online(&config);
    const uint32_t baseline_bytes = status_now().engine_bytes;
    observation_t link;
    connect(&link, 0);
    const uint32_t first = link.generation;
    CHECK(xiaotai_link_watch_release(first) == XIAOTAI_ERR_NOT_SUPPORTED);
    xiaotai_transport_host_set_caps(&(xiaotai_transport_host_caps_t){.cancel_pending = true});
    CHECK(xiaotai_link_watch_release(first) == XIAOTAI_ERR_NOT_SUPPORTED);
    xiaotai_transport_host_set_caps(&(xiaotai_transport_host_caps_t){.cancel_pending = true, .published_retirement_barrier = true});
    CHECK(xiaotai_link_watch_release(first) == XIAOTAI_ERR_INVALID_STATE);
    answer(&link, 91);
    CHECK(xiaotai_link_watch_release(first) == XIAOTAI_OK);
    CHECK(xiaotai_link_watch_release(first) == XIAOTAI_ERR_BUSY);
    xiaotai_critical_enter(&xt.lock);
    CHECK(xt_link_pin_locked(xt_link_generation_locked(first)));
    xiaotai_critical_exit(&xt.lock);
    disconnect(&link);
    xiaotai_transport_host_pending(1);
    xiaotai_rtc_on_closed_for(91, first);
    settle();
    CHECK(xiaotai_link_release_state(first) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_link_unwatch_release(first + 1U);
    CHECK(xiaotai_link_release_state(first) == XIAOTAI_LINK_RELEASE_PENDING);
    xt_link_unpin(first);
    settle();
    CHECK(!status_now().links_releasing && status_now().engine_bytes == baseline_bytes &&
          calls(XIAOTAI_TRANSPORT_HOST_PENDING) != 0);
    CHECK(xiaotai_link_release_state(first) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_transport_host_pending(0);
    advance(20);
    CHECK(xiaotai_link_release_state(first) == XIAOTAI_LINK_RELEASE_COMPLETE);
    xiaotai_link_unwatch_release(first);

    connect(&link, 0);
    answer(&link, 91); /* The SDK may reuse its handle. */
    CHECK(link.generation != first && xiaotai_link_watch_release(link.generation) == XIAOTAI_OK);
    CHECK(xiaotai_link_release_state(first) == XIAOTAI_LINK_RELEASE_UNWATCHED);
    xiaotai_rtc_on_closed_for(91, first); /* A duplicate, not closure of the current object. */
    settle();
    CHECK(status_now().links_open == 1 && !status_now().links_releasing);
    CHECK(xiaotai_link_release_state(link.generation) == XIAOTAI_LINK_RELEASE_PENDING);
    const uint32_t second = link.generation;
    xiaotai_transport_host_pending(1);
    xiaotai_rtc_on_closed_for(91, second); /* Remote directly acknowledges, with zero pins. */
    xiaotai_rtc_on_closed_for(91, second); /* Duplicate must not repeat the release. */
    settle();
    CHECK(!status_now().links_open && !status_now().links_releasing &&
          status_now().engine_bytes == baseline_bytes && strcmp(link.order, "OX") == 0);
    CHECK(xiaotai_link_release_state(second) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_transport_host_pending(0);
    advance(20);
    CHECK(xiaotai_link_release_state(second) == XIAOTAI_LINK_RELEASE_COMPLETE);
    xiaotai_link_unwatch_release(second);

    connect(&link, 0);
    answer(&link, 92);
    CHECK(xiaotai_link_watch_release(link.generation) == XIAOTAI_OK);
    xiaotai_transport_host_closed(92); /* Legacy untagged end is not a proof. */
    advance(100);
    CHECK(xiaotai_link_release_state(link.generation) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_link_unwatch_release(link.generation);

    connect(&link, 0);
    answer(&link, 93);
    const uint32_t pinned = link.generation;
    CHECK(xiaotai_link_watch_release(pinned) == XIAOTAI_OK);
    const size_t polls = calls(XIAOTAI_TRANSPORT_HOST_PENDING);
    xiaotai_critical_enter(&xt.lock);
    CHECK(xt_link_pin_locked(xt_link_generation_locked(pinned)));
    xiaotai_critical_exit(&xt.lock);
    xiaotai_rtc_on_closed_for(93, pinned); /* Direct remote ACK with an app-side pin. */
    settle();
    CHECK(!status_now().links_open && status_now().links_releasing == 1 &&
          status_now().engine_bytes > baseline_bytes && calls(XIAOTAI_TRANSPORT_HOST_PENDING) == polls);
    CHECK(xiaotai_link_release_state(pinned) == XIAOTAI_LINK_RELEASE_PENDING);
    xt_link_unpin(pinned);
    settle();
    CHECK(xiaotai_link_release_state(pinned) == XIAOTAI_LINK_RELEASE_COMPLETE &&
          status_now().engine_bytes == baseline_bytes);
    xiaotai_link_unwatch_release(pinned);
    shutdown_all();
}

static struct {
    unsigned calls;
    uintptr_t conn;
    uint32_t generation;
    bool succeeds;
} peer_tag;

static bool tag_peer(uintptr_t conn, uint32_t generation)
{
    xiaotai_link_stats_t stats;
    /* Reading the slot here also requires the setter to run outside xt.lock. */
    CHECK(xiaotai_link_stats(generation, &stats));
    ++peer_tag.calls;
    peer_tag.conn = conn;
    peer_tag.generation = generation;
    return peer_tag.succeeds;
}

static void open_peer(observation_t *link, uintptr_t conn, bool track, int detail)
{
    connect_kind(link, 0, XIAOTAI_LINK_P2P, track);
    xiaotai_rtc_on_connected_peer(link->generation, conn, XIAOTAI_LINK_END_CONNECT_FAILED, detail, tag_peer);
    settle();
}

static void test_p2p_release_watch(void)
{
    boot_online(&(xiaotai_transport_config_t){.links = 1});
    xiaotai_transport_host_set_caps(&(xiaotai_transport_host_caps_t){.cancel_pending = true,
        .published_retirement_barrier = true});
    const uint32_t baseline = status_now().engine_bytes;
    observation_t link;
    peer_tag.succeeds = true;
    peer_tag.calls = 0;
    open_peer(&link, 91, false, 0);
    CHECK(strcmp(link.order, "O") == 0 && peer_tag.calls == 0);
    CHECK(xiaotai_link_watch_release(link.generation) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_transport_host_closed(91);
    settle();

    open_peer(&link, 91, true, -41);
    CHECK(peer_tag.calls == 0 && xiaotai_link_watch_release(link.generation) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_transport_host_closed(91);
    settle();

    peer_tag.succeeds = false;
    open_peer(&link, 91, true, 0);
    const uint32_t failed_tag = link.generation;
    CHECK(strcmp(link.order, "O") == 0 && peer_tag.calls == 1 && peer_tag.generation == failed_tag);
    CHECK(xiaotai_link_watch_release(failed_tag) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_transport_host_closed(91);
    settle();

    peer_tag.succeeds = true;
    open_peer(&link, 91, true, 0);
    const uint32_t generation = link.generation;
    CHECK(peer_tag.calls == 2 && peer_tag.conn == 91 && peer_tag.generation == generation);
    CHECK(generation != failed_tag && xiaotai_link_watch_release(generation) == XIAOTAI_OK);
    xiaotai_rtc_on_closed_for(91, failed_tag);
    settle();
    CHECK(status_now().links_open == 1 && strcmp(link.order, "O") == 0);

    disconnect(&link);
    CHECK(status_now().links_releasing == 1 && status_now().engine_bytes > baseline);
    advance(20); /* Even an SDK quiescence reading cannot replace the same-generation ACK. */
    CHECK(xiaotai_link_release_state(generation) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_transport_host_pending(1);
    xiaotai_rtc_on_closed_for(91, generation);
    settle();
    CHECK(status_now().links_releasing == 0 && status_now().engine_bytes == baseline);
    CHECK(xiaotai_link_release_state(generation) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_transport_host_pending(0);
    advance(20);
    CHECK(xiaotai_link_release_state(generation) == XIAOTAI_LINK_RELEASE_COMPLETE);
    xiaotai_link_unwatch_release(generation);
    shutdown_all();
}

static void inbound_opened(void *ctx, uint32_t generation)
{
    observation_t *link = ctx;
    CHECK(generation && !link->generation);
    link->generation = generation;
    opened(ctx, generation);
}

static void test_inbound_peer_release_watch(void)
{
    boot_online(&(xiaotai_transport_config_t){.links = 1});
    xiaotai_transport_host_set_caps(&(xiaotai_transport_host_caps_t){.cancel_pending = true,
        .published_retirement_barrier = true});
    observation_t link = {0};
    xiaotai_link_listener_t listener = {.name = "tracked-peer", .profile = {.track_peer_retirement = true},
        .handlers = {.opened = inbound_opened, .closed = closed, .ctx = &link}};
    uint32_t tracked;
    CHECK(xiaotai_link_listen(&listener, &tracked) == XIAOTAI_OK);
    listener.name = "default-peer";
    listener.profile.track_peer_retirement = false;
    uint32_t ordinary;
    CHECK(xiaotai_link_listen(&listener, &ordinary) == XIAOTAI_OK);

    peer_tag.calls = 0;
    peer_tag.succeeds = true;
    xiaotai_rtc_on_accepted_peer(91, tag_peer); /* Nobody armed this admission. */
    settle();
    CHECK(peer_tag.calls == 0 && status_now().inbound_refused == 1 && !link.generation);
    xiaotai_transport_host_closed(91);
    settle();

    CHECK(xiaotai_link_listen_set(ordinary, XIAOTAI_LINK_LISTEN_ONCE) == XIAOTAI_OK);
    xiaotai_rtc_on_accepted_peer(91, tag_peer);
    settle();
    CHECK(peer_tag.calls == 0 && strcmp(link.order, "O") == 0);
    CHECK(xiaotai_link_watch_release(link.generation) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_transport_host_closed(91);
    settle();
    link = (observation_t){0};

    peer_tag.succeeds = false;
    CHECK(xiaotai_link_listen_set(tracked, XIAOTAI_LINK_LISTEN_ONCE) == XIAOTAI_OK);
    xiaotai_rtc_on_accepted_peer(91, tag_peer);
    settle();
    const uint32_t failed_tag = link.generation;
    CHECK(peer_tag.calls == 1 && peer_tag.generation == failed_tag);
    CHECK(xiaotai_link_watch_release(failed_tag) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_transport_host_closed(91);
    settle();
    link = (observation_t){0};

    peer_tag.succeeds = true;
    CHECK(xiaotai_link_listen_set(tracked, XIAOTAI_LINK_LISTEN_ONCE) == XIAOTAI_OK);
    xiaotai_rtc_on_accepted_peer(91, tag_peer);
    settle();
    const uint32_t generation = link.generation;
    CHECK(peer_tag.calls == 2 && peer_tag.conn == 91 && peer_tag.generation == generation);
    CHECK(generation != failed_tag && xiaotai_link_watch_release(generation) == XIAOTAI_OK);
    xiaotai_rtc_on_closed_for(91, failed_tag);
    advance(20); /* No borrowed args; a live link still must not look released. */
    CHECK(xiaotai_link_release_state(generation) == XIAOTAI_LINK_RELEASE_PENDING);
    CHECK(status_now().links_open == 1 && strcmp(link.order, "O") == 0);

    xiaotai_critical_enter(&xt.lock);
    CHECK(xt_link_pin_locked(xt_link_generation_locked(generation)));
    xiaotai_critical_exit(&xt.lock);
    const uint32_t polls = calls(XIAOTAI_TRANSPORT_HOST_PENDING);
    xiaotai_transport_host_pending(1);
    xiaotai_rtc_on_closed_for(91, generation); /* Remote close; release waits for the last app owner. */
    settle();
    CHECK(status_now().links_releasing == 1 && calls(XIAOTAI_TRANSPORT_HOST_PENDING) == polls);
    CHECK(xiaotai_link_release_state(generation) == XIAOTAI_LINK_RELEASE_PENDING);
    xt_link_unpin(generation);
    settle();
    CHECK(status_now().links_releasing == 0 && calls(XIAOTAI_TRANSPORT_HOST_PENDING) > polls);
    CHECK(xiaotai_link_release_state(generation) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_transport_host_pending(0);
    advance(20);
    CHECK(strcmp(link.order, "OX") == 0 && xiaotai_link_release_state(generation) == XIAOTAI_LINK_RELEASE_COMPLETE);
    xiaotai_link_unwatch_release(generation);

    link = (observation_t){0};
    CHECK(xiaotai_link_listen_set(tracked, XIAOTAI_LINK_LISTEN_ONCE) == XIAOTAI_OK);
    xiaotai_rtc_on_accepted_peer(91, tag_peer); /* A different object may use the now-free address. */
    settle();
    const uint32_t replacement = link.generation;
    CHECK(replacement != generation && xiaotai_link_watch_release(replacement) == XIAOTAI_OK);
    xiaotai_rtc_on_closed_for(91, generation);
    settle();
    CHECK(strcmp(link.order, "O") == 0 && xiaotai_link_release_state(replacement) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_transport_host_pending(1);
    xiaotai_rtc_on_closed_for(91, replacement); /* An inbound close without app pins still needs an ACK record. */
    settle();
    CHECK(strcmp(link.order, "OX") == 0 && status_now().links_releasing == 0);
    CHECK(xiaotai_link_release_state(replacement) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_transport_host_pending(0);
    advance(20);
    CHECK(xiaotai_link_release_state(replacement) == XIAOTAI_LINK_RELEASE_COMPLETE);
    xiaotai_link_unwatch_release(replacement);
    shutdown_all();
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "events") == 0) {
        test_early_event_order();
        test_event_overflow(false);
        test_event_overflow(true);
    } else if (argc > 1 && strcmp(argv[1], "finish_opening") == 0) {
        test_finish_opening();
    } else {
        test_late_result_and_timeout();
        test_cancellable_release();
        test_release_tolerated();
        test_release_watch();
        test_p2p_release_watch();
        test_inbound_peer_release_watch();
    }
}
