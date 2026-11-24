// SPDX-License-Identifier: MIT
#include <string.h>

#include "store.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"

#define SCAN_HISTORY_MAX 5U
#define CANCEL_ATTEMPTS_MAX 4U
#define CANCEL_RECHECK_US 500000

/* Worker-only. A timed-out scan may still own the radio after a failed cancel. */
static uint32_t s_token;
static int64_t s_started_us;
static unsigned s_cancel_calls;
static bool s_started, s_discard;

static void state(xiaotai_net_scan_state_t phase, xiaotai_err_t error)
{
    xiaotai_net_scan_status_t *status = net_scan_edit();
    if (status->state == phase && status->error == error) {
        net_snapshot_cancel();
        return;
    }
    status->state = phase;
    status->error = error;
    net_scan_commit();
}

bool net_scan_running(void) { return s_token != 0U; }

static void finish(void)
{
    s_token = 0;
    s_discard = false;
    s_cancel_calls = 0;
    net_deadline_clear(NET_DEADLINE_SCAN);
    net_gate_scan_end();
    net_link_scan_finished();
}

static void cancel_due(void)
{
    if (!s_token || !s_discard) return;
    const xiaotai_net_driver_t *driver = net_state->driver;
    const xiaotai_err_t err = driver->scan_cancel(driver->self);
    if (err == XIAOTAI_OK) {
        finish();
        return;
    }
    if (++s_cancel_calls < CANCEL_ATTEMPTS_MAX) {
        net_deadline_set(NET_DEADLINE_SCAN, xiaotai_time_us() + CANCEL_RECHECK_US, cancel_due);
    } else {
        XIAOTAI_LOGW("net", "scan cancel failed; waiting for completion (error=0x%x)", (unsigned)err);
    }
}

static void timeout(void)
{
    if (!s_token) return;
    xiaotai_net_diag_t *diag = net_diag_edit();
    if (diag->scan_timeouts != UINT32_MAX) ++diag->scan_timeouts;
    net_diag_done();
    state(XIAOTAI_NET_SCAN_ERROR, XIAOTAI_ERR_TIMEOUT);
    s_discard = true;
    cancel_due();
}

static bool supported(xiaotai_net_security_t mode)
{
    return mode == XIAOTAI_NET_SECURITY_OPEN || mode == XIAOTAI_NET_SECURITY_WPA2 ||
           mode == XIAOTAI_NET_SECURITY_WPA_WPA2 || mode == XIAOTAI_NET_SECURITY_WPA3 ||
           mode == XIAOTAI_NET_SECURITY_WPA2_WPA3;
}

static bool same(const xiaotai_net_network_t *a, const xiaotai_net_network_t *b)
{
    return a->secure == b->secure && !strcmp(a->ssid, b->ssid);
}

static void strongest_first(xiaotai_net_scan_record_t *raw, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        const xiaotai_net_scan_record_t item = raw[i];
        size_t j = i;
        for (; j && raw[j - 1U].signal_dbm < item.signal_dbm; --j) raw[j] = raw[j - 1U];
        raw[j] = item;
    }
}

/* Publish the list and its revision together; never keep a saved password in
 * this snapshot. A new scan reuses the lazy allocation. */
static xiaotai_err_t publish_list(const xiaotai_net_scan_record_t *raw, size_t count, bool truncated,
                                  xiaotai_net_scan_state_t phase)
{
    size_t saved_count = 0;
    xiaotai_net_network_t saved[SCAN_HISTORY_MAX];
    (void)net_history_list(saved, SCAN_HISTORY_MAX, &saved_count); /* history_error is separate */
    if ((count || saved_count) && !net_state->networks) {
        const size_t capacity = XIAOTAI_NET_SCAN_MAX +
            (net_state->config.layout == XIAOTAI_NET_STORE_RECORD ? SCAN_HISTORY_MAX : 0U);
        xiaotai_net_network_t *list = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, capacity * sizeof(*list));
        if (!list) return XIAOTAI_ERR_NO_MEM;
        (void)net_scan_edit();
        net_state->networks = list;
        net_snapshot_cancel();
    }

    const char *own = net_state->provision.ap_ssid;
    xiaotai_net_scan_status_t *status = net_scan_edit();
    xiaotai_net_network_t *list = net_state->networks;
    size_t used = 0U;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_net_scan_record_t *item = &raw[i];
        if (!item->ssid[0] || !memchr(item->ssid, '\0', sizeof(item->ssid)) || !strcmp(own, item->ssid)) continue;
        const xiaotai_net_security_t mode = (unsigned)item->security <= XIAOTAI_NET_SECURITY_OTHER
            ? item->security : XIAOTAI_NET_SECURITY_OTHER;
        xiaotai_net_network_t seen = {
            .available = true, .signal_valid = true, .signal_dbm = item->signal_dbm,
            .security = mode, .secure = mode != XIAOTAI_NET_SECURITY_OPEN, .supported = supported(mode),
        };
        memcpy(seen.ssid, item->ssid, sizeof(seen.ssid));
        size_t j = 0;
        while (j < used && !same(&list[j], &seen)) ++j;
        if (j < used) continue;
        for (size_t k = 0; k < saved_count; ++k) {
            if (same(&saved[k], &seen)) { seen.saved = true; break; }
        }
        list[used++] = seen;
    }
    for (size_t i = 0; i < saved_count; ++i) {
        if (!strcmp(own, saved[i].ssid)) continue;
        size_t j = 0U;
        while (j < used && !same(&list[j], &saved[i])) ++j;
        if (j == used) list[used++] = saved[i];
    }
    net_state->network_count = used;
    status->state = phase;
    status->truncated = truncated;
    status->error = XIAOTAI_OK;
    net_scan_commit();
    return XIAOTAI_OK;
}

void net_scan_prepare(void)
{
    if (s_token || net_state->networks || net_state->config.layout != XIAOTAI_NET_STORE_RECORD) return;
    const xiaotai_err_t err = publish_list(NULL, 0, false, XIAOTAI_NET_SCAN_IDLE);
    if (err != XIAOTAI_OK) state(XIAOTAI_NET_SCAN_ERROR, err);
}

static xiaotai_err_t start_reserved(void)
{
    const xiaotai_net_driver_t *driver = net_state->driver;
    if (!driver || !(driver->capabilities & XIAOTAI_NET_DRIVER_SCAN)) {
        net_gate_scan_end();
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    if (!net_state->driver_running || !net_state->link.radio_ready) {
        state(XIAOTAI_NET_SCAN_ERROR, XIAOTAI_ERR_INVALID_STATE);
        net_gate_scan_end();
        return XIAOTAI_ERR_INVALID_STATE;
    }
    const int64_t now = xiaotai_time_us();
    if (s_started && now - s_started_us < (int64_t)XIAOTAI_NET_SCAN_INTERVAL_MS * 1000) {
        net_gate_scan_end();
        return XIAOTAI_OK;
    }
    const int64_t retry_at = net_deadline_due(NET_DEADLINE_BACKOFF);
    if (net_link_attempting() || (retry_at && retry_at <= now)) {
        state(XIAOTAI_NET_SCAN_BUSY, XIAOTAI_ERR_BUSY);
        net_gate_scan_end();
        return XIAOTAI_ERR_BUSY;
    }

    const uint32_t token = net_token_next();
    state(XIAOTAI_NET_SCAN_QUEUED, XIAOTAI_OK);
    s_started_us = now;
    s_started = true;
    s_token = token;
    const xiaotai_err_t err = driver->scan(driver->self, token);
    if (err != XIAOTAI_OK) {
        state(err == XIAOTAI_ERR_BUSY ? XIAOTAI_NET_SCAN_BUSY : XIAOTAI_NET_SCAN_ERROR, err);
        finish();
        return err;
    }
    state(XIAOTAI_NET_SCAN_RUNNING, XIAOTAI_OK);
    xiaotai_net_diag_t *diag = net_diag_edit();
    if (diag->scans != UINT32_MAX) ++diag->scans;
    net_diag_done();
    net_deadline_set(NET_DEADLINE_SCAN, xiaotai_time_us() + (int64_t)XIAOTAI_NET_SCAN_TIMEOUT_MS * 1000, timeout);
    return XIAOTAI_OK;
}

xiaotai_err_t net_scan_start(void)
{
    bool acquired;
    const xiaotai_err_t err = net_gate_scan_begin(&acquired);
    return acquired ? start_reserved() : err;
}

static void scan_job(const net_message_t *message)
{
    bool (*active)(uint32_t);
    memcpy(&active, message->u.job.data, sizeof(active));
    if (active && !active(message->u.job.arg)) {
        net_gate_scan_end();
        return;
    }
    (void)start_reserved();
}

xiaotai_err_t net_scan_request(uint32_t token, bool (*active)(uint32_t))
{
    if (active && !active(token)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_net_driver_t *driver = xiaotai_net_driver_bound();
    if (!driver) return XIAOTAI_ERR_INVALID_STATE;
    if (!(driver->capabilities & XIAOTAI_NET_DRIVER_SCAN)) return XIAOTAI_ERR_NOT_SUPPORTED;
    bool acquired;
    xiaotai_err_t err = net_gate_scan_begin(&acquired);
    if (acquired) {
        err = net_post(scan_job, token, &active, sizeof(active));
        if (err != XIAOTAI_OK) net_gate_scan_end();
    }
    return err;
}

xiaotai_err_t xiaotai_net_scan(void) { return net_scan_request(0, NULL); }

size_t xiaotai_net_networks(xiaotai_net_network_t *networks, size_t capacity, xiaotai_net_scan_status_t *status)
{
    if (status) memset(status, 0, sizeof(*status));
    if (!net_state) return 0;
    const xiaotai_net_scan_status_t *visible = net_scan_edit();
    if (status) *status = *visible;
    size_t copied = net_state->network_count < capacity ? net_state->network_count : capacity;
    if (!networks) copied = 0;
    if (copied) memcpy(networks, net_state->networks, copied * sizeof(*networks));
    net_snapshot_cancel();
    return copied;
}

static void drop_snapshot(void)
{
    xiaotai_net_scan_status_t *status = net_scan_edit();
    xiaotai_net_network_t *old = net_state->networks;
    net_state->networks = NULL;
    net_state->network_count = 0;
    status->state = XIAOTAI_NET_SCAN_IDLE;
    status->error = XIAOTAI_OK;
    status->truncated = false;
    net_scan_commit();
    xiaotai_mem_free(old);
}

void net_scan_cancel(void)
{
    drop_snapshot();
    if (s_token && !s_discard) {
        s_discard = true;
        cancel_due();
    }
}

static void completed(const xiaotai_net_driver_event_t *event)
{
    const xiaotai_net_driver_t *driver = net_state->driver;
    if (s_discard) {
        /* Actual completion makes the radio available. Discard driver results
         * as well, otherwise old results can block the next explicit scan. */
        if (event->error == XIAOTAI_OK) (void)driver->scan_cancel(driver->self);
        finish();
        return;
    }
    xiaotai_net_scan_record_t *raw = NULL;
    xiaotai_err_t result = event->error;
    if (result == XIAOTAI_OK) {
        raw = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, XIAOTAI_NET_SCAN_MAX * sizeof(*raw));
        if (!raw) {
            (void)driver->scan_cancel(driver->self);
            result = XIAOTAI_ERR_NO_MEM;
        } else {
            size_t count = 0;
            bool truncated = false;
            result = driver->scan_read(driver->self, raw, XIAOTAI_NET_SCAN_MAX, &count, &truncated);
            if (result == XIAOTAI_OK && count > XIAOTAI_NET_SCAN_MAX) result = XIAOTAI_ERR_INVALID_SIZE;
            if (result == XIAOTAI_OK) {
                strongest_first(raw, count);
                result = publish_list(raw, count, truncated, XIAOTAI_NET_SCAN_READY);
            }
        }
    }
    xiaotai_mem_free(raw);
    if (result != XIAOTAI_OK) state(XIAOTAI_NET_SCAN_ERROR, result);
    finish();
}

static void driver_event(const xiaotai_net_driver_event_t *event)
{
    if (event->kind == XIAOTAI_NET_DRIVER_STOPPED && s_token) {
        state(XIAOTAI_NET_SCAN_ERROR, XIAOTAI_ERR_INVALID_STATE);
        finish();
    }
    if (event->kind != XIAOTAI_NET_DRIVER_SCAN_DONE) return;
    if (!s_token || event->token != s_token) {
        xiaotai_net_diag_t *diag = net_diag_edit();
        if (diag->stale_events != UINT32_MAX) ++diag->stale_events;
        net_diag_done();
        return;
    }
    completed(event);
}

static void reset(void)
{
    if (net_state && net_state->networks) drop_snapshot();
    net_gate_scan_end();
    s_token = 0;
    s_started = false;
    s_discard = false;
    s_cancel_calls = 0;
    s_started_us = 0;
}

const net_unit_t net_scan_unit = {.name = "scan", .driver_event = driver_event, .reset = reset};
