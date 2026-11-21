// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_s3_discovered_endpoint.py
// Replaces legacy: tests/test_boot_contract.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_external_connect_memory
/* transport_run: configuration, sizing and the run rules of the transport
 * engine (work package WP1) on the scripted backend, inside the real
 * framework, with a manual engine clock so every timer is checked at its
 * absolute instant. Links are not requested here (that is the link
 * package); where a rule acts on links, a link is placed in a slot directly,
 * on the worker, as the link package would. */
#include "support.h"

/* ------------------------------------------------------------ test links */

static uint32_t g_inject_generation;
static xiaotai_rtc_conn_t g_inject_conn;
static uint8_t g_inject_state;
static uint32_t g_closed_generation;
static xiaotai_link_end_t g_closed_reason;

static void link_closed(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    (void)ctx;
    (void)detail;
    g_closed_generation = generation;
    g_closed_reason = reason;
}

static void inject_slot(void)
{
    xt_slot_t *slot = &xt.slots[0];
    xiaotai_critical_enter(&xt.lock);
    atomic_store(&slot->generation, g_inject_generation);
    atomic_store(&slot->state, g_inject_state);
    atomic_store(&slot->flags, XT_SLOT_REPORTED);
    slot->handlers = (xiaotai_link_handlers_t){.closed = link_closed};
    slot->conn = g_inject_conn;
    slot->kind = XIAOTAI_LINK_WHIP;
    slot->request = 0;
    xiaotai_critical_exit(&xt.lock);
    xt_run_links_changed();
}

static void open_link(uint32_t generation, xiaotai_rtc_conn_t conn)
{
    g_closed_generation = 0;
    g_inject_generation = generation;
    g_inject_conn = conn;
    g_inject_state = XT_SLOT_OPEN;
    on_worker(inject_slot);
}

static bool link_ended(uint32_t generation, xiaotai_link_end_t reason)
{
    return g_closed_generation == generation && g_closed_reason == reason &&
           atomic_load(&xt.slots[0].generation) == 0U;
}

static void fault_overflow(void) { xt_fault(XT_FAULT_OVERFLOW, 0); }

static size_t call_sequence(xiaotai_transport_host_call_kind_t *kinds, size_t capacity)
{
    xiaotai_transport_host_call_t list[64];
    const size_t count = xiaotai_transport_host_calls(list, 64);
    size_t n = 0;
    for (size_t i = 0; i < count && i < 64 && n < capacity; ++i) {
        if (list[i].kind != XIAOTAI_TRANSPORT_HOST_STATISTICS) kinds[n++] = list[i].kind;
    }
    return n;
}

/* ------------------------------------------------------------ configuration */

static void test_configure(void)
{
    xiaotai_transport_config_t c;
#define REJECT(assignments)                                                    \
    do {                                                                       \
        memset(&c, 0, sizeof(c));                                              \
        assignments;                                                           \
        CHECK(xiaotai_transport_configure(&c) == XIAOTAI_ERR_INVALID_ARG);    \
    } while (0)
#define ACCEPT(assignments)                                                    \
    do {                                                                       \
        memset(&c, 0, sizeof(c));                                              \
        assignments;                                                           \
        CHECK(xiaotai_transport_configure(&c) == XIAOTAI_OK);                 \
    } while (0)
    REJECT(c.links = 5);
    REJECT(c.poll_timeout_ms = 51);
    REJECT(c.stack_log_level = 6);
    REJECT(c.worker_stack_bytes = 4095);
    REJECT(c.vendor_idle_stack_bytes = 4095);
    REJECT(c.vendor_idle_stack_bytes = 4097);
    REJECT(c.vendor_idle_stack_bytes = 8208);
    REJECT(c.worker_priority = 25);
    REJECT(c.yield_connect_free_bytes = 65536);
    REJECT(c.yield_connect_block_bytes = 32768);
    REJECT(c.release_timeout_ms = 999);
    REJECT(c.release_timeout_ms = 600001);
    REJECT(c.command_pool_bytes = XIAOTAI_LINK_COMMAND_LIMIT_BYTES * XIAOTAI_TRANSPORT_LINKS_MAX + 1U);
    REJECT(c.command_pool_mode = (xiaotai_transport_command_pool_mode_t)99);
    REJECT(c.start_failure = (xiaotai_transport_recovery_t)4);
    REJECT(c.unsolicited_stop = (xiaotai_transport_recovery_t)9);
    REJECT(c.connect_cache = (xiaotai_transport_switch_t)3);
    REJECT(c.media_transport_before_init = true);
    REJECT(c.media_transport = XIAOTAI_TRANSPORT_OFF; c.media_transport_before_init = true);
    ACCEPT((void)0);
    ACCEPT(c.vendor_idle_stack_bytes = 4096);
    ACCEPT(c.vendor_idle_stack_bytes = 6144);
    ACCEPT(c.vendor_idle_stack_bytes = 8192);
    ACCEPT(c.media_transport = XIAOTAI_TRANSPORT_ON; c.media_transport_before_init = true);
    ACCEPT(c.links = 4; c.poll_timeout_ms = 50; c.stack_log_level = 5; c.worker_stack_bytes = 4096;
           c.worker_priority = 24; c.release_timeout_ms = 1000);
    ACCEPT(c.release_timeout_ms = 600000; c.yield_connect_free_bytes = 65536; c.yield_connect_block_bytes = 32768);
    ACCEPT(c.command_pool_bytes = XIAOTAI_LINK_COMMAND_LIMIT_BYTES * XIAOTAI_TRANSPORT_LINKS_MAX);
    ACCEPT(c.start_failure = XIAOTAI_TRANSPORT_RECOVER_FAULT; c.unsolicited_stop = XIAOTAI_TRANSPORT_RECOVER_ON_EVENT);
    CHECK(xiaotai_transport_configure(NULL) == XIAOTAI_OK);
#undef REJECT
#undef ACCEPT
    boot(NULL);
    memset(&c, 0, sizeof(c));
    CHECK(xiaotai_transport_configure(&c) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_transport_version() && strcmp(xiaotai_transport_version(), "host-script") == 0);
    shutdown_all();
}

/* ------------------------------------------------------------ sizing */

static uint32_t compact_engine_bytes(uint8_t links, uint8_t events, uint32_t stack)
{
    return (uint32_t)(links * (sizeof(xt_release_t) + sizeof(xt_slot_t)) + events * XT_EVENT_ITEM_BYTES + 2048U +
                      (events + 1U) * (1U + sizeof(xt_command_payload_t *)) +
                      XT_JOB_DEPTH * sizeof(xt_job_t) + stack);
}

static void test_sizing(void)
{
    /* Requirements: refused before anything is sized. */
    xiaotai_transport_requirement_t r = {0};
    CHECK(xiaotai_transport_require(NULL) == XIAOTAI_ERR_INVALID_ARG);
    r.links = 5;
    CHECK(xiaotai_transport_require(&r) == XIAOTAI_ERR_INVALID_ARG);
    r = (xiaotai_transport_requirement_t){.send_buffer_bytes = XT_SEND_BUFFER_CEILING_INTERNAL + 1U};
    CHECK(xiaotai_transport_require(&r) == XIAOTAI_ERR_INVALID_SIZE);
    r = (xiaotai_transport_requirement_t){.command_pool_bytes = XIAOTAI_LINK_COMMAND_LIMIT_BYTES + 1U};
    CHECK(xiaotai_transport_require(&r) == XIAOTAI_ERR_INVALID_SIZE);
    xiaotai_transport_reset();

    /* The largest of product and modules; pool = largest share x links. */
    static const xiaotai_transport_requirement_t need = {.send_buffer_bytes = 8192, .links = 2,
                                                         .command_pool_bytes = 40960};
    g_requirement = &need;
    g_observed.measure_start = true;
    const xiaotai_transport_config_t one_link = {.links = 1};
    boot_with(&one_link, (boot_options_t){.with_requiring = true});
    CHECK(g_requirement_result == XIAOTAI_OK);
    xiaotai_transport_status_t s = status_now();
    CHECK(s.links_max == 2U);
    CHECK(s.command_pool_bytes == 81920U);
    CHECK(s.send_buffer_bytes == 8192U);
    CHECK(s.state == XIAOTAI_TRANSPORT_DOWN && s.wait == XIAOTAI_TRANSPORT_WAIT_IDENTITY);
    /* Link table, event ring, command slots, resident reservation and job
     * queue. The cap and the reserved bytes are measured separately. */
    CHECK(g_observed.start_allocations == 5U);
    CHECK(g_observed.start_outstanding == 5);
    CHECK(s.engine_bytes == compact_engine_bytes(2, XT_EVENT_SLOTS_INTERNAL, XT_WORKER_STACK_INTERNAL));
    /* Worker: BULK stack, the owner's priority by default. */
    CHECK(strcmp(xt.worker_config.name, XT_WORKER_NAME) == 0);
    CHECK(xt.worker_config.stack == XIAOTAI_MEM_BULK);
    CHECK(xt.worker_config.stack_bytes == XT_WORKER_STACK_INTERNAL);
    CHECK(xt.worker_config.priority == (unsigned)XIAOTAI_FRAMEWORK_OWNER_PRIORITY);
    /* Closed once the engine is sized. */
    CHECK(xiaotai_transport_require(&need) == XIAOTAI_ERR_INVALID_STATE);
    give_account();
    xiaotai_transport_host_start_t start;
    xiaotai_transport_host_last_start(&start);
    CHECK(start.links == 2U && start.send_buffer_bytes == 8192U);
    shutdown_all();

    /* Product values win; without a declared share each link gets 2 x 4096. */
    const xiaotai_transport_config_t product = {.command_pool_bytes = 16384, .worker_priority = 4,
                                                .worker_stack_bytes = 8192, .event_slots = 4};
    boot(&product);
    s = status_now();
    CHECK(s.links_max == 1U && s.command_pool_bytes == 16384U);
    CHECK(s.send_buffer_bytes == XT_SEND_BUFFER_DEFAULT_INTERNAL);
    CHECK(xt.worker_config.priority == 4U && xt.worker_config.stack_bytes == 8192U);
    CHECK(s.engine_bytes == compact_engine_bytes(1, 4, 8192));
    shutdown_all();
    boot(NULL);
    CHECK(status_now().command_pool_bytes == 2U * XIAOTAI_LINK_COMMAND_DEFAULT_BYTES);
    shutdown_all();
}

/* ------------------------------------------------------- start conditions */

static void test_start_conditions(void)
{
    boot(NULL);
    xiaotai_transport_status_t s = status_now();
    CHECK(s.state == XIAOTAI_TRANSPORT_DOWN && s.wait == XIAOTAI_TRANSPORT_WAIT_IDENTITY);
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, 0);
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_ENDPOINT);
    /* Discovered without an RTC entry: the stack's built-in entry. */
    fake_platform_endpoint(XIAOTAI_ERR_NOT_SUPPORTED, NULL);
    emit(XIAOTAI_EVENT_PLATFORM_DISCOVERED, 0);
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_NETWORK);
    uint32_t hold = 0;
    CHECK(xiaotai_transport_hold(&hold) == XIAOTAI_OK && hold != 0U);
    fake_net_set(XIAOTAI_NET_UP, true);
    emit(XIAOTAI_EVENT_NET_UP, 1);
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_HOLD);
    fake_platform_gated(true);
    xiaotai_transport_release(hold);
    settle();
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_MAINTENANCE);
    /* Unbound matters only with down_while_unbound. */
    emit(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, 1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 0U);
    /* No event tells that the gate opened: the periodic pass reads it. */
    fake_platform_gated(false);
    advance(1000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    CHECK(xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_START) == T0 + 1000);
    xiaotai_transport_host_start_t start;
    xiaotai_transport_host_last_start(&start);
    CHECK(strcmp(start.device_id, "device-test-1") == 0 && strcmp(start.device_secret, "key-test-1") == 0);
    CHECK(strcmp(start.client_id, "test-020000123456") == 0);
    CHECK(start.endpoint[0] == '\0');
    CHECK(status_now().state == XIAOTAI_TRANSPORT_STARTING);
    shutdown_all();

    /* down_while_unbound: down until the platform is online again. */
    const xiaotai_transport_config_t unbound = {.down_while_unbound = true};
    boot(&unbound);
    emit(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, 0);
    give_account();
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_UNBOUND && calls(XIAOTAI_TRANSPORT_HOST_START) == 0U);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    xiaotai_transport_host_started();
    settle();
    /* Unbound while running: the stack is brought down. */
    emit(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, 1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1U);
    CHECK(status_now().state == XIAOTAI_TRANSPORT_STOPPING && status_now().wait == XIAOTAI_TRANSPORT_WAIT_UNBOUND);
    xiaotai_transport_host_stopped(0);
    settle();
    CHECK(status_now().state == XIAOTAI_TRANSPORT_DOWN && status_now().wait == XIAOTAI_TRANSPORT_WAIT_UNBOUND);
    shutdown_all();

    /* endpoint_required: no discovered entry, no start. */
    const xiaotai_transport_config_t required = {.endpoint_required = true};
    boot(&required);
    give_account();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    shutdown_all();
    boot(&required);
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    fake_platform_endpoint(XIAOTAI_ERR_NOT_SUPPORTED, NULL);
    fake_net_set(XIAOTAI_NET_UP, true);
    emit(XIAOTAI_EVENT_PLATFORM_DISCOVERED, 0);
    advance(20000);
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_ENDPOINT && calls(XIAOTAI_TRANSPORT_HOST_START) == 0U);
    fake_platform_endpoint(XIAOTAI_OK, TEST_ENDPOINT);
    emit(XIAOTAI_EVENT_PLATFORM_DISCOVERED, 0);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    xiaotai_transport_host_last_start(&start);
    CHECK(strcmp(start.endpoint, TEST_ENDPOINT) == 0);
    shutdown_all();

    /* start_after_online: only while the platform session is online; a
     * session that drops later does not stop the stack. */
    const xiaotai_transport_config_t after_online = {.start_after_online = true};
    boot(&after_online);
    give_account();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    shutdown_all();
    boot(&after_online);
    fake_platform_phase(XIAOTAI_PLATFORM_LOGIN);
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    fake_platform_endpoint(XIAOTAI_OK, TEST_ENDPOINT);
    fake_net_set(XIAOTAI_NET_UP, true);
    emit(XIAOTAI_EVENT_NET_UP, 1);
    advance(5000);
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_PLATFORM && calls(XIAOTAI_TRANSPORT_HOST_START) == 0U);
    fake_platform_phase(XIAOTAI_PLATFORM_ONLINE);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    xiaotai_transport_host_started();
    settle();
    fake_platform_phase(XIAOTAI_PLATFORM_CONNECTING);
    emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, XIAOTAI_PLATFORM_OFFLINE_SIGNALING);
    advance(5000);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE && calls(XIAOTAI_TRANSPORT_HOST_STOP) == 0U);
    shutdown_all();
}

/* ---------------------------------------------------- asynchronous start */

static void test_async_online(void)
{
    boot(NULL);
    give_account();
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_STARTING);
    CHECK(xt_link_admissible() == XIAOTAI_ERR_INVALID_STATE);
    advance(60000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U && xiaotai_transport_state() == XIAOTAI_TRANSPORT_STARTING);
    xiaotai_transport_host_started();
    settle();
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    CHECK(xt_link_admissible() == XIAOTAI_OK);
    CHECK(g_observed.state_count == 2U);
    CHECK(g_observed.states[0] == XIAOTAI_TRANSPORT_STARTING && g_observed.states[1] == XIAOTAI_TRANSPORT_ONLINE);
    CHECK(fake_platform_profile_ready());
    CHECK(logged("start") && logged("online"));
    CHECK(status_now().starts == 1U);
    shutdown_all();

    /* STARTED reported before the start call returned. */
    boot(NULL);
    xiaotai_transport_host_auto_started(true);
    give_account();
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    shutdown_all();
}

/* ------------------------------------------------------- start failures */

static void test_start_failure_retry(void)
{
    const xiaotai_transport_config_t config = {.start_failure = XIAOTAI_TRANSPORT_RECOVER_RETRY};
    boot(&config);
    xiaotai_transport_host_fail_start(XIAOTAI_FAIL, -7, 2);
    give_account();
    const int64_t failed_at = now_ms();
    xiaotai_transport_status_t s = status_now();
    CHECK(s.state == XIAOTAI_TRANSPORT_DOWN && s.wait == XIAOTAI_TRANSPORT_WAIT_RETRY);
    CHECK(s.start_failures == 1U && s.last_start_detail == -7);
    CHECK(!xiaotai_transport_host_initialized());
    advance(4999);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    advance(1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    CHECK(xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_START) == failed_at + 5000);
    advance(5000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 3U);
    CHECK(xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_START) == failed_at + 10000);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_STARTING);
    /* A failed start leaves the stack uninitialized: no uninit, no start on
     * an initialized stack. */
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 0U && xiaotai_transport_host_violations() == 0U);
    CHECK(logged("start_failed"));
    shutdown_all();

    const xiaotai_transport_config_t quick = {.start_retry_ms = 2000};
    boot(&quick);
    xiaotai_transport_host_fail_start(XIAOTAI_FAIL, -7, 1);
    give_account();
    advance(1999);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    advance(1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    shutdown_all();
}

static void test_start_failure_on_event(void)
{
    const xiaotai_transport_config_t config = {.start_failure = XIAOTAI_TRANSPORT_RECOVER_ON_EVENT};
    boot(&config);
    xiaotai_transport_host_fail_start(XIAOTAI_FAIL, -7, 2);
    give_account();
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_START_FAILED);
    advance(50000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    /* A platform event: one more try. */
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_START_FAILED);
    /* The application's restart on its own triggers: one more try. */
    CHECK(xiaotai_transport_restart() == XIAOTAI_OK);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 3U);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_STARTING);
    shutdown_all();
}

static void test_retry_deadline_while_gated(void)
{
    const xiaotai_transport_config_t config = {.start_retry_ms = 2000};
    boot(&config);
    xiaotai_transport_host_fail_start(XIAOTAI_FAIL, -7, 1);
    give_account();
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_RETRY);
    fake_platform_gated(true);
    emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, 0);
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_MAINTENANCE);
    advance(config.start_retry_ms);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_MAINTENANCE);
    fake_platform_gated(false);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    CHECK(xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_START) == now_ms());
    shutdown_all();
}

static void test_start_failure_fault(void)
{
    const xiaotai_transport_config_t config = {.start_failure = XIAOTAI_TRANSPORT_RECOVER_FAULT};
    boot(&config);
    xiaotai_transport_host_fail_start(XIAOTAI_FAIL, -7, 1);
    give_account();
    xiaotai_transport_status_t s = status_now();
    CHECK(s.state == XIAOTAI_TRANSPORT_FAULT && s.wait == XIAOTAI_TRANSPORT_WAIT_FAULT && s.faults == 1U);
    CHECK(s.last_fault_detail == -7);
    advance(10U * XT_START_RETRY_DEFAULT_MS);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    /* The stack is uninitialized after a failed start: nothing to stop. */
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 0U && calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 0U);
    CHECK(logged("fault"));
    CHECK(xiaotai_transport_restart() == XIAOTAI_OK);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U && xiaotai_transport_state() == XIAOTAI_TRANSPORT_STARTING);
    shutdown_all();
}

/* ------------------------------------------------------ unsolicited stop */

static void unsolicited_case(xiaotai_transport_recovery_t recovery)
{
    const xiaotai_transport_config_t config = {.unsolicited_stop = recovery};
    boot_online(&config);
    open_link(77, 0x55);
    xiaotai_transport_host_stopped(-3);
    settle();
    xiaotai_transport_status_t s = status_now();
    CHECK(s.unsolicited_stops == 1U);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U && calls(XIAOTAI_TRANSPORT_HOST_STOP) == 0U);
    /* The links ended with STOPPED; the stack released them itself. */
    CHECK(link_ended(77, XIAOTAI_LINK_END_STOPPED));
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 0U && s.links_releasing == 0U);
    CHECK(logged("stopped_unsolicited"));
    switch (recovery) {
    case XIAOTAI_TRANSPORT_RECOVER_RETRY: {
        const int64_t at = now_ms();
        CHECK(s.state == XIAOTAI_TRANSPORT_DOWN && s.wait == XIAOTAI_TRANSPORT_WAIT_RETRY);
        advance(4999);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
        advance(1);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
        CHECK(xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_START) == at + 5000);
        break;
    }
    case XIAOTAI_TRANSPORT_RECOVER_AT_ONCE:
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
        CHECK(xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_START) == now_ms());
        CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_STARTING);
        break;
    case XIAOTAI_TRANSPORT_RECOVER_ON_EVENT:
        CHECK(s.state == XIAOTAI_TRANSPORT_DOWN && s.wait == XIAOTAI_TRANSPORT_WAIT_START_FAILED);
        advance(50000);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
        emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
        break;
    case XIAOTAI_TRANSPORT_RECOVER_FAULT:
        CHECK(s.state == XIAOTAI_TRANSPORT_FAULT && s.wait == XIAOTAI_TRANSPORT_WAIT_FAULT);
        advance(10U * XT_START_RETRY_DEFAULT_MS);
        emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U && calls(XIAOTAI_TRANSPORT_HOST_STOP) == 0U);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U);
        break;
    }
    CHECK(xiaotai_transport_host_violations() == 0U);
    shutdown_all();
}

static void test_unsolicited_stop(void)
{
    unsolicited_case(XIAOTAI_TRANSPORT_RECOVER_RETRY);
    unsolicited_case(XIAOTAI_TRANSPORT_RECOVER_AT_ONCE);
    unsolicited_case(XIAOTAI_TRANSPORT_RECOVER_ON_EVENT);
    unsolicited_case(XIAOTAI_TRANSPORT_RECOVER_FAULT);
}

/* ------------------------------------------------------- account change */

static void test_account_change(void)
{
    boot_online(NULL);
    open_link(41, 0x66);
    /* A re-binding with the same identity: nothing restarts. */
    emit(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, 1);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    advance(1000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 0U);
    /* Another key: stop, release the link's connection, start again. */
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-2", "");
    advance(1000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1U && xiaotai_transport_state() == XIAOTAI_TRANSPORT_STOPPING);
    CHECK(link_ended(41, XIAOTAI_LINK_END_STOPPED));
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1U);
    xiaotai_transport_host_call_kind_t seq[16];
    size_t n = call_sequence(seq, 16);
    CHECK(n == 3U && seq[0] == XIAOTAI_TRANSPORT_HOST_START && seq[1] == XIAOTAI_TRANSPORT_HOST_DISCONNECT &&
          seq[2] == XIAOTAI_TRANSPORT_HOST_STOP);
    xiaotai_transport_host_stopped(0);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U && calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    xiaotai_transport_host_start_t start;
    xiaotai_transport_host_last_start(&start);
    CHECK(strcmp(start.device_secret, "key-test-2") == 0);
    xiaotai_transport_host_started();
    settle();
    /* Another endpoint is another account too. */
    fake_platform_endpoint(XIAOTAI_OK, "http://rtc2.example.test");
    advance(1000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 2U);
    CHECK(xiaotai_transport_host_violations() == 0U);
    shutdown_all();
}

/* ------------------------------------------------------------------ holds */

static void test_holds(void)
{
    boot_online(NULL);
    uint32_t first = 0, second = 0;
    CHECK(xiaotai_transport_hold(NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_transport_hold(&first) == XIAOTAI_OK && first != 0U);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1U);
    CHECK(status_now().state == XIAOTAI_TRANSPORT_STOPPING && status_now().wait == XIAOTAI_TRANSPORT_WAIT_HOLD);
    CHECK(xiaotai_transport_restart() == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_transport_host_stopped(0);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U);
    CHECK(status_now().state == XIAOTAI_TRANSPORT_DOWN && status_now().wait == XIAOTAI_TRANSPORT_WAIT_HOLD);
    CHECK(!fake_platform_profile_ready());
    CHECK(xiaotai_transport_hold(&second) == XIAOTAI_OK && second != first);
    CHECK(status_now().holds == 2U);
    xiaotai_transport_release(first);
    xiaotai_transport_release(first);      /* already released: ignored */
    xiaotai_transport_release(0xBADU);     /* unknown: ignored */
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U && status_now().holds == 1U);
    xiaotai_transport_release(second);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    const int expected[] = {XIAOTAI_TRANSPORT_STARTING, XIAOTAI_TRANSPORT_ONLINE, XIAOTAI_TRANSPORT_STOPPING,
                            XIAOTAI_TRANSPORT_DOWN, XIAOTAI_TRANSPORT_STARTING};
    CHECK(g_observed.state_count == 5U);
    for (size_t i = 0; i < 5U; ++i) CHECK(g_observed.states[i] == expected[i]);
    /* At most XT_HOLDS_MAX at once. */
    uint32_t holds[XT_HOLDS_MAX];
    for (size_t i = 0; i < XT_HOLDS_MAX; ++i) CHECK(xiaotai_transport_hold(&holds[i]) == XIAOTAI_OK);
    uint32_t extra = 0;
    CHECK(xiaotai_transport_hold(&extra) == XIAOTAI_ERR_NO_MEM);
    for (size_t i = 0; i < XT_HOLDS_MAX; ++i) xiaotai_transport_release(holds[i]);
    settle();
    shutdown_all();

    /* A hold taken before the engine started keeps it down from the start. */
    xiaotai_transport_reset();
    uint32_t early = 0;
    CHECK(xiaotai_transport_hold(&early) == XIAOTAI_OK);
    boot(NULL);
    give_account();
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_HOLD && calls(XIAOTAI_TRANSPORT_HOST_START) == 0U);
    xiaotai_transport_release(early);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    shutdown_all();
}

static void test_restart_running(void)
{
    boot(NULL);
    CHECK(xiaotai_transport_restart() == XIAOTAI_ERR_INVALID_STATE);   /* down for its own reason */
    shutdown_all();
    boot_online(NULL);
    CHECK(xiaotai_transport_restart() == XIAOTAI_OK);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1U);
    xiaotai_transport_host_stopped(0);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U && calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    CHECK(xiaotai_transport_host_violations() == 0U);
    shutdown_all();
}

/* ------------------------------------------------------------------ faults */

static void test_fault_releases_stack(void)
{
    const xiaotai_transport_config_t config = {.overflow_fault = true, .connect_reserve_bytes = 17408};
    boot_online(&config);
    CHECK(status_now().connect_reserve_held);
    open_link(90, 0x91);
    on_worker(fault_overflow);
    xiaotai_transport_status_t s = status_now();
    CHECK(s.state == XIAOTAI_TRANSPORT_FAULT && s.wait == XIAOTAI_TRANSPORT_WAIT_FAULT && s.faults == 1U);
    CHECK(link_ended(90, XIAOTAI_LINK_END_STOPPED));
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1U && calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 0U);
    xiaotai_transport_host_stopped(0);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U);
    s = status_now();
    CHECK(s.state == XIAOTAI_TRANSPORT_FAULT && s.wait == XIAOTAI_TRANSPORT_WAIT_FAULT);
    CHECK(!s.connect_reserve_held);
    advance(10U * XT_START_RETRY_DEFAULT_MS);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U && calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U);
    CHECK(xiaotai_transport_host_violations() == 0U);
    shutdown_all();
}

static void test_fault_stop_failed(void)
{
    boot_online(NULL);
    xiaotai_transport_host_fail_stop(XIAOTAI_FAIL, -9, 1);
    uint32_t hold = 0;
    CHECK(xiaotai_transport_hold(&hold) == XIAOTAI_OK);
    settle();
    xiaotai_transport_status_t s = status_now();
    CHECK(s.state == XIAOTAI_TRANSPORT_FAULT && s.last_fault_detail == -9);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 0U && xiaotai_transport_host_initialized());
    xiaotai_transport_release(hold);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1U && xiaotai_transport_state() == XIAOTAI_TRANSPORT_FAULT);
    CHECK(xiaotai_transport_restart() == XIAOTAI_OK);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 2U && calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    xiaotai_transport_host_stopped(0);
    settle();
    xiaotai_transport_host_call_kind_t seq[16];
    const size_t n = call_sequence(seq, 16);
    CHECK(n == 5U && seq[1] == XIAOTAI_TRANSPORT_HOST_STOP && seq[2] == XIAOTAI_TRANSPORT_HOST_STOP &&
          seq[3] == XIAOTAI_TRANSPORT_HOST_UNINIT && seq[4] == XIAOTAI_TRANSPORT_HOST_START);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_STARTING);
    CHECK(xiaotai_transport_host_violations() == 0U);
    shutdown_all();
}

static void test_fault_stop_timeout(void)
{
    const xiaotai_transport_config_t config = {.release_timeout_ms = 15000};
    boot_online(&config);
    uint32_t hold = 0;
    CHECK(xiaotai_transport_hold(&hold) == XIAOTAI_OK);
    settle();
    const int64_t stop_at = xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_STOP);
    advance(14999);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_STOPPING && status_now().release_stalls == 0U);
    advance(1);
    xiaotai_transport_status_t s = status_now();
    CHECK(now_ms() == stop_at + 15000);
    CHECK(s.state == XIAOTAI_TRANSPORT_FAULT && s.release_stalls == 1U);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 0U && logged("release_stall"));
    xiaotai_transport_release(hold);
    CHECK(xiaotai_transport_restart() == XIAOTAI_OK);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 2U);
    xiaotai_transport_host_stopped(0);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U && calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    CHECK(xiaotai_transport_host_violations() == 0U);
    shutdown_all();
}

static void test_restart_during_fault_stop(void)
{
    boot_online(NULL);
    on_worker(fault_overflow);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1U);
    CHECK(xiaotai_transport_restart() == XIAOTAI_OK);
    settle();
    advance(1000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U && xiaotai_transport_state() == XIAOTAI_TRANSPORT_FAULT);
    xiaotai_transport_host_stopped(0);
    settle();
    xiaotai_transport_host_call_kind_t seq[16];
    const size_t n = call_sequence(seq, 16);
    CHECK(n == 4U && seq[1] == XIAOTAI_TRANSPORT_HOST_STOP && seq[2] == XIAOTAI_TRANSPORT_HOST_UNINIT &&
          seq[3] == XIAOTAI_TRANSPORT_HOST_START);
    advance(10000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    shutdown_all();
}

static void test_restart_waits_for_gate(void)
{
    boot_online(NULL);
    xiaotai_transport_host_auto_stopped(true);
    on_worker(fault_overflow);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U);
    fake_platform_gated(true);
    CHECK(xiaotai_transport_restart() == XIAOTAI_OK);
    settle();
    xiaotai_transport_status_t s = status_now();
    CHECK(s.state == XIAOTAI_TRANSPORT_DOWN && s.wait == XIAOTAI_TRANSPORT_WAIT_MAINTENANCE);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    fake_platform_gated(false);
    advance(1000);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    CHECK(xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_START) == now_ms());
    shutdown_all();
}

static void test_fault_restart_ms(void)
{
    const xiaotai_transport_config_t config = {.fault_restart_ms = 7000};
    boot_online(&config);
    xiaotai_transport_host_auto_stopped(true);
    const int64_t fault_at = now_ms();
    on_worker(fault_overflow);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_FAULT && calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U);
    advance(6999);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    advance(1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U);
    CHECK(xiaotai_transport_host_last_at(XIAOTAI_TRANSPORT_HOST_START) == fault_at + 7000);
    shutdown_all();
}

/* --------------------------------------------------------- start reserve */

#define START_RESERVE 20480U

static bool start_reserve_held(void) { return status_now().start_reserve_held; }

static atomic_uint rejected_start_workers;
static atomic_bool reject_start_worker;

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (config && config->name && atomic_load(&reject_start_worker) && !strcmp(config->name, XT_WORKER_NAME)) {
        atomic_fetch_add(&rejected_start_workers, 1U);
        return XIAOTAI_ERR_NO_MEM;
    }
    return __real_xiaotai_task_create(config, task);
}

static void test_start_worker_failure(bool net_first)
{
    fakes_reset();
    if (net_first) fake_net_set(XIAOTAI_NET_CONNECTING, false);
    atomic_store(&reject_start_worker, true);
    atomic_store(&rejected_start_workers, 0U);
    const xiaotai_transport_config_t config = {.start_reserve_bytes = START_RESERVE};
    boot_with(&config, (boot_options_t){.keep_fakes = true});
    atomic_store(&reject_start_worker, false);
    CHECK(atomic_load(&rejected_start_workers) == 1U);
    CHECK(status_now().state == XIAOTAI_TRANSPORT_FAULT && status_now().last_fault_detail == XIAOTAI_ERR_NO_MEM);
    CHECK(!start_reserve_held() && !status_now().engine_bytes);

    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_CONNECTING);
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_UP);
    const xiaotai_transport_status_t status = status_now();
    fprintf(stderr, "net_first=%u state=%u detail=%d reserved=%u engine_bytes=%lu\n",
            (unsigned)net_first, (unsigned)status.state, status.last_fault_detail,
            (unsigned)status.start_reserve_held, (unsigned long)status.engine_bytes);
    CHECK(status.state == XIAOTAI_TRANSPORT_FAULT && !status.start_reserve_held && !status.engine_bytes);
    CHECK(!calls(XIAOTAI_TRANSPORT_HOST_START));
    shutdown_all();
}

static void test_start_reserve(void)
{
    const xiaotai_transport_config_t config = {.start_reserve_bytes = START_RESERVE};
    boot(&config);
    const uint32_t base = status_now().engine_bytes;
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_PREPARING);
    CHECK(!start_reserve_held());
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_CONNECTING);
    CHECK(start_reserve_held() && status_now().engine_bytes == base + START_RESERVE);
    /* Once per boot. */
    xiaotai_mem_host_clear();
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_UP);
    CHECK(xiaotai_mem_host_allocations() == 0U);
    /* Given up for a hold, taken again when the last hold goes. */
    uint32_t hold = 0;
    CHECK(xiaotai_transport_hold(&hold) == XIAOTAI_OK);
    settle();
    CHECK(!start_reserve_held() && status_now().engine_bytes == base);
    xiaotai_transport_release(hold);
    settle();
    CHECK(start_reserve_held());
    /* Given up right before the stack starts. */
    give_account();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U && !start_reserve_held());
    shutdown_all();

    /* Not taken: counted, the stack starts all the same. */
    boot(&config);
    xiaotai_mem_host_fail_nth(1);
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_CONNECTING);
    CHECK(!start_reserve_held() && xt.counters.reserve_misses == 1U && logged("reserve_miss"));
    give_account();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    shutdown_all();
}

/* P41: whatever happens, the reserve has one owner and is freed once. */
typedef enum { PATH_START_OK = 0, PATH_START_RETRY, PATH_HOLD, PATH_RESET, PATH_COUNT } reserve_path_t;

static uint32_t g_fail_in_start;

static void transport_start_failing(void)
{
    xiaotai_mem_host_clear();
    if (g_fail_in_start) xiaotai_mem_host_fail_nth(g_fail_in_start);
    xiaotai_engine_transport_start();
    xiaotai_mem_host_fail_nth(0);
}

static void reserve_handoff_case(bool taken, reserve_path_t path, bool net_first)
{
    xiaotai_transport_host_reset();
    fakes_reset();
    if (net_first) fake_net_set(XIAOTAI_NET_CONNECTING, false);
    /* Before start: the sixth allocation of start() is the reserve (link
     * table, ring, command slots, command reservation, job queue, reserve). */
    g_fail_in_start = net_first && !taken ? 6U : 0U;
    static const char *const depends[] = {"platform", NULL};
    static const xiaotai_module_t transport = {.name = "transport", .depends = depends,
                                               .init = xiaotai_engine_transport_init,
                                               .start = transport_start_failing};
    const xiaotai_module_t *modules[3] = {&g_platform_module, &transport, &g_observer_module};
    xiaotai_transport_host_clock_manual(T0);
    const xiaotai_transport_config_t config = {.start_reserve_bytes = START_RESERVE};
    CHECK(xiaotai_transport_configure(&config) == XIAOTAI_OK);
    const xiaotai_framework_config_t framework = {.modules = modules, .module_count = 3,
                                                  .available_needs = UINT32_MAX};
    CHECK(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    settle();
    if (!net_first) {
        if (!taken) xiaotai_mem_host_fail_nth(1);
        emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_CONNECTING);
    }
    CHECK(start_reserve_held() == taken);
    switch (path) {
    case PATH_START_OK:
        give_account();
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U && !start_reserve_held());
        break;
    case PATH_START_RETRY:
        xiaotai_transport_host_fail_start(XIAOTAI_FAIL, -1, 1);
        give_account();
        /* The stack is uninitialized again: the reserve is taken back. */
        CHECK(start_reserve_held());
        advance(XT_START_RETRY_DEFAULT_MS);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U && !start_reserve_held());
        break;
    case PATH_HOLD: {
        uint32_t hold = 0;
        CHECK(xiaotai_transport_hold(&hold) == XIAOTAI_OK);
        settle();
        CHECK(!start_reserve_held());
        xiaotai_transport_release(hold);
        settle();
        CHECK(start_reserve_held());
        break;
    }
    case PATH_RESET:
    case PATH_COUNT:
        break;
    }
    const bool held = start_reserve_held();
    xiaotai_framework_reset();
    xiaotai_mem_host_clear();
    xiaotai_transport_reset();
    /* The five start allocations, plus the reserve when held, are each
     * freed exactly once (a second free would stop the sanitizer). */
    CHECK(xiaotai_mem_host_outstanding() == -(int64_t)(5 + (held ? 1 : 0)));
    xiaotai_transport_host_reset();
    fakes_reset();
}

static void test_reserve_handoff(void)
{
    for (int net_first = 0; net_first < 2; ++net_first) {
        for (int taken = 0; taken < 2; ++taken) {
            for (int path = 0; path < PATH_COUNT; ++path) {
                reserve_handoff_case(taken != 0, (reserve_path_t)path, net_first != 0);
            }
        }
    }
}

/* The reserve is in place before the platform hears net.up (real dispatch). */
static void test_reserve_before_net_up(void)
{
    const xiaotai_transport_config_t config = {.start_reserve_bytes = START_RESERVE};
    boot(&config);
    const xiaotai_args_t connecting = {.value = XIAOTAI_NET_CONNECTING};
    const xiaotai_args_t up = {.value = 1};
    CHECK(xiaotai_event_emit(XIAOTAI_EVENT_NET_STATE, &connecting, NULL, 0) == XIAOTAI_OK);
    CHECK(xiaotai_event_emit(XIAOTAI_EVENT_NET_UP, &up, NULL, 0) == XIAOTAI_OK);
    settle();
    CHECK(g_observed.net_up_seen && g_observed.start_reserve_at_net_up);
    shutdown_all();
}

/* ----------------------------------- start hold and connect reserve order */

#define CONNECT_RESERVE 17408U

static void test_start_hold_and_connect_reserve(void)
{
    const xiaotai_transport_config_t config = {.start_before_signaling = true,
                                               .start_reserve_bytes = START_RESERVE,
                                               .connect_reserve_bytes = CONNECT_RESERVE};
    fakes_reset();
    fake_platform_hold_auto(false);
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    boot_with(&config, (boot_options_t){.keep_fakes = true});
    /* Taken in start(), before the platform could log in. */
    fake_hold_record_t records[8];
    CHECK(fake_platform_hold_records(records, 8) == 1U && records[0].take);
    CHECK(records[0].state == XIAOTAI_TRANSPORT_DOWN);
    give_account();
    /* Signaling not gone yet: the stack waits, the start reserve stays. */
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 0U && xiaotai_transport_state() == XIAOTAI_TRANSPORT_STARTING);
    CHECK(start_reserve_held());
    fake_platform_suspend();
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U && !start_reserve_held());
    CHECK(fake_platform_holds_active() == 1U);
    xiaotai_transport_host_started();
    settle();
    CHECK(fake_platform_hold_records(records, 8) == 2U && !records[1].take);
    CHECK(records[1].connect_reserve_held);     /* the reserve first, then signaling */
    CHECK(fake_platform_holds_active() == 0U && status_now().signaling_yields == 1U);
    shutdown_all();

    /* The reserve missed: signaling is given back anyway (default) and the
     * reserve is tried again 5000 ms later. */
    fakes_reset();
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    boot_with(&config, (boot_options_t){.keep_fakes = true});
    give_account();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    xiaotai_mem_host_fail_nth(1);
    xiaotai_transport_host_started();
    settle();
    const int64_t online_at = now_ms();
    CHECK(fake_platform_hold_records(records, 8) == 2U && !records[1].take && !records[1].connect_reserve_held);
    CHECK(!status_now().connect_reserve_held && !status_now().connect_ready);
    advance(4999);
    CHECK(!status_now().connect_reserve_held);
    advance(1);
    CHECK(status_now().connect_reserve_held && status_now().connect_ready && now_ms() == online_at + 5000);
    shutdown_all();

    /* start_hold_until_reserve: signaling waits for the reserve. */
    xiaotai_transport_config_t until = config;
    until.start_hold_until_reserve = true;
    fakes_reset();
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    boot_with(&until, (boot_options_t){.keep_fakes = true});
    give_account();
    xiaotai_mem_host_fail_nth(1);
    xiaotai_transport_host_started();
    settle();
    CHECK(fake_platform_holds_active() == 1U);
    advance(5000);
    CHECK(fake_platform_holds_active() == 0U);
    const size_t count = fake_platform_hold_records(records, 8);
    CHECK(count == 2U && records[1].connect_reserve_held && records[1].at_ms == online_at + 5000);
    shutdown_all();
}

static void test_start_hold_limit(void)
{
    /* 0: the hold lasts until the stack is online. */
    const xiaotai_transport_config_t forever = {.start_before_signaling = true};
    fakes_reset();
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    boot_with(&forever, (boot_options_t){.keep_fakes = true});
    give_account();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    advance(60000);
    CHECK(fake_platform_holds_active() == 1U);
    xiaotai_transport_host_started();
    settle();
    CHECK(fake_platform_holds_active() == 0U);
    shutdown_all();

    /* 3000: given back at that instant; the stack keeps starting. */
    const xiaotai_transport_config_t limited = {.start_before_signaling = true, .start_signaling_hold_max_ms = 3000};
    fakes_reset();
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    boot_with(&limited, (boot_options_t){.keep_fakes = true});
    give_account();
    advance(2999);
    CHECK(fake_platform_holds_active() == 1U);
    advance(1);
    CHECK(fake_platform_holds_active() == 0U && xiaotai_transport_state() == XIAOTAI_TRANSPORT_STARTING);
    fake_hold_record_t records[8];
    CHECK(fake_platform_hold_records(records, 8) == 2U && records[1].at_ms == T0 + 3000);
    xiaotai_transport_host_started();
    settle();
    CHECK(fake_platform_hold_records(records, 8) == 2U && xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    shutdown_all();

    /* A failed start gives the hold back; the retry takes a new one. */
    fakes_reset();
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    boot_with(&forever, (boot_options_t){.keep_fakes = true});
    xiaotai_transport_host_fail_start(XIAOTAI_FAIL, -2, 1);
    give_account();
    CHECK(fake_platform_holds_active() == 0U);
    advance(XT_START_RETRY_DEFAULT_MS);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 2U && fake_platform_holds_active() == 1U);
    CHECK(fake_platform_hold_records(records, 8) == 3U);
    shutdown_all();
}

static void test_connect_reserve_retry_while_stopping(void)
{
    const xiaotai_transport_config_t config = {.connect_reserve_bytes = CONNECT_RESERVE};
    boot(&config);
    give_account();
    xiaotai_mem_host_fail_nth(1);
    xiaotai_transport_host_started();
    settle();
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    CHECK(!status_now().connect_reserve_held && logged("reserve_miss"));
    uint32_t hold = 0;
    CHECK(xiaotai_transport_hold(&hold) == XIAOTAI_OK);
    settle();
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_STOPPING);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STOP) == 1U);
    advance(5000);
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_STOPPING);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 0U && !status_now().connect_reserve_held);
    xiaotai_transport_host_stopped(0);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_UNINIT) == 1U && status_now().wait == XIAOTAI_TRANSPORT_WAIT_HOLD);
    shutdown_all();
}

static void test_hold_abandons_early_signaling_yield(void)
{
    const xiaotai_transport_config_t config = {.start_before_signaling = true};
    fakes_reset();
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    boot_with(&config, (boot_options_t){.keep_fakes = true});
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 0U && fake_platform_holds_active() == 1U);
    uint32_t hold = 0;
    CHECK(xiaotai_transport_hold(&hold) == XIAOTAI_OK);
    settle();
    CHECK(fake_platform_holds_active() == 0U);
    give_account();
    CHECK(status_now().wait == XIAOTAI_TRANSPORT_WAIT_HOLD && calls(XIAOTAI_TRANSPORT_HOST_START) == 0U);
    xiaotai_transport_release(hold);
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U && fake_platform_holds_active() == 1U);
    xiaotai_transport_host_started();
    settle();
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE && fake_platform_holds_active() == 0U);
    shutdown_all();
}

/* ------------------------------------------------------ signaling yields */

static atomic_bool g_use_heap_sample;
static atomic_size_t g_free_bytes, g_largest_block;
enum {HOLD_NORMAL, HOLD_REFUSED, HOLD_DESTROY_FAILED};
static atomic_int g_hold_failure;
static xiaotai_platform_done_fn_t g_real_suspended;
static void *g_real_suspended_ctx;

void __real_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats);
void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    __real_xiaotai_mem_stats(kind, stats);
    if (stats && kind == XIAOTAI_MEM_INTERNAL && atomic_load(&g_use_heap_sample)) {
        stats->free = atomic_load(&g_free_bytes);
        stats->largest_block = atomic_load(&g_largest_block);
    }
}

xiaotai_err_t __real_xiaotai_platform_signaling_hold(xiaotai_platform_done_fn_t done, void *ctx, uint32_t *hold);
static void fail_suspended(xiaotai_err_t result, void *ctx)
{
    CHECK(result == XIAOTAI_OK && g_real_suspended);
    g_real_suspended(XIAOTAI_ERR_INVALID_STATE, ctx);
}

xiaotai_err_t __wrap_xiaotai_platform_signaling_hold(xiaotai_platform_done_fn_t done, void *ctx, uint32_t *hold)
{
    const int failure = atomic_load(&g_hold_failure);
    if (failure == HOLD_REFUSED) return XIAOTAI_ERR_NO_MEM;
    g_real_suspended = done;
    g_real_suspended_ctx = ctx;
    if (failure == HOLD_DESTROY_FAILED) {
        return __real_xiaotai_platform_signaling_hold(fail_suspended, ctx, hold);
    }
    return __real_xiaotai_platform_signaling_hold(done, ctx, hold);
}

static const xiaotai_transport_config_t s_s3_connect_memory = {
    .connect_reserve_bytes = 17U * 1024U,
    .yield_connect_free_bytes = 64U * 1024U,
    .yield_connect_block_bytes = 32U * 1024U,
};

typedef struct {
    uint32_t generation, timeout_ms, closed;
    xiaotai_link_end_t reason;
    int detail;
} memory_request_t;

static void memory_closed(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    memory_request_t *o = ctx;
    CHECK(o->generation == generation);
    ++o->closed;
    o->reason = reason;
    o->detail = detail;
}

static void owner_memory_connect(void *ctx)
{
    memory_request_t *o = ctx;
    const xiaotai_link_request_t request = {
        .kind = XIAOTAI_LINK_WHIP, .peer = "http://peer.test", .token = "copied-token", .timeout_ms = o->timeout_ms,
        .profile = {.audio = {.send = true, .send_stream = 1, .send_format = {XIAOTAI_LINK_CODEC_G711A, 8000, 1}},
                    .command_max_bytes = 24, .command_pool_bytes = 48},
    };
    const xiaotai_link_handlers_t handlers = {.closed = memory_closed, .ctx = o};
    CHECK(xiaotai_link_connect(&request, &handlers, &o->generation) == XIAOTAI_OK && o->generation);
}

static void memory_connect(memory_request_t *o)
{
    CHECK(xiaotai_run(owner_memory_connect, o) == XIAOTAI_OK);
    settle();
}

static void owner_memory_cancel(void *ctx)
{
    xiaotai_link_disconnect(((memory_request_t *)ctx)->generation);
}

static void memory_answer(const memory_request_t *o)
{
    xiaotai_transport_host_connected(o->generation, 0, XIAOTAI_LINK_END_CONNECT_FAILED, -41);
    settle();
}

static void test_connect_memory_thresholds(void)
{
    const uint32_t free_min = s_s3_connect_memory.yield_connect_free_bytes;
    const uint32_t block_min = s_s3_connect_memory.yield_connect_block_bytes;
    const struct {uint32_t free, block; bool suspend;} samples[] = {
        {free_min, block_min, false},
        {free_min - 1U, block_min, true},
        {free_min, block_min - 1U, true},
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        boot_online(&s_s3_connect_memory);
        fake_platform_hold_auto(false);
        atomic_store(&g_free_bytes, samples[i].free);
        atomic_store(&g_largest_block, samples[i].block);
        atomic_store(&g_use_heap_sample, true);
        const uint64_t original_bytes = status_now().engine_bytes;
        memory_request_t request = {0};
        memory_connect(&request);
        if (samples[i].suspend) {
            CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 0U && fake_platform_holds_active() == 1U);
            CHECK(status_now().connect_reserve_held && status_now().engine_bytes > original_bytes);
            fake_platform_suspend();
            settle();
        } else {
            CHECK(fake_platform_holds_active() == 0U);
        }
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1U && !status_now().connect_reserve_held);
        CHECK(xt_connect_memory_check() == XIAOTAI_ERR_NO_MEM);
        memory_answer(&request);
        CHECK(request.closed == 1U && request.detail == -41);
        CHECK(status_now().connect_ready && status_now().engine_bytes == original_bytes);
        CHECK(fake_platform_holds_active() == 0U);
        fake_hold_record_t holds[2];
        const size_t count = fake_platform_hold_records(holds, 2);
        CHECK(count == (samples[i].suspend ? 2U : 0U));
        if (count) CHECK(holds[0].take && !holds[1].take && holds[1].connect_reserve_held);
        atomic_store(&g_use_heap_sample, false);
        shutdown_all();
    }
    puts("connect-memory-thresholds: equality and each threshold minus one passed");
}

static void test_connect_memory_hold_failure(int failure)
{
    boot_online(&s_s3_connect_memory);
    fake_platform_hold_auto(false);
    const uint64_t base = status_now().engine_bytes;
    atomic_store(&g_hold_failure, failure);
    memory_request_t request = {0};
    memory_connect(&request);
    if (failure == HOLD_DESTROY_FAILED) {
        CHECK(!request.closed && !calls(XIAOTAI_TRANSPORT_HOST_CONNECT) && status_now().connect_reserve_held);
        fake_platform_suspend();
        settle();
    }
    const xiaotai_transport_status_t observed = status_now();
    CHECK(request.closed == 1U && request.detail == (failure == HOLD_REFUSED
              ? XIAOTAI_ERR_NO_MEM : XIAOTAI_ERR_INVALID_STATE));
    CHECK(request.reason == (failure == HOLD_REFUSED ? XIAOTAI_LINK_END_NO_MEMORY : XIAOTAI_LINK_END_CONNECT_FAILED));
    CHECK(!calls(XIAOTAI_TRANSPORT_HOST_CONNECT) && observed.connect_reserve_held && observed.connect_ready);
    CHECK(!observed.links_requested && !observed.links_releasing && !observed.signaling_suspended);
    CHECK(observed.engine_bytes == base && !fake_platform_holds_active());
    atomic_store(&g_hold_failure, HOLD_NORMAL);

    memory_request_t next = {0};
    memory_connect(&next);
    CHECK(next.generation != request.generation && !next.closed && !calls(XIAOTAI_TRANSPORT_HOST_CONNECT));
    fake_platform_suspend();
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1U && !status_now().connect_reserve_held);
    memory_answer(&next);
    CHECK(request.closed == 1U && next.closed == 1U && status_now().engine_bytes == base);
    shutdown_all();
    printf("hold_failure=%d: no RTC call; same error once, reserve intact; next generation passed\n", failure);
}

typedef struct {xiaotai_platform_done_fn_t done; void *ctx;} late_hold_t;
static void owner_deliver_late_hold(void *ctx)
{
    const late_hold_t *late = ctx;
    late->done(XIAOTAI_ERR_INVALID_STATE, late->ctx);
}

static void test_connect_memory_abandoned(bool timeout)
{
    boot_online(&s_s3_connect_memory);
    fake_platform_hold_auto(false);
    const uint64_t base = status_now().engine_bytes;
    memory_request_t first = {.timeout_ms = timeout ? 30U : 0U}, next = {0};
    memory_connect(&first);
    late_hold_t late = {.done = g_real_suspended, .ctx = g_real_suspended_ctx};
    CHECK(late.done && fake_platform_holds_active() == 1U && !calls(XIAOTAI_TRANSPORT_HOST_CONNECT));
    if (timeout) {
        advance(29);
        CHECK(!first.closed && fake_platform_holds_active() == 1U);
        advance(1);
        CHECK(first.closed == 1U && first.reason == XIAOTAI_LINK_END_TIMEOUT);
    } else {
        CHECK(xiaotai_run(owner_memory_cancel, &first) == XIAOTAI_OK);
        settle();
        CHECK(!first.closed);
    }
    CHECK(!fake_platform_holds_active() && status_now().engine_bytes == base && status_now().connect_reserve_held);
    memory_connect(&next);
    CHECK(next.generation != first.generation && fake_platform_holds_active() == 1U);
    CHECK(xiaotai_run(owner_deliver_late_hold, &late) == XIAOTAI_OK);
    settle();
    CHECK(!next.closed && !calls(XIAOTAI_TRANSPORT_HOST_CONNECT) && status_now().connect_reserve_held);
    fake_platform_suspend();
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1U && !status_now().connect_reserve_held);
    memory_answer(&next);
    CHECK(next.closed == 1U && status_now().engine_bytes == base && !fake_platform_holds_active());
    shutdown_all();
}

static void suspend_for_connect(memory_request_t *request)
{
    const size_t submits = calls(XIAOTAI_TRANSPORT_HOST_CONNECT);
    memory_connect(request);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == submits && fake_platform_holds_active() == 1U);
    fake_platform_suspend();
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == submits + 1U &&
          !status_now().connect_reserve_held && fake_platform_holds_active() == 1U);
}

static void test_connect_memory_owner(void)
{
    boot_online(&s_s3_connect_memory);
    fake_platform_hold_auto(false);
    memory_request_t old = {0}, current = {0};
    suspend_for_connect(&old);
    memory_answer(&old);
    CHECK(old.closed == 1U && status_now().connect_reserve_held && !fake_platform_holds_active());

    suspend_for_connect(&current);
    const uint64_t live_bytes = status_now().engine_bytes;
    const size_t live_holds = fake_platform_hold_records(NULL, 0);
    xiaotai_transport_host_connected(old.generation, 91, XIAOTAI_LINK_END_PEER, 0);
    memory_answer(&old);
    CHECK(old.closed == 1U && !current.closed && status_now().links_requested == 1U);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1U && status_now().late_links_closed == 1U);
    CHECK(!status_now().connect_reserve_held && status_now().engine_bytes == live_bytes &&
          fake_platform_hold_records(NULL, 0) == live_holds && fake_platform_holds_active() == 1U);
    xiaotai_transport_host_closed(91);
    settle();

    xiaotai_transport_host_connected(current.generation, 92, XIAOTAI_LINK_END_PEER, 0);
    settle();
    CHECK(status_now().links_open == 1U && status_now().connect_reserve_held && !fake_platform_holds_active());
    CHECK(fake_platform_hold_records(NULL, 0) == live_holds + 1U);
    xiaotai_transport_host_connected(current.generation, 92, XIAOTAI_LINK_END_PEER, 0);
    settle();
    CHECK(status_now().connect_reserve_held && fake_platform_hold_records(NULL, 0) == live_holds + 1U);
    xiaotai_transport_host_closed(92);
    settle();
    CHECK(current.closed == 1U);
    shutdown_all();
    puts("connect-reserve owner: stale success/failure left newer loan and signaling untouched; matching result restored once");
}

typedef enum { RETIRED_BY_POLL, RETIRED_BY_SUCCESS, RETIRED_BY_FAILURE } retire_method_t;

static void test_connect_memory_retired(retire_method_t method)
{
    boot_online(&s_s3_connect_memory);
    fake_platform_hold_auto(false);
    if (method == RETIRED_BY_POLL) {
        xiaotai_transport_host_set_caps(&(xiaotai_transport_host_caps_t){.cancel_pending = true});
        xiaotai_transport_host_pending(1);
    }
    memory_request_t cancelled = {0}, next = {0};
    suspend_for_connect(&cancelled);
    CHECK(xiaotai_run(owner_memory_cancel, &cancelled) == XIAOTAI_OK);
    settle();
    const size_t live_holds = fake_platform_hold_records(NULL, 0);
    CHECK(!cancelled.closed && status_now().links_releasing == 1U && !status_now().connect_reserve_held);
    CHECK(fake_platform_holds_active() == 1U);
    if (method == RETIRED_BY_POLL) {
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_CANCEL) == 1U);
        advance(20);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_PENDING) == 1U && status_now().links_releasing == 1U &&
              !status_now().connect_reserve_held && fake_platform_holds_active() == 1U);
        xiaotai_transport_host_pending(0);
        advance(20);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_PENDING) == 2U);
    } else if (method == RETIRED_BY_SUCCESS) {
        xiaotai_transport_host_connected(cancelled.generation, 93, XIAOTAI_LINK_END_PEER, 0);
        settle();
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1U);
        xiaotai_transport_host_closed(93);
        settle();
    } else {
        memory_answer(&cancelled);
    }
    CHECK(!cancelled.closed && !status_now().links_releasing && status_now().connect_reserve_held &&
          !fake_platform_holds_active() && fake_platform_hold_records(NULL, 0) == live_holds + 1U);
    suspend_for_connect(&next);
    memory_answer(&cancelled);
    CHECK(!cancelled.closed && !next.closed && !status_now().connect_reserve_held &&
          fake_platform_holds_active() == 1U);
    memory_answer(&next);
    CHECK(next.closed == 1U && status_now().connect_reserve_held && !fake_platform_holds_active());
    shutdown_all();
    printf("connect-reserve retirement=%d: only its pending=0 proof or matching result restored reserve\n", method);
}

static void test_connect_yield(void)
{
    /* Thresholds set and (host statistics read 0) memory short: signaling
     * is suspended before the reserve is lent, given back after the
     * reserve is in place again. */
    const xiaotai_transport_config_t config = {.links = 2,
                                               .connect_reserve_bytes = CONNECT_RESERVE,
                                               .yield_connect_free_bytes = 65536,
                                               .yield_connect_block_bytes = 32768};
    boot_online(&config);
    fake_platform_hold_auto(false);
    memory_request_t first = {0}, second = {0};
    memory_connect(&first);
    memory_connect(&second);
    CHECK(!calls(XIAOTAI_TRANSPORT_HOST_CONNECT) && fake_platform_holds_active() == 1U && status_now().connect_reserve_held);
    fake_platform_suspend();
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1U && !status_now().connect_reserve_held && !status_now().connect_ready);
    CHECK(xt_connect_memory_check() == XIAOTAI_ERR_NO_MEM && status_now().connect_memory_misses == 1U);
    /* Taking the reserve back fails once: signaling stays suspended and the
     * reserve is tried every 1000 ms. */
    xiaotai_mem_host_fail_nth(1);
    memory_answer(&first);
    CHECK(first.closed == 1 && !second.closed && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1U);
    CHECK(!status_now().connect_reserve_held && fake_platform_holds_active() == 1U);
    advance(999);
    CHECK(fake_platform_holds_active() == 1U);
    advance(1);
    CHECK(status_now().connect_reserve_held && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1U);
    fake_platform_suspend();
    settle();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 2U && !status_now().connect_reserve_held);
    memory_answer(&second);
    CHECK(second.closed == 1 && !fake_platform_holds_active() && status_now().signaling_yields == 2U &&
          status_now().connect_ready);
    shutdown_all();

    /* No thresholds: never suspends. */
    const xiaotai_transport_config_t plain = {.connect_reserve_bytes = CONNECT_RESERVE};
    boot_online(&plain);
    first = (memory_request_t){0};
    memory_connect(&first);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1U && fake_platform_holds_active() == 0U &&
          !status_now().connect_reserve_held);
    memory_answer(&first);
    CHECK(status_now().connect_reserve_held && status_now().signaling_yields == 0U);
    shutdown_all();
}

static void end_links(void) { xt_links_end_all(XIAOTAI_LINK_END_PEER, 0); }

static void test_yield_while_open(void)
{
    const xiaotai_transport_config_t config = {.yield_while_open = true};
    boot_online(&config);
    CHECK(fake_platform_holds_active() == 0U);
    open_link(12, 0x77);
    CHECK(fake_platform_holds_active() == 1U && status_now().signaling_suspended);
    /* Ended but still held by the stack: signaling stays suspended. */
    on_worker(end_links);
    CHECK(fake_platform_holds_active() == 1U && status_now().links_releasing == 1U);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1U);
    xiaotai_transport_host_closed(0x77);
    settle();
    CHECK(fake_platform_holds_active() == 0U && calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 1U);
    CHECK(g_observed.link_count >= 1U && g_observed.links[g_observed.link_count - 1U] == 0);
    shutdown_all();

    boot_online(NULL);
    open_link(13, 0x78);
    CHECK(fake_platform_holds_active() == 0U);
    shutdown_all();
}

/* ------------------------------------------------------------ statistics */

static void test_statistics(void)
{
    const xiaotai_transport_host_caps_t caps = {.https = true, .media_transport = true, .stack_log = true,
                                                .video = true, .statistics = true};
    xiaotai_transport_host_reset();
    boot_online(NULL);
    xiaotai_transport_host_set_caps(&caps);
    fake_net_trace(true, false);
    emit(XIAOTAI_EVENT_NET_TRACE, 1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STATISTICS) == 0U);
    fake_net_trace(true, true);
    emit(XIAOTAI_EVENT_NET_TRACE, 1);
    emit(XIAOTAI_EVENT_NET_TRACE, 1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STATISTICS) == 1U);
    xiaotai_transport_host_call_t list[16];
    size_t n = xiaotai_transport_host_calls(list, 16);
    CHECK(list[n - 1U].kind == XIAOTAI_TRANSPORT_HOST_STATISTICS && list[n - 1U].value == 1);
    fake_net_trace(false, false);
    emit(XIAOTAI_EVENT_NET_TRACE, 0);
    n = xiaotai_transport_host_calls(list, 16);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STATISTICS) == 2U && list[n - 1U].value == 0);
    shutdown_all();

    /* Without the capability nothing is switched. */
    boot_online(NULL);
    fake_net_trace(true, true);
    emit(XIAOTAI_EVENT_NET_TRACE, 1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_STATISTICS) == 0U);
    shutdown_all();
}

/* ------------------------------------------------------------------ alert */

static void test_alert(void)
{
    boot_online(NULL);
    open_link(20, 0x20);
    xiaotai_transport_host_alert();
    settle();
    CHECK(status_now().alerts == 1U && atomic_load(&xt.slots[0].generation) == 20U);
    shutdown_all();

    const xiaotai_transport_config_t ends = {.alert_ends_links = true};
    boot_online(&ends);
    open_link(21, 0x21);
    xiaotai_transport_host_alert();
    settle();
    CHECK(link_ended(21, XIAOTAI_LINK_END_ALERT));
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE && calls(XIAOTAI_TRANSPORT_HOST_STOP) == 0U);
    shutdown_all();
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "connect-memory-owner") == 0) {
        test_connect_memory_owner();
        test_connect_memory_retired(RETIRED_BY_POLL);
        test_connect_memory_retired(RETIRED_BY_SUCCESS);
        test_connect_memory_retired(RETIRED_BY_FAILURE);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "connect-memory-thresholds") == 0) {
        test_connect_memory_thresholds();
        test_connect_memory_hold_failure(HOLD_REFUSED);
        test_connect_memory_hold_failure(HOLD_DESTROY_FAILED);
        test_connect_memory_abandoned(false);
        test_connect_memory_abandoned(true);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "start-worker-failure") == 0) {
        test_start_worker_failure(false);
        test_start_worker_failure(true);
        return 0;
    }
    CHECK(argc == 1);
    test_configure();
    test_sizing();
    test_start_conditions();
    test_async_online();
    test_start_failure_retry();
    test_start_failure_on_event();
    test_retry_deadline_while_gated();
    test_start_failure_fault();
    test_unsolicited_stop();
    test_account_change();
    test_holds();
    test_restart_running();
    test_fault_releases_stack();
    test_fault_stop_failed();
    test_fault_stop_timeout();
    test_restart_during_fault_stop();
    test_restart_waits_for_gate();
    test_fault_restart_ms();
    test_start_reserve();
    test_reserve_handoff();
    test_reserve_before_net_up();
    test_start_hold_and_connect_reserve();
    test_start_hold_limit();
    test_connect_reserve_retry_while_stopping();
    test_hold_abandons_early_signaling_yield();
    test_connect_yield();
    test_yield_while_open();
    test_statistics();
    test_alert();
    printf("transport_run: all checks passed\n");
    return 0;
}
