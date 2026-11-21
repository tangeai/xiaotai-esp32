// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_captive_portal_contract.py
// Replaces legacy: tests/regression/test_wifi_portal.py
// Replaces legacy: tests/regression/test_native_wifi_maintenance.py
// Replaces legacy: tests/regression/test_wifi_credentials.py
// Replaces legacy: tests/regression/test_wifi_manual_disconnect.py
#include "internal.h"
#include "net_platform.h"
#include "portal.h"
#include "xiaotai/at.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_net_host.h"
#include "xiaotai_os_host.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define WAIT_MS 2000U

struct net_exchange {
    const char *request;
    size_t size, consumed, received, chunks;
    unsigned receive_timeouts, fail_chunk;
    bool late, close_on_read;
    int code;
    bool ended, no_store, nosniff, redirected;
    char content_type[64], response[12000];
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wait = PTHREAD_COND_INITIALIZER;
static net_request_fn_t s_handler;
static void *s_context;
static unsigned s_inflight;
static xiaotai_signal_t s_stop_entered, s_owner, s_held, s_release, s_submit_queued;
static atomic_uint s_restarts;
static atomic_uint s_active_checks;
static atomic_bool s_observe_submit;
static bool (*s_real_active)(uint32_t);
static const xiaotai_network_t wifi = {.desc = {
    .kind = XIAOTAI_NETWORK_WIFI, .provisioning = XIAOTAI_PROVISION_PORTAL,
}};

const xiaotai_network_t *xiaotai_hal_network(size_t index) { return index ? NULL : &wifi; }

xiaotai_err_t __wrap_xiaotai_restart_request(const char *reason, uint32_t ticket);
xiaotai_err_t __wrap_xiaotai_restart_request(const char *reason, uint32_t ticket)
{
    assert(xiaotai_on_owner() && !strcmp(reason, "network") && ticket == 0);
    atomic_fetch_add(&s_restarts, 1);
    return XIAOTAI_OK;
}

xiaotai_err_t __real_net_call_cancellable(xiaotai_err_t (*fn)(net_call_t *call), uint32_t token,
                                         bool (*active)(uint32_t), const void *in, size_t in_size,
                                         void *out, size_t out_capacity, uint32_t timeout_ms);
xiaotai_err_t __wrap_net_call_cancellable(xiaotai_err_t (*fn)(net_call_t *call), uint32_t token,
                                         bool (*active)(uint32_t), const void *in, size_t in_size,
                                         void *out, size_t out_capacity, uint32_t timeout_ms);

static bool observe_active(uint32_t token)
{
    /* Slot acquisition and its immediate recheck are first. The third
     * check occurs in the result wait, after the message was enqueued. */
    if (atomic_fetch_add(&s_active_checks, 1) == 2U) xiaotai_signal_give(&s_submit_queued);
    return s_real_active(token);
}

xiaotai_err_t __wrap_net_call_cancellable(xiaotai_err_t (*fn)(net_call_t *call), uint32_t token,
                                         bool (*active)(uint32_t), const void *in, size_t in_size,
                                         void *out, size_t out_capacity, uint32_t timeout_ms)
{
    if (atomic_load(&s_observe_submit)) {
        s_real_active = active;
        active = observe_active;
    }
    return __real_net_call_cancellable(fn, token, active, in, in_size, out, out_capacity, timeout_ms);
}

xiaotai_err_t net_platform_server_start(uint32_t ap_ipv4, uint32_t stack_bytes,
                                        net_request_fn_t handler, void *ctx)
{
    const uint8_t ap[] = {192, 168, 6, 1};
    assert(net_on_worker() && !memcmp(&ap_ipv4, ap, sizeof(ap)) && stack_bytes == XIAOTAI_NET_PORTAL_STACK_BYTES);
    pthread_mutex_lock(&s_lock);
    assert(!s_handler && !s_inflight && handler);
    s_handler = handler;
    s_context = ctx;
    pthread_mutex_unlock(&s_lock);
    return XIAOTAI_OK;
}

xiaotai_err_t net_platform_server_stop(void)
{
    assert(net_on_worker());
    pthread_mutex_lock(&s_lock);
    s_handler = NULL;
    if (s_inflight) xiaotai_signal_give(&s_stop_entered);
    while (s_inflight) pthread_cond_wait(&s_wait, &s_lock);
    pthread_mutex_unlock(&s_lock);
    return XIAOTAI_OK;
}

xiaotai_err_t net_platform_body_read(net_exchange_t *x, uint8_t *data, size_t capacity, size_t *size)
{
    assert(!net_on_worker() && !x->code && capacity);
    ++x->received;
    *size = 0;
    if (x->close_on_read) {
        x->close_on_read = false;
        assert(xiaotai_net_provision_close() == XIAOTAI_OK);
        assert(xiaotai_signal_take(&s_stop_entered, WAIT_MS));
    }
    if (x->late) {
        x->late = false;
        xiaotai_os_host_advance_us(5000000);
    }
    if (x->receive_timeouts) {
        --x->receive_timeouts;
        return XIAOTAI_ERR_TIMEOUT;
    }
    if (x->consumed < x->size) {
        data[0] = (uint8_t)x->request[x->consumed++];
        *size = 1;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t net_platform_reply_begin(net_exchange_t *x, int status, const char *content_type,
                                       const char *const *headers, size_t count)
{
    assert(!net_on_worker() && !x->code);
    x->code = status;
    (void)snprintf(x->content_type, sizeof(x->content_type), "%s", content_type);
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(headers[i], "Cache-Control: no-store")) x->no_store = true;
        else if (!strcmp(headers[i], "X-Content-Type-Options: nosniff")) x->nosniff = true;
        else if (!strcmp(headers[i], "Location: " XIAOTAI_NET_AP_URL)) x->redirected = true;
        else assert(false);
    }
    return XIAOTAI_OK;
}

xiaotai_err_t net_platform_reply_chunk(net_exchange_t *x, const void *body, size_t size)
{
    assert(x->code && !x->ended && (!x->fail_chunk || x->chunks < x->fail_chunk));
    if (++x->chunks == x->fail_chunk) return XIAOTAI_FAIL;
    const size_t length = strlen(x->response);
    assert(length + size < sizeof(x->response));
    memcpy(x->response + length, body, size);
    x->response[length + size] = '\0';
    return XIAOTAI_OK;
}

xiaotai_err_t net_platform_reply_end(net_exchange_t *x)
{
    assert(x->code && !x->ended && (!x->fail_chunk || x->chunks < x->fail_chunk));
    x->ended = true;
    return XIAOTAI_OK;
}

static void run(net_exchange_t *x, const net_request_t *request)
{
    pthread_mutex_lock(&s_lock);
    assert(s_handler && !s_inflight);
    net_request_fn_t handler = s_handler;
    void *ctx = s_context;
    ++s_inflight;
    pthread_mutex_unlock(&s_lock);
    handler(request, x, ctx);
    pthread_mutex_lock(&s_lock);
    --s_inflight;
    pthread_cond_broadcast(&s_wait);
    pthread_mutex_unlock(&s_lock);
}

static void checked(net_exchange_t *x, const char *verb, const char *path, const char *origin,
                    bool ap, size_t length, int status, const char *body)
{
    const net_request_t request = {.method = verb, .path = path, .origin = origin, .local_is_ap = ap,
                                    .content_length = length};
    run(x, &request);
    assert(x->no_store && x->nosniff && x->ended && x->code == status);
    if (body) assert(!strcmp(x->response, body));
}

static void get(const char *path, const char *contains)
{
    net_exchange_t x = {0};
    checked(&x, "GET", path, NULL, true, 0, 200, NULL);
    assert(!strcmp(x.content_type, "application/json") && strstr(x.response, contains));
}

static void post(const char *payload, int status, const char *body)
{
    net_exchange_t x = {.request = payload, .size = strlen(payload)};
    checked(&x, "POST", "/api/wifi", XIAOTAI_NET_AP_URL, true, x.size, status, body);
}

static void notify(void *ctx) { xiaotai_signal_give(ctx); }

static void sync(void)
{
    assert(xiaotai_run(notify, &s_owner) == XIAOTAI_OK && xiaotai_signal_take(&s_owner, WAIT_MS));
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_run(notify, &s_owner) == XIAOTAI_OK && xiaotai_signal_take(&s_owner, WAIT_MS));
}

static xiaotai_err_t dns_start(uint32_t ipv4) { return ipv4 && net_on_worker() ? XIAOTAI_OK : XIAOTAI_FAIL; }
static xiaotai_err_t dns_stop(void) { return net_on_worker() ? XIAOTAI_OK : XIAOTAI_FAIL; }

static void boot(const char *space, bool unverified)
{
    static const net_provision_portal_io_t portal = {net_portal_start, net_portal_stop, dns_start, dns_stop};
    static const xiaotai_module_t net = {.name = "net", .init = xiaotai_engine_net_init,
                                          .start = xiaotai_engine_net_start};
    const xiaotai_module_t *const modules[] = {&net};
    const xiaotai_net_config_t config = {.space = space, .store_unverified = unverified};
    atomic_store(&s_restarts, 0);
    assert(xiaotai_signal_init(&s_owner) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_stop_entered) == XIAOTAI_OK);
    xiaotai_net_host_set_mac(XIAOTAI_OK, (const uint8_t[]){1, 2, 3, 4, 5, 6});
    assert(xiaotai_net_configure(&config) == XIAOTAI_OK);
    assert(net_provision_portal_register(&portal) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 1}) == XIAOTAI_OK);
    sync();
    pthread_mutex_lock(&s_lock);
    assert(s_handler);
    pthread_mutex_unlock(&s_lock);
}

static void stop(void)
{
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_net_host_reset();
    xiaotai_signal_deinit(&s_owner);
    xiaotai_signal_deinit(&s_stop_entered);
}

static uint32_t driver_token(xiaotai_net_host_call_t method)
{
    xiaotai_net_host_record_t records[16];
    const size_t count = xiaotai_net_host_records(records, sizeof(records) / sizeof(records[0]));
    uint32_t token = 0;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].call == method) token = records[i].token;
    }
    assert(token);
    return token;
}

static void routes(void)
{
    static const char credentials[] = "{\"ssid\":\"fixture\",\"password\":\"local-fixture-psk\"}";
    boot("nw7-routes", false);
    net_exchange_t x = {0};
    checked(&x, "GET", "/", NULL, true, 0, 200, NULL);
    assert(!strcmp(x.content_type, "text/html; charset=utf-8") && x.chunks == 1 && x.response[0]);
    x = (net_exchange_t){0};
    checked(&x, "GET", "/generate_204", NULL, true, 0, 303, NULL);
    assert(x.redirected && strstr(x.response, "http-equiv=\"refresh\""));
    x = (net_exchange_t){0};
    checked(&x, "GET", "/api/wifi/networks", NULL, false, 0, 403, "provisioning closed");
    x = (net_exchange_t){.request = credentials, .size = strlen(credentials)};
    checked(&x, "POST", "/api/wifi", NULL, false, x.size, 403, "provisioning closed");
    assert(!x.received);
    x = (net_exchange_t){.request = credentials, .size = strlen(credentials)};
    checked(&x, "POST", "/api/wifi", "http://wrong.invalid", true, x.size, 403, "invalid origin");
    assert(!x.received);
    x = (net_exchange_t){0};
    checked(&x, "POST", "/api/wifi/scan", "", true, 0, 403, "invalid origin");
    x = (net_exchange_t){0};
    checked(&x, "POST", "/api/wifi/scan", NULL, true, 1, 400, "invalid request length");
    x = (net_exchange_t){0};
    checked(&x, "POST", "/api/wifi", NULL, true, NET_PORTAL_BODY_MAX + 1, 400, "invalid request length");
    x = (net_exchange_t){0};
    checked(&x, "POST", "/api/wifi", NULL, true, 0, 400, "invalid request length");
    x = (net_exchange_t){.request = credentials, .size = strlen(credentials), .receive_timeouts = 3};
    checked(&x, "POST", "/api/wifi", NULL, true, x.size, 408, "request timeout");
    assert(x.received == 3 && !x.consumed);
    x = (net_exchange_t){.request = credentials, .size = strlen(credentials), .late = true};
    checked(&x, "POST", "/api/wifi", NULL, true, x.size, 408, "request timeout");
    x = (net_exchange_t){.request = credentials, .size = strlen(credentials) - 1};
    checked(&x, "POST", "/api/wifi", NULL, true, x.size + 1, 400, "invalid request body");
    post("{\"ssid\":\"x\",\"password\":\"short\"}", 400, "invalid credentials");
    get("/api/wifi/status", "{\"state\":\"open\",\"error\":\"none\",\"reason\":\"unconfigured\"");
    x = (net_exchange_t){0};
    checked(&x, "POST", "/api/wifi/scan", NULL, true, 0, 200, "{\"accepted\":true}");
    sync();
    const uint32_t scan = driver_token(XIAOTAI_NET_HOST_SCAN);
    get("/api/wifi/networks", "\"state\":\"scanning\"");
    post(credentials, 409, "scan running");
    const xiaotai_net_scan_record_t scan_result[] = {
        {.ssid = "test\"\\<", .signal_dbm = -31, .security = XIAOTAI_NET_SECURITY_WPA2},
        {.ssid = "raw \xff", .signal_dbm = -40, .security = XIAOTAI_NET_SECURITY_OPEN},
    };
    xiaotai_net_host_set_scan(scan_result, 2, true);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = scan}) ==
           XIAOTAI_OK);
    sync();
    get("/api/wifi/networks", "\"ssid\":\"test\\\"\\\\\\u003c\",\"ssid_hex\":\"74657374225c3c\",\"rssi\":-31");
    get("/api/wifi/networks", "\"ssid\":\"raw \\ufffd\",\"ssid_hex\":\"72617720ff\"");
    get("/api/wifi/networks", "\"truncated\":true,\"history_error\":false,\"scan_error\":0");
    x = (net_exchange_t){.fail_chunk = 2};
    run(&x, &(net_request_t){.method = "GET", .path = "/api/wifi/networks", .local_is_ap = true});
    assert(x.code == 200 && !x.ended && x.chunks == 2);
    post("{\"ssid\":\"not-saved\",\"use_saved\":true}", 409, "saved password not found");
    uint32_t maintenance;
    assert(xiaotai_net_maintenance_begin(&maintenance) == XIAOTAI_OK);
    post(credentials, 503, "network unavailable");
    xiaotai_net_maintenance_end(maintenance);

    x = (net_exchange_t){.request = credentials, .size = strlen(credentials), .fail_chunk = 1};
    run(&x, &(net_request_t){.method = "POST", .path = "/api/wifi", .local_is_ap = true, .content_length = x.size});
    assert(x.code == 200 && !x.ended && x.chunks == 1);
    sync();
    get("/api/wifi/status", "\"state\":\"verifying\"");
    post(credentials, 409, "submission pending");
    const uint32_t join = driver_token(XIAOTAI_NET_HOST_JOIN);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_JOINED, .token = join, .ssid = "fixture",
    }) == XIAOTAI_OK);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = join, .ipv4 = 42,
    }) == XIAOTAI_OK);
    sync();
    xiaotai_net_provision_status_t status;
    xiaotai_net_provision_status(&status);
    assert(status.state == XIAOTAI_NET_PROVISION_DONE && status.restart_required && atomic_load(&s_restarts) == 1);
    get("/api/wifi/status", "\"restart\":true");
    post(credentials, 409, "restart pending");
    stop();
}

static void close_during_body(void)
{
    boot("nw7-body", false);
    const char payload[] = "{\"ssid\":\"fixture\",\"password\":\"\"}";
    net_exchange_t x = {.close_on_read = true, .request = payload, .size = strlen(payload)};
    checked(&x, "POST", "/api/wifi", NULL, true, x.size, 408, "request timeout");
    sync();
    assert(!xiaotai_net_has_saved_credentials() && atomic_load(&s_restarts) == 0);
    stop();
}

static void hold_worker(const net_message_t *message)
{
    (void)message;
    xiaotai_signal_give(&s_held);
    assert(xiaotai_signal_take(&s_release, WAIT_MS));
}

static void reopen(const net_message_t *message)
{
    (void)message;
    net_provision_open(XIAOTAI_NET_PROVISION_REASON_REQUESTED);
}

static void *cancelled_submit(void *unused)
{
    (void)unused;
    post("{\"ssid\":\"retired-request\",\"password\":\"\"}", 403, "provisioning closed");
    return NULL;
}

static void close_during_submit(void)
{
    boot("nw7-cancel", false);
    assert(xiaotai_signal_init(&s_held) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_release) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_submit_queued) == XIAOTAI_OK);
    atomic_store(&s_active_checks, 0);
    atomic_store(&s_observe_submit, true);
    assert(net_post(hold_worker, 0, NULL, 0) == XIAOTAI_OK && xiaotai_signal_take(&s_held, WAIT_MS));
    assert(xiaotai_net_provision_close() == XIAOTAI_OK);
    assert(net_post(reopen, 0, NULL, 0) == XIAOTAI_OK);
    pthread_t client;
    assert(!pthread_create(&client, NULL, cancelled_submit, NULL));
    assert(xiaotai_signal_take(&s_submit_queued, WAIT_MS));
    const int64_t start = xiaotai_time_us();
    xiaotai_signal_give(&s_release);
    assert(!pthread_join(client, NULL));
    assert(xiaotai_time_us() - start < 1000000);
    sync();
    atomic_store(&s_observe_submit, false);
    assert(!xiaotai_net_has_saved_credentials() && atomic_load(&s_restarts) == 0);
    get("/api/wifi/status", "{\"state\":\"open\",\"error\":\"none\",\"reason\":\"requested\",\"ssid\":\"\"");
    post("{\"ssid\":\"current-request\",\"password\":\"\"}", 200,
         "{\"accepted\":true,\"verify\":true,\"restart\":true}");
    xiaotai_signal_deinit(&s_submit_queued);
    xiaotai_signal_deinit(&s_release);
    xiaotai_signal_deinit(&s_held);
    stop();
}

static void direct_save(void)
{
    boot("nw7-direct", true);
    post("{\"ssid\":\"fixture\",\"password\":\"\"}", 200, "{\"accepted\":true,\"verify\":false,\"restart\":true}");
    sync();
    assert(xiaotai_net_has_saved_credentials() && atomic_load(&s_restarts) == 1);
    stop();
}

static void close_before_scan_starts(void)
{
    boot("nw7-scan-old", false);
    assert(xiaotai_signal_init(&s_held) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_release) == XIAOTAI_OK);
    assert(net_post(hold_worker, 0, NULL, 0) == XIAOTAI_OK && xiaotai_signal_take(&s_held, WAIT_MS));
    assert(xiaotai_net_provision_close() == XIAOTAI_OK);
    assert(net_post(reopen, 0, NULL, 0) == XIAOTAI_OK);
    net_exchange_t x = {0};
    checked(&x, "POST", "/api/wifi/scan", NULL, true, 0, 200, "{\"accepted\":true}");
    xiaotai_signal_give(&s_release);
    sync();
    xiaotai_net_host_record_t records[16];
    const size_t count = xiaotai_net_host_records(records, sizeof(records) / sizeof(records[0]));
    for (size_t i = 0; i < count; ++i) assert(records[i].call != XIAOTAI_NET_HOST_SCAN);
    x = (net_exchange_t){0};
    checked(&x, "POST", "/api/wifi/scan", NULL, true, 0, 200, "{\"accepted\":true}");
    sync();
    assert(driver_token(XIAOTAI_NET_HOST_SCAN));
    xiaotai_signal_deinit(&s_release);
    xiaotai_signal_deinit(&s_held);
    stop();
}

static void only_saved_inside_device(void)
{
    const xiaotai_net_credentials_t saved = {.ssid = "history-only", .password = "fixture-secret-key"};
    uint8_t history[492] = {1, 1};
    memcpy(history + 2, &saved, sizeof(saved));
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    assert(xiaotai_kv_set("nw7-saved", "known", history, sizeof(history), WAIT_MS) == XIAOTAI_OK);
    boot("nw7-saved", false);
    net_exchange_t x = {0};
    checked(&x, "GET", "/api/wifi/networks", NULL, true, 0, 200, NULL);
    assert(strstr(x.response, "\"ssid\":\"history-only\"") && strstr(x.response, "\"rssi\":null") &&
           strstr(x.response, "\"saved\":true,\"available\":false"));
    assert(!strstr(x.response, saved.password) && !strstr(x.response, "\"password\""));
    post("{\"ssid\":\"history-only\",\"use_saved\":true}", 200,
         "{\"accepted\":true,\"verify\":true,\"restart\":true}");
    sync();
    xiaotai_net_host_record_t records[16];
    const size_t count = xiaotai_net_host_records(records, sizeof(records) / sizeof(records[0]));
    unsigned joins = 0;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].call == XIAOTAI_NET_HOST_JOIN) {
            ++joins;
            assert(records[i].has_credentials && !strcmp(records[i].credentials.ssid, saved.ssid) &&
                   !strcmp(records[i].credentials.password, saved.password));
        }
    }
    assert(joins == 1);
    stop();
}

static void scan_history_rows(void)
{
    static const char space[] = "nw7-old-scan";
    static const struct {uint8_t version, count; xiaotai_net_credentials_t entries[5];} saved = {
        .version = 1, .count = 2, .entries = {
            {.ssid = "home", .password = "fixture-secret"},
            {.ssid = "offline", .password = "fixture-secret-2"},
        },
    };
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    assert(xiaotai_kv_set(space, "known", &saved, sizeof(saved), WAIT_MS) == XIAOTAI_OK);
    boot(space, false);
    const xiaotai_net_scan_record_t scanned[] = {
        {.ssid = "home", .signal_dbm = -40, .security = XIAOTAI_NET_SECURITY_WPA2},
        {.ssid = "home", .signal_dbm = -60, .security = XIAOTAI_NET_SECURITY_WPA2},
        {.ssid = "home", .signal_dbm = -40, .security = XIAOTAI_NET_SECURITY_OPEN},
        {.ssid = "\xe4\xb8\xad\xe6\x96\x87\"\\", .signal_dbm = -70, .security = XIAOTAI_NET_SECURITY_OPEN},
        {.ssid = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", .security = XIAOTAI_NET_SECURITY_WEP},
        {.ssid = ""},
    };
    xiaotai_net_host_set_scan(scanned, sizeof(scanned) / sizeof(scanned[0]), false);
    for (unsigned failed = 0; failed < 2; ++failed) {
        if (failed) {
            xiaotai_os_host_advance_us((int64_t)XIAOTAI_NET_SCAN_INTERVAL_MS * 1000);
            xiaotai_kv_host_fail_key("known", XIAOTAI_FAIL);
        }
        net_exchange_t x = {0};
        checked(&x, "POST", "/api/wifi/scan", NULL, true, 0, 200, "{\"accepted\":true}");
        sync();
        const uint32_t token = driver_token(XIAOTAI_NET_HOST_SCAN);
        assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
            .kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = token,
        }) == XIAOTAI_OK);
        sync();

        x = (net_exchange_t){0};
        checked(&x, "GET", "/api/wifi/networks", NULL, true, 0, 200, NULL);
        assert(strstr(x.response, "\"state\":\"ready\""));
        assert(strstr(x.response, failed ? "\"history_error\":true" : "\"history_error\":false"));
        assert(strstr(x.response, failed ? "\"rssi\":-40,\"secure\":true,\"saved\":false" :
                                          "\"rssi\":-40,\"secure\":true,\"saved\":true"));
        assert(strstr(x.response, "\"rssi\":-40,\"secure\":false,\"saved\":false"));
        assert(!strstr(x.response, "\"rssi\":-60") && !strstr(x.response, "\"ssid\":\"\""));
        assert(strstr(x.response, "\"ssid_hex\":\"e4b8ade69687225c\",\"rssi\":-70"));
        assert(strstr(x.response, "\"ssid\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"") &&
               strstr(x.response, "\"supported\":false"));
        if (failed) assert(!strstr(x.response, "\"ssid\":\"offline\""));
        else assert(strstr(x.response, "\"ssid\":\"offline\",\"ssid_hex\":\"6f66666c696e65\",\"rssi\":null,"
                                       "\"secure\":true,\"saved\":true,\"available\":false"));
        assert(!strstr(x.response, "fixture-secret") && !strstr(x.response, "\"password\""));
    }
    xiaotai_net_scan_record_t overflow[32] = {0};
    for (unsigned i = 0; i < sizeof(overflow) / sizeof(overflow[0]); ++i) {
        snprintf(overflow[i].ssid, sizeof(overflow[i].ssid), "scan-fixture-%u", i);
    }
    xiaotai_net_host_set_scan(overflow, sizeof(overflow) / sizeof(overflow[0]), false);
    xiaotai_os_host_advance_us((int64_t)XIAOTAI_NET_SCAN_INTERVAL_MS * 1000);
    net_exchange_t x = {0};
    checked(&x, "POST", "/api/wifi/scan", NULL, true, 0, 200, "{\"accepted\":true}");
    sync();
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = driver_token(XIAOTAI_NET_HOST_SCAN),
    }) == XIAOTAI_OK);
    sync();
    xiaotai_net_network_t rows[32];
    xiaotai_net_scan_status_t status;
    assert(xiaotai_net_networks(rows, sizeof(rows) / sizeof(rows[0]), &status) == 24 && status.truncated);
    get("/api/wifi/networks", "\"truncated\":true,\"history_error\":true,\"scan_error\":0");
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    puts("legacy scan vectors: duplicate strongest, open unsaved, exact SSID, history read failure, 32 results capped at 24");
    stop();
}

static void no_op(const net_message_t *message) { (void)message; }
static void queue_tail(const net_message_t *message)
{
    (void)message;
    xiaotai_signal_give(&s_submit_queued);
}

static void full_worker_queue(void)
{
    boot("nw7-queue", false);
    assert(xiaotai_signal_init(&s_held) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_release) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_submit_queued) == XIAOTAI_OK);
    assert(net_post(hold_worker, 0, NULL, 0) == XIAOTAI_OK && xiaotai_signal_take(&s_held, WAIT_MS));
    for (unsigned i = 1; i < XIAOTAI_NET_QUEUE_DEPTH; ++i) assert(net_post(no_op, 0, NULL, 0) == XIAOTAI_OK);
    assert(net_post(queue_tail, 0, NULL, 0) == XIAOTAI_OK);
    net_exchange_t x = {0};
    checked(&x, "POST", "/api/wifi/scan", NULL, true, 0, 500, "scan unavailable");
    post("{\"ssid\":\"network\",\"password\":\"\"}", 500, "submission failed");
    xiaotai_signal_give(&s_release);
    assert(xiaotai_signal_take(&s_submit_queued, WAIT_MS));
    post("{\"ssid\":\"network\",\"password\":\"\"}", 200, "{\"accepted\":true,\"verify\":true,\"restart\":true}");
    xiaotai_signal_deinit(&s_submit_queued);
    xiaotai_signal_deinit(&s_release);
    xiaotai_signal_deinit(&s_held);
    stop();
}

/* The native Wi-Fi legacy oracle used the same request, but its success
 * response followed the write. Here 200 means admitted, not persisted. */
static void native_transaction(void)
{
    static const char credentials[] = "{\"ssid\":\"synthetic-ap\",\"password\":\"synthetic-pass\"}";
    boot("nw7-native", false);
    xiaotai_kv_host_counts_t initial, replied, after;
    xiaotai_kv_host_counts(&initial);
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    post(credentials, 503, "network unavailable");
    xiaotai_kv_host_counts(&replied);
    assert(replied.writes == initial.writes && !atomic_load(&s_restarts));
    xiaotai_net_maintenance_end(ticket);

    xiaotai_kv_host_fail_key("credentials", XIAOTAI_ERR_TIMEOUT);
    post(credentials, 200, "{\"accepted\":true,\"verify\":true,\"restart\":true}");
    xiaotai_kv_host_counts(&replied);
    assert(replied.writes == initial.writes && !atomic_load(&s_restarts));
    sync();
    const uint32_t join = driver_token(XIAOTAI_NET_HOST_JOIN);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_JOINED, .token = join, .ssid = "synthetic-ap",
    }) == XIAOTAI_OK);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = join, .ipv4 = 42,
    }) == XIAOTAI_OK);
    sync();
    xiaotai_kv_host_counts(&after);
    xiaotai_net_provision_status_t status;
    xiaotai_net_provision_status(&status);
    assert(status.state == XIAOTAI_NET_PROVISION_STORE_FAILED && status.error == XIAOTAI_ERR_TIMEOUT &&
           !status.restart_required && !atomic_load(&s_restarts) && after.writes == replied.writes + 1);
    get("/api/wifi/status", "\"state\":\"store_failed\",\"error\":\"storage\"");
    ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE && !ticket);

    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    assert(xiaotai_net_provision_store_retry() == XIAOTAI_OK);
    sync();
    xiaotai_net_provision_status(&status);
    assert(status.state == XIAOTAI_NET_PROVISION_DONE && status.restart_required && atomic_load(&s_restarts) == 1);
    puts("native portal: 503 without write, 200 before verification, storage timeout without restart, one restart on retry");
    stop();
}

static void legacy_s3_credentials(void)
{
    static const char space[] = "wifi_cfg";
    static const xiaotai_net_credentials_t old = {.ssid = "ap-6", .password = "fixture-only"};
    static const xiaotai_net_credentials_t saved = {.ssid = "ap-3", .password = "updated-fixture"};
    static const char reuse[] = "{\"ssid\":\"display\",\"ssid_hex\":\"61702d33\",\"use_saved\":true}";
    uint8_t history[492] = {1, 2}, previous[99] = {1}, actual[492];
    memcpy(history + 2, &old, sizeof(old));
    memcpy(history + 2 + sizeof(old), &saved, sizeof(saved));
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    assert(xiaotai_kv_set(space, "known", history, sizeof(history), WAIT_MS) == XIAOTAI_OK);
    boot(space, false);
    xiaotai_net_config_t configured, effective;
    xiaotai_net_host_config(&configured, &effective);
    assert(effective.history_capacity == 5 && effective.layout == XIAOTAI_NET_STORE_RECORD &&
           !strcmp(effective.space, space) && !effective.store_unverified);
    memcpy(previous + 1, &old, sizeof(old));
    assert(xiaotai_kv_set(space, "credentials", previous, sizeof(previous), WAIT_MS) == XIAOTAI_OK);

    net_exchange_t list = {0};
    checked(&list, "GET", "/api/wifi/networks", NULL, true, 0, 200, NULL);
    assert(strstr(list.response, "\"ssid\":\"ap-3\"") && strstr(list.response, "\"ssid\":\"ap-6\""));
    assert(!strstr(list.response, saved.password) && !strstr(list.response, old.password) &&
           !strstr(list.response, "\"password\""));
    xiaotai_kv_host_counts_t initial, denied;
    xiaotai_kv_host_counts(&initial);
    post("{\"ssid\":\"missing\",\"use_saved\":true}", 409, "saved password not found");
    post("{\"ssid_hex\":\"6100\",\"password\":\"fixture-only\"}", 400, "invalid credentials");
    post("{\"ssid\":\"ap-3\",\"use_saved\":\"true\"}", 400, "invalid credentials");
    uint32_t ticket;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    post(reuse, 503, "network unavailable");
    xiaotai_net_maintenance_end(ticket);
    xiaotai_kv_host_counts(&denied);
    assert(denied.writes == initial.writes && !atomic_load(&s_restarts));

    /* The credential write, not a preliminary history read, is the fault. */
    xiaotai_kv_host_fail_key("credentials", XIAOTAI_ERR_TIMEOUT);
    net_exchange_t lost = {.request = reuse, .size = sizeof(reuse) - 1U, .fail_chunk = 1};
    run(&lost, &(net_request_t){.method = "POST", .path = "/api/wifi", .local_is_ap = true,
                              .content_length = lost.size});
    assert(lost.code == 200 && !lost.ended && lost.chunks == 1);
    sync();
    xiaotai_net_host_record_t records[16];
    const size_t count = xiaotai_net_host_records(records, sizeof(records) / sizeof(records[0]));
    uint32_t join = 0;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].call != XIAOTAI_NET_HOST_JOIN) continue;
        assert(!join && records[i].has_credentials &&
               !memcmp(&records[i].credentials, &saved, sizeof(saved)));
        join = records[i].token;
    }
    assert(join && !atomic_load(&s_restarts));
    get("/api/wifi/status", "\"state\":\"verifying\"");
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_JOINED, .token = join, .ssid = "ap-3",
    }) == XIAOTAI_OK);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = join, .ipv4 = 42,
    }) == XIAOTAI_OK);
    sync();
    get("/api/wifi/status", "\"state\":\"store_failed\",\"error\":\"storage\"");
    assert(!atomic_load(&s_restarts));
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    size_t size = sizeof(actual);
    assert(xiaotai_kv_get(space, "credentials", actual, &size, WAIT_MS) == XIAOTAI_OK &&
           size == sizeof(previous) && !memcmp(actual, previous, size));
    size = sizeof(actual);
    assert(xiaotai_kv_get(space, "known", actual, &size, WAIT_MS) == XIAOTAI_OK &&
           size == sizeof(history) && !memcmp(actual, history, size));

    assert(xiaotai_net_provision_store_retry() == XIAOTAI_OK);
    sync();
    assert(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    get("/api/wifi/status", "\"state\":\"done\"");
    post(reuse, 409, "restart pending");
    assert(atomic_load(&s_restarts) == 1 && xiaotai_net_has_saved_credentials());
    memcpy(previous + 1, &saved, sizeof(saved));
    size = sizeof(actual);
    assert(xiaotai_kv_get(space, "credentials", actual, &size, WAIT_MS) == XIAOTAI_OK &&
           size == sizeof(previous) && !memcmp(actual, previous, size));
    memcpy(history + 2, &saved, sizeof(saved));
    memcpy(history + 2 + sizeof(saved), &old, sizeof(old));
    size = sizeof(actual);
    assert(xiaotai_kv_get(space, "known", actual, &size, WAIT_MS) == XIAOTAI_OK &&
           size == sizeof(history) && !memcmp(actual, history, size));
    puts("S3 Portal: saved-password hex selection after reply loss, async storage failure preserves 99/492 bytes, one retry and restart");
    stop();
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "native-transaction")) {
        native_transaction();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "legacy-s3-credentials")) {
        legacy_s3_credentials();
        return 0;
    }
    assert(argc == 1);
    routes();
    close_during_body();
    close_during_submit();
    close_before_scan_starts();
    direct_save();
    only_saved_inside_device();
    scan_history_rows();
    full_worker_queue();
    puts("net_portal: ok");
    return 0;
}
