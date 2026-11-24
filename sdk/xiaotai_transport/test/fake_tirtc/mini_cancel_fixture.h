// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_rtc/test_rtc.c::rtc
/* Included by backend_test.c to share typed SDK doubles with the ABI tests. */
#include "../support.h"

/* Use a manual clock without pulling the scripted backend into this test. */
static _Atomic int64_t engine_clock;

int64_t xt_now_ms(void) { return atomic_load(&engine_clock); }
uint32_t xt_clock_wait_ms(int64_t delta_ms) { return delta_ms <= 0 ? 0 : XIAOTAI_WAIT_FOREVER; }
void xiaotai_transport_host_reset(void) { atomic_store(&engine_clock, T0); }
int64_t xiaotai_transport_host_clock(void) { return xt_now_ms(); }
void xiaotai_transport_host_clock_manual(int64_t start_ms) { atomic_store(&engine_clock, start_ms); }
void xiaotai_transport_host_clock_advance(uint32_t ms)
{
    atomic_fetch_add(&engine_clock, ms);
    xt_worker_kick();
}

bool xiaotai_transport_host_settle(uint32_t timeout_ms)
{
    const int64_t deadline = xiaotai_time_us() + (int64_t)timeout_ms * 1000;
    unsigned quiet = 0;
    while (xiaotai_time_us() < deadline) {
        quiet = xt_worker_settled() ? quiet + 1U : 0U;
        if (quiet == 3U) return true;
        xiaotai_delay_ms(1);
    }
    return false;
}

typedef struct {
    uint32_t generation, timeout_ms;
    xiaotai_link_kind_t kind;
    unsigned opened, closed;
    xiaotai_link_end_t reason;
} mini_call_t;

static void mini_opened(void *ctx, uint32_t generation)
{
    mini_call_t *call = ctx;
    CHECK(generation == call->generation);
    ++call->opened;
}

static void mini_closed(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    (void)detail;
    mini_call_t *call = ctx;
    CHECK(generation == call->generation);
    ++call->closed;
    call->reason = reason;
}

static void mini_connect(void *ctx)
{
    mini_call_t *call = ctx;
    const xiaotai_link_request_t request = {
        .kind = call->kind, .peer = "fixture-peer", .token = "fixture-token",
        .timeout_ms = call->timeout_ms,
    };
    const xiaotai_link_handlers_t handlers = {.ctx = call, .opened = mini_opened, .closed = mini_closed};
    CHECK(xiaotai_link_connect(&request, &handlers, &call->generation) == XIAOTAI_OK);
}

static void mini_disconnect(void *ctx) { xiaotai_link_disconnect(((mini_call_t *)ctx)->generation); }

static void owner_call(void (*fn)(void *), mini_call_t *call)
{
    CHECK(xiaotai_run(fn, call) == XIAOTAI_OK);
    settle();
}

int main(void)
{
    CHECK(!strcmp(xiaotai_rtc_caps()->version, "TiRTC 2.5.0") && xiaotai_rtc_caps()->cancel_pending);
    boot(&(xiaotai_transport_config_t){.links = 1U, .release_timeout_ms = 15000U,
        .connect_cache = XIAOTAI_TRANSPORT_OFF, .option_strings_without_nul = true});
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "fixture-device", "fixture-secret", "fixture-client");
    fake_platform_endpoint(XIAOTAI_OK, TEST_ENDPOINT);
    fake_platform_phase(XIAOTAI_PLATFORM_ONLINE);
    fake_net_set(XIAOTAI_NET_UP, true);
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_UP);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE && f.started);
    const unsigned preinit[] = {TIRTC_OPT_MAX_SEND_BUFFER, TIRTC_OPT_MAX_CONNECTIONS, TIRTC_OPT_CONNECT_CACHE, INIT};
    CHECK(!memcmp(f.trace, preinit, sizeof(preinit)) && f.buffer == 16384U && f.connection_limit == 1 && !f.cache);
    CHECK(f.key_size == strlen("fixture-secret") && f.client_size == strlen("fixture-client") &&
          f.endpoint_size == strlen(TEST_ENDPOINT) && !strcmp(f.endpoint, TEST_ENDPOINT));

    mini_call_t cancelled = {0}, timed = {.timeout_ms = 100U}, late = {.kind = XIAOTAI_LINK_P2P}, next = {0};
    const uint32_t baseline_bytes = status_now().engine_bytes;
    atomic_store(&mini_pending, 1U);
    owner_call(mini_connect, &cancelled);
    CHECK(atomic_load(&mini_connects) == 1U && f.downcall == 1U &&
          f.tag == (void *)(uintptr_t)cancelled.generation);
    CHECK(status_now().engine_bytes > baseline_bytes);
    owner_call(mini_disconnect, &cancelled);
    CHECK(atomic_load(&mini_cancels) == 1U && atomic_load(&mini_polls) == 0U);
    CHECK(status_now().links_releasing == 1U && !cancelled.opened && !cancelled.closed);
    advance(19);
    CHECK(atomic_load(&mini_polls) == 0U);
    advance(1);
    CHECK(atomic_load(&mini_polls) == 1U && status_now().links_releasing == 1U &&
          status_now().engine_bytes > baseline_bytes);
    atomic_store(&mini_pending, 0U);
    advance(19);
    CHECK(atomic_load(&mini_polls) == 1U && status_now().links_releasing == 1U);
    advance(1);
    CHECK(atomic_load(&mini_polls) == 2U && status_now().links_releasing == 0U && !cancelled.closed &&
          status_now().engine_bytes == baseline_bytes);
    puts("Mini explicit cancel: one SDK cancel; after 20 ms pending polls, all owned bytes returned; no owner callback");

    atomic_store(&mini_pending, 1U);
    owner_call(mini_connect, &timed);
    advance(99);
    CHECK(!timed.closed && atomic_load(&mini_cancels) == 1U);
    advance(1);
    CHECK(timed.closed == 1U && timed.reason == XIAOTAI_LINK_END_TIMEOUT && !timed.opened);
    CHECK(atomic_load(&mini_cancels) == 2U && status_now().links_releasing == 1U);
    atomic_store(&mini_pending, 0U);
    advance(20);
    CHECK(status_now().links_releasing == 0U && timed.closed == 1U && status_now().engine_bytes == baseline_bytes);
    puts("Mini deadline: one TIMEOUT at 100 ms, wait until SDK pending releases");

    atomic_store(&mini_pending, 1U);
    owner_call(mini_connect, &late);
    CHECK(f.downcall == 2U && f.tag == (void *)(uintptr_t)late.generation);
    owner_call(mini_disconnect, &late);
    owner_call(mini_connect, &next);
    CHECK(atomic_load(&mini_cancels) == 3U && atomic_load(&mini_connects) == 3U && !next.opened);
    conn_a.tag = f.tag;
    f.connected(0, &conn_a, f.tag);
    settle();
    CHECK(f.disconnects == 1U && f.close_pending && status_now().late_links_closed == 1U);
    CHECK(status_now().links_releasing == 1U && !late.opened && !late.closed);
    CHECK(atomic_load(&mini_connects) == 3U);
    atomic_store(&mini_pending, 0U);
    finish_disconnect();
    settle();
    CHECK(!status_now().links_releasing && atomic_load(&mini_connects) == 4U);
    CHECK(f.tag == (void *)(uintptr_t)next.generation && !next.opened && !next.closed);
    f.connected(TIRTC_E_TIMEOUTED, NULL, f.tag);
    settle();
    CHECK(next.closed == 1U && !next.opened && next.reason == XIAOTAI_LINK_END_TIMEOUT);
    CHECK(!late.opened && !late.closed && f.disconnects == 1U && status_now().engine_bytes == baseline_bytes);
    puts("Mini late success: physical handle closed once, newer connect waited, old owner received no callbacks");
    shutdown_all();
    return 0;
}
