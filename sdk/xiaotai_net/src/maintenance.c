// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include "internal.h"
#include "xiaotai/framework.h"

#define RESTART_REPOST_US 100000

typedef enum {RESTART_NONE, RESTART_NEEDED, RESTART_POSTED, RESTART_QUIESCED, RESTART_ACCEPTED} restart_t;

static atomic_bool s_ready;
static atomic_int s_restart;
static bool s_restart_notice; /* worker only, until the promise event is queued */
/* Protected by net_gate_lock. The worker consumes it when it can hold a
 * configuration lease, so maintenance cannot start in the middle of a write. */
static bool s_history_ready;

static bool exclusive(bool maintenance_info)
{
    return (!maintenance_info && net_gate.maintenance_ticket) || net_gate.coprocessor_op ||
           net_gate.bringup || net_gate.restart_pending;
}

static void wake_worker(const net_message_t *message) { (void)message; }

static void wake_if_needed(bool pending)
{
    /* When the queue is full, the worker already has work queued. It checks
     * the retained flag after dispatch; the wake carries no state to lose. */
    if (pending) (void)net_post(wake_worker, 0, NULL, 0);
}

xiaotai_err_t net_gate_config_begin(void)
{
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_critical_enter(&net_gate_lock);
    xiaotai_err_t result = XIAOTAI_OK;
    if (exclusive(false)) result = XIAOTAI_ERR_INVALID_STATE;
    else if (net_gate.config_ops == UINT16_MAX) result = XIAOTAI_ERR_NO_MEM;
    else ++net_gate.config_ops;
    xiaotai_critical_exit(&net_gate_lock);
    return result;
}

void net_gate_config_end(void)
{
    if (!atomic_load(&s_ready)) return;
    xiaotai_critical_enter(&net_gate_lock);
    const bool wake = s_history_ready && net_gate.config_ops == UINT16_MAX;
    if (net_gate.config_ops) --net_gate.config_ops;
    xiaotai_critical_exit(&net_gate_lock);
    wake_if_needed(wake && !net_on_worker());
}

static xiaotai_err_t coprocessor_begin(bool write, bool info)
{
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_critical_enter(&net_gate_lock);
    const bool maintenance_info = info && net_state->config.allow_coprocessor_info_during_maintenance;
    const bool ready = (!net_state->coprocessor_power_cycle || info) &&
                       !exclusive(maintenance_info) && !net_gate.config_ops &&
                       (!write || (net_gate.radio_ready && !net_gate.scan_pending));
    if (ready) {
        net_gate.coprocessor_op = true;
        net_gate.coprocessor_write = write;
    }
    xiaotai_critical_exit(&net_gate_lock);
    return ready ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t net_gate_coprocessor_begin(void) { return coprocessor_begin(false, false); }
xiaotai_err_t net_gate_coprocessor_info_begin(void) { return coprocessor_begin(false, true); }
xiaotai_err_t net_gate_coprocessor_write_begin(void) { return coprocessor_begin(true, false); }

void net_gate_coprocessor_end(void)
{
    if (!atomic_load(&s_ready)) return;
    xiaotai_critical_enter(&net_gate_lock);
    net_gate.coprocessor_op = false;
    net_gate.coprocessor_write = false;
    const bool wake = s_history_ready;
    xiaotai_critical_exit(&net_gate_lock);
    wake_if_needed(wake && !net_on_worker());
}

xiaotai_err_t net_gate_scan_begin(bool *acquired)
{
    *acquired = false;
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_critical_enter(&net_gate_lock);
    const bool allowed = !net_gate.coprocessor_write && !net_gate.restart_pending;
    if (allowed && !net_gate.scan_pending) *acquired = net_gate.scan_pending = true;
    xiaotai_critical_exit(&net_gate_lock);
    return allowed ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

void net_gate_scan_end(void)
{
    if (!atomic_load(&s_ready)) return;
    xiaotai_critical_enter(&net_gate_lock);
    net_gate.scan_pending = false;
    xiaotai_critical_exit(&net_gate_lock);
}

bool net_gate_maintenance_held(void)
{
    if (!atomic_load(&s_ready)) return false;
    xiaotai_critical_enter(&net_gate_lock);
    const bool held = net_gate.maintenance_ticket != 0;
    xiaotai_critical_exit(&net_gate_lock);
    return held;
}

bool net_gate_restart_pending(void)
{
    if (!atomic_load(&s_ready)) return false;
    xiaotai_critical_enter(&net_gate_lock);
    const bool pending = net_gate.restart_pending;
    xiaotai_critical_exit(&net_gate_lock);
    return pending;
}

bool xiaotai_net_provisioning_held(void)
{
    if (!atomic_load(&s_ready)) return false;
    xiaotai_critical_enter(&net_gate_lock);
    const bool held = net_gate.maintenance_ticket || net_gate.restart_pending ||
                      net_provision_submission_pending() || net_gate.credential_retire_pending;
    xiaotai_critical_exit(&net_gate_lock);
    return held;
}

xiaotai_err_t xiaotai_net_maintenance_begin(uint32_t *ticket)
{
    if (!ticket) return XIAOTAI_ERR_INVALID_ARG;
    *ticket = 0;
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_critical_enter(&net_gate_lock);
    const bool ready = !exclusive(false) && !net_gate.config_ops;
    if (ready) {
        if (++net_gate.next_ticket == 0) ++net_gate.next_ticket;
        *ticket = net_gate.maintenance_ticket = net_gate.next_ticket;
    }
    xiaotai_critical_exit(&net_gate_lock);
    return ready ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

void xiaotai_net_maintenance_end(uint32_t ticket)
{
    if (!ticket || !atomic_load(&s_ready)) return;
    xiaotai_critical_enter(&net_gate_lock);
    const bool release = ticket == net_gate.maintenance_ticket && !net_gate.maintenance_restart;
    if (release) {
        net_gate.maintenance_ticket = 0;
        s_history_ready = true;
    }
    xiaotai_critical_exit(&net_gate_lock);
    wake_if_needed(release && !net_on_worker());
}

xiaotai_err_t xiaotai_net_maintenance_restart(uint32_t ticket)
{
    if (!ticket || !atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_critical_enter(&net_gate_lock);
    const bool holder = ticket == net_gate.maintenance_ticket;
    if (holder) {
        net_gate.maintenance_restart = true;
        net_gate.restart_pending = true;
    }
    xiaotai_critical_exit(&net_gate_lock);
    /* The holder already owns the framework quiesce and must hand it over. */
    return holder ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

void net_maintenance_defer_history(void)
{
    /* The worker checks this again after dispatch, even if a holder released
     * between the refused lease and marking the history pending. */
    xiaotai_critical_enter(&net_gate_lock);
    s_history_ready = true;
    xiaotai_critical_exit(&net_gate_lock);
}

void net_maintenance_work(void)
{
    xiaotai_critical_enter(&net_gate_lock);
    const bool flush = s_history_ready && !exclusive(false) && net_gate.config_ops < UINT16_MAX;
    if (flush) {
        s_history_ready = false;
        ++net_gate.config_ops;
    }
    xiaotai_critical_exit(&net_gate_lock);
    if (flush) {
        net_units_maintenance_released();
        net_gate_config_end();
    }
}

static void owner_restart(void *ctx)
{
    (void)ctx;
    if (atomic_load(&s_restart) != RESTART_POSTED) return;
    const xiaotai_err_t err = xiaotai_restart_request("network", 0);
    atomic_store(&s_restart, err == XIAOTAI_OK ? RESTART_ACCEPTED :
                             err == XIAOTAI_ERR_BUSY ? RESTART_QUIESCED : RESTART_NEEDED);
}

static bool publish_restart_notice(void)
{
    if (!s_restart_notice) return true;
    if (net_provision_publish() != XIAOTAI_OK) return false;
    s_restart_notice = false;
    return true;
}

static void post_restart(void)
{
    int state = atomic_load(&s_restart);
    if (state == RESTART_NONE || state == RESTART_ACCEPTED) return;
    /* Exclusive provisioning holds the quiesce until it receives this
     * event and can hand its ticket to the framework. A bare request
     * cannot take that ticket, so the notice must get through first. */
    if (publish_restart_notice() && (state == RESTART_NEEDED ||
        (state == RESTART_QUIESCED && (!xiaotai_quiesced() || xiaotai_restart_pending())))) {
        if (atomic_compare_exchange_strong(&s_restart, &state, RESTART_POSTED) &&
            xiaotai_run(owner_restart, NULL) != XIAOTAI_OK) {
            atomic_store(&s_restart, RESTART_NEEDED);
        }
    }
    /* The owner queue can reject the notice or job and drop a subsequent
     * quiesce-release. Retain one deadline until the framework owns it. */
    if (atomic_load(&s_restart) != RESTART_ACCEPTED) {
        net_deadline_set(NET_DEADLINE_MAINTENANCE, xiaotai_time_us() + RESTART_REPOST_US, post_restart);
    }
}

bool net_restart_prepare(void)
{
    xiaotai_critical_enter(&net_gate_lock);
    const bool first = !net_gate.restart_pending;
    if (first) {
        net_gate.restart_pending = true;
        atomic_store(&s_restart, RESTART_NEEDED);
    }
    xiaotai_critical_exit(&net_gate_lock);
    return first;
}

void net_restart_publish(void)
{
    s_restart_notice = true;
    post_restart();
}

void net_restart_required(void)
{
    if (!net_restart_prepare()) return;
    xiaotai_net_provision_status_t *status = net_provision_edit();
    status->restart_required = true;
    net_provision_commit(false);
    net_restart_publish();
}

static void on_quiesce(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    int expected = RESTART_QUIESCED;
    if (!event->args.value && atomic_compare_exchange_strong(&s_restart, &expected, RESTART_POSTED)) {
        owner_restart(NULL);
    }
}

static xiaotai_err_t init(void)
{
    const xiaotai_err_t err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_QUIESCE, on_quiesce, NULL);
    if (err == XIAOTAI_OK) atomic_store(&s_ready, true);
    return err;
}

static void reset(void)
{
    atomic_store(&s_ready, false);
    atomic_store(&s_restart, RESTART_NONE);
    s_restart_notice = false;
    s_history_ready = false;
}

const net_unit_t net_maintenance_unit = {.name = "maintenance", .init = init, .reset = reset};
