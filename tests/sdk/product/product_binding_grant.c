// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_c3_binding_grant.py
// Replaces legacy: tests/regression/test_binding_grant_durability.py
// Replaces legacy: tests/regression/test_recognition_binding.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "product_platform.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_net_host.h"

void product_binding_grant_prepare(bool reload);
void product_binding_grant(bool reload);

enum { ID_BYTES = 65, KEY_BYTES = 257, RECORD_BYTES = ID_BYTES + KEY_BYTES, WAIT_MS = 8000, POLL_MS = 5 };
static atomic_uint logins;

static const char *header(const xiaotai_http_host_request_t *req, const char *name)
{
    const char *value = NULL;
    for (size_t i = 0; i < req->header_count; ++i) {
        if (strcmp(req->headers[i].name, name)) continue;
        assert(!value);
        value = req->headers[i].value;
    }
    assert(value && *value);
    return value;
}

static void observe_login(const xiaotai_http_host_request_t *req)
{
    const char *device = header(req, "X-Device-Id");
    assert(req->method == XIAOTAI_HTTP_POST && !strcmp(device, PRODUCT_FIXTURE_DEVICE));
    const char *stamp = header(req, "X-Timestamp");
    const char *nonce = header(req, "X-Nonce");
    const char *signature = header(req, "X-Signature");
    assert(strlen(nonce) == 16 && strlen(signature) == 44);
    /* All fields are produced with synthetic identity and the fixed host clock. */
    printf("XT-LOGIN: %s %s %s %s\n", device, stamp, nonce, signature);
    atomic_fetch_add(&logins, 1);
}

static void grant_report(void *ctx, const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    const char *path = "http://fixture.invalid/device/v1/device/report";
    if (strcmp(req->url, path)) { reply->status = 404; return; }
    /* The token expires six seconds after the fixture wall clock. */
    static const char body[] = "{\"code\":200,\"data\":{\"code\":\"012345\","
        "\"temp_token\":\"a.eyJleHAiOjE3MDAwMDAxMDZ9.b\",\"temp_client_id\":\"" PRODUCT_FIXTURE_GUEST "\"}}";
    assert(req->method == XIAOTAI_HTTP_POST);
    reply->status = 200;
    reply->body = (const uint8_t *)body;
    reply->body_size = sizeof(body) - 1;
}

static unsigned hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    assert(c >= 'a' && c <= 'f');
    return (unsigned)(c - 'a') + 10U;
}

static void restore_persisted_record(const char record[RECORD_BYTES])
{
    assert(memchr(record, '\0', ID_BYTES) && memchr(record + ID_BYTES, '\0', KEY_BYTES));
#if PRODUCT_TEST_BINDING_BLOB_LAYOUT
    assert(xiaotai_kv_set("xmini", "identity", record, RECORD_BYTES, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
#else
    assert(xiaotai_kv_set_str("tirtc_cfg", "device_id", record, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_set_str("tirtc_cfg", "secret", record + ID_BYTES, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
#endif
}

static void read_persisted_record(char record[RECORD_BYTES])
{
#if PRODUCT_TEST_BINDING_BLOB_LAYOUT
    size_t size = RECORD_BYTES;
    assert(xiaotai_kv_get("xmini", "identity", record, &size, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    assert(size == RECORD_BYTES);
#else
    assert(xiaotai_kv_get_str_strict("tirtc_cfg", "device_id", record, ID_BYTES,
        XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_get_str_strict("tirtc_cfg", "secret", record + ID_BYTES, KEY_BYTES,
        XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
#endif
}

void product_binding_grant_prepare(bool reload)
{
    product_platform_prepare(false);
    if (!reload) {
        product_platform_http_responder(grant_report, NULL);
        return;
    }
    char line[2 * RECORD_BYTES + 2];
    assert(fgets(line, (int)sizeof(line), stdin) && strlen(line) == 2 * RECORD_BYTES + 1 &&
           line[2 * RECORD_BYTES] == '\n');
    char record[RECORD_BYTES];
    for (size_t i = 0; i < sizeof(record); ++i)
        record[i] = (char)(hex_digit(line[2 * i]) << 4U | hex_digit(line[2 * i + 1]));
    restore_persisted_record(record);
    product_platform_login_observer(observe_login);
}

static xiaotai_binding_status_t binding(void)
{
    xiaotai_binding_status_t status;
    xiaotai_binding_status(&status);
    return status;
}

static xiaotai_err_t take(xiaotai_mqtt_host_publication_t *out, char topic[96], char body[40])
{
    return product_platform_take_publication(PRODUCT_FIXTURE_GUEST, out, topic, 96, body, 40);
}

static void lose_confirmation(void)
{
    product_platform_link_ready();
    for (unsigned i = 0; i < WAIT_MS / POLL_MS; ++i) {
        if (binding().phase == XIAOTAI_BINDING_WAITING && product_platform_has_client(PRODUCT_FIXTURE_GUEST)) break;
        xiaotai_delay_ms(POLL_MS);
    }
    const xiaotai_binding_status_t ready = binding();
    assert(ready.phase == XIAOTAI_BINDING_WAITING && !strcmp(ready.verification, "012345"));
    assert(!xiaotai_identity_matches(PRODUCT_FIXTURE_DEVICE, PRODUCT_FIXTURE_SECRET));
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    product_platform_withhold_puback(PRODUCT_FIXTURE_GUEST);

    xiaotai_kv_host_gate(false);
    product_platform_message_to(PRODUCT_FIXTURE_GUEST, "cmd", "{\"type\":\"auth_grant\",\"payload\":{"
        "\"device_id\":\"" PRODUCT_FIXTURE_DEVICE "\",\"device_key\":\"" PRODUCT_FIXTURE_SECRET "\"}}");
    assert(xiaotai_kv_host_wait_held(WAIT_MS) && binding().phase == XIAOTAI_BINDING_STORING);
    xiaotai_mqtt_host_publication_t pub;
    char topic[96], body[40];
    product_platform_message_to(PRODUCT_FIXTURE_GUEST, "cmd", "{\"type\":\"auth_grant\",\"payload\":{"
        "\"device_id\":\"second-identity\",\"device_key\":\"second-key\"}}");
    assert(take(&pub, topic, body) == XIAOTAI_ERR_NOT_FOUND);
    xiaotai_kv_host_gate(true);

    xiaotai_err_t received = XIAOTAI_ERR_NOT_FOUND;
    for (unsigned i = 0; i < WAIT_MS / POLL_MS; ++i) {
        received = take(&pub, topic, body);
        if (received == XIAOTAI_OK) break;
        assert(received == XIAOTAI_ERR_NOT_FOUND);
        xiaotai_delay_ms(POLL_MS);
    }
    assert(received == XIAOTAI_OK);
    static const char confirmation[] = "{\"ack\":true}";
    assert(pub.message_id > 0 && pub.qos == 1 && !strcmp(topic, "device/" PRODUCT_FIXTURE_GUEST "/ack"));
    assert(pub.size == sizeof(confirmation) - 1 && !memcmp(body, confirmation, sizeof(confirmation) - 1));
    assert(take(&pub, topic, body) == XIAOTAI_ERR_NOT_FOUND);

    xiaotai_binding_stats_t stats = {0};
    for (unsigned i = 0; i < WAIT_MS / POLL_MS; ++i) {
        xiaotai_binding_stats(&stats);
        if (stats.confirm_timeouts && binding().phase == XIAOTAI_BINDING_FAILED) break;
        xiaotai_delay_ms(POLL_MS);
    }
    assert(stats.grants == 1 && stats.stores == 1 && stats.confirm_timeouts == 1 && stats.confirmations == 0);
    const xiaotai_binding_status_t timed_out = binding();
    assert(timed_out.phase == XIAOTAI_BINDING_FAILED && timed_out.error == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_identity_matches(PRODUCT_FIXTURE_DEVICE, PRODUCT_FIXTURE_SECRET));
    char record[RECORD_BYTES] = {0};
    read_persisted_record(record);
    assert(!strcmp(record, PRODUCT_FIXTURE_DEVICE) && !strcmp(record + ID_BYTES, PRODUCT_FIXTURE_SECRET));
    static const char hex[] = "0123456789abcdef";
    char encoded[2 * RECORD_BYTES + 1] = {0};
    for (size_t i = 0; i < sizeof(record); ++i) {
        const uint8_t byte = (uint8_t)record[i];
        encoded[2 * i] = hex[byte >> 4U];
        encoded[2 * i + 1] = hex[byte & 15U];
    }
    printf("XT-IDENTITY: %s\n", encoded);
    puts("Binding grant: no ACK before durable commit; one ACK despite lost PUBACK");
}

void product_binding_grant(bool reload)
{
    if (!reload) {
        lose_confirmation();
        return;
    }
    product_platform_online();
    assert(atomic_load(&logins) == 1 && xiaotai_identity_matches(PRODUCT_FIXTURE_DEVICE, PRODUCT_FIXTURE_SECRET));
    assert(!product_platform_has_client(PRODUCT_FIXTURE_GUEST));
    puts("Cold process: loaded the same identity and brought up formal signaling");
}

#if PRODUCT_TEST_BINDING_SCOPE
#include "product_test.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/platform.h"

void product_binding_scope_prepare(const char *name);
void product_binding_scope(void);

typedef enum {
    SCOPE_FRESH, SCOPE_STORAGE_FAILED, SCOPE_STORAGE_TIMEOUT, SCOPE_REPORT_RETRY,
    SCOPE_RECONNECT, SCOPE_LOGIN_FAILED, SCOPE_LOGIN_UNBOUND, SCOPE_CONFIRMED,
} scope_scenario_t;

enum { SCOPE_INITIAL = 41 };
static scope_scenario_t scope_case;
static bool scope_enabled;
static xiaotai_signal_t scope_entered, scope_release;
static atomic_uint scope_attempts, scope_commits, scope_reports, scope_logins;
static const char scope_space[] = "xt_recognition", scope_key[] = "scope";

static int64_t scope_current(void)
{
    int64_t value = -1;
    assert(xiaotai_kv_get_int(scope_space, scope_key, XIAOTAI_KV_U32, &value, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    return value;
}

xiaotai_err_t __real_xiaotai_kv_set_int(const char *, const char *, xiaotai_kv_int_t, int64_t, uint32_t);
xiaotai_err_t __wrap_xiaotai_kv_set_int(const char *space, const char *key, xiaotai_kv_int_t type,
                                        int64_t value, uint32_t wait_ms)
{
    if (!scope_enabled || strcmp(space, scope_space) || strcmp(key, scope_key) || type != XIAOTAI_KV_U32)
        return __real_xiaotai_kv_set_int(space, key, type, value, wait_ms);
    const bool first = atomic_fetch_add(&scope_attempts, 1) == 0U;
    if (first) {
        xiaotai_signal_give(&scope_entered);
        assert(xiaotai_signal_take(&scope_release, WAIT_MS));
    }
    const bool fail = first && scope_case == SCOPE_STORAGE_FAILED;
    if (fail) xiaotai_kv_host_fail_key(scope_key, XIAOTAI_FAIL);
    xiaotai_err_t result = __real_xiaotai_kv_set_int(space, key, type, value, wait_ms);
    if (fail) xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    if (result == XIAOTAI_OK) {
        atomic_fetch_add(&scope_commits, 1);
        /* Keep the real committed value; the first caller sees a lost completion. */
        if (first && scope_case == SCOPE_STORAGE_TIMEOUT) result = XIAOTAI_ERR_TIMEOUT;
    }
    return result;
}

static bool scope_signed(void)
{
    return scope_case != SCOPE_FRESH && scope_case != SCOPE_RECONNECT && scope_case != SCOPE_LOGIN_FAILED;
}

static void scope_report(void *ctx, const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    if (strcmp(req->url, "http://fixture.invalid/device/v1/device/report")) {
        reply->status = 404;
        return;
    }
    assert(req->method == XIAOTAI_HTTP_POST);
    const unsigned n = atomic_fetch_add(&scope_reports, 1) + 1U;
    const unsigned committed = atomic_load(&scope_commits);
    const int64_t value = scope_current();
    assert(committed == n + (scope_case == SCOPE_STORAGE_TIMEOUT) && value == SCOPE_INITIAL + committed);
    if (scope_signed()) {
        assert(!strcmp(header(req, "X-Device-Id"), PRODUCT_FIXTURE_DEVICE));
        assert(strlen(header(req, "X-Signature")) == 44U);
    } else {
        for (size_t i = 0; i < req->header_count; ++i)
            assert(strcmp(req->headers[i].name, "X-Device-Id") && strcmp(req->headers[i].name, "X-Signature"));
    }
    printf("Binding scope: Report %u observed durable scope=%lld signed=%u\n",
           n, (long long)value, (unsigned)scope_signed());
    if (scope_case == SCOPE_REPORT_RETRY && n == 1U) {
        static const uint8_t refused[] = "{\"code\":40901}";
        reply->status = 200;
        reply->body = refused;
        reply->body_size = sizeof(refused) - 1U;
        return;
    }
    grant_report(ctx, req, reply);
}

static bool scope_token(const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    assert(!strcmp(header(req, "X-Device-Id"), PRODUCT_FIXTURE_DEVICE));
    const unsigned n = atomic_fetch_add(&scope_logins, 1) + 1U;
    static const uint8_t failed[] = "{\"code\":503}";
    static const uint8_t unbound[] = "{\"code\":6006}";
    if (scope_case == SCOPE_LOGIN_FAILED) {
        reply->status = 503;
        reply->body = failed;
        reply->body_size = sizeof(failed) - 1U;
        return true;
    }
    if (scope_signed() && scope_case != SCOPE_CONFIRMED && n == 1U) {
        reply->status = 410;
        reply->body = unbound;
        reply->body_size = sizeof(unbound) - 1U;
        return true;
    }
    return false;
}

void product_binding_scope_prepare(const char *name)
{
    static const char *const names[] = {"fresh", "storage_failed", "storage_timeout", "report_retry",
        "reconnect", "login_failed", "login_unbound", "confirmed"};
    unsigned chosen = 0;
    while (chosen < sizeof(names) / sizeof(*names) && strcmp(name, names[chosen])) ++chosen;
    assert(chosen < sizeof(names) / sizeof(*names));
    scope_case = (scope_scenario_t)chosen;
    assert(xiaotai_kv_set_int(scope_space, scope_key, XIAOTAI_KV_U32, SCOPE_INITIAL, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&scope_entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&scope_release) == XIAOTAI_OK);
    product_platform_prepare(scope_case != SCOPE_FRESH);
    product_platform_http_responder(scope_report, NULL);
    product_platform_login_override(scope_token);
    scope_enabled = true;
}

static void duplicate_scope(void *ctx)
{
    (void)ctx;
    assert(binding().phase == XIAOTAI_BINDING_PREPARING);
    assert(xiaotai_binding_start() == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_binding_retry() == XIAOTAI_ERR_INVALID_STATE);
}

static void read_scope_recognition(void *ctx)
{
    assert(xiaotai_recognition_status(ctx) == XIAOTAI_OK);
}

static void reject_record_during_scope(void)
{
    xiaotai_recognition_status_t before, after;
    product_on_owner(read_scope_recognition, &before);
    assert(xiaotai_recognition_record(1) == XIAOTAI_OK);
    product_on_owner(read_scope_recognition, &after);
    assert(after.decision_serial == before.decision_serial + 1U &&
           after.decision_action == XIAOTAI_RECOGNITION_ACTION_RECORD &&
           after.decision_error == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_recognition_stats_t counts;
    xiaotai_recognition_stats(&counts);
    assert(counts.recordings == 0 && counts.submissions == 0);
}

static void scope_retry(void *ctx)
{
    (void)ctx;
    assert(xiaotai_binding_retry() == XIAOTAI_OK);
}

static void scope_unbound(void *ctx)
{
    const bool confirmed = *(const bool *)ctx;
    xiaotai_platform_report_unbound(confirmed);
    xiaotai_platform_status_t status;
    xiaotai_platform_status(&status);
    assert(status.verifying == !confirmed);
}

static xiaotai_binding_status_t scope_wait_phase(xiaotai_binding_phase_t phase)
{
    xiaotai_binding_status_t status = binding();
    for (unsigned i = 0; i < WAIT_MS / POLL_MS && status.phase != phase; ++i) {
        xiaotai_delay_ms(POLL_MS);
        status = binding();
    }
    if (status.phase != phase)
        fprintf(stderr, "scope binding phase=%u expected=%u error=%d code=%ld\n",
                (unsigned)status.phase, (unsigned)phase, status.error, (long)status.code);
    assert(status.phase == phase);
    return status;
}

static void scope_no_attempt(void)
{
    assert(atomic_load(&scope_attempts) == 0 && atomic_load(&scope_reports) == 0);
    assert(scope_current() == SCOPE_INITIAL && !product_platform_has_client(PRODUCT_FIXTURE_GUEST));
    assert(xiaotai_identity_matches(PRODUCT_FIXTURE_DEVICE, PRODUCT_FIXTURE_SECRET));
}

void product_binding_scope(void)
{
    product_platform_link_ready();
    if (scope_case == SCOPE_LOGIN_FAILED) {
        xiaotai_platform_status_t status;
        for (unsigned i = 0; i < WAIT_MS / POLL_MS; ++i) {
            xiaotai_platform_status(&status);
            if (status.last_error == XIAOTAI_FAIL) break;
            xiaotai_delay_ms(POLL_MS);
        }
        assert(atomic_load(&scope_logins) >= 1 && status.last_error == XIAOTAI_FAIL && !status.signaling);
        scope_no_attempt();
        puts("Binding scope: general token failure had no scope or Report");
    } else if (scope_case == SCOPE_RECONNECT || scope_case == SCOPE_CONFIRMED) {
        product_platform_online();
        scope_no_attempt();
        bool confirmed = scope_case == SCOPE_CONFIRMED;
        product_on_owner(scope_unbound, &confirmed);
        if (!confirmed) {
            product_platform_online();
            assert(atomic_load(&scope_logins) >= 2);
            scope_no_attempt();
            puts("Binding scope: successful login and verification reconnect did not advance scope");
        }
    }

    if (scope_case != SCOPE_LOGIN_FAILED && scope_case != SCOPE_RECONNECT) {
        assert(xiaotai_signal_take(&scope_entered, WAIT_MS));
        assert(atomic_load(&scope_attempts) == 1 && atomic_load(&scope_reports) == 0);
        assert(scope_current() == SCOPE_INITIAL && !product_platform_has_client(PRODUCT_FIXTURE_GUEST));
        assert(binding().signed_rebinding == scope_signed() &&
               xiaotai_identity_matches(PRODUCT_FIXTURE_DEVICE, PRODUCT_FIXTURE_SECRET) == scope_signed());
        if (scope_case == SCOPE_CONFIRMED) assert(atomic_load(&scope_logins) == 1);
        product_on_owner(duplicate_scope, NULL);
        reject_record_during_scope();
        assert(atomic_load(&scope_attempts) == 1 && atomic_load(&scope_reports) == 0);
        puts("Binding scope: held write rejected recording and duplicate binding, emitted zero Reports");
        xiaotai_signal_give(&scope_release);

        const bool storage_failed = scope_case == SCOPE_STORAGE_FAILED;
        const bool storage_timeout = scope_case == SCOPE_STORAGE_TIMEOUT;
        const bool report_retry = scope_case == SCOPE_REPORT_RETRY;
        const bool retry = storage_failed || storage_timeout || report_retry;
        if (retry) {
            const xiaotai_binding_status_t error = scope_wait_phase(XIAOTAI_BINDING_FAILED);
            assert(error.error == (storage_timeout ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_FAIL));
            if (report_retry) assert(error.code == 40901);
            assert(atomic_load(&scope_attempts) == 1 && atomic_load(&scope_reports) == (unsigned)report_retry);
            assert(scope_current() == SCOPE_INITIAL + !storage_failed);
            assert(!product_platform_has_client(PRODUCT_FIXTURE_GUEST));
            printf("Binding scope: rejected phase error=%d scope=%lld; starting explicit retry\n",
                   error.error, (long long)scope_current());
            product_on_owner(scope_retry, NULL);
        }
        const xiaotai_binding_status_t ready = scope_wait_phase(XIAOTAI_BINDING_WAITING);
        assert(!strcmp(ready.verification, "012345") && ready.signed_rebinding == scope_signed());
        assert(atomic_load(&scope_attempts) == 1U + (unsigned)retry);
        assert(atomic_load(&scope_reports) == 1U + (unsigned)report_retry);
        assert(scope_current() == SCOPE_INITIAL + 1 + (storage_timeout || report_retry));
        if (scope_signed()) {
            product_platform_message_to(PRODUCT_FIXTURE_GUEST, "cmd", "{\"type\":\"auth_grant\",\"payload\":{"
                "\"device_id\":\"" PRODUCT_FIXTURE_DEVICE "\",\"device_key\":\"" PRODUCT_FIXTURE_SECRET "\"}}");
            product_platform_online();
            (void)scope_wait_phase(XIAOTAI_BINDING_BOUND);
            assert(atomic_load(&scope_attempts) == 1U + (unsigned)retry);
            assert(atomic_load(&scope_reports) == 1U + (unsigned)report_retry);
            assert(scope_current() == SCOPE_INITIAL + 1 + (storage_timeout || report_retry));
            assert(!product_platform_has_client(PRODUCT_FIXTURE_GUEST));
            puts("Binding scope: signed rebind restored the session without another scope or Report");
        }
    }
    xiaotai_signal_deinit(&scope_release);
    xiaotai_signal_deinit(&scope_entered);
}
#endif
