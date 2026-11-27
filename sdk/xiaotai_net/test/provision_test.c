// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_wifi_credentials.c::wifi_credentials
// Replaces legacy: tests/regression/test_wifi_manual_disconnect.py
// Replaces legacy: tests/xmini/test_platform_network.c::platform_network
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "wifi_policy.h"
#include "xiaotai/at.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_net_host.h"

static xiaotai_network_t wifi = {.desc = {
    .kind = XIAOTAI_NETWORK_WIFI,
    .provisioning = XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL | XIAOTAI_PROVISION_BLE,
}};
static const xiaotai_net_credentials_t old = {.ssid = "old", .password = "fixture-password"};
static const xiaotai_net_credentials_t fresh = {.ssid = "new", .password = "another-fixture"};
static const xiaotai_net_credentials_t short_key = {.ssid = "test-legacy", .password = "12345"};
static const char *const space = "nw5";
static _Atomic int64_t now_us;
static atomic_uint restarts;
static xiaotai_signal_t owner_barrier;
static bool observe_store;
static atomic_uint failure_notices, done_notices, early_notices, done_before_restart;
static xiaotai_signal_t owner_full, owner_release;
static char operations[24], fail_at;
static size_t used;

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&now_us); }

xiaotai_err_t __wrap_xiaotai_restart_request(const char *reason, uint32_t ticket);
xiaotai_err_t __wrap_xiaotai_restart_request(const char *reason, uint32_t ticket)
{
    assert(xiaotai_on_owner() && !strcmp(reason, "network") && ticket == 0);
    if (observe_store) atomic_store(&done_before_restart, atomic_load(&done_notices));
    atomic_fetch_add(&restarts, 1);
    return XIAOTAI_OK;
}

const xiaotai_network_t *xiaotai_hal_network(size_t index) { return index ? NULL : &wifi; }

static xiaotai_net_provision_status_t status(void)
{
    xiaotai_net_provision_status_t status;
    xiaotai_net_provision_status(&status);
    return status;
}

static void notify(void *ctx) { xiaotai_signal_give(ctx); }

static void settle(void)
{
    assert(xiaotai_run(notify, &owner_barrier) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_barrier, 2000));
    assert(xiaotai_net_host_sync(2000) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(2000) == XIAOTAI_OK);
    assert(xiaotai_run(notify, &owner_barrier) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_barrier, 2000));
}

static void advance(unsigned ms)
{
    atomic_fetch_add(&now_us, (int64_t)ms * 1000);
    settle();
}

static xiaotai_err_t operation(char step)
{
    assert(net_on_worker() && used + 1 < sizeof(operations));
    operations[used++] = step;
    operations[used] = '\0';
    return step == fail_at ? XIAOTAI_FAIL : XIAOTAI_OK;
}

static xiaotai_err_t server_start(uint32_t ipv4)
{
    const uint8_t wanted[] = {192, 168, 6, 1};
    assert(!memcmp(&ipv4, wanted, sizeof(wanted)) && status().methods == 0);
    return operation('S');
}
static xiaotai_err_t dns_start(uint32_t ipv4) { assert(ipv4 != 0); return operation('D'); }
static xiaotai_err_t dns_stop(void) { assert(status().methods == 0); return operation('d'); }
static xiaotai_err_t server_stop(void) { assert(status().methods == 0); return operation('s'); }
static xiaotai_err_t ble_start(void *ctx, const xiaotai_net_provision_status_t *snapshot)
{
    assert(!ctx && snapshot->reason && !strcmp(snapshot->ap_ssid, "Fixture-AB12"));
    return operation('B');
}
static xiaotai_err_t ble_stop(void *ctx) { assert(!ctx && status().methods == 0); return operation('b'); }
static const net_provision_portal_io_t portal = {server_start, server_stop, dns_start, dns_stop};
static const xiaotai_net_provision_method_t ble = {
    .method = XIAOTAI_PROVISION_BLE, .name = "test", .open = ble_start, .close = ble_stop,
};

static void provision_notice(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && event->size == sizeof(xiaotai_net_provision_status_t));
    if (!observe_store) return;
    const xiaotai_net_provision_status_t *snapshot = event->data;
    assert(event->args.value == (int32_t)snapshot->state);
    if (snapshot->state == XIAOTAI_NET_PROVISION_STORE_FAILED) {
        assert(snapshot->error == XIAOTAI_FAIL && !snapshot->restart_required);
        atomic_fetch_add(&failure_notices, 1);
    }
    if (snapshot->restart_required) {
        if (snapshot->state == XIAOTAI_NET_PROVISION_DONE) {
            assert(!snapshot->error && !snapshot->ssid[0]);
            atomic_fetch_add(&done_notices, 1);
        } else atomic_fetch_add(&early_notices, 1);
    }
}

static xiaotai_err_t init_observer(void)
{
    return xiaotai_event_subscribe(XIAOTAI_EVENT_NET_PROVISION, provision_notice, NULL);
}

static const xiaotai_module_t module = {
    .name = "net", .init = xiaotai_engine_net_init, .start = xiaotai_engine_net_start,
};
static const xiaotai_module_t watcher = {.name = "observer", .init = init_observer};
static const xiaotai_module_t *const modules[] = {&module, &watcher};

static void boot_with(const xiaotai_net_config_t *config, bool install)
{
    used = 0;
    operations[0] = '\0';
    atomic_store(&now_us, 1000000);
    atomic_store(&restarts, 0);
    xiaotai_net_host_set_mac(XIAOTAI_OK, (const uint8_t[]){1, 2, 3, 4, 0xAB, 0x12});
    assert(xiaotai_signal_init(&owner_barrier) == XIAOTAI_OK);
    assert(xiaotai_net_configure(config) == XIAOTAI_OK);
    assert(xiaotai_net_provision_method_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_net_provision_method_register(&ble) == XIAOTAI_OK);
    assert(xiaotai_net_provision_method_register(&ble) == XIAOTAI_ERR_INVALID_ARG);
    if (install) assert(net_provision_portal_register(&portal) == XIAOTAI_OK);
    const xiaotai_framework_config_t framework = {.modules = modules, .module_count = observe_store ? 2U : 1U};
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    settle();
    assert(xiaotai_net_provision_method_register(&ble) == XIAOTAI_ERR_INVALID_STATE);
}

static void boot(const xiaotai_net_config_t *config) { boot_with(config, true); }

static void stop(void)
{
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_net_host_reset();
    xiaotai_signal_deinit(&owner_barrier);
}

static xiaotai_net_config_t config(bool exclusive, bool unverified, bool keep)
{
    return (xiaotai_net_config_t){.space = space, .ap_prefix = "Fixture-",
        .provisioning = exclusive ? XIAOTAI_NET_PROVISIONING_EXCLUSIVE : XIAOTAI_NET_PROVISIONING_CONCURRENT,
        .layout = exclusive ? XIAOTAI_NET_STORE_PLAIN : XIAOTAI_NET_STORE_RECORD,
        .store_unverified = unverified, .keep_running = keep,
    };
}

static unsigned calls(xiaotai_net_host_call_t what, xiaotai_net_host_record_t *last)
{
    xiaotai_net_host_record_t records[XIAOTAI_NET_HOST_RECORDS_MAX];
    const size_t count = xiaotai_net_host_records(records, XIAOTAI_NET_HOST_RECORDS_MAX);
    unsigned found = 0;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].call == what) {
            assert(records[i].on_worker);
            ++found;
            if (last) *last = records[i];
        }
    }
    return found;
}

static uint32_t join(const xiaotai_net_credentials_t *want, unsigned count)
{
    xiaotai_net_host_record_t last;
    assert(calls(XIAOTAI_NET_HOST_JOIN, &last) == count && last.has_credentials && last.token);
    assert(!strcmp(want->ssid, last.credentials.ssid) && !strcmp(want->password, last.credentials.password));
    return last.token;
}

static void emit(xiaotai_net_driver_event_t event)
{
    assert(xiaotai_net_host_emit(&event) == XIAOTAI_OK);
    settle();
}

static void up(uint32_t token, const char *ssid)
{
    xiaotai_net_driver_event_t joined = {.kind = XIAOTAI_NET_DRIVER_JOINED, .token = token};
    memcpy(joined.ssid, ssid, strlen(ssid) + 1);
    emit(joined);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = token, .ipv4 = 42});
}

static void lost(uint32_t token, xiaotai_net_loss_t loss)
{
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_LOST, .token = token, .loss = loss});
}

static xiaotai_err_t submit(const xiaotai_net_credentials_t *credentials, uint32_t method)
{
    const xiaotai_net_submission_t submission = {.credentials = *credentials, .method = method};
    return xiaotai_net_provision_submit(&submission);
}

static void assert_stored(bool plain, const xiaotai_net_credentials_t *expected)
{
    uint8_t record[99] = {0};
    size_t size = sizeof(record);
    assert(xiaotai_kv_get_blob_strict(space, plain ? "wifi" : "credentials", record, &size, 2000) == XIAOTAI_OK);
    assert(size == (plain ? 98 : 99));
    assert(plain || record[0] == 1);
    assert(!memcmp(record + !plain, expected, sizeof(*expected)));
}

static void assert_unselected(void)
{
    assert(!xiaotai_net_has_saved_credentials() && !xiaotai_net_generation());
    assert(!status().restart_required && !atomic_load(&restarts));
}

static void xmini_wrong_ssid(const uint8_t *actual, size_t size)
{
    const xiaotai_net_config_t c = config(true, false, false);
    boot(&c);
    assert(xiaotai_net_provisioning_boot());
    assert(submit(&fresh, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_OK);
    advance(1);
    const uint32_t token = join(&fresh, 1);
    net_wifi_flow_t flow = {.join = token};
    xiaotai_net_driver_event_t event;
    if (net_wifi_flow_joined(&flow, actual, size, &event)) {
        emit(event);
        assert(net_wifi_flow_address(&flow, 42, NULL, &event));
        emit(event);
    } else assert(!net_wifi_flow_address(&flow, 42, NULL, &event));
    assert_unselected();
    stop();
}

static void xmini_credentials(void)
{
    const xiaotai_net_config_t c = config(true, false, false);
    const uint32_t method = XIAOTAI_PROVISION_PORTAL;
    wifi.desc.provisioning = method | XIAOTAI_PROVISION_SERIAL;
    xmini_wrong_ssid((const uint8_t *)"ne", 2);
    xmini_wrong_ssid((const uint8_t *)"new-2", 5);
    xmini_wrong_ssid((const uint8_t *)"new\0x", 5);

    assert(XIAOTAI_NET_VERIFY_TIMEOUT_MS == 30000);
    boot(&c);
    assert(xiaotai_net_provisioning_boot() && status().state == XIAOTAI_NET_PROVISION_OPEN);
    advance(30000);
    assert(status().state == XIAOTAI_NET_PROVISION_OPEN && !calls(XIAOTAI_NET_HOST_JOIN, NULL));
    assert_unselected();
    const int64_t deadline = atomic_load(&now_us) + INT64_C(30000001);
    assert(submit(&fresh, method) == XIAOTAI_OK);
    advance(1);
    const uint32_t expired = join(&fresh, 1);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = expired, .ipv4 = 42});
    atomic_store(&now_us, deadline - 1);
    settle();
    assert(status().state == XIAOTAI_NET_PROVISION_VERIFYING);
    assert_unselected();
    atomic_store(&now_us, deadline);
    settle();
    assert(status().state == XIAOTAI_NET_PROVISION_REJECTED && status().error == XIAOTAI_ERR_TIMEOUT);
    const bool still_disconnecting = xiaotai_net_provisioning_held();
    fprintf(stderr, "timed-out candidate: disconnects=%u held=%d\n",
            calls(XIAOTAI_NET_HOST_LEAVE, NULL), still_disconnecting);
    assert(calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 1 && still_disconnecting);
    up(expired, fresh.ssid);
    assert_unselected();
    lost(expired, XIAOTAI_NET_LOSS_REQUESTED);
    assert(status().state == XIAOTAI_NET_PROVISION_REJECTED && !xiaotai_net_provisioning_held());

    const int64_t retry_deadline = atomic_load(&now_us) + INT64_C(30000001);
    assert(submit(&fresh, method) == XIAOTAI_OK);
    advance(1);
    const uint32_t retry = join(&fresh, 2);
    assert(retry != expired);
    up(expired, fresh.ssid);
    assert(status().state == XIAOTAI_NET_PROVISION_VERIFYING);
    assert_unselected();
    atomic_store(&now_us, retry_deadline - 1);
    settle();
    up(retry, fresh.ssid);
    assert(status().state == XIAOTAI_NET_PROVISION_DONE && status().restart_required);
    assert(atomic_load(&restarts) == 1);
    assert_stored(true, &fresh);
    stop();

    xiaotai_kv_host_clear();
    boot(&c);
    assert(xiaotai_net_provisioning_boot());
    assert(submit(&fresh, method) == XIAOTAI_OK);
    advance(1);
    (void)join(&fresh, 1);
    advance(30000);
    assert(status().state == XIAOTAI_NET_PROVISION_REJECTED && xiaotai_net_provisioning_held());
    advance(2000);
    assert(status().state == XIAOTAI_NET_PROVISION_REJECTED && !xiaotai_net_provisioning_held());
    assert_unselected();
    stop();
}

static void assert_open(void)
{
    assert(status().state == XIAOTAI_NET_PROVISION_OPEN);
    assert(status().methods == wifi.desc.provisioning && !strcmp(status().ap_ssid, "Fixture-AB12"));
    assert(!strcmp(status().url, "http://192.168.6.1") && !status().ap_password[0]);
    xiaotai_net_host_record_t ap;
    assert(calls(XIAOTAI_NET_HOST_AP_START, &ap) == 1 && ap.ap.channel == 1 && ap.ap.max_clients == 4);
    assert(!strcmp(ap.ap.ssid, status().ap_ssid));
}

static void lifecycle(void)
{
    const xiaotai_net_config_t c = config(false, false, true);
    for (unsigned i = 0; i < 3; ++i) {
        fail_at = "SDB"[i];
        boot(&c);
        assert(status().state == XIAOTAI_NET_PROVISION_CLOSED && status().error == XIAOTAI_FAIL);
        const char *wanted[] = {"Ss", "SDs", "SDBds"};
        assert(!strcmp(operations, wanted[i]) && calls(XIAOTAI_NET_HOST_AP_STOP, NULL) == 1);
        stop();
    }
    fail_at = 0;
    /* Startup may fail after acquiring shared driver resources. If the first
     * cleanup fails as well, the next open must release it before trying again. */
    const xiaotai_net_host_step_t failed = {.result = XIAOTAI_FAIL};
    assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_AP_START, &failed) == XIAOTAI_OK);
    assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_AP_STOP, &failed) == XIAOTAI_OK);
    boot(&c);
    assert(status().state == XIAOTAI_NET_PROVISION_CLOSED && status().error == XIAOTAI_FAIL);
    assert(!status().methods && !used && calls(XIAOTAI_NET_HOST_AP_STOP, NULL) == 1);
    assert(xiaotai_net_reprovision(false) == XIAOTAI_OK);
    settle();
    const xiaotai_net_provision_status_t reopened = status();
    assert(reopened.state == XIAOTAI_NET_PROVISION_OPEN && (reopened.methods & XIAOTAI_PROVISION_PORTAL));
    assert(!strcmp(operations, "SDB"));
    const xiaotai_net_host_call_t wanted_ap[] = {XIAOTAI_NET_HOST_AP_START, XIAOTAI_NET_HOST_AP_STOP,
                                                XIAOTAI_NET_HOST_AP_STOP, XIAOTAI_NET_HOST_AP_START};
    xiaotai_net_host_record_t records[12];
    const size_t count = xiaotai_net_host_records(records, sizeof(records) / sizeof(records[0]));
    assert(count < sizeof(records) / sizeof(records[0]));
    size_t ap_index = 0;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_net_host_call_t call = records[i].call;
        if (call != XIAOTAI_NET_HOST_AP_START && call != XIAOTAI_NET_HOST_AP_STOP) continue;
        assert(records[i].on_worker && ap_index < sizeof(wanted_ap) / sizeof(wanted_ap[0]) &&
               call == wanted_ap[ap_index++]);
    }
    assert(ap_index == sizeof(wanted_ap) / sizeof(wanted_ap[0]));
    stop();
    boot_with(&c, false);
    assert(status().state == XIAOTAI_NET_PROVISION_CLOSED && status().error == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(calls(XIAOTAI_NET_HOST_AP_START, NULL) == 0);
    stop();
    boot(&c);
    assert_open();
    assert(!strcmp(operations, "SDB"));
    assert(xiaotai_net_provision_close() == XIAOTAI_OK);
    settle();
    assert(!strcmp(operations, "SDBbds"));
    assert(!status().methods && status().state == XIAOTAI_NET_PROVISION_CLOSED);
    assert(calls(XIAOTAI_NET_HOST_AP_STOP, NULL) == 1);
    assert(submit(&fresh, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_net_reprovision(false) == XIAOTAI_OK);
    settle();
    assert(status().state == XIAOTAI_NET_PROVISION_OPEN && status().reason == XIAOTAI_NET_PROVISION_REASON_REQUESTED);
    stop();
}

static void combinations(void)
{
    for (unsigned bits = 0; bits < 4; ++bits) {
        xiaotai_kv_host_clear();
        const bool exclusive = bits & 1, unverified = bits & 2;
        xiaotai_net_config_t c = config(exclusive, unverified, !exclusive && !unverified);
        boot(&c);
        assert_open();
        assert(xiaotai_net_provisioning_boot() == exclusive && status().exclusive == exclusive);
        if (exclusive) assert(xiaotai_net_provision_close() == XIAOTAI_ERR_NOT_FOUND);
        assert(submit(&fresh, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_OK);
        advance(1);
        if (unverified) assert(calls(XIAOTAI_NET_HOST_JOIN, NULL) == 0);
        else {
            assert(status().state == XIAOTAI_NET_PROVISION_VERIFYING);
            assert(submit(&old, XIAOTAI_PROVISION_SERIAL) == XIAOTAI_ERR_BUSY);
            uint32_t ticket = 0;
            assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE && !ticket);
            assert(xiaotai_net_disconnect() == XIAOTAI_ERR_INVALID_STATE);
            assert(xiaotai_net_provision_close() == XIAOTAI_ERR_INVALID_STATE);
            up(join(&fresh, 1), fresh.ssid);
        }
        assert(status().state == XIAOTAI_NET_PROVISION_DONE);
        const bool restarted = exclusive || unverified;
        assert(status().restart_required == restarted && atomic_load(&restarts) == (unsigned)restarted);
        assert_stored(exclusive, &fresh);
        if (!restarted) {
            assert(!status().methods && !strcmp(operations, "SDBbds"));
            xiaotai_net_status_t link;
            xiaotai_net_status(&link);
            assert(link.state == XIAOTAI_NET_UP);
        } else {
            assert(submit(&old, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_ERR_BUSY);
        }
        stop();
    }
}

static void legacy(void)
{
    xiaotai_net_config_t c = config(true, false, false);
    c.unverified_methods = XIAOTAI_PROVISION_SERIAL;
    c.serial_short_passwords = true;
    boot(&c);
    assert_open();
    assert(submit(&short_key, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_net_scan() == XIAOTAI_OK);
    settle();
    assert(calls(XIAOTAI_NET_HOST_SCAN, NULL) == 1);
    assert(submit(&fresh, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_ERR_BUSY);
    assert(submit(&short_key, XIAOTAI_PROVISION_SERIAL) == XIAOTAI_OK);
    advance(1);
    assert(status().restart_required && atomic_load(&restarts) == 1 && calls(XIAOTAI_NET_HOST_JOIN, NULL) == 0);
    assert_stored(true, &short_key);
    stop();

    boot(&c);
    assert(!status().methods && !xiaotai_net_provisioning_boot());
    assert(submit(&fresh, XIAOTAI_PROVISION_SERIAL) == XIAOTAI_ERR_INVALID_STATE);
    const uint32_t old_token = join(&short_key, 1);
    up(old_token, short_key.ssid);
    assert(xiaotai_net_reprovision(false) == XIAOTAI_OK);
    settle();
    assert(status().restart_required && atomic_load(&restarts) == 1);
    uint8_t marker = 0;
    assert(xiaotai_kv_get_u8(space, "wifi_provision", &marker, 2000) == XIAOTAI_OK && marker == 1);
    assert_stored(true, &short_key);
    stop();

    boot(&c);
    assert_open();
    assert(xiaotai_net_provisioning_boot() && calls(XIAOTAI_NET_HOST_JOIN, NULL) == 0);
    assert(xiaotai_net_provision_close() == XIAOTAI_OK);
    settle();
    assert(status().restart_required && atomic_load(&restarts) == 1);
    assert(xiaotai_kv_get_u8(space, "wifi_provision", &marker, 2000) == XIAOTAI_ERR_NOT_FOUND);
    assert_stored(true, &short_key);
    stop();

    boot(&c);
    assert(!xiaotai_net_provisioning_boot());
    assert(xiaotai_net_reprovision(true) == XIAOTAI_OK);
    settle();
    assert(xiaotai_kv_get_u8(space, "wifi_provision", &marker, 2000) == XIAOTAI_OK && marker == 1);
    uint8_t blob[98];
    size_t size = sizeof(blob);
    assert(xiaotai_kv_get_blob_strict(space, "wifi", blob, &size, 2000) == XIAOTAI_ERR_NOT_FOUND);
    stop();
}

static void recovery(void)
{
    xiaotai_net_config_t c = config(false, false, true);
    boot(&c);
    assert_open();
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK);
    assert(submit(&fresh, XIAOTAI_PROVISION_SERIAL) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_net_maintenance_end(ticket);
    assert(submit(&fresh, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_OK);
    advance(1);
    for (unsigned i = 0; i <= XIAOTAI_NET_VERIFY_RETRIES; ++i) {
        lost(join(&fresh, i + 1), XIAOTAI_NET_LOSS_NOT_FOUND);
        advance(1);
    }
    assert(status().state == XIAOTAI_NET_PROVISION_REJECTED && status().error == XIAOTAI_ERR_NOT_FOUND);
    assert(status().methods && !status().restart_required);
    assert(submit(&fresh, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_OK);
    advance(1);
    advance(XIAOTAI_NET_VERIFY_TIMEOUT_MS);
    assert(status().state == XIAOTAI_NET_PROVISION_REJECTED && status().error == XIAOTAI_ERR_TIMEOUT);
    uint32_t abandoned = join(&fresh, XIAOTAI_NET_VERIFY_RETRIES + 2);
    up(abandoned, fresh.ssid);
    lost(abandoned, XIAOTAI_NET_LOSS_REQUESTED);
    assert(!xiaotai_net_has_saved_credentials() && !status().restart_required);

    assert(submit(&fresh, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_OK);
    advance(1);
    uint32_t mismatched = join(&fresh, XIAOTAI_NET_VERIFY_RETRIES + 3);
    up(mismatched, old.ssid);
    lost(mismatched, XIAOTAI_NET_LOSS_REQUESTED);
    assert(status().state == XIAOTAI_NET_PROVISION_REJECTED && status().error == XIAOTAI_ERR_NOT_FOUND);
    assert(!xiaotai_net_has_saved_credentials());

    assert(submit(&fresh, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_OK);
    advance(1);
    xiaotai_kv_host_fail_key("credentials", XIAOTAI_FAIL);
    up(join(&fresh, XIAOTAI_NET_VERIFY_RETRIES + 4), fresh.ssid);
    assert(status().state == XIAOTAI_NET_PROVISION_STORE_FAILED && !atomic_load(&restarts));
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_net_provision_close() == XIAOTAI_ERR_INVALID_STATE);
    assert(submit(&old, XIAOTAI_PROVISION_SERIAL) == XIAOTAI_ERR_BUSY);
    xiaotai_kv_host_fail_key("credentials", XIAOTAI_OK);
    assert(xiaotai_net_provision_store_retry() == XIAOTAI_OK);
    advance(1);
    assert(status().state == XIAOTAI_NET_PROVISION_DONE && !atomic_load(&restarts));
    assert_stored(false, &fresh);
    stop();

    /* Verification of a candidate leaves the old association but preserves the
     * confirmed stored record until another candidate obtains its address. */
    xiaotai_kv_host_clear();
    uint8_t old_record[99] = {1};
    memcpy(old_record + 1, &old, sizeof(old));
    assert(xiaotai_kv_set(space, "credentials", old_record, sizeof(old_record), 2000) == XIAOTAI_OK);
    c.keep_running = false;
    boot(&c);
    uint32_t prior = join(&old, 1);
    up(prior, old.ssid);
    assert(!status().methods);
    assert(submit(&fresh, XIAOTAI_PROVISION_SERIAL) == XIAOTAI_OK);
    advance(1);
    assert(calls(XIAOTAI_NET_HOST_JOIN, NULL) == 1 && calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 1);
    lost(prior, XIAOTAI_NET_LOSS_REQUESTED);
    uint32_t rejected = join(&fresh, 2);
    up(rejected, old.ssid);
    lost(rejected, XIAOTAI_NET_LOSS_REQUESTED);
    assert(status().state == XIAOTAI_NET_PROVISION_REJECTED);
    prior = join(&old, 3);
    up(prior, old.ssid);
    assert_stored(false, &old);
    assert(submit(&fresh, XIAOTAI_PROVISION_SERIAL) == XIAOTAI_OK);
    advance(1);
    lost(prior, XIAOTAI_NET_LOSS_REQUESTED);
    up(join(&fresh, 4), fresh.ssid);
    assert(status().state == XIAOTAI_NET_PROVISION_DONE && status().restart_required);
    assert(atomic_load(&restarts) == 1 && !xiaotai_net_generation());
    assert_stored(false, &fresh);
    stop();
}

static void saved(void)
{
    struct {
        uint8_t version, count;
        xiaotai_net_credentials_t entries[5];
    } history = {.version = 1, .count = 1, .entries = {old}};
    assert(sizeof(history) == 492 && xiaotai_kv_set(space, "known", &history, sizeof(history), 2000) == XIAOTAI_OK);
    xiaotai_net_config_t c = config(false, false, true);
    boot(&c);
    xiaotai_net_submission_t request = {.method = XIAOTAI_PROVISION_PORTAL, .use_saved = true};
    strcpy(request.credentials.ssid, "missing");
    assert(xiaotai_net_provision_submit(&request) == XIAOTAI_ERR_NOT_FOUND);
    strcpy(request.credentials.ssid, old.ssid);
    strcpy(request.credentials.password, old.password);
    assert(xiaotai_net_provision_submit(&request) == XIAOTAI_ERR_INVALID_ARG);
    memset(request.credentials.password, 0, sizeof(request.credentials.password));
    assert(xiaotai_net_provision_submit(&request) == XIAOTAI_OK);
    advance(1);
    up(join(&old, 1), old.ssid);
    assert_stored(false, &old);
    stop();
}

static void manual_disconnect(void)
{
    uint8_t old_record[99] = {1};
    memcpy(old_record + 1, &old, sizeof(old));
    assert(xiaotai_kv_set(space, "credentials", old_record, sizeof(old_record), 2000) == XIAOTAI_OK);
    const xiaotai_net_config_t c = config(false, false, true);
    boot(&c);
    const uint32_t token = join(&old, 1);
    up(token, old.ssid);
    assert(xiaotai_net_generation() && !status().methods);

    assert(xiaotai_net_disconnect() == XIAOTAI_OK);
    settle();
    lost(token, XIAOTAI_NET_LOSS_REQUESTED);
    assert_open();
    assert(status().reason == XIAOTAI_NET_PROVISION_REASON_DISCONNECTED && !xiaotai_net_generation());
    up(token, old.ssid);
    assert(!xiaotai_net_generation());
    assert(xiaotai_net_disconnect() == XIAOTAI_OK);
    advance(60000);
    assert_open();
    assert(calls(XIAOTAI_NET_HOST_JOIN, NULL) == 1 && !atomic_load(&restarts));
    assert(xiaotai_net_has_saved_credentials());
    assert_stored(false, &old);
    stop();

    boot(&c); /* Reset the owner and driver but keep the old storage bytes. */
    const uint32_t rebooted = join(&old, 1);
    assert(!xiaotai_net_provisioning_boot() && !status().methods);
    up(rebooted, old.ssid);
    assert(xiaotai_net_generation() && !status().methods);
    assert_stored(false, &old);
    puts("manual disconnect: one portal, late address ignored, old credentials join after cold boot");
    stop();
}

static unsigned operation_count(char wanted)
{
    unsigned count = 0;
    for (size_t i = 0; i < used; ++i) count += operations[i] == wanted;
    return count;
}

/* An offline replay of a current, repeated GOT_IP after one close failure.
 * Run explicitly until the policy for an automatic retry is decided. */
static bool legacy_portal_close_retry(void)
{
    uint8_t old_record[99] = {1};
    memcpy(old_record + 1, &old, sizeof(old));
    assert(xiaotai_kv_set(space, "credentials", old_record, sizeof(old_record), 2000) == XIAOTAI_OK);
    const xiaotai_net_config_t c = config(false, false, true);
    boot(&c);
    xiaotai_net_status_t link;
    for (unsigned i = 0; i < XIAOTAI_NET_FAILURES_NOTICE; ++i) {
        lost(join(&old, i + 1U), XIAOTAI_NET_LOSS_NOT_FOUND);
        xiaotai_net_status(&link);
        assert(link.state == XIAOTAI_NET_WAITING && link.retry_in_ms);
        advance(link.retry_in_ms);
    }
    assert_open();
    assert(status().reason == XIAOTAI_NET_PROVISION_REASON_FAILING);
    const uint32_t token = join(&old, XIAOTAI_NET_FAILURES_NOTICE + 1U);
    fail_at = 's';
    up(token, old.ssid);
    xiaotai_net_status(&link);
    assert(link.state == XIAOTAI_NET_UP && link.generation && !status().methods);
    assert(operation_count('s') == 1 && !atomic_load(&restarts));
    const xiaotai_err_t close = status().error, control = link.control_error;
    const uint32_t generation = link.generation;
    assert_stored(false, &old);

    fail_at = '\0';
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = token, .ipv4 = 42});
    const unsigned cleans = operation_count('s');
    const unsigned ap_stops = calls(XIAOTAI_NET_HOST_AP_STOP, NULL);
    xiaotai_net_status(&link);
    assert(link.state == XIAOTAI_NET_UP && link.generation == generation && !status().methods);
    if (cleans < 2) {
        assert(xiaotai_net_provision_close() == XIAOTAI_OK);
        settle();
        assert(operation_count('s') == 2);
        assert(calls(XIAOTAI_NET_HOST_AP_STOP, NULL) > ap_stops);
    }
    assert_stored(false, &old);
    assert(!atomic_load(&restarts));
    printf("portal close error: provision=%d control=%d, same address close attempts=%u AP stops=%u, final closes=%u\n",
           close, control, cleans, ap_stops, operation_count('s'));
    stop();
    return close == XIAOTAI_FAIL && control == XIAOTAI_FAIL && cleans == 2;
}

static void noop(void *ctx) { (void)ctx; }

static void hold_owner(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < XIAOTAI_FRAMEWORK_QUEUE_DEPTH; ++i) assert(xiaotai_run(noop, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(noop, NULL) == XIAOTAI_ERR_NO_MEM);
    xiaotai_signal_give(&owner_full);
    assert(xiaotai_signal_take(&owner_release, 2000));
}

static void drain_owner(void)
{
    xiaotai_signal_give(&owner_release);
    xiaotai_err_t result = XIAOTAI_ERR_NO_MEM;
    for (unsigned i = 0; i < 1000 && result == XIAOTAI_ERR_NO_MEM; ++i) {
        result = xiaotai_run(notify, &owner_barrier);
        if (result == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1);
    }
    assert(result == XIAOTAI_OK && xiaotai_signal_take(&owner_barrier, 2000));
}

static void store_notice(bool full)
{
    observe_store = true;
    const xiaotai_net_config_t cfg = config(true, true, false);
    boot(&cfg);
    assert(submit(&fresh, XIAOTAI_PROVISION_SERIAL) == XIAOTAI_OK);
    if (full) {
        assert(xiaotai_signal_init(&owner_full) == XIAOTAI_OK);
        assert(xiaotai_signal_init(&owner_release) == XIAOTAI_OK);
        assert(xiaotai_run(hold_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&owner_full, 2000));
        xiaotai_kv_host_fail_key("wifi", XIAOTAI_FAIL);
        atomic_fetch_add(&now_us, 1000);
        assert(xiaotai_net_host_sync(2000) == XIAOTAI_OK && xiaotai_net_host_sync(2000) == XIAOTAI_OK);
        assert(status().state == XIAOTAI_NET_PROVISION_STORE_FAILED && !xiaotai_net_has_saved_credentials());
        assert(!atomic_load(&failure_notices) && !atomic_load(&restarts));
        drain_owner();
        advance(200);
        assert(atomic_load(&failure_notices) == 1 && !atomic_load(&restarts));
        advance(200);
        assert(atomic_load(&failure_notices) == 1);

        xiaotai_kv_host_fail_key("wifi", XIAOTAI_OK);
        assert(xiaotai_net_provision_store_retry() == XIAOTAI_OK);
        assert(xiaotai_run(hold_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&owner_full, 2000));
        atomic_fetch_add(&now_us, 1000);
        assert(xiaotai_net_host_sync(2000) == XIAOTAI_OK && xiaotai_net_host_sync(2000) == XIAOTAI_OK);
        assert_stored(true, &fresh);
        assert(status().state == XIAOTAI_NET_PROVISION_DONE && status().restart_required);
        assert(!atomic_load(&done_notices) && !atomic_load(&restarts));
        drain_owner();
        advance(200);
        xiaotai_signal_deinit(&owner_release);
        xiaotai_signal_deinit(&owner_full);
    } else advance(1);
    assert_stored(true, &fresh);
    assert(atomic_load(&early_notices) == 0 && atomic_load(&done_notices) == 1);
    assert(atomic_load(&restarts) == 1 && atomic_load(&done_before_restart) == 1);
    stop();
    observe_store = false;
}

/* The deployed Xmini identity shares the Wi-Fi namespace but not its keys. */
static const struct { char id[65], key[257]; } xmini_identity = {
    .id = "test-device", .key = "not-a-real-key",
};

static void expect_xmini_storage(const xiaotai_net_credentials_t *wanted, bool marked)
{
    xiaotai_net_credentials_t credentials;
    size_t size = sizeof(credentials);
    assert(xiaotai_kv_get_blob_strict("xmini", "wifi", &credentials, &size, 2000) == XIAOTAI_OK);
    assert(size == 98U && !memcmp(&credentials, wanted, size));
    uint8_t identity[sizeof(xmini_identity)];
    size = sizeof(identity);
    assert(xiaotai_kv_get_blob_strict("xmini", "identity", identity, &size, 2000) == XIAOTAI_OK);
    assert(size == 322U && !memcmp(identity, &xmini_identity, size));
    uint8_t marker = 0;
    const xiaotai_err_t found = xiaotai_kv_get_u8("xmini", "wifi_provision", &marker, 2000);
    assert(marked ? found == XIAOTAI_OK && marker == 1U : found == XIAOTAI_ERR_NOT_FOUND);
}

static void xmini_portal_storage(void)
{
    const xiaotai_net_credentials_t previous = {.ssid = "test-old", .password = "test-password"};
    const xiaotai_net_credentials_t submitted = {.ssid = "test-new", .password = "test-new-password"};
    assert(xiaotai_kv_set("xmini", "wifi", &previous, sizeof(previous), 2000) == XIAOTAI_OK);
    assert(xiaotai_kv_set("xmini", "identity", &xmini_identity, sizeof(xmini_identity), 2000) == XIAOTAI_OK);
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    xiaotai_net_config_t c = config(true, false, false);
    c.space = "xmini";
    boot(&c);
    assert(!xiaotai_net_provisioning_boot());
    up(join(&previous, 1), previous.ssid);
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    assert(xiaotai_net_reprovision(false) == XIAOTAI_ERR_INVALID_STATE);
    settle();
    xiaotai_kv_host_counts(&after);
    assert(after.writes == before.writes && after.commits == before.commits && !atomic_load(&restarts));
    xiaotai_net_maintenance_end(ticket);
    expect_xmini_storage(&previous, false);

    const xiaotai_err_t marker_errors[] = {XIAOTAI_FAIL, XIAOTAI_ERR_NO_MEM};
    for (size_t i = 0; i < sizeof(marker_errors) / sizeof(marker_errors[0]); ++i) {
        if (i == 0) xiaotai_kv_host_fail_key("wifi_provision", marker_errors[i]);
        else xiaotai_kv_host_fail(marker_errors[i]);
        assert(xiaotai_net_reprovision(false) == XIAOTAI_OK);
        settle();
        xiaotai_net_status_t link;
        xiaotai_net_status(&link);
        assert(!link.control_pending && link.control_error == marker_errors[i]);
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        xiaotai_kv_host_fail(XIAOTAI_OK);
        assert(!status().restart_required && !atomic_load(&restarts));
        expect_xmini_storage(&previous, false);
    }
    assert(xiaotai_net_reprovision(false) == XIAOTAI_OK);
    settle();
    expect_xmini_storage(&previous, true);
    assert(status().restart_required && atomic_load(&restarts) == 1U);
    stop();

    observe_store = true;
    boot(&c);
    assert_open();
    assert(xiaotai_net_provisioning_boot() && !calls(XIAOTAI_NET_HOST_JOIN, NULL));
    assert(submit(&submitted, XIAOTAI_PROVISION_PORTAL) == XIAOTAI_OK);
    advance(1);
    xiaotai_kv_host_fail_key("wifi", XIAOTAI_FAIL);
    up(join(&submitted, 1), submitted.ssid);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    assert(status().state == XIAOTAI_NET_PROVISION_STORE_FAILED && !status().restart_required &&
           !atomic_load(&restarts) && atomic_load(&failure_notices) == 1U);
    expect_xmini_storage(&previous, true);

    /* A later erase failure is still an unsaved submission even if the
     * preceding credential write reached the backend. Do not assume rollback. */
    xiaotai_kv_host_fail_key("wifi_provision", XIAOTAI_FAIL);
    assert(xiaotai_net_provision_store_retry() == XIAOTAI_OK);
    advance(1);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    assert(status().state == XIAOTAI_NET_PROVISION_STORE_FAILED && !status().restart_required &&
           !atomic_load(&restarts) && !atomic_load(&done_notices) && atomic_load(&failure_notices) == 2U);
    expect_xmini_storage(&submitted, true);

    assert(xiaotai_net_provision_store_retry() == XIAOTAI_OK);
    advance(1);
    expect_xmini_storage(&submitted, false);
    assert(status().state == XIAOTAI_NET_PROVISION_DONE && status().restart_required &&
           atomic_load(&restarts) == 1U && atomic_load(&done_before_restart) == 1U);
    stop();
    observe_store = false;

    boot(&c);
    assert(!xiaotai_net_provisioning_boot());
    up(join(&submitted, 1), submitted.ssid);
    expect_xmini_storage(&submitted, false);
    stop();
}

int main(int argc, char **argv)
{
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    const int64_t outstanding = xiaotai_mem_host_outstanding();
    assert(argc <= 2);
    if (argc == 1) lifecycle();
    else if (!strcmp(argv[1], "combinations")) combinations();
    else if (!strcmp(argv[1], "legacy")) legacy();
    else if (!strcmp(argv[1], "recovery")) recovery();
    else if (!strcmp(argv[1], "saved")) saved();
    else if (!strcmp(argv[1], "manual_disconnect")) manual_disconnect();
    else if (!strcmp(argv[1], "legacy_portal_close_retry")) return legacy_portal_close_retry() ? 0 : 1;
    else if (!strcmp(argv[1], "store_order")) store_notice(false);
    else if (!strcmp(argv[1], "store_notice")) store_notice(true);
    else if (!strcmp(argv[1], "xmini_credentials")) xmini_credentials();
    else if (!strcmp(argv[1], "xmini_portal_storage")) xmini_portal_storage();
    else assert(false);
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    assert(xiaotai_mem_host_outstanding() == outstanding);
    return 0;
}
