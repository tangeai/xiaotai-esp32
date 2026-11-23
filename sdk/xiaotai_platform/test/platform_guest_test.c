// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_c3_mqtt_frames.py
// Replaces legacy: tests/regression/test_binding_prompt_cancel.py
// Replaces legacy: tests/regression/test_binding_grant_durability.py
// Replaces legacy: tests/regression/test_c3_binding_grant.py
// Replaces legacy: tests/regression/test_c3_mqtt_readiness.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "xiaotai/framework.h"
#include "xiaotai_http_host.h"
#include "xiaotai_mqtt_host.h"
#include "xiaotai_os_host.h"

enum { WAIT = 3000 };
static const uint8_t mac[] = {2, 1, 2, 3, 4, 5};
static const char grant[] = "{\"type\":\"auth_\\u0067rant\",\"channel\":\"fixture\",\"value\":42}";
static const char guest_topic[] = "device/temp-fixture/cmd", ack_topic[] = "device/temp-fixture/ack";
static atomic_uintptr_t guest_client;
static atomic_uint ready, messages, completed, closed, stopped, destroyed, stops, logins;
static atomic_int close_result, ack_result, subscribe_packet;
static atomic_bool valid_identity, fail_stop;
static bool refuse_conn, refuse_sub, withhold_sub, login_case, reject_ack_enqueue;
static bool legacy_subscribe_13s;
static const char *subscribe_case;
static atomic_int_fast64_t subscribe_clock_us;
static xiaotai_mqtt_handler_t subscribe_handler;
static void *subscribe_context;
static xiaotai_signal_t rendezvous, blocked, allow_owner;
static uint32_t guest_id, ack_ms = 5000;
static xiaotai_err_t open_result;

static xiaotai_mqtt_t *client(void) { return (xiaotai_mqtt_t *)atomic_load(&guest_client); }

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    const int64_t fixed = atomic_load(&subscribe_clock_us);
    return fixed ? fixed : __real_xiaotai_time_us();
}

xiaotai_identity_state_t __wrap_xiaotai_identity_state(void)
{
    return atomic_load(&valid_identity) ? XIAOTAI_IDENTITY_VALID : XIAOTAI_IDENTITY_ABSENT;
}

xiaotai_err_t __wrap_xiaotai_identity_get(xiaotai_identity_t *out)
{
    if (!atomic_load(&valid_identity)) return XIAOTAI_ERR_INVALID_STATE;
    *out = (xiaotai_identity_t){.device_id = "fixture", .device_key = "local-fixture-key"};
    return XIAOTAI_OK;
}

xiaotai_err_t __real_xiaotai_mqtt_create(const xiaotai_mqtt_config_t *, xiaotai_mqtt_t **);
xiaotai_err_t __wrap_xiaotai_mqtt_create(const xiaotai_mqtt_config_t *config, xiaotai_mqtt_t **out)
{
    assert(!xiaotai_on_owner());
    xiaotai_mqtt_config_t limited;
    if (reject_ack_enqueue && !strcmp(config->client_id, "temp-fixture")) {
        limited = *config;
        limited.outbox_limit_bytes = 1U;
        config = &limited;
    }
    xiaotai_err_t result = __real_xiaotai_mqtt_create(config, out);
    if (result == XIAOTAI_OK && !strcmp(config->client_id, "temp-fixture")) {
        subscribe_handler = config->handler;
        subscribe_context = config->ctx;
        assert(config->task_stack_bytes == platform.config.signaling_stack_bytes);
        assert(config->event_stack_bytes == 0U);
        assert(!client() && !strcmp(config->uri, "mqtt://fixture"));
        assert(!strcmp(config->username, "temp-fixture") && !strcmp(config->password, "local-fixture-token"));
        assert(config->rx_buffer_bytes == 64 && config->tx_buffer_bytes == 64);
        if (refuse_conn) xiaotai_mqtt_host_connack(*out, XIAOTAI_MQTT_ERROR_REFUSED, 5);
        const uint8_t refused[] = {XIAOTAI_MQTT_REFUSED};
        assert(xiaotai_mqtt_host_suback(*out, !withhold_sub, refuse_sub ? refused : NULL, refuse_sub ? 1 : 0)
               == XIAOTAI_OK);
        xiaotai_mqtt_host_puback(*out, false);
        atomic_store(&guest_client, (uintptr_t)*out);
    }
    return result;
}

xiaotai_err_t __real_xiaotai_mqtt_subscribe(xiaotai_mqtt_t *, const xiaotai_mqtt_topic_t *, size_t, int32_t *);
xiaotai_err_t __wrap_xiaotai_mqtt_subscribe(xiaotai_mqtt_t *c, const xiaotai_mqtt_topic_t *topics, size_t count,
                                          int32_t *packet)
{
    const xiaotai_err_t result = __real_xiaotai_mqtt_subscribe(c, topics, count, packet);
    if (c == client()) {
        assert(count == 1 && topics[0].qos == 1 && !strcmp(topics[0].topic, guest_topic));
        assert(result == XIAOTAI_OK && packet && *packet > 0);
        atomic_store(&subscribe_packet, *packet);
    }
    return result;
}

xiaotai_err_t __real_xiaotai_mqtt_stop(xiaotai_mqtt_t *);
xiaotai_err_t __wrap_xiaotai_mqtt_stop(xiaotai_mqtt_t *c)
{
    const bool is_guest = c && c == client();
    if (is_guest) {
        ++stops;
        if (atomic_exchange(&fail_stop, false)) return XIAOTAI_ERR_TIMEOUT;
    }
    const xiaotai_err_t result = __real_xiaotai_mqtt_stop(c);
    if (is_guest && result == XIAOTAI_OK) ++stopped;
    return result;
}

xiaotai_err_t __real_xiaotai_mqtt_destroy(xiaotai_mqtt_t *);
xiaotai_err_t __wrap_xiaotai_mqtt_destroy(xiaotai_mqtt_t *c)
{
    const bool is_guest = c && c == client();
    const xiaotai_err_t result = __real_xiaotai_mqtt_destroy(c);
    if (is_guest && result == XIAOTAI_OK) {
        assert(atomic_load(&stopped) == atomic_load(&destroyed) + 1U);
        atomic_store(&guest_client, (uintptr_t)NULL);
        ++destroyed;
    }
    return result;
}

static void until(atomic_uint *counter, unsigned number)
{
    for (unsigned elapsed = 0; atomic_load(counter) < number && elapsed < WAIT; ++elapsed)
        xiaotai_delay_ms(1);
    assert(atomic_load(counter) == number);
}

static void signal_main(void *ctx) { xiaotai_signal_give(ctx); }
static void owner_fence(void *ctx) { (void)ctx; }
static void owner(void (*fn)(void *))
{
    assert(xiaotai_run(fn, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(signal_main, &rendezvous) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&rendezvous, WAIT));
}

static void seen_ready(uint32_t id, void *ctx)
{
    assert(id == guest_id && ctx == &ready && xiaotai_on_owner());
    ++ready;
}

static void seen_message(uint32_t id, const xiaotai_platform_signal_t *s, void *ctx)
{
    assert(id == guest_id && ctx == &ready && xiaotai_on_owner());
    assert(s->generation == xiaotai_identity_generation() && s->command);
    assert(s->size == sizeof(grant) - 1U && !strcmp(s->json, grant));
    assert(!strcmp(s->type, "auth_grant") && !strcmp(s->channel, "fixture"));
    ++messages;
}

static void seen_close(uint32_t id, xiaotai_err_t result, void *ctx)
{
    assert(id == guest_id && ctx == &ready && xiaotai_on_owner() && !client());
    atomic_store(&close_result, result);
    ++closed;
}

static void done(xiaotai_err_t result, void *ctx)
{
    assert(ctx == &ready && xiaotai_on_owner());
    atomic_store(&ack_result, result);
    ++completed;
}

static void open_guest(void *ctx)
{
    (void)ctx;
    char id[] = "temp-fixture", token[] = "local-fixture-token";
    const int64_t cap_us = subscribe_case ? xiaotai_time_us() +
        (!strcmp(subscribe_case, "subscribe_jwt_cap") ? 1250000 : 20000000) : 0;
    const xiaotai_platform_guest_t config = {.client_id = id, .token = token, .ready = seen_ready,
        .message = seen_message, .closed = seen_close, .ctx = &ready,
        .subscribe_timeout_s = subscribe_case ? 20 : 0, .initial_subscribe_deadline_us = cap_us};
    open_result = xiaotai_platform_guest_open(&config, &guest_id);
    if (open_result == XIAOTAI_OK) {
        uint32_t abandoned = 123;
        assert(xiaotai_platform_guest_open(&config, &abandoned) == XIAOTAI_ERR_BUSY && !abandoned);
    }
    memset(id, 'x', sizeof(id) - 1U);
    memset(token, 'x', sizeof(token) - 1U);
}

static void acknowledge(void *ctx)
{
    (void)ctx;
    assert(xiaotai_platform_guest_ack(guest_id, ack_ms, done, &ready) == XIAOTAI_OK);
    assert(xiaotai_platform_guest_ack(guest_id, ack_ms, done, &ready) == XIAOTAI_ERR_BUSY);
}

static void close_guest(void *ctx)
{
    (void)ctx;
    xiaotai_platform_guest_close(guest_id);
}

static void response(void *ctx, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    const char *body;
    if (!strcmp(request->url, "http://fixture/services")) {
        assert(request->method == XIAOTAI_HTTP_GET);
        body = "{\"device-srv\":\"http://fixture/device\",\"mqtt-srv\":\"mqtt://fixture\"}";
        reply->status = 200;
    } else {
        assert(login_case && !strcmp(request->url, "http://fixture/device/v1/device/token"));
        assert(request->method == XIAOTAI_HTTP_POST && !client() && atomic_load(&destroyed) == 1);
        assert(atomic_load(&closed) == 1 && atomic_load(&close_result) == XIAOTAI_ERR_INVALID_STATE);
        assert(atomic_load(&completed) == 1 && atomic_load(&ack_result) == XIAOTAI_ERR_PLATFORM_STALE);
        ++logins;
        body = "{\"code\":6006}";
        reply->status = 410;
    }
    reply->body = (const uint8_t *)body;
    reply->body_size = strlen(body);
    reply->content_length = (int64_t)reply->body_size;
}

static void boot(void)
{
    static const char *const needs[] = {"identity", NULL};
    static const xiaotai_module_t identity = {.name = "identity", .init = xiaotai_engine_identity_init};
    static const xiaotai_module_t engine = {.name = "platform", .depends = needs,
        .init = xiaotai_engine_platform_init, .start = xiaotai_engine_platform_start};
    static const xiaotai_module_t *const modules[] = {&identity, &engine};
    assert(xiaotai_signal_init(&rendezvous) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&blocked) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&allow_owner) == XIAOTAI_OK);
    xiaotai_http_host_set_responder(response, NULL, false);
    assert(xiaotai_platform_configure(&(xiaotai_platform_config_t){.discovery_url = "http://fixture/services",
        .subscribe_timeout_s = legacy_subscribe_13s || subscribe_case ? 12 : 2,
        .signaling_buffer_bytes = 64, .reconnect_ms = 100}) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 2}) == XIAOTAI_OK);
    owner(open_guest);
    assert(open_result == XIAOTAI_ERR_PLATFORM_OFFLINE && !guest_id);
    xiaotai_platform_set_network(true, mac);
    xiaotai_platform_status_t status;
    for (unsigned i = 0; i <= WAIT; ++i) {
        xiaotai_platform_status(&status);
        if (status.phase == XIAOTAI_PLATFORM_NO_IDENTITY) break;
        xiaotai_delay_ms(1);
    }
    assert(status.phase == XIAOTAI_PLATFORM_NO_IDENTITY && status.discovered);
    owner(open_guest);
    assert(open_result == XIAOTAI_OK && guest_id);
}

static uint16_t take(xiaotai_mqtt_t *c, const char *expected_topic, const char *expected_text)
{
    xiaotai_mqtt_host_publication_t published;
    char topic[80], data[40];
    xiaotai_err_t result = XIAOTAI_ERR_NOT_FOUND;
    for (unsigned i = 0; i < WAIT && result == XIAOTAI_ERR_NOT_FOUND; ++i) {
        result = xiaotai_mqtt_host_take(c, &published, topic, sizeof(topic), data, sizeof(data));
        if (result == XIAOTAI_ERR_NOT_FOUND) xiaotai_delay_ms(1);
    }
    assert(result == XIAOTAI_OK && published.qos == 1 && published.message_id > 0);
    assert(!strcmp(topic, expected_topic) && strlen(expected_text) == published.size &&
           !memcmp(data, expected_text, published.size));
    return (uint16_t)published.message_id;
}

static void fixture_message(xiaotai_mqtt_t *c)
{
    const size_t parts[] = {0, 15, 6, sizeof(grant) - 22U};
    assert(xiaotai_mqtt_host_message(c, "device/temp-fixture/notify", grant, sizeof(grant) - 1U, 1, 12,
        false, false, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_message(c, guest_topic, grant, sizeof(grant) - 1U, 1, 13,
        false, false, parts, sizeof(parts) / sizeof(*parts)) == XIAOTAI_OK);
    until(&messages, 1);
}

static void test_wire(void)
{
    until(&ready, 1);
    xiaotai_mqtt_t *c = client();
    assert(c && xiaotai_mqtt_host_subscriptions(c) == 1);
    xiaotai_mqtt_host_publication_t publication;
    char t[80], b[40];
    const char trailing[] = "{\"type\":\"auth_grant\",\"payload\":{\"device_id\":\"new-id\",\"device_key\":\"new-key\"}} trailing";
    const char embedded[] = "{\"type\":\"auth_grant\",\"payload\":{\"device_id\":\"new-id\",\"device_key\":\"new-key\"}}\0junk";
    assert(xiaotai_mqtt_host_message(c, guest_topic, trailing, sizeof(trailing) - 1U, 1, 10,
        false, false, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_message(c, guest_topic, embedded, sizeof(embedded) - 1U, 1, 11,
        false, false, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(c, WAIT) == XIAOTAI_OK && atomic_load(&messages) == 0);
    assert(xiaotai_mqtt_host_take(c, &publication, t, sizeof(t), b, sizeof(b)) == XIAOTAI_ERR_NOT_FOUND);
    fixture_message(c);
    assert(xiaotai_mqtt_host_take(c, &publication, t, sizeof(t), b, sizeof(b)) == XIAOTAI_ERR_NOT_FOUND);
    owner(acknowledge);
    const uint16_t id = take(c, ack_topic, "{\"ack\":true}");
    int32_t noise = 0;
    assert(xiaotai_mqtt_publish(c, "fixture/noise", "x", 1, 1, XIAOTAI_MQTT_SEND_QUEUED, &noise) == XIAOTAI_OK);
    assert(take(c, "fixture/noise", "x") == noise && noise != id);
    assert(xiaotai_mqtt_host_ack(c, (uint16_t)noise) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(c, WAIT) == XIAOTAI_OK && !atomic_load(&completed));
    assert(xiaotai_mqtt_host_ack(c, id) == XIAOTAI_OK);
    until(&completed, 1);
    assert(atomic_load(&ack_result) == XIAOTAI_OK);
    owner(acknowledge);
    const uint16_t retransmitted = take(c, ack_topic, "{\"ack\":true}");
    assert(xiaotai_mqtt_host_disconnect(c) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(c, WAIT) == XIAOTAI_OK);
    xiaotai_mqtt_host_advance_ms(c, 1000);
    until(&ready, 2);
    assert(client() == c && !atomic_load(&closed));
    assert(take(c, ack_topic, "{\"ack\":true}") == retransmitted);
    assert(xiaotai_mqtt_host_ack(c, retransmitted) == XIAOTAI_OK);
    until(&completed, 2);
    xiaotai_mqtt_host_puback(c, true);
    owner(acknowledge);
    (void)take(c, ack_topic, "{\"ack\":true}");
    until(&completed, 3);
    atomic_store(&fail_stop, true);
    owner(close_guest);
    until(&destroyed, 1);
    assert(atomic_load(&stops) == 2 && !atomic_load(&closed));
}

static void test_close(xiaotai_err_t expected)
{
    until(&destroyed, 1);
    until(&closed, 1);
    assert(atomic_load(&close_result) == expected && !client());
}

static void test_timeout(void)
{
    for (unsigned i = 0; !atomic_load(&subscribe_packet) && i < WAIT; ++i) xiaotai_delay_ms(1);
    assert(atomic_load(&subscribe_packet) > 0 && !atomic_load(&ready));
    xiaotai_os_host_advance_us(3000000);
    platform_wake();
    test_close(XIAOTAI_ERR_TIMEOUT);
}

static bool test_legacy_subscribe_13s(void)
{
    for (unsigned i = 0; !atomic_load(&subscribe_packet) && i < WAIT; ++i) xiaotai_delay_ms(1);
    assert(atomic_load(&subscribe_packet) > 0 && !atomic_load(&ready));
    /* The old provisional client had one SUBSCRIBE and the caller's 20 s
     * wait, with no independent 12 s timeout in its MQTT callback. */
    xiaotai_os_host_advance_us(13000000);
    platform_wake();
    for (unsigned i = 0; i < 250 && !atomic_load(&closed); ++i) xiaotai_delay_ms(1);
    fprintf(stderr, "guest at 13000ms: closed=%u destroys=%u err=%d ready=%u\n", atomic_load(&closed),
        atomic_load(&destroyed), atomic_load(&close_result), atomic_load(&ready));
    if (!client() || atomic_load(&closed) || atomic_load(&ready)) return false;
    assert(xiaotai_mqtt_host_suback_reply(client(), (uint16_t)atomic_load(&subscribe_packet)) == XIAOTAI_OK);
    until(&ready, 1);
    owner(close_guest);
    until(&destroyed, 1);
    return true;
}

static void test_manual(void)
{
    until(&ready, 1);
    owner(acknowledge);
    (void)take(client(), ack_topic, "{\"ack\":true}");
    owner(close_guest);
    until(&destroyed, 1);
    assert(!atomic_load(&completed) && !atomic_load(&closed));
}

static void test_login(void)
{
    until(&ready, 1);
    owner(acknowledge);
    (void)take(client(), ack_topic, "{\"ack\":true}");
    atomic_store(&valid_identity, true);
    assert(xiaotai_platform_retry() == XIAOTAI_OK);
    until(&logins, 1);
    assert(atomic_load(&closed) == 1 && atomic_load(&destroyed) == 1 && atomic_load(&completed) == 1);
}

static void block_owner(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&blocked);
    assert(xiaotai_signal_take(&allow_owner, WAIT));
}

static void noop(void *ctx) { (void)ctx; }

static void test_subscribe_override(void)
{
    for (unsigned i = 0; !atomic_load(&subscribe_packet) && i < WAIT; ++i) xiaotai_delay_ms(1);
    assert(atomic_load(&subscribe_packet) > 0 && !atomic_load(&ready) && subscribe_handler);
    assert(xiaotai_mqtt_host_flush(client(), WAIT) == XIAOTAI_OK);
    const bool owner_pressure = !strcmp(subscribe_case, "subscribe_owner_deadline");
    const bool accepted = !strcmp(subscribe_case, "subscribe_13s") || !strcmp(subscribe_case, "subscribe_before_20s");
    const int64_t elapsed_us = !strcmp(subscribe_case, "subscribe_13s") ? 13000000 :
        (!strcmp(subscribe_case, "subscribe_jwt_cap") ? 1250000 :
         (accepted || owner_pressure ? 19999999 : 20000000));
    if (owner_pressure) {
        assert(xiaotai_run(block_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&blocked, WAIT));
    }
    atomic_fetch_add(&subscribe_clock_us, elapsed_us);
    const uint8_t qos = 1;
    subscribe_handler(subscribe_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_SUBSCRIBED,
        .message_id = atomic_load(&subscribe_packet), .granted = &qos, .granted_count = 1});
    if (owner_pressure) {
        assert(!atomic_load(&ready) && !atomic_load(&closed));
        atomic_fetch_add(&subscribe_clock_us, 1);
        xiaotai_signal_give(&allow_owner);
    }
    if (accepted) {
        until(&ready, 1);
        assert(!atomic_load(&closed));
        owner(close_guest);
        until(&destroyed, 1);
    } else {
        test_close(XIAOTAI_ERR_TIMEOUT);
        assert(!atomic_load(&ready) && atomic_load(&closed) == 1U && atomic_load(&stops) == 1U);
    }
    printf("%s: accepted=%u ready=%u closes=%u\n", subscribe_case, (unsigned)accepted,
           atomic_load(&ready), atomic_load(&closed));
}

static void test_pressure(void)
{
    for (unsigned i = 0; !atomic_load(&subscribe_packet) && i < WAIT; ++i) xiaotai_delay_ms(1);
    assert(atomic_load(&subscribe_packet) > 0 && !atomic_load(&ready));
    assert(xiaotai_run(block_owner, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&blocked, WAIT));
    while (xiaotai_run(noop, NULL) == XIAOTAI_OK) {}
    assert(xiaotai_mqtt_host_suback_reply(client(), (uint16_t)atomic_load(&subscribe_packet)) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(client(), WAIT) == XIAOTAI_OK);
    assert(!atomic_load(&ready));
    xiaotai_signal_give(&allow_owner);
    until(&ready, 1);
    assert(xiaotai_run(block_owner, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&blocked, WAIT));
    assert(xiaotai_mqtt_host_message(client(), guest_topic, grant, sizeof(grant) - 1U, 1, 13,
        false, false, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(client(), WAIT) == XIAOTAI_OK);
    (void)xiaotai_identity_generation_advance();
    xiaotai_signal_give(&allow_owner);
    owner(noop);
    assert(!atomic_load(&messages));
    fixture_message(client());
    owner(close_guest);
    until(&destroyed, 1);
}

static void test_ack_timeout(void)
{
    until(&ready, 1);
    ack_ms = 100;
    owner(acknowledge);
    const uint16_t old = take(client(), ack_topic, "{\"ack\":true}");
    xiaotai_os_host_advance_us(101000);
    platform_wake();
    until(&completed, 1);
    assert(atomic_load(&ack_result) == XIAOTAI_ERR_TIMEOUT);
    ack_ms = 5000;
    owner(acknowledge);
    const uint16_t current = take(client(), ack_topic, "{\"ack\":true}");
    assert(current != old && xiaotai_mqtt_host_ack(client(), old) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(client(), WAIT) == XIAOTAI_OK && atomic_load(&completed) == 1);
    assert(xiaotai_mqtt_host_ack(client(), current) == XIAOTAI_OK);
    until(&completed, 2);
    assert(atomic_load(&ack_result) == XIAOTAI_OK);
    owner(close_guest);
    until(&destroyed, 1);
}

static void test_ack_enqueue_rejected(void)
{
    until(&ready, 1);
    xiaotai_mqtt_t *c = client();
    assert(c);
    owner(acknowledge);
    until(&completed, 1);
    assert(atomic_load(&ack_result) == XIAOTAI_ERR_NO_MEM && !atomic_load(&closed));
    xiaotai_mqtt_stats_t stats;
    xiaotai_mqtt_stats(c, &stats);
    assert(stats.publish_failures == 1U && stats.outbox_full == 1U);

    xiaotai_os_host_advance_us(6000000);
    platform_wake();
    assert(xiaotai_mqtt_host_flush(c, WAIT) == XIAOTAI_OK);
    owner(owner_fence);
    xiaotai_mqtt_host_publication_t publication;
    char topic[80], body[40];
    assert(xiaotai_mqtt_host_take(c, &publication, topic, sizeof(topic), body, sizeof(body)) == XIAOTAI_ERR_NOT_FOUND);
    xiaotai_mqtt_stats(c, &stats);
    assert(stats.publish_failures == 1U && atomic_load(&completed) == 1);
    owner(close_guest);
    until(&destroyed, 1);
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 2);
    const char *what = argc == 2 ? argv[1] : "wire";
    refuse_conn = !strcmp(what, "refused");
    refuse_sub = !strcmp(what, "sub_refused");
    legacy_subscribe_13s = !strcmp(what, "legacy_subscribe_13s");
    if (!strncmp(what, "subscribe_", sizeof("subscribe_") - 1U)) {
        subscribe_case = what;
        atomic_store(&subscribe_clock_us, 1000000);
    }
    withhold_sub = subscribe_case || legacy_subscribe_13s || !strcmp(what, "timeout") || !strcmp(what, "owner_pressure");
    login_case = !strcmp(what, "login");
    reject_ack_enqueue = !strcmp(what, "ack_enqueue_rejected");
    boot();
    bool matches_old = true;
    if (refuse_conn || refuse_sub) test_close(XIAOTAI_FAIL);
    else if (!strcmp(what, "timeout")) test_timeout();
    else if (legacy_subscribe_13s) matches_old = test_legacy_subscribe_13s();
    else if (subscribe_case) test_subscribe_override();
    else if (!strcmp(what, "offline")) {
        until(&ready, 1);
        xiaotai_platform_set_network(false, NULL);
        test_close(XIAOTAI_ERR_PLATFORM_OFFLINE);
    } else if (login_case) test_login();
    else if (!strcmp(what, "manual")) test_manual();
    else if (!strcmp(what, "owner_pressure")) test_pressure();
    else if (!strcmp(what, "ack_timeout")) test_ack_timeout();
    else if (reject_ack_enqueue) test_ack_enqueue_rejected();
    else { assert(!strcmp(what, "wire")); test_wire(); }
    xiaotai_platform_reset();
    xiaotai_framework_reset();
    xiaotai_identity_reset();
    xiaotai_signal_deinit(&rendezvous);
    xiaotai_signal_deinit(&blocked);
    xiaotai_signal_deinit(&allow_owner);
    assert(!client());
    if (!matches_old) return 1;
    puts("Guest signaling scenario passed using the in-process broker and actual owner");
    return 0;
}
