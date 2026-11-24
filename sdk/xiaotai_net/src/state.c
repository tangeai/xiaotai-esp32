// SPDX-License-Identifier: MIT
/* The engine's shared state: the snapshots and their lock, the counters the
 * worker and other tasks share, the address generation and the gate's
 * storage, with the public functions that only read them. Kept apart from
 * the lifecycle so that a component which only asks for a snapshot (or
 * whether this is a provisioning boot) links no driver or board code. */
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/log.h"

net_t *net_state;
net_gate_t net_gate;
xiaotai_critical_t net_gate_lock;

static xiaotai_mutex_t s_snapshot_lock;
static atomic_bool s_ready;
static atomic_bool s_saved_credentials;
static _Atomic uint32_t s_generation;
static _Atomic uint32_t s_generation_last;
static _Atomic uint32_t s_dropped;
static _Atomic uint32_t s_dns_queries;

/* -------------------------------------------------------------- lifecycle */

xiaotai_err_t net_state_open(void)
{
    const xiaotai_err_t result = xiaotai_mutex_init(&s_snapshot_lock);
    if (result != XIAOTAI_OK) return result;
    xiaotai_critical_init(&net_gate_lock);
    atomic_store(&s_saved_credentials, false);
    atomic_store(&s_ready, true);
    return XIAOTAI_OK;
}

void net_state_close(void)
{
    if (!atomic_exchange(&s_ready, false)) return;
    xiaotai_mutex_deinit(&s_snapshot_lock);
}

void net_state_reset(void)
{
    atomic_store(&s_generation, 0U);
    atomic_store(&s_generation_last, 0U);
    atomic_store(&s_dropped, 0U);
    atomic_store(&s_dns_queries, 0U);
    memset(&net_gate, 0, sizeof(net_gate));
}

/* --------------------------------------------------------------- counters */

uint32_t net_dropped_events(void) { return atomic_load(&s_dropped); }

void net_dropped_count(void)
{
    uint32_t seen = atomic_load(&s_dropped);
    while (seen != UINT32_MAX && !atomic_compare_exchange_weak(&s_dropped, &seen, seen + 1U)) {
    }
}

/* -------------------------------------------------------------- generation */

uint32_t xiaotai_net_generation(void) { return atomic_load(&s_generation); }

uint32_t net_generation_issue(void)
{
    uint32_t next = atomic_fetch_add(&s_generation_last, 1U) + 1U;
    if (next == 0U) next = atomic_fetch_add(&s_generation_last, 1U) + 1U;
    atomic_store(&s_generation, next);
    return next;
}

void net_generation_clear(void) { atomic_store(&s_generation, 0U); }

/* ---------------------------------------------------------------- snapshots */

static void lock(void) { (void)xiaotai_mutex_lock(&s_snapshot_lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&s_snapshot_lock); }

xiaotai_net_status_t *net_status_edit(void)
{
    lock();
    return &net_state->status;
}

void net_snapshot_cancel(void) { unlock(); }

static void emit_status(const xiaotai_net_status_t *copy, net_emit_t emit)
{
    xiaotai_args_t args = {0};
    const char *name = NULL;
    const void *data = copy;
    size_t size = sizeof(*copy);
    switch (emit) {
    case NET_EMIT_NONE:
        return;
    case NET_EMIT_STATE:
        name = XIAOTAI_EVENT_NET_STATE;
        args.value = (int32_t)copy->state;
        break;
    case NET_EMIT_UP:
        name = XIAOTAI_EVENT_NET_UP;
        args.value = (int32_t)(copy->generation & 0x7FFFFFFFU);
        break;
    case NET_EMIT_DOWN:
        name = XIAOTAI_EVENT_NET_DOWN;
        break;
    case NET_EMIT_SIGNAL:
        name = XIAOTAI_EVENT_NET_SIGNAL;
        args.value = copy->signal_valid ? copy->signal_dbm : 0;
        break;
    case NET_EMIT_PREPARE:
        name = XIAOTAI_EVENT_NET_PREPARE;
        args.value = copy->prepare.percent;
        data = &copy->prepare;
        size = sizeof(copy->prepare);
        break;
    }
    /* The bus copies the payload; a full owner queue loses the event, the
     * snapshot still holds the state for whoever reads it. */
    (void)xiaotai_event_emit(name, &args, data, size);
}

/* The story of the link, one line per state: what a developer watching the
 * serial port needs to know without asking for the status. */
static void log_state(const xiaotai_net_status_t *status)
{
    switch (status->state) {
    case XIAOTAI_NET_PREPARING:
        XIAOTAI_LOGI("net", "preparing the network hardware");
        break;
    case XIAOTAI_NET_UNCONFIGURED:
        XIAOTAI_LOGI("net", "no network stored: provision the device (hotspot page, or wifi-set on the console)");
        break;
    case XIAOTAI_NET_CONNECTING:
        XIAOTAI_LOGI("net", "connecting (attempt %u)", (unsigned)status->failures + 1U);
        break;
    case XIAOTAI_NET_UP: {
        const uint8_t *ip = (const uint8_t *)&status->ipv4;
        XIAOTAI_LOGI("net", "connected ip=%u.%u.%u.%u channel=%u gen=%" PRIu32, ip[0], ip[1], ip[2], ip[3],
                     (unsigned)status->channel, status->generation);
        break;
    }
    case XIAOTAI_NET_WAITING:
        XIAOTAI_LOGW("net", "not connected: next attempt in %" PRIu32 " ms (failed %u in a row)",
                     status->retry_in_ms, (unsigned)status->failures);
        break;
    case XIAOTAI_NET_HELD:
        XIAOTAI_LOGI("net", "disconnected on request; stays off until retry or new credentials");
        break;
    default:
        break; /* BLOCKED is reported with its fault where it is set; STOPPED says nothing */
    }
}

void net_status_commit_link(bool state_changed, net_emit_t emit)
{
    xiaotai_net_status_t *status = &net_state->status;
    const xiaotai_net_config_t *config = &net_state->config;
    const xiaotai_net_fault_t fault = status->prepare.fault;
    if (fault == XIAOTAI_NET_FAULT_POWER_CYCLE) {
        status->prepare.retry_blocked = true;
    } else if (config->restrict_blocked_retry_to_recoverable && status->state == XIAOTAI_NET_BLOCKED) {
        status->prepare.retry_blocked = fault != XIAOTAI_NET_FAULT_NO_LINK && fault != XIAOTAI_NET_FAULT_INTERRUPTED;
    } else {
        status->prepare.retry_blocked = config->block_retry_after_driver_start_failure && fault == XIAOTAI_NET_FAULT_START;
    }
    ++net_state->status.revision;
    const xiaotai_net_status_t copy = net_state->status;
    static xiaotai_net_state_t logged = XIAOTAI_NET_STOPPED;
    const bool entered = copy.state != logged;
    logged = copy.state;
    unlock();
    if (entered) log_state(&copy);
    if (state_changed && emit != NET_EMIT_STATE) emit_status(&copy, NET_EMIT_STATE);
    emit_status(&copy, emit);
}

void net_status_commit(net_emit_t emit) { net_status_commit_link(false, emit); }

void net_set_state(xiaotai_net_state_t state)
{
    xiaotai_net_status_t *status = net_status_edit();
    if (status->state == state) {
        net_snapshot_cancel();
        return;
    }
    status->state = state;
    net_status_commit(NET_EMIT_STATE);
}

xiaotai_net_state_t net_state_now(void)
{
    if (!atomic_load(&s_ready) || !net_state) return XIAOTAI_NET_STOPPED;
    lock();
    const xiaotai_net_state_t state = net_state->status.state;
    unlock();
    return state;
}

xiaotai_net_provision_status_t *net_provision_edit(void)
{
    lock();
    return &net_state->provision;
}

static xiaotai_err_t emit_provision(const xiaotai_net_provision_status_t *copy)
{
    const xiaotai_args_t args = {.value = (int32_t)copy->state};
    return xiaotai_event_emit(XIAOTAI_EVENT_NET_PROVISION, &args, copy, sizeof(*copy));
}

xiaotai_err_t net_provision_publish(void)
{
    xiaotai_net_provision_status_t copy;
    xiaotai_net_provision_status(&copy);
    return emit_provision(&copy);
}

void net_provision_commit(bool emit)
{
    ++net_state->provision.revision;
    const xiaotai_net_provision_status_t copy = net_state->provision;
    unlock();
    if (emit) (void)emit_provision(&copy);
}

xiaotai_net_scan_status_t *net_scan_edit(void)
{
    lock();
    return &net_state->scan;
}

void net_scan_commit(void)
{
    ++net_state->scan.revision;
    unlock();
}

xiaotai_net_diag_t *net_diag_edit(void)
{
    lock();
    return &net_state->diag;
}

void net_diag_done(void) { unlock(); }

void net_diag_count_dns(void)
{
    uint32_t seen = atomic_load(&s_dns_queries);
    while (seen != UINT32_MAX && !atomic_compare_exchange_weak(&s_dns_queries, &seen, seen + 1U)) {
    }
}

/* ----------------------------------------------------------- public reads */

bool xiaotai_net_has_saved_credentials(void)
{
    return atomic_load(&s_ready) && atomic_load(&s_saved_credentials);
}

void net_saved_credentials_update(bool available)
{
    if (atomic_exchange(&s_saved_credentials, available) == available) return;
    const xiaotai_args_t args = {.value = available};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_NET_CREDENTIALS_CHANGED, &args, NULL, 0);
}

void xiaotai_net_status(xiaotai_net_status_t *status)
{
    if (!status) return;
    if (!atomic_load(&s_ready) || !net_state) {
        memset(status, 0, sizeof(*status));
        return;
    }
    lock();
    *status = net_state->status;
    unlock();
}

void xiaotai_net_provision_status(xiaotai_net_provision_status_t *status)
{
    if (!status) return;
    if (!atomic_load(&s_ready) || !net_state) {
        memset(status, 0, sizeof(*status));
        return;
    }
    lock();
    *status = net_state->provision;
    unlock();
}

bool xiaotai_net_provisioning_boot(void)
{
    return atomic_load(&s_ready) && net_state && net_state->provisioning_boot;
}

bool xiaotai_net_provisioning_exclusive(void)
{
    return atomic_load(&s_ready) && net_state && net_state->config.provisioning == XIAOTAI_NET_PROVISIONING_EXCLUSIVE;
}

bool net_diag_snapshot(xiaotai_net_diag_t *diag)
{
    if (!diag) return false;
    bool first = false;
    if (!atomic_load(&s_ready) || !net_state) {
        memset(diag, 0, sizeof(*diag));
    } else {
        lock();
        *diag = net_state->diag;
        first = net_state->link.first_loss_present;
        unlock();
    }
    diag->dropped_events = atomic_load(&s_dropped);
    diag->dns_queries = atomic_load(&s_dns_queries);
    return first;
}

void xiaotai_net_diag(xiaotai_net_diag_t *diag) { (void)net_diag_snapshot(diag); }

/* ----------------------------------------------------------------- the gate */

void net_gate_set_bringup(bool active)
{
    xiaotai_critical_enter(&net_gate_lock);
    net_gate.bringup = active;
    xiaotai_critical_exit(&net_gate_lock);
}

bool net_gate_bringup(void)
{
    xiaotai_critical_enter(&net_gate_lock);
    const bool active = net_gate.bringup;
    xiaotai_critical_exit(&net_gate_lock);
    return active;
}
