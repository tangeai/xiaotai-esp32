// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_binding_lifecycle.py
// Replaces legacy: tests/regression/test_companion_maintenance.py
// Replaces legacy: tests/regression/test_s3_discovered_endpoint.py
// Replaces legacy: tests/regression/test_service_http_policy.py
// Replaces legacy: tests/regression/test_environment_startup.py
// Replaces legacy: tests/regression/test_p4_environment_startup.py
#include "product_platform.h"
#include "product_test.h"
#if PRODUCT_TEST_HOSTED_STARTUP_FAILURES
#include "xiaotai/apps/companion.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai_console_host.h"
#endif
#if PRODUCT_TEST_COPROCESSOR
#include "xiaotai_coprocessor_host.h"
#endif
#include "xiaotai/hal.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/mqtt.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai/view.h"
#include "transport_backend.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_net_host.h"
#include "xiaotai_transport_host.h"

#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

void product_endpoint_prepare(const char *test);
void product_endpoint(const char *test);

static const char broker[] = "mqtts://broker.invalid:8883";
static const char business[] = "https://fixture.invalid/device/";
static const char *const malformed[] = {"ftp://rtc.invalid", "http://", "http://host:0/path",
    "http://host:70000", "http://user@host/path", "http://host/path?value", "http://host/path#",
    "http://host\\\\path", "http://host name", "http://host/path\\n"};
/* JSON string contents from the deployed service policy oracle. */
static const char *const policy_bad_http[] = {
    "ftp://host", "http://", "http://user@host", "http://host:0", "http://host:65536", "http://host?secret",
    "http://host#fragment", "http://host\\\\path", "http://host/path\\t", "http://host name",
    "http://host/\\u007f", "http://host/path\\r\\n",
};
static const char *const policy_good_mqtt[] = {
    "mqtt://broker.test", "mqtts://broker.test", "mqtt://broker.test:1884", "mqtts://broker.test:8884",
    "MQTT://broker.test:1884/", "mqtt://127.0.0.1:1883", "mqtts://[::1]:8883",
};
static const char *const policy_bad_mqtt[] = {
    "", "mqtt://", "mqtts://", "http://broker.test", "ws://broker.test", "mqtt://user@broker.test",
    "mqtts://user:pass@broker.test", "mqtt://broker.test:0", "mqtts://broker.test:65536",
    "mqtt://broker.test:abc", "mqtt://:1883", "mqtt://broker.test?token", "mqtts://broker.test#fragment",
    "mqtt://broker.test\\\\path", "mqtt://broker name", "mqtt://broker.test/\\u007f", "mqtts://broker.test/\\r\\n",
};
static const char policy_base[] = "https://service.test:8080/tenant/";
static const char policy_normalized[] = "http://service.test:8080/tenant";
static const char *policy_broker;
static const char *policy_recognition;
static xiaotai_err_t policy_error;
static char discovery[1024], long_endpoint[132];
static const char recovered[] = "{\"device-srv\":\"http://fixture.invalid/device\","
    "\"mqtt-srv\":\"mqtts://broker.invalid:8883\","
    "\"ai-srv\":\"http://fixture.invalid/device\",\"call-srv\":\"http://fixture.invalid/device\","
    "\"voip-srv\":\"http://fixture.invalid/device\",\"tirtc-srv\":\"http://rtc.invalid\"}";
static bool startup_order;
static bool boot_bad_environment;
static atomic_uint bad_environment_reserve_attempts;
static bool fail_start_reserve;
static atomic_uint start_reserve_failures;
static _Atomic(void *) reserve_pointer;
static atomic_uint startup_step;
static const char startup_endpoint[] = "http://[::1]:8090/tenant";
static bool watch_binding_view, fixture_bound;
static atomic_uint unexpected_http;

typedef struct {
    char page[48], title[96], text[128], status[128], first_row[128];
    bool retry_binding, retry_icon, change_wifi;
} startup_view_t;
static startup_view_t last_view;

#if PRODUCT_TEST_HOSTED_STARTUP_FAILURES
static bool fail_worker;
static bool fail_platform_worker;
static atomic_uint platform_worker_failures, worker_repair_reserve_attempts;
static bool fail_companion_start;
static xiaotai_net_fault_t companion_recovery_fault;
#if PRODUCT_TEST_COPROCESSOR
static uint16_t companion_good_chip;
#endif
static bool prepare_view_seen, prepare_view_offers_retry;
static bool door_prepare_seen, door_prepare_offers_retry;
static char prepare_view_status[32];
static bool watch_worker_feedback;
static unsigned worker_toasts;
static char last_worker_toast[80];
static atomic_uint worker_failures;

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (fail_platform_worker && config && config->name && !strcmp(config->name, "xiaotai_platform")) {
        ++platform_worker_failures;
        return XIAOTAI_ERR_NO_MEM;
    }
    if (fail_worker && config && config->name && !strcmp(config->name, "xt_transport")) {
        ++worker_failures;
        return XIAOTAI_ERR_NO_MEM;
    }
    return __real_xiaotai_task_create(config, task);
}
#endif

void product_discovery_observe_view(const xiaotai_view_t *view);
void product_discovery_observe_view(const xiaotai_view_t *view)
{
#if PRODUCT_TEST_HOSTED_STARTUP_FAILURES
    if (watch_worker_feedback && view && view->layer == XIAOTAI_LAYER_TOAST && view->text && xiaotai_on_owner()) {
        ++worker_toasts;
        snprintf(last_worker_toast, sizeof(last_worker_toast), "%s", view->text);
    }
    if ((fail_companion_start || companion_recovery_fault) && view && view->id &&
        view->layer == XIAOTAI_LAYER_PAGE && xiaotai_on_owner()) {
        if (!strcmp(view->id, "preparing") || !strcmp(view->id, "door.prepare")) {
            bool offers_retry = false;
            for (size_t i = 0; i < view->action_count; ++i) {
                const char *intent = view->actions[i].intent;
                offers_retry |= intent && view->actions[i].role == XIAOTAI_ROLE_PRIMARY &&
                                !strcmp(intent, XIAOTAI_INTENT_PROVISIONING_RETRY);
            }
            if (!strcmp(view->id, "door.prepare")) {
                if (view->kind == XIAOTAI_VIEW_NOTICE) {
                    door_prepare_seen = true;
                    door_prepare_offers_retry = offers_retry;
                }
            } else {
                prepare_view_seen = true;
                prepare_view_offers_retry = offers_retry;
                snprintf(prepare_view_status, sizeof(prepare_view_status), "%s", view->status ? view->status : "");
            }
        }
    }
#endif
    if (watch_binding_view && view && view->id && xiaotai_on_owner() && view->layer == XIAOTAI_LAYER_PAGE) {
        last_view = (startup_view_t){0};
        snprintf(last_view.page, sizeof(last_view.page), "%s", view->id);
        snprintf(last_view.title, sizeof(last_view.title), "%s", view->title ? view->title : "");
        snprintf(last_view.text, sizeof(last_view.text), "%s", view->text ? view->text : "");
        snprintf(last_view.status, sizeof(last_view.status), "%s", view->status ? view->status : "");
        if (view->item_count && view->items[0].label)
            snprintf(last_view.first_row, sizeof(last_view.first_row), "%s", view->items[0].label);
        for (size_t i = 0; i < view->action_count; ++i) {
            const xiaotai_view_action_t *action = &view->actions[i];
            if (action->intent && !strcmp(action->intent, XIAOTAI_INTENT_BINDING_RETRY)) {
                last_view.retry_binding = true;
                last_view.retry_icon = action->icon && !strcmp(action->icon, XIAOTAI_ICON_REFRESH);
                if (!last_view.retry_icon) assert(!strcmp(action->label, "刷新验证码"));
            }
            last_view.change_wifi |= action->intent && !strcmp(action->intent, XIAOTAI_INTENT_PROVISIONING_BEGIN);
        }
    }
}

static void reject_http(void *ctx, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    ++unexpected_http;
    fprintf(stderr, "unexpected HTTP while environment load failed: %s\n", request->url);
    reply->status = 503;
}

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void __real_xiaotai_mem_free(void *pointer);
void __wrap_xiaotai_mem_free(void *pointer);
xiaotai_err_t __real_xiaotai_rtc_start(const xiaotai_rtc_start_t *options, int *detail);
xiaotai_err_t __wrap_xiaotai_rtc_start(const xiaotai_rtc_start_t *options, int *detail);
xiaotai_err_t __real_xiaotai_mqtt_start(xiaotai_mqtt_t *client);
xiaotai_err_t __wrap_xiaotai_mqtt_start(xiaotai_mqtt_t *client);

#if PRODUCT_TEST_RECOGNITION_HTTP
bool product_recognition_view_fail_allocation(xiaotai_mem_kind_t kind);
#endif

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
#if PRODUCT_TEST_RECOGNITION_HTTP
    if (product_recognition_view_fail_allocation(kind)) return NULL;
#endif
    if (fail_start_reserve && kind == XIAOTAI_MEM_INTERNAL && size == 20480U) {
        atomic_fetch_add(&start_reserve_failures, 1U);
        return NULL;
    }
    if (boot_bad_environment && kind == XIAOTAI_MEM_INTERNAL && size == 20480U)
        atomic_fetch_add(&bad_environment_reserve_attempts, 1U);
#if PRODUCT_TEST_HOSTED_STARTUP_FAILURES
    if (fail_platform_worker && kind == XIAOTAI_MEM_INTERNAL && size == 20480U)
        atomic_fetch_add(&worker_repair_reserve_attempts, 1U);
#endif
    void *pointer = __real_xiaotai_mem_alloc(kind, size);
    xiaotai_mem_host_buffer_site(pointer, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)));
    if (startup_order && kind == XIAOTAI_MEM_INTERNAL && size == 20480U) {
        void *none = NULL;
        assert(pointer && atomic_compare_exchange_strong(&reserve_pointer, &none, pointer));
    }
    return pointer;
}

void __wrap_xiaotai_mem_free(void *pointer)
{
    const bool is_reserve = startup_order && pointer && pointer == atomic_load(&reserve_pointer);
    if (is_reserve) {
        xiaotai_platform_status_t status;
        xiaotai_platform_status(&status);
        char endpoint[XIAOTAI_PLATFORM_URL_MAX];
        assert(status.clock_synced && status.discovered && status.held && !status.authorized && !status.signaling);
        assert(xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_TIRTC, endpoint, sizeof(endpoint)) == XIAOTAI_OK);
        assert(!strcmp(endpoint, startup_endpoint));
        assert(atomic_load(&startup_step) == 0);
    }
    __real_xiaotai_mem_free(pointer);
    if (is_reserve) {
        atomic_store(&reserve_pointer, NULL);
        atomic_store(&startup_step, 1U);
    }
}

xiaotai_err_t __wrap_xiaotai_rtc_start(const xiaotai_rtc_start_t *options, int *detail)
{
    if (startup_order) {
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        assert(atomic_load(&startup_step) == 1 && !stats.logins);
        assert(!product_platform_has_client(PRODUCT_FIXTURE_CLIENT));
        assert(!strcmp(options->endpoint, startup_endpoint));
        atomic_store(&startup_step, 2U);
    }
    return __real_xiaotai_rtc_start(options, detail);
}

xiaotai_err_t __wrap_xiaotai_mqtt_start(xiaotai_mqtt_t *client)
{
    if (startup_order) {
        assert(atomic_load(&startup_step) == 2U);
        assert(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
        atomic_store(&startup_step, 3U);
    }
    return __real_xiaotai_mqtt_start(client);
}

static bool test_is(const char *test, const char *name) { return !strcmp(test, name); }

static bool service_policy(const char *test)
{
    return !strncmp(test, "policy-", sizeof("policy-") - 1U);
}

static void prepare_service_policy(const char *test)
{
    const char *device = policy_base;
    const char *rtc = ",\"tirtc-srv\":\"https://service.test:8080/tenant/\"";
    char recognition[96] = "";
    unsigned example;
    char suffix;
    policy_broker = broker;
    if (sscanf(test, "policy-http-bad-%u%c", &example, &suffix) == 1) {
        assert(example < sizeof(policy_bad_http) / sizeof(*policy_bad_http));
        device = policy_bad_http[example];
        policy_error = XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
    } else if (sscanf(test, "policy-mqtt-bad-%u%c", &example, &suffix) == 1) {
        assert(example < sizeof(policy_bad_mqtt) / sizeof(*policy_bad_mqtt));
        policy_broker = policy_bad_mqtt[example];
        policy_error = XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
    } else if (sscanf(test, "policy-mqtt-good-%u%c", &example, &suffix) == 1) {
        assert(example < sizeof(policy_good_mqtt) / sizeof(*policy_good_mqtt));
        policy_broker = policy_good_mqtt[example];
    } else if (test_is(test, "policy-missing-rtc")) {
        rtc = "";
        policy_error = XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
    } else if (test_is(test, "policy-retry-201")) {
        policy_error = XIAOTAI_FAIL;
    } else if (test_is(test, "policy-recognition-valid")) {
        policy_recognition = "https://recognition.test:8081/tenant/";
    } else if (test_is(test, "policy-recognition-invalid")) {
        policy_recognition = "http://user@bad.test";
    } else {
        assert(test_is(test, "policy-recognition-missing"));
    }
    if (policy_recognition) {
        const int size = snprintf(recognition, sizeof(recognition), ",\"recognition-srv\":\"%s\"", policy_recognition);
        assert(size > 0 && (size_t)size < sizeof(recognition));
    }
    const int size = snprintf(discovery, sizeof(discovery), "{\"device-srv\":\"%s\",\"mqtt-srv\":\"%s\","
        "\"ai-srv\":\"%s\",\"call-srv\":\"%s\",\"voip-srv\":\"%s\"%s%s}",
        device, policy_broker, policy_base, policy_base, policy_base, rtc, recognition);
    assert(size > 0 && (size_t)size < sizeof(discovery));
    product_platform_prepare(true);
    product_platform_discovery_response(policy_error == XIAOTAI_FAIL ? 201 : 200, discovery, policy_broker);
}

void product_endpoint_prepare(const char *test)
{
    if (service_policy(test)) {
        prepare_service_policy(test);
        return;
    }
    startup_order = test_is(test, "startup-order");
#if PRODUCT_TEST_HOSTED_STARTUP_FAILURES
#if PRODUCT_TEST_COPROCESSOR
    if (test_is(test, "companion-recovery-no-link") || test_is(test, "companion-recovery-unsupported")) {
        product_platform_prepare(true);
        xiaotai_coprocessor_host_t *peer = xiaotai_coprocessor_host();
        companion_good_chip = peer->chip;
        if (test_is(test, "companion-recovery-no-link")) {
            peer->wait_result = peer->start_result = XIAOTAI_ERR_TIMEOUT;
            companion_recovery_fault = XIAOTAI_NET_FAULT_NO_LINK;
        } else {
            peer->chip = 0x73;
            companion_recovery_fault = XIAOTAI_NET_FAULT_UNSUPPORTED;
        }
        return;
    }
#endif
    if (test_is(test, "companion-driver-start-failure")) {
        fail_companion_start = true;
        product_platform_prepare(true);
        /* Replace the product fixture's queued successful start. */
        xiaotai_net_host_reset();
        const xiaotai_net_host_step_t failure = {.result = XIAOTAI_FAIL};
        assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_START, &failure) == XIAOTAI_OK);
        return;
    }
    if (test_is(test, "transport-worker-lifecycle")) {
        fail_worker = true;
        if (PRODUCT_HOST_PLATFORM_CONSOLE) xiaotai_console_host_pipe();
        product_platform_prepare(true);
        product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
            "{\"type\":\"device\",\"device_id\":\"peer\",\"remark\":\"Peer\",\"online\":true}]}}");
        return;
    }
    if (test_is(test, "platform-worker-repair")) {
        fail_platform_worker = fixture_bound = true;
        product_platform_prepare(true);
        xiaotai_http_host_set_responder(reject_http, NULL, true);
        assert(xiaotai_kv_set_u8("product", "volume", 5, 3000) == XIAOTAI_OK);
        assert(xiaotai_kv_set_str("service_env", "ep_base", "http://old.invalid", 3000) == XIAOTAI_OK);
        return;
    }
#endif
    if (test_is(test, "environment-binding-failed-bound") || test_is(test, "environment-binding-failed-unbound")) {
        watch_binding_view = true;
        fixture_bound = test_is(test, "environment-binding-failed-bound");
        product_platform_prepare(fixture_bound);
        xiaotai_http_host_set_responder(reject_http, NULL, true);
        assert(xiaotai_kv_set_str("service_env", "ep_base", "ftp://invalid", 3000) == XIAOTAI_OK);
        return;
    }
    if (test_is(test, "environment-healthy-bound") || test_is(test, "environment-healthy-unbound")) {
        fixture_bound = test_is(test, "environment-healthy-bound");
        product_platform_prepare(fixture_bound);
        return;
    }
    if (test_is(test, "environment-boot-error")) {
        boot_bad_environment = fixture_bound = true;
        product_platform_prepare(true);
        assert(xiaotai_kv_set_u8("product", "volume", 5, 3000) == XIAOTAI_OK);
        assert(xiaotai_kv_set_str("service_env", "ep_base", "ftp://invalid", 3000) == XIAOTAI_OK);
        return;
    }
    if (test_is(test, "startup-reserve-oom")) {
        fail_start_reserve = true;
        product_platform_prepare(true);
        return;
    }
    char rtc[170] = {0};
    const char *rtc_url = "http://rtc.invalid";
    const char *device = business;
    const char *recognition = "";
    if (!strncmp(test, "bad", 3)) {
        assert(test[3] >= '0' && test[3] <= '9' && !test[4]);
        rtc_url = malformed[test[3] - '0'];
    } else if (test_is(test, "path")) {
        rtc_url = "https://rtc.invalid:8080/tenant/";
    } else if (test_is(test, "ipv6") || startup_order) {
        rtc_url = "http://[::1]:8090/tenant/";
    } else if (test_is(test, "long")) {
        strcpy(long_endpoint, "http://host/");
        memset(long_endpoint + strlen(long_endpoint), 'x', sizeof(long_endpoint) - 13U);
        rtc_url = long_endpoint;
    } else if (test_is(test, "missing") || test_is(test, "retry-missing") || test_is(test, "retry-manual")) {
        rtc_url = NULL;
    } else if (test_is(test, "retry-invalid")) {
        rtc_url = malformed[0];
    } else if (test_is(test, "mandatory")) {
        device = "https://user@business.invalid";
    } else if (test_is(test, "recognition")) {
        recognition = ",\"recognition-srv\":\"https://recognition.invalid:8081/tenant/\"";
    } else if (test_is(test, "invalid-recognition")) {
        recognition = ",\"recognition-srv\":\"http://user@recognition.invalid\"";
    }
    if (rtc_url) {
        const int n = snprintf(rtc, sizeof(rtc), ",\"tirtc-srv\":\"%s\"", rtc_url);
        assert(n > 0 && (size_t)n < sizeof(rtc));
    }
    const int n = snprintf(discovery, sizeof(discovery), "{\"device-srv\":\"%s\",\"mqtt-srv\":\"%s\","
        "\"ai-srv\":\"%s\",\"call-srv\":\"%s\",\"voip-srv\":\"%s\"%s%s}",
        device, broker, business, business, business, rtc, recognition);
    assert(n > 0 && (size_t)n < sizeof(discovery));
    product_platform_prepare(true);
    product_platform_discovery_response((test_is(test, "redirect") || startup_order) ? 302 : 200, discovery, broker);
}

static void endpoint(const char *service, const char *expected)
{
    char actual[XIAOTAI_PLATFORM_URL_MAX];
    assert(xiaotai_platform_endpoint(service, actual, sizeof(actual)) == XIAOTAI_OK);
    assert(!strcmp(actual, expected));
}

static void check_service_policy(const char *test)
{
    xiaotai_platform_status_t status;
    if (policy_error != XIAOTAI_OK) {
        for (unsigned i = 0; i < 1000U; ++i) {
            xiaotai_platform_status(&status);
            if (status.discovered || status.last_error) break;
            xiaotai_delay_ms(5);
        }
        assert(!status.discovered && status.last_error == policy_error && !status.authorized && !status.signaling);
        char value[XIAOTAI_PLATFORM_URL_MAX] = "stale";
        assert(xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_DEVICE, value, sizeof(value)) ==
               XIAOTAI_ERR_PLATFORM_OFFLINE && !value[0]);
        assert(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
        assert(!product_platform_has_client(PRODUCT_FIXTURE_CLIENT));
        printf("%s: rejected discovery before publishing services, error=%d\n", test, status.last_error);
        if (!test_is(test, "policy-retry-201")) return;
        product_platform_discovery_status(200);
        if (status.phase == XIAOTAI_PLATFORM_PAUSED) assert(xiaotai_platform_retry() == XIAOTAI_OK);
    }
    for (unsigned i = 0; i < 2400U; ++i) {
        xiaotai_platform_status(&status);
        if (status.discovered) break;
        xiaotai_delay_ms(5);
    }
    assert(status.discovered);
    endpoint(XIAOTAI_PLATFORM_SERVICE_DEVICE, policy_normalized);
    endpoint(XIAOTAI_PLATFORM_SERVICE_AI, policy_normalized);
    endpoint(XIAOTAI_PLATFORM_SERVICE_CALL, policy_normalized);
    endpoint(XIAOTAI_PLATFORM_SERVICE_VOIP, policy_normalized);
    endpoint(XIAOTAI_PLATFORM_SERVICE_TIRTC, policy_normalized);
    endpoint(XIAOTAI_PLATFORM_SERVICE_MQTT, policy_broker);
    if (test_is(test, "policy-recognition-valid")) {
        endpoint(XIAOTAI_PLATFORM_SERVICE_RECOGNITION, "http://recognition.test:8081/tenant");
    } else {
        char value[XIAOTAI_PLATFORM_URL_MAX] = "stale";
        assert(xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_RECOGNITION, value, sizeof(value)) ==
               (policy_recognition ? XIAOTAI_ERR_PLATFORM_BAD_SERVICE : XIAOTAI_ERR_NOT_SUPPORTED) && !value[0]);
    }
    if (test_is(test, "policy-retry-201")) {
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        assert(stats.discoveries == 2);
    }
    printf("%s: services published with HTTP bases and unchanged MQTT\n", test);
}

static void request_environment_repair(void *context)
{
    const char *base = context;
    assert(xiaotai_environment_set(base) == XIAOTAI_OK);
}

static void copy_view(void *ctx) { *(startup_view_t *)ctx = last_view; }

static void assert_binding_identity(void)
{
    assert(xiaotai_identity_matches(PRODUCT_FIXTURE_DEVICE, PRODUCT_FIXTURE_SECRET) == fixture_bound);
    char saved[65];
    const xiaotai_err_t err = xiaotai_kv_get_str("tirtc_cfg", "device_id", saved, sizeof(saved), 3000);
    if (fixture_bound) assert(err == XIAOTAI_OK && !strcmp(saved, PRODUCT_FIXTURE_DEVICE));
    else assert(err == XIAOTAI_ERR_NOT_FOUND);
}

static void assert_boot_settings(void)
{
    assert_binding_identity();
    assert(xiaotai_net_has_saved_credentials());
    uint8_t volume = 0;
    assert(xiaotai_kv_get_u8("product", "volume", &volume, 3000) == XIAOTAI_OK && volume == 5);
}

static void check_environment_binding_failure(void)
{
    assert(xiaotai_net_host_sync(3000) == XIAOTAI_OK);
    assert(xiaotai_transport_host_settle(3000));
    startup_view_t view = {0};
    product_on_owner(copy_view, &view);
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    xiaotai_platform_status_t status;
    xiaotai_platform_status(&status);
    xiaotai_platform_stats_t platform;
    xiaotai_platform_stats(&platform);
    const uint32_t rtc = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START);
    fprintf(stderr, "first_failure=environment error=%d binding=%u page=%s title=[%s] text=[%s] status=[%s] row=[%s]"
                    " retry_binding=%u discoveries=%u logins=%u http=%u rtc=%u\n",
            status.last_error, (unsigned)binding.phase, view.page, view.title, view.text, view.status, view.first_row,
            (unsigned)view.retry_binding, (unsigned)platform.discoveries, (unsigned)platform.logins,
            (unsigned)atomic_load(&unexpected_http), (unsigned)rtc);
    assert(xiaotai_platform_environment_boot_failed() && status.last_error != XIAOTAI_OK &&
           status.phase == XIAOTAI_PLATFORM_STOPPED && !status.gated);
    assert(platform.discoveries == 0 && platform.logins == 0 && !rtc && !atomic_load(&unexpected_http));
    const bool s3 = !strcmp(XIAOTAI_PRODUCT_NAME, "xiaotai-szpi-s3") ||
                    !strcmp(XIAOTAI_PRODUCT_NAME, "xiaotai-cores3-lite");
    assert(binding.phase == XIAOTAI_BINDING_FAILED && binding.environment_boot_failed &&
           binding.error == status.last_error && !binding.verification[0]);
    assert(view.retry_binding && view.retry_icon == s3 && view.change_wifi &&
           (!strcmp(view.page, "binding") || !strcmp(view.page, "door.binding")));
    assert(!strcmp(view.title, "绑定设备") && !strcmp(view.text, "------"));
    assert(!strcmp(view.first_row, s3 ? "绑定未完成，请点击右上角重试" : "绑定未完成，请点击刷新"));
    const xiaotai_display_t *const display = xiaotai_hal_display(0);
    assert(display);
    if (display->desc.rotation) {
        /* The host LVGL port rejects hardware-rotated displays independently
         * of the selected app page. The owner-visible view is still tested. */
        printf("host renderer lacks rotation=%u; checking view and intents only\n",
               (unsigned)display->desc.rotation);
    } else {
        xiaotai_ui_info_t info = {0};
        for (unsigned i = 0; i < 400U; ++i) {
            if (xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.ready && info.frames &&
                !strcmp(info.page, view.page)) break;
            xiaotai_delay_ms(5);
        }
        assert(info.ready && info.frames && !info.present_fault && !strcmp(info.page, view.page));
    }
    assert_binding_identity();
    assert(xiaotai_intent_post(XIAOTAI_INTENT_BINDING_RETRY, NULL) == XIAOTAI_OK);
    for (unsigned i = 0; i < 400U; ++i) {
        product_on_owner(copy_view, &view);
        xiaotai_binding_status(&binding);
        if (binding.phase == XIAOTAI_BINDING_NEEDED && !view.retry_binding) break;
        xiaotai_delay_ms(5);
    }
    fprintf(stderr, "refresh binding=%u retry=%u row=[%s]\n", (unsigned)binding.phase,
            (unsigned)view.retry_binding, view.first_row);
    assert(binding.phase == XIAOTAI_BINDING_NEEDED && binding.environment_boot_failed && !view.retry_binding);
    assert(!strcmp(view.first_row, "正在获取验证码"));
    assert_binding_identity();
    xiaotai_platform_stats(&platform);
    assert(!platform.discoveries && !platform.logins && !atomic_load(&unexpected_http) &&
           !xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
}

static void check_healthy_environment(void)
{
    if (fixture_bound) product_platform_online();
    xiaotai_platform_stats_t platform;
    unsigned elapsed = 0;
    for (; elapsed < 5000U; elapsed += 5U) {
        xiaotai_platform_stats(&platform);
        if (platform.discoveries) break;
        xiaotai_delay_ms(5);
    }
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    assert(elapsed < 5000U && !xiaotai_platform_environment_boot_failed() && !binding.environment_boot_failed);
    assert_binding_identity();
    if (fixture_bound) assert(binding.phase == XIAOTAI_BINDING_BOUND && platform.logins == 1 &&
                              xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START) == 1);
    else assert(binding.phase != XIAOTAI_BINDING_BOUND && platform.logins == 0);
    printf("healthy environment: bound=%u discoveries=%u logins=%u boot_fault=0\n",
           (unsigned)fixture_bound, (unsigned)platform.discoveries, (unsigned)platform.logins);
}

static void reject_offline_refresh(void *unused)
{
    (void)unused;
    assert(xiaotai_binding_retry() == XIAOTAI_ERR_PLATFORM_OFFLINE);
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    assert(binding.environment_boot_failed && binding.phase == XIAOTAI_BINDING_FAILED);
}

#if PRODUCT_TEST_HOSTED_STARTUP_FAILURES
static void diagnostic(void *ctx, const char *text)
{
    if (strncmp(text, "+LINK: ", 7U)) return;
    assert(strstr(text, "started=0 connected=0") && strstr(text, "error=257 wait=fault"));
    *(bool *)ctx = true;
    puts(text);
}

static void check_worker_failure(void *unused)
{
    (void)unused;
    xiaotai_transport_status_t status;
    xiaotai_transport_status(&status);
    assert(atomic_load(&worker_failures) == 1U && status.state == XIAOTAI_TRANSPORT_FAULT &&
           status.last_fault_detail == XIAOTAI_ERR_NO_MEM && !status.start_reserve_held && !status.connect_ready);
    assert(xiaotai_transport_restart() == XIAOTAI_ERR_INVALID_STATE);
    const xiaotai_link_request_t request = {.kind = XIAOTAI_LINK_WHIP, .peer = "offline-fixture"};
    const xiaotai_link_handlers_t handlers = {0};
    uint32_t generation = UINT32_MAX;
    assert(xiaotai_link_connect(&request, &handlers, &generation) == XIAOTAI_ERR_INVALID_STATE && !generation);
    bool diagnosed = false;
    const xiaotai_reply_t out = {.write = diagnostic, .ctx = &diagnosed};
    xiaotai_diag_report("link", &out);
    assert(diagnosed && !xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
}

static void watch_worker_toast(void *unused)
{
    (void)unused;
    if (strcmp(xiaotai_app_home(), "companion")) {
        assert(xiaotai_app_switch("companion") == XIAOTAI_OK);
    }
    xiaotai_app_companion_status_t app;
    xiaotai_app_companion_status(&app);
    assert(app.foreground && !app.setup);
    watch_worker_feedback = true;
}

static void check_worker_ai_ui(void *unused)
{
    (void)unused;
    watch_worker_feedback = false;
    xiaotai_ai_talk_status_t ai;
    xiaotai_ai_talk_status(&ai);
    printf("AI input while RTC failed: toasts=%u text=[%s] session=%u link=%u\n", worker_toasts,
           last_worker_toast, (unsigned)ai.session, (unsigned)ai.link);
    assert(worker_toasts == 1 && !strcmp(last_worker_toast, "操作未受理，请重试") && !ai.session && !ai.link);
}

static void check_worker_call_idle(void *unused)
{
    (void)unused;
    xiaotai_call_status_t call;
    xiaotai_call_status(&call);
    assert(call.state == XIAOTAI_CALL_IDLE && !call.session && !call.link && !call.dialled);
}

static void check_worker_commands_without_console(void *unused)
{
    (void)unused;
    uint32_t session = UINT32_MAX;
    const xiaotai_err_t ai = xiaotai_ai_talk_start(NULL, &session);
    assert(ai == XIAOTAI_ERR_PLATFORM_OFFLINE && !session);
    session = UINT32_MAX;
    const xiaotai_err_t call = xiaotai_call_dial("peer", 0, &session);
    assert(call == XIAOTAI_ERR_PLATFORM_OFFLINE && !session);
    printf("local AI and device call explicitly refused: ai=%d call=%d\n", ai, call);
}

static void check_at_error(const char *command)
{
    char wire[512] = {0};
    while (xiaotai_console_host_take(wire, sizeof(wire))) { }
    memset(wire, 0, sizeof(wire));
    assert(xiaotai_console_host_feed(command, strlen(command)) == strlen(command));
    size_t used = 0;
    const int64_t deadline = xiaotai_time_us() + INT64_C(2500000);
    do {
        used += xiaotai_console_host_take(wire + used, sizeof(wire) - 1U - used);
        if (strstr(wire, "ERROR: 0x7101\r\n")) break;
        assert(used < sizeof(wire) - 1U && !strstr(wire, "OK\r\n"));
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    printf("AT bytes [%.*s] -> [%s]", (int)strlen(command) - 2, command, wire);
    assert(!strcmp(wire, "ERROR: 0x7101\r\n"));
}

static void check_worker_visible_failures(void)
{
    xiaotai_platform_status_t platform;
    int kind;
    assert(xiaotai_contacts_kind_find("device", &kind) == XIAOTAI_OK);
    xiaotai_contact_t peer;
    for (unsigned i = 0; i < 1600U; ++i) {
        xiaotai_platform_status(&platform);
        if (platform.phase == XIAOTAI_PLATFORM_ONLINE && platform.signaling &&
            xiaotai_contacts_find("peer", kind, 0, &peer) == XIAOTAI_OK) break;
        xiaotai_delay_ms(5);
    }
    assert(platform.phase == XIAOTAI_PLATFORM_ONLINE && platform.signaling &&
           product_platform_has_client(PRODUCT_FIXTURE_CLIENT));
    assert(xiaotai_contacts_find("peer", kind, 0, &peer) == XIAOTAI_OK && peer.online);
    if (PRODUCT_HOST_PLATFORM_CONSOLE) {
        check_at_error("AT+AI START\r\n");
        check_at_error("AT+CALL DEVICE peer\r\n");
    } else {
        product_on_owner(check_worker_commands_without_console, NULL);
    }
    product_on_owner(check_worker_call_idle, NULL);
    product_on_owner(watch_worker_toast, NULL);
    const xiaotai_args_t click = {0};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_MAIN, &click) == XIAOTAI_OK);
    product_on_owner(check_worker_ai_ui, NULL);
    xiaotai_platform_stats_t counts;
    xiaotai_platform_stats(&counts);
    assert(counts.discoveries == 1 && counts.logins == 1 && !xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
    printf("platform online: AI and device call via %s returned PLATFORM_OFFLINE; no activity or native RTC start\n",
           PRODUCT_HOST_PLATFORM_CONSOLE ? "AT" : "owner API");
}

static void check_worker_lifecycle(void)
{
    const uint32_t first_address = xiaotai_net_generation();
    const uint32_t first_join = product_platform_network_token();
    assert(first_address && first_join && xiaotai_net_has_saved_credentials());
    product_on_owner(check_worker_failure, NULL);
    check_worker_visible_failures();
    assert(xiaotai_net_disconnect() == XIAOTAI_OK);
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_LEAVE, 1U, 3000));
    const xiaotai_net_driver_event_t left = {.kind = XIAOTAI_NET_DRIVER_LOST, .token = first_join,
        .loss = XIAOTAI_NET_LOSS_REQUESTED};
    assert(xiaotai_net_host_emit(&left) == XIAOTAI_OK && xiaotai_net_host_sync(3000) == XIAOTAI_OK);
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    assert(net.state == XIAOTAI_NET_HELD && !net.generation && !net.control_pending &&
           net.control_error == XIAOTAI_OK && xiaotai_net_has_saved_credentials());
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_JOIN, 2U, 3000));
    const uint32_t next_join = product_platform_network_token();
    assert(next_join && next_join != first_join);
    product_platform_link_ready();
    xiaotai_net_status(&net);
    assert(net.state == XIAOTAI_NET_UP && net.generation && net.generation != first_address &&
           !net.user_disconnected && xiaotai_net_has_saved_credentials());
    product_on_owner(check_worker_failure, NULL);
    printf("Wi-Fi: manual leave and retry succeeded, address generation %u -> %u; RTC remains faulted\n",
           (unsigned)first_address, (unsigned)net.generation);
}

static void companion_in_foreground(void *unused)
{
    (void)unused;
    if (!strcmp(xiaotai_app_home(), "door")) {
        printf("door startup failure: prepare_notice=%u retry_action=%u\n",
               (unsigned)door_prepare_seen, (unsigned)door_prepare_offers_retry);
        assert(door_prepare_seen && !door_prepare_offers_retry);
        assert(xiaotai_app_switch("companion") == XIAOTAI_OK);
    }
    xiaotai_app_companion_status_t app;
    xiaotai_app_companion_status(&app);
    assert(app.foreground && app.setup && !strcmp(app.page, "preparing"));
    printf("companion startup failure: UI status=%s retry_action=%u\n",
           prepare_view_status, (unsigned)prepare_view_offers_retry);
    assert(prepare_view_seen && !strcmp(prepare_view_status, "ERR FFFF") && !prepare_view_offers_retry);
}

static void check_companion_start_failure(void)
{
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_START, 1U, 3000));
    assert(xiaotai_net_host_sync(3000) == XIAOTAI_OK);
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    assert(net.state == XIAOTAI_NET_BLOCKED && net.prepare.stage == XIAOTAI_NET_PREPARE_FAILED &&
           net.prepare.fault == XIAOTAI_NET_FAULT_START && net.prepare.error == XIAOTAI_FAIL &&
           net.prepare.retry_blocked && !net.control_pending);
    product_on_owner(companion_in_foreground, NULL);
    assert(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    xiaotai_net_maintenance_end(ticket);
}

static void unexpected_cloud_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    (void)reply;
    (void)ctx;
    assert(false && "cloud request admitted without platform worker");
}

static void check_dormant_platform_gate(void *ctx)
{
    (void)ctx;
    uint32_t ticket = 0, other = 0;
    assert(xiaotai_platform_gate_close_binding_idle(&ticket) == XIAOTAI_OK && ticket);
    assert(xiaotai_platform_gate_drained(ticket));
    assert(xiaotai_platform_gate_close(&other) == XIAOTAI_ERR_PLATFORM_GATED && !other);
    assert(xiaotai_platform_gate_open(ticket + 1U) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    assert(platform.gated && !platform.signaling);
    const xiaotai_platform_request_t request = {.service = XIAOTAI_PLATFORM_SERVICE_DEVICE,
        .method = XIAOTAI_PLATFORM_GET, .path = "/v1/device/profile", .auth = XIAOTAI_PLATFORM_AUTH_NONE};
    assert(xiaotai_platform_request(&request, unexpected_cloud_reply, NULL) == XIAOTAI_ERR_PLATFORM_OFFLINE);
    assert(xiaotai_platform_gate_open(ticket) == XIAOTAI_OK && !xiaotai_platform_gate_drained(ticket));
}

static void check_platform_worker_repair(void)
{
    product_check_owner();
    assert(xiaotai_net_host_sync(3000) == XIAOTAI_OK && xiaotai_transport_host_settle(3000));
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    xiaotai_transport_status_t transport;
    xiaotai_transport_status(&transport);
    printf("platform task create=%d attempts=%u reserve_attempts=%u reserve_held=%u signaling_suspended=%u\n",
           platform.last_error, atomic_load(&platform_worker_failures), atomic_load(&worker_repair_reserve_attempts),
           (unsigned)transport.start_reserve_held, (unsigned)transport.signaling_suspended);
    assert(atomic_load(&platform_worker_failures) == 1U && platform.last_error == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_platform_start_failed() && !xiaotai_platform_environment_boot_failed());
    assert(!platform.discovered && !platform.authorized && !platform.signaling && !platform.held && !platform.gated);
    assert(!transport.start_reserve_held && !transport.signaling_suspended && !atomic_load(&worker_repair_reserve_attempts));
    assert(xiaotai_platform_retry() == XIAOTAI_ERR_PLATFORM_OFFLINE);
    product_on_owner(check_dormant_platform_gate, NULL);
    assert_boot_settings();
    char stored[XIAOTAI_PLATFORM_URL_MAX];
    assert(xiaotai_kv_get_str("service_env", "ep_base", stored, sizeof(stored), 3000) == XIAOTAI_OK);
    assert(!strcmp(stored, "http://old.invalid"));

    product_expect_restart();
    char repaired[] = "http://repair.invalid";
    product_on_owner(request_environment_repair, repaired);
    for (unsigned i = 0; i < 2000U && !product_restarted(); ++i) xiaotai_delay_ms(5);
    assert(product_restarted());
    assert(xiaotai_kv_get_str("service_env", "ep_base", stored, sizeof(stored), 3000) == XIAOTAI_OK);
    assert(!strcmp(stored, repaired));
    assert_boot_settings();
    xiaotai_platform_stats_t stats;
    xiaotai_platform_stats(&stats);
    xiaotai_platform_status(&platform);
    assert(platform.last_error == XIAOTAI_ERR_NO_MEM && !platform.signaling);
    assert(!stats.discoveries && !stats.logins && !atomic_load(&unexpected_http) &&
           !product_platform_has_client(PRODUCT_FIXTURE_CLIENT) &&
           !xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
    puts("Environment repair: preserved settings and saved legacy key; no HTTP, MQTT or RTC before restart");
}

#if PRODUCT_TEST_COPROCESSOR
static void check_recovery_button(void *expected)
{
    const bool retry = *(const bool *)expected;
    const bool door = !strcmp(xiaotai_app_home(), "door");
    const bool shown = door ? door_prepare_seen : prepare_view_seen;
    const bool offered = door ? door_prepare_offers_retry : prepare_view_offers_retry;
    printf("%s recovery fault=%d notice=%u retry_action=%u expected=%u\n", xiaotai_app_home(),
           companion_recovery_fault, (unsigned)shown, (unsigned)offered, (unsigned)retry);
    fflush(stdout);
    assert(shown && offered == retry);
}

static void check_companion_recovery(void)
{
    const bool door = !strcmp(xiaotai_app_home(), "door");
    const bool retry = door || companion_recovery_fault == XIAOTAI_NET_FAULT_NO_LINK;
    xiaotai_net_status_t net;
    for (unsigned i = 0; i < 300; ++i) {
        xiaotai_net_status(&net);
        if (net.state == XIAOTAI_NET_BLOCKED) break;
        xiaotai_delay_ms(10);
    }
    assert(net.state == XIAOTAI_NET_BLOCKED && net.prepare.stage == XIAOTAI_NET_PREPARE_FAILED &&
           net.prepare.fault == companion_recovery_fault && net.prepare.retry_blocked != retry);
    product_on_owner(check_recovery_button, (void *)&retry);
    xiaotai_coprocessor_host_t *peer = xiaotai_coprocessor_host();
    peer->chip = companion_good_chip;
    peer->wait_result = peer->start_result = XIAOTAI_OK;
    if (!retry) {
        assert(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
        return;
    }
    assert(xiaotai_intent_post(XIAOTAI_INTENT_PROVISIONING_RETRY, &(xiaotai_args_t){0}) == XIAOTAI_OK);
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_START, 1U, 3000));
    product_platform_link_ready();
    xiaotai_net_status(&net);
    assert(net.state == XIAOTAI_NET_UP && net.generation && net.prepare.stage == XIAOTAI_NET_PREPARE_READY);
    puts("visible retry intent recovered the companion and brought Wi-Fi up");
}
#endif
#endif

void product_endpoint(const char *test)
{
#if PRODUCT_TEST_HOSTED_STARTUP_FAILURES
#if PRODUCT_TEST_COPROCESSOR
    if (companion_recovery_fault) {
        check_companion_recovery();
        return;
    }
#endif
    if (fail_companion_start) {
        check_companion_start_failure();
        return;
    }
#endif
    if (fail_start_reserve) {
        product_platform_online();
        xiaotai_transport_status_t transport;
        xiaotai_transport_status(&transport);
        assert(atomic_load(&start_reserve_failures) == 1U && !transport.start_reserve_held);
        assert(xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START) == 1U);
        assert(xiaotai_net_has_saved_credentials() && !product_restarted());
        puts("start reserve: allocation rejected once, no retained block, one RTC start and platform online");
        return;
    }
    if (watch_binding_view) product_on_owner(reject_offline_refresh, NULL);
    product_platform_link_ready();
#if PRODUCT_TEST_HOSTED_STARTUP_FAILURES
    if (fail_platform_worker) {
        check_platform_worker_repair();
        return;
    }
#endif
    if (service_policy(test)) {
        check_service_policy(test);
        return;
    }
#if PRODUCT_TEST_HOSTED_STARTUP_FAILURES
    if (fail_worker) {
        check_worker_lifecycle();
        return;
    }
#endif
    if (watch_binding_view) {
        check_environment_binding_failure();
        return;
    }
    if (test_is(test, "environment-healthy-bound") || test_is(test, "environment-healthy-unbound")) {
        check_healthy_environment();
        return;
    }
    if (test_is(test, "environment-boot-error")) {
        xiaotai_platform_environment_t env;
        xiaotai_platform_status_t status;
        xiaotai_platform_stats_t platform;
        xiaotai_transport_status_t transport;
        xiaotai_platform_environment(&env);
        xiaotai_platform_status(&status);
        xiaotai_platform_stats(&platform);
        xiaotai_transport_status(&transport);
        xiaotai_identity_stats_t identity;
        xiaotai_identity_stats(&identity);
        fprintf(stderr, "environment boot: error=%d platform=%u discovery=%u login=%u "
                        "transport=%u wait=%u reserve=%u reserve_attempts=%u hold=%u identity_loads=%u rtc_start=%u\n",
                (int)env.boot_error, (unsigned)status.phase, (unsigned)platform.discoveries,
                (unsigned)platform.logins, (unsigned)transport.state, (unsigned)transport.wait,
                (unsigned)transport.start_reserve_held, atomic_load(&bad_environment_reserve_attempts),
                (unsigned)status.held, (unsigned)identity.loads,
                (unsigned)xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
        assert(env.boot_error != XIAOTAI_OK && !env.active[0] && status.phase == XIAOTAI_PLATFORM_STOPPED);
        assert(!platform.discoveries && !platform.logins &&
               !xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
        assert(!transport.start_reserve_held && !atomic_load(&bad_environment_reserve_attempts));
        assert_boot_settings();
        char stored[XIAOTAI_PLATFORM_URL_MAX];
        assert(xiaotai_kv_get_str("service_env", "ep_base", stored, sizeof(stored), 3000) == XIAOTAI_OK);
        assert(!strcmp(stored, "ftp://invalid"));
        product_check_owner();
        product_expect_restart();
        char repaired[] = "http://repair.invalid";
        product_on_owner(request_environment_repair, repaired);
        for (unsigned i = 0; i < 2000U && !product_restarted(); ++i) xiaotai_delay_ms(5);
        assert(product_restarted());
        xiaotai_platform_environment(&env);
        assert(env.boot_error != XIAOTAI_OK && !env.active[0] &&
               !strcmp(env.next, "http://repair.invalid/services"));
        assert(xiaotai_kv_get_str("service_env", "ep_base", stored, sizeof(stored), 3000) == XIAOTAI_OK);
        assert(!strcmp(stored, repaired));
        assert_boot_settings();
        xiaotai_platform_stats(&platform);
        assert(!platform.discoveries && !platform.logins &&
               !xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
        puts("Environment repair: legacy key read back, no same-boot discovery or RTC, normal restart");
        return;
    }
    const bool retry = !strncmp(test, "retry-", sizeof("retry-") - 1U);
    const bool manual = test_is(test, "retry-manual");
    const uint32_t address = xiaotai_net_generation();
    const uint32_t binding = xiaotai_identity_generation();
    if (test_is(test, "redirect") || test_is(test, "mandatory") || startup_order) {
        xiaotai_platform_status_t status;
        for (unsigned i = 0; i < 1000U; ++i) {
            xiaotai_platform_status(&status);
            if (status.last_error) break;
            xiaotai_delay_ms(5);
        }
        assert(!status.discovered && status.last_error && !status.authorized);
        assert(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
        assert(!product_platform_has_client(PRODUCT_FIXTURE_CLIENT));
        printf("%s: entire discovery rejected, start=0 error=%d\n", test, status.last_error);
        if (!startup_order) return;
        xiaotai_transport_status_t transport;
        xiaotai_transport_status(&transport);
        assert(transport.start_reserve_held && atomic_load(&reserve_pointer));
        product_platform_discovery_status(200);
    }

    if (retry || test_is(test, "missing") || !strncmp(test, "bad", 3)) {
        xiaotai_platform_status_t platform;
        for (unsigned i = 0; i < 1000U; ++i) {
            xiaotai_platform_status(&platform);
            if (platform.discovered || platform.last_error) break;
            xiaotai_delay_ms(5);
        }
        fprintf(stderr, "%s first response: discovered=%u held=%u authorized=%u signal=%u error=%d\n",
                test, platform.discovered, platform.held, platform.authorized,
                platform.signaling, platform.last_error);
        assert(!platform.discovered && platform.last_error == XIAOTAI_ERR_PLATFORM_BAD_SERVICE);
        assert(!platform.authorized && !platform.signaling);
        assert(!product_platform_has_client(PRODUCT_FIXTURE_CLIENT));
        assert(!xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START));
        char value[XIAOTAI_PLATFORM_URL_MAX] = "stale";
        assert(xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_TIRTC, value, sizeof(value)) ==
               XIAOTAI_ERR_PLATFORM_OFFLINE && !value[0]);
        value[0] = 'X';
        assert(xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_MQTT, value, sizeof(value)) ==
               XIAOTAI_ERR_PLATFORM_OFFLINE && !value[0]);
        if (!retry) return;
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        assert(stats.discoveries == 1 && stats.logins == 0);
        if (manual) assert(platform.phase == XIAOTAI_PLATFORM_PAUSED);
        else assert(platform.phase != XIAOTAI_PLATFORM_PAUSED);
        product_platform_discovery_body(recovered);
        if (manual) assert(xiaotai_platform_retry() == XIAOTAI_OK);
    }

    product_platform_online();
    const char *expected = "http://rtc.invalid";
    if (test_is(test, "path")) expected = "http://rtc.invalid:8080/tenant";
    if (test_is(test, "ipv6") || startup_order) expected = startup_endpoint;
    if (test_is(test, "long")) expected = long_endpoint;
    endpoint(XIAOTAI_PLATFORM_SERVICE_TIRTC, expected);
    endpoint(XIAOTAI_PLATFORM_SERVICE_MQTT, broker);
    endpoint(XIAOTAI_PLATFORM_SERVICE_DEVICE, "http://fixture.invalid/device");
    endpoint(XIAOTAI_PLATFORM_SERVICE_AI, "http://fixture.invalid/device");
    endpoint(XIAOTAI_PLATFORM_SERVICE_CALL, "http://fixture.invalid/device");
    endpoint(XIAOTAI_PLATFORM_SERVICE_VOIP, "http://fixture.invalid/device");
    if (startup_order) {
        xiaotai_platform_stats_t before, after;
        xiaotai_platform_stats(&before);
        char small[] = {'X', 'G'};
        assert(xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_TIRTC, small, 1) ==
               XIAOTAI_ERR_INVALID_SIZE && small[0] == '\0' && small[1] == 'G');
        endpoint(XIAOTAI_PLATFORM_SERVICE_TIRTC, expected);
        xiaotai_platform_stats(&after);
        assert(before.discoveries == 2 && after.discoveries == before.discoveries);
        assert(address && binding && xiaotai_net_generation() == address &&
               xiaotai_identity_generation() == binding);
        assert(atomic_load(&startup_step) == 3);
        puts("startup-order: clock, endpoint published, free internal reserve, RTC online, MQTT start");
        puts("startup-order: 1 B output refused and cleared; same endpoint reread without rediscovery");
    }
    xiaotai_transport_host_start_t start;
    xiaotai_transport_host_last_start(&start);
    assert(xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START) == 1 && !strcmp(start.endpoint, expected));
    if (retry) {
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        assert(stats.discoveries == 2 && stats.logins == 1);
        assert(address && binding && xiaotai_net_generation() == address &&
               xiaotai_identity_generation() == binding);
        printf("%s: recovered via %s discovery retry: address generation=%u binding generation=%u\n",
               test, manual ? "explicit" : "scheduled", address, binding);
    }
    if (test_is(test, "recognition")) {
        endpoint(XIAOTAI_PLATFORM_SERVICE_RECOGNITION, "http://recognition.invalid:8081/tenant");
    } else {
        char value[XIAOTAI_PLATFORM_URL_MAX] = "stale";
        assert(xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_RECOGNITION, value, sizeof(value)) ==
               (test_is(test, "invalid-recognition") ? XIAOTAI_ERR_PLATFORM_BAD_SERVICE : XIAOTAI_ERR_NOT_SUPPORTED));
        assert(!value[0]);
    }
    printf("%s: RTC start endpoint lengths expected=%zu actual=%zu; HTTPS service rewrite and unchanged MQTT\n",
           test, strlen(expected), strlen(start.endpoint));
}
