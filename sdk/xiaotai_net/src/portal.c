// SPDX-License-Identifier: MIT
#include "portal.h"
#include "net_platform.h"
#include "store.h"
#include "xiaotai/net_config.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define BODY_TIMEOUT_US INT64_C(5000000)
#define BODY_TIMEOUTS 3U
/* The scan snapshot may append five saved-only RECORD entries. */
#define LIST_CAPACITY (XIAOTAI_NET_SCAN_MAX + 5U)
#define JSON_TYPE "application/json"
#define HTML_TYPE "text/html; charset=utf-8"
#define TEXT_TYPE "text/plain; charset=utf-8"

static _Atomic(uint32_t) s_token;
static uint32_t s_next; /* Network worker only. */
static const char *const s_headers[] = {
    "Cache-Control: no-store",
    "X-Content-Type-Options: nosniff",
};

bool net_portal_active(uint32_t token)
{
    return token && atomic_load_explicit(&s_token, memory_order_acquire) == token;
}

xiaotai_err_t net_provision_portal_submit(const xiaotai_net_submission_t *submission, uint32_t token,
                                          net_portal_submit_result_t *result)
{
    return net_provision_submit_checked(submission, token, net_portal_active, result);
}

static bool open_snapshot(uint32_t token, xiaotai_net_provision_status_t *status)
{
    if (!net_portal_active(token)) return false;
    xiaotai_net_provision_status(status);
    return (status->methods & XIAOTAI_PROVISION_PORTAL) != 0 && net_portal_active(token);
}

static const char *boolean_word(bool value) { return value ? "true" : "false"; }

static xiaotai_err_t begin(net_exchange_t *x, int code, const char *type)
{
    return net_platform_reply_begin(x, code, type, s_headers, sizeof(s_headers) / sizeof(s_headers[0]));
}

static void reply(net_exchange_t *x, int code, const char *type, const void *body, size_t size)
{
    if (begin(x, code, type) == XIAOTAI_OK && net_platform_reply_chunk(x, body, size) == XIAOTAI_OK)
        (void)net_platform_reply_end(x);
}

static void error_reply(net_exchange_t *x, int code, const char *message)
{
    reply(x, code, TEXT_TYPE, message, strlen(message));
}

static void redirect(net_exchange_t *x)
{
    static const char *const headers[] = {
        "Cache-Control: no-store",
        "X-Content-Type-Options: nosniff",
        "Location: " XIAOTAI_NET_AP_URL,
    };
    static const char body[] = "<!doctype html><meta http-equiv=\"refresh\" content=\"0; url="
                               XIAOTAI_NET_AP_URL "\"><a href=\"" XIAOTAI_NET_AP_URL "\">Wi-Fi setup</a>";
    if (net_platform_reply_begin(x, 303, HTML_TYPE, headers, sizeof(headers) / sizeof(headers[0])) == XIAOTAI_OK &&
        net_platform_reply_chunk(x, body, sizeof(body) - 1U) == XIAOTAI_OK)
        (void)net_platform_reply_end(x);
}

static const char *provision_state(xiaotai_net_provision_state_t state)
{
    switch (state) {
    case XIAOTAI_NET_PROVISION_VERIFYING: return "verifying";
    case XIAOTAI_NET_PROVISION_REJECTED: return "rejected";
    case XIAOTAI_NET_PROVISION_STORING: return "storing";
    case XIAOTAI_NET_PROVISION_STORE_FAILED: return "store_failed";
    case XIAOTAI_NET_PROVISION_DONE: return "done";
    default: return "open";
    }
}

static const char *provision_error(const xiaotai_net_provision_status_t *status)
{
    if (status->state == XIAOTAI_NET_PROVISION_STORE_FAILED) return "storage";
    if (status->state != XIAOTAI_NET_PROVISION_REJECTED) return "none";
    if (status->error == XIAOTAI_ERR_TIMEOUT) return "timeout";
    if (status->error == XIAOTAI_ERR_NOT_FOUND) return "not_found";
    return "refused";
}

static const char *provision_reason(xiaotai_net_provision_reason_t reason)
{
    switch (reason) {
    case XIAOTAI_NET_PROVISION_REASON_FAILING: return "failing";
    case XIAOTAI_NET_PROVISION_REASON_DISCONNECTED: return "disconnected";
    case XIAOTAI_NET_PROVISION_REASON_REQUESTED: return "requested";
    case XIAOTAI_NET_PROVISION_REASON_SUBMISSION: return "submission";
    default: return "unconfigured";
    }
}

static void status_reply(net_exchange_t *x, const xiaotai_net_provision_status_t *status)
{
    char text[NET_PORTAL_TEXT_MAX], hex[NET_PORTAL_HEX_MAX], body[448];
    net_portal_json_ssid(status->ssid, text, hex);
    const int size = snprintf(body, sizeof(body),
                              "{\"state\":\"%s\",\"error\":\"%s\",\"reason\":\"%s\",\"ssid\":%s,"
                              "\"ssid_hex\":\"%s\",\"restart\":%s,\"exclusive\":%s}",
                              provision_state(status->state), provision_error(status), provision_reason(status->reason),
                              text, hex, boolean_word(status->restart_required), boolean_word(status->exclusive));
    if (size < 0 || (size_t)size >= sizeof(body)) error_reply(x, 500, "status unavailable");
    else reply(x, 200, JSON_TYPE, body, (size_t)size);
}

static const char *scan_state(xiaotai_net_scan_state_t state)
{
    switch (state) {
    case XIAOTAI_NET_SCAN_QUEUED: return "queued";
    case XIAOTAI_NET_SCAN_RUNNING: return "scanning";
    case XIAOTAI_NET_SCAN_READY: return "ready";
    case XIAOTAI_NET_SCAN_BUSY: return "busy";
    case XIAOTAI_NET_SCAN_ERROR: return "error";
    default: return "idle";
    }
}

static bool row(net_exchange_t *x, const xiaotai_net_network_t *network, bool first)
{
    char text[NET_PORTAL_TEXT_MAX], hex[NET_PORTAL_HEX_MAX], body[400], rssi[8] = "null";
    net_portal_json_ssid(network->ssid, text, hex);
    if (network->signal_valid) (void)snprintf(rssi, sizeof(rssi), "%d", (int)network->signal_dbm);
    const int size = snprintf(body, sizeof(body),
                              "%s{\"ssid\":%s,\"ssid_hex\":\"%s\",\"rssi\":%s,\"secure\":%s,\"saved\":%s,"
                              "\"available\":%s,\"supported\":%s}", first ? "" : ",", text, hex, rssi,
                              boolean_word(network->secure), boolean_word(network->saved),
                              boolean_word(network->available), boolean_word(network->supported));
    return size >= 0 && (size_t)size < sizeof(body) && net_platform_reply_chunk(x, body, (size_t)size) == XIAOTAI_OK;
}

static void networks_reply(net_exchange_t *x, uint32_t token)
{
    xiaotai_net_scan_status_t status;
    xiaotai_net_network_t *list = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, LIST_CAPACITY * sizeof(*list));
    if (!list) {
        error_reply(x, 500, "networks unavailable");
        return;
    }
    const size_t count = xiaotai_net_networks(list, LIST_CAPACITY, &status);
    xiaotai_net_provision_status_t visible;
    if (!open_snapshot(token, &visible)) {
        xiaotai_mem_free(list);
        error_reply(x, 403, "provisioning closed");
        return;
    }
    char header[144];
    const int size = snprintf(header, sizeof(header), "{\"state\":\"%s\",\"truncated\":%s,\"history_error\":%s,"
                              "\"scan_error\":%d,\"networks\":[", scan_state(status.state),
                              boolean_word(status.truncated), boolean_word(status.history_error), status.error);
    if (size < 0 || (size_t)size >= sizeof(header)) {
        error_reply(x, 500, "networks unavailable");
    } else if (begin(x, 200, JSON_TYPE) == XIAOTAI_OK &&
               net_platform_reply_chunk(x, header, (size_t)size) == XIAOTAI_OK) {
        bool ok = true;
        for (size_t i = 0; i < count && ok; ++i) ok = net_portal_active(token) && row(x, &list[i], i == 0);
        if (ok && net_portal_active(token) && net_platform_reply_chunk(x, "]}", 2) == XIAOTAI_OK)
            (void)net_platform_reply_end(x);
    }
    xiaotai_mem_free(list);
}

/* The server port guarantees its timeout bounds each read. Check the total
 * budget even after a successful final fragment, and observe withdrawal
 * before dispatching any credentials. */
static int read_body(net_exchange_t *x, uint32_t token, uint8_t *body, size_t total)
{
    size_t used = 0;
    unsigned timeouts = 0;
    const int64_t started = xiaotai_time_us();
    xiaotai_net_provision_status_t visible;
    while (used < total) {
        if (!open_snapshot(token, &visible) || xiaotai_time_us() - started >= BODY_TIMEOUT_US) return 408;
        size_t size = 0;
        const xiaotai_err_t err = net_platform_body_read(x, body + used, total - used, &size);
        if (!open_snapshot(token, &visible) || xiaotai_time_us() - started >= BODY_TIMEOUT_US) return 408;
        if (err == XIAOTAI_ERR_TIMEOUT) {
            if (++timeouts >= BODY_TIMEOUTS) return 408;
            continue;
        }
        if (err != XIAOTAI_OK || !size || size > total - used) return 400;
        used += size;
        timeouts = 0;
    }
    return 200;
}

static void submit_error(net_exchange_t *x, xiaotai_err_t err, net_portal_submit_reason_t reason)
{
    switch (err) {
    case XIAOTAI_ERR_INVALID_ARG: error_reply(x, 400, "invalid credentials"); break;
    case XIAOTAI_ERR_NOT_FOUND: error_reply(x, 409, "saved password not found"); break;
    case XIAOTAI_ERR_BUSY:
        error_reply(x, 409, reason == NET_PORTAL_SUBMIT_RESTART ? "restart pending" :
                            reason == NET_PORTAL_SUBMIT_SCAN ? "scan running" : "submission pending");
        break;
    case XIAOTAI_ERR_INVALID_STATE: error_reply(x, 503, "network unavailable"); break;
    default: error_reply(x, 500, "submission failed"); break;
    }
}

static void submit(net_exchange_t *x, uint32_t token, size_t length)
{
    if (!length || length > NET_PORTAL_BODY_MAX) {
        error_reply(x, 400, "invalid request length");
        return;
    }
    uint8_t body[NET_PORTAL_BODY_MAX];
    const int status = read_body(x, token, body, length);
    xiaotai_net_submission_t submission;
    const bool valid = status == 200 && net_portal_json_credentials(body, length, &submission);
    net_store_wipe(body, length);
    if (status != 200) {
        error_reply(x, status, status == 408 ? "request timeout" : "invalid request body");
        return;
    }
    if (!valid) {
        error_reply(x, 400, "invalid credentials");
        return;
    }
    xiaotai_net_provision_status_t visible;
    if (!open_snapshot(token, &visible)) {
        net_store_wipe(&submission, sizeof(submission));
        error_reply(x, 403, "provisioning closed");
        return;
    }
    net_portal_submit_result_t result = {0};
    const xiaotai_err_t err = net_provision_portal_submit(&submission, token, &result);
    net_store_wipe(&submission, sizeof(submission));
    if (err != XIAOTAI_OK) {
        if (!net_portal_active(token)) error_reply(x, 403, "provisioning closed");
        else submit_error(x, err, result.reason);
        return;
    }
    char response[62];
    const int size = snprintf(response, sizeof(response), "{\"accepted\":true,\"verify\":%s,\"restart\":%s}",
                              boolean_word(result.verify), boolean_word(result.restart));
    if (size < 0 || (size_t)size >= sizeof(response)) error_reply(x, 500, "submission unavailable");
    else reply(x, 200, JSON_TYPE, response, (size_t)size);
}

static void handle(const net_request_t *r, net_exchange_t *x, void *context)
{
    (void)context;
    const uint32_t token = atomic_load_explicit(&s_token, memory_order_acquire);
    xiaotai_net_provision_status_t status;
    if (!r->local_is_ap || !open_snapshot(token, &status)) {
        error_reply(x, 403, "provisioning closed");
        return;
    }
    if (!strcmp(r->method, "GET")) {
        if (!strcmp(r->path, "/")) {
            size_t size = 0;
            const char *page = net_platform_page(&size);
            if (page && size) reply(x, 200, HTML_TYPE, page, size);
            else error_reply(x, 500, "page unavailable");
            return;
        }
        if (!strcmp(r->path, "/api/wifi/status")) {
            status_reply(x, &status);
            return;
        }
        if (!strcmp(r->path, "/api/wifi/networks")) {
            networks_reply(x, token);
            return;
        }
    } else if (!strcmp(r->method, "POST")) {
        const bool scan = !strcmp(r->path, "/api/wifi/scan");
        if (scan || !strcmp(r->path, "/api/wifi")) {
            if (r->origin && strcmp(r->origin, XIAOTAI_NET_AP_URL)) {
                error_reply(x, 403, "invalid origin");
            } else if (!scan) {
                submit(x, token, r->content_length);
            } else if (r->content_length) {
                error_reply(x, 400, "invalid request length");
            } else {
                static const char accepted[] = "{\"accepted\":true}";
                const xiaotai_err_t err = net_scan_request(token, net_portal_active);
                if (err == XIAOTAI_OK) reply(x, 200, JSON_TYPE, accepted, sizeof(accepted) - 1U);
                else if (!net_portal_active(token)) error_reply(x, 403, "provisioning closed");
                else error_reply(x, 500, "scan unavailable");
            }
            return;
        }
    }
    redirect(x);
}

xiaotai_err_t net_portal_start(uint32_t ipv4)
{
    if (atomic_load_explicit(&s_token, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    if (++s_next == 0) ++s_next;
    atomic_store_explicit(&s_token, s_next, memory_order_release);
    const xiaotai_err_t err = net_platform_server_start(ipv4, XIAOTAI_NET_PORTAL_STACK_BYTES, handle, NULL);
    if (err != XIAOTAI_OK) atomic_store_explicit(&s_token, 0, memory_order_release);
    return err;
}

xiaotai_err_t net_portal_stop(void)
{
    /* A handler waiting for worker admission sees this before the native
     * server joins it. Queued requests carry the retired token as well. */
    atomic_store_explicit(&s_token, 0, memory_order_release);
    return net_platform_server_stop();
}
