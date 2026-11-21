// SPDX-License-Identifier: MIT
/* Run control: the worker task and its jobs; when the RTC stack runs (the
 * run rules of xiaotai/transport.h); start, stop and release of the stack;
 * holds, restart and faults; the start and connect reserves; the signaling
 * holds the engine takes from the platform core.
 *
 * Only the worker changes xt.run and calls the backend's lifecycle. Owner
 * event handlers, holds and restarts reach it through atomics and a kick;
 * the stack's upcalls set xt.upcalls. The worker judges the rules on every
 * pass: after each job, each upcall and at least every XT_EVALUATE_PERIOD_MS. */
#include <string.h>

#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/log.h"

/* Longest a reset waits for the worker to finish. */
#define XT_WORKER_EXIT_WAIT_MS 2000U

/* Everything a start needs; wiped after each use (it holds the device key). */
typedef struct {
    xiaotai_identity_t identity;
    char client_id[XIAOTAI_IDENTITY_CLIENT_ID_MAX + 1U];
    char endpoint[XIAOTAI_PLATFORM_URL_MAX];
    bool complete;   /* identity, endpoint and network were readable */
} xt_account_t;

static int64_t real_ms(void) { return xiaotai_time_us() / 1000; }

/* ------------------------------------------------------------------ worker */

bool xt_worker_post(void (*fn)(uint32_t a, uint32_t b), uint32_t a, uint32_t b)
{
    if (!xt.jobs_ready || !atomic_load(&xt.worker_running)) return false;
    const xt_job_t job = {.fn = fn, .a = a, .b = b};
    return xiaotai_queue_send(&xt.jobs, &job, 0);
}

void xt_worker_kick(void)
{
    /* A full queue wakes the worker anyway; the kick then is not needed. */
    (void)xt_worker_post(NULL, 0, 0);
}

void xt_worker_wake_at(int64_t at_ms)
{
    if (at_ms > 0 && (xt.run.wake_at == 0 || at_ms < xt.run.wake_at)) xt.run.wake_at = at_ms;
}

bool xt_on_worker(void)
{
    return atomic_load(&xt.worker_running) && xiaotai_task_is_current(&xt.worker);
}

/* ------------------------------------------------------------------ events */

static void queue_event(bool links, int32_t value)
{
    if (xt.pending_count == XT_STATE_EVENTS_PENDING) {
        /* The owner queue refused events for a long time: the oldest goes,
         * handlers read the present truth from xiaotai_transport_state(). */
        memmove(&xt.pending[0], &xt.pending[1], sizeof(xt.pending[0]) * (XT_STATE_EVENTS_PENDING - 1U));
        --xt.pending_count;
        ++xt.pending_dropped;
    }
    xt.pending[xt.pending_count].links = links;
    xt.pending[xt.pending_count].value = value;
    ++xt.pending_count;
}

static void publish_events(void)
{
    if (xt.event_retry_at && real_ms() < xt.event_retry_at) return;
    xt.event_retry_at = 0;
    while (xt.pending_count) {
        const xiaotai_args_t args = {.value = xt.pending[0].value};
        const xiaotai_err_t err = xiaotai_event_emit(
            xt.pending[0].links ? XIAOTAI_EVENT_TRANSPORT_LINKS : XIAOTAI_EVENT_TRANSPORT_STATE, &args, NULL, 0);
        if (err == XIAOTAI_ERR_NO_MEM) {
            xt.event_retry_at = real_ms() + XT_EVENT_RETRY_MS;
            return;
        }
        /* Delivered, or no framework runs that could deliver it. */
        memmove(&xt.pending[0], &xt.pending[1], sizeof(xt.pending[0]) * (XT_STATE_EVENTS_PENDING - 1U));
        --xt.pending_count;
    }
    if (xt.links_event_pending) {
        uint8_t requested = 0, open = 0, releasing = 0;
        xt_links_count(&requested, &open, &releasing);
        const xiaotai_args_t args = {.value = (int32_t)(requested + open + releasing)};
        const xiaotai_err_t err = xiaotai_event_emit(XIAOTAI_EVENT_TRANSPORT_LINKS, &args, NULL, 0);
        if (err == XIAOTAI_ERR_NO_MEM) {
            xt.event_retry_at = real_ms() + XT_EVENT_RETRY_MS;
            return;
        }
        xt.links_event_pending = false;
    }
}

void xt_publish_links(void)
{
    xt.links_event_pending = true;
}

static const char *state_name(int state)
{
    static const char *const names[] = {"down", "starting", "online", "stopping", "fault"};
    return state >= 0 && state <= (int)XIAOTAI_TRANSPORT_FAULT ? names[state] : "?";
}

static void set_state(xiaotai_transport_state_t state, xiaotai_transport_wait_t wait)
{
    atomic_store(&xt.wait, (int)wait);
    const int old = atomic_exchange(&xt.state, (int)state);
    if (old == (int)state) return;
    if (old == XIAOTAI_TRANSPORT_ONLINE) atomic_fetch_add(&xt.service_epoch, 1U);
    queue_event(false, (int32_t)state);
    XIAOTAI_LOGI(XT_TAG, "state %s -> %s (wait %d)", state_name(old), state_name((int)state), (int)wait);
    if (state == XIAOTAI_TRANSPORT_DOWN) xiaotai_eventlog_note(XT_LOG_SOURCE, "down", 0, (int32_t)wait);
}

/* ---------------------------------------------------------------- reserves */

static void reserve_miss(uint32_t bytes)
{
    atomic_fetch_add(&xt.counters.reserve_misses, 1U);
    xiaotai_eventlog_warn(XT_LOG_SOURCE, "reserve_miss", 0, (int32_t)bytes);
}

/* [owner, worker] Takes the start reserve when the product has one, the
 * network has started, no hold exists and the stack is not running. */
static void start_reserve_take(void)
{
    const uint32_t bytes = xt.config.start_reserve_bytes;
    if (!bytes || atomic_load(&xt.provisioning) || !atomic_load(&xt.net_started) || !xt.sync_ready) return;
    /* Neither failure can supply an endpoint until the next boot. */
    if (xiaotai_platform_environment_boot_failed() || xiaotai_platform_start_failed()) return;
    xiaotai_critical_enter(&xt.lock);
    const bool wanted = !xt.stack_active && !xt.start_reserve && atomic_load(&xt.hold_count) == 0U;
    xiaotai_critical_exit(&xt.lock);
    if (!wanted) return;
    void *block = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, bytes);
    if (!block) {
        reserve_miss(bytes);
        return;
    }
    xiaotai_critical_enter(&xt.lock);
    if (!xt.stack_active && !xt.start_reserve && atomic_load(&xt.hold_count) == 0U) {
        xt.start_reserve = block;
        block = NULL;
    }
    xiaotai_critical_exit(&xt.lock);
    if (block) {
        xiaotai_mem_free(block);
    } else {
        xt_account((int64_t)bytes);
    }
}

/* [worker] Gives the start reserve up; for_start also marks the stack
 * active so the owner does not take it again while the stack runs. */
static void start_reserve_give_up(bool for_start)
{
    xiaotai_critical_enter(&xt.lock);
    if (for_start) xt.stack_active = true;
    void *block = xt.start_reserve;
    xt.start_reserve = NULL;
    xiaotai_critical_exit(&xt.lock);
    if (block) {
        xiaotai_mem_free(block);
        xt_account(-(int64_t)xt.config.start_reserve_bytes);
    }
}

/* [worker] The stack holds no memory any more: the start reserve may be
 * taken again, best effort. */
static void stack_released(void)
{
    xiaotai_critical_enter(&xt.lock);
    xt.stack_active = false;
    xiaotai_critical_exit(&xt.lock);
    start_reserve_take();
}

/* [owner] The network made its own start-up allocations: the start reserve
 * is taken now, once per boot, before the platform session can begin
 * (XIAOTAI_EVENT_NET_UP follows this event on the same owner task). */
static void network_started(void)
{
    if (atomic_exchange(&xt.net_started, true)) return;
    start_reserve_take();
}

/* [worker] Takes the connect reserve (back). */
static bool connect_reserve_take(void)
{
    const uint32_t bytes = xt.config.connect_reserve_bytes;
    if (!bytes || xt.run.connect_reserve) return true;
    void *block = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, bytes);
    if (!block) {
        reserve_miss(bytes);
        return false;
    }
    xt_account((int64_t)bytes);
    xiaotai_critical_enter(&xt.lock);
    xt.run.connect_reserve = block;
    xt.run.connect_loan_generation = 0;
    xiaotai_critical_exit(&xt.lock);
    xt.run.connect_retry_at = 0;
    return true;
}

/* [worker] The stack goes away: the connect reserve has nothing to serve. */
static void connect_reserve_drop(void)
{
    xiaotai_critical_enter(&xt.lock);
    void *block = xt.run.connect_reserve;
    xt.run.connect_reserve = NULL;
    xt.run.connect_loan_generation = 0;
    atomic_store(&xt.connect_result_generation, 0);
    xiaotai_critical_exit(&xt.lock);
    if (block) {
        xiaotai_mem_free(block);
        xt_account(-(int64_t)xt.config.connect_reserve_bytes);
    }
    xt.run.connect_retry_at = 0;
}

bool xt_connect_memory_ready(void)
{
    if (!xt.config.connect_reserve_bytes) return true;
    if (!xt.sync_ready) return false;
    xiaotai_critical_enter(&xt.lock);
    const bool ready = xt.run.connect_reserve != NULL && !xt.run.connect_loan_generation;
    xiaotai_critical_exit(&xt.lock);
    return ready;
}

xiaotai_err_t xt_connect_memory_check(void)
{
    if (xt_connect_memory_ready()) return XIAOTAI_OK;
    atomic_fetch_add(&xt.counters.connect_memory_misses, 1U);
    return XIAOTAI_ERR_NO_MEM;
}

xiaotai_err_t xt_link_admissible(void)
{
    if (atomic_load(&xt.provisioning) || !atomic_load(&xt.sized) || atomic_load(&xt.admission_closed))
        return XIAOTAI_ERR_INVALID_STATE;
    return atomic_load(&xt.state) == (int)XIAOTAI_TRANSPORT_ONLINE ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

/* --------------------------------------------------------- signaling holds */

/* [owner] Startup uses its own policy and completion tag. */
static void on_signaling_suspended(xiaotai_err_t result, void *ctx)
{
    (void)result;
    const uint32_t tag = (uint32_t)(uintptr_t)ctx;
    atomic_store(&xt.suspended_start_tag, tag);
    xt_worker_kick();
}

/* Link callbacks are serialized on the platform owner. The worker arms an
 * even tag before taking the hold; the callback sets its low bit after
 * storing the result, independently of worker queue space. The other 31
 * bits come from the boot-wide generation counter and survive engine resets. */
static _Atomic uint32_t s_link_completion;
static _Atomic xiaotai_err_t s_link_result;

static void on_link_signaling_suspended(xiaotai_err_t result, void *ctx)
{
    uint32_t tag = (uint32_t)(uintptr_t)ctx;
    if (atomic_load(&s_link_completion) != tag) return;
    atomic_store(&s_link_result, result);
    if (atomic_compare_exchange_strong(&s_link_completion, &tag, tag | 1U)) xt_worker_kick();
}

/* Takes a platform signaling hold; odd tags for the start, even for links. */
static uint32_t take_signaling_hold(bool start, uint32_t *tag, xiaotai_err_t *error)
{
    uint32_t next;
    if (start) {
        next = xt.run.hold_tags + 1U;
        if (!(next & 1U)) ++next;
        xt.run.hold_tags = next;
    } else {
        do { next = xiaotai_generation_next() << 1; } while (!next);
        atomic_store(&s_link_completion, next);
    }
    *tag = next;
    uint32_t hold = 0;
    xiaotai_err_t err = start ?
        xiaotai_platform_signaling_hold_startup(on_signaling_suspended, (void *)(uintptr_t)next, &hold) :
        xiaotai_platform_signaling_hold(on_link_signaling_suspended, (void *)(uintptr_t)next, &hold);
    if (err == XIAOTAI_OK && !hold) err = XIAOTAI_ERR_INVALID_STATE;
    if (error) *error = err;
    if (err != XIAOTAI_OK || hold == 0U) {
        if (!start) atomic_store(&s_link_completion, 0);
        XIAOTAI_LOGW(XT_TAG, "signaling hold refused: %d", err);
        return 0;
    }
    atomic_fetch_add(&xt.counters.signaling_yields, 1U);
    return hold;
}

static void set_start_hold(uint32_t hold, uint32_t tag, bool suspended, int64_t until)
{
    xiaotai_critical_enter(&xt.lock);
    xt.run.start_hold = hold;
    xt.run.start_hold_tag = tag;
    xt.run.start_hold_suspended = suspended;
    xt.run.start_hold_until = until;
    xiaotai_critical_exit(&xt.lock);
}

static void take_start_hold(int64_t now)
{
    uint32_t tag = 0;
    const uint32_t hold = take_signaling_hold(true, &tag, NULL);
    const uint32_t max = xt.config.start_signaling_hold_max_ms;
    set_start_hold(hold, tag, false, hold && max ? now + (int64_t)max : 0);
}

static void release_start_hold(void)
{
    const uint32_t hold = xt.run.start_hold;
    if (!hold) return;
    set_start_hold(0, 0, false, 0);
    xiaotai_platform_signaling_release(hold);
}

/* The link hold exists while a connect or open links ask for it. */
static void drop_link_hold(void)
{
    const uint32_t hold = xt.run.link_hold;
    if (!hold) return;
    atomic_store(&s_link_completion, 0);
    xiaotai_critical_enter(&xt.lock);
    xt.run.link_hold = 0;
    xt.run.link_hold_tag = 0;
    xiaotai_critical_exit(&xt.lock);
    xiaotai_platform_signaling_release(hold);
}

static xiaotai_err_t update_link_hold(void)
{
    const bool wanted = xt.run.yield_connect || xt.run.yield_open;
    if (wanted && !xt.run.link_hold) {
        uint32_t tag = 0;
        xiaotai_err_t error;
        const uint32_t hold = take_signaling_hold(false, &tag, &error);
        if (error != XIAOTAI_OK) return error;
        xiaotai_critical_enter(&xt.lock);
        xt.run.link_hold = hold;
        xt.run.link_hold_tag = tag;
        xiaotai_critical_exit(&xt.lock);
    } else if (!wanted) drop_link_hold();
    return XIAOTAI_OK;
}

static void release_link_hold(void)
{
    xt.run.connect_wait_generation = 0;
    xt.run.yield_connect = false;
    xt.run.yield_open = false;
    update_link_hold();
}

/* A start hold is given back once the stack is online, after the connect
 * reserve was attempted (or taken, where the product keeps the hold until
 * then). */
static void maybe_release_start_hold(void)
{
    if (!xt.run.start_hold || xt.run.stack != XT_STACK_UP) return;
    if (xt.config.start_hold_until_reserve && xt.config.connect_reserve_bytes && !xt.run.connect_reserve) return;
    release_start_hold();
}

static bool memory_short_for_connect(void)
{
    const uint32_t free_min = xt.config.yield_connect_free_bytes;
    const uint32_t block_min = xt.config.yield_connect_block_bytes;
    if (!free_min && !block_min) return false;
    xiaotai_mem_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    xiaotai_mem_stats(XIAOTAI_MEM_INTERNAL, &stats);
    return stats.free < free_min || stats.largest_block < block_min;
}

static void abandon_connect_hold(bool failed)
{
    xt.run.connect_wait_generation = 0;
    xt.run.yield_connect = false;
    if (failed || !xt.run.yield_open) drop_link_hold();
}

void xt_connect_memory_abandon_unsubmitted(void)
{
    const uint32_t generation = xt.run.connect_wait_generation;
    if (!generation) return;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    const bool pending = slot && slot->args;
    xiaotai_critical_exit(&xt.lock);
    if (!pending) abandon_connect_hold(false);
}

xt_connect_memory_t xt_connect_memory_lend(uint32_t generation, xiaotai_err_t *error)
{
    const bool threshold = xt.config.yield_connect_free_bytes || xt.config.yield_connect_block_bytes;
    const uint32_t waiting = xt.run.connect_wait_generation;
    if (threshold && (xt.run.connect_loan_generation || (waiting && waiting != generation) ||
                      (xt.run.yield_connect && !waiting))) return XT_CONNECT_MEMORY_WAIT;
    if (!xt.run.yield_connect && !xt.run.connect_loan_generation && memory_short_for_connect()) {
        xt.run.yield_connect = true;
        xt.run.connect_wait_generation = generation;
        *error = update_link_hold();
        if (*error != XIAOTAI_OK) {
            abandon_connect_hold(true);
            return XT_CONNECT_MEMORY_FAILED;
        }
    }
    if (xt.run.yield_connect) {
        if (atomic_load(&s_link_completion) != (xt.run.link_hold_tag | 1U)) return XT_CONNECT_MEMORY_WAIT;
        *error = atomic_load(&s_link_result);
        if (*error != XIAOTAI_OK) {
            abandon_connect_hold(true);
            return XT_CONNECT_MEMORY_FAILED;
        }
    }
    xt.run.connect_wait_generation = 0;
    xiaotai_critical_enter(&xt.lock);
    void *block = xt.run.connect_reserve;
    xt.run.connect_reserve = NULL;
    if (block || xt.run.yield_connect) xt.run.connect_loan_generation = generation;
    xiaotai_critical_exit(&xt.lock);
    if (block) {
        xiaotai_mem_free(block);
        xt_account(-(int64_t)xt.config.connect_reserve_bytes);
    }
    return XT_CONNECT_MEMORY_READY;
}

/* Signaling is given back only once the reserve is in place again, so the
 * signaling connection cannot take the memory the next connect needs. */
static void finish_restore(void)
{
    xiaotai_critical_enter(&xt.lock);
    xt.run.connect_loan_generation = 0;
    xiaotai_critical_exit(&xt.lock);
    xt.run.connect_retry_at = 0;
    if (xt.run.yield_connect) {
        xt.run.yield_connect = false;
        update_link_hold();
    }
    /* A request admitted before the prior loan can now be submitted. */
    if (xt.config.yield_connect_free_bytes) xt_worker_kick();
}

void xt_connect_memory_restore(uint32_t generation)
{
    if (!generation || generation != xt.run.connect_loan_generation ||
        xt.run.stack != XT_STACK_UP || xt.run.connect_retry_at) return;
    if (!connect_reserve_take()) {
        xt.run.connect_retry_at = xt_now_ms() + XT_RESTORE_RETRY_MS;
        return;
    }
    finish_restore();
}

void xt_run_links_changed(void)
{
    if (!xt.config.yield_while_open) return;
    uint8_t requested = 0, open = 0, releasing = 0;
    xt_links_count(&requested, &open, &releasing);
    if (open) {
        xt.run.yield_open = true;
    } else if (requested + releasing == 0U) {
        xt.run.yield_open = false;
    }
    update_link_hold();
}

/* --------------------------------------------------------------- statistics */

static void apply_statistics(void)
{
    const xiaotai_rtc_caps_t *caps = xiaotai_rtc_caps();
    const bool wanted = caps && caps->statistics && xt.run.stack == XT_STACK_UP &&
                        xiaotai_net_trace_transport_active();
    if (wanted == xt.run.statistics) return;
    xiaotai_rtc_set_statistics(wanted);
    xt.run.statistics = wanted;
}

/* ------------------------------------------------------------ the account */

/* 64-bit FNV-1a: the worker compares accounts without keeping the key. */
static uint64_t digest_add(uint64_t hash, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        hash = (hash ^ *p) * 0x100000001b3ULL;
    }
    return (hash ^ 0xffU) * 0x100000001b3ULL;   /* field separator */
}

static uint64_t account_digest(const xt_account_t *account)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    hash = digest_add(hash, account->identity.device_id);
    hash = digest_add(hash, account->identity.device_key);
    hash = digest_add(hash, account->client_id);
    return digest_add(hash, account->endpoint);
}

/* Reads the start conditions in the order of the specification's table and
 * returns the first that is not met. */
static xiaotai_transport_wait_t read_conditions(xt_account_t *account)
{
    account->complete = false;
    if (atomic_load(&xt.provisioning)) return XIAOTAI_TRANSPORT_WAIT_PROVISIONING;
    if (xiaotai_platform_start_failed()) return XIAOTAI_TRANSPORT_WAIT_PLATFORM;
    if (xiaotai_identity_state() != XIAOTAI_IDENTITY_VALID ||
        xiaotai_identity_get(&account->identity) != XIAOTAI_OK) {
        return XIAOTAI_TRANSPORT_WAIT_IDENTITY;
    }
    const xiaotai_err_t found = xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_TIRTC,
                                                        account->endpoint, sizeof(account->endpoint));
    if (found != XIAOTAI_OK) {
        /* Discovered without a usable entry: the stack's built-in one. */
        account->endpoint[0] = '\0';
        if (found == XIAOTAI_ERR_PLATFORM_OFFLINE) return XIAOTAI_TRANSPORT_WAIT_ENDPOINT;
    }
    if (xt.config.endpoint_required && account->endpoint[0] == '\0') return XIAOTAI_TRANSPORT_WAIT_ENDPOINT;
    xiaotai_net_status_t net;
    memset(&net, 0, sizeof(net));
    xiaotai_net_status(&net);
    if (net.mac_valid) {
        memcpy(xt.run.mac, net.mac, sizeof(xt.run.mac));
        xt.run.mac_valid = true;
        xt.run.network_kind = (uint8_t)net.kind;
    }
    if (!xt.run.mac_valid) return XIAOTAI_TRANSPORT_WAIT_NETWORK;
    if (xiaotai_identity_client_id(xt.run.mac, account->client_id, sizeof(account->client_id)) != XIAOTAI_OK) {
        return XIAOTAI_TRANSPORT_WAIT_IDENTITY;
    }
    account->complete = true;
    if (atomic_load(&xt.hold_count)) return XIAOTAI_TRANSPORT_WAIT_HOLD;
    xiaotai_platform_status_t platform;
    memset(&platform, 0, sizeof(platform));
    xiaotai_platform_status(&platform);
    if (xt.run.gated && !platform.gated) {
        /* The gate opened: a start condition changed. */
        xt.run.start_failed = false;
    }
    xt.run.gated = platform.gated;
    if (platform.gated) return XIAOTAI_TRANSPORT_WAIT_MAINTENANCE;
    if (xt.config.down_while_unbound && atomic_load(&xt.unbound)) return XIAOTAI_TRANSPORT_WAIT_UNBOUND;
    if (xt.config.start_after_online && platform.phase != XIAOTAI_PLATFORM_ONLINE) {
        return XIAOTAI_TRANSPORT_WAIT_PLATFORM;
    }
    return XIAOTAI_TRANSPORT_WAIT_NONE;
}

static void account_wipe(xt_account_t *account)
{
    xiaotai_identity_wipe(account, sizeof(*account));
}

/* ---------------------------------------------------------- start and stop */

static void start_failed(int64_t now, xiaotai_err_t err, int detail);

static void begin_start(int64_t now, xt_account_t *account)
{
    if (xt.config.start_before_signaling && !xt.run.prestart && !xt.run.start_hold &&
        !xt.run.start_hold_until) {
        take_start_hold(now);
    }
    if (xt.run.start_hold && !xt.run.start_hold_suspended) {
        /* Signaling must be gone before the start reserve is given up. */
        xt.run.prestart = true;
        set_state(XIAOTAI_TRANSPORT_STARTING, XIAOTAI_TRANSPORT_WAIT_NONE);
        return;
    }
    xt.run.prestart = false;
    set_state(XIAOTAI_TRANSPORT_STARTING, XIAOTAI_TRANSPORT_WAIT_NONE);
    start_reserve_give_up(true);
    const xiaotai_transport_config_t *c = &xt.config;
    const xiaotai_rtc_start_t options = {
        .device_id = account->identity.device_id,
        .device_secret = account->identity.device_key,
        .client_id = account->client_id,
        .endpoint = account->endpoint,
        .send_buffer_bytes = xt.send_buffer_bytes,
        .vendor_idle_stack_bytes = c->vendor_idle_stack_bytes,
        .links = xt.links_max,
        .network_kind = xt.run.network_kind,
        .poll_timeout_ms = c->poll_timeout_ms,
        .connect_cache = c->connect_cache,
        .media_transport = c->media_transport,
        .stack_log_level = c->stack_log_level,
        .media_transport_before_init = c->media_transport_before_init,
        .option_strings_without_nul = c->option_strings_without_nul,
        .endpoint_http_rewrite = c->endpoint_http_rewrite,
        .endpoint_default_unset = c->endpoint_default_unset,
    };
    int detail = 0;
    const xiaotai_err_t err = xiaotai_rtc_start(&options, &detail);
    if (err != XIAOTAI_OK) {
        start_failed(now, err, detail);
        return;
    }
    xt.run.stack = XT_STACK_STARTING;
    xt.run.account = account_digest(account);
    atomic_fetch_add(&xt.counters.starts, 1U);
    xiaotai_eventlog_note(XT_LOG_SOURCE, "start", 0, 0);
    XIAOTAI_LOGI(XT_TAG, "stack start submitted (%s)", xiaotai_transport_version());
}

static void enter_down_after_failure(int64_t now, xiaotai_transport_recovery_t recovery, xt_fault_t cause, int detail)
{
    switch (recovery) {
    case XIAOTAI_TRANSPORT_RECOVER_RETRY:
        xt.run.retry_at = now + (int64_t)xt.start_retry_ms;
        set_state(XIAOTAI_TRANSPORT_DOWN, XIAOTAI_TRANSPORT_WAIT_RETRY);
        break;
    case XIAOTAI_TRANSPORT_RECOVER_AT_ONCE:
        set_state(XIAOTAI_TRANSPORT_DOWN, XIAOTAI_TRANSPORT_WAIT_NONE);
        break;
    case XIAOTAI_TRANSPORT_RECOVER_ON_EVENT:
        xt.run.start_failed = true;
        set_state(XIAOTAI_TRANSPORT_DOWN, XIAOTAI_TRANSPORT_WAIT_START_FAILED);
        break;
    case XIAOTAI_TRANSPORT_RECOVER_FAULT:
        xt_fault(cause, detail);
        break;
    }
}

static void start_failed(int64_t now, xiaotai_err_t err, int detail)
{
    const int code = detail ? detail : err;
    atomic_fetch_add(&xt.counters.start_failures, 1U);
    atomic_store(&xt.counters.last_start_detail, code);
    xiaotai_eventlog_emit(&(xiaotai_eventlog_event_t){.source = XT_LOG_SOURCE,
                                                      .label = "start_failed",
                                                      .value = code,
                                                      .level = XIAOTAI_EVENTLOG_ERROR,
                                                      .kind = XIAOTAI_EVENTLOG_LINK});
    XIAOTAI_LOGW(XT_TAG, "stack start failed: %d", code);
    /* The stack is back to uninitialized: give signaling back and keep the
     * memory for the next attempt. */
    release_start_hold();
    stack_released();
    xiaotai_transport_recovery_t recovery = xt.config.start_failure;
    if (recovery == XIAOTAI_TRANSPORT_RECOVER_AT_ONCE) {
        /* A start that keeps failing must not spin the worker: "at once"
         * after a failed start is the next periodic pass. */
        xt.run.retry_at = now + XT_EVALUATE_PERIOD_MS;
        set_state(XIAOTAI_TRANSPORT_DOWN, XIAOTAI_TRANSPORT_WAIT_RETRY);
        return;
    }
    enter_down_after_failure(now, recovery, XT_FAULT_START_FAILED, code);
}

static bool commit_stop_request(void)
{
    if (!xt.config.fail_environment_on_unsolicited_stop) return true;
    xiaotai_critical_enter(&xt.lock);
    /* The first raw STOP and the native request are ordered on the same lock.
     * A report already pending cannot acknowledge a later request. */
    const bool pending = atomic_load(&xt.upcalls) & XT_UP_STOPPED;
    if (!pending) xt.stop_request_committed = true;
    xiaotai_critical_exit(&xt.lock);
    return !pending;
}

/* Stop sequence: links end, connections are released, the stack is asked to
 * stop; STOPPED then leads to finish_stop(). wait: shown while STOPPING. */
static void begin_stop(int64_t now, xiaotai_transport_wait_t wait)
{
    atomic_store(&xt.admission_closed, true);
    xt.run.bounce = false;
    xt.run.prestart = false;
    xt.run.connect_retry_at = 0; /* only an online stack needs this reserve */
    if (!xt.run.drain_to_stop) xt_links_end_all(XIAOTAI_LINK_END_STOPPED, 0);
    if (atomic_load(&xt.backend_users)) {
        /* New pins are closed. Finishing uses wakes the worker; the stack
         * remains initialized until the outstanding downcalls have left. */
        xt.run.drain_to_stop = true;
        xt.run.drain_wait = wait;
        if (!xt.run.fault) set_state(XIAOTAI_TRANSPORT_STOPPING, wait);
        return;
    }
    xt.run.drain_to_stop = false;
    xt_links_release_all();
    release_start_hold();
    release_link_hold();
    xt.run.statistics = false;
    if (!commit_stop_request()) {
        /* Keep the request represented until the earlier report is classified.
         * Confirmed stop paths clear the drain before looking at it again. */
        xt.run.drain_to_stop = true;
        xt.run.drain_wait = wait;
        return;
    }
    int detail = 0;
    const xiaotai_err_t err = xiaotai_rtc_stop(&detail);
    if (err != XIAOTAI_OK) {
        xt.run.stack = XT_STACK_STUCK;
        xt.run.stop_deadline = 0;
        xt.run.restart_after_stop = false;
        XIAOTAI_LOGE(XT_TAG, "stack stop failed: %d", detail ? detail : err);
        xt_fault(XT_FAULT_STOP_FAILED, detail ? detail : err);
        return;
    }
    xt.run.stack = XT_STACK_STOPPING;
    xt.run.stop_deadline = now + (int64_t)xt.release_timeout_ms;
    atomic_fetch_add(&xt.counters.stops, 1U);
    if (!xt.run.fault) set_state(XIAOTAI_TRANSPORT_STOPPING, wait);
}

/* The stack confirmed STOPPED: release it (only now, never in the upcall). */
static void finish_stop(void)
{
    xiaotai_rtc_uninit();
    if (xt.config.fail_environment_on_unsolicited_stop) {
        xiaotai_critical_enter(&xt.lock);
        xt.stop_request_committed = false;
        xiaotai_critical_exit(&xt.lock);
        xt.run.drain_to_stop = false;
    }
    xt.run.stack = XT_STACK_DOWN;
    xt.run.stop_deadline = 0;
    xt.run.statistics = false;
    xt_links_stack_released();
    connect_reserve_drop();
    stack_released();
    if (!xt.run.fault) set_state(XIAOTAI_TRANSPORT_DOWN, XIAOTAI_TRANSPORT_WAIT_NONE);
}

/* The stack reported STOPPED without being asked. */
static void complete_unsolicited_stop(int64_t now, int detail)
{
    xt.run.stopped_pending = false;
    xt.run.drain_to_stop = false;
    release_start_hold();
    release_link_hold();
    xiaotai_rtc_uninit();
    xt_links_stack_released();
    xt.run.stack = XT_STACK_DOWN;
    xt.run.statistics = false;
    connect_reserve_drop();
    stack_released();
    enter_down_after_failure(now, xt.config.unsolicited_stop, XT_FAULT_UNSOLICITED_STOP, detail);
}

static void unsolicited_stop(int64_t now, int detail)
{
    atomic_fetch_add(&xt.counters.unsolicited_stops, 1U);
    xiaotai_eventlog_warn(XT_LOG_SOURCE, "stopped_unsolicited", 0, detail);
    XIAOTAI_LOGW(XT_TAG, "stack stopped by itself");
    xt.run.prestart = false;
    xt.run.bounce = false;
    xt_links_end_all(XIAOTAI_LINK_END_STOPPED, 0);
    if (atomic_load(&xt.backend_users)) {
        xt.run.stopped_pending = true;
        xt.run.stopped_detail = detail;
        return;
    }
    complete_unsolicited_stop(now, detail);
}

static void stack_online(int64_t now)
{
    xt.run.stack = XT_STACK_UP;
    atomic_store(&xt.admission_closed, false);
    set_state(XIAOTAI_TRANSPORT_ONLINE, XIAOTAI_TRANSPORT_WAIT_NONE);
    xiaotai_eventlog_note(XT_LOG_SOURCE, "online", 0, 0);
    /* Order: the connect reserve first, only then signaling (it would
     * otherwise fragment the block the next connect needs). */
    if (!connect_reserve_take()) xt.run.connect_retry_at = now + XT_RESERVE_RETRY_MS;
    maybe_release_start_hold();
    apply_statistics();
}

void xt_fault(xt_fault_t cause, int detail)
{
    atomic_store(&xt.admission_closed, true);
    const int64_t now = xt_now_ms();
    if (!xt.run.fault) {
        xt.run.fault = true;
        xt.run.restart_pending = false;
        xt.run.fault_restart_at = xt.config.fault_restart_ms ? now + (int64_t)xt.config.fault_restart_ms : 0;
        atomic_fetch_add(&xt.counters.faults, 1U);
    }
    atomic_store(&xt.counters.last_fault_detail, detail);
    xiaotai_eventlog_emit(&(xiaotai_eventlog_event_t){.source = XT_LOG_SOURCE,
                                                      .label = "fault",
                                                      .value = (int32_t)cause,
                                                      .level = XIAOTAI_EVENTLOG_ERROR,
                                                      .kind = XIAOTAI_EVENTLOG_NUMBER});
    XIAOTAI_LOGE(XT_TAG, "fault %d (detail %d)", (int)cause, detail);
    xt.run.bounce = false;
    xt.run.start_failed = false;
    xt.run.retry_at = 0;
    if (xt.run.prestart) {
        xt.run.prestart = false;
        release_start_hold();
    }
    xt_links_end_all(XIAOTAI_LINK_END_STOPPED, 0);
    set_state(XIAOTAI_TRANSPORT_FAULT, XIAOTAI_TRANSPORT_WAIT_FAULT);
    /* A faulted stack never stays online or keeps its memory. */
    if (xt.run.stack == XT_STACK_STARTING || xt.run.stack == XT_STACK_UP) {
        begin_stop(now, XIAOTAI_TRANSPORT_WAIT_FAULT);
    } else {
        release_link_hold();
        connect_reserve_drop();
    }
}

/* In FAULT: the fault's own stop, and the two ways out. True while the
 * engine stays in FAULT. */
static bool fault_progress(int64_t now)
{
    if (xt.run.fault_restart_at && now >= xt.run.fault_restart_at) {
        xt.run.fault_restart_at = 0;
        xt.run.restart_pending = true;
    }
    switch (xt.run.stack) {
    case XT_STACK_STARTING:
    case XT_STACK_UP:
        begin_stop(now, XIAOTAI_TRANSPORT_WAIT_FAULT);
        return true;
    case XT_STACK_STOPPING:
        return true;
    case XT_STACK_STUCK:
        if (xt.run.restart_pending) {
            /* The restart asks the stuck stack to stop once more. */
            xt.run.restart_pending = false;
            xt.run.restart_after_stop = true;
            begin_stop(now, XIAOTAI_TRANSPORT_WAIT_FAULT);
        }
        return true;
    case XT_STACK_DOWN:
        if (!xt.run.restart_pending && !xt.run.restart_after_stop) return true;
        break;
    }
    xt.run.fault = false;
    xt.run.restart_pending = false;
    xt.run.restart_after_stop = false;
    xt.run.fault_restart_at = 0;
    XIAOTAI_LOGI(XT_TAG, "leaving fault");
    set_state(XIAOTAI_TRANSPORT_DOWN, XIAOTAI_TRANSPORT_WAIT_NONE);
    return false;
}

/* ------------------------------------------------------------- the passes */

static void handle_upcalls(int64_t now)
{
    const bool monitor = xt.config.fail_environment_on_unsolicited_stop;
    if (monitor) xiaotai_critical_enter(&xt.lock);
    const uint32_t bits = atomic_exchange(&xt.upcalls, 0U);
    const int held_detail = monitor && (bits & XT_UP_STOPPED) ? atomic_load(&xt.stop_detail) : 0;
    const uint32_t environment_epoch = monitor && (bits & XT_UP_STOPPED) ? xt.stop_environment_epoch : 0U;
    const bool report_expected = monitor && (bits & XT_UP_STOPPED) && xt.stop_report_expected;
    if (monitor) {
        xt.classifying_stop_epoch = environment_epoch;
        xiaotai_critical_exit(&xt.lock);
    }
    if ((bits & XT_UP_STARTED) && xt.run.stack == XT_STACK_STARTING &&
        !(monitor && (bits & XT_UP_STOPPED) && !report_expected)) stack_online(now);
    if (bits & XT_UP_STOPPED) {
        const int detail = monitor ? held_detail : atomic_load(&xt.stop_detail);
        const bool expected = (!monitor || report_expected) &&
            (xt.run.stack == XT_STACK_STOPPING || xt.run.stack == XT_STACK_STUCK);
        const bool failed = !report_expected && !xt.run.stopped_pending &&
            (xt.run.stack == XT_STACK_STARTING || xt.run.stack == XT_STACK_UP);
        if (failed) atomic_store(&xt.admission_closed, true);
        if (environment_epoch & 1U) {
            xiaotai_critical_enter(&xt.lock);
            if (failed && xt.environment_epoch == environment_epoch)
                xt.environment_error = detail ? detail : XIAOTAI_ERR_INVALID_STATE;
            xt.classifying_stop_epoch = 0;
            xiaotai_critical_exit(&xt.lock);
        }
        if (expected) {
            finish_stop();
        } else if (failed) {
            unsolicited_stop(now, detail);
        }
    }
    if (bits & XT_UP_ALERT) {
        atomic_fetch_add(&xt.counters.alerts, 1U);
        xiaotai_eventlog_warn(XT_LOG_SOURCE, "alert", 0, 0);
        if (xt.config.alert_ends_links) xt_links_end_all(XIAOTAI_LINK_END_ALERT, 0);
    }
}

static void handle_requests(void)
{
    /* Holds: given up and taken again with the start reserve. */
    const uint8_t holds = atomic_load(&xt.hold_count);
    const bool platform_failed = xiaotai_platform_start_failed();
    if (holds || platform_failed) {
        start_reserve_give_up(false);
        /* A boot-time yield can precede both a usable account and prestart.
         * An explicit hold abandons that start, so return its signaling hold
         * even when there is no stack to stop and no prestart to cancel. */
        if (xt.run.stack == XT_STACK_DOWN && (!xt.run.prestart || platform_failed)) release_start_hold();
    }
    if (holds != xt.holds_seen) {
        if (holds == 0U) {
            xt.run.start_failed = false;   /* a start condition changed */
            if (xt.run.stack == XT_STACK_DOWN) stack_released();
        }
        xt.holds_seen = holds;
    }
    const uint32_t conditions = atomic_load(&xt.conditions);
    if (conditions != xt.run.conditions_seen) {
        xt.run.conditions_seen = conditions;
        xt.run.start_failed = false;
    }
    const uint32_t restarts = atomic_load(&xt.restarts);
    if (restarts != xt.restarts_seen) {
        xt.restarts_seen = restarts;
        if (xt.run.fault) {
            xt.run.restart_pending = true;
        } else if (xt.run.stack == XT_STACK_STARTING || xt.run.stack == XT_STACK_UP || xt.run.prestart) {
            xt.run.bounce = true;
        } else {
            xt.run.start_failed = false;
            xt.run.retry_at = 0;
        }
    }
    const uint32_t start_tag = atomic_load(&xt.suspended_start_tag);
    if (xt.run.start_hold && start_tag == xt.run.start_hold_tag) {
        xiaotai_critical_enter(&xt.lock);
        xt.run.start_hold_suspended = true;
        xiaotai_critical_exit(&xt.lock);
    }
    if (atomic_exchange(&xt.trace_changed, false)) apply_statistics();
}

static void handle_timers(int64_t now)
{
    /* The retry backoff ends even when a start condition is still missing.
     * Only evaluate() decides whether another start can run. */
    if (xt.run.retry_at && now >= xt.run.retry_at) xt.run.retry_at = 0;
    if (xt.run.stack == XT_STACK_STOPPING && xt.run.stop_deadline && now >= xt.run.stop_deadline) {
        /* A stop that is never confirmed: the stack stays initialized
         * (uninit only follows STOPPED) and the engine faults. */
        xt.run.stack = XT_STACK_STUCK;
        xt.run.stop_deadline = 0;
        xt.run.restart_after_stop = false;
        atomic_fetch_add(&xt.counters.release_stalls, 1U);
        xiaotai_eventlog_warn(XT_LOG_SOURCE, "release_stall", 0, 0);
        xt_fault(XT_FAULT_STOP_TIMEOUT, XIAOTAI_ERR_TIMEOUT);
    }
    if (xt.run.start_hold && xt.run.start_hold_until && now >= xt.run.start_hold_until) {
        /* The platform goes on; the stack keeps trying without the hold. */
        const uint32_t hold = xt.run.start_hold;
        set_start_hold(0, 0, false, xt.run.start_hold_until);
        xiaotai_platform_signaling_release(hold);
    }
    if (xt.run.connect_retry_at && now >= xt.run.connect_retry_at && xt.run.stack == XT_STACK_UP) {
        const bool lent = xt.run.connect_loan_generation != 0;
        if (connect_reserve_take()) {
            if (lent) finish_restore();
            maybe_release_start_hold();
        } else {
            xt.run.connect_retry_at = now + (lent ? XT_RESTORE_RETRY_MS : XT_RESERVE_RETRY_MS);
        }
    }
}

static void evaluate(int64_t now)
{
    if (atomic_load(&xt.provisioning)) return;
    if (xt.run.stopped_pending) {
        if (!atomic_load(&xt.backend_users)) complete_unsolicited_stop(now, xt.run.stopped_detail);
        return;
    }
    if (xt.run.drain_to_stop) {
        begin_stop(now, xt.run.drain_wait);
        return;
    }
    if (xt.run.fault && fault_progress(now)) return;
    if (xt.run.stack == XT_STACK_STOPPING || xt.run.stack == XT_STACK_STUCK) return;
    xt_account_t account;
    memset(&account, 0, sizeof(account));
    const xiaotai_transport_wait_t wait = read_conditions(&account);
    const bool running = xt.run.stack == XT_STACK_STARTING || xt.run.stack == XT_STACK_UP;
    if (running || xt.run.prestart) {
        const bool held = atomic_load(&xt.hold_count) != 0U;
        const bool unbound = xt.config.down_while_unbound && atomic_load(&xt.unbound);
        bool stop = held || unbound || xt.run.bounce;
        /* An account that changed: the stack restarts with the new one. A
         * re-binding with the same identity keeps the same digest. */
        if (!stop && running && account.complete && account_digest(&account) != xt.run.account) stop = true;
        const xiaotai_transport_wait_t why =
            held ? XIAOTAI_TRANSPORT_WAIT_HOLD : (unbound ? XIAOTAI_TRANSPORT_WAIT_UNBOUND : XIAOTAI_TRANSPORT_WAIT_NONE);
        if (stop && running) {
            begin_stop(now, why);
        } else if (xt.run.prestart && (stop || wait != XIAOTAI_TRANSPORT_WAIT_NONE)) {
            xt.run.prestart = false;
            xt.run.bounce = false;
            release_start_hold();
            set_state(XIAOTAI_TRANSPORT_DOWN, wait != XIAOTAI_TRANSPORT_WAIT_NONE ? wait : why);
        } else if (xt.run.prestart) {
            begin_start(now, &account);
        }
        account_wipe(&account);
        return;
    }
    if (wait != XIAOTAI_TRANSPORT_WAIT_NONE) {
        set_state(XIAOTAI_TRANSPORT_DOWN, wait);
    } else if (xt.run.start_failed) {
        set_state(XIAOTAI_TRANSPORT_DOWN, XIAOTAI_TRANSPORT_WAIT_START_FAILED);
    } else if (xt.run.retry_at && now < xt.run.retry_at) {
        set_state(XIAOTAI_TRANSPORT_DOWN, XIAOTAI_TRANSPORT_WAIT_RETRY);
    } else {
        xt.run.retry_at = 0;
        /* A fresh attempt may take a new start hold. */
        if (!xt.run.start_hold) xt.run.start_hold_until = 0;
        begin_start(now, &account);
    }
    account_wipe(&account);
}

static void pass(void)
{
    const int64_t now = xt_now_ms();
    xt.run.wake_at = 0;
    handle_requests();
    handle_upcalls(now);
    xt_links_tick(now);
    handle_timers(now);
    evaluate(now);
    /* An upcall that arrived during the pass is handled on the next one. */
    if (atomic_load(&xt.upcalls)) xt_worker_kick();
    if (now >= xt.run.next_evaluate) xt.run.next_evaluate = now + XT_EVALUATE_PERIOD_MS;
    publish_events();
    xt.service_reply_retry = xt_service_replies();
}

static int64_t earliest(int64_t a, int64_t b)
{
    if (!a) return b;
    if (!b) return a;
    return a < b ? a : b;
}

static uint32_t wait_before_next_pass(void)
{
    int64_t deadline = xt.run.next_evaluate;
    deadline = earliest(deadline, xt.run.retry_at);
    deadline = earliest(deadline, xt.run.fault_restart_at);
    deadline = earliest(deadline, xt.run.stop_deadline);
    if (xt.run.start_hold) deadline = earliest(deadline, xt.run.start_hold_until);
    deadline = earliest(deadline, xt.run.connect_retry_at);
    deadline = earliest(deadline, xt.run.wake_at);
    const int64_t delta = deadline - xt_now_ms();
    uint32_t wait = delta <= 0 ? 0U : xt_clock_wait_ms(delta);
    if (xt.pending_count || xt.links_event_pending) {
        /* Framework events are retried in real time whatever the clock. */
        const int64_t left = xt.event_retry_at ? xt.event_retry_at - real_ms() : 0;
        const uint32_t retry = left <= 0 ? 0U : (uint32_t)left;
        if (retry < wait) wait = retry;
    }
    if (xt.owner_drain_retry_at) {
        const int64_t left = xt.owner_drain_retry_at - real_ms();
        const uint32_t retry = left <= 0 ? 0U : (uint32_t)left;
        if (retry < wait) wait = retry;
    }
    if (xt.service_reply_retry && wait > XT_SERVICE_REPLY_RETRY_MS) wait = XT_SERVICE_REPLY_RETRY_MS;
    return wait;
}

static void worker_quit(void)
{
    /* Host tests only (reset): leave nothing initialized behind. */
    if (xt.run.stack != XT_STACK_DOWN) {
        int detail = 0;
        (void)xiaotai_rtc_stop(&detail);
        xiaotai_rtc_uninit();
        xt.run.stack = XT_STACK_DOWN;
    }
    release_start_hold();
    release_link_hold();
    connect_reserve_drop();
}

static void worker_main(void *arg)
{
    (void)arg;
    int64_t sample_at = 0;
    xiaotai_media_uplink_work_t audio = XIAOTAI_MEDIA_UPLINK_IDLE;
    for (;;) {
        pass();
        const int64_t now = real_ms();
        if (now >= sample_at) {
            /* Measure the live worker from itself, not a possibly deleted
             * handle or the system-wide task tracer. */
            atomic_store(&xt.worker_stack_free, xiaotai_task_current_stack_free());
            sample_at = now + 1000;
        }
        uint32_t wait = wait_before_next_pass();
        if (audio == XIAOTAI_MEDIA_UPLINK_PENDING) wait = 0;
        if (audio == XIAOTAI_MEDIA_UPLINK_CONTENDED && wait > 1U) wait = 1U;
        xt_job_t job;
        bool got = false;
        if (wait == 0U) {
            got = xiaotai_queue_receive(&xt.jobs, &job, 0);
        } else {
            atomic_store(&xt.worker_waiting, true);
            got = xiaotai_queue_receive(&xt.jobs, &job, wait);
            atomic_store(&xt.worker_waiting, false);
        }
        while (got) {
            if (job.fn) {
                /* Upcalls/holds queued while a blocking job ran take effect
                 * before another job is allowed into the stack. */
                pass();
                job.fn(job.a, job.b);
            }
            got = xiaotai_queue_receive(&xt.jobs, &job, 0);
        }
        if (atomic_load(&xt.worker_quit)) break;
        const bool audio_wake = xt.audio_executor_attached && atomic_exchange(&xt.audio_wake_posted, false);
        if (audio_wake || audio != XIAOTAI_MEDIA_UPLINK_IDLE) {
            /* The next send may block in the backend. Control work and gates
             * are evaluated before borrowing each individual frame. */
            pass();
            if (atomic_load(&xt.worker_quit)) break;
            audio = xiaotai_media_uplink_step(&xt_audio_executor);
            atomic_store(&xt.audio_pending, audio != XIAOTAI_MEDIA_UPLINK_IDLE);
        }
    }
    worker_quit();
    atomic_store(&xt.worker_running, false);
    xiaotai_signal_give(&xt.worker_exited);
    xiaotai_task_exit();
}

bool xt_worker_settled(void)
{
    if (!atomic_load(&xt.worker_running)) return true;
    if (!atomic_load(&xt.worker_waiting) || xiaotai_queue_count(&xt.jobs) != 0U) return false;
    if (atomic_load(&xt.audio_wake_posted) || atomic_load(&xt.audio_pending)) return false;
    if (atomic_load(&xt.upcalls) || atomic_load(&xt.trace_changed)) return false;
    if (atomic_load(&xt.conditions) != xt.run.conditions_seen) return false;
    if (atomic_load(&xt.restarts) != xt.restarts_seen || atomic_load(&xt.hold_count) != xt.holds_seen) return false;
    if (xt.run.start_hold && !xt.run.start_hold_suspended &&
        atomic_load(&xt.suspended_start_tag) == xt.run.start_hold_tag) {
        return false;
    }
    return xt.pending_count == 0U && !xt.links_event_pending && !atomic_load(&xt.links_dirty) &&
           !atomic_load(&xt.lifecycle_overflow) && !atomic_load(&xt.connect_result_generation) && !xt.owner_drain_retry_at;
}

/* ----------------------------------------------------------- stack upcalls */

void xiaotai_rtc_on_system(xiaotai_rtc_system_t event, int detail)
{
    switch (event) {
    case XIAOTAI_RTC_STARTED:
        atomic_fetch_or(&xt.upcalls, XT_UP_STARTED);
        break;
    case XIAOTAI_RTC_STOPPED: {
        const bool monitor = xt.config.fail_environment_on_unsolicited_stop;
        if (monitor) {
            xiaotai_critical_enter(&xt.lock);
            if (!(atomic_load(&xt.upcalls) & XT_UP_STOPPED)) {
                xt.stop_environment_epoch = xt.environment_epoch;
                xt.stop_report_expected = xt.stop_request_committed;
                atomic_store(&xt.stop_detail, detail);
                atomic_fetch_or(&xt.upcalls, XT_UP_STOPPED);
            }
            xiaotai_critical_exit(&xt.lock);
        } else {
            atomic_store(&xt.stop_detail, detail);
            atomic_fetch_or(&xt.upcalls, XT_UP_STOPPED);
        }
        break;
    }
    case XIAOTAI_RTC_ALERT:
        atomic_fetch_or(&xt.upcalls, XT_UP_ALERT);
        break;
    }
    /* From inside xiaotai_rtc_start or _stop the worker is the caller and
     * looks at the bits after the call; elsewhere it is woken. */
    if (!xt_on_worker()) xt_worker_kick();
}

/* ------------------------------------------------------- owner event handlers */

static bool handlers_live(void)
{
    return atomic_load(&xt.sized) && !atomic_load(&xt.provisioning);
}

static void condition_changed(void)
{
    atomic_fetch_add(&xt.conditions, 1U);
    xt_worker_kick();
}

static void on_net_state(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    /* A failed worker start releases the reserve; later Wi-Fi events
     * must not allocate it again with no worker to consume it. */
    if (!handlers_live() || !atomic_load(&xt.worker_running)) return;
    const int state = event->args.value;
    if (state != (int)XIAOTAI_NET_STOPPED && state != (int)XIAOTAI_NET_PREPARING) network_started();
    condition_changed();
}

static void on_condition(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    if (handlers_live()) condition_changed();
}

static void on_net_trace(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    if (!handlers_live()) return;
    atomic_store(&xt.trace_changed, true);
    xt_worker_kick();
}

static void on_binding_needed(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    if (!handlers_live()) return;
    atomic_store(&xt.unbound, true);
    condition_changed();
}

static void on_platform_online(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    if (!handlers_live()) return;
    /* After a binding was needed the platform comes online again only once
     * the device is bound. */
    atomic_store(&xt.unbound, false);
    condition_changed();
}

static void on_transport_state(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    if (handlers_live()) xiaotai_platform_profile_ready(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
}

static xiaotai_err_t environment_begin(const char *reason, void *ctx)
{
    (void)ctx;
    if (strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT)) return XIAOTAI_OK;
    xiaotai_critical_enter(&xt.lock);
    ++xt.environment_epoch;
    xt.environment_error = XIAOTAI_OK;
    xiaotai_critical_exit(&xt.lock);
    return XIAOTAI_OK;
}

static void environment_release(const char *reason, void *ctx)
{
    (void)ctx;
    if (strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT)) return;
    xiaotai_critical_enter(&xt.lock);
    ++xt.environment_epoch;
    xt.environment_error = XIAOTAI_OK;
    xiaotai_critical_exit(&xt.lock);
}

static xiaotai_err_t environment_check(const char *reason, void *ctx, bool *ready)
{
    (void)ctx;
    if (strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT)) return XIAOTAI_OK;
    xiaotai_critical_enter(&xt.lock);
    const xiaotai_err_t err = xt.environment_error;
    const uint32_t epoch = xt.environment_epoch;
    /* The owner must not approve writes while this hold's raw callback is
     * queued or being classified; a requested STOP acknowledgement can pass. */
    *ready = !(epoch & 1U) || (epoch != xt.classifying_stop_epoch &&
        (epoch != xt.stop_environment_epoch || !(atomic_load(&xt.upcalls) & XT_UP_STOPPED)));
    xiaotai_critical_exit(&xt.lock);
    return err;
}

xiaotai_err_t xt_run_init(void)
{
    static const xiaotai_subscriber_descriptor_t subscriptions[] = {
        {XIAOTAI_EVENT_NET_STATE, on_net_state, NULL},
        {XIAOTAI_EVENT_NET_UP, on_condition, NULL},
        {XIAOTAI_EVENT_NET_DOWN, on_condition, NULL},
        {XIAOTAI_EVENT_NET_TRACE, on_net_trace, NULL},
        {XIAOTAI_EVENT_PLATFORM_DISCOVERED, on_condition, NULL},
        {XIAOTAI_EVENT_PLATFORM_ONLINE, on_platform_online, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, on_condition, NULL},
        {XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, on_binding_needed, NULL},
        {XIAOTAI_EVENT_TRANSPORT_STATE, on_transport_state, NULL},
    };
    for (size_t i = 0; i < sizeof(subscriptions) / sizeof(subscriptions[0]); ++i) {
        const xiaotai_err_t err = xiaotai_event_subscribe_static(&subscriptions[i]);
        if (err != XIAOTAI_OK) return err;
    }
    if (xt.config.fail_environment_on_unsolicited_stop) {
        static const xiaotai_quiesce_gate_t gate = {
            .acquire = environment_begin, .release = environment_release, .check = environment_check,
        };
        return xiaotai_quiesce_gate_register("transport", &gate, NULL);
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xt_run_start(void)
{
    xiaotai_err_t err = xiaotai_queue_init(&xt.jobs, XT_JOB_DEPTH, sizeof(xt_job_t), XIAOTAI_MEM_BULK);
    if (err != XIAOTAI_OK) return err;
    xt.jobs_ready = true;
    xt_account((int64_t)(XT_JOB_DEPTH * sizeof(xt_job_t)));
    atomic_store(&xt.state, XIAOTAI_TRANSPORT_DOWN);
    atomic_store(&xt.wait, XIAOTAI_TRANSPORT_WAIT_IDENTITY);
    const int64_t now = xt_now_ms();
    xt.run.next_evaluate = now;
    xt.holds_seen = atomic_load(&xt.hold_count);
    xt.run.conditions_seen = atomic_load(&xt.conditions);
    /* With an identity at boot the start's hold is taken here, before the
     * platform can log in. */
    if (xt.config.start_before_signaling && !xiaotai_platform_start_failed() && atomic_load(&xt.hold_count) == 0U &&
        xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID) {
        take_start_hold(now);
    }
    /* The network may have reported its start before this engine started. */
    xiaotai_net_status_t net;
    memset(&net, 0, sizeof(net));
    xiaotai_net_status(&net);
    if (net.state != XIAOTAI_NET_STOPPED && net.state != XIAOTAI_NET_PREPARING) network_started();

    xt.worker_config = (xiaotai_task_config_t){
        .name = XT_WORKER_NAME,
        .entry = worker_main,
        .arg = NULL,
        .stack_bytes = xt.worker_stack_bytes,
        .stack = XIAOTAI_MEM_BULK,
        .priority = xt.worker_priority,
        .core = XIAOTAI_CORE_ANY,
    };
    atomic_store(&xt.worker_quit, false);
    atomic_store(&xt.worker_running, true);
    err = xiaotai_task_create(&xt.worker_config, &xt.worker);
    if (err != XIAOTAI_OK) {
        atomic_store(&xt.worker_running, false);
        release_start_hold();
        start_reserve_give_up(false);
        xiaotai_queue_deinit(&xt.jobs);
        xt.jobs_ready = false;
        xt_account(-(int64_t)(XT_JOB_DEPTH * sizeof(xt_job_t)));
        return err;
    }
    xt.worker_created = true;
    xt_account((int64_t)xt.worker_stack_bytes);
    return XIAOTAI_OK;
}

bool xt_run_stop_worker(void)
{
    if (xt.worker_created) {
        if (atomic_load(&xt.worker_running)) {
            atomic_store(&xt.worker_quit, true);
            xt_worker_kick();
        }
        if (!xiaotai_signal_take(&xt.worker_exited, XT_WORKER_EXIT_WAIT_MS)) {
            XIAOTAI_LOGE(XT_TAG, "worker did not stop");
            return false;
        }
        xt.worker_created = false;
    } else {
        release_start_hold();
    }
    start_reserve_give_up(false);
    if (xt.jobs_ready) {
        xiaotai_queue_deinit(&xt.jobs);
        xt.jobs_ready = false;
    }
    return true;
}

/* ------------------------------------------------------------- run control */

xiaotai_err_t xiaotai_transport_hold(uint32_t *hold)
{
    if (!hold) return XIAOTAI_ERR_INVALID_ARG;
    uint32_t id;
    do {
        id = atomic_fetch_add(&xt.hold_next, 1U) + 1U;
    } while (id == 0U);
    for (size_t i = 0; i < XT_HOLDS_MAX; ++i) {
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong(&xt.holds[i], &expected, id)) {
            atomic_fetch_add(&xt.hold_count, 1U);
            *hold = id;
            xt_worker_kick();
            return XIAOTAI_OK;
        }
    }
    return XIAOTAI_ERR_NO_MEM;
}

void xiaotai_transport_release(uint32_t hold)
{
    if (!hold) return;
    for (size_t i = 0; i < XT_HOLDS_MAX; ++i) {
        uint32_t expected = hold;
        if (atomic_compare_exchange_strong(&xt.holds[i], &expected, 0U)) {
            atomic_fetch_sub(&xt.hold_count, 1U);
            xt_worker_kick();
            return;
        }
    }
}

xiaotai_err_t xiaotai_transport_restart(void)
{
    if (!atomic_load(&xt.sized) || atomic_load(&xt.provisioning) || !atomic_load(&xt.worker_running) ||
        atomic_load(&xt.hold_count) != 0U) {
        return XIAOTAI_ERR_INVALID_STATE;
    }
    const int state = atomic_load(&xt.state);
    const int wait = atomic_load(&xt.wait);
    const bool accepted = state == (int)XIAOTAI_TRANSPORT_FAULT || state == (int)XIAOTAI_TRANSPORT_STARTING ||
                          state == (int)XIAOTAI_TRANSPORT_ONLINE || state == (int)XIAOTAI_TRANSPORT_STOPPING ||
                          wait == (int)XIAOTAI_TRANSPORT_WAIT_START_FAILED ||
                          wait == (int)XIAOTAI_TRANSPORT_WAIT_RETRY;
    if (!accepted) return XIAOTAI_ERR_INVALID_STATE;
    atomic_fetch_add(&xt.restarts, 1U);
    xt_worker_kick();
    return XIAOTAI_OK;
}
