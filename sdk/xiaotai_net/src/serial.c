// SPDX-License-Identifier: MIT
#include "portal.h"
#include "store.h"
#include "trace_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/at_command.h"
#include "xiaotai/port/mem.h"

static const char *link_name(xiaotai_net_state_t state)
{
    static const char *const names[] = {
        "stopped", "preparing", "blocked", "unconfigured", "connecting", "up", "waiting", "held",
    };
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "-";
}

static const char *kind_name(xiaotai_network_kind_t kind)
{
    static const char *const names[] = {"wifi", "wifi_coprocessor", "ethernet", "cellular"};
    return (unsigned)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "-";
}

static void ipv4_text(uint32_t value, char text[16])
{
    if (!value) { strcpy(text, "-"); return; }
    uint8_t parts[4];
    memcpy(parts, &value, sizeof(parts));
    (void)snprintf(text, 16, "%u.%u.%u.%u", (unsigned)parts[0], (unsigned)parts[1], (unsigned)parts[2],
                   (unsigned)parts[3]);
}

static void address_text(const xiaotai_net_status_t *status, char text[40])
{
    if (status->ipv4 || !status->ipv6_valid) { ipv4_text(status->ipv4, text); return; }
    size_t used = 0;
    for (unsigned i = 0; i < 16; i += 2) {
        const int n = snprintf(text + used, 40 - used, "%s%02x%02x", i ? ":" : "",
                               (unsigned)status->ipv6[i], (unsigned)status->ipv6[i + 1]);
        used += (size_t)n;
    }
}

static xiaotai_err_t status_run(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_net_status_t status;
    xiaotai_net_provision_status_t provision;
    xiaotai_net_status(&status);
    xiaotai_net_provision_status(&provision);
    char ip[40], gateway[16], mac[18] = "-", rssi[8] = "-", channel[8] = "-";
    char ssid[2 * XIAOTAI_NET_SSID_MAX + 1];
    address_text(&status, ip);
    ipv4_text(status.gateway_ipv4, gateway);
    if (status.mac_valid)
        (void)snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", (unsigned)status.mac[0],
                       (unsigned)status.mac[1], (unsigned)status.mac[2], (unsigned)status.mac[3],
                       (unsigned)status.mac[4], (unsigned)status.mac[5]);
    if (status.signal_valid) (void)snprintf(rssi, sizeof(rssi), "%d", (int)status.signal_dbm);
    if (status.channel) (void)snprintf(channel, sizeof(channel), "%u", (unsigned)status.channel);
    (void)xiaotai_at_text_encode(status.ssid, ssid, sizeof(ssid));
    const xiaotai_err_t err = status.prepare.error ? status.prepare.error : status.control_error;
    xiaotai_at_printf(reply, "+NET: state=%s kind=%s connected=%d provisioning=%d held=%d gen=%" PRIu32
                     " ssid_hex=%s ip=%s gateway=%s rssi=%s channel=%s mac=%s error=%d",
                     link_name(status.state), xiaotai_net_driver_bound() ? kind_name(status.kind) : "-",
                     status.state == XIAOTAI_NET_UP, provision.methods != 0 || provision.exclusive,
                     status.state == XIAOTAI_NET_HELD, status.generation, ssid, ip, gateway, rssi, channel, mac, err);
    return XIAOTAI_OK;
}

static bool net_started(void)
{
    const xiaotai_net_state_t state = net_state_now();
    return state != XIAOTAI_NET_STOPPED && state != XIAOTAI_NET_PREPARING && state != XIAOTAI_NET_BLOCKED;
}

xiaotai_err_t net_serial_write_result(const xiaotai_reply_t *reply, xiaotai_err_t err, bool submit)
{
    if (err != XIAOTAI_ERR_INVALID_STATE) return err;
    if (!net_started()) return xiaotai_at_fail(reply, err, "NOT_STARTED");
    if (submit && xiaotai_net_provisioning_exclusive() && !xiaotai_net_provisioning_boot())
        return xiaotai_at_fail(reply, err, "NOT_PROVISIONING");
    return xiaotai_at_fail(reply, err, "MAINTENANCE");
}

#if XIAOTAI_NET_PROVISION_SERIAL
static xiaotai_err_t credentials(bool submit, const char *const *argv, const xiaotai_reply_t *reply)
{
    if (!xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_ALLOWED;
    if (!net_started()) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "NOT_STARTED");
    xiaotai_net_submission_t request = {.method = XIAOTAI_PROVISION_SERIAL};
    xiaotai_err_t err = xiaotai_at_text_decode(argv[0], request.credentials.ssid, sizeof(request.credentials.ssid));
    if (err == XIAOTAI_OK)
        err = xiaotai_at_text_decode(argv[1], request.credentials.password, sizeof(request.credentials.password));
    if (err != XIAOTAI_OK || !net_credentials_valid_for(&request.credentials, XIAOTAI_PROVISION_SERIAL)) {
        net_store_wipe(&request, sizeof(request));
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (xiaotai_activity_current(NULL)) {
        net_store_wipe(&request, sizeof(request));
        return XIAOTAI_ERR_BUSY;
    }
    net_portal_submit_result_t receipt = {0};
    err = submit ? net_provision_submit_checked(&request, 0, NULL, &receipt) :
                   xiaotai_net_set_credentials(&request.credentials);
    net_store_wipe(&request, sizeof(request));
    if (err != XIAOTAI_OK) return net_serial_write_result(reply, err, submit);
    /* WIFI is queued: only later progress confirms verified/stored data. */
    xiaotai_at_printf(reply, "+NET-WIFI: accepted=1 verified=0 saved=%d restart=%d", !submit, receipt.restart);
    return XIAOTAI_OK;
}

static xiaotai_err_t wifi(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    return credentials(true, argv, reply);
}

static xiaotai_err_t store(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    return credentials(false, argv, reply);
}
#endif

#define NET_ACTION(name, call)                                                         \
    static xiaotai_err_t name(int argc, const char *const *argv, const xiaotai_reply_t *reply) \
    {                                                                                 \
        (void)argc; (void)argv;                                                        \
        return net_serial_write_result(reply, call(), false);                          \
    }
NET_ACTION(retry, xiaotai_net_retry)
NET_ACTION(disconnect, xiaotai_net_disconnect)
NET_ACTION(scan, xiaotai_net_scan)
#undef NET_ACTION

static bool list_next(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    enum { LIST_MAX = XIAOTAI_NET_SCAN_MAX + 5 };
    xiaotai_net_network_t *items = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, LIST_MAX * sizeof(*items));
    if (!items) { (void)xiaotai_at_fail(reply, XIAOTAI_ERR_NO_MEM, NULL); return false; }
    const size_t count = xiaotai_net_networks(items, LIST_MAX, NULL);
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_net_network_t *item = &items[i];
        char ssid[2 * XIAOTAI_NET_SSID_MAX + 1], rssi[8] = "-";
        (void)xiaotai_at_text_encode(item->ssid, ssid, sizeof(ssid));
        if (item->signal_valid) (void)snprintf(rssi, sizeof(rssi), "%d", (int)item->signal_dbm);
        xiaotai_at_printf(reply, "+NET-AP: ssid_hex=%s rssi=%s secure=%d saved=%d available=%d supported=%d",
                         ssid, rssi, item->secure, item->saved, item->available, item->supported);
    }
    xiaotai_mem_free(items);
    return false;
}

static xiaotai_err_t list(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return xiaotai_at_stream(reply, &(xiaotai_at_stream_t){.next = list_next});
}

static void provision_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    static const char *const stages[] = {
        [XIAOTAI_NET_PROVISION_VERIFYING] = "verifying", [XIAOTAI_NET_PROVISION_REJECTED] = "rejected",
        [XIAOTAI_NET_PROVISION_STORING] = "storing", [XIAOTAI_NET_PROVISION_STORE_FAILED] = "store_failed",
        [XIAOTAI_NET_PROVISION_DONE] = "done",
    };
    if (event->size != sizeof(xiaotai_net_provision_status_t)) return;
    const xiaotai_net_provision_status_t *status = event->data;
    const unsigned state = (unsigned)status->state;
    if (state < sizeof(stages) / sizeof(stages[0]) && stages[state])
        (void)xiaotai_at_notifyf("+NET-WIFI: provision=%s error=%d", stages[state], status->error);
}

xiaotai_err_t net_serial_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = status_run},
#if XIAOTAI_NET_PROVISION_SERIAL
        {.word = "WIFI", .min_args = 2, .max_args = 2, .run = wifi, .usage = "<ssid-hex> <password-hex|->"},
        {.word = "STORE", .min_args = 2, .max_args = 2, .run = store, .usage = "<ssid-hex> <password-hex|->"},
#endif
        {.word = "RETRY", .run = retry}, {.word = "DISCONNECT", .run = disconnect}, {.word = "SCAN", .run = scan},
        {.word = "LIST", .run = list}, {.word = "DIAG", .run = net_diagnostics_run},
    };
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+NET", verbs, sizeof(verbs) / sizeof(verbs[0]));
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_NET_PROVISION, provision_event, NULL);
    if (err == XIAOTAI_OK) err = net_diag_at_register();
    if (err == XIAOTAI_OK) err = net_diagnostics_register();
#if XIAOTAI_NET_CONSOLE_FORGET
    if (err == XIAOTAI_OK) err = xiaotai_net_console_forget_register();
#endif
    return err;
}
