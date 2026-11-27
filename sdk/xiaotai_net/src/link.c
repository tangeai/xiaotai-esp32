// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/sys.h"

/* A driver event can be late or dropped. Retry observing the same leave, never
 * issue a new join merely because a callback deadline elapsed. */
#define LEAVE_WINDOW_MS 500U
#define LEAVE_CALLS_MAX 4U
#define LEAVE_TOTAL_MS (LEAVE_WINDOW_MS * LEAVE_CALLS_MAX)
#define SIGNAL_CHANGE_DB 3

static void attempt_due(void);
static void leave_due(void);
static void address_due(void);
static void sample_signal(void);
static void read_mac_again(void);

static int64_t after_ms(uint32_t ms)
{
    const int64_t due = xiaotai_time_us() + (int64_t)ms * 1000;
    return due ? due : 1;
}

static uint32_t elapsed_ms(int64_t since)
{
    const int64_t ms = (xiaotai_time_us() - since) / 1000;
    return ms <= 0 ? 0U : ms >= UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static bool needs_credentials(void)
{
    return (net_state->driver->capabilities & XIAOTAI_NET_DRIVER_CREDENTIALS) != 0;
}

static bool can_join(void) { return net_state->has_credentials || !needs_credentials(); }

static void set_radio_ready(bool ready)
{
    xiaotai_critical_enter(&net_gate_lock);
    net_state->link.radio_ready = net_gate.radio_ready = ready;
    if (!ready) net_gate.credential_retire_pending = false;
    xiaotai_critical_exit(&net_gate_lock);
}

static void set_candidate_retiring(bool pending)
{
    xiaotai_critical_enter(&net_gate_lock);
    net_gate.credential_retire_pending = pending;
    xiaotai_critical_exit(&net_gate_lock);
}

static void cancel_join_wait(void)
{
    net_state->link.deferred_join = false;
    net_deadline_clear(NET_DEADLINE_BACKOFF);
}

static void finish_control(xiaotai_err_t result)
{
    if (!net_state->link.control_pending) return;
    net_state->link.control_pending = false;
    net_link_control_finish(result);
}

static void offline(xiaotai_net_state_t next, uint32_t retry_ms, bool user)
{
    net_deadline_clear(NET_DEADLINE_ADDRESS);
    net_deadline_clear(NET_DEADLINE_SIGNAL);
    net_deadline_clear(NET_DEADLINE_MAC);
    xiaotai_net_status_t *s = net_status_edit();
    const bool state_changed = s->state != next;
    const bool down = s->generation != 0;
    const bool changed = state_changed || down || s->retry_in_ms != retry_ms || (user && !s->user_disconnected);
    if (!changed) {
        net_snapshot_cancel();
        return;
    }
    s->state = next;
    s->retry_in_ms = retry_ms;
    if (user) s->user_disconnected = true;
    net_generation_clear();
    s->generation = 0;
    s->ipv4 = 0;
    s->gateway_ipv4 = 0;
    s->channel = 0;
    s->ipv6_valid = false;
    memset(s->ipv6, 0, sizeof(s->ipv6));
    s->signal_valid = false;
    s->signal_dbm = 0;
    net_status_commit_link(state_changed, down ? NET_EMIT_DOWN : NET_EMIT_NONE);
}

static void reset_retry_pacing(net_link_t *link)
{
    link->retry_base_ms = 0;
    link->retry_outage_failures = 0;
    link->retry_credentials = false;
}

static void clear_failures(void)
{
    reset_retry_pacing(&net_state->link);
    xiaotai_net_status_t *s = net_status_edit();
    const bool changed = s->failures || s->failing;
    s->failures = 0;
    s->failing = false;
    if (changed) net_status_commit(NET_EMIT_NONE);
    else net_snapshot_cancel();
}

static void count_stale(void)
{
    xiaotai_net_diag_t *d = net_diag_edit();
    if (d->stale_events != UINT32_MAX) ++d->stale_events;
    net_diag_done();
}

/* Why the link was lost or an attempt failed, with what to check. */
static void log_loss(xiaotai_net_loss_t loss, int32_t code, bool had_address)
{
    static const char *const why[] = {
        [XIAOTAI_NET_LOSS_UNKNOWN] = "reason unknown",
        [XIAOTAI_NET_LOSS_NOT_FOUND] = "network not found: check its name and that the access point is in range",
        [XIAOTAI_NET_LOSS_CREDENTIALS] = "authentication failed: check the password",
        [XIAOTAI_NET_LOSS_SIGNAL] = "the access point is no longer heard",
        [XIAOTAI_NET_LOSS_REFUSED] = "the driver refused the attempt",
        [XIAOTAI_NET_LOSS_ADDRESS] = "joined but got no address: check the router's DHCP",
        [XIAOTAI_NET_LOSS_SECURITY] = "the network's security is not accepted",
    };
    if (loss == XIAOTAI_NET_LOSS_REQUESTED) {
        XIAOTAI_LOGD("net", "left the network on request");
        return;
    }
    const char *text = (size_t)loss < sizeof(why) / sizeof(why[0]) && why[loss] ? why[loss] : why[0];
    if (loss == XIAOTAI_NET_LOSS_ADDRESS && had_address) text = "the address was lost";
    XIAOTAI_LOGW("net", "link lost: %s (code %" PRId32 ")", text, code);
}

static void record_loss(xiaotai_net_loss_t loss, int32_t code, bool signal_valid, int8_t dbm)
{
    log_loss(loss, code, xiaotai_net_generation() != 0U);
    const xiaotai_net_loss_record_t received = {
        .loss = loss, .code = code, .signal_valid = signal_valid,
        .signal_dbm = signal_valid ? dbm : 0,
        .uptime_s = (uint32_t)(xiaotai_time_us() / 1000000),
    };
    xiaotai_net_diag_t *d = net_diag_edit();
    if (d->losses != UINT32_MAX) ++d->losses;
    if (loss == XIAOTAI_NET_LOSS_SIGNAL && d->signal_losses != UINT32_MAX) ++d->signal_losses;
    if (!net_state->link.first_loss_present) {
        net_state->link.first_loss_present = true;
        d->first = received;
    }
    if (d->recent_count == XIAOTAI_NET_LOSS_HISTORY) {
        memmove(&d->recent[0], &d->recent[1], (XIAOTAI_NET_LOSS_HISTORY - 1U) * sizeof(d->recent[0]));
        --d->recent_count;
    }
    d->recent[d->recent_count++] = received;
    net_diag_done();
}

static bool read_mac(uint8_t mac[6])
{
    const xiaotai_net_driver_t *driver = net_state->driver;
    return driver->mac && driver->mac(driver->self, mac) == XIAOTAI_OK;
}

static void read_mac_again(void)
{
    uint8_t mac[6];
    if (net_state_now() != XIAOTAI_NET_UP) return;
    if (!read_mac(mac)) {
        net_deadline_set(NET_DEADLINE_MAC, after_ms(XIAOTAI_NET_MAC_RETRY_MS), read_mac_again);
        return;
    }
    xiaotai_net_status_t *s = net_status_edit();
    s->mac_valid = true;
    memcpy(s->mac, mac, sizeof(s->mac));
    net_status_commit(NET_EMIT_UP);
}

static void sample_signal(void)
{
    if (net_state_now() != XIAOTAI_NET_UP) return;
    const xiaotai_net_driver_t *driver = net_state->driver;
    if (!(driver->capabilities & XIAOTAI_NET_DRIVER_SIGNAL)) return;
    int8_t dbm = 0;
    const bool valid = driver->signal(driver->self, &dbm) == XIAOTAI_OK;
    if (!valid) dbm = 0;
    xiaotai_net_status_t *s = net_status_edit();
    const int diff = dbm - s->signal_dbm;
    const bool notify = valid != s->signal_valid || diff >= SIGNAL_CHANGE_DB || diff <= -SIGNAL_CHANGE_DB;
    if (diff || valid != s->signal_valid) {
        s->signal_dbm = dbm;
        s->signal_valid = valid;
        net_status_commit(notify ? NET_EMIT_SIGNAL : NET_EMIT_NONE);
    } else {
        net_snapshot_cancel();
    }
    net_deadline_set(NET_DEADLINE_SIGNAL, after_ms(XIAOTAI_NET_SIGNAL_INTERVAL_MS), sample_signal);
}

static void publish_address(const xiaotai_net_driver_event_t *event)
{
    net_link_t *link = &net_state->link;
    xiaotai_net_status_t old;
    xiaotai_net_status(&old);
    const bool family6_same = event->ipv6_valid && old.ipv6_valid && !memcmp(event->ipv6, old.ipv6, 16);
    const bool family4_same = event->ipv4 && event->ipv4 == old.ipv4;
    const bool first = old.state != XIAOTAI_NET_UP;
    const bool new_generation = first || (!family4_same && !family6_same);
    const uint32_t gateway = event->ipv4 ? event->gateway_ipv4 : 0;
    if (!new_generation && event->ipv4 == old.ipv4 && event->ipv6_valid == old.ipv6_valid &&
        (!event->ipv6_valid || family6_same) && old.gateway_ipv4 == gateway && old.channel == link->channel) return;

    uint8_t mac[6];
    const bool got_mac = first && !old.mac_valid && read_mac(mac);
    reset_retry_pacing(link);
    net_deadline_clear(NET_DEADLINE_ADDRESS);
    xiaotai_net_status_t *s = net_status_edit();
    s->state = XIAOTAI_NET_UP;
    if (new_generation) s->generation = net_generation_issue();
    s->ipv4 = event->ipv4;
    s->gateway_ipv4 = gateway;
    s->channel = link->channel;
    s->ipv6_valid = event->ipv6_valid;
    if (event->ipv6_valid) memcpy(s->ipv6, event->ipv6, sizeof(s->ipv6));
    else memset(s->ipv6, 0, sizeof(s->ipv6));
    s->failures = 0;
    s->failing = false;
    s->user_disconnected = false;
    s->retry_in_ms = 0;
    if (got_mac) {
        s->mac_valid = true;
        memcpy(s->mac, mac, sizeof(s->mac));
    }
    net_status_commit_link(first, new_generation ? NET_EMIT_UP : NET_EMIT_NONE);

    if (new_generation) {
        xiaotai_net_diag_t *d = net_diag_edit();
        if (d->addresses != UINT32_MAX) ++d->addresses;
        d->address_ms = elapsed_ms(link->joined ? link->joined_us : link->started_us);
        d->backoff_ms = 0;
        memset(&d->first, 0, sizeof(d->first));
        link->first_loss_present = false;
        net_diag_done();
    }
    if (first) {
        if (link->attempt_credentials.ssid[0]) net_units_link_address(&link->attempt_credentials);
        net_units_link_provision(false, XIAOTAI_NET_PROVISION_REASON_NONE);
        if (!got_mac && !old.mac_valid && net_state->driver->mac) {
            net_deadline_set(NET_DEADLINE_MAC, after_ms(XIAOTAI_NET_MAC_RETRY_MS), read_mac_again);
        }
        sample_signal();
    }
}

static void notify_verification_loss(xiaotai_net_loss_t loss, int32_t code)
{
    const net_link_observer_t observer = net_state->link.observer;
    if (observer.lost) observer.lost(loss, code, observer.ctx);
}

/* The automatic retry budget counts address/association failures, not calls to
 * drain the same failed association. */
static bool schedule_failure(xiaotai_net_loss_t reason, int32_t code, bool signal_valid, int8_t dbm,
                             bool join_refused)
{
    const xiaotai_net_retry_policy_t *policy = &net_state->config.retry;
    xiaotai_net_status_t *s = net_status_edit();
    const uint32_t count = (uint32_t)s->failures + 1U;
    const bool give_up = net_backoff_give_up(policy, count);
    const bool notice = !s->failing && (count >= XIAOTAI_NET_FAILURES_NOTICE || give_up);
    if (notice) s->failing = true;
    if (s->failures != UINT16_MAX) ++s->failures;
    net_status_commit(NET_EMIT_NONE);
    record_loss(reason, code, signal_valid, dbm);
    net_link_t *link = &net_state->link;
    uint32_t retry_count = count;
    xiaotai_net_loss_t retry_reason = reason;
    if (policy->grow_from_previous_base) {
        if (join_refused) {
            retry_count = (uint32_t)policy->fast_retries + 1U;
            retry_reason = link->retry_credentials ? XIAOTAI_NET_LOSS_CREDENTIALS : XIAOTAI_NET_LOSS_UNKNOWN;
        } else {
            if (link->retry_outage_failures != UINT16_MAX) ++link->retry_outage_failures;
            link->retry_credentials = reason == XIAOTAI_NET_LOSS_CREDENTIALS || reason == XIAOTAI_NET_LOSS_SECURITY;
            retry_count = link->retry_outage_failures;
        }
    }
    const uint32_t wait = give_up ? 0U :
        net_backoff_wait_ms(policy, retry_count, retry_reason, xiaotai_random(), &link->retry_base_ms);
    xiaotai_net_diag_t *d = net_diag_edit();
    d->backoff_ms = wait;
    net_diag_done();
    offline(give_up ? XIAOTAI_NET_HELD : XIAOTAI_NET_WAITING, wait, false);
    if (notice) net_units_link_provision(true, XIAOTAI_NET_PROVISION_REASON_FAILING);
    return !give_up;
}

static uint32_t current_wait(void)
{
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    return status.retry_in_ms;
}

static void retry_when_ready(void)
{
    if (net_state->link.radio_ready && !net_state->link.retry_waits_for_radio) {
        net_deadline_set(NET_DEADLINE_BACKOFF, after_ms(current_wait()), attempt_due);
    }
}

static void clear_attempt(void)
{
    net_link_t *link = &net_state->link;
    set_candidate_retiring(false);
    link->token = 0;
    link->joined = false;
    link->channel = 0;
    link->leave_calls = 0;
    link->drain_until_us = 0;
    link->leave_error = XIAOTAI_OK;
    link->drain_exhausted = false;
    memset(&link->attempt_credentials, 0, sizeof(link->attempt_credentials));
    memset(&link->verified_address, 0, sizeof(link->verified_address));
    net_deadline_clear(NET_DEADLINE_LEAVE);
    net_deadline_clear(NET_DEADLINE_ADDRESS);
}

static void drain_done(void)
{
    net_link_t *link = &net_state->link;
    const net_drain_t action = link->drain;
    const xiaotai_net_loss_t reason = link->verification_loss;
    const int32_t code = link->verification_code;
    clear_attempt();
    link->drain = NET_DRAIN_NONE;
    switch (action) {
    case NET_DRAIN_RETRY:
        retry_when_ready();
        finish_control(XIAOTAI_OK);
        break;
    case NET_DRAIN_CONNECT:
    case NET_DRAIN_VERIFY:
        attempt_due();
        break;
    case NET_DRAIN_VERIFY_FAILED:
        notify_verification_loss(reason, code);
        break;
    case NET_DRAIN_HOLD:
    case NET_DRAIN_NONE:
        finish_control(XIAOTAI_OK);
        break;
    }
}

static void leave_due(void)
{
    net_link_t *link = &net_state->link;
    if (!link->token || link->drain == NET_DRAIN_NONE || link->drain_exhausted || !link->radio_ready) return;
    if (!link->drain_until_us) link->drain_until_us = after_ms(LEAVE_TOTAL_MS);
    if (link->leave_calls == LEAVE_CALLS_MAX || xiaotai_time_us() >= link->drain_until_us) {
        const xiaotai_err_t error = link->leave_error == XIAOTAI_OK ? XIAOTAI_ERR_TIMEOUT : link->leave_error;
        const bool verification = link->verifying;
        set_candidate_retiring(false);
        link->drain_exhausted = true;
        link->drain = NET_DRAIN_HOLD;
        xiaotai_net_status_t *s = net_status_edit();
        s->failing = true;
        net_status_commit(NET_EMIT_NONE);
        offline(XIAOTAI_NET_HELD, 0, false);
        XIAOTAI_LOGW("net", "association not released after %u leave calls (error=0x%x)",
                     link->leave_calls, (unsigned)error);
        finish_control(error);
        if (verification) notify_verification_loss(XIAOTAI_NET_LOSS_ADDRESS, error);
        return;
    }
    ++link->leave_calls;
    const xiaotai_net_driver_t *driver = net_state->driver;
    const xiaotai_err_t result = driver->leave(driver->self);
    if (result == XIAOTAI_ERR_INVALID_STATE) {
        /* The driver contract reserves this result for no outstanding link. */
        drain_done();
        return;
    }
    if (result != XIAOTAI_OK) link->leave_error = result;
    const int64_t next = after_ms(LEAVE_WINDOW_MS);
    net_deadline_set(NET_DEADLINE_LEAVE, next < link->drain_until_us ? next : link->drain_until_us, leave_due);
}

static void request_drain(net_drain_t action)
{
    net_link_t *link = &net_state->link;
    link->drain = action;
    if (!link->token) {
        drain_done();
    } else if (link->drain_exhausted && action != NET_DRAIN_HOLD) {
        link->drain_exhausted = false;
        link->leave_calls = 0;
        link->drain_until_us = 0;
        link->leave_error = XIAOTAI_OK;
        leave_due();
    } else if (!net_deadline_due(NET_DEADLINE_LEAVE) && !link->drain_exhausted) {
        leave_due();
    } else if (link->drain_exhausted) {
        finish_control(link->leave_error == XIAOTAI_OK ? XIAOTAI_ERR_TIMEOUT : link->leave_error);
    }
}

static void lose_address(xiaotai_net_loss_t loss, int32_t code)
{
    net_link_t *link = &net_state->link;
    if (link->drain != NET_DRAIN_NONE) return;
    if (link->verifying) {
        offline(XIAOTAI_NET_CONNECTING, 0, false);
        link->verification_loss = loss;
        link->verification_code = code;
        request_drain(NET_DRAIN_VERIFY_FAILED);
        return;
    }
    const bool again = schedule_failure(loss, code, false, 0, false);
    request_drain(again ? NET_DRAIN_RETRY : NET_DRAIN_HOLD);
}

static void address_due(void) { lose_address(XIAOTAI_NET_LOSS_ADDRESS, 0); }

static void attempt_due(void)
{
    net_link_t *link = &net_state->link;
    if (!link->radio_ready || link->retry_waits_for_radio || link->token || link->drain != NET_DRAIN_NONE) {
        finish_control(XIAOTAI_OK);
        return;
    }
    if (!link->verifying && !can_join()) {
        xiaotai_net_status_t *s = net_status_edit();
        const bool changed = s->ssid[0] != '\0';
        memset(s->ssid, 0, sizeof(s->ssid));
        if (changed) net_status_commit(NET_EMIT_NONE);
        else net_snapshot_cancel();
        offline(XIAOTAI_NET_UNCONFIGURED, 0, false);
        net_units_link_provision(true, XIAOTAI_NET_PROVISION_REASON_UNCONFIGURED);
        finish_control(XIAOTAI_OK);
        return;
    }
    if (net_scan_running()) {
        link->deferred_join = true;
        offline(XIAOTAI_NET_WAITING, 0, false);
        finish_control(XIAOTAI_OK);
        return;
    }

    (void)net_events_lost_take();
    link->deferred_join = false;
    const xiaotai_net_credentials_t *creds = link->verifying ? &link->candidate
        : needs_credentials() ? &net_state->credentials : NULL;
    const xiaotai_net_driver_t *driver = net_state->driver;
    link->token = net_token_next();
    link->started_us = xiaotai_time_us();
    link->joined = false;
    if (creds) link->attempt_credentials = *creds;
    xiaotai_net_status_t *s = net_status_edit();
    const bool changed = s->state != XIAOTAI_NET_CONNECTING;
    s->state = XIAOTAI_NET_CONNECTING;
    s->retry_in_ms = 0;
    memset(s->ssid, 0, sizeof(s->ssid));
    if (creds) memcpy(s->ssid, creds->ssid, sizeof(s->ssid));
    net_status_commit_link(changed, NET_EMIT_NONE);

    xiaotai_net_diag_t *d = net_diag_edit();
    if (d->attempts != UINT32_MAX) ++d->attempts;
    d->join_ms = 0;
    d->address_ms = 0;
    net_diag_done();
    const uint32_t flags = net_state->config.accept_weak_security ? 0U : XIAOTAI_NET_JOIN_REQUIRE_WPA2;
    const xiaotai_err_t result = driver->join(driver->self, link->token, creds, flags);
    if (result != XIAOTAI_OK) {
        clear_attempt();
        finish_control(result);
        if (link->verifying) {
            notify_verification_loss(XIAOTAI_NET_LOSS_REFUSED, result);
        } else if (net_state->config.retry.join_immediately_on_radio_start &&
                   driver->join_radio_unavailable && driver->join_radio_unavailable(result)) {
            /* The driver may not have delivered a radio edge yet. Do not
             * turn this into a periodic retry or change the observed state. */
            link->retry_waits_for_radio = true;
            record_loss(XIAOTAI_NET_LOSS_REFUSED, result, false, 0);
            xiaotai_net_diag_t *d = net_diag_edit();
            d->backoff_ms = 0;
            net_diag_done();
            offline(XIAOTAI_NET_WAITING, 0, false);
        } else if (schedule_failure(XIAOTAI_NET_LOSS_REFUSED, result, false, 0, true)) {
            retry_when_ready();
        }
        return;
    }
    /* Bound the silent-driver case as well as DHCP after association. */
    if (XIAOTAI_NET_ADDRESS_TIMEOUT_MS) {
        net_deadline_set(NET_DEADLINE_ADDRESS, after_ms(XIAOTAI_NET_ADDRESS_TIMEOUT_MS), address_due);
    }
    finish_control(XIAOTAI_OK);
}

void net_link_connect(void)
{
    if (!net_state->driver_running) return;
    cancel_join_wait();
    net_state->link.retry_waits_for_radio = false;
    if (net_state_now() == XIAOTAI_NET_HELD) clear_failures();
    if (!net_state->link.radio_ready) {
        offline(XIAOTAI_NET_WAITING, 0, false);
        if (net_state->link.token) net_state->link.drain = NET_DRAIN_CONNECT;
        finish_control(XIAOTAI_OK);
        return;
    }
    if (net_state->link.token) offline(XIAOTAI_NET_WAITING, 0, false);
    request_drain(NET_DRAIN_CONNECT);
}

static void forget_verification(void)
{
    net_link_t *link = &net_state->link;
    link->verifying = false;
    memset(&link->observer, 0, sizeof(link->observer));
    memset(&link->candidate, 0, sizeof(link->candidate));
    memset(&link->verified_address, 0, sizeof(link->verified_address));
}

static void hold(bool user, xiaotai_net_provision_reason_t reason)
{
    if (!net_state->driver_running) {
        finish_control(XIAOTAI_ERR_INVALID_STATE);
        return;
    }
    net_link_t *link = &net_state->link;
    const bool leaving_live = user && link->token && link->radio_ready && link->drain == NET_DRAIN_NONE;
    xiaotai_err_t leave = XIAOTAI_ERR_INVALID_STATE;
    int64_t drain_until_us = 0;
    if (leaving_live) {
        const xiaotai_net_driver_t *driver = net_state->driver;
        drain_until_us = after_ms(LEAVE_TOTAL_MS);
        leave = driver->leave(driver->self);
        if (leave != XIAOTAI_OK && leave != XIAOTAI_ERR_INVALID_STATE) {
            /* A refused manual command did not invalidate a usable address.
             * A real address loss still goes through the driver event path. */
            finish_control(leave);
            return;
        }
    }
    cancel_join_wait();
    forget_verification();
    offline(XIAOTAI_NET_HELD, 0, user);
    if (user) net_units_link_provision(true, reason);
    if (leaving_live && leave == XIAOTAI_OK) {
        link->drain = NET_DRAIN_HOLD;
        link->leave_calls = 1U;
        link->drain_until_us = drain_until_us;
        const int64_t next = after_ms(LEAVE_WINDOW_MS);
        net_deadline_set(NET_DEADLINE_LEAVE, next < drain_until_us ? next : drain_until_us, leave_due);
    } else {
        if (leaving_live) clear_attempt();
        request_drain(NET_DRAIN_HOLD);
    }
    if (!link->radio_ready) finish_control(XIAOTAI_OK);
}

void net_link_hold(bool user) { hold(user, XIAOTAI_NET_PROVISION_REASON_DISCONNECTED); }

void net_link_coprocessor_power_cycle(void)
{
    /* The part already reported the transport cannot continue. Retire its
     * address even if a command to the broken radio would fail. */
    xiaotai_critical_enter(&net_gate_lock);
    net_state->coprocessor_power_cycle = true;
    xiaotai_critical_exit(&net_gate_lock);
    set_radio_ready(false);
    cancel_join_wait();
    clear_attempt();
    offline(XIAOTAI_NET_BLOCKED, 0, false);
}

void net_link_resume(void)
{
    const xiaotai_net_state_t state = net_state_now();
    if (state == XIAOTAI_NET_BLOCKED) {
        net_bringup(true);
        xiaotai_net_status_t status;
        xiaotai_net_status(&status);
        finish_control(status.state == XIAOTAI_NET_BLOCKED ? status.prepare.error : XIAOTAI_OK);
    } else if (state == XIAOTAI_NET_WAITING || state == XIAOTAI_NET_HELD || state == XIAOTAI_NET_UNCONFIGURED) {
        clear_failures();
        net_link_connect();
    } else {
        finish_control(state == XIAOTAI_NET_STOPPED ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK);
    }
}

static void control_job(const net_message_t *message)
{
    net_state->link.control_pending = true;
    if (message->u.job.arg) net_link_hold(true);
    else net_link_resume();
}

xiaotai_err_t xiaotai_net_retry(void) { return net_link_control_enqueue(control_job, 0, true); }
xiaotai_err_t xiaotai_net_disconnect(void) { return net_link_control_enqueue(control_job, 1, false); }

bool net_link_attempting(void) { return net_state->link.token != 0; }

void net_link_scan_finished(void)
{
    if (net_state->link.deferred_join) attempt_due();
}

xiaotai_err_t net_link_try(const xiaotai_net_credentials_t *credentials, const net_link_observer_t *observer)
{
    net_link_t *link = &net_state->link;
    if (!credentials || !observer || !credentials->ssid[0] ||
        !memchr(credentials->ssid, '\0', sizeof(credentials->ssid)) ||
        !memchr(credentials->password, '\0', sizeof(credentials->password))) return XIAOTAI_ERR_INVALID_ARG;
    if (!net_state->driver_running || !link->radio_ready || !needs_credentials()) return XIAOTAI_ERR_INVALID_STATE;
    if (net_scan_running() || (link->verifying && (link->token || link->drain != NET_DRAIN_NONE))) return XIAOTAI_ERR_BUSY;
    cancel_join_wait();
    link->retry_waits_for_radio = false;
    link->verifying = true;
    link->candidate = *credentials;
    link->observer = *observer;
    offline(XIAOTAI_NET_CONNECTING, 0, false);
    request_drain(NET_DRAIN_VERIFY);
    return XIAOTAI_OK;
}

void net_link_try_end(bool keep)
{
    net_link_t *link = &net_state->link;
    if (!link->verifying) return;
    const xiaotai_net_driver_event_t address = link->verified_address;
    const bool ready = link->token && link->drain == NET_DRAIN_NONE && (address.ipv4 || address.ipv6_valid);
    if (keep && ready) {
        net_state->credentials = link->candidate;
        net_state->has_credentials = true;
        forget_verification();
        publish_address(&address);
    } else {
        forget_verification();
        net_link_connect();
    }
}

void net_link_mark_candidate_retiring(void)
{
    const net_link_t *link = &net_state->link;
    set_candidate_retiring(link->verifying && link->token && link->radio_ready && !link->drain_exhausted);
}

void net_link_try_discard(void)
{
    if (net_state->link.verifying) net_link_hold(false);
}

void net_link_control_provision(bool open)
{
    net_state->link.control_pending = true;
    if (open) hold(true, XIAOTAI_NET_PROVISION_REASON_REQUESTED);
    else net_link_resume();
}

static void link_joined(const xiaotai_net_driver_event_t *event)
{
    net_link_t *link = &net_state->link;
    if (link->joined) return;
    link->joined = true;
    link->channel = event->channel;
    link->joined_us = xiaotai_time_us();
    xiaotai_net_diag_t *d = net_diag_edit();
    d->join_ms = elapsed_ms(link->started_us);
    net_diag_done();
    if (XIAOTAI_NET_ADDRESS_TIMEOUT_MS && net_state_now() != XIAOTAI_NET_UP &&
        !link->verified_address.ipv4 && !link->verified_address.ipv6_valid) {
        net_deadline_set(NET_DEADLINE_ADDRESS, after_ms(XIAOTAI_NET_ADDRESS_TIMEOUT_MS), address_due);
    }
    if (link->verifying) {
        if (strcmp(event->ssid, link->candidate.ssid) != 0) {
            lose_address(XIAOTAI_NET_LOSS_REFUSED, XIAOTAI_ERR_NOT_FOUND);
        } else if (link->observer.joined) {
            link->observer.joined(event->ssid, link->observer.ctx);
        }
    }
}

static void link_address(const xiaotai_net_driver_event_t *event)
{
    net_link_t *link = &net_state->link;
    if (!event->ipv4 && !event->ipv6_valid) {
        lose_address(XIAOTAI_NET_LOSS_ADDRESS, event->code);
    } else if (!link->verifying) {
        publish_address(event);
    } else if (link->joined) {
        const bool first = !link->verified_address.ipv4 && !link->verified_address.ipv6_valid;
        net_deadline_clear(NET_DEADLINE_ADDRESS);
        link->verified_address = *event;
        if (first && link->observer.address) link->observer.address(link->observer.ctx);
    }
}

static void driver_event(const xiaotai_net_driver_event_t *event)
{
    if (net_state->coprocessor_power_cycle) return;
    net_link_t *link = &net_state->link;
    if (event->kind == XIAOTAI_NET_DRIVER_STOPPED) {
        if (!link->radio_ready) return;
        set_radio_ready(false);
        cancel_join_wait();
        net_deadline_clear(NET_DEADLINE_LEAVE);
        link->drain_until_us = 0;
        link->leave_calls = 0;
        const bool immediate = net_state->config.retry.join_immediately_on_radio_start;
        if (link->token) {
            if (immediate && !link->verifying && link->drain == NET_DRAIN_NONE) {
                offline(XIAOTAI_NET_WAITING, 0, false);
                request_drain(NET_DRAIN_RETRY);
            } else {
                lose_address(XIAOTAI_NET_LOSS_ADDRESS, 0);
                offline(net_state_now() == XIAOTAI_NET_HELD ? XIAOTAI_NET_HELD : XIAOTAI_NET_WAITING,
                        current_wait(), false);
            }
        }
        if (immediate && net_state_now() == XIAOTAI_NET_WAITING) {
            xiaotai_net_diag_t *d = net_diag_edit();
            d->backoff_ms = 0;
            net_diag_done();
            offline(XIAOTAI_NET_WAITING, 0, false);
        }
        finish_control(XIAOTAI_OK);
        return;
    }
    if (event->kind == XIAOTAI_NET_DRIVER_STARTED) {
        if (!link->radio_ready || link->retry_waits_for_radio) {
            link->retry_waits_for_radio = false;
            set_radio_ready(net_state->driver_running);
            if (link->token) leave_due();
            else if (net_state_now() == XIAOTAI_NET_WAITING) retry_when_ready();
        }
        return;
    }
    if (event->kind == XIAOTAI_NET_DRIVER_SCAN_DONE) return;
    if (!event->token || event->token != link->token ||
        (link->drain != NET_DRAIN_NONE && event->kind != XIAOTAI_NET_DRIVER_LOST)) {
        count_stale();
        return;
    }
    switch (event->kind) {
    case XIAOTAI_NET_DRIVER_JOINED:
        link_joined(event);
        break;
    case XIAOTAI_NET_DRIVER_ADDRESS:
        link_address(event);
        break;
    case XIAOTAI_NET_DRIVER_LOST:
        if (link->drain != NET_DRAIN_NONE) {
            drain_done();
        } else {
            clear_attempt();
            if (link->verifying) notify_verification_loss(event->loss, event->code);
            else if (schedule_failure(event->loss, event->code, event->signal_valid, event->signal_dbm, false))
                retry_when_ready();
        }
        break;
    default:
        break;
    }
}

void net_link_queue_recover(void)
{
    net_link_t *link = &net_state->link;
    if (link->token && net_events_lost_take()) lose_address(XIAOTAI_NET_LOSS_UNKNOWN, XIAOTAI_ERR_TIMEOUT);
}

static void driver_started(void)
{
    set_radio_ready(true);
    if (!net_state->provisioning_boot) net_link_connect();
}

const net_unit_t net_link_unit = {
    .name = "link", .driver_started = driver_started, .driver_event = driver_event,
};
