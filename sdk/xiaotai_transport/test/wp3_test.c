// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_s3_audio_contract.py
// Replaces legacy: tests/xmini/xmini_rtc/test_rx_repacketize.c::rx_repacketize
// Replaces legacy: tests/regression/test_contact_query.py
// Replaces legacy: tests/native/cores3/test_h5_video_send.py
// Replaces legacy: tests/regression/test_h5_video_send.py
// Replaces legacy: tests/xmini/xmini_rtc/test_rtc.c::rtc
#include "support.h"
#include <pthread.h>
#include "internal.h"
#include "xiaotai/media_format.h"

static const xiaotai_link_format_t alaw = {XIAOTAI_LINK_CODEC_G711A, 8000, 1};
static const xiaotai_link_format_t opus = {XIAOTAI_LINK_CODEC_OPUS, 16000, 1};
static const xiaotai_link_format_t h264 = {XIAOTAI_LINK_CODEC_H264, 0, 0};
static const uint8_t audio[160] = {31};
static bool external_ram;

bool __wrap_xiaotai_mem_has_external(void) { return external_ram; }

typedef struct {
    uint32_t word;
    size_t size;
    bool dropped;
    uint8_t first, last;
} command_seen_t;

typedef struct {
    xiaotai_link_profile_t profile;
    xiaotai_err_t result;
    uint32_t generation;
    unsigned opened, closed, commands, subscriptions;
    xiaotai_link_media_kind_t last_subscription_kind;
    uint8_t last_subscription_stream;
    bool last_subscription_on;
    uintptr_t echo_conn;
    const char *expected_command_payload;
    command_seen_t last_command;
    command_seen_t initial_commands[5];
    xiaotai_link_end_t reason;
    int detail;
} observation_t;

static void open_seen(void *arg, uint32_t gen)
{
    observation_t *o = arg;
    CHECK(gen == o->generation);
    ++o->opened;
}

static void close_seen(void *arg, uint32_t gen, xiaotai_link_end_t reason, int detail)
{
    observation_t *o = arg;
    CHECK(gen == o->generation);
    ++o->closed;
    o->reason = reason;
    o->detail = detail;
}

static void command_seen(void *arg, uint32_t gen, const xiaotai_link_command_t *msg)
{
    observation_t *o = arg;
    CHECK(gen == o->generation && o->opened == 1);
    CHECK(msg->id == ((msg->word >> 1) & 0x7fff) && msg->sequence == (msg->word >> 16));
    CHECK(msg->response == (bool)(msg->word & 1U));
    CHECK((msg->size && !msg->dropped) == (msg->data != NULL));
    if (msg->data) CHECK(msg->data[msg->size] == 0);
    if (o->expected_command_payload)
        CHECK(msg->data && msg->size == strlen(o->expected_command_payload) &&
              !memcmp(msg->data, o->expected_command_payload, msg->size));
    ++o->commands;
    o->last_command = (command_seen_t){msg->word, msg->size, msg->dropped,
        msg->data ? msg->data[0] : 0, msg->data ? msg->data[msg->size - 1U] : 0};
    if (o->commands <= sizeof(o->initial_commands) / sizeof(o->initial_commands[0]))
        o->initial_commands[o->commands - 1U] = o->last_command;
    if (o->echo_conn && o->commands == 1U) {
        CHECK(msg->data);
        const uint8_t payload = 99;
        xiaotai_transport_host_command(o->echo_conn, 0, 0x2101, &payload, 1U);
        CHECK(msg->data[0] == o->last_command.first && msg->data[msg->size - 1U] == o->last_command.last);
    }
}

static void subscribed_seen(void *arg, uint32_t gen, xiaotai_link_media_kind_t kind, uint8_t stream, bool on)
{
    observation_t *o = arg;
    CHECK(gen == o->generation && o->opened == 1);
    ++o->subscriptions;
    o->last_subscription_kind = kind;
    o->last_subscription_stream = stream;
    o->last_subscription_on = on;
}

static void request_on_owner(void *arg)
{
    observation_t *o = arg;
    const xiaotai_link_request_t r = {.kind = XIAOTAI_LINK_WHIP, .peer = "http://peer.test", .token = "fixture",
        .profile = o->profile};
    const xiaotai_link_handlers_t h = {.opened = open_seen, .closed = close_seen, .command = command_seen,
        .peer_subscribed = subscribed_seen, .ctx = o};
    o->result = xiaotai_link_connect(&r, &h, &o->generation);
}

static void open_link_with(observation_t *o, uintptr_t conn)
{
    CHECK(xiaotai_run(request_on_owner, o) == XIAOTAI_OK);
    settle();
    CHECK(o->result == XIAOTAI_OK && o->generation);
    xiaotai_transport_host_connected(o->generation, conn, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    settle();
    CHECK(o->opened == 1);
}

static void disconnect_on_owner(void *arg)
{
    xiaotai_link_disconnect(((observation_t *)arg)->generation);
}

static xiaotai_link_stats_t stats(const observation_t *o)
{
    xiaotai_link_stats_t s;
    CHECK(xiaotai_link_stats(o->generation, &s));
    return s;
}

static xiaotai_link_profile_t profile(void)
{
    return (xiaotai_link_profile_t){
        .audio = {.send = true, .send_stream = 3, .send_format = alaw,
            .peer_streams = XIAOTAI_LINK_PEER_STREAM_FOLLOW, .receive = true, .receive_stream = 2,
            .receive_request = true, .receive_formats = {alaw, opus}},
        .video = {.send = true, .send_stream = 8, .receive = true, .receive_stream = 6,
            .receive_request = true, .receive_formats = {h264}},
    };
}

static void test_send_rules(void)
{
    boot_online(&(xiaotai_transport_config_t){.audio_backlog_limit_bytes = 860});
    observation_t o = {.profile = profile()};
    open_link_with(&o, 10);
    const uint32_t gen = o.generation;
    const xiaotai_media_sink_t *sink = xiaotai_link_audio_sink();
    CHECK(sink->send_stack_bytes == XIAOTAI_RTC_SEND_STACK_UNMEASURED);
    CHECK(!sink->ready(sink->ctx, gen));
    CHECK(sink->send(sink->ctx, gen, 11, audio, sizeof(audio)) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_link_permit(gen, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
    CHECK(sink->ready(sink->ctx, gen));
    CHECK(sink->send(sink->ctx, gen, 11, audio, sizeof(audio) - 1U) == XIAOTAI_ERR_INVALID_SIZE);
    xiaotai_transport_host_backlog_set(860);
    CHECK(xiaotai_link_backlog(gen) == 860);
    CHECK(sink->send(sink->ctx, gen, 11, audio, sizeof(audio)) == XIAOTAI_ERR_BUSY);
    CHECK(!calls(XIAOTAI_TRANSPORT_HOST_SEND));
    xiaotai_transport_host_backlog_set(859);
    CHECK(sink->send(sink->ctx, gen, 11, audio, sizeof(audio)) == XIAOTAI_OK);
    xiaotai_link_frame_t frame;
    CHECK(xiaotai_transport_host_last_frame(&frame));
    CHECK(frame.stream == 3 && frame.timestamp_ms == 11 && frame.format.codec == alaw.codec &&
          frame.format.rate_hz == alaw.rate_hz && frame.format.channels == alaw.channels);
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_AUDIO, 5, true) == XIAOTAI_OK);
    CHECK(xiaotai_link_permit(gen, XIAOTAI_LINK_AUDIO, false) == XIAOTAI_OK);
    CHECK(xiaotai_link_send_audio(gen, 12, audio, sizeof(audio)) == XIAOTAI_OK);
    CHECK(xiaotai_transport_host_last_frame(&frame) && frame.stream == 5);
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_AUDIO, 3, false) == XIAOTAI_OK);
    CHECK(xiaotai_link_can_send(gen, XIAOTAI_LINK_AUDIO));
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_AUDIO, 5, false) == XIAOTAI_OK);
    CHECK(!xiaotai_link_can_send(gen, XIAOTAI_LINK_AUDIO));
    const xiaotai_link_stats_t s = stats(&o);
    CHECK(s.audio.sent == 2U && s.audio.send_gated == 1U && s.audio.send_congested == 1U);
    settle();
    const unsigned subscriptions = o.subscriptions;
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_AUDIO, 0, true) == XIAOTAI_OK);
    settle();
    CHECK(o.subscriptions == subscriptions + 1 && o.last_subscription_kind == XIAOTAI_LINK_AUDIO &&
          o.last_subscription_stream == 0 && o.last_subscription_on);
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_AUDIO, 15, true) == XIAOTAI_OK);
    settle();
    CHECK(o.subscriptions == subscriptions + 2 && o.last_subscription_stream == 15 && o.last_subscription_on);
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_AUDIO, 16, true) == XIAOTAI_ERR_INVALID_ARG);
    settle();
    CHECK(o.subscriptions == subscriptions + 2 && o.closed == 0);
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_AUDIO, 10, false) == XIAOTAI_OK);
    settle();
    CHECK(o.subscriptions == subscriptions + 3 && o.last_subscription_stream == 10 && !o.last_subscription_on);
    shutdown_all();
}

static void test_audio_failure_policy(uint8_t limit)
{
    boot_online(&(xiaotai_transport_config_t){.audio_failures_end = limit});
    observation_t o = {.profile = profile()};
    open_link_with(&o, 10);
    CHECK(xiaotai_link_permit(o.generation, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
    const unsigned failures = limit ? limit : 10;
    for (unsigned i = 1; i <= failures; ++i) {
        xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SEND, XIAOTAI_FAIL, -23, 1);
        CHECK(xiaotai_link_send_audio(o.generation, i, audio, sizeof(audio)) == XIAOTAI_FAIL);
        settle();
        if (i == 1 && limit > 1) {
            CHECK(xiaotai_link_send_audio(o.generation, i, audio, sizeof(audio)) == XIAOTAI_OK);
            xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SEND, XIAOTAI_FAIL, -22, 1);
            CHECK(xiaotai_link_send_audio(o.generation, i, audio, sizeof(audio)) == XIAOTAI_FAIL);
            xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SEND, XIAOTAI_ERR_BUSY, 0, 1);
            CHECK(xiaotai_link_send_audio(o.generation, i, audio, sizeof(audio)) == XIAOTAI_ERR_BUSY);
        }
        if (i < failures || !limit) CHECK(o.closed == 0);
    }
    CHECK(o.closed == (limit ? 1U : 0U));
    if (limit) CHECK(o.reason == XIAOTAI_LINK_END_NETWORK && o.detail == -23);
    shutdown_all();
}

typedef struct { xiaotai_signal_t entered, resume; } pause_t;

static void pause_init(pause_t *pause)
{
    CHECK(xiaotai_signal_init(&pause->entered) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&pause->resume) == XIAOTAI_OK);
}

static void pause_free(pause_t *pause)
{
    xiaotai_signal_deinit(&pause->entered);
    xiaotai_signal_deinit(&pause->resume);
}

static void pause_here(pause_t *pause)
{
    xiaotai_signal_give(&pause->entered);
    CHECK(xiaotai_signal_take(&pause->resume, 5000));
}

static _Thread_local pause_t *allocation_pause;
static _Thread_local bool allocation_fails;
static _Atomic size_t miss_reservation;

extern void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    size_t expected = size;
    if (kind == XIAOTAI_MEM_BULK && size && atomic_compare_exchange_strong(&miss_reservation, &expected, 0U))
        return NULL;
    if (allocation_pause && kind == XIAOTAI_MEM_BULK) {
        pause_here(allocation_pause);
        if (allocation_fails) return NULL;
    }
    return __real_xiaotai_mem_alloc(kind, size);
}

typedef struct {
    pause_t pause;
    xiaotai_transport_host_call_kind_t kind;
    uintptr_t blocked_conn;
} hook_t;

static void block_media(void *arg, xiaotai_transport_host_call_kind_t kind, uintptr_t conn, int64_t value)
{
    (void)value;
    hook_t *h = arg;
    if (kind == h->kind && conn == h->blocked_conn) pause_here(&h->pause);
}

typedef struct { uint32_t gen; xiaotai_err_t result; bool command; } tx_t;

static void *send_thread(void *arg)
{
    tx_t *tx = arg;
    tx->result = tx->command ? xiaotai_link_command(tx->gen, UINT32_C(0x12122101), audio, 1) :
        xiaotai_link_send_audio(tx->gen, 12, audio, sizeof(audio));
    return NULL;
}

static void test_send_lease(void)
{
    boot_online(&(xiaotai_transport_config_t){.links = 2, .release_failures_tolerated = true});
    observation_t first = {.profile = profile()}, next = {.profile = profile()};
    open_link_with(&first, 10);
    CHECK(xiaotai_link_permit(first.generation, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
    hook_t hook = {.kind = XIAOTAI_TRANSPORT_HOST_SEND, .blocked_conn = 10};
    pause_init(&hook.pause);
    xiaotai_transport_host_media_hook(block_media, &hook);
    pthread_t thread;
    tx_t tx = {.gen = first.generation};
    CHECK(pthread_create(&thread, NULL, send_thread, &tx) == 0);
    CHECK(xiaotai_signal_take(&hook.pause.entered, 3000));
    CHECK(xiaotai_run(disconnect_on_owner, &first) == XIAOTAI_OK);
    settle();
    CHECK(!xiaotai_link_can_send(first.generation, XIAOTAI_LINK_AUDIO));
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 0);
    open_link_with(&next, 11); /* Same logical slot, distinct generation. */
    CHECK(first.generation != next.generation);
    CHECK(xiaotai_link_permit(next.generation, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
    CHECK(xiaotai_link_send_audio(next.generation, 13, audio, sizeof(audio)) == XIAOTAI_ERR_BUSY);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SEND) == 1);
    CHECK(xiaotai_transport_restart() == XIAOTAI_OK);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 0 && calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 0);
    xiaotai_signal_give(&hook.pause.resume);
    CHECK(pthread_join(thread, NULL) == 0 && tx.result == XIAOTAI_OK);
    settle();
    CHECK(first.closed == 0 && next.closed == 1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 2 && calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1);
    CHECK(status_now().send_call_max_us > 0);
    xiaotai_transport_host_stopped(0);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1);
    pause_free(&hook.pause);
    shutdown_all();
}

static xiaotai_err_t worker_send_result;
static xiaotai_signal_t worker_result_ready, owner_close_ready, worker_control_ready;
static unsigned worker_audio_steps;
static uint32_t worker_audio_generation;
static bool worker_control_seen;

static xiaotai_media_uplink_work_t worker_send_one(void)
{
    CHECK(xt_on_worker() && !xiaotai_on_owner());
    if (worker_audio_steps++) {
        CHECK(worker_control_seen);
        return XIAOTAI_MEDIA_UPLINK_IDLE;
    }
    xiaotai_transport_reset(); /* Reject resetting the worker from itself. */
    worker_send_result = xiaotai_link_send_audio(worker_audio_generation, 12, audio, sizeof(audio));
    xiaotai_signal_give(&worker_result_ready);
    return XIAOTAI_MEDIA_UPLINK_PENDING;
}

static void owner_close_during_worker_send(void *arg)
{
    CHECK(xiaotai_on_owner() && !xt_on_worker());
    disconnect_on_owner(arg);
    xiaotai_signal_give(&owner_close_ready);
}

static void worker_control_after_send(uint32_t generation, uint32_t unused)
{
    (void)unused;
    CHECK(xt_on_worker());
    CHECK(!xiaotai_link_can_send(generation, XIAOTAI_LINK_AUDIO));
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SEND) == 1);
    worker_control_seen = true;
    xiaotai_signal_give(&worker_control_ready);
}

/* Exercise the real worker scheduling with a fake media frame source. */
static void test_worker_send_close_boundary(void)
{
    boot_online(&(xiaotai_transport_config_t){.media_uplink_worker = XIAOTAI_TRANSPORT_ON});
    observation_t o = {.profile = profile()};
    open_link_with(&o, 10);
    CHECK(xiaotai_link_permit(o.generation, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
    hook_t hook = {.kind = XIAOTAI_TRANSPORT_HOST_SEND, .blocked_conn = 10};
    pause_init(&hook.pause);
    xiaotai_signal_t *const signals[] = {&worker_result_ready, &owner_close_ready, &worker_control_ready};
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i)
        CHECK(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    worker_audio_steps = 0;
    worker_audio_generation = o.generation;
    worker_control_seen = false;
    xiaotai_transport_host_media_hook(block_media, &hook);
    CHECK(fake_media_uplink_wake(worker_send_one));
    CHECK(xiaotai_signal_take(&hook.pause.entered, 3000));
    const int64_t owner_posted_at = xiaotai_time_us();
    CHECK(xiaotai_run(owner_close_during_worker_send, &o) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&owner_close_ready, 3000));
    const int64_t owner_close_us = xiaotai_time_us() - owner_posted_at;
    CHECK(!xiaotai_link_can_send(o.generation, XIAOTAI_LINK_AUDIO));
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 0);
    CHECK(xt_worker_post(worker_control_after_send, o.generation, 0));
    CHECK(!xiaotai_signal_take(&worker_control_ready, 0));
    xiaotai_signal_give(&hook.pause.resume);
    CHECK(xiaotai_signal_take(&worker_result_ready, 3000) && worker_send_result == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&worker_control_ready, 3000));
    settle();
    CHECK(worker_audio_steps == 2);
    printf("{\"case\":\"worker_send_close_boundary\",\"owner_gate_while_blocked\":true,"
           "\"disconnect_before_next_job\":true,\"sdk_disconnect_while_blocked\":false,"
           "\"owner_close_post_ack_us\":%lld}\n", (long long)owner_close_us);
    fflush(stdout);
    pause_free(&hook.pause);
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) xiaotai_signal_deinit(signals[i]);
    shutdown_all();
}

static void worker_send_for_reset(uint32_t generation, uint32_t unused)
{
    (void)unused;
    CHECK(xt_on_worker());
    CHECK(xiaotai_link_send_audio(generation, 12, audio, sizeof(audio)) == XIAOTAI_OK);
}

static void test_worker_reset_after_timeout(void)
{
    boot_online(&(xiaotai_transport_config_t){.media_uplink_worker = XIAOTAI_TRANSPORT_ON});
    observation_t o = {.profile = profile()};
    open_link_with(&o, 10);
    CHECK(xiaotai_link_permit(o.generation, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
    hook_t hook = {.kind = XIAOTAI_TRANSPORT_HOST_SEND, .blocked_conn = 10};
    pause_init(&hook.pause);
    xiaotai_transport_host_media_hook(block_media, &hook);
    CHECK(xt_worker_post(worker_send_for_reset, o.generation, 0));
    CHECK(xiaotai_signal_take(&hook.pause.entered, 3000));
    xiaotai_transport_reset(); /* Bounded wait expires while SDK owns the frame. */
    CHECK(xt.worker_created && xt.jobs_ready && atomic_load(&xt.worker_running));
    xiaotai_signal_give(&hook.pause.resume);
    const int64_t limit = xiaotai_time_us() + 5000000;
    while (atomic_load(&xt.worker_running) && xiaotai_time_us() < limit) xiaotai_delay_ms(1);
    CHECK(!atomic_load(&xt.worker_running) && xt.worker_created);
    xiaotai_transport_reset(); /* Joins the late exit exactly once. */
    CHECK(!xt.worker_created && !xt.jobs_ready && !xiaotai_signal_take(&xt.worker_exited, 0));
    pause_free(&hook.pause);
    shutdown_all();
    boot_online(NULL);
    shutdown_all();
    CHECK(!xiaotai_signal_take(&xt.worker_exited, 0));
}

static void test_control_lock(void)
{
    boot_online(NULL);
    observation_t o = {.profile = profile()};
    open_link_with(&o, 10);
    hook_t hook = {.kind = XIAOTAI_TRANSPORT_HOST_COMMAND, .blocked_conn = 10};
    pause_init(&hook.pause);
    xiaotai_transport_host_media_hook(block_media, &hook);
    pthread_t thread;
    tx_t tx = {.gen = o.generation, .command = true};
    CHECK(pthread_create(&thread, NULL, send_thread, &tx) == 0);
    CHECK(xiaotai_signal_take(&hook.pause.entered, 3000));
    CHECK(xiaotai_link_request_key_frame(o.generation) == XIAOTAI_ERR_BUSY);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_KEY_FRAME) == 0 && stats(&o).control_busy == 1);
    xiaotai_signal_give(&hook.pause.resume);
    CHECK(pthread_join(thread, NULL) == 0 && tx.result == XIAOTAI_OK);
    CHECK(stats(&o).commands_sent == 1);
    xiaotai_transport_host_media_hook(NULL, NULL);
    CHECK(xiaotai_link_command(o.generation, 0x2001, NULL, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_link_command(o.generation, 0x2001, audio, XIAOTAI_LINK_COMMAND_LIMIT_BYTES + 1U) ==
          XIAOTAI_ERR_INVALID_SIZE);
    xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_COMMAND, XIAOTAI_FAIL, 0, 1);
    CHECK(xiaotai_link_command(o.generation, 0x2001, NULL, 0) == XIAOTAI_FAIL);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_COMMAND) == 2 && stats(&o).commands_send_failed == 1);
    pause_free(&hook.pause);
    shutdown_all();
}

static void pause_owner(void *arg) { pause_here(arg); }

static void test_audio_unsubscribe_permit(bool revoke)
{
    boot_online(&(xiaotai_transport_config_t){.audio_unsubscribe_revokes_permit = revoke});
    observation_t o = {.profile = profile()};
    const uintptr_t conn = 10;
    open_link_with(&o, conn);
    CHECK(xiaotai_link_permit(o.generation, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
    CHECK(xiaotai_link_permit(o.generation, XIAOTAI_LINK_VIDEO, true) == XIAOTAI_OK);
    CHECK(xiaotai_transport_host_peer_subscribe(conn, o.generation, XIAOTAI_LINK_AUDIO, 5, true) == XIAOTAI_OK);
    settle();
    CHECK(o.subscriptions == 1 && stats(&o).audio_send_stream == 5);

    pause_t pause;
    pause_init(&pause);
    CHECK(xiaotai_run(pause_owner, &pause) == XIAOTAI_OK && xiaotai_signal_take(&pause.entered, 5000));
    CHECK(xiaotai_transport_host_peer_subscribe(conn, o.generation, XIAOTAI_LINK_AUDIO, 3, false) == XIAOTAI_OK);
    CHECK(xiaotai_link_can_send(o.generation, XIAOTAI_LINK_AUDIO));
    CHECK(xiaotai_link_send_audio(o.generation, 20, audio, sizeof(audio)) == XIAOTAI_OK);
    xiaotai_link_frame_t frame;
    CHECK(xiaotai_transport_host_last_frame(&frame) && frame.stream == 5);

    CHECK(xiaotai_transport_host_peer_subscribe(conn + 1U, 0, XIAOTAI_LINK_AUDIO, 5, false) ==
          XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_link_can_send(o.generation, XIAOTAI_LINK_AUDIO));

    CHECK(xiaotai_transport_host_peer_subscribe(conn, 0, XIAOTAI_LINK_AUDIO, 5, false) == XIAOTAI_OK);
    const bool ready = xiaotai_link_can_send(o.generation, XIAOTAI_LINK_AUDIO);
    const xiaotai_err_t result = xiaotai_link_send_audio(o.generation, 40, audio, sizeof(audio));
    const xiaotai_link_stats_t withdrawn = stats(&o);
    printf("audio withdrawal before owner: opt=%u ready=%u send=%d permit=%u subscribed=%u wire=%u\n",
           revoke, ready, result, withdrawn.audio_permitted, withdrawn.audio_subscribed,
           (unsigned)calls(XIAOTAI_TRANSPORT_HOST_SEND));
    CHECK(ready == !revoke && result == (revoke ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK));
    CHECK(withdrawn.audio_permitted == !revoke && !withdrawn.audio_subscribed &&
          withdrawn.video_permitted && xiaotai_link_can_send(o.generation, XIAOTAI_LINK_VIDEO));
    CHECK(o.subscriptions == 1 && calls(XIAOTAI_TRANSPORT_HOST_SEND) == (revoke ? 1U : 2U));
    xiaotai_signal_give(&pause.resume);
    settle();
    CHECK(o.subscriptions == 3 && o.last_subscription_stream == 5 && !o.last_subscription_on);
    CHECK(xiaotai_link_permit(o.generation, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
    CHECK(xiaotai_link_send_audio(o.generation, 60, audio, sizeof(audio)) == XIAOTAI_OK);
    CHECK(xiaotai_transport_host_last_frame(&frame) && frame.stream == 5 && frame.timestamp_ms == 60);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SEND) == (revoke ? 2U : 3U));
    CHECK(xiaotai_run(disconnect_on_owner, &o) == XIAOTAI_OK);
    settle();
    CHECK(xiaotai_transport_host_peer_subscribe(conn, 0, XIAOTAI_LINK_AUDIO, 5, false) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!xiaotai_link_can_send(o.generation, XIAOTAI_LINK_AUDIO) &&
          xiaotai_link_send_audio(o.generation, 80, audio, sizeof(audio)) == XIAOTAI_ERR_INVALID_STATE);
    pause_free(&pause);
    shutdown_all();
}

static void test_command_pool(xiaotai_transport_command_pool_mode_t mode)
{
    const bool demand = mode == XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND;
    boot_online(&(xiaotai_transport_config_t){.links = 2, .command_pool_bytes = 16, .command_pool_mode = mode});
    CHECK(xt.command_on_demand == demand);
    CHECK((xt.command_pool != NULL) != demand && (xt.command_slots != NULL) == demand);
    observation_t a = {.profile = profile()}, b = {.profile = profile()};
    a.profile.command_max_bytes = 6;
    a.profile.command_pool_bytes = 8;
    b.profile.command_max_bytes = b.profile.command_pool_bytes = 16;
    open_link_with(&a, 10);
    open_link_with(&b, 11);
    pause_t pause;
    pause_init(&pause);
    CHECK(xiaotai_run(pause_owner, &pause) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&pause.entered, 3000));
    uint8_t payload[20] = {31};
    xiaotai_mem_host_clear();
    xiaotai_transport_host_command(10, 0, 0x2100, payload, 7); /* Per-command limit. */
    xiaotai_transport_host_command(10, 0, 0x2101, payload, 6);
    xiaotai_transport_host_command(10, 0, 0x2102, payload, 3); /* Would exceed a's share. */
    xiaotai_transport_host_command(11, 0, 0x2103, payload, 10); /* Shared exact fit: 6 + 10. */
    xiaotai_transport_host_command(11, 0, 0x2104, payload, 1); /* Share fits; shared pool full. */
    CHECK(xiaotai_mem_host_allocations() == (demand ? 2U : 0U));
    memset(payload, 0, sizeof(payload)); /* Stored events cannot alias SDK data. */
    xiaotai_signal_give(&pause.resume);
    settle();
    CHECK(a.commands == 3 && a.last_command.word == 0x2102 && a.last_command.dropped);
    CHECK(b.commands == 2 && b.last_command.word == 0x2104 && b.last_command.dropped);
    CHECK(a.initial_commands[0].word == 0x2100 && a.initial_commands[0].dropped);
    CHECK(a.initial_commands[1].word == 0x2101 && !a.initial_commands[1].dropped && a.initial_commands[1].first == 31);
    CHECK(b.initial_commands[0].word == 0x2103 && !b.initial_commands[0].dropped && b.initial_commands[0].first == 31);
    CHECK(stats(&a).command_largest == 7 && stats(&a).command_pool_peak == 6 && stats(&a).commands_dropped == 2);
    CHECK(stats(&b).command_largest == 10 && stats(&b).command_pool_peak == 10 && stats(&b).commands_dropped == 1);
    CHECK(status_now().command_pool_bytes == 16 && status_now().command_pool_peak == 16 &&
          status_now().command_largest == 10);
    CHECK(xt.command_tail == (demand ? 0U : 18U));
    payload[0] = 41;
    xiaotai_transport_host_command(11, 0, UINT32_C(0x00062101), payload, 16); /* Full pool reusable. */
    memset(payload, 0, sizeof(payload));
    settle();
    CHECK(b.commands == 3 && b.last_command.word == UINT32_C(0x00062101) && b.last_command.size == 16 &&
          b.last_command.first == 41 && !b.last_command.dropped);
    CHECK(xt.command_tail == (demand ? 0U : 17U)); /* Resident wrap: 16 bytes + NUL. */
    xiaotai_transport_host_command(11, 0, 0x2000, NULL, 0);
    settle();
    CHECK(!b.last_command.dropped && b.last_command.size == 0 && stats(&b).command_pool_peak == 16);
    pause_free(&pause);
    shutdown_all();
}

static void test_xmini_command_budget(xiaotai_transport_command_pool_mode_t mode)
{
    const bool demand = mode == XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND;
    boot_online(&(xiaotai_transport_config_t){.command_pool_bytes = 16384, .command_pool_mode = mode});
    observation_t o = {.profile = {.command_max_bytes = 12288, .command_pool_bytes = 16384}};
    open_link_with(&o, 10);
    pause_t pause;
    pause_init(&pause);
    CHECK(xiaotai_run(pause_owner, &pause) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&pause.entered, 3000));
    uint8_t payload[9000] = {31};
    payload[sizeof(payload) - 1U] = 43;
    const uint32_t allocations = xiaotai_mem_host_allocations();
    xiaotai_transport_host_command(10, 0, UINT32_C(0x00054400), payload, sizeof(payload));
    xiaotai_transport_host_command(10, 0, UINT32_C(0x00064400), payload, sizeof(payload));
    CHECK(xiaotai_mem_host_allocations() == allocations + (demand ? 1U : 0U));
    memset(payload, 'x', sizeof(payload));
    xiaotai_signal_give(&pause.resume);
    settle();
    CHECK(o.commands == 2 && o.initial_commands[0].first == 31 && o.initial_commands[0].last == 43 &&
          o.initial_commands[0].size == sizeof(payload) && !o.initial_commands[0].dropped);
    CHECK(o.last_command.word == UINT32_C(0x00064400) && o.last_command.dropped);
    CHECK(stats(&o).command_pool_peak == sizeof(payload) && stats(&o).commands_dropped == 1);
    xiaotai_transport_host_command(10, 0, UINT32_C(0x00074400), payload, sizeof(payload));
    settle();
    CHECK(o.commands == 3 && !o.last_command.dropped && o.last_command.first == 'x' && o.last_command.last == 'x');
    pause_free(&pause);
    shutdown_all();
}

static void test_xmini_command_words(void)
{
    static const uint32_t words[] = {
        0x2000U, 0x2001U, 0x2100U, 0x2200U,
        0x00004000U, 0xffff4002U, 0xffff4200U, 0xffff4400U,
        0xabcd4200U, 0xabcd4201U,
    };
    boot_online(&(xiaotai_transport_config_t){.links = 1, .command_pool_bytes = 16384, .early_events_dropped = true});
    observation_t o = {.profile = {.command_max_bytes = 12288, .command_pool_bytes = 16384},
                       .expected_command_payload = "hello"};
    open_link_with(&o, 10);
    for (size_t i = 0; i < sizeof(words) / sizeof(*words); ++i)
        CHECK(xiaotai_link_command(o.generation, words[i], "{}", 2) == XIAOTAI_OK);
    xiaotai_transport_host_call_t records[64];
    const size_t count = xiaotai_transport_host_calls(records, sizeof(records) / sizeof(*records));
    CHECK(count <= sizeof(records) / sizeof(*records));
    size_t sent = 0;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].kind != XIAOTAI_TRANSPORT_HOST_COMMAND) continue;
        CHECK(records[i].connection == 10 && sent < sizeof(words) / sizeof(*words) && records[i].value == words[sent]);
        ++sent;
    }
    CHECK(sent == sizeof(words) / sizeof(*words));

    pause_t pause;
    pause_init(&pause);
    for (size_t i = 0; i < sent; ++i) {
        CHECK(xiaotai_run(pause_owner, &pause) == XIAOTAI_OK && xiaotai_signal_take(&pause.entered, 3000));
        uint8_t borrowed[] = "hello";
        xiaotai_transport_host_command(10, o.generation, words[i], borrowed, sizeof(borrowed) - 1U);
        memset(borrowed, '?', sizeof(borrowed));
        xiaotai_signal_give(&pause.resume);
        settle();
        CHECK(o.commands == i + 1U && o.last_command.word == words[i] && !o.last_command.dropped);
    }
    pause_free(&pause);
    xiaotai_transport_host_closed(10);
    settle();
    CHECK(xiaotai_link_command(o.generation, words[0], "{}", 2) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_COMMAND) == sent);
    puts("Xmini RTC: raw and packed command words round trip unchanged; owner received a copy; stale TX rejected");
    shutdown_all();
}

static void test_default_command_storage(bool external)
{
    const uint32_t resident_message = external ? 4096U : 2048U;
    const unsigned first_batch = external ? 3U : 4U;
    external_ram = external;
    boot_online(&(xiaotai_transport_config_t){.command_pool_bytes = 16384});
    CHECK(xt.command_pool && !!xt.command_slots == !external);
    CHECK(xt.command_resident_bytes == (external ? 16384U : 2048U));
    CHECK(status_now().command_pool_bytes == 16384);
    observation_t o = {.profile = {.command_max_bytes = 12288, .command_pool_bytes = 16384}};
    open_link_with(&o, 10);
    const uint32_t idle = status_now().engine_bytes;
    pause_t pause;
    pause_init(&pause);
    CHECK(xiaotai_run(pause_owner, &pause) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&pause.entered, 3000));
    static uint8_t data[12288];
    memset(data, 31, sizeof(data));
    const uint32_t allocations = xiaotai_mem_host_allocations();
    xiaotai_transport_host_command(10, 0, 0x2100, data, resident_message);
    CHECK(xiaotai_mem_host_allocations() == allocations);
    xiaotai_transport_host_command(10, 0, 0x2101, data, sizeof(data));
    if (!external) xiaotai_transport_host_command(10, 0, 0x2105, data, 2048U);
    xiaotai_transport_host_command(10, 0, 0x2102, data, 1); /* Shared cap, not free heap. */
    CHECK(xiaotai_mem_host_allocations() == allocations + (external ? 0U : 2U));
    CHECK(status_now().command_pool_peak == 16384);
    memset(data, 43, sizeof(data));
    xiaotai_signal_give(&pause.resume);
    settle();
    CHECK(o.commands == first_batch && o.initial_commands[0].size == resident_message &&
          o.initial_commands[0].first == 31);
    CHECK(o.initial_commands[1].size == 12288 && o.initial_commands[1].last == 31);
    if (!external) CHECK(o.initial_commands[2].size == 2048 && !o.initial_commands[2].dropped &&
                         o.initial_commands[2].last == 31);
    CHECK(!o.initial_commands[0].dropped && !o.initial_commands[1].dropped && o.last_command.dropped);
    CHECK(status_now().engine_bytes == idle && stats(&o).command_pool_peak == 16384);

    if (!external) {
        CHECK(xiaotai_run(pause_owner, &pause) == XIAOTAI_OK);
        CHECK(xiaotai_signal_take(&pause.entered, 3000));
        const uint32_t next = xiaotai_mem_host_allocations();
        xiaotai_transport_host_command(10, 0, 0x2103, data, 2042);
        CHECK(xiaotai_mem_host_allocations() == next);
        /* Terminator capacity must not enlarge the 2048-byte resident budget. */
        xiaotai_transport_host_command(10, 0, 0x2104, data, 7);
        CHECK(xiaotai_mem_host_allocations() == next + 1U);
        memset(data, 0, sizeof(data));
        xiaotai_signal_give(&pause.resume);
        settle();
        CHECK(o.commands == first_batch + 2U && o.last_command.size == 7 && o.last_command.last == 43);
        CHECK(status_now().engine_bytes == idle && xt.command_retired_bytes == 0);
    }
    pause_free(&pause);
    shutdown_all();
    CHECK(!xt.command_pool && !xt.command_slots && !xt.command_resident_bytes && !xt.command_retired_bytes);
    external_ram = false;
}

static void test_hybrid_reservation_unavailable(void)
{
    atomic_store(&miss_reservation, 2048U + XT_EVENT_SLOTS_INTERNAL + 1U);
    boot_online(&(xiaotai_transport_config_t){.command_pool_bytes = 16384});
    CHECK(!atomic_load(&miss_reservation) && !xt.command_pool && !xt.command_resident_bytes);
    CHECK(xt.command_slots && status_now().command_pool_bytes == 16384);
    observation_t o = {.profile = {.command_max_bytes = 12288, .command_pool_bytes = 16384}};
    open_link_with(&o, 10);
    const uint32_t idle = status_now().engine_bytes;
    uint8_t data[] = {31, 43};
    xiaotai_transport_host_command(10, 0, 0x2100, data, sizeof(data));
    data[0] = data[1] = 0;
    settle();
    CHECK(o.commands == 1 && !o.last_command.dropped && o.last_command.first == 31 && o.last_command.last == 43);
    CHECK(status_now().engine_bytes == idle);
    shutdown_all();
}

static void test_demand_command_oom(bool closes, bool hybrid)
{
    const uint32_t budget = hybrid ? 8192U : 2048U;
    const uint32_t size = hybrid ? 2049U : 1024U;
    boot_online(&(xiaotai_transport_config_t){.command_pool_bytes = budget,
                .command_oom_ends_link = closes,
                .command_pool_mode = hybrid ? XIAOTAI_TRANSPORT_COMMAND_POOL_DEFAULT :
                    XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND});
    const uint32_t idle_bytes = status_now().engine_bytes;
    observation_t o = {.profile = {.command_max_bytes = size, .command_pool_bytes = budget}};
    open_link_with(&o, 10);
    const uint32_t bytes = status_now().engine_bytes;
    uint8_t payload[4097] = {20};
    pause_t pause;
    pause_init(&pause);
    CHECK(xiaotai_run(pause_owner, &pause) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&pause.entered, 3000));
    xiaotai_mem_host_fail_nth(1);
    xiaotai_transport_host_command(10, 0, 0x2000, payload, size);
    CHECK(!xiaotai_mem_host_fail_pending() && o.commands == 0);
    CHECK(!stats(&o).command_pool_peak && !status_now().command_pool_peak && status_now().engine_bytes == bytes);
    xiaotai_transport_host_command(10, 0, 0x2101, payload, size);
    xiaotai_signal_give(&pause.resume);
    settle();
    if (closes) {
        CHECK(o.commands == 0 && o.closed == 1 && o.reason == XIAOTAI_LINK_END_NO_MEMORY &&
              o.detail == XIAOTAI_ERR_NO_MEM);
        CHECK(stats(&o).commands_received == 1 && stats(&o).commands_dropped == 1);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1);
        xiaotai_transport_host_closed(10);
        settle();
        CHECK(o.closed == 1);
    } else {
        CHECK(o.closed == 0 && o.commands == 1 && !o.last_command.dropped && o.last_command.first == 20 &&
              o.last_command.word == 0x2101);
        CHECK(stats(&o).commands_received == 2 && stats(&o).commands_dropped == 1);
    }
    CHECK(status_now().command_pool_bytes == budget);
    CHECK(status_now().engine_bytes == (closes ? idle_bytes : bytes));
    CHECK(stats(&o).command_pool_peak == size);
    pause_free(&pause);
    shutdown_all();
}

static void test_borrowed_owner_command(xiaotai_transport_command_pool_mode_t mode)
{
    boot_online(&(xiaotai_transport_config_t){.command_pool_bytes = 16, .command_pool_mode = mode});
    observation_t o = {.profile = {.command_max_bytes = 16, .command_pool_bytes = 16}, .echo_conn = 10};
    open_link_with(&o, 10);
    uint8_t payload[16] = {31};
    payload[15] = 47;
    xiaotai_transport_host_command(10, 0, 0x2100, payload, sizeof(payload));
    settle();
    CHECK(o.commands == 2 && !o.initial_commands[0].dropped && o.initial_commands[1].dropped);
    CHECK(stats(&o).command_pool_peak == sizeof(payload) && stats(&o).commands_dropped == 1U);
    shutdown_all();
}

typedef struct {
    pause_t pause;
    bool fail_copy;
    uint32_t bytes;
} copy_fixture_t;

static void *waiting_command(void *arg)
{
    copy_fixture_t *fixture = arg;
    const uint8_t data[4097] = {1, 2, 3};
    allocation_pause = &fixture->pause;
    allocation_fails = fixture->fail_copy;
    xiaotai_transport_host_command(10, 0, 0x2100, data, fixture->bytes);
    allocation_pause = NULL;
    allocation_fails = false;
    return NULL;
}

static void test_demand_command_order(bool cancel, bool fail_copy, bool hybrid)
{
    const uint32_t budget = hybrid ? 8192U : 16U;
    const uint32_t size = hybrid ? 4097U : 3U;
    boot_online(&(xiaotai_transport_config_t){.command_pool_bytes = budget,
                .command_oom_ends_link = fail_copy,
                .command_pool_mode = hybrid ? XIAOTAI_TRANSPORT_COMMAND_POOL_DEFAULT :
                    XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND});
    const xiaotai_link_profile_t settings = {.command_max_bytes = budget, .command_pool_bytes = budget};
    observation_t o = {.profile = settings};
    observation_t next = {.profile = settings};
    open_link_with(&o, 10);
    copy_fixture_t fixture = {.fail_copy = fail_copy, .bytes = size};
    pause_init(&fixture.pause);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, waiting_command, &fixture) == 0);
    CHECK(xiaotai_signal_take(&fixture.pause.entered, 3000));
    const uint8_t payload[] = {9, 8, 7};
    xiaotai_transport_host_command(10, 0, 0x2101, payload, sizeof(payload));
    settle();
    /* A later command cannot pass an earlier unfinished copy. */
    CHECK(o.commands == 0);
    if (cancel) {
        CHECK(xiaotai_run(disconnect_on_owner, &o) == XIAOTAI_OK);
        settle();
        xiaotai_transport_host_closed(10);
        settle();
        CHECK(o.closed == 0 && status_now().links_releasing == 0);
        open_link_with(&next, 11);
    }
    xiaotai_signal_give(&fixture.pause.resume);
    CHECK(pthread_join(thread, NULL) == 0);
    settle();
    if (cancel) {
        CHECK(o.commands == 0 && o.closed == 0 && next.closed == 0);
        uint8_t full[16] = {31};
        xiaotai_transport_host_command(11, 0, 0x2102, full, sizeof(full));
        settle();
        CHECK(next.commands == 1 && !next.last_command.dropped && next.last_command.first == 31);
    } else {
        CHECK(o.commands == 2 && o.initial_commands[0].word == 0x2100 && o.initial_commands[0].first == 1);
        CHECK(o.initial_commands[1].word == 0x2101 && o.initial_commands[1].first == 9);
        CHECK(stats(&o).command_pool_peak == size + sizeof(payload));
    }
    CHECK(xt.command_retired_bytes == 0U && xt.event_count == 0U);
    for (unsigned i = 0; i <= xt.event_slots; ++i) CHECK(!xt.command_slots[i]);
    pause_free(&fixture.pause);
    shutdown_all();
}

static void test_full_room_command_budget(xiaotai_transport_command_pool_mode_t mode)
{
    enum { ROOM_MAX = 32768, ROOM_SHARE = ROOM_MAX * 2 };
    static uint8_t payload[ROOM_MAX + 1];
    const bool demand = mode == XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND;
    boot_online(&(xiaotai_transport_config_t){.command_pool_bytes = ROOM_SHARE, .command_pool_mode = mode});
    const uint32_t idle = status_now().engine_bytes;
    for (uintptr_t conn = 10; conn < 13; ++conn) {
        observation_t o = {.profile = {.command_max_bytes = ROOM_MAX, .command_pool_bytes = ROOM_SHARE}};
        open_link_with(&o, conn);
        const uint32_t open = status_now().engine_bytes;
        pause_t pause;
        pause_init(&pause);
        CHECK(xiaotai_run(pause_owner, &pause) == XIAOTAI_OK);
        CHECK(xiaotai_signal_take(&pause.entered, 3000));
        const uint32_t allocations = xiaotai_mem_host_allocations();
        payload[0] = 31;
        xiaotai_transport_host_command(conn, 0, 0x2100, payload, ROOM_MAX);
        payload[0] = 47;
        xiaotai_transport_host_command(conn, 0, 0x2101, payload, ROOM_MAX);
        xiaotai_transport_host_command(conn, 0, 0x2102, payload, 1);
        xiaotai_transport_host_command(conn, 0, 0x2103, payload, sizeof(payload));
        CHECK(xiaotai_mem_host_allocations() == allocations + (demand ? 2U : 0U));
        const uint32_t pending = status_now().engine_bytes;
        CHECK(demand ? pending > open + ROOM_SHARE : pending == open);
        memset(payload, 0, sizeof(payload));
        xiaotai_signal_give(&pause.resume);
        settle();
        CHECK(o.commands == 4 && !o.initial_commands[0].dropped && o.initial_commands[0].first == 31);
        CHECK(!o.initial_commands[1].dropped && o.initial_commands[1].first == 47);
        CHECK(o.initial_commands[2].dropped && o.initial_commands[3].dropped);
        CHECK(stats(&o).command_pool_peak == ROOM_SHARE && stats(&o).commands_dropped == 2);
        CHECK(status_now().engine_bytes == open);
        CHECK(xiaotai_run(disconnect_on_owner, &o) == XIAOTAI_OK);
        settle();
        xiaotai_transport_host_closed(conn);
        settle();
        const xiaotai_transport_status_t after = status_now();
        CHECK(o.closed == 0 && !after.links_open && !after.links_releasing && after.engine_bytes == idle);
        pause_free(&pause);
    }
    shutdown_all();
}

static void frame_in(uintptr_t conn, xiaotai_link_media_kind_t kind, xiaotai_link_format_t format, uint8_t stream)
{
    const xiaotai_link_frame_t f = {.kind = kind, .format = format, .stream = stream, .timestamp_ms = 415};
    xiaotai_transport_host_frame(conn, &f, audio, 80);
}

static void frame_in_on_subscribe(void *arg, xiaotai_transport_host_call_kind_t kind, uintptr_t conn, int64_t value)
{
    (void)arg;
    if (kind == XIAOTAI_TRANSPORT_HOST_SUBSCRIBE && (value & 1)) frame_in(conn, XIAOTAI_LINK_AUDIO, opus, 2);
}

static xiaotai_eventlog_topic_snapshot_t topic(const char *name)
{
    xiaotai_eventlog_topic_snapshot_t s;
    for (size_t i = 0; i < xiaotai_eventlog_topic_count(); ++i) {
        CHECK(xiaotai_eventlog_topic_read(i, &s));
        if (!strcmp(s.desc->source, "link") && !strcmp(s.desc->name, name)) return s;
    }
    CHECK(false);
    return (xiaotai_eventlog_topic_snapshot_t){0};
}

static bool refuse_frame(void *arg, uint32_t gen, const xiaotai_link_frame_t *f, const uint8_t *data, size_t bytes)
{
    (void)arg; (void)gen; (void)f; (void)data; (void)bytes;
    return false;
}

static void test_pcma_opus_compatibility(void)
{
    const uint8_t encoded[] = {0xf8, 0xff, 0xfe}; /* Mono Opus, 20 ms. */
    static const struct {
        bool enabled, prepared, explicit_opus, default_receiver, accepts;
        uint8_t stream;
    } cases[] = {
        {false, true, false, true, false, XIAOTAI_LINK_STREAM_ANY},
        {true, false, false, true, false, 1},
        {true, true, false, false, false, 10},
        {true, true, false, true, true, XIAOTAI_LINK_STREAM_ANY},
        {true, true, false, true, true, 1},
        {true, true, false, true, true, 10},
        {false, true, true, true, true, 14},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const bool specific = cases[i].stream != XIAOTAI_LINK_STREAM_ANY;
        const uint8_t selected_stream = specific ? cases[i].stream : 3U;
        boot_online(&(xiaotai_transport_config_t){.accept_pcma_opus_downlink = cases[i].enabled});
        fake_media_opus(cases[i].prepared);
        observation_t o = {.profile.audio = {.send = true, .send_stream = 10, .send_format = alaw,
            .receive = true, .receive_stream = cases[i].stream, .receive_request = specific,
            .receive_formats = {alaw, cases[i].explicit_opus ? opus : (xiaotai_link_format_t){0}}}};
        open_link_with(&o, 10);
        xiaotai_link_frame_t f = {.kind = XIAOTAI_LINK_AUDIO, .format = opus, .stream = selected_stream,
                                  .timestamp_ms = 415};
        xiaotai_transport_host_frame(10, &f, encoded, sizeof(encoded));
        CHECK(fake_media_record().calls == 0 && stats(&o).audio.receive_closed == 1);
        CHECK(xiaotai_link_receive(o.generation, XIAOTAI_LINK_AUDIO,
                                   cases[i].default_receiver ? NULL : refuse_frame, NULL) == XIAOTAI_OK);
        const unsigned subscribed = calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE);
        CHECK(subscribed == (specific ? 1U : 0U));

        xiaotai_transport_host_frame(11, &f, encoded, sizeof(encoded));
        CHECK(fake_media_record().calls == 0);
        if (specific) {
            ++f.stream;
            xiaotai_transport_host_frame(10, &f, encoded, sizeof(encoded));
            CHECK(fake_media_record().calls == 0);
            f.stream = selected_stream;
        }
        f.format.rate_hz = 8000U;
        xiaotai_transport_host_frame(10, &f, encoded, sizeof(encoded));
        f.format.rate_hz = 16000U;
        f.format.channels = 2;
        xiaotai_transport_host_frame(10, &f, encoded, sizeof(encoded));
        CHECK(fake_media_record().calls == 0);
        f.format = opus;
        const xiaotai_link_media_stats_t before = stats(&o).audio;
        xiaotai_transport_host_frame(10, &f, encoded, sizeof(encoded));
        const fake_media_record_t received = fake_media_record();
        CHECK(received.calls == (unsigned)cases[i].accepts);
        if (cases[i].accepts) {
            CHECK(received.generation == o.generation && received.frame.encoding == XIAOTAI_CODEC_OPUS &&
                  received.frame.rate_hz == 16000 && received.frame.stream == selected_stream &&
                  received.frame.size == sizeof(encoded) && received.first_byte == encoded[0]);
        } else {
            const xiaotai_link_media_stats_t after = stats(&o).audio;
            CHECK(after.receive_filtered == before.receive_filtered + 1U &&
                  after.receive_refused == before.receive_refused);
        }
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == subscribed && !o.closed);

        f.format = alaw;
        xiaotai_transport_host_frame(10, &f, audio, sizeof(audio));
        CHECK(fake_media_record().calls == received.calls + (unsigned)cases[i].default_receiver);
        const unsigned calls_before_close = fake_media_record().calls;
        CHECK(xiaotai_run(disconnect_on_owner, &o) == XIAOTAI_OK);
        settle();
        f.format = opus;
        xiaotai_transport_host_frame(10, &f, encoded, sizeof(encoded));
        CHECK(fake_media_record().calls == calls_before_close);
        shutdown_all();
    }
}

static void test_receive_rules(void)
{
    boot_online(&(xiaotai_transport_config_t){.format_error_ends_link = true});
    observation_t o = {.profile = profile()};
    open_link_with(&o, 10);
    xiaotai_mem_host_clear();
    frame_in(10, XIAOTAI_LINK_AUDIO, alaw, 2);
    xiaotai_transport_host_media_hook(frame_in_on_subscribe, NULL);
    xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE, XIAOTAI_FAIL, 0, 1);
    CHECK(xiaotai_link_receive(o.generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_FAIL);
    CHECK(fake_media_record().calls == 1); /* First frame while request is still in progress. */
    frame_in(10, XIAOTAI_LINK_AUDIO, alaw, 2);
    CHECK(fake_media_record().calls == 1); /* Failed request revoked receiver. */
    CHECK(xiaotai_link_receive(o.generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_link_receive(o.generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == 2);
    const fake_media_record_t r = fake_media_record();
    CHECK(r.calls == 2 && r.generation == o.generation && r.frame.encoding == opus.codec &&
          r.frame.rate_hz == 16000 && r.frame.timestamp_ms == 415 && r.frame.stream == 2 &&
          r.frame.size == 80 && r.first_byte == 31);
    for (unsigned i = 2; i < 100; ++i) frame_in(10, XIAOTAI_LINK_AUDIO, alaw, 2);
    for (unsigned i = 2; i < 100; ++i) frame_in(10, XIAOTAI_LINK_AUDIO, alaw, 7);
    settle();
    CHECK(!o.closed && stats(&o).audio.receive_filtered == 98);
    const xiaotai_eventlog_topic_snapshot_t accepted = topic("downlink.accepted");
    const xiaotai_eventlog_topic_snapshot_t rejected = topic("downlink.rejected");
    CHECK(accepted.posts == 2 && accepted.values[6] == 100 && accepted.values[1] == o.generation);
    CHECK(rejected.posts == 4 && rejected.values[6] == 100 && rejected.values[7] == 2);
    fake_media_accept(false);
    frame_in(10, XIAOTAI_LINK_AUDIO, alaw, 2);
    CHECK(stats(&o).audio.receive_refused == 1);
    CHECK(xiaotai_link_receive(o.generation, XIAOTAI_LINK_VIDEO, refuse_frame, NULL) == XIAOTAI_OK);
    frame_in(10, XIAOTAI_LINK_VIDEO, (xiaotai_link_format_t){XIAOTAI_LINK_CODEC_JPEG, 0, 0}, 6);
    settle();
    CHECK(!o.closed && stats(&o).video.receive_filtered == 1);
    CHECK(xiaotai_mem_host_allocations() == 0);
    frame_in(10, XIAOTAI_LINK_AUDIO, (xiaotai_link_format_t){XIAOTAI_LINK_CODEC_OPUS, 8000, 1}, 2);
    settle();
    CHECK(o.closed == 1 && o.reason == XIAOTAI_LINK_END_FORMAT);
    shutdown_all();
}

static void retire_during_subscribe(void *ctx, xiaotai_transport_host_call_kind_t kind, uintptr_t conn, int64_t value)
{
    if (kind != XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) return;
    unsigned *hits = ctx;
    CHECK(++*hits == 1U && conn == 10U && value == (((int64_t)XIAOTAI_LINK_AUDIO << 16) | (2U << 8) | 1U));
    xiaotai_transport_host_closed(conn);
    const xiaotai_transport_status_t status = status_now();
    CHECK(!status.links_open && status.links_releasing == 1U);
}

static void test_subscribe_retired(bool backend_failed)
{
    boot_online(&(xiaotai_transport_config_t){.links = 1});
    observation_t old = {.profile = profile()}, next = {.profile = profile()};
    open_link_with(&old, 10);
    const unsigned requests = calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE);
    const unsigned disconnects = calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
    unsigned hits = 0;
    xiaotai_transport_host_media_hook(retire_during_subscribe, &hits);
    if (backend_failed) xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE, XIAOTAI_FAIL, 0, 1);
    const xiaotai_err_t result = xiaotai_link_receive(old.generation, XIAOTAI_LINK_AUDIO, NULL, NULL);
    xiaotai_transport_host_media_hook(NULL, NULL);
    settle();
    CHECK(hits == 1U && old.closed == 1U && old.reason == XIAOTAI_LINK_END_PEER);
    CHECK(!status_now().links_open && !status_now().links_releasing);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == requests + 1U &&
          calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == disconnects);
    open_link_with(&next, 10);
    CHECK(next.generation != old.generation);
    CHECK(xiaotai_link_receive(old.generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == requests + 1U);
    frame_in(10, XIAOTAI_LINK_AUDIO, alaw, 2);
    CHECK(!fake_media_record().calls && stats(&next).audio.receive_closed == 1U);
    CHECK(xiaotai_link_receive(next.generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
    frame_in(10, XIAOTAI_LINK_AUDIO, alaw, 2);
    CHECK(fake_media_record().calls == 1U && fake_media_record().generation == next.generation);
    shutdown_all();
    const xiaotai_err_t expected = backend_failed ? XIAOTAI_FAIL : XIAOTAI_ERR_INVALID_STATE;
    fprintf(stderr, "subscribe returned %d after synchronous retirement; expected %d\n", (int)result, (int)expected);
    CHECK(result == expected);
}

static void stop_during_subscribe(void *ctx, xiaotai_transport_host_call_kind_t kind, uintptr_t conn, int64_t value)
{
    if (kind != XIAOTAI_TRANSPORT_HOST_SUBSCRIBE || !(value & 1)) return;
    CHECK(conn == 10U);
    xiaotai_link_receive_stop(*(const uint32_t *)ctx, XIAOTAI_LINK_AUDIO);
}

static bool replacement_receiver(void *ctx, uint32_t generation, const xiaotai_link_frame_t *frame,
                                 const uint8_t *data, size_t size)
{
    CHECK(generation != 0U && frame->kind == XIAOTAI_LINK_AUDIO && frame->stream == 2U && data && size);
    ++*(unsigned *)ctx;
    return true;
}

static void test_subscribe_superseded_by_stop(void)
{
    boot_online(&(xiaotai_transport_config_t){.links = 1});
    observation_t o = {.profile = profile()};
    open_link_with(&o, 10);
    const unsigned requests = calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE);
    unsigned original = 0, replacement = 0;
    xiaotai_transport_host_media_hook(stop_during_subscribe, &o.generation);
    CHECK(xiaotai_link_receive(o.generation, XIAOTAI_LINK_AUDIO, replacement_receiver, &original) ==
          XIAOTAI_ERR_INVALID_STATE);
    xiaotai_transport_host_media_hook(NULL, NULL);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == requests + 2U); /* True, then deferred false. */
    CHECK(status_now().links_open == 1U && !o.closed);
    frame_in(10, XIAOTAI_LINK_AUDIO, alaw, 2);
    CHECK(!original && !replacement && stats(&o).audio.receive_closed == 1U);
    CHECK(xiaotai_link_receive(o.generation, XIAOTAI_LINK_AUDIO, replacement_receiver, &replacement) == XIAOTAI_OK);
    frame_in(10, XIAOTAI_LINK_AUDIO, alaw, 2);
    CHECK(!original && replacement == 1U && calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == requests + 3U);
    shutdown_all();
}

static bool see_jpeg(void *arg, uint32_t gen, const xiaotai_link_frame_t *frame, const uint8_t *data, size_t size)
{
    const observation_t *o = arg;
    CHECK(o->generation == gen && frame->kind == XIAOTAI_LINK_VIDEO &&
          frame->format.codec == XIAOTAI_LINK_CODEC_JPEG && !frame->format.rate_hz &&
          !frame->format.channels && size == 6814 && data[0] == 255);
    return true;
}

static void test_receive_video_peer_streams(bool accept_peer)
{
    static const uint8_t data[6814] = {0xff, 0xd8, [6812] = 0xff, [6813] = 0xd9};
    static const uint8_t audio40[320] = {0xaa};
    const xiaotai_link_format_t jpeg = {XIAOTAI_LINK_CODEC_JPEG, 0, 0};
    xiaotai_link_frame_t peer = {.kind = XIAOTAI_LINK_VIDEO, .format = jpeg, .stream = 1};
    boot_online(&(xiaotai_transport_config_t){0});
    observation_t wx = {.profile = profile()};
    wx.profile.audio = (xiaotai_link_media_t){.send = true, .send_stream = 10, .send_format = alaw,
        .receive = true, .receive_stream = XIAOTAI_LINK_STREAM_ANY, .receive_formats = {alaw}};
    wx.profile.video = (xiaotai_link_media_t){.send = true, .send_stream = 11, .send_format = h264,
        .receive = true, .receive_stream = 11, .receive_request = true, .accept_peer_stream = accept_peer,
        .receive_formats = {jpeg}};
    open_link_with(&wx, 10);
    xiaotai_transport_host_frame(10, &peer, data, sizeof(data));
    CHECK(stats(&wx).video.receive_closed == 1);
    CHECK(xiaotai_link_receive(wx.generation, XIAOTAI_LINK_VIDEO, see_jpeg, &wx) == XIAOTAI_OK);
    CHECK(xiaotai_link_receive(wx.generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
    const xiaotai_link_frame_t voice = {.kind = XIAOTAI_LINK_AUDIO, .format = alaw, .stream = 0};
    xiaotai_transport_host_frame(10, &voice, audio40, sizeof(audio40));
    CHECK(fake_media_record().calls == 1 && fake_media_record().frame.size == sizeof(audio40));

    const uint32_t expected = accept_peer ? 1U : 0U;
    xiaotai_transport_host_frame(99, &peer, data, sizeof(data));
    CHECK(!stats(&wx).video.received);
    xiaotai_transport_host_frame(10, &peer, data, sizeof(data));
    CHECK(stats(&wx).video.received == expected);
    peer.stream = 11;
    xiaotai_transport_host_frame(10, &peer, data, sizeof(data));
    peer.stream = 16;
    xiaotai_transport_host_frame(10, &peer, data, sizeof(data));
    peer.stream = 1;
    peer.format = h264;
    xiaotai_transport_host_frame(10, &peer, data, sizeof(data));
    CHECK(stats(&wx).video.received == expected + 1U && stats(&wx).video.receive_filtered == 3U - expected);

    CHECK(xiaotai_link_permit(wx.generation, XIAOTAI_LINK_VIDEO, true) == XIAOTAI_OK);
    const xiaotai_link_video_frame_t up = {.codec = XIAOTAI_LINK_CODEC_H264, .key_frame = true,
        .data = audio, .size = sizeof(audio)};
    CHECK(xiaotai_link_send_video(wx.generation, &up) == XIAOTAI_OK);
    xiaotai_link_frame_t last;
    CHECK(xiaotai_transport_host_last_frame(&last) && last.stream == 11 && last.format.codec == h264.codec);

    xiaotai_link_receive_stop(wx.generation, XIAOTAI_LINK_VIDEO);
    peer.format = jpeg;
    xiaotai_transport_host_frame(10, &peer, data, sizeof(data));
    CHECK(stats(&wx).video.receive_closed == 2 && stats(&wx).video.received == expected + 1U);
    xiaotai_transport_host_call_t history[64];
    const size_t count = xiaotai_transport_host_calls(history, sizeof(history) / sizeof(*history));
    CHECK(count <= sizeof(history) / sizeof(*history));
    unsigned requests = 0;
    for (size_t i = 0; i < count; ++i) {
        if (history[i].kind != XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) continue;
        CHECK(history[i].connection == 10);
        CHECK(history[i].value == ((XIAOTAI_LINK_VIDEO << 16) | (11 << 8) | (requests == 0)));
        ++requests;
    }
    CHECK(requests == 2);

    xiaotai_transport_host_closed(10);
    settle();
    CHECK(xiaotai_link_receive(wx.generation, XIAOTAI_LINK_VIDEO, see_jpeg, &wx) == XIAOTAI_ERR_INVALID_STATE);
    observation_t next = {.profile = wx.profile};
    open_link_with(&next, 11);
    CHECK(next.generation != wx.generation && xiaotai_link_receive(next.generation, XIAOTAI_LINK_VIDEO, see_jpeg, &next) == XIAOTAI_OK);
    xiaotai_transport_host_frame(10, &peer, data, sizeof(data));
    CHECK(!stats(&next).video.received);
    shutdown_all();
}

static bool block_receiver(void *arg, uint32_t gen, const xiaotai_link_frame_t *f, const uint8_t *data, size_t bytes)
{
    (void)gen; (void)f; (void)data; (void)bytes;
    pause_here(arg);
    return true;
}

static void *receive_thread(void *arg)
{
    frame_in(*(const uintptr_t *)arg, XIAOTAI_LINK_AUDIO, alaw, 2);
    return NULL;
}

static void test_receive_lease(void)
{
    boot_online(&(xiaotai_transport_config_t){.links = 2});
    observation_t a = {.profile = profile()}, b = {.profile = profile()}, recycled = {.profile = profile()};
    const uintptr_t conn = 10;
    open_link_with(&a, conn);
    open_link_with(&b, 11);
    pause_t pause;
    pause_init(&pause);
    CHECK(xiaotai_link_receive(a.generation, XIAOTAI_LINK_AUDIO, block_receiver, &pause) == XIAOTAI_OK);
    CHECK(xiaotai_link_receive(b.generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_link_receive(b.generation, XIAOTAI_LINK_VIDEO, refuse_frame, NULL) == XIAOTAI_OK);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, receive_thread, (void *)&conn) == 0);
    CHECK(xiaotai_signal_take(&pause.entered, 3000));
    frame_in(11, XIAOTAI_LINK_AUDIO, alaw, 2);
    CHECK(!fake_media_record().calls && stats(&b).audio.receive_overlapped == 1);
    frame_in(11, XIAOTAI_LINK_VIDEO, h264, 6);
    CHECK(stats(&b).video.receive_refused == 1);
    xiaotai_transport_host_closed(conn);
    settle();
    CHECK(!a.closed && status_now().links_releasing == 1);
    xiaotai_signal_give(&pause.resume);
    CHECK(pthread_join(thread, NULL) == 0);
    settle();
    CHECK(a.closed == 1 && status_now().links_releasing == 0);
    open_link_with(&recycled, conn);
    CHECK(recycled.generation != a.generation);
    CHECK(xiaotai_link_receive(a.generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    frame_in(conn, XIAOTAI_LINK_AUDIO, alaw, 2);
    CHECK(!fake_media_record().calls && stats(&recycled).audio.receive_closed == 1);
    xiaotai_link_receive_stop(b.generation, XIAOTAI_LINK_AUDIO);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == 4);
    frame_in(11, XIAOTAI_LINK_AUDIO, alaw, 2);
    CHECK(!fake_media_record().calls && stats(&b).audio.receive_closed == 1);
    pause_free(&pause);
    shutdown_all();
}

static struct {
    uint32_t listener, other, old, next;
    char order[12];
    size_t count;
} handoff;

static void handoff_note(char event)
{
    CHECK(xiaotai_on_owner() && handoff.count + 1 < sizeof(handoff.order));
    handoff.order[handoff.count++] = event;
}

static void handoff_open(void *ctx, uint32_t generation)
{
    if (ctx) handoff_note('U');
    else if (!handoff.old) {
        handoff.old = generation;
        handoff_note('A');
    } else {
        CHECK(!handoff.next && handoff.old != generation);
        handoff.next = generation;
        handoff_note('B');
    }
}

static void handoff_close(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    (void)ctx;
    CHECK(reason == XIAOTAI_LINK_END_PEER && !detail);
    CHECK(generation == handoff.old || generation == handoff.next);
    handoff_note(generation == handoff.old ? 'C' : 'D');
}

static void handoff_command(void *ctx, uint32_t generation, const xiaotai_link_command_t *command)
{
    (void)ctx;
    CHECK(generation == handoff.old && !command->dropped && command->size == 3 && command->data);
    if (command->word == 0x2100) {
        CHECK(command->data[0] == 1 && command->data[2] == 3);
        handoff_note('1');
    } else {
        CHECK(command->word == 0x2101 && command->data[0] == 9 && command->data[2] == 7);
        handoff_note('2');
    }
}

static void handoff_register(void)
{
    xiaotai_link_listener_t listener = {.name = "handoff", .profile = profile(),
        .handlers = {.opened = handoff_open, .closed = handoff_close, .command = handoff_command}};
    listener.profile.command_max_bytes = listener.profile.command_pool_bytes = 16;
    CHECK(xiaotai_link_listen(&listener, &handoff.listener) == XIAOTAI_OK);
    CHECK(xiaotai_link_listen_set(handoff.listener, XIAOTAI_LINK_LISTEN_OPEN) == XIAOTAI_OK);
    const xiaotai_link_listener_t other = {.name = "independent", .handlers = {.opened = handoff_open, .ctx = &handoff}};
    CHECK(xiaotai_link_listen(&other, &handoff.other) == XIAOTAI_OK);
}

static void handoff_start(unsigned slots)
{
    memset(&handoff, 0, sizeof(handoff));
    g_observer_start = handoff_register;
    boot_online(&(xiaotai_transport_config_t){.links = slots, .command_pool_bytes = 16,
        .command_pool_mode = XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND});
    xiaotai_transport_host_accepted(10);
    settle();
    CHECK(handoff.old && !strcmp(handoff.order, "A"));
}

static void handoff_arm_other(void *unused)
{
    (void)unused;
    CHECK(xiaotai_link_listen_set(handoff.other, XIAOTAI_LINK_LISTEN_ONCE) == XIAOTAI_OK);
}

static void handoff_stop_old(void *unused)
{
    (void)unused;
    xiaotai_link_disconnect(handoff.old);
}

static void test_handoff_commands(bool cancel_old)
{
    handoff_start(cancel_old ? 2U : 3U);
    copy_fixture_t copy = {.bytes = 3};
    pause_init(&copy.pause);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, waiting_command, &copy) == 0);
    CHECK(xiaotai_signal_take(&copy.pause.entered, 3000));
    uint8_t second[] = {9, 8, 7};
    xiaotai_transport_host_command(10, 0, 0x2101, second, sizeof(second));
    memset(second, 0, sizeof(second));
    xiaotai_transport_host_closed(10);
    xiaotai_transport_host_accepted(11);
    settle();
    CHECK(!strcmp(handoff.order, "A") && !calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT));
    if (cancel_old) {
        CHECK(xiaotai_run(handoff_stop_old, NULL) == XIAOTAI_OK);
        settle();
        CHECK(!strcmp(handoff.order, "AB"));
    } else {
        CHECK(xiaotai_run(handoff_arm_other, NULL) == XIAOTAI_OK);
        settle();
        xiaotai_transport_host_accepted(12);
        settle();
        CHECK(!strcmp(handoff.order, "AU")); /* Cross-listener OPENED is not deferred. */
    }
    xiaotai_signal_give(&copy.pause.resume);
    CHECK(pthread_join(thread, NULL) == 0);
    settle();
    CHECK(!strcmp(handoff.order, cancel_old ? "AB" : "AU12CB"));
    CHECK(!xt.event_count && !xt.command_retired_bytes);
    pause_free(&copy.pause);
    shutdown_all();
}

static void test_handoff_rx(bool reused)
{
    handoff_start(2U);
    const uintptr_t old = 10;
    pause_t pause;
    pause_init(&pause);
    CHECK(xiaotai_link_receive(handoff.old, XIAOTAI_LINK_AUDIO, block_receiver, &pause) == XIAOTAI_OK);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, receive_thread, (void *)&old) == 0);
    CHECK(xiaotai_signal_take(&pause.entered, 3000));
    xiaotai_transport_host_closed(old);
    settle();
    CHECK(status_now().links_releasing == 1); /* Confirmed old handle still pinned by the receive. */
    const uintptr_t next = reused ? old : 11;
    xiaotai_transport_host_accepted(next);
    settle();
    CHECK(!strcmp(handoff.order, "A") && status_now().links_open == 1);
    if (reused) {
        xiaotai_transport_host_closed(next);
        settle();
        CHECK(!status_now().links_open && status_now().links_releasing == 1);
    }
    xiaotai_signal_give(&pause.resume);
    CHECK(pthread_join(thread, NULL) == 0);
    settle();
    CHECK(!strcmp(handoff.order, reused ? "AC" : "ACB"));
    CHECK(!status_now().links_releasing && !calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT));
    if (!reused) {
        xiaotai_transport_host_closed(next);
        settle();
        CHECK(!strcmp(handoff.order, "ACBD"));
    }
    pause_free(&pause);
    shutdown_all();
}

static void test_audio_profiles(void)
{
    static const struct {
        uint8_t send, receive, accepted_stream;
        bool explicit_subscription, peer_opens_tx, ptt;
        xiaotai_link_peer_stream_t peer_streams;
    } cases[] = {
        {10, 14, 14, true, true, false, XIAOTAI_LINK_PEER_STREAM_REFUSE}, // web stream
        {1, XIAOTAI_LINK_STREAM_ANY, 5, false, false, false, XIAOTAI_LINK_PEER_STREAM_REFUSE}, // AI
        {1, 1, 1, false, false, true, XIAOTAI_LINK_PEER_STREAM_REFUSE}, // room
        {10, 10, 10, true, false, false, XIAOTAI_LINK_PEER_STREAM_REFUSE}, // device call
        {10, XIAOTAI_LINK_STREAM_ANY, 6, false, false, false, XIAOTAI_LINK_PEER_STREAM_FOLLOW}, // WeChat
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        boot_online(NULL);
        observation_t o = {.profile.audio = {.send = true, .receive = true,
            .send_stream = cases[i].send, .send_format = alaw, .peer_streams = cases[i].peer_streams,
            .receive_stream = cases[i].receive, .receive_request = cases[i].explicit_subscription,
            .receive_formats = {alaw}}};
        open_link_with(&o, 71);
        const uint32_t gen = o.generation;
        CHECK(xiaotai_link_send_audio(gen, 123, audio, sizeof(audio)) == XIAOTAI_ERR_INVALID_STATE);
        CHECK(!calls(XIAOTAI_TRANSPORT_HOST_SEND));
        if (cases[i].peer_opens_tx) {
            CHECK(xiaotai_transport_host_peer_subscribe(71, 0, XIAOTAI_LINK_AUDIO, 4, true) ==
                  XIAOTAI_ERR_NOT_SUPPORTED);
            CHECK(xiaotai_transport_host_peer_subscribe(71, 0, XIAOTAI_LINK_AUDIO, cases[i].send, true) == XIAOTAI_OK);
        } else CHECK(xiaotai_link_permit(gen, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
        CHECK(xiaotai_link_send_audio(gen, 123, audio, sizeof(audio) - 1) == XIAOTAI_ERR_INVALID_SIZE);
        CHECK(xiaotai_link_send_audio(gen, 123, audio, sizeof(audio)) == XIAOTAI_OK);
        xiaotai_link_frame_t sent;
        CHECK(xiaotai_transport_host_last_frame(&sent) && sent.stream == cases[i].send && sent.timestamp_ms == 123 &&
              sent.kind == XIAOTAI_LINK_AUDIO && sent.format.codec == alaw.codec &&
              sent.format.rate_hz == alaw.rate_hz && sent.format.channels == alaw.channels);
        if (cases[i].ptt) {
            CHECK(xiaotai_link_permit(gen, XIAOTAI_LINK_AUDIO, false) == XIAOTAI_OK);
            CHECK(xiaotai_link_send_audio(gen, 123, audio, sizeof(audio)) == XIAOTAI_ERR_INVALID_STATE);
        }
        CHECK(xiaotai_link_receive(gen, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == (unsigned)cases[i].explicit_subscription);
        frame_in(72, XIAOTAI_LINK_AUDIO, alaw, cases[i].accepted_stream);
        frame_in(71, XIAOTAI_LINK_AUDIO, opus, cases[i].accepted_stream);
        frame_in(71, XIAOTAI_LINK_AUDIO, (xiaotai_link_format_t){XIAOTAI_LINK_CODEC_G711A, 16000, 1},
                 cases[i].accepted_stream);
        frame_in(71, XIAOTAI_LINK_AUDIO, (xiaotai_link_format_t){XIAOTAI_LINK_CODEC_G711A, 8000, 2},
                 cases[i].accepted_stream);
        const bool fixed = cases[i].receive != XIAOTAI_LINK_STREAM_ANY;
        if (fixed) frame_in(71, XIAOTAI_LINK_AUDIO, alaw, 7);
        CHECK(!fake_media_record().calls && stats(&o).audio.receive_filtered == 3U + fixed);
        frame_in(71, XIAOTAI_LINK_AUDIO, alaw, cases[i].accepted_stream);
        const fake_media_record_t got = fake_media_record();
        CHECK(got.calls == 1 && got.generation == gen && got.frame.encoding == alaw.codec &&
              got.frame.rate_hz == alaw.rate_hz && got.frame.stream == cases[i].accepted_stream &&
              got.frame.timestamp_ms == 415 &&
              got.frame.size == 80 && got.first_byte == 31);
        shutdown_all();
    }
}

static void test_video(void)
{
    external_ram = true;
    boot_online(&(xiaotai_transport_config_t){.links = 2, .audio_failures_end = 1});
    observation_t a = {.profile = profile()}, b = {.profile = profile()};
    b.profile.video.send_format = h264;
    open_link_with(&a, 10);
    open_link_with(&b, 11);
    const uint32_t gen = a.generation;
    xiaotai_link_video_frame_t f = {.codec = XIAOTAI_LINK_CODEC_JPEG, .key_frame = false, .timestamp_ms = 22,
        .data = audio, .size = sizeof(audio), .backlog_limit_bytes = 32768};
    CHECK(xiaotai_link_send_video(gen, NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_link_send_video(gen, &(xiaotai_link_video_frame_t){.size = sizeof(audio)}) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_link_send_video(gen, &(xiaotai_link_video_frame_t){.data = audio}) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_link_send_video(gen, &f) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_transport_host_peer_subscribe(99, 0, XIAOTAI_LINK_VIDEO, 8, true) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_VIDEO, 7, true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_VIDEO, 8, true) == XIAOTAI_OK);
    CHECK(xiaotai_link_take_key_frame_request(gen) && !xiaotai_link_take_key_frame_request(gen));
    CHECK(xiaotai_link_send_video(gen, &f) == XIAOTAI_ERR_INVALID_STATE);
    f.key_frame = true;
    xiaotai_transport_host_backlog_set(0);
    CHECK(xiaotai_transport_host_backlog_for(10, 32768) == XIAOTAI_OK);
    CHECK(xiaotai_transport_host_backlog_for(11, 32767) == XIAOTAI_OK);
    CHECK(xiaotai_link_backlog(gen) == 32768 && xiaotai_link_backlog(b.generation) == 32767);
    CHECK(xiaotai_link_send_video(gen, &f) == XIAOTAI_ERR_BUSY);
    CHECK(xiaotai_transport_host_backlog_for(10, 32767) == XIAOTAI_OK);
    CHECK(xiaotai_link_send_video(gen, &f) == XIAOTAI_OK);
    xiaotai_link_frame_t last;
    CHECK(xiaotai_transport_host_last_frame(&last) && last.stream == 8 && last.timestamp_ms == 22 &&
          last.format.codec == XIAOTAI_LINK_CODEC_JPEG && last.key_frame);
    CHECK(xiaotai_link_permit(b.generation, XIAOTAI_LINK_VIDEO, true) == XIAOTAI_OK);
    CHECK(xiaotai_link_send_video(b.generation, &f) == XIAOTAI_ERR_NOT_SUPPORTED);
    f.codec = XIAOTAI_LINK_CODEC_H264;
    CHECK(xiaotai_link_send_video(gen, &f) == XIAOTAI_OK);
    CHECK(xiaotai_link_send_video(b.generation, &f) == XIAOTAI_OK);
    xiaotai_transport_host_key_frame(10, 3);
    CHECK(xiaotai_link_take_key_frame_request(gen)); /* The prior BUSY asked for one. */
    CHECK(!xiaotai_link_take_key_frame_request(gen));
    xiaotai_transport_host_key_frame(10, 8);
    CHECK(xiaotai_link_take_key_frame_request(gen));
    CHECK(xiaotai_link_request_key_frame(gen) == XIAOTAI_OK);
    xiaotai_transport_host_bitrate(10, 8, 500);
    uint32_t bps = 0;
    CHECK(!xiaotai_link_take_video_bitrate(gen, &bps));
    CHECK(xiaotai_link_set_video_bitrate(gen, 100, 100, 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_link_set_video_bitrate(gen, 100, 200, 1000) == XIAOTAI_OK);
    xiaotai_transport_host_bitrate(10, 8, 400);
    xiaotai_transport_host_bitrate(10, 8, 500);
    xiaotai_transport_host_bitrate(10, 7, 900);
    CHECK(xiaotai_link_take_video_bitrate(gen, &bps) && bps == 500);
    CHECK(!xiaotai_link_take_video_bitrate(gen, &bps));
    xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SEND, XIAOTAI_FAIL, -18, 1);
    CHECK(xiaotai_link_send_video(gen, &f) == XIAOTAI_FAIL);
    settle();
    CHECK(!a.closed && stats(&a).video.send_failed == 1);
    CHECK(xiaotai_link_take_key_frame_request(gen));
    f.key_frame = false;
    CHECK(xiaotai_link_send_video(gen, &f) == XIAOTAI_ERR_INVALID_STATE);
    f.key_frame = true;
    CHECK(xiaotai_link_send_video(gen, &f) == XIAOTAI_OK);

    hook_t hook = {.kind = XIAOTAI_TRANSPORT_HOST_SEND, .blocked_conn = 10};
    pause_init(&hook.pause);
    xiaotai_transport_host_media_hook(block_media, &hook);
    CHECK(xiaotai_link_permit(gen, XIAOTAI_LINK_AUDIO, true) == XIAOTAI_OK);
    pthread_t thread;
    tx_t tx = {.gen = gen};
    CHECK(pthread_create(&thread, NULL, send_thread, &tx) == 0);
    CHECK(xiaotai_signal_take(&hook.pause.entered, 3000));
    CHECK(xiaotai_link_send_video(b.generation, &f) == XIAOTAI_OK);
    xiaotai_signal_give(&hook.pause.resume);
    CHECK(pthread_join(thread, NULL) == 0 && tx.result == XIAOTAI_OK);
    pause_free(&hook.pause);
    CHECK(xiaotai_run(disconnect_on_owner, &a) == XIAOTAI_OK);
    settle();
    CHECK(xiaotai_link_send_video(gen, &f) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!xiaotai_link_take_key_frame_request(gen) && !xiaotai_link_take_video_bitrate(gen, &bps));
    shutdown_all();
    external_ram = false;
}

typedef struct {
    uint32_t generation;
    const xiaotai_link_video_frame_t *frame;
    xiaotai_err_t result;
} h5_send_t;

static void *send_h5_video(void *arg)
{
    h5_send_t *send = arg;
    send->result = xiaotai_link_send_video(send->generation, send->frame);
    return NULL;
}

/* The shared S3 H5 adapter used these stream numbers and this exact JPEG
 * input in tests/native/cores3/test_h5_video_send.py. */
static void test_h5_jpeg(void)
{
    enum { UP_AUDIO = 10, UP_VIDEO = 11, DOWN_AUDIO = 14, BACKLOG = 32U * 1024U };
    static const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
    const xiaotai_link_video_frame_t frame = {
        .codec = XIAOTAI_LINK_CODEC_JPEG, .key_frame = true, .timestamp_ms = 123,
        .data = jpeg, .size = sizeof(jpeg), .backlog_limit_bytes = BACKLOG,
    };
    observation_t h5 = {.profile = {
        .audio = {.send = true, .send_stream = UP_AUDIO, .send_format = alaw, .receive = true,
            .receive_stream = DOWN_AUDIO, .receive_request = true, .receive_formats = {alaw}},
        .video = {.send = true, .send_stream = UP_VIDEO, .send_format = {XIAOTAI_LINK_CODEC_JPEG, 0, 0}},
    }};
    external_ram = true;
    boot_online(&(xiaotai_transport_config_t){.links = 1});
    open_link_with(&h5, 10);
    const uint32_t gen = h5.generation;

    CHECK(xiaotai_link_receive(0, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_link_receive(gen + 1U, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE));
    xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE, XIAOTAI_ERR_BUSY, 0, 1);
    CHECK(xiaotai_link_receive(gen, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_ERR_BUSY);
    CHECK(xiaotai_link_receive(gen, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_link_receive(gen, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_OK);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == 2U);
    xiaotai_transport_host_call_t history[32];
    const size_t n = xiaotai_transport_host_calls(history, sizeof(history) / sizeof(*history));
    CHECK(n && n < sizeof(history) / sizeof(*history));
    CHECK(history[n - 1U].kind == XIAOTAI_TRANSPORT_HOST_SUBSCRIBE &&
          history[n - 1U].connection == 10 &&
          history[n - 1U].value == (((int64_t)XIAOTAI_LINK_AUDIO << 16) | (DOWN_AUDIO << 8) | 1));

    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_AUDIO, UP_AUDIO, true) == XIAOTAI_OK);
    CHECK(!xiaotai_link_can_send(gen, XIAOTAI_LINK_VIDEO));
    CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_VIDEO, UP_AUDIO, true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_transport_host_peer_subscribe(11, 0, XIAOTAI_LINK_VIDEO, UP_VIDEO, true) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!xiaotai_link_can_send(gen, XIAOTAI_LINK_VIDEO));
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_VIDEO, UP_VIDEO, true) == XIAOTAI_OK);
    CHECK(xiaotai_link_can_send(gen, XIAOTAI_LINK_VIDEO));
    CHECK(xiaotai_link_send_video(0, &frame) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_link_send_video(gen + 1U, &frame) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_link_video_frame_t bad = frame;
    bad.data = NULL;
    CHECK(xiaotai_link_send_video(gen, &bad) == XIAOTAI_ERR_INVALID_ARG);
    bad.data = jpeg;
    bad.size = 0;
    CHECK(xiaotai_link_send_video(gen, &bad) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(!calls(XIAOTAI_TRANSPORT_HOST_BACKLOG) && !calls(XIAOTAI_TRANSPORT_HOST_SEND));

    CHECK(xiaotai_transport_host_backlog_for(10, BACKLOG) == XIAOTAI_OK);
    CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_BUSY);
    CHECK(!calls(XIAOTAI_TRANSPORT_HOST_SEND));
    CHECK(xiaotai_transport_host_backlog_for(10, BACKLOG - 1U) == XIAOTAI_OK);
    CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_OK);
    xiaotai_link_frame_t sent;
    CHECK(xiaotai_transport_host_last_frame(&sent) && sent.kind == XIAOTAI_LINK_VIDEO &&
          sent.stream == UP_VIDEO && sent.format.codec == XIAOTAI_LINK_CODEC_JPEG &&
          sent.key_frame && sent.timestamp_ms == 123);
    xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SEND, XIAOTAI_ERR_BUSY, 0, 1);
    CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_BUSY);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SEND) == 2U);

    hook_t hook = {.kind = XIAOTAI_TRANSPORT_HOST_SEND, .blocked_conn = 10};
    pause_init(&hook.pause);
    xiaotai_transport_host_media_hook(block_media, &hook);
    pthread_t sender;
    h5_send_t pending = {.generation = gen, .frame = &frame};
    CHECK(pthread_create(&sender, NULL, send_h5_video, &pending) == 0);
    CHECK(xiaotai_signal_take(&hook.pause.entered, 3000));
    CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_BUSY);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SEND) == 3U);
    CHECK(xiaotai_run(disconnect_on_owner, &h5) == XIAOTAI_OK);
    settle();
    CHECK(!xiaotai_link_can_send(gen, XIAOTAI_LINK_VIDEO) && !calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT));
    xiaotai_signal_give(&hook.pause.resume);
    CHECK(pthread_join(sender, NULL) == 0 && pending.result == XIAOTAI_OK);
    xiaotai_transport_host_media_hook(NULL, NULL);
    pause_free(&hook.pause);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1U);
    CHECK(xiaotai_run(disconnect_on_owner, &h5) == XIAOTAI_OK);
    settle();
    const unsigned backlog_queries = calls(XIAOTAI_TRANSPORT_HOST_BACKLOG);
    CHECK(xiaotai_link_receive(gen, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SEND) == 3U && calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == 2U &&
          calls(XIAOTAI_TRANSPORT_HOST_BACKLOG) == backlog_queries && calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1U);
    xiaotai_transport_host_closed(10);
    settle();

    /* Non-H5 modes use their own link profile, without an H5 video stream or
     * an explicit H5 audio request. It may reuse the old SDK handle. */
    observation_t other = {.profile = {.audio = {.send = true, .send_format = alaw}}};
    open_link_with(&other, 10);
    CHECK(other.generation != gen);
    CHECK(xiaotai_link_receive(other.generation, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_VIDEO, UP_VIDEO, true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_link_send_video(other.generation, &frame) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_SEND) == 3U && calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) == 2U &&
          calls(XIAOTAI_TRANSPORT_HOST_BACKLOG) == backlog_queries);
    shutdown_all();
    external_ram = false;
}

static void test_ai_jpeg(void)
{
    enum { BACKLOG = 32U * 1024U };
    static const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
    const xiaotai_link_video_frame_t frame = {
        .codec = XIAOTAI_LINK_CODEC_JPEG, .key_frame = true, .timestamp_ms = 123,
        .data = jpeg, .size = sizeof(jpeg), .backlog_limit_bytes = BACKLOG,
    };
    for (unsigned enabled = 0; enabled < 2U; ++enabled) {
        external_ram = true;
        boot_online(&(xiaotai_transport_config_t){.links = 1});
        observation_t ai = {.profile.audio = {.send = true, .send_stream = 1, .send_format = alaw}};
        if (enabled) ai.profile.video = (xiaotai_link_media_t){
            .send = true, .send_stream = 0, .send_format = {XIAOTAI_LINK_CODEC_JPEG, 0, 0},
        };
        open_link_with(&ai, 10);
        const uint32_t gen = ai.generation;

        CHECK(!xiaotai_link_can_send(gen, XIAOTAI_LINK_VIDEO));
        CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_INVALID_STATE);
        CHECK(xiaotai_transport_host_peer_subscribe(10, 0, XIAOTAI_LINK_VIDEO, 11, true) ==
              XIAOTAI_ERR_NOT_SUPPORTED);
        CHECK(!ai.subscriptions && !calls(XIAOTAI_TRANSPORT_HOST_BACKLOG) && !calls(XIAOTAI_TRANSPORT_HOST_SEND));
        CHECK(xiaotai_link_permit(gen, XIAOTAI_LINK_VIDEO, true) ==
              (enabled ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED));

        if (enabled) {
            /* An accepted AI session grants video directly: there is no H5 subscription. */
            CHECK(xiaotai_link_can_send(gen, XIAOTAI_LINK_VIDEO));
            CHECK(xiaotai_link_send_video(0, &frame) == XIAOTAI_ERR_INVALID_STATE);
            CHECK(xiaotai_link_send_video(gen + 1U, &frame) == XIAOTAI_ERR_INVALID_STATE);
            CHECK(!calls(XIAOTAI_TRANSPORT_HOST_BACKLOG) && !calls(XIAOTAI_TRANSPORT_HOST_SEND));
            CHECK(xiaotai_transport_host_backlog_for(10, BACKLOG) == XIAOTAI_OK);
            CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_BUSY);
            CHECK(!calls(XIAOTAI_TRANSPORT_HOST_SEND));
            CHECK(xiaotai_transport_host_backlog_for(10, BACKLOG - 1U) == XIAOTAI_OK);
            CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_OK);
            xiaotai_link_frame_t sent;
            CHECK(xiaotai_transport_host_last_frame(&sent) && sent.kind == XIAOTAI_LINK_VIDEO &&
                  sent.stream == 0 && sent.format.codec == XIAOTAI_LINK_CODEC_JPEG &&
                  sent.timestamp_ms == 123 && sent.key_frame);
            xiaotai_transport_host_media_fail(XIAOTAI_TRANSPORT_HOST_SEND, XIAOTAI_ERR_BUSY, 0, 1);
            CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_BUSY);
            CHECK(stats(&ai).video.sent == 1U && stats(&ai).video.send_congested == 2U);
            CHECK(xiaotai_link_permit(gen, XIAOTAI_LINK_VIDEO, false) == XIAOTAI_OK);
        }

        const unsigned sends = calls(XIAOTAI_TRANSPORT_HOST_SEND);
        const unsigned queries = calls(XIAOTAI_TRANSPORT_HOST_BACKLOG);
        CHECK(!xiaotai_link_can_send(gen, XIAOTAI_LINK_VIDEO));
        CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_INVALID_STATE);
        CHECK(xiaotai_run(disconnect_on_owner, &ai) == XIAOTAI_OK);
        settle();
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1U);
        CHECK(!xiaotai_link_can_send(gen, XIAOTAI_LINK_VIDEO));
        CHECK(xiaotai_link_send_video(gen, &frame) == XIAOTAI_ERR_INVALID_STATE);
        CHECK(!ai.subscriptions && !calls(XIAOTAI_TRANSPORT_HOST_SUBSCRIBE) &&
              calls(XIAOTAI_TRANSPORT_HOST_SEND) == sends && calls(XIAOTAI_TRANSPORT_HOST_BACKLOG) == queries);
        shutdown_all();
        external_ram = false;
    }
}

static void test_legacy_ai_command_words(void)
{
    const uint16_t command = 0x2100U;
    for (uint32_t value = 0; value <= UINT16_MAX; ++value) {
        const uint32_t sequence = value << 16;
        const uint32_t raw = sequence | command;
        const uint32_t packed = sequence | ((uint32_t)command << 1);
        CHECK(xiaotai_link_command_is(raw, command));
        CHECK(xiaotai_link_command_is(raw | 1U, command));
        CHECK(xiaotai_link_command_is(packed, command));
        CHECK(xiaotai_link_command_is(packed | 1U, command));

        /* Old starter matched either GET_CMD or the bare id without the reply bit. */
        const bool legacy = (value >> 1) == command || (value & ~1U) == command;
        CHECK(xiaotai_link_command_is(UINT32_C(0xa55a0000) | value, command) == legacy);
    }
}

static void test_worker_policy(void)
{
    const struct {
        bool external, delegated;
        xiaotai_transport_switch_t preference;
        uint32_t worker_stack_bytes;
    } cases[] = {
        {false, true, XIAOTAI_TRANSPORT_STACK_DEFAULT, 8192},
        {true, false, XIAOTAI_TRANSPORT_STACK_DEFAULT, 0},
        {false, false, XIAOTAI_TRANSPORT_OFF, 8192},
        {false, false, XIAOTAI_TRANSPORT_STACK_DEFAULT, 4096},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        external_ram = cases[i].external;
        boot_online(&(xiaotai_transport_config_t){.media_uplink_worker = cases[i].preference,
                    .worker_stack_bytes = cases[i].worker_stack_bytes});
        CHECK(xt.audio_executor_attached == cases[i].delegated);
        shutdown_all();
    }
    external_ram = false;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        test_send_rules();
        test_audio_failure_policy(0);
        test_audio_failure_policy(1);
        test_audio_failure_policy(3);
        test_send_lease();
        test_worker_send_close_boundary();
    } else if (!strcmp(argv[1], "audio_unsubscribe_permit")) {
        test_audio_unsubscribe_permit(true);
        test_audio_unsubscribe_permit(false);
    } else if (!strcmp(argv[1], "receive")) {
        test_receive_rules();
        test_pcma_opus_compatibility();
        test_receive_lease();
    } else if (!strcmp(argv[1], "subscribe_retired")) {
        test_subscribe_retired(false);
        test_subscribe_retired(true);
        test_subscribe_superseded_by_stop();
    } else if (!strcmp(argv[1], "receive_video_peer_streams")) {
        test_receive_video_peer_streams(false);
        test_receive_video_peer_streams(true);
    } else if (!strcmp(argv[1], "commands")) {
        test_legacy_ai_command_words();
        test_command_pool(XIAOTAI_TRANSPORT_COMMAND_POOL_RESIDENT);
        test_full_room_command_budget(XIAOTAI_TRANSPORT_COMMAND_POOL_RESIDENT);
        test_xmini_command_budget(XIAOTAI_TRANSPORT_COMMAND_POOL_RESIDENT);
        test_borrowed_owner_command(XIAOTAI_TRANSPORT_COMMAND_POOL_RESIDENT);
        test_control_lock();
    } else if (!strcmp(argv[1], "commands_on_demand")) {
        test_command_pool(XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND);
        test_full_room_command_budget(XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND);
        test_xmini_command_budget(XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND);
        test_demand_command_oom(false, false);
        test_demand_command_oom(true, false);
        test_borrowed_owner_command(XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND);
        test_demand_command_order(false, false, false);
        test_demand_command_order(true, false, false);
        test_demand_command_order(true, true, false);
    } else if (!strcmp(argv[1], "commands_hybrid")) {
        test_xmini_command_words();
        test_default_command_storage(true);
        test_default_command_storage(false);
        test_hybrid_reservation_unavailable();
        test_demand_command_oom(false, true);
        test_demand_command_oom(true, true);
        test_borrowed_owner_command(XIAOTAI_TRANSPORT_COMMAND_POOL_DEFAULT);
        test_demand_command_order(false, false, true);
        test_demand_command_order(true, false, true);
        test_demand_command_order(true, true, true);
    } else if (!strcmp(argv[1], "video")) {
        test_video();
    } else if (!strcmp(argv[1], "h5_jpeg")) {
        test_h5_jpeg();
    } else if (!strcmp(argv[1], "ai_jpeg")) {
        test_ai_jpeg();
    } else if (!strcmp(argv[1], "inbound_handoff")) {
        test_handoff_commands(false);
        test_handoff_commands(true);
        test_handoff_rx(false);
        test_handoff_rx(true);
    } else if (!strcmp(argv[1], "audio_profiles")) {
        test_audio_profiles();
    } else if (!strcmp(argv[1], "worker_reset")) {
        test_worker_reset_after_timeout();
    } else if (!strcmp(argv[1], "worker_policy")) {
        test_worker_policy();
    } else CHECK(false);
    xiaotai_eventlog_reset();
    return 0;
}
