// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "captive_dns.h"
#include "portal.h"
#include "store.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/log.h"

#define METHOD_BITS 32U
#define STORE_NOTICE_RETRY_US 100000
_Static_assert(sizeof(xiaotai_net_submission_t) <= NET_CALL_DATA_MAX, "submission fits the worker slot");
static const xiaotai_net_provision_method_t *s_methods[METHOD_BITS];
static const net_provision_portal_io_t *s_portal;
#ifdef XIAOTAI_NET_PORTAL_BUILTIN
static const net_provision_portal_io_t builtin_portal = {
    net_portal_start, net_portal_stop, net_captive_dns_start, net_captive_dns_stop,
};
#endif
static atomic_bool s_frozen, s_pending;
static uint32_t s_opened;
static bool s_ap, s_server, s_dns;
static bool s_unverified;
static unsigned s_retries;

static bool one_bit(uint32_t methods) { return methods && !(methods & (methods - 1)); }

static uint32_t available(void)
{
    return net_state->part ? net_state->part->desc.provisioning : 0;
}

static bool wifi(void)
{
    return net_state->driver && (net_state->driver->capabilities & XIAOTAI_NET_DRIVER_CREDENTIALS);
}

static void store_failed_notice(void)
{
    if (net_state->provision.state != XIAOTAI_NET_PROVISION_STORE_FAILED) return;
    if (net_provision_publish() != XIAOTAI_OK) {
        net_deadline_set(NET_DEADLINE_PROVISION, xiaotai_time_us() + STORE_NOTICE_RETRY_US, store_failed_notice);
    }
}

static void set_phase(xiaotai_net_provision_state_t state, xiaotai_err_t error, bool restart)
{
    xiaotai_net_provision_status_t *status = net_provision_edit();
    status->state = state;
    status->error = error;
    if (restart) status->restart_required = true;
    if (state == XIAOTAI_NET_PROVISION_CLOSED || state == XIAOTAI_NET_PROVISION_REJECTED ||
        state == XIAOTAI_NET_PROVISION_DONE) memset(status->ssid, 0, sizeof(status->ssid));
    const bool failed = state == XIAOTAI_NET_PROVISION_STORE_FAILED;
    net_provision_commit(!restart && !failed);
    if (restart) net_restart_publish();
    else if (failed) store_failed_notice();
}

static void phase(xiaotai_net_provision_state_t state, xiaotai_err_t error) { set_phase(state, error, false); }

bool net_provision_submission_pending(void) { return atomic_load(&s_pending); }

xiaotai_err_t xiaotai_net_provision_method_register(const xiaotai_net_provision_method_t *method)
{
    if (!method || !one_bit(method->method) || !method->name || !*method->name ||
        !method->open || !method->close || (method->method & (XIAOTAI_PROVISION_SERIAL | XIAOTAI_PROVISION_PORTAL)))
        return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_load(&s_frozen)) return XIAOTAI_ERR_INVALID_STATE;
    unsigned slot = 0;
    while ((UINT32_C(1) << slot) != method->method) ++slot;
    if (s_methods[slot]) return XIAOTAI_ERR_INVALID_ARG;
    s_methods[slot] = method;
    return XIAOTAI_OK;
}

xiaotai_err_t net_provision_portal_register(const net_provision_portal_io_t *io)
{
    if (!io || !io->server_start || !io->server_stop || !io->dns_start || !io->dns_stop)
        return XIAOTAI_ERR_INVALID_ARG;
    if (s_portal || atomic_load(&s_frozen)) return XIAOTAI_ERR_INVALID_STATE;
    s_portal = io;
    return XIAOTAI_OK;
}

static xiaotai_err_t stop_portal(void)
{
    xiaotai_err_t err = XIAOTAI_OK;
    if (s_dns) {
        err = s_portal->dns_stop();
        if (err == XIAOTAI_OK) s_dns = false;
    }
    if (err == XIAOTAI_OK && s_server) {
        err = s_portal->server_stop();
        if (err == XIAOTAI_OK) s_server = false;
    }
    if (err == XIAOTAI_OK && s_ap) {
        err = net_state->driver->ap_stop(net_state->driver->self);
        if (err == XIAOTAI_OK) s_ap = false;
    }
    return err;
}

static xiaotai_err_t start_portal(xiaotai_net_provision_status_t *snapshot)
{
    const xiaotai_net_driver_t *driver = net_state->driver;
    if (!s_portal || !driver->mac || !(driver->capabilities & XIAOTAI_NET_DRIVER_ACCESS_POINT))
        return XIAOTAI_ERR_NOT_SUPPORTED;
    uint8_t mac[6];
    xiaotai_err_t err = driver->mac(driver->self, mac);
    if (err != XIAOTAI_OK) return err;
    xiaotai_net_ap_config_t config = {.channel = XIAOTAI_NET_AP_CHANNEL, .max_clients = XIAOTAI_NET_AP_CLIENTS_MAX};
    const uint8_t ip[] = {192, 168, 6, 1};
    memcpy(&config.ipv4, ip, sizeof(ip));
    (void)snprintf(config.ssid, sizeof(config.ssid), "%s%02X%02X", net_state->config.ap_prefix,
                   (unsigned)mac[4], (unsigned)mac[5]);
    /* A failed start can leave work for the driver's idempotent stop. Keep
     * ownership until teardown is confirmed, even before opening HTTP/DNS. */
    s_ap = true;
    err = driver->ap_start(driver->self, &config);
    if (err == XIAOTAI_OK) {
        /* A server whose partial startup failed may still need its stop
         * retried before the underlying AP can be torn down. */
        s_server = true;
        err = s_portal->server_start(config.ipv4);
    }
    if (err == XIAOTAI_OK) {
        err = s_portal->dns_start(config.ipv4);
    }
    if (err == XIAOTAI_OK) {
        s_dns = true;
        memcpy(snapshot->ap_ssid, config.ssid, sizeof(snapshot->ap_ssid));
        memcpy(snapshot->url, "http://" XIAOTAI_NET_AP_IPV4, sizeof("http://" XIAOTAI_NET_AP_IPV4));
    }
    return err;
}

static xiaotai_err_t stop_methods(void)
{
    xiaotai_err_t first = XIAOTAI_OK;
    for (unsigned i = METHOD_BITS; i > 0; --i) {
        const xiaotai_net_provision_method_t *method = s_methods[i - 1];
        if (!method || !(s_opened & method->method)) continue;
        const xiaotai_err_t err = method->close(method->ctx);
        if (err == XIAOTAI_OK) s_opened &= ~method->method;
        else if (first == XIAOTAI_OK) first = err;
    }
    s_opened &= ~(uint32_t)XIAOTAI_PROVISION_SERIAL;
    const xiaotai_err_t portal = stop_portal();
    if (portal == XIAOTAI_OK) s_opened &= ~(uint32_t)XIAOTAI_PROVISION_PORTAL;
    return first == XIAOTAI_OK ? portal : first;
}

void net_provision_open(xiaotai_net_provision_reason_t reason)
{
    if (!net_state->driver_running || !wifi()) return;
    xiaotai_net_provision_status_t visible;
    xiaotai_net_provision_status(&visible);
    if (net_provision_submission_pending() || visible.restart_required) return;
    if (visible.methods) {
        if (reason == XIAOTAI_NET_PROVISION_REASON_REQUESTED && visible.reason != reason) {
            xiaotai_net_provision_status_t *current = net_provision_edit();
            current->reason = reason;
            net_provision_commit(true);
        }
        return;
    }
    uint32_t methods = available();
    if (!XIAOTAI_NET_PROVISION_SERIAL) methods &= ~(uint32_t)XIAOTAI_PROVISION_SERIAL;
    if (!XIAOTAI_NET_PROVISION_PORTAL) methods &= ~(uint32_t)XIAOTAI_PROVISION_PORTAL;
    if (!methods) return;
    visible.reason = reason;
    visible.state = XIAOTAI_NET_PROVISION_OPENING;
    visible.error = XIAOTAI_OK;
    xiaotai_net_provision_status_t *status = net_provision_edit();
    status->reason = reason;
    status->state = XIAOTAI_NET_PROVISION_OPENING;
    status->error = XIAOTAI_OK;
    net_provision_commit(true);

    xiaotai_err_t err = stop_methods();
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGW("xiaotai_net", "provisioning residue: 0x%x", (unsigned)err);
        phase(XIAOTAI_NET_PROVISION_CLOSED, err);
        return;
    }
    if (err == XIAOTAI_OK && (methods & XIAOTAI_PROVISION_PORTAL)) {
        err = start_portal(&visible);
        if (err == XIAOTAI_OK) s_opened |= XIAOTAI_PROVISION_PORTAL;
    }
    if (err == XIAOTAI_OK && (methods & XIAOTAI_PROVISION_SERIAL)) s_opened |= XIAOTAI_PROVISION_SERIAL;
    for (unsigned i = 0; err == XIAOTAI_OK && i < METHOD_BITS; ++i) {
        const xiaotai_net_provision_method_t *method = s_methods[i];
        if (!method || !(methods & method->method)) continue;
        err = method->open(method->ctx, &visible);
        if (err == XIAOTAI_OK) s_opened |= method->method;
    }
    if (err == XIAOTAI_OK && s_opened != methods) err = XIAOTAI_ERR_NOT_SUPPORTED;
    if (err != XIAOTAI_OK) {
        const xiaotai_err_t cleanup = stop_methods();
        if (cleanup != XIAOTAI_OK) XIAOTAI_LOGW("xiaotai_net", "provisioning cleanup: 0x%x", (unsigned)cleanup);
        phase(XIAOTAI_NET_PROVISION_CLOSED, err);
        return;
    }
    net_scan_prepare();
    status = net_provision_edit();
    status->methods = s_opened;
    status->state = XIAOTAI_NET_PROVISION_OPEN;
    memset(status->ssid, 0, sizeof(status->ssid));
    memcpy(status->ap_ssid, visible.ap_ssid, sizeof(status->ap_ssid));
    memcpy(status->url, visible.url, sizeof(status->url));
    net_provision_commit(true);
}

static xiaotai_err_t close_methods(void)
{
    xiaotai_net_provision_status_t *status = net_provision_edit();
    const bool was_open = status->methods != 0;
    const bool preserve_progress = net_provision_submission_pending() || status->state == XIAOTAI_NET_PROVISION_DONE;
    if (was_open) {
        status->methods = 0;
        memset(status->ap_ssid, 0, sizeof(status->ap_ssid));
        memset(status->ap_password, 0, sizeof(status->ap_password));
        memset(status->url, 0, sizeof(status->url));
        if (!preserve_progress) status->state = XIAOTAI_NET_PROVISION_CLOSING;
        net_provision_commit(true);
    } else {
        net_snapshot_cancel();
    }
    const xiaotai_err_t err = stop_methods();
    if (was_open) net_scan_cancel();
    if (was_open && !preserve_progress) phase(XIAOTAI_NET_PROVISION_CLOSED, err);
    return err;
}

void net_provision_close_all(void)
{
    const xiaotai_err_t err = close_methods();
    if (err != XIAOTAI_OK) XIAOTAI_LOGW("xiaotai_net", "provisioning close: 0x%x", (unsigned)err);
}

static void link_provision(bool open, xiaotai_net_provision_reason_t reason)
{
    if (open) net_provision_open(reason);
    else net_provision_close_all();
}

static void drop_candidate(void)
{
    net_deadline_clear(NET_DEADLINE_VERIFY);
    net_deadline_clear(NET_DEADLINE_PROVISION);
    net_store_wipe(&net_state->provision_candidate, sizeof(net_state->provision_candidate));
    atomic_store(&s_pending, false);
    net_gate_config_end();
}

static void reject(xiaotai_err_t err)
{
    const bool exclusive = net_state->provisioning_boot;
    xiaotai_net_diag_t *diag = net_diag_edit();
    if (diag->submissions_rejected != UINT32_MAX) ++diag->submissions_rejected;
    net_diag_done();
    if (exclusive) net_link_mark_candidate_retiring();
    drop_candidate();
    phase(XIAOTAI_NET_PROVISION_REJECTED, err);
    if (exclusive) net_link_try_discard();
    else net_link_try_end(false);
}

static void verify_timeout(void) { reject(XIAOTAI_ERR_TIMEOUT); }

static void store_candidate(void)
{
    const xiaotai_net_credentials_t *candidate = &net_state->provision_candidate;
    const xiaotai_err_t err = net_store_save(candidate);
    if (err != XIAOTAI_OK) {
        xiaotai_net_diag_t *diag = net_diag_edit();
        if (diag->store_failures != UINT32_MAX) ++diag->store_failures;
        net_diag_done();
        phase(XIAOTAI_NET_PROVISION_STORE_FAILED, err);
        return;
    }
    const bool restart = net_state->provisioning_boot || s_unverified || !net_state->config.keep_running;
    bool promised = false;
    if (restart) {
        if (!s_unverified) net_history_remember(candidate);
        promised = net_restart_prepare();
    } else {
        net_state->credentials = *candidate;
        net_state->has_credentials = true;
    }
    drop_candidate();
    /* Publish only the durable final result before the generic restart job,
     * so the owner can apply the product's chosen restart policy first. */
    set_phase(XIAOTAI_NET_PROVISION_DONE, XIAOTAI_OK, promised);
    if (restart) net_link_try_discard();
    else net_link_try_end(true);
}

static void verified(void *ctx)
{
    (void)ctx;
    if (!net_provision_submission_pending() || !net_deadline_due(NET_DEADLINE_VERIFY)) return;
    if (xiaotai_time_us() >= net_deadline_due(NET_DEADLINE_VERIFY)) {
        verify_timeout();
        return;
    }
    net_deadline_clear(NET_DEADLINE_VERIFY);
    phase(XIAOTAI_NET_PROVISION_STORING, XIAOTAI_OK);
    store_candidate();
}

static void try_candidate(void);

static void verification_lost(xiaotai_net_loss_t reason, int32_t code, void *ctx)
{
    (void)ctx;
    const int64_t due = net_deadline_due(NET_DEADLINE_VERIFY);
    if (!net_provision_submission_pending() || !due) return;
    if (xiaotai_time_us() >= due) {
        verify_timeout();
    } else if (reason != XIAOTAI_NET_LOSS_REFUSED && s_retries < XIAOTAI_NET_VERIFY_RETRIES) {
        ++s_retries;
        net_deadline_set(NET_DEADLINE_PROVISION, xiaotai_time_us() + 1, try_candidate);
    } else {
        reject(reason == XIAOTAI_NET_LOSS_NOT_FOUND || code == XIAOTAI_ERR_NOT_FOUND ? XIAOTAI_ERR_NOT_FOUND :
               reason == XIAOTAI_NET_LOSS_ADDRESS || code == XIAOTAI_ERR_TIMEOUT ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_FAIL);
    }
}

static void try_candidate(void)
{
    if (xiaotai_time_us() >= net_deadline_due(NET_DEADLINE_VERIFY)) {
        verify_timeout();
        return;
    }
    const net_link_observer_t observer = {.address = verified, .lost = verification_lost};
    const xiaotai_err_t err = net_link_try(&net_state->provision_candidate, &observer);
    if (err != XIAOTAI_OK) reject(err == XIAOTAI_ERR_TIMEOUT ? err : XIAOTAI_FAIL);
}

static bool can_submit(uint32_t method)
{
    if (method == XIAOTAI_PROVISION_SERIAL)
        return XIAOTAI_NET_PROVISION_SERIAL && (available() & method);
    return (s_opened & method) && (net_state->provision.methods & method);
}

static xiaotai_err_t submit(net_call_t *call)
{
    xiaotai_net_submission_t request;
    memcpy(&request, call->data, sizeof(request));
    net_store_wipe(call->data, call->size);
    call->size = 0;
    net_portal_submit_result_t receipt = {0};
    xiaotai_err_t err = XIAOTAI_OK;
    if (!net_state->driver_running || !net_state->link.radio_ready) err = XIAOTAI_ERR_INVALID_STATE;
    else if (!wifi()) err = XIAOTAI_ERR_NOT_SUPPORTED;
    else if (net_state->config.provisioning == XIAOTAI_NET_PROVISIONING_EXCLUSIVE && !net_state->provisioning_boot)
        err = XIAOTAI_ERR_INVALID_STATE;
    else if (net_provision_submission_pending() || net_state->provision.restart_required) {
        receipt.reason = net_state->provision.restart_required ? NET_PORTAL_SUBMIT_RESTART : NET_PORTAL_SUBMIT_PENDING;
        err = XIAOTAI_ERR_BUSY;
    }
    else if (!can_submit(request.method)) err = XIAOTAI_ERR_INVALID_STATE;
    const bool unverified = net_state->config.store_unverified || (net_state->config.unverified_methods & request.method);
    if (err == XIAOTAI_OK) err = net_gate_config_begin();
    const bool leased = err == XIAOTAI_OK;
    if (err == XIAOTAI_OK && !unverified && net_scan_running()) {
        receipt.reason = NET_PORTAL_SUBMIT_SCAN;
        err = XIAOTAI_ERR_BUSY;
    }
    if (err == XIAOTAI_OK && request.use_saved) {
        err = net_history_password(request.credentials.ssid, true, request.credentials.password);
        if (err == XIAOTAI_ERR_NOT_FOUND)
            err = net_history_password(request.credentials.ssid, false, request.credentials.password);
        if (err == XIAOTAI_ERR_NOT_FOUND) receipt.reason = NET_PORTAL_SUBMIT_SAVED_MISS;
    }
    if (err == XIAOTAI_OK) {
        xiaotai_net_status_t *link = net_status_edit();
        if (link->control_pending) err = XIAOTAI_ERR_INVALID_STATE;
        else atomic_store(&s_pending, true);
        net_snapshot_cancel();
    }
    if (err == XIAOTAI_OK) {
        xiaotai_net_diag_t *diag = net_diag_edit();
        if (diag->submissions != UINT32_MAX) ++diag->submissions;
        net_diag_done();
        net_state->provision_candidate = request.credentials;
        s_unverified = unverified;
        s_retries = 0;
        receipt.verify = !unverified;
        receipt.restart = net_state->provisioning_boot || unverified || !net_state->config.keep_running;
        const int64_t now = xiaotai_time_us() + 1;
        xiaotai_net_provision_status_t *status = net_provision_edit();
        if (!status->methods) status->reason = XIAOTAI_NET_PROVISION_REASON_SUBMISSION;
        status->state = unverified ? XIAOTAI_NET_PROVISION_STORING : XIAOTAI_NET_PROVISION_VERIFYING;
        status->error = XIAOTAI_OK;
        memcpy(status->ssid, request.credentials.ssid, sizeof(status->ssid));
        net_provision_commit(true);
        if (!unverified)
            net_deadline_set(NET_DEADLINE_VERIFY, now + (int64_t)XIAOTAI_NET_VERIFY_TIMEOUT_MS * 1000, verify_timeout);
        net_deadline_set(NET_DEADLINE_PROVISION, now, unverified ? store_candidate : try_candidate);
    } else if (leased) {
        net_gate_config_end();
    }
    net_store_wipe(&request, sizeof(request));
    memcpy(call->data, &receipt, sizeof(receipt));
    call->size = sizeof(receipt);
    return err;
}

xiaotai_err_t net_provision_submit_checked(const xiaotai_net_submission_t *submission, uint32_t token,
                                           bool (*active)(uint32_t), net_portal_submit_result_t *receipt)
{
    if (receipt) *receipt = (net_portal_submit_result_t){0};
    if (!submission || !one_bit(submission->method) ||
        !net_credentials_valid_for(&submission->credentials, submission->method) ||
        (submission->use_saved && submission->credentials.password[0]))
        return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_net_submission_t copy = {.method = submission->method, .use_saved = submission->use_saved};
    memcpy(copy.credentials.ssid, submission->credentials.ssid, strlen(submission->credentials.ssid));
    memcpy(copy.credentials.password, submission->credentials.password, strlen(submission->credentials.password));
    const size_t out_size = receipt ? sizeof(*receipt) : 0;
    const xiaotai_err_t err = active ? net_call_cancellable(submit, token, active, &copy, sizeof(copy), receipt,
                                                           out_size, XIAOTAI_KV_WAIT_MAX_MS) :
        net_call(submit, 0, &copy, sizeof(copy), receipt, out_size, NULL, XIAOTAI_KV_WAIT_MAX_MS);
    net_store_wipe(&copy, sizeof(copy));
    return err;
}

xiaotai_err_t xiaotai_net_provision_submit(const xiaotai_net_submission_t *submission)
{
    return net_provision_submit_checked(submission, 0, NULL, NULL);
}

static xiaotai_err_t store_retry(net_call_t *call)
{
    (void)call;
    if (!net_provision_submission_pending() || net_state->provision.state != XIAOTAI_NET_PROVISION_STORE_FAILED)
        return XIAOTAI_ERR_INVALID_STATE;
    phase(XIAOTAI_NET_PROVISION_STORING, XIAOTAI_OK);
    net_deadline_set(NET_DEADLINE_PROVISION, xiaotai_time_us() + 1, store_candidate);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_net_provision_store_retry(void)
{
    return net_call(store_retry, 0, NULL, 0, NULL, 0, NULL, XIAOTAI_KV_WAIT_MAX_MS);
}

static void control(const net_message_t *message)
{
    const bool close = message->u.job.arg == 2;
    const bool forget = message->u.job.arg == 1;
    xiaotai_err_t err = XIAOTAI_OK;
    if (!net_state->driver_running) err = XIAOTAI_ERR_INVALID_STATE;
    else if (!wifi()) err = XIAOTAI_ERR_NOT_SUPPORTED;
    else if (net_state->config.provisioning == XIAOTAI_NET_PROVISIONING_EXCLUSIVE) {
        if (close) {
            if (!net_state->provisioning_boot) err = XIAOTAI_ERR_INVALID_STATE;
            else if (!xiaotai_net_has_saved_credentials()) err = XIAOTAI_ERR_NOT_FOUND;
            else err = net_store_clear_boot();
        } else {
            err = net_store_request_boot(forget);
        }
        if (err == XIAOTAI_OK) net_restart_required();
    } else if (close) {
        if (net_state->provision.methods || s_opened || s_ap || s_server || s_dns) {
            err = close_methods();
            if (err == XIAOTAI_OK && net_state->has_credentials && net_state_now() == XIAOTAI_NET_HELD) {
                net_link_control_provision(false);
                return;
            }
        }
    } else {
        if (forget) {
            err = net_store_forget();
            if (err == XIAOTAI_OK) {
                net_state->has_credentials = false;
                net_store_wipe(&net_state->credentials, sizeof(net_state->credentials));
            }
        }
        if (err == XIAOTAI_OK) {
            net_link_control_provision(true);
            return;
        }
    }
    net_link_control_finish(err);
}

xiaotai_err_t xiaotai_net_reprovision(bool forget) { return net_link_control_enqueue(control, forget, false); }

xiaotai_err_t xiaotai_net_provision_close(void)
{
    if (net_provision_submission_pending()) return XIAOTAI_ERR_INVALID_STATE;
    if (xiaotai_net_provisioning_exclusive()) {
        if (!xiaotai_net_provisioning_boot()) return XIAOTAI_ERR_INVALID_STATE;
        if (!xiaotai_net_has_saved_credentials()) return XIAOTAI_ERR_NOT_FOUND;
    }
    return net_link_control_enqueue(control, 2, false);
}

static xiaotai_err_t init(void)
{
    /* Unreadable storage is not an unconfigured device. Do not open a portal. */
    if (xiaotai_storage_boot_error() != XIAOTAI_OK) return XIAOTAI_OK;
#ifdef XIAOTAI_NET_PORTAL_BUILTIN
    if (!s_portal) s_portal = &builtin_portal;
#endif
    if (net_state->config.provisioning == XIAOTAI_NET_PROVISIONING_EXCLUSIVE) {
        bool requested = false;
        const xiaotai_err_t err = net_store_boot_flag(&requested);
        if (err != XIAOTAI_OK)
            XIAOTAI_LOGW("xiaotai_net", "provisioning flag read: 0x%x", (unsigned)err);
        net_state->provisioning_boot = !net_state->has_credentials || requested;
        xiaotai_net_provision_status_t *status = net_provision_edit();
        status->exclusive = net_state->provisioning_boot;
        net_provision_commit(false);
    }
    return XIAOTAI_OK;
}

static void bind(void) { atomic_store(&s_frozen, true); }

static void driver_started(void)
{
    if (net_state->provisioning_boot)
        net_provision_open(net_state->has_credentials ? XIAOTAI_NET_PROVISION_REASON_REQUESTED :
                                                        XIAOTAI_NET_PROVISION_REASON_UNCONFIGURED);
}

static void reset(void)
{
    memset(s_methods, 0, sizeof(s_methods));
    s_portal = NULL;
    s_opened = 0;
    s_ap = s_server = s_dns = s_unverified = false;
    s_retries = 0;
    atomic_store(&s_frozen, false);
    atomic_store(&s_pending, false);
}

static void worker_stop(void)
{
    if (net_provision_submission_pending()) drop_candidate();
    (void)close_methods();
}

const net_unit_t net_provision_unit = {.name = "provision", .init = init, .bind = bind,
    .driver_started = driver_started, .link_provision = link_provision, .worker_stop = worker_stop, .reset = reset};
