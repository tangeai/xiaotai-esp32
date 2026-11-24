// SPDX-License-Identifier: MIT
// Replaces legacy: tests/mz28/test_rtc_inputs.c::rtc_inputs
#include "support.h"

typedef struct {
    xiaotai_link_request_t request;
    uint32_t generation;
    xiaotai_err_t result;
    unsigned opened, closed;
    xiaotai_link_end_t reason;
    int detail;
} session_t;

static void opened(void *ctx, uint32_t generation)
{
    session_t *s = ctx;
    CHECK(xiaotai_on_owner() && s->generation == generation);
    ++s->opened;
}

static void closed(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    session_t *s = ctx;
    CHECK(xiaotai_on_owner() && s->generation == generation);
    ++s->closed;
    s->reason = reason;
    s->detail = detail;
}

static void connect_owner(void *ctx)
{
    session_t *s = ctx;
    const xiaotai_link_handlers_t h = {.opened = opened, .closed = closed, .ctx = s};
    s->result = xiaotai_link_connect(&s->request, &h, &s->generation);
}

static void disconnect_owner(void *ctx)
{
    const session_t *s = ctx;
    xiaotai_link_disconnect(s->generation);
}

static void connect(session_t *s)
{
    CHECK(xiaotai_run(connect_owner, s) == XIAOTAI_OK);
    settle();
}

static void disconnect(session_t *s)
{
    CHECK(xiaotai_run(disconnect_owner, s) == XIAOTAI_OK);
    settle();
}

typedef struct {
    const char *peer, *token;
    uint32_t request;
    xiaotai_link_kind_t kind;
    uintptr_t inline_conn;
} submit_t;

static void remember_submit(void *ctx, xiaotai_link_kind_t kind, uint32_t request,
                            const char *peer, const char *token)
{
    submit_t *seen = ctx;
    seen->peer = peer;
    seen->token = token;
    seen->request = request;
    seen->kind = kind;
    if (seen->inline_conn) {
        xiaotai_transport_host_connected(request, seen->inline_conn, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
        CHECK(*peer && *token); /* the downcall still owns these */
    }
}

static xiaotai_signal_t worker_entered, worker_resume;
static void stop_worker(uint32_t a, uint32_t b)
{
    (void)a;
    (void)b;
    xiaotai_signal_give(&worker_entered);
    CHECK(xiaotai_signal_take(&worker_resume, 3000));
}

static void input_lifetime(void)
{
    _Static_assert(XIAOTAI_LINK_PEER_MAX == 2048 && XIAOTAI_LINK_TOKEN_MAX == 1024, "review input boundaries");
    boot_online(NULL);
    const uint32_t before = status_now().engine_bytes;
    char peer[XIAOTAI_LINK_PEER_MAX + 1], token[XIAOTAI_LINK_TOKEN_MAX + 1];
    memset(peer, 'p', sizeof(peer) - 1);
    memset(token, 't', sizeof(token) - 1);
    peer[sizeof(peer) - 1] = token[sizeof(token) - 1] = 0;
    session_t rejected = {.request = {.kind = XIAOTAI_LINK_WHIP, .peer = peer, .token = "valid"}};
    connect(&rejected);
    CHECK(rejected.result == XIAOTAI_ERR_INVALID_SIZE && !rejected.generation);
    rejected.request.peer = "valid";
    rejected.request.token = token;
    connect(&rejected);
    CHECK(rejected.result == XIAOTAI_ERR_INVALID_SIZE && !rejected.generation);
    rejected.request.peer = NULL;
    connect(&rejected);
    CHECK(rejected.result == XIAOTAI_ERR_INVALID_ARG && !calls(XIAOTAI_TRANSPORT_HOST_CONNECT));
    CHECK(status_now().engine_bytes == before);

    peer[XIAOTAI_LINK_PEER_MAX - 1] = token[XIAOTAI_LINK_TOKEN_MAX - 1] = 0;
    CHECK(xiaotai_signal_init(&worker_entered) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&worker_resume) == XIAOTAI_OK);
    CHECK(xt_worker_post(stop_worker, 0, 0) && xiaotai_signal_take(&worker_entered, 2000));
    session_t s = {.request = {.kind = XIAOTAI_LINK_WHIP, .peer = peer, .token = token}};
    submit_t seen = {0};
    xiaotai_transport_host_connect_hook(remember_submit, &seen);
    CHECK(xiaotai_run(connect_owner, &s) == XIAOTAI_OK);
    owner_sync();
    CHECK(s.result == XIAOTAI_OK && s.generation && !calls(XIAOTAI_TRANSPORT_HOST_CONNECT));
    memset(peer, 'x', XIAOTAI_LINK_PEER_MAX - 1);
    memset(token, 'x', XIAOTAI_LINK_TOKEN_MAX - 1);
    xiaotai_signal_give(&worker_resume);
    settle();
    CHECK(seen.kind == XIAOTAI_LINK_WHIP && seen.request == s.generation && seen.peer != peer && seen.token != token);
    CHECK(status_now().engine_bytes >= before + XIAOTAI_LINK_PEER_MAX + XIAOTAI_LINK_TOKEN_MAX);
    CHECK(strlen(seen.peer) == XIAOTAI_LINK_PEER_MAX - 1 && strlen(seen.token) == XIAOTAI_LINK_TOKEN_MAX - 1);
    CHECK(strspn(seen.peer, "p") == XIAOTAI_LINK_PEER_MAX - 1 && strspn(seen.token, "t") == XIAOTAI_LINK_TOKEN_MAX - 1);
    xiaotai_transport_host_connected(seen.request, 41, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    settle();
    CHECK(s.opened == 1 && !s.closed && seen.peer[0] == 'p');
    disconnect(&s);
    CHECK(status_now().links_releasing == 1 && seen.token[0] == 't');
    xiaotai_transport_host_closed(41);
    settle();
    CHECK(!status_now().links_releasing && status_now().engine_bytes == before);
    xiaotai_signal_deinit(&worker_resume);
    xiaotai_signal_deinit(&worker_entered);
    shutdown_all();
}

static void input_one_slot(void)
{
    const xiaotai_transport_config_t config = {.links = 1};
    boot_online(&config);
    submit_t seen = {0};
    xiaotai_transport_host_connect_hook(remember_submit, &seen);

    char peer[] = "whips://test?room=sample", token[] = "sample-token";
    session_t first = {.request = {.kind = XIAOTAI_LINK_WHIP, .peer = peer, .token = token}};
    connect(&first);
    CHECK(first.result == XIAOTAI_OK && !first.opened && seen.request == first.generation);
    CHECK(seen.peer != peer && seen.token != token && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1);
    memset(peer, 'x', sizeof(peer) - 1U);
    memset(token, 'x', sizeof(token) - 1U);

    session_t next = {.request = {.kind = XIAOTAI_LINK_WHIP, .peer = "whips://second", .token = "second-token"}};
    connect(&next);
    CHECK(next.result == XIAOTAI_ERR_BUSY && !next.generation);
    char oversized[XIAOTAI_LINK_PEER_MAX + 1U];
    memset(oversized, 'a', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = 0;
    session_t invalid = {.request = {.kind = XIAOTAI_LINK_WHIP, .peer = oversized, .token = "test"}};
    connect(&invalid);
    CHECK(invalid.result == XIAOTAI_ERR_INVALID_SIZE && !invalid.generation);
    invalid.request.peer = NULL;
    connect(&invalid);
    CHECK(invalid.result == XIAOTAI_ERR_INVALID_ARG && !invalid.generation);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1 &&
          !strcmp(seen.peer, "whips://test?room=sample") && !strcmp(seen.token, "sample-token"));

    xiaotai_transport_host_connected(seen.request, 51, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    settle();
    disconnect(&first);
    xiaotai_transport_host_closed(51);
    settle();
    connect(&next);
    CHECK(next.result == XIAOTAI_OK && seen.request == next.generation &&
          calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 2 &&
          !strcmp(seen.peer, "whips://second") && !strcmp(seen.token, "second-token"));
    shutdown_all();
}

static void link_results(void)
{
    boot_online(NULL);
    const uint32_t before = status_now().engine_bytes;
    const xiaotai_link_request_t request = {.kind = XIAOTAI_LINK_P2P, .peer = "peer", .token = "fixture"};
    submit_t seen = {0};
    xiaotai_transport_host_connect_hook(remember_submit, &seen);
    session_t failed = {.request = request};
    xiaotai_transport_host_fail_connect(XIAOTAI_FAIL, -713, 1);
    connect(&failed);
    CHECK(failed.result == XIAOTAI_OK && !failed.opened && failed.closed == 1 && !seen.request &&
          failed.reason == XIAOTAI_LINK_END_CONNECT_FAILED && failed.detail == -713);
    CHECK(status_now().engine_bytes == before);

    session_t timeout = {.request = request};
    connect(&timeout);
    CHECK(timeout.result == XIAOTAI_OK && !timeout.closed && seen.request == timeout.generation);
    xiaotai_transport_host_connected(seen.request, 0, XIAOTAI_LINK_END_TIMEOUT, -412);
    settle();
    CHECK(!timeout.opened && timeout.closed == 1 && timeout.reason == XIAOTAI_LINK_END_TIMEOUT &&
          timeout.detail == -412 && status_now().engine_bytes == before);

    session_t late = {.request = request};
    connect(&late);
    CHECK(late.result == XIAOTAI_OK && seen.request == late.generation);
    disconnect(&late);
    CHECK(status_now().links_releasing == 1 && !strcmp(seen.token, "fixture"));
    xiaotai_transport_host_connected(seen.request, 42, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    settle();
    CHECK(!late.opened && !late.closed && status_now().links_releasing == 1);
    xiaotai_transport_host_closed(42);
    settle();
    CHECK(status_now().engine_bytes == before);

    seen.inline_conn = 43;
    session_t inline_ok = {.request = request};
    connect(&inline_ok);
    CHECK(inline_ok.result == XIAOTAI_OK && inline_ok.opened == 1 && seen.kind == XIAOTAI_LINK_P2P &&
          !strcmp(seen.peer, "peer"));
    CHECK(xiaotai_transport_host_backlog_for(43, 33) == XIAOTAI_OK);
    for (unsigned i = 0; i < 256; ++i) CHECK(xiaotai_link_backlog(inline_ok.generation) == 33);
    advance(17);
    CHECK(xiaotai_link_backlog(inline_ok.generation) == 33);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_BACKLOG) == 257 &&
          xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_BACKLOG) == T0 + 17);
    xiaotai_transport_host_call_t records[32];
    CHECK(xiaotai_transport_host_calls(records, 32) > 256);
    bool saw_backlog = false;
    for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); ++i) {
        if (records[i].kind == XIAOTAI_TRANSPORT_HOST_BACKLOG) {
            CHECK(records[i].connection == 43 && records[i].at_ms == T0 && records[i].value == 33);
            saw_backlog = true;
        }
    }
    CHECK(saw_backlog);
    xiaotai_transport_host_error(43, XIAOTAI_LINK_END_NETWORK, -901);
    settle();
    CHECK(inline_ok.closed == 1 && inline_ok.reason == XIAOTAI_LINK_END_NETWORK && inline_ok.detail == -901);
    xiaotai_transport_host_closed(43);
    settle();
    CHECK(status_now().engine_bytes == before);
    shutdown_all();
}

static void release_input_budget(bool tolerate)
{
    const xiaotai_transport_config_t config = {
        .links = 1, .release_timeout_ms = 1000, .release_failures_tolerated = tolerate,
    };
    boot_online(&config);
    const uint32_t before = status_now().engine_bytes;
    const xiaotai_link_request_t request = {.kind = XIAOTAI_LINK_P2P, .peer = "peer", .token = "fixture"};
    session_t old = {.request = request}, next = {.request = request};
    connect(&old);
    CHECK(old.result == XIAOTAI_OK && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1);
    const uint32_t one = status_now().engine_bytes - before;
    CHECK(one > 0);
    disconnect(&old);
    CHECK(status_now().links_releasing == 1 && status_now().engine_bytes == before + one);
    if (!tolerate) {
        connect(&next);
        CHECK(next.result == XIAOTAI_OK && status_now().engine_bytes == before + 2 * one);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1);
        advance(999);
        CHECK(status_now().state == XIAOTAI_TRANSPORT_ONLINE && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1);
        advance(1);
        CHECK(status_now().state == XIAOTAI_TRANSPORT_FAULT && status_now().engine_bytes == before + one);
    } else {
        for (unsigned i = 0; i < 20; ++i) {
            CHECK(status_now().engine_bytes == before + one);
            advance(999);
            CHECK(status_now().links_releasing == 1 && status_now().engine_bytes == before + one);
            advance(1);
            CHECK(!status_now().links_releasing && status_now().engine_bytes == before);
            CHECK(status_now().state == XIAOTAI_TRANSPORT_ONLINE && status_now().release_stalls == i + 1);
            old = (session_t){.request = request};
            connect(&old);
            CHECK(old.result == XIAOTAI_OK && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == i + 2);
            disconnect(&old);
        }
        connect(&next);
        CHECK(next.result == XIAOTAI_OK && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 22);
        CHECK(status_now().engine_bytes == before + 2 * one);
        disconnect(&next); /* No free release record for this submitted request. */
        CHECK(status_now().state == XIAOTAI_TRANSPORT_FAULT);
        CHECK(status_now().engine_bytes == before + 2 * one);
    }
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1 && !calls(XIAOTAI_TRANSPORT_HOST_UNINIT));
    xiaotai_transport_host_stopped(0);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1 && status_now().engine_bytes == before);
    shutdown_all();
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "results")) link_results();
    else if (argc == 2 && !strcmp(argv[1], "one_slot")) input_one_slot();
    else if (argc == 2 && !strcmp(argv[1], "release_strict")) release_input_budget(false);
    else if (argc == 2 && !strcmp(argv[1], "release_tolerated")) release_input_budget(true);
    else input_lifetime();
}
