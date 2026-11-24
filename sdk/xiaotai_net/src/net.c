// SPDX-License-Identifier: MIT
/* Lifecycle of the network engine: init (state, queue, built-in drivers,
 * units, the worker), start (binding the part in use to its driver), the
 * worker with its queue, deadlines and the one blocking call slot, the
 * bring-up of the part on XIAOTAI_EVENT_STARTED, and the host tests' reset.
 * The link state machine, storage, scanning, provisioning, the gate and the
 * console commands are units (internal.h) driven from here. */
#include <stdatomic.h>
#include <string.h>
#include "internal.h"
#include "net_platform.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/sys.h"

static const char *TAG = "net";

/* Normal background priority, as the deployed connection tasks ran. */
#define WORKER_PRIORITY 4U
/* Parts the engine looks at; boards list one today. */
#define PARTS_MAX 8U
/* Reset (host tests) waits this long for the worker to leave. */
#define STOP_WAIT_MS 5000U
#define CONTROL_NOTIFY_RETRY_US 100000

_Static_assert(sizeof(net_message_t) == NET_MESSAGE_BYTES, "queue items are 64 bytes");

#define NET_UNIT_DECLARE(unit) extern const net_unit_t unit;
NET_UNIT_LIST(NET_UNIT_DECLARE)
#define NET_UNIT_ENTRY(unit) &unit,
static const net_unit_t *const s_units[] = {NET_UNIT_LIST(NET_UNIT_ENTRY) NULL};

/* Kernel objects: static, internal RAM. */
static xiaotai_queue_t s_queue;
static xiaotai_task_t s_worker;
static xiaotai_signal_t s_worker_exited;
static atomic_bool s_worker_running;
static atomic_bool s_queue_ready;
static atomic_bool s_initialized;
static atomic_bool s_started;
static atomic_bool s_link_controls_ready;
static atomic_bool s_events_lost;
/* Lifecycle edges are not expendable when per-attempt messages fill the queue. */
enum { RADIO_STOP_SEEN = 1U, RADIO_START_SEEN = 2U };
static atomic_uint s_radio_pending;

/* Worker only. */
static uint32_t s_token;
static bool s_bringup_begun;
static bool s_stage_reported;
static xiaotai_network_stage_t s_last_stage;

static void (*s_restart)(void);
static void (*s_tap)(const xiaotai_net_driver_event_t *event, void *ctx);
static void *s_tap_ctx;

/* The call slot protocol (net_call). */
typedef enum { CALL_IDLE = 0, CALL_PENDING, CALL_DONE, CALL_ABANDONED } call_state_t;
static xiaotai_signal_t s_call_free;
static xiaotai_signal_t s_call_done;
static xiaotai_critical_t s_call_lock;
static uint32_t s_call_sequence;
static call_state_t s_call_state;
static xiaotai_err_t s_call_result;
static bool s_call_ready;

/* ---------------------------------------------------------------- messages */

void net_event_pack(const xiaotai_net_driver_event_t *event, net_message_t *message)
{
    memset(message, 0, sizeof(*message));
    message->kind = NET_MSG_DRIVER;
    message->event = (uint8_t)event->kind;
    message->token = event->token;
    switch (event->kind) {
    case XIAOTAI_NET_DRIVER_JOINED:
        memcpy(message->u.joined.ssid, event->ssid, sizeof(message->u.joined.ssid));
        message->u.joined.ssid[XIAOTAI_NET_SSID_MAX] = '\0';
        message->u.joined.channel = event->channel;
        break;
    case XIAOTAI_NET_DRIVER_ADDRESS:
        message->u.address.ipv4 = event->ipv4;
        message->u.address.gateway_ipv4 = event->gateway_ipv4;
        if (event->ipv6_valid) {
            message->flags |= NET_EVENT_IPV6_VALID;
            memcpy(message->u.address.ipv6, event->ipv6, sizeof(message->u.address.ipv6));
        }
        break;
    case XIAOTAI_NET_DRIVER_LOST:
        message->u.result.loss = (int32_t)event->loss;
        message->u.result.code = event->code;
        if (event->signal_valid) {
            message->flags |= NET_EVENT_SIGNAL_VALID;
            message->signal_dbm = event->signal_dbm;
        }
        break;
    case XIAOTAI_NET_DRIVER_SCAN_DONE:
        message->u.result.error = event->error;
        break;
    case XIAOTAI_NET_DRIVER_STOPPED:
    case XIAOTAI_NET_DRIVER_STARTED:
        break;
    }
}

void net_event_unpack(const net_message_t *message, xiaotai_net_driver_event_t *event)
{
    memset(event, 0, sizeof(*event));
    event->kind = (xiaotai_net_driver_event_kind_t)message->event;
    event->token = message->token;
    switch (event->kind) {
    case XIAOTAI_NET_DRIVER_JOINED:
        memcpy(event->ssid, message->u.joined.ssid, sizeof(event->ssid));
        event->ssid[XIAOTAI_NET_SSID_MAX] = '\0';
        event->channel = message->u.joined.channel;
        break;
    case XIAOTAI_NET_DRIVER_ADDRESS:
        event->ipv4 = message->u.address.ipv4;
        event->gateway_ipv4 = message->u.address.gateway_ipv4;
        if (message->flags & NET_EVENT_IPV6_VALID) {
            event->ipv6_valid = true;
            memcpy(event->ipv6, message->u.address.ipv6, sizeof(event->ipv6));
        }
        break;
    case XIAOTAI_NET_DRIVER_LOST:
        event->loss = (xiaotai_net_loss_t)message->u.result.loss;
        event->code = message->u.result.code;
        if (message->flags & NET_EVENT_SIGNAL_VALID) {
            event->signal_valid = true;
            event->signal_dbm = message->signal_dbm;
        }
        break;
    case XIAOTAI_NET_DRIVER_SCAN_DONE:
        event->error = message->u.result.error;
        break;
    case XIAOTAI_NET_DRIVER_STOPPED:
    case XIAOTAI_NET_DRIVER_STARTED:
        break;
    }
}

/* Never waits: a full queue is counted and reported to the caller. */
static xiaotai_err_t send_message(const net_message_t *message)
{
    if (xiaotai_queue_send(&s_queue, message, 0)) return XIAOTAI_OK;
    net_dropped_count();
    return XIAOTAI_ERR_TIMEOUT;
}

void net_driver_emit(void *ctx, const xiaotai_net_driver_event_t *event)
{
    (void)ctx;
    if (!event || !atomic_load(&s_queue_ready)) return;
    if (event->kind == XIAOTAI_NET_DRIVER_STOPPED || event->kind == XIAOTAI_NET_DRIVER_STARTED) {
        if (event->kind == XIAOTAI_NET_DRIVER_STOPPED) atomic_store(&s_radio_pending, RADIO_STOP_SEEN);
        else atomic_fetch_or(&s_radio_pending, RADIO_START_SEEN);
        /* A full queue is already a wakeup; the notice lives in the mailbox. */
        const net_message_t wake = {.kind = NET_MSG_NONE};
        (void)xiaotai_queue_send(&s_queue, &wake, 0);
        return;
    }
    net_message_t message;
    net_event_pack(event, &message);
    if (send_message(&message) != XIAOTAI_OK) atomic_store(&s_events_lost, true);
}

bool net_events_lost_take(void) { return atomic_exchange(&s_events_lost, false); }

xiaotai_err_t net_post(net_job_fn_t fn, uint32_t arg, const void *data, size_t size)
{
    if (!fn) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&s_queue_ready)) return XIAOTAI_ERR_INVALID_STATE;
    if (size > NET_JOB_DATA_MAX || (size && !data)) return XIAOTAI_ERR_INVALID_SIZE;
    net_message_t message;
    memset(&message, 0, sizeof(message));
    message.kind = NET_MSG_JOB;
    message.u.job.fn = fn;
    message.u.job.arg = arg;
    message.u.job.size = (uint8_t)size;
    if (size) memcpy(message.u.job.data, data, size);
    return send_message(&message);
}

bool net_on_worker(void) { return atomic_load(&s_worker_running) && xiaotai_task_is_current(&s_worker); }

xiaotai_err_t net_link_control_enqueue(net_job_fn_t fn, uint32_t arg, bool retry)
{
    if (!atomic_load(&s_link_controls_ready)) return XIAOTAI_ERR_INVALID_STATE;
    if (!xiaotai_net_driver_bound()) return XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_err_t result = net_gate_config_begin();
    if (result != XIAOTAI_OK) return result;
    xiaotai_net_status_t *status = net_status_edit();
    if (!status->control_pending && !net_provision_submission_pending() &&
        !(retry && status->prepare.retry_blocked)) {
        result = net_post(fn, arg, NULL, 0);
        if (result == XIAOTAI_OK) {
            status->control_pending = true;
            net_status_commit(NET_EMIT_NONE);
            return XIAOTAI_OK;
        }
    } else {
        result = XIAOTAI_ERR_INVALID_STATE;
    }
    net_snapshot_cancel();
    net_gate_config_end();
    return result;
}

static void control_notify(void)
{
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    const xiaotai_args_t args = {.value = status.state};
    const xiaotai_err_t sent = xiaotai_event_emit(XIAOTAI_EVENT_NET_STATE, &args, &status, sizeof(status));
    if (sent == XIAOTAI_ERR_TIMEOUT || sent == XIAOTAI_ERR_NO_MEM) {
        net_deadline_set(NET_DEADLINE_CONTROL, xiaotai_time_us() + CONTROL_NOTIFY_RETRY_US, control_notify);
    } else {
        net_deadline_clear(NET_DEADLINE_CONTROL);
    }
}

void net_link_control_finish(xiaotai_err_t result)
{
    xiaotai_net_status_t *status = net_status_edit();
    status->control_pending = false;
    status->control_error = result;
    net_status_commit(NET_EMIT_NONE);
    net_gate_config_end();
    /* HELD can precede the terminal event by seconds. A provisioning owner
     * needs the completion notification even when the state has not changed. */
    control_notify();
}

void net_set_driver_tap(void (*tap)(const xiaotai_net_driver_event_t *event, void *ctx), void *ctx)
{
    s_tap_ctx = ctx;
    s_tap = tap;
}

uint32_t net_token_next(void)
{
    if (++s_token == 0U) ++s_token;
    return s_token;
}

/* ------------------------------------------------------------ call slot */

static uint32_t remaining_ms(uint32_t timeout_ms, int64_t since_us)
{
    if (timeout_ms == XIAOTAI_WAIT_FOREVER) return XIAOTAI_WAIT_FOREVER;
    const int64_t spent_ms = (xiaotai_time_us() - since_us) / 1000;
    return spent_ms >= (int64_t)timeout_ms ? 0U : timeout_ms - (uint32_t)spent_ms;
}

static xiaotai_err_t wait_call_signal(xiaotai_signal_t *signal, uint32_t timeout_ms, int64_t since,
                                      bool (*active)(uint32_t), uint32_t token)
{
    const uint32_t cancel_poll_ms = 20;
    if (!active) return xiaotai_signal_take(signal, remaining_ms(timeout_ms, since)) ?
                            XIAOTAI_OK : XIAOTAI_ERR_TIMEOUT;
    do {
        if (!active(token)) return XIAOTAI_ERR_INVALID_STATE;
        const uint32_t left_ms = remaining_ms(timeout_ms, since);
        if (xiaotai_signal_take(signal, left_ms < cancel_poll_ms ? left_ms : cancel_poll_ms)) return XIAOTAI_OK;
        if (!left_ms) break;
    } while (remaining_ms(timeout_ms, since));
    return XIAOTAI_ERR_TIMEOUT;
}

static xiaotai_err_t call_worker(xiaotai_err_t (*fn)(net_call_t *call), uint32_t arg, bool (*active)(uint32_t),
                                  const void *in, size_t in_size, void *out, size_t out_capacity,
                                  size_t *out_size, uint32_t timeout_ms)
{
    if (out_size) *out_size = 0;
    if (!fn) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&s_queue_ready) || net_on_worker()) return XIAOTAI_ERR_INVALID_STATE;
    if (in_size > NET_CALL_DATA_MAX || (in_size && !in)) return XIAOTAI_ERR_INVALID_SIZE;
    const int64_t since = xiaotai_time_us();
    const xiaotai_err_t slot = wait_call_signal(&s_call_free, timeout_ms, since, active, arg);
    if (slot != XIAOTAI_OK) return slot;
    if (active && !active(arg)) {
        xiaotai_signal_give(&s_call_free);
        return XIAOTAI_ERR_INVALID_STATE;
    }

    net_call_t *call = &net_state->call;
    call->fn = fn;
    call->active = active;
    call->arg = arg;
    call->size = in_size;
    if (in_size) memcpy(call->data, in, in_size);
    net_message_t message;
    memset(&message, 0, sizeof(message));
    message.kind = NET_MSG_CALL;
    xiaotai_critical_enter(&s_call_lock);
    message.token = ++s_call_sequence;
    s_call_state = CALL_PENDING;
    xiaotai_critical_exit(&s_call_lock);
    if (send_message(&message) != XIAOTAI_OK) {
        memset(call, 0, sizeof(*call));
        xiaotai_critical_enter(&s_call_lock);
        s_call_state = CALL_IDLE;
        xiaotai_critical_exit(&s_call_lock);
        xiaotai_signal_give(&s_call_free);
        return XIAOTAI_ERR_TIMEOUT;
    }

    const xiaotai_err_t waited = wait_call_signal(&s_call_done, timeout_ms, since, active, arg);
    const bool done = waited == XIAOTAI_OK;
    bool late = false;
    xiaotai_critical_enter(&s_call_lock);
    if (!done) {
        if (s_call_state == CALL_DONE) {
            late = true;
        } else {
            /* The worker frees the slot once the call has run. */
            s_call_state = CALL_ABANDONED;
        }
    }
    xiaotai_critical_exit(&s_call_lock);
    if (!done && !late) return waited;
    /* Finished just after the wait ended: the worker gives the signal right
     * after marking it done, so take it before the slot changes hands. */
    if (late) (void)xiaotai_signal_take(&s_call_done, XIAOTAI_WAIT_FOREVER);

    const xiaotai_err_t result = s_call_result;
    const size_t size = call->size < out_capacity ? call->size : out_capacity;
    if (out && size) memcpy(out, call->data, size);
    if (out_size) *out_size = out ? size : 0U;
    memset(call, 0, sizeof(*call));
    xiaotai_critical_enter(&s_call_lock);
    s_call_state = CALL_IDLE;
    xiaotai_critical_exit(&s_call_lock);
    xiaotai_signal_give(&s_call_free);
    return result;
}

xiaotai_err_t net_call(xiaotai_err_t (*fn)(net_call_t *call), uint32_t arg, const void *in, size_t in_size,
                       void *out, size_t out_capacity, size_t *out_size, uint32_t timeout_ms)
{
    return call_worker(fn, arg, NULL, in, in_size, out, out_capacity, out_size, timeout_ms);
}

xiaotai_err_t net_call_cancellable(xiaotai_err_t (*fn)(net_call_t *call), uint32_t token,
                                  bool (*active)(uint32_t), const void *in, size_t in_size,
                                  void *out, size_t out_capacity, uint32_t timeout_ms)
{
    if (!active || !token) return XIAOTAI_ERR_INVALID_ARG;
    return call_worker(fn, token, active, in, in_size, out, out_capacity, NULL, timeout_ms);
}

static void run_call(uint32_t sequence)
{
    xiaotai_critical_enter(&s_call_lock);
    const bool current = sequence == s_call_sequence &&
                         (s_call_state == CALL_PENDING || s_call_state == CALL_ABANDONED);
    xiaotai_critical_exit(&s_call_lock);
    if (!current) return;

    net_call_t *call = &net_state->call;
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (!net_state->restarting && (!call->active || call->active(call->arg))) {
        result = call->fn(call);
        if (call->size > NET_CALL_DATA_MAX) call->size = NET_CALL_DATA_MAX;
    } else {
        memset(call, 0, sizeof(*call));
    }
    xiaotai_critical_enter(&s_call_lock);
    const bool abandoned = s_call_state == CALL_ABANDONED;
    s_call_result = result;
    s_call_state = abandoned ? CALL_IDLE : CALL_DONE;
    xiaotai_critical_exit(&s_call_lock);
    /* Only handing back the slot semaphore lets another caller use it. */
    if (abandoned) memset(call, 0, sizeof(*call));
    xiaotai_signal_give(abandoned ? &s_call_free : &s_call_done);
}

/* -------------------------------------------------------------- deadlines */

void net_deadline_set(net_deadline_t which, int64_t due_us, void (*fn)(void))
{
    if ((unsigned)which >= NET_DEADLINE_COUNT) return;
    net_deadline_slot_t *slot = &net_state->deadlines[which];
    slot->due_us = fn ? due_us : 0;
    slot->fn = slot->due_us ? fn : NULL;
}

void net_deadline_clear(net_deadline_t which) { net_deadline_set(which, 0, NULL); }

int64_t net_deadline_due(net_deadline_t which)
{
    if ((unsigned)which >= NET_DEADLINE_COUNT) return 0;
    return net_state->deadlines[which].due_us;
}

/* Queue wait: up to the nearest deadline, rounded up so the wait never ends
 * before it (the port's waits never end early either). */
static uint32_t wait_ms(void)
{
    int64_t nearest = 0;
    for (unsigned i = 0; i < NET_DEADLINE_COUNT; ++i) {
        const int64_t due = net_state->deadlines[i].due_us;
        if (due && (!nearest || due < nearest)) nearest = due;
    }
    if (!nearest) return XIAOTAI_WAIT_FOREVER;
    const int64_t now = xiaotai_time_us();
    if (nearest <= now) return 0U;
    const int64_t ms = (nearest - now + 999) / 1000;
    return ms >= (int64_t)XIAOTAI_WAIT_FOREVER ? XIAOTAI_WAIT_FOREVER - 1U : (uint32_t)ms;
}

static void run_due(void)
{
    /* A handler may arm other slots; each pass runs every slot due by then,
     * and the number of passes is bounded so a slot re-armed in the past
     * cannot keep the worker from its queue. */
    for (unsigned pass = 0; pass < NET_DEADLINE_COUNT; ++pass) {
        bool ran = false;
        for (unsigned i = 0; i < NET_DEADLINE_COUNT; ++i) {
            net_deadline_slot_t *slot = &net_state->deadlines[i];
            if (!slot->due_us || slot->due_us > xiaotai_time_us()) continue;
            void (*fn)(void) = slot->fn;
            slot->due_us = 0;
            slot->fn = NULL;
            if (fn && !net_state->restarting) fn();
            ran = true;
        }
        if (!ran) return;
    }
}

/* ---------------------------------------------------------------- bring-up */

void net_driver_start_args(xiaotai_net_driver_start_t *args)
{
    memset(args, 0, sizeof(*args));
    args->fallback_dns = XIAOTAI_NET_FALLBACK_DNS_IPV4;
    args->emit = net_driver_emit;
    args->emit_ctx = NULL;
    args->fallback_dns_rule = net_state->config.dns_fallback;
    args->driver_storage = net_state->config.driver_storage;
}

void net_set_restart(void (*restart)(void)) { s_restart = restart; }

void net_restart_now(void)
{
    net_state->restarting = true;
    void (*restart)(void) = s_restart ? s_restart : xiaotai_restart;
    restart();
}

static bool final_failure(xiaotai_network_stage_t stage)
{
    return stage == XIAOTAI_NETWORK_NO_LINK || stage == XIAOTAI_NETWORK_UNFINISHED ||
           stage == XIAOTAI_NETWORK_BAD_IMAGE || stage == XIAOTAI_NETWORK_UNSUPPORTED ||
           stage == XIAOTAI_NETWORK_POWER_CYCLE;
}

static xiaotai_net_prepare_stage_t progress_stage(xiaotai_network_stage_t stage)
{
    switch (stage) {
    case XIAOTAI_NETWORK_CHECKING: return XIAOTAI_NET_PREPARE_CHECKING;
    case XIAOTAI_NETWORK_VERIFYING: return XIAOTAI_NET_PREPARE_VERIFYING;
    case XIAOTAI_NETWORK_WRITING: return XIAOTAI_NET_PREPARE_WRITING;
    case XIAOTAI_NETWORK_ACTIVATING: return XIAOTAI_NET_PREPARE_ACTIVATING;
    case XIAOTAI_NETWORK_RESTARTING: return XIAOTAI_NET_PREPARE_RESTARTING;
    case XIAOTAI_NETWORK_READY: return XIAOTAI_NET_PREPARE_READY;
    default: return XIAOTAI_NET_PREPARE_FAILED;
    }
}

static xiaotai_net_fault_t fault_of(xiaotai_network_stage_t stage)
{
    switch (stage) {
    case XIAOTAI_NETWORK_NO_LINK: return XIAOTAI_NET_FAULT_NO_LINK;
    case XIAOTAI_NETWORK_UNFINISHED: return XIAOTAI_NET_FAULT_INTERRUPTED;
    case XIAOTAI_NETWORK_BAD_IMAGE: return XIAOTAI_NET_FAULT_BAD_IMAGE;
    case XIAOTAI_NETWORK_UNSUPPORTED: return XIAOTAI_NET_FAULT_UNSUPPORTED;
    case XIAOTAI_NETWORK_POWER_CYCLE: return XIAOTAI_NET_FAULT_POWER_CYCLE;
    default: return XIAOTAI_NET_FAULT_ERROR;
    }
}

void net_report_prepare(xiaotai_network_stage_t stage, uint8_t percent, xiaotai_err_t result)
{
    xiaotai_net_status_t *status = net_status_edit();
    status->prepare.stage = result == XIAOTAI_OK ? progress_stage(stage) : XIAOTAI_NET_PREPARE_FAILED;
    status->prepare.percent = percent > 100U ? 100U : percent;
    status->prepare.error = result;
    status->prepare.fault = result == XIAOTAI_OK ? XIAOTAI_NET_FAULT_NONE : fault_of(stage);
    net_status_commit(NET_EMIT_PREPARE);
}

/* [worker, the part's prepare()] Progress stages are published as they
 * come; a final failure is published with its result when prepare()
 * returns. */
static void prepare_progress(xiaotai_network_stage_t stage, uint8_t percent, void *ctx)
{
    (void)ctx;
    s_stage_reported = true;
    s_last_stage = stage;
    if (stage == XIAOTAI_NETWORK_WRITING) net_coprocessor_invalidate();
    if (final_failure(stage)) return;
    xiaotai_net_status_t *status = net_status_edit();
    status->prepare.stage = progress_stage(stage);
    status->prepare.percent = percent > 100U ? 100U : percent;
    net_status_commit(NET_EMIT_PREPARE);
}

static void bringup_failed(xiaotai_net_fault_t fault, xiaotai_err_t error)
{
    net_gate_set_bringup(false);
    xiaotai_net_status_t *status = net_status_edit();
    status->prepare.stage = XIAOTAI_NET_PREPARE_FAILED;
    status->prepare.fault = fault;
    status->prepare.error = error;
    net_status_commit(NET_EMIT_PREPARE);
    net_set_state(XIAOTAI_NET_BLOCKED);
    XIAOTAI_LOGE(TAG, "bring-up failed: fault=%d error=0x%x", (int)fault, (unsigned)error);
}

void net_bringup(bool manual_retry)
{
    net_t *net = net_state;
    const xiaotai_network_t *part = net->part;
    if (!part || !net->driver || net->driver_running || net->restarting) return;
    if (manual_retry &&
        (net->config.block_retry_after_driver_start_failure || net->config.restrict_blocked_retry_to_recoverable)) {
        /* Recheck work admitted before the bring-up result became observable. */
        const bool retry_disallowed = net_status_edit()->prepare.retry_blocked;
        net_snapshot_cancel();
        if (retry_disallowed) return;
    }
    net_gate_set_bringup(true);

    if (part->prepare) {
        const bool publishes_identity = part->prepare_with_identity && part->coprocessor_info;
        if (publishes_identity) net_coprocessor_publish_prepare_identity(NULL, NULL);
        s_stage_reported = false;
        s_last_stage = XIAOTAI_NETWORK_CHECKING;
        xiaotai_net_status_t *status = net_status_edit();
        status->state = XIAOTAI_NET_PREPARING;
        memset(&status->prepare, 0, sizeof(status->prepare));
        net_status_commit(NET_EMIT_STATE);

        const xiaotai_err_t result = publishes_identity ?
            part->prepare_with_identity(part->self, manual_retry, prepare_progress,
                                        net_coprocessor_publish_prepare_identity, NULL) :
            part->prepare(part->self, manual_retry, prepare_progress, NULL);
        if (!publishes_identity) net_coprocessor_cache_prepare();
        if (result == XIAOTAI_OK && s_stage_reported && s_last_stage == XIAOTAI_NETWORK_RESTARTING) {
            /* The coprocessor already restarts with new firmware: no orderly
             * restart, no holds, before the host link's watchdog fires. */
            XIAOTAI_LOGI(TAG, "coprocessor restarts with new firmware; restarting");
            net_restart_now();
            return;
        }
        if (!(result == XIAOTAI_OK && s_stage_reported && s_last_stage == XIAOTAI_NETWORK_READY)) {
            const bool final = s_stage_reported && final_failure(s_last_stage);
            /* An XIAOTAI_OK that names no outcome is a fault of the hook. */
            const xiaotai_err_t error = result != XIAOTAI_OK ? result
                                        : final            ? XIAOTAI_FAIL
                                                           : XIAOTAI_ERR_INVALID_STATE;
            bringup_failed(final ? fault_of(s_last_stage) : XIAOTAI_NET_FAULT_ERROR, error);
            return;
        }
    }

    xiaotai_net_driver_start_t args;
    net_driver_start_args(&args);
    const xiaotai_err_t started = net->driver->start(net->driver->self, part, &args);
    if (started != XIAOTAI_OK) {
        bringup_failed(XIAOTAI_NET_FAULT_START, started);
        return;
    }
    net->driver_running = true;
    net_gate_set_bringup(false);
    for (const net_unit_t *const *unit = s_units; *unit; ++unit) {
        if ((*unit)->driver_started) (*unit)->driver_started();
    }
    /* Without credentials (or a unit that decides otherwise) the network runs
     * unconfigured. */
    const xiaotai_net_state_t state = net_state_now();
    if (state == XIAOTAI_NET_STOPPED || state == XIAOTAI_NET_PREPARING) net_set_state(XIAOTAI_NET_UNCONFIGURED);
}

/* ------------------------------------------------------------------ worker */

static void dispatch_driver(const xiaotai_net_driver_event_t *event)
{
    if (s_tap) s_tap(event, s_tap_ctx);
    for (const net_unit_t *const *unit = s_units; *unit; ++unit) {
        if ((*unit)->driver_event) (*unit)->driver_event(event);
    }
}

static void dispatch_radio(void)
{
    const unsigned notices = atomic_exchange(&s_radio_pending, 0);
    if (notices & RADIO_STOP_SEEN) {
        const xiaotai_net_driver_event_t stop = {.kind = XIAOTAI_NET_DRIVER_STOPPED};
        dispatch_driver(&stop);
    }
    if (notices & RADIO_START_SEEN) {
        const xiaotai_net_driver_event_t start = {.kind = XIAOTAI_NET_DRIVER_STARTED};
        dispatch_driver(&start);
    }
}

static void dispatch(const net_message_t *message)
{
    if (net_state->restarting && message->kind != NET_MSG_CALL) return;
    switch ((net_message_kind_t)message->kind) {
    case NET_MSG_DRIVER: {
        xiaotai_net_driver_event_t event;
        net_event_unpack(message, &event);
        dispatch_driver(&event);
        break;
    }
    case NET_MSG_JOB:
        message->u.job.fn(message);
        break;
    case NET_MSG_CALL:
        run_call(message->token);
        break;
    case NET_MSG_STARTED:
        if (!s_bringup_begun) {
            s_bringup_begun = true;
            net_bringup(false);
        }
        break;
    case NET_MSG_NONE:
    case NET_MSG_STOP:
        break;
    }
}

void net_units_link_address(const xiaotai_net_credentials_t *credentials)
{
    for (const net_unit_t *const *unit = s_units; *unit; ++unit) {
        if ((*unit)->link_address) (*unit)->link_address(credentials);
    }
}

void net_units_link_provision(bool open, xiaotai_net_provision_reason_t reason)
{
    if (net_state->config.provisioning != XIAOTAI_NET_PROVISIONING_CONCURRENT) return;
    for (const net_unit_t *const *unit = s_units; *unit; ++unit) {
        if ((*unit)->link_provision) (*unit)->link_provision(open, reason);
    }
}

void net_units_maintenance_released(void)
{
    for (const net_unit_t *const *unit = s_units; *unit; ++unit) {
        if ((*unit)->maintenance_released) (*unit)->maintenance_released();
    }
}

static void worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        net_message_t message;
        if (xiaotai_queue_receive(&s_queue, &message, wait_ms())) {
            if (message.kind == NET_MSG_STOP) break;
            if (!net_state->restarting) {
                dispatch_radio();
                net_link_queue_recover();
            }
            dispatch(&message);
        }
        if (!net_state->restarting) {
            dispatch_radio();
            net_link_queue_recover();
            net_maintenance_work();
            net_coprocessor_work();
            if (!net_state->restarting) net_companion_at_work();
        }
        run_due();
    }
    for (const net_unit_t *const *unit = s_units; *unit; ++unit) {
        if ((*unit)->worker_stop) (*unit)->worker_stop();
    }
    atomic_store(&s_worker_running, false);
    xiaotai_signal_give(&s_worker_exited);
    xiaotai_task_exit();
}

/* --------------------------------------------------------------- lifecycle */

/* The first part whose kind has a driver; *index its part index. */
const xiaotai_network_t *net_part_in_use(size_t *index)
{
    for (size_t i = 0; i < PARTS_MAX; ++i) {
        const xiaotai_network_t *part = xiaotai_hal_network(i);
        if (!part) break;
        if (net_driver_for(part->desc.kind)) {
            if (index) *index = i;
            return part;
        }
    }
    return NULL;
}

static void on_system_started(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    net_message_t message;
    memset(&message, 0, sizeof(message));
    message.kind = NET_MSG_STARTED;
    if (send_message(&message) != XIAOTAI_OK) XIAOTAI_LOGE(TAG, "queue full: bring-up not started");
}

/* The three signals, all or none. */
static xiaotai_err_t init_signals(void)
{
    xiaotai_err_t result = xiaotai_signal_init(&s_call_free);
    if (result != XIAOTAI_OK) return result;
    result = xiaotai_signal_init(&s_call_done);
    if (result != XIAOTAI_OK) {
        xiaotai_signal_deinit(&s_call_free);
        return result;
    }
    result = xiaotai_signal_init(&s_worker_exited);
    if (result != XIAOTAI_OK) {
        xiaotai_signal_deinit(&s_call_done);
        xiaotai_signal_deinit(&s_call_free);
        return result;
    }
    s_call_ready = true;
    return XIAOTAI_OK;
}

static void release_objects(void)
{
    atomic_store(&s_queue_ready, false);
    if (s_call_ready) {
        xiaotai_signal_deinit(&s_call_free);
        xiaotai_signal_deinit(&s_call_done);
        xiaotai_signal_deinit(&s_worker_exited);
        s_call_ready = false;
    }
    if (net_state) {
        xiaotai_queue_deinit(&s_queue);
        memset(&s_queue, 0, sizeof(s_queue));
        net_t *state = net_state;
        net_state = NULL;
        xiaotai_mem_free(state);
    }
    net_state_close();
}

static void reset_units(void)
{
    for (const net_unit_t *const *unit = s_units; *unit; ++unit) {
        if ((*unit)->reset) (*unit)->reset();
    }
}

xiaotai_err_t xiaotai_engine_net_init(void)
{
    if (atomic_load(&s_initialized)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t result = net_state_open();
    if (result != XIAOTAI_OK) return result;

    net_t *state = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(net_t));
    if (!state) {
        net_state_close();
        return XIAOTAI_ERR_NO_MEM;
    }
    xiaotai_net_config_t configured;
    net_config_configured(&configured);
    net_config_resolve(&configured, &state->config);

    result = xiaotai_queue_init(&s_queue, XIAOTAI_NET_QUEUE_DEPTH, sizeof(net_message_t), XIAOTAI_MEM_BULK);
    if (result != XIAOTAI_OK) {
        xiaotai_mem_free(state);
        net_state_close();
        return result;
    }
    net_state = state;
    result = init_signals();
    if (result != XIAOTAI_OK) goto fail;
    xiaotai_critical_init(&s_call_lock);
    s_call_state = CALL_IDLE;
    xiaotai_signal_give(&s_call_free);
    atomic_store(&s_queue_ready, true);

    net_platform_register_drivers();
    /* Bring-up counts as a coprocessor operation from init on. */
    const xiaotai_network_t *part = xiaotai_storage_boot_error() == XIAOTAI_OK ? net_part_in_use(NULL) : NULL;
    net_gate_set_bringup(part && part->prepare);

    for (const net_unit_t *const *unit = s_units; *unit; ++unit) {
        if (!(*unit)->init) continue;
        result = (*unit)->init();
        if (result != XIAOTAI_OK) {
            XIAOTAI_LOGE(TAG, "unit %s: init failed (0x%x)", (*unit)->name, (unsigned)result);
            goto fail;
        }
    }
    result = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_STARTED, on_system_started, NULL);
    if (result != XIAOTAI_OK) goto fail;
    result = net_serial_register();
    if (result != XIAOTAI_OK) goto fail;

    const xiaotai_task_config_t worker = {
        .name = "xiaotai_net",
        .entry = worker_main,
        .arg = NULL,
        .stack_bytes = XIAOTAI_NET_WORKER_STACK_BYTES,
        /* Driver calls and part hooks may touch flash; the immediate restart
         * runs here too. */
        .stack = XIAOTAI_MEM_INTERNAL,
        .priority = WORKER_PRIORITY,
        .core = XIAOTAI_CORE_ANY,
    };
    atomic_store(&s_worker_running, true);
    result = xiaotai_task_create(&worker, &s_worker);
    if (result != XIAOTAI_OK) {
        atomic_store(&s_worker_running, false);
        goto fail;
    }
    net_config_close();
    atomic_store(&s_initialized, true);
    return XIAOTAI_OK;

fail:
    reset_units();
    net_gate_set_bringup(false);
    release_objects();
    return result;
}

void xiaotai_engine_net_start(void)
{
    if (!atomic_load(&s_initialized) || atomic_exchange(&s_started, true)) return;
    size_t index = 0;
    const xiaotai_err_t storage = xiaotai_storage_boot_error();
    const xiaotai_network_t *part = storage == XIAOTAI_OK ? net_part_in_use(&index) : NULL;
    const xiaotai_net_driver_t *driver = part ? net_driver_for(part->desc.kind) : NULL;
    net_state->part = part;
    net_state->driver = driver;
    net_drivers_bind(driver);
    net_gate_set_bringup(part && part->prepare);
    if (part) {
        /* The worker does not write the status before XIAOTAI_EVENT_STARTED. */
        xiaotai_net_status_t *status = net_status_edit();
        status->part = (uint8_t)index;
        status->kind = part->desc.kind;
        net_status_commit(NET_EMIT_NONE);
    } else if (storage != XIAOTAI_OK) {
        bringup_failed(XIAOTAI_NET_FAULT_ERROR, storage);
    } else {
        XIAOTAI_LOGW(TAG, "no network part with a driver");
    }
    for (const net_unit_t *const *unit = s_units; *unit; ++unit) {
        if ((*unit)->bind) (*unit)->bind();
    }
    atomic_store(&s_link_controls_ready, true);
}

void xiaotai_net_reset(void)
{
    atomic_store(&s_link_controls_ready, false);
    bool stopped = true;
    if (atomic_load(&s_worker_running)) {
        net_message_t stop;
        memset(&stop, 0, sizeof(stop));
        stop.kind = NET_MSG_STOP;
        stopped = xiaotai_queue_send(&s_queue, &stop, STOP_WAIT_MS) &&
                  xiaotai_signal_take(&s_worker_exited, STOP_WAIT_MS);
    }
    if (!stopped) {
        /* A worker stuck in a driver call still uses the state: keep it
         * (a leak in a failed test beats a use after free). */
        XIAOTAI_LOGE(TAG, "reset: the worker did not stop; its state is kept");
        atomic_store(&s_queue_ready, false);
        net_state = NULL;
        net_state_close();
    } else {
        reset_units();
        release_objects();
    }
    net_state_reset();
    net_config_reset();
    net_drivers_reset();
    atomic_store(&s_initialized, false);
    atomic_store(&s_started, false);
    atomic_store(&s_events_lost, false);
    atomic_store(&s_radio_pending, 0);
    s_token = 0;
    s_bringup_begun = false;
    s_stage_reported = false;
    s_restart = NULL;
    s_tap = NULL;
    s_tap_ctx = NULL;
    s_call_sequence = 0;
    s_call_state = CALL_IDLE;
    s_call_result = XIAOTAI_OK;
}
