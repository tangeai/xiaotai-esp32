// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_inbound_admission.py
// Replaces legacy: tests/regression/test_p4_inbound_admission.py
#include <pthread.h>

#include "support.h"

typedef struct {
    _Atomic unsigned opened, closed, guard_calls;
    _Atomic uint32_t generation;
    _Atomic bool busy;
} observer_t;
static observer_t guarded, ordinary, once;
static uint32_t guarded_id, ordinary_id, once_id;
static xiaotai_signal_t blocked, resume;
static _Atomic bool deny_links, saw_denial;
static _Atomic bool reject_opened;
static uint32_t off_id;
static bool retiring;
static xiaotai_signal_t callback_in_gate, release_callback, owner_at_gate;
static _Atomic bool hold_callback, owner_attempted, owner_finished;
static _Thread_local bool watch_gate;
static _Thread_local unsigned transport_lock_depth;

void __real_xiaotai_critical_enter(xiaotai_critical_t *lock);
void __real_xiaotai_critical_exit(xiaotai_critical_t *lock);

void __wrap_xiaotai_critical_enter(xiaotai_critical_t *lock)
{
    if (watch_gate) {
        CHECK(lock == &xt.lock && !transport_lock_depth);
        watch_gate = false;
        atomic_store(&owner_attempted, true);
        xiaotai_signal_give(&owner_at_gate);
    }
    __real_xiaotai_critical_enter(lock);
    if (lock == &xt.lock) ++transport_lock_depth;
}

void __wrap_xiaotai_critical_exit(xiaotai_critical_t *lock)
{
    if (lock == &xt.lock) {
        CHECK(transport_lock_depth == 1U);
        --transport_lock_depth;
    }
    __real_xiaotai_critical_exit(lock);
}

xiaotai_err_t __real_xiaotai_event_emit(const char *name, const xiaotai_args_t *args, const void *data, size_t bytes);
xiaotai_err_t __wrap_xiaotai_event_emit(const char *name, const xiaotai_args_t *args, const void *data, size_t bytes)
{
    if (atomic_load(&deny_links) && strcmp(name, XIAOTAI_EVENT_TRANSPORT_LINKS) == 0) {
        atomic_store(&saw_denial, true);
        return XIAOTAI_ERR_NO_MEM;
    }
    return __real_xiaotai_event_emit(name, args, data, bytes);
}

static bool admit(void *ctx)
{
    observer_t *observer = ctx;
    atomic_fetch_add(&observer->guard_calls, 1);
    if (observer == &guarded && atomic_exchange(&hold_callback, false)) {
        /* The real acceptance path has chosen OPEN and a free slot, but has
         * not published the handle. Let the owner contend for its lock here. */
        CHECK(transport_lock_depth == 1U);
        xiaotai_signal_give(&callback_in_gate);
        CHECK(xiaotai_signal_take(&release_callback, 3000));
    }
    return !atomic_load(&observer->busy);
}

static void opened(void *ctx, uint32_t generation)
{
    observer_t *observer = ctx;
    CHECK(generation != 0);
    atomic_store(&observer->generation, generation);
    atomic_fetch_add(&observer->opened, 1);
    if (observer == &guarded && atomic_load(&reject_opened)) {
        /* HTTP can become busy just after transport's final guard passes. */
        atomic_store(&guarded.busy, true);
        xiaotai_link_disconnect(generation);
        retiring = xiaotai_link_listener_retiring(guarded_id);
    }
}

static void closed(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    (void)reason;
    (void)detail;
    observer_t *observer = ctx;
    CHECK(generation != 0);
    atomic_fetch_add(&observer->closed, 1);
}

static void register_listeners(void)
{
    xiaotai_link_listener_t listener = {
        .name = "guarded", .handlers = {.opened = opened, .closed = closed, .ctx = &guarded}, .admit_open = admit};
    CHECK(xiaotai_link_listen(&listener, &guarded_id) == XIAOTAI_OK);
    CHECK(xiaotai_link_listen_set(guarded_id, XIAOTAI_LINK_LISTEN_OPEN) == XIAOTAI_OK);
    listener.name = "ordinary";
    listener.handlers.ctx = &ordinary;
    listener.admit_open = NULL;
    CHECK(xiaotai_link_listen(&listener, &ordinary_id) == XIAOTAI_OK);
    CHECK(xiaotai_link_listen_set(ordinary_id, XIAOTAI_LINK_LISTEN_OPEN) == XIAOTAI_OK);
    listener.name = "once";
    listener.handlers.ctx = &once;
    listener.admit_open = admit;
    CHECK(xiaotai_link_listen(&listener, &once_id) == XIAOTAI_OK);
}

static void owner_mode(void *ctx)
{
    const uint32_t *args = ctx;
    CHECK(xiaotai_link_listen_set(args[0], (xiaotai_link_listen_t)args[1]) == XIAOTAI_OK);
    retiring = xiaotai_link_listener_retiring(args[0]);
}

static void mode(uint32_t id, xiaotai_link_listen_t new_mode)
{
    const uint32_t args[] = {id, new_mode};
    CHECK(xiaotai_run(owner_mode, (void *)args) == XIAOTAI_OK);
    owner_sync();
}

static void owner_block(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&blocked);
    CHECK(xiaotai_signal_take(&resume, 3000));
    if (off_id) {
        CHECK(xiaotai_link_listen_set(off_id, XIAOTAI_LINK_LISTEN_OFF) == XIAOTAI_OK);
        retiring = xiaotai_link_listener_retiring(off_id);
    }
}

static void block_owner(uint32_t id)
{
    off_id = id;
    CHECK(xiaotai_run(owner_block, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&blocked, 3000));
}

static void owner_hold(void *ctx)
{
    const uint32_t *args = ctx;
    CHECK(xiaotai_link_listener_block_inbound(0, true) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_link_listener_block_inbound(args[0], args[1] != 0U) == XIAOTAI_OK);
}

static void hold(uint32_t id, bool blocked)
{
    const uint32_t args[] = {id, blocked};
    CHECK(xiaotai_run(owner_hold, (void *)args) == XIAOTAI_OK);
    owner_sync();
}

static void end_open(void *unused)
{
    (void)unused;
    xiaotai_link_disconnect(atomic_load(&ordinary.generation));
}

static void refused(xiaotai_rtc_conn_t conn, unsigned count)
{
    const uint32_t base = xiaotai_generation_next();
    const size_t disconnects = calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
    xiaotai_transport_host_accepted(conn);
    settle();
    CHECK(status_now().inbound_refused == count && xiaotai_generation_next() == base + 1U);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == disconnects + 1U);
    xiaotai_transport_host_closed(conn);
    settle();
}

static void test_cancel_block(void)
{
    const xiaotai_transport_config_t config = {.links = 1, .once_consumed_on_refusal = true};
    g_observer_start = register_listeners;
    boot_online(&config);
    CHECK(status_now().links_max == 1);
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OFF);
    xiaotai_transport_host_accepted(10); // H5 was already present when cancellation fired.
    settle();
    CHECK(atomic_load(&ordinary.opened) == 1 && status_now().links_open == 1);

    hold(once_id, true);
    mode(once_id, XIAOTAI_LINK_LISTEN_ONCE); // A new real dial is armed underneath the block.
    refused(11, 1);
    refused(12, 2);
    CHECK(status_now().links_open == 1 && !atomic_load(&ordinary.closed) && !atomic_load(&once.opened));
    CHECK(xiaotai_run(end_open, NULL) == XIAOTAI_OK);
    settle();
    CHECK(status_now().links_releasing == 1 && !status_now().links_open);
    refused(13, 3); // No physical slot while the existing link retires.
    xiaotai_transport_host_closed(10);
    settle();
    CHECK(!status_now().links_releasing);
    refused(14, 4); // Free slot, but still no H5 handoff.

    hold(guarded_id, true);
    hold(once_id, false);
    refused(15, 5); // Another owner cannot release this hold.
    CHECK(atomic_load(&ordinary.opened) == 1 && !atomic_load(&once.opened));
    hold(guarded_id, false);
    xiaotai_transport_host_accepted(16);
    settle();
    CHECK(atomic_load(&once.opened) == 1 && atomic_load(&ordinary.opened) == 1);
    xiaotai_transport_host_closed(16);
    settle();
    xiaotai_transport_host_accepted(17);
    settle();
    CHECK(atomic_load(&ordinary.opened) == 2);

    mode(once_id, XIAOTAI_LINK_LISTEN_ONCE);
    refused(18, 6); // The product still consumes ordinary ONCE on slot refusal.
    xiaotai_transport_host_closed(17);
    settle();
    xiaotai_transport_host_accepted(19);
    settle();
    CHECK(atomic_load(&ordinary.opened) == 3 && atomic_load(&once.opened) == 1);
    xiaotai_transport_host_closed(19);
    settle();
    shutdown_all();
}

static void test_admission_orders(void)
{
    CHECK(xiaotai_signal_init(&blocked) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&resume) == XIAOTAI_OK);
    g_observer_start = register_listeners;
    boot_online(&(xiaotai_transport_config_t){.links = 2});
    mode(ordinary_id, XIAOTAI_LINK_LISTEN_OFF);
    for (unsigned i = 0; i < 32; ++i) {
        const uintptr_t conn = 100U + 2U * i;
        mode(guarded_id, XIAOTAI_LINK_LISTEN_OFF);
        refused(conn, i + 1U); /* The close reaches the owner before the stack callback. */

        mode(guarded_id, XIAOTAI_LINK_LISTEN_OPEN);
        block_owner(guarded_id);
        const uint32_t disconnects = calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
        xiaotai_transport_host_accepted(conn + 1U);
        CHECK(xiaotai_transport_host_settle(3000));
        CHECK(status_now().links_open == 1); /* Claimed by the stack, not handed to a module yet. */
        xiaotai_signal_give(&resume);
        settle();
        CHECK(retiring && !status_now().links_open && status_now().links_releasing == 1);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == disconnects + 1U);
        CHECK(!atomic_load(&guarded.opened) && !atomic_load(&guarded.closed));
        xiaotai_transport_host_closed(conn + 1U);
        settle();
        CHECK(!status_now().links_releasing);
    }
    mode(ordinary_id, XIAOTAI_LINK_LISTEN_OPEN);
    xiaotai_transport_host_accepted(201);
    settle();
    mode(once_id, XIAOTAI_LINK_LISTEN_ONCE);
    xiaotai_transport_host_accepted(202);
    settle();
    CHECK(atomic_load(&ordinary.opened) == 1 && atomic_load(&once.opened) == 1 && status_now().links_open == 2);
    refused(203, 33);
    CHECK(xiaotai_run(end_open, NULL) == XIAOTAI_OK);
    settle();
    xiaotai_transport_host_closed(201);
    settle();
    CHECK(status_now().links_open == 1 && !status_now().links_releasing && !atomic_load(&once.closed));
    xiaotai_transport_host_closed(202);
    settle();
    CHECK(!status_now().links_open && atomic_load(&once.closed) == 1);
    shutdown_all();
    xiaotai_signal_deinit(&resume);
    xiaotai_signal_deinit(&blocked);
}

static unsigned link_notices(uint32_t generation, const char *label)
{
    xiaotai_eventlog_entry_t entries[64];
    unsigned found = 0;
    uint32_t after = 0;
    for (;;) {
        const size_t n = xiaotai_eventlog_read(after, entries, 64);
        if (!n) return found;
        for (size_t i = 0; i < n; ++i) {
            const xiaotai_eventlog_entry_t *event = &entries[i];
            if (event->generation == generation && strcmp(event->source, "link") == 0 &&
                strcmp(event->label, label) == 0) ++found;
        }
        after = entries[n - 1U].sequence;
    }
}

static void expect_retirement(uint32_t listener, xiaotai_link_listen_t arm, uintptr_t conn, bool notify)
{
    const uint32_t disconnects = calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
    mode(listener, arm);
    block_owner(listener);
    xiaotai_transport_host_accepted(conn);
    CHECK(xiaotai_transport_host_settle(3000));
    CHECK(status_now().links_open == 1);
    uint32_t generation = 0;
    xiaotai_critical_enter(&xt.lock);
    for (uint8_t i = 0; i < xt.links_max; ++i)
        if (xt.slots[i].conn == conn) generation = atomic_load(&xt.slots[i].generation);
    xiaotai_critical_exit(&xt.lock);
    CHECK(generation);
    xiaotai_signal_give(&resume);
    settle();
    CHECK(retiring && !status_now().links_open && status_now().links_releasing == 1);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == disconnects + 1U);
    CHECK(link_notices(generation, "inbound_open_retired") == (unsigned)notify);
    CHECK(link_notices(generation, "link_end") == 1U && !link_notices(generation, "link_open"));
    xiaotai_rtc_on_closed_for(conn, generation + 1U);
    settle();
    CHECK(status_now().links_releasing == 1);
    xiaotai_rtc_on_closed_for(conn, generation);
    settle();
    CHECK(!status_now().links_releasing);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == disconnects + 1U);
    CHECK(link_notices(generation, "inbound_open_retired") == (unsigned)notify);
}

static void test_retired_open(bool notify)
{
    CHECK(xiaotai_signal_init(&blocked) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&resume) == XIAOTAI_OK);
    g_observer_start = register_listeners;
    boot_online(&(xiaotai_transport_config_t){.links = 2, .once_consumed_on_refusal = true,
                                               .report_retired_open = notify});
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OFF);
    mode(ordinary_id, XIAOTAI_LINK_LISTEN_OFF);
    for (unsigned i = 0; i < 32U; ++i) {
        refused(400U, i + 1U); /* OFF won before taking any slot or generation. */
        expect_retirement(ordinary_id, XIAOTAI_LINK_LISTEN_OPEN, 400U, notify);
    }
    expect_retirement(once_id, XIAOTAI_LINK_LISTEN_ONCE, 400U, false);
    expect_retirement(guarded_id, XIAOTAI_LINK_LISTEN_OPEN, 400U, false);
    CHECK(!atomic_load(&ordinary.opened) && !atomic_load(&ordinary.closed));
    CHECK(!atomic_load(&guarded.opened) && !atomic_load(&guarded.closed));
    CHECK(!atomic_load(&once.opened) && !atomic_load(&once.closed));
    shutdown_all();
    xiaotai_signal_deinit(&resume);
    xiaotai_signal_deinit(&blocked);
}

static uint32_t pending_admission(uintptr_t conn, uint32_t listener)
{
    uint32_t generation = 0;
    xiaotai_critical_enter(&xt.lock);
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        const xt_slot_t *slot = &xt.slots[i];
        if (slot->conn != conn) continue;
        const uint32_t flags = atomic_load(&slot->flags);
        CHECK(slot->listener_id == listener && atomic_load(&slot->state) == XT_SLOT_OPEN);
        CHECK((flags & (XT_SLOT_OPENED | XT_SLOT_REPORTED)) == XT_SLOT_OPENED);
        generation = atomic_load(&slot->generation);
    }
    xiaotai_critical_exit(&xt.lock);
    CHECK(generation);
    return generation;
}

static unsigned disconnect_count(uintptr_t conn)
{
    xiaotai_transport_host_call_t log[128];
    const size_t count = xiaotai_transport_host_calls(log, sizeof(log) / sizeof(*log));
    unsigned found = 0;
    for (size_t i = 0; i < count; ++i) {
        if (log[i].kind == XIAOTAI_TRANSPORT_HOST_DISCONNECT && log[i].value == (int64_t)conn) ++found;
    }
    return found;
}

static void owner_disconnect_generation(void *ctx) { xiaotai_link_disconnect(*(const uint32_t *)ctx); }

static void test_p4_call_selector(void)
{
    CHECK(xiaotai_signal_init(&blocked) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&resume) == XIAOTAI_OK);
    g_observer_start = register_listeners;
    boot_online(&(xiaotai_transport_config_t){.links = 2, .once_consumed_on_refusal = true, .report_retired_open = true});
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OFF);
    mode(ordinary_id, XIAOTAI_LINK_LISTEN_OFF);
    refused(31U, 1U);

    mode(once_id, XIAOTAI_LINK_LISTEN_ONCE);
    xiaotai_transport_host_accepted(55U);
    settle();
    const uint32_t first = atomic_load(&once.generation);
    CHECK(first && status_now().links_open == 1 && atomic_load(&once.opened) == 1);
    refused(32U, 2U); /* Consumed once; H5 is closed and cannot inherit the CALL. */
    CHECK(xiaotai_run(owner_disconnect_generation, (void *)&first) == XIAOTAI_OK);
    settle();
    CHECK(disconnect_count(55U) == 1 && status_now().links_releasing == 1);
    xiaotai_rtc_on_closed_for(55U, first);
    settle();
    CHECK(!status_now().links_releasing);

    mode(ordinary_id, XIAOTAI_LINK_LISTEN_OPEN);
    mode(once_id, XIAOTAI_LINK_LISTEN_ONCE);
    block_owner(0);
    xiaotai_transport_host_accepted(88U);
    CHECK(xiaotai_transport_host_settle(3000));
    const uint32_t chosen = pending_admission(88U, once_id);
    CHECK(chosen != first && atomic_load(&once.opened) == 1 && !atomic_load(&ordinary.opened));
    xiaotai_signal_give(&resume);
    settle();
    CHECK(atomic_load(&once.opened) == 2 && atomic_load(&once.generation) == chosen && !atomic_load(&ordinary.opened));
    CHECK(link_notices(chosen, "link_open") == 1U && !link_notices(chosen, "inbound_open_retired"));
    CHECK(xiaotai_run(owner_disconnect_generation, (void *)&chosen) == XIAOTAI_OK);
    settle();
    CHECK(disconnect_count(88U) == 1 && link_notices(chosen, "link_end") == 1U);
    xiaotai_rtc_on_closed_for(88U, chosen);
    settle();
    CHECK(!status_now().links_open && !status_now().links_releasing && !atomic_load(&once.closed));
    puts("PASS: one CALL listener selected before an earlier H5 OPEN; the once tag was consumed and released once");
    shutdown_all();
    xiaotai_signal_deinit(&resume);
    xiaotai_signal_deinit(&blocked);
}

static void owner_connect_ai(void *ctx)
{
    static observer_t ai;
    const xiaotai_link_request_t request = {.kind = XIAOTAI_LINK_WHIP, .peer = "http://peer.test", .token = "fixture"};
    const xiaotai_link_handlers_t handlers = {.opened = opened, .closed = closed, .ctx = &ai};
    CHECK(xiaotai_link_connect(&request, &handlers, ctx) == XIAOTAI_OK);
}

static void owner_retire_both(void *ctx)
{
    xiaotai_signal_give(&blocked);
    CHECK(xiaotai_signal_take(&resume, 3000));
    CHECK(xiaotai_link_listen_set(ordinary_id, XIAOTAI_LINK_LISTEN_OFF) == XIAOTAI_OK);
    xiaotai_link_disconnect(*(const uint32_t *)ctx);
}

static void test_p4_pending_h5_cleanup(void)
{
    CHECK(xiaotai_signal_init(&blocked) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&resume) == XIAOTAI_OK);
    g_observer_start = register_listeners;
    boot_online(&(xiaotai_transport_config_t){.links = 2, .once_consumed_on_refusal = true, .report_retired_open = true});
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OFF);

    uint32_t ai = 0;
    CHECK(xiaotai_run(owner_connect_ai, &ai) == XIAOTAI_OK);
    settle();
    CHECK(ai && status_now().links_requested == 1 && calls(XIAOTAI_TRANSPORT_HOST_CONNECT) == 1);
    xiaotai_transport_host_connected(ai, 9U, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    settle();
    CHECK(status_now().links_open == 1 && link_notices(ai, "link_open") == 1U);

    CHECK(xiaotai_run(owner_retire_both, &ai) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&blocked, 3000));
    xiaotai_transport_host_accepted(1U);
    CHECK(xiaotai_transport_host_settle(3000));
    const uint32_t h5 = pending_admission(1U, ordinary_id);
    CHECK(status_now().links_open == 2 && !atomic_load(&ordinary.opened));
    CHECK(xiaotai_link_receive(h5, XIAOTAI_LINK_AUDIO, NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    const uint32_t before_refusal = xiaotai_generation_next();
    xiaotai_transport_host_accepted(2U);
    CHECK(xiaotai_transport_host_settle(3000));
    CHECK(status_now().links_open == 2 && status_now().inbound_refused == 1);
    CHECK(xiaotai_generation_next() == before_refusal + 1U && disconnect_count(2U) == 1);
    CHECK(!disconnect_count(9U) && !disconnect_count(1U));
    xiaotai_transport_host_closed(2U);
    CHECK(xiaotai_transport_host_settle(3000));
    CHECK(!status_now().links_releasing); /* The refused handle never occupied either slot. */
    xiaotai_signal_give(&resume);
    settle();

    CHECK(!status_now().links_open && status_now().links_releasing == 2);
    CHECK(disconnect_count(9U) == 1 && disconnect_count(1U) == 1);
    CHECK(!atomic_load(&ordinary.opened) && !atomic_load(&ordinary.closed));
    CHECK(link_notices(h5, "inbound_open_retired") == 1 && !link_notices(h5, "link_open"));
    CHECK(link_notices(h5, "link_end") == 1U && link_notices(ai, "link_end") == 1U);
    xiaotai_rtc_on_closed_for(1U, ai); /* Only its own confirmation may return either slot. */
    settle();
    CHECK(status_now().links_releasing == 2);
    xiaotai_rtc_on_closed_for(1U, h5);
    settle();
    CHECK(status_now().links_releasing == 1);
    xiaotai_rtc_on_closed_for(9U, ai);
    settle();
    CHECK(!status_now().links_releasing && calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 3);

    mode(ordinary_id, XIAOTAI_LINK_LISTEN_OPEN);
    xiaotai_transport_host_accepted(1U);
    settle();
    CHECK(status_now().links_open == 1 && atomic_load(&ordinary.opened) == 1);
    CHECK(atomic_load(&ordinary.generation) != h5);
    xiaotai_rtc_on_closed_for(1U, h5);
    settle();
    CHECK(status_now().links_open == 1 && !atomic_load(&ordinary.closed));
    xiaotai_transport_host_closed(1U);
    settle();
    CHECK(!status_now().links_open && atomic_load(&ordinary.closed) == 1);
    puts("PASS: one AI plus one pending H5, no pending media or third slot, two confirmations restore capacity");
    shutdown_all();
    xiaotai_signal_deinit(&resume);
    xiaotai_signal_deinit(&blocked);
}

static void set_open_gate(void *ctx)
{
    const bool acquire = *(const bool *)ctx;
    CHECK(xiaotai_link_listener_open_gate(guarded_id, acquire) == XIAOTAI_OK);
    if (acquire) CHECK(xiaotai_link_listener_open_gate(guarded_id, true) == XIAOTAI_ERR_BUSY);
}

static void open_gate(bool acquire)
{
    CHECK(xiaotai_run(set_open_gate, &acquire) == XIAOTAI_OK);
    owner_sync();
}

static void set_open_pause(void *ctx)
{
    CHECK(xiaotai_link_listener_pause_open(guarded_id, *(const bool *)ctx) == XIAOTAI_OK);
}

static void open_pause(bool paused)
{
    CHECK(xiaotai_run(set_open_pause, &paused) == XIAOTAI_OK);
    owner_sync();
}

static void test_open_gate(void)
{
    g_observer_start = register_listeners;
    const xiaotai_transport_config_t config = {.links = 2};
    boot_online(&config);
    open_gate(true);
    refused(51, 1);
    CHECK(!atomic_load(&guarded.opened) && !atomic_load(&ordinary.opened)); /* No OPEN fallback. */
    mode(once_id, XIAOTAI_LINK_LISTEN_ONCE);
    xiaotai_transport_host_accepted(52);
    settle();
    CHECK(atomic_load(&once.opened) == 1 && status_now().links_open == 1);
    refused(53, 2); /* Real spare slot; OPEN gate still blocks. */
    hold(ordinary_id, true);
    open_gate(false);
    refused(54, 3); /* Clearing OPEN gate cannot release another owner's inbound block. */
    hold(ordinary_id, false);
    xiaotai_transport_host_accepted(55);
    settle();
    CHECK(atomic_load(&guarded.opened) == 1 && status_now().links_open == 2);
    xiaotai_transport_host_closed(52);
    settle();
    open_pause(true);
    refused(56, 4); /* New OPEN refused; the already handed-off OPEN remains. */
    CHECK(atomic_load(&guarded.opened) == 1 && !atomic_load(&guarded.closed) && status_now().links_open == 1);
    mode(once_id, XIAOTAI_LINK_LISTEN_ONCE);
    xiaotai_transport_host_accepted(57);
    settle();
    CHECK(atomic_load(&once.opened) == 2 && status_now().links_open == 2);
    xiaotai_transport_host_closed(57);
    xiaotai_transport_host_closed(55);
    settle();
    open_gate(true);
    open_pause(false);
    refused(58, 5); /* Releasing the pause cannot release the environment gate. */
    open_gate(false);
    xiaotai_transport_host_accepted(59);
    settle();
    CHECK(atomic_load(&guarded.opened) == 2 && status_now().links_open == 1);
    xiaotai_transport_host_closed(59);
    settle();
    shutdown_all();
}

static void *native_accept(void *ctx)
{
    const xiaotai_rtc_conn_t conn = *(const xiaotai_rtc_conn_t *)ctx;
    xiaotai_transport_host_accepted(conn);
    return NULL;
}

static void owner_race_gate(void *ctx)
{
    CHECK(xiaotai_on_owner());
    xiaotai_err_t *result = ctx;
    watch_gate = true;
    *result = xiaotai_link_listener_open_gate(guarded_id, true);
    CHECK(!watch_gate);
    atomic_store(&owner_finished, true);
}

static void test_s3_gate_race(void)
{
    /* Both S3 product plans select one slot and preserve an already admitted
     * stream at the environment gate; ONCE represents their expected call. */
    g_observer_start = register_listeners;
    boot_online(&(xiaotai_transport_config_t){.links = 1});
    CHECK(status_now().links_max == 1);
    open_gate(true);
    refused(11, 1);
    CHECK(!atomic_load(&guarded.opened) && !atomic_load(&ordinary.opened));
    open_gate(false);
    xiaotai_transport_host_accepted(12);
    settle();
    const uint32_t first = atomic_load(&guarded.generation);
    CHECK(first && atomic_load(&guarded.opened) == 1 && status_now().links_open == 1);
    refused(13, 2);
    CHECK(atomic_load(&guarded.generation) == first && atomic_load(&guarded.opened) == 1 &&
          !atomic_load(&guarded.closed));
    xiaotai_transport_host_closed(12);
    settle();

    open_gate(true);
    mode(once_id, XIAOTAI_LINK_LISTEN_ONCE);
    xiaotai_transport_host_accepted(14);
    settle();
    CHECK(atomic_load(&once.generation) && atomic_load(&once.opened) == 1 &&
          !atomic_load(&once.guard_calls) && atomic_load(&guarded.opened) == 1);
    xiaotai_transport_host_closed(14);
    settle();
    refused(15, 3); /* ONCE already consumed; the OPEN gate is still closed. */
    open_gate(false);

    CHECK(xiaotai_signal_init(&callback_in_gate) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&release_callback) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&owner_at_gate) == XIAOTAI_OK);
    for (unsigned i = 0; i < 32U; ++i) {
        xiaotai_rtc_conn_t conn = 100U + 2U * i;
        const uint32_t disconnects = calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
        pthread_t callback;
        atomic_store(&hold_callback, true);
        atomic_store(&owner_attempted, false);
        atomic_store(&owner_finished, false);
        CHECK(!pthread_create(&callback, NULL, native_accept, &conn));
        CHECK(xiaotai_signal_take(&callback_in_gate, 3000));
        xiaotai_err_t result = XIAOTAI_OK;
        CHECK(xiaotai_run(owner_race_gate, &result) == XIAOTAI_OK);
        CHECK(xiaotai_signal_take(&owner_at_gate, 3000));
        CHECK(atomic_load(&owner_attempted) && !atomic_load(&owner_finished));
        CHECK(atomic_load(&guarded.opened) == i + 1U);
        xiaotai_signal_give(&release_callback);
        CHECK(!pthread_join(callback, NULL));
        settle();
        CHECK(atomic_load(&owner_finished) && result == XIAOTAI_ERR_BUSY);
        CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == disconnects);
        CHECK(status_now().links_open == 1 && atomic_load(&guarded.opened) == i + 2U);
        const uint32_t winner = atomic_load(&guarded.generation);
        refused(conn + 1U, i + 4U);
        CHECK(status_now().links_open == 1 && atomic_load(&guarded.generation) == winner &&
              atomic_load(&guarded.closed) == i + 1U);
        xiaotai_transport_host_closed(conn);
        settle();
        CHECK(!status_now().links_open && atomic_load(&guarded.closed) == i + 2U);
    }
    CHECK(!atomic_load(&ordinary.opened));
    puts("PASS: one-slot closed/open/CALL admission and 32 in-lock races that preserve the winner");
    shutdown_all();
    xiaotai_signal_deinit(&owner_at_gate);
    xiaotai_signal_deinit(&release_callback);
    xiaotai_signal_deinit(&callback_in_gate);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "p4_call_selector")) {
        test_p4_call_selector();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "p4_pending_h5_cleanup")) {
        test_p4_pending_h5_cleanup();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "s3_gate_race")) {
        test_s3_gate_race();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "open_gate")) {
        test_open_gate();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "cancel_block")) {
        test_cancel_block();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "orders")) {
        test_admission_orders();
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "retired_open") || !strcmp(argv[1], "retired_open_default"))) {
        test_retired_open(!strcmp(argv[1], "retired_open"));
        return 0;
    }
    CHECK(xiaotai_signal_init(&blocked) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&resume) == XIAOTAI_OK);
    g_observer_start = register_listeners;
    boot_online(NULL);

    atomic_store(&guarded.busy, true);
    const uint32_t base = xiaotai_generation_next();
    xiaotai_transport_host_accepted(1);
    settle();
    CHECK(xiaotai_generation_next() == base + 1);
    CHECK(status_now().inbound_refused == 1 && !status_now().links_open);
    CHECK(!atomic_load(&ordinary.opened) && !atomic_load(&guarded.opened));
    xiaotai_transport_host_closed(1);
    settle();

    atomic_store(&once.busy, true);
    mode(once_id, XIAOTAI_LINK_LISTEN_ONCE);
    xiaotai_transport_host_accepted(2);
    settle();
    CHECK(atomic_load(&once.opened) == 1 && !atomic_load(&once.guard_calls));
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OFF);
    CHECK(!retiring && status_now().links_open == 1);
    xiaotai_transport_host_closed(2);
    settle();

    atomic_store(&guarded.busy, false);
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OPEN);
    block_owner(guarded_id);
    xiaotai_transport_host_accepted(3);
    CHECK(xiaotai_transport_host_settle(3000));
    CHECK(status_now().links_open == 1 && !atomic_load(&guarded.opened));
    atomic_store(&guarded.busy, true);
    xiaotai_signal_give(&resume);
    settle();
    CHECK(retiring && status_now().links_open == 0 && status_now().links_releasing == 1);
    CHECK(!atomic_load(&guarded.opened) && !atomic_load(&guarded.closed));
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_DISCONNECT) == 2); /* Refused 1 and revoked 3. */
    mode(ordinary_id, XIAOTAI_LINK_LISTEN_OPEN);
    CHECK(!retiring);

    atomic_store(&deny_links, true);
    xiaotai_transport_host_closed(3);
    for (unsigned i = 0; !atomic_load(&saw_denial) && i < 1000; ++i) xiaotai_delay_ms(1);
    CHECK(atomic_load(&saw_denial));
    CHECK(status_now().links_releasing == 0);
    atomic_store(&deny_links, false);
    settle();
    CHECK(g_observed.link_count && g_observed.links[g_observed.link_count - 1] == 0);
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OPEN);
    CHECK(!retiring);

    atomic_store(&guarded.busy, false);
    block_owner(0);
    xiaotai_transport_host_accepted(4);
    CHECK(xiaotai_transport_host_settle(3000));
    atomic_store(&guarded.busy, true);
    xiaotai_signal_give(&resume);
    settle(); /* The owner handoff guard closes a candidate even before an OFF call. */
    CHECK(!atomic_load(&guarded.opened) && !atomic_load(&guarded.closed));
    CHECK(status_now().links_releasing == 1);
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OFF);
    CHECK(retiring);
    xiaotai_transport_host_closed(4);
    settle();

    atomic_store(&guarded.busy, false);
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OPEN);
    xiaotai_transport_host_accepted(5);
    settle();
    CHECK(atomic_load(&guarded.opened) == 1);
    atomic_store(&guarded.busy, true);
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OFF);
    CHECK(!retiring && status_now().links_open == 1);
    xiaotai_transport_host_closed(5);
    settle();
    CHECK(atomic_load(&guarded.closed) == 1);

    atomic_store(&guarded.busy, false);
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OPEN);
    atomic_store(&reject_opened, true);
    xiaotai_transport_host_accepted(6);
    settle();
    CHECK(atomic_load(&guarded.opened) == 2);
    CHECK(retiring && status_now().links_open == 0 && status_now().links_releasing == 1);
    mode(ordinary_id, XIAOTAI_LINK_LISTEN_OPEN);
    CHECK(!retiring);
    xiaotai_transport_host_closed(6);
    settle();
    mode(guarded_id, XIAOTAI_LINK_LISTEN_OFF);
    CHECK(!retiring && atomic_load(&guarded.closed) == 1);
    shutdown_all();
    xiaotai_signal_deinit(&resume);
    xiaotai_signal_deinit(&blocked);
}
