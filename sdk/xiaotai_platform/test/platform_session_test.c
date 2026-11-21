// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_xmini_ai_token_http.py
// Replaces legacy: tests/xmini/test_platform_control.c::platform_control
// Replaces legacy: tests/xmini/test_platform_state.c::platform_state
// Replaces legacy: tests/regression/test_binding_lifecycle.py
// Replaces legacy: tests/regression/test_p4_mqtt_commands.py
// Replaces legacy: tests/regression/test_p4_mqtt_readiness.py
// Replaces legacy: tests/regression/test_c3_mqtt_refresh_window.py
// Replaces legacy: tests/regression/test_c3_mqtt_token_admission.py
// Replaces legacy: tests/regression/test_c3_mqtt_readiness.py
// Replaces legacy: tests/regression/test_mqtt_readiness.py
// Replaces legacy: tests/regression/test_device_profile_response.py
// Replaces legacy: tests/regression/test_platform_token_binding.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_voip_profile_delivery_recovery
// Replaces legacy: tests/regression/test_voice_reliability.py::test_external_connect_memory
// Replaces legacy: tests/regression/test_voice_reliability.py::test_mqtt_lifetime
// Replaces legacy: tests/regression/test_queued_http.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "xiaotai/platform_config.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/net.h"
#include "xiaotai/platform_config.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_clock_host.h"
#include "xiaotai_http_host.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_mqtt_host.h"

#define WAIT 3000U
#define WALL_SECONDS 1700000100LL
static const uint8_t mac[6] = {0x02, 0xAB, 0xCD, 0xEE, 0xFF, 0x10};
static const char discovery[] = "{\"device-srv\":\"https://fixture/device/\",\"mqtt-srv\":\"mqtts://fixture\","
    "\"ai-srv\":\"http://fixture/ai\",\"recognition-srv\":34}";
static const char valid_login[] = "{\"code\":200,\"data\":{\"mqtt_token\":\"fixture-token\"}}";
static const char refreshing_login[] = "{\"code\":200,\"data\":{\"mqtt_token\":\"a.eyJleHAiOjE3MDAwMDAxMzB9.b\"}}";
static const char refreshed_login[] = "{\"code\":200,\"data\":{\"mqtt_token\":\"a.eyJleHAiOjE3MDAwMDA1MDB9.b\"}}";
static const char short_login[] = "{\"code\":200,\"data\":{\"mqtt_token\":\"a.eyJleHAiOjE3MDAwMDAxMDJ9.b\"}}";
static const char short_second_login[] = "{\"code\":200,\"data\":{\"mqtt_token\":\"a.eyJleHAiOjE3MDAwMDAxMDR9.b\"}}";
static const char *services_json = discovery, *login_json = valid_login;
static const char *expected_mac = "02:AB:CD:EE:FF:10", *expected_device = "fixture";
static const char *expected_broker = "mqtts://fixture";
static bool provision, require_ai, config_clock, refresh_fixture, signature_fixture, auth_link, start_fails;
static bool block_discovery, block_start, short_fixture;
static bool active_media_fixture;
static uint32_t media_id;
static atomic_uint media_submit, media_owner_turn, media_reply;
static const char media_presence_path[] = "/v1/call/group/device/presence";
static const char media_presence_url[] = "http://fixture/call/v1/call/group/device/presence";
static const char media_presence_body[] =
    "{\"room_id\":\"fixture\",\"assignment_version\":1,\"session_id\":\"fixture-media\",\"state\":\"joined\"}";
static xiaotai_signal_t http_entered, http_release, start_entered, start_release;
static int login_http = 200;
static xiaotai_err_t transport_error;
static atomic_uint discoveries, logins, barriers, starts, destroys, last_epoch, clocks, online, offline, paused, needed;
static atomic_uint offline_reason, request_done, last_binding;
static atomic_bool client, no_provider;
static atomic_bool observe_gate_idle;
static atomic_uint gate_idle;
static atomic_int_fast64_t warp_us;
static atomic_int_fast64_t refresh_clock_us;
static atomic_uint random_parts;
static bool wire_fixture, wire_split, wire_manual, wire_receive;
static bool wire_split_reconnect;
static bool wire_split_request_failure, wire_split_first_request_fails;
static bool wire_batch_first_request_fails;
static atomic_int wire_batch_bad_grant_id;
static enum { WIRE_ACK_NORMAL, WIRE_ACK_REFUSED, WIRE_ACK_EXCESS_QOS, WIRE_ACK_EXCESS_COUNT,
              WIRE_ACK_CORRELATED_ERROR, WIRE_ACK_STALE_ERROR, WIRE_ACK_UNCORRELATED_ERROR } wire_ack_case;
static bool wire_at, wire_no_reap, wire_timeout;
static bool bounded_exchange_fixture;
static bool wire_binding, wire_binding_s3, binding_http_unbound, binding_verify_unbound, binding_early_ack;
static bool binding_during_start;
static bool profile_fixture, profile_fail_writer, profile_hold_writer;
static bool profile_fill_slot;
static bool profile_reply_pressure;
static bool profile_large;
static bool profile_fast_hold;
static enum { PROFILE_NO_HOLD, PROFILE_HOLD_PREFLIGHT, PROFILE_HOLD_PREWIRE } profile_hold_at;
static bool profile_preflight_gated;
static atomic_uint profile_hold_ticket, profile_hold_generation;
static atomic_bool profile_worker_paused;
static atomic_uint profile_requests, profile_events, profile_ready_events, profile_writes;
static atomic_int profile_http, profile_code;
static _Atomic(const char *) profile_raw_reply;
static atomic_int_fast64_t profile_budget_clock_us;
static atomic_bool profile_budget_checked;
static char profile_body[2048];
static xiaotai_signal_t binding_release;
static atomic_uint binding_callbacks_finished, binding_request_reply, binding_stale_reply;
static atomic_uint binding_second_stale_reply;
static _Thread_local bool console_running;
static uint8_t wire_codes[2] = {0, 1};
static atomic_uint wire_creates, wire_destroys, wire_subscribes, hold_results;
static atomic_uint wire_received;
static atomic_uint wire_steps;
static atomic_uint wire_event_serial, wire_online_at, wire_offline_at, wire_binding_at;
static atomic_bool wire_duplicate;
static atomic_bool wire_fail_stop, wire_fail_start;
static atomic_bool wire_fail_destroy, wire_block_stop;
static atomic_uint wire_destroy_attempts, wire_heartbeat_publishes;
static xiaotai_signal_t stop_entered, stop_release;
static atomic_uint bounded_stop_failures, bounded_stop_attempts;
static atomic_int hold_error;
static _Atomic(xiaotai_mqtt_t *) wire_client;
static xiaotai_mqtt_handler_t retained_handler;
static void *retained_context;
static int32_t wire_ids[128];
static unsigned wire_samples;
static const char *at_input;
static char at_output[8192];
static size_t at_used;

static void hold_done(xiaotai_err_t result, void *ctx);

static void take_profile_hold(void)
{
    if (atomic_load(&profile_hold_ticket)) return;
    uint32_t ticket;
    atomic_store(&profile_hold_generation, xiaotai_identity_generation());
    assert(xiaotai_platform_signaling_hold(hold_done, &hold_results, &ticket) == XIAOTAI_OK && ticket);
    atomic_store(&profile_hold_ticket, ticket);
}

xiaotai_err_t __real_xiaotai_http_create(const xiaotai_http_options_t *options, xiaotai_http_t **client);
xiaotai_err_t __wrap_xiaotai_http_create(const xiaotai_http_options_t *options, xiaotai_http_t **client);
xiaotai_err_t __wrap_xiaotai_http_create(const xiaotai_http_options_t *options, xiaotai_http_t **client)
{
    if (profile_hold_at == PROFILE_HOLD_PREWIRE && atomic_load(&online)) take_profile_hold();
    return __real_xiaotai_http_create(options, client);
}

xiaotai_err_t __real_xiaotai_http_perform(xiaotai_http_t *client, const xiaotai_http_request_t *req,
                                        xiaotai_http_response_t *response);
xiaotai_err_t __wrap_xiaotai_http_perform(xiaotai_http_t *client, const xiaotai_http_request_t *req,
                                        xiaotai_http_response_t *response);
xiaotai_err_t __wrap_xiaotai_http_perform(xiaotai_http_t *client, const xiaotai_http_request_t *req,
                                        xiaotai_http_response_t *response)
{
    if (!strcmp(req->url, "http://fixture/device/v1/device/token")) assert(req->timeout_ms == 15000U);
    if (active_media_fixture && !strcmp(req->url, media_presence_url))
        assert(req->timeout_ms > 0U && req->timeout_ms <= 8000U && !xiaotai_on_owner());
    if (!strcmp(req->url, "http://fixture/device/v1/device/profile") && atomic_exchange(&profile_budget_clock_us, 0)) {
        assert(req->timeout_ms == 10000U);
        atomic_store(&profile_budget_checked, true);
    }
    return __real_xiaotai_http_perform(client, req, response);
}

xiaotai_err_t __real_xiaotai_mqtt_publish(xiaotai_mqtt_t *c, const char *topic, const void *data,
    size_t size, uint8_t qos, xiaotai_mqtt_send_t mode, int32_t *id);
xiaotai_err_t __wrap_xiaotai_mqtt_publish(xiaotai_mqtt_t *c, const char *topic, const void *data,
    size_t size, uint8_t qos, xiaotai_mqtt_send_t mode, int32_t *id);
xiaotai_err_t __wrap_xiaotai_mqtt_publish(xiaotai_mqtt_t *c, const char *topic, const void *data,
    size_t size, uint8_t qos, xiaotai_mqtt_send_t mode, int32_t *id)
{
    if (topic && !strcmp(topic, "device/sn_fixture/up")) atomic_fetch_add(&wire_heartbeat_publishes, 1U);
    const xiaotai_err_t err = __real_xiaotai_mqtt_publish(c, topic, data, size, qos, mode, id);
    if (binding_early_ack && err == XIAOTAI_OK && id && !strcmp(topic, "device/sn_fixture/ack"))
        retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_PUBLISHED, .message_id = *id});
    return err;
}

bool xiaotai_net_provisioning_boot(void) { return provision; }
bool __wrap_xiaotai_mem_has_external(void);
bool __wrap_xiaotai_mem_has_external(void) { return profile_large || wire_binding_s3; }
int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    const int64_t frozen = atomic_load(&profile_budget_clock_us);
    const int64_t refresh_clock = atomic_load(&refresh_clock_us);
    return frozen ? frozen : refresh_clock ? refresh_clock : __real_xiaotai_time_us() + atomic_load(&warp_us);
}
uint32_t __wrap_xiaotai_random(void);
uint32_t __wrap_xiaotai_random(void) { return atomic_fetch_add(&random_parts, 1U) & 1U ? 0x12345678U : 0xaabbccddU; }
void __real_xiaotai_delay_ms(uint32_t ms);
void __wrap_xiaotai_delay_ms(uint32_t ms);
void __wrap_xiaotai_delay_ms(uint32_t ms)
{
    if (console_running && wire_timeout) {
        atomic_fetch_add(&warp_us, 1000000);
        __real_xiaotai_delay_ms(1);
    } else __real_xiaotai_delay_ms(ms);
}

bool __real_platform_signaling_heap_reaped(void);
bool __wrap_platform_signaling_heap_reaped(void);
bool __wrap_platform_signaling_heap_reaped(void) { return !wire_no_reap && __real_platform_signaling_heap_reaped(); }

void __real_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats);
void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats);
void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    if (wire_at && console_running && kind == XIAOTAI_MEM_INTERNAL) {
        *stats = (xiaotai_mem_stats_t){.free = 8192, .largest_block = 4096};
        ++wire_samples;
    } else __real_xiaotai_mem_stats(kind, stats);
}

xiaotai_err_t __wrap_platform_signaling_default_link(void);
xiaotai_err_t __real_platform_signaling_default_link(void);
xiaotai_err_t __wrap_platform_signaling_default_link(void)
{
    return wire_fixture ? __real_platform_signaling_default_link() : XIAOTAI_OK;
}

static void fixture_mqtt_event(void *ctx, const xiaotai_mqtt_event_t *event)
{
    int id = event->message_id;
    if (event->kind == XIAOTAI_MQTT_SUBSCRIBED && id > 0 &&
        atomic_compare_exchange_strong(&wire_batch_bad_grant_id, &id, 0)) {
        const uint8_t bad[] = {1, 2};
        xiaotai_mqtt_event_t ack = *event;
        assert(!wire_split && event->granted_count == sizeof(bad));
        ack.granted = bad;
        retained_handler(ctx, &ack);
        retained_handler(ctx, event); /* A correction cannot revive the rejected attempt. */
        return;
    }
    if (wire_ack_case != WIRE_ACK_NORMAL && event->kind == XIAOTAI_MQTT_SUBSCRIBED &&
        event->message_id == wire_ids[1]) {
        if (wire_ack_case >= WIRE_ACK_CORRELATED_ERROR) {
            const xiaotai_mqtt_event_t error = {
                .kind = XIAOTAI_MQTT_ERROR, .error = XIAOTAI_MQTT_ERROR_PROTOCOL,
                .message_id = wire_ack_case == WIRE_ACK_CORRELATED_ERROR ? event->message_id :
                              (wire_ack_case == WIRE_ACK_STALE_ERROR ? wire_ids[0] : 0),
            };
            retained_handler(ctx, &error);
            /* Stock consumes the matching request even when the ACK is malformed. */
            if (wire_ack_case != WIRE_ACK_CORRELATED_ERROR) retained_handler(ctx, event);
            return;
        }
        const uint8_t codes[2] = {wire_ack_case == WIRE_ACK_REFUSED ? XIAOTAI_MQTT_REFUSED :
            (wire_ack_case == WIRE_ACK_EXCESS_QOS ? 2U : 1U), 1};
        xiaotai_mqtt_event_t declined = *event;
        declined.granted = codes;
        declined.granted_count = wire_ack_case == WIRE_ACK_EXCESS_COUNT ? 2U : 1U;
        retained_handler(ctx, &declined);
        retained_handler(ctx, event); /* A corrected ACK for this attempt must still be ignored. */
        return;
    }
    retained_handler(ctx, event);
    if (wire_split && event->kind == XIAOTAI_MQTT_SUBSCRIBED && !atomic_exchange(&wire_duplicate, true))
        retained_handler(ctx, event); /* The port normally consumes the pending SUBACK on its first delivery. */
    if (wire_split_reconnect && event->kind == XIAOTAI_MQTT_CONNECTED) {
        const unsigned current = atomic_load(&wire_subscribes);
        if (current > 2U) {
            const uint8_t qos = 1;
            for (size_t i = current - 4U; i < current - 2U; ++i) {
                const xiaotai_mqtt_event_t stale = {.kind = XIAOTAI_MQTT_SUBSCRIBED,
                    .message_id = wire_ids[i], .granted = &qos, .granted_count = 1};
                retained_handler(ctx, &stale);
            }
        }
    }
}

xiaotai_err_t __wrap_xiaotai_mqtt_create(const xiaotai_mqtt_config_t *cfg, xiaotai_mqtt_t **out);
xiaotai_err_t __real_xiaotai_mqtt_create(const xiaotai_mqtt_config_t *cfg, xiaotai_mqtt_t **out);
xiaotai_err_t __wrap_xiaotai_mqtt_create(const xiaotai_mqtt_config_t *cfg, xiaotai_mqtt_t **out)
{
    assert(wire_fixture && !xiaotai_on_owner());
    assert(cfg->task_stack_bytes == platform.config.signaling_stack_bytes);
    assert(cfg->event_stack_bytes == XIAOTAI_PLATFORM_SIGNALING_EVENT_STACK_BYTES);
    const char *password = !refresh_fixture ? "fixture-token" : atomic_load(&logins) == 1 ?
        "a.eyJleHAiOjE3MDAwMDAxMzB9.b" : "a.eyJleHAiOjE3MDAwMDA1MDB9.b";
    assert(!strcmp(cfg->uri, expected_broker) && !strcmp(cfg->client_id, "sn_fixture") &&
           !strcmp(cfg->username, "fixture") && !strcmp(cfg->password, password));
    xiaotai_mqtt_config_t fixture = *cfg;
    if (!atomic_load(&wire_creates)) { retained_handler = cfg->handler; retained_context = cfg->ctx; }
    fixture.handler = fixture_mqtt_event;
    const xiaotai_err_t err = __real_xiaotai_mqtt_create(&fixture, out);
    if (err == XIAOTAI_OK) {
        const bool reply = !wire_manual && (!wire_timeout || !atomic_load(&wire_creates));
        assert(xiaotai_mqtt_host_suback(*out, reply, wire_codes, wire_split ? 1U : 2U) == XIAOTAI_OK);
        atomic_store(&wire_client, *out);
        ++wire_creates;
    }
    return err;
}

xiaotai_err_t __wrap_xiaotai_mqtt_subscribe(xiaotai_mqtt_t *client, const xiaotai_mqtt_topic_t *topics,
                                           size_t count, int32_t *id);
xiaotai_err_t __real_xiaotai_mqtt_subscribe(xiaotai_mqtt_t *client, const xiaotai_mqtt_topic_t *topics,
                                           size_t count, int32_t *id);
xiaotai_err_t __wrap_xiaotai_mqtt_subscribe(xiaotai_mqtt_t *client, const xiaotai_mqtt_topic_t *topics,
                                           size_t count, int32_t *id)
{
    const unsigned slot = atomic_load(&wire_subscribes);
    assert(slot < sizeof(wire_ids) / sizeof(*wire_ids));
    assert(count == (wire_split ? 1U : 2U));
    if (wire_split) assert(!strcmp(topics[0].topic, slot % 2U ? "device/sn_fixture/notify" : "device/sn_fixture/cmd"));
    else assert(!strcmp(topics[0].topic, "device/sn_fixture/cmd") && !strcmp(topics[1].topic, "device/sn_fixture/notify"));
    for (size_t i = 0; i < count; ++i) assert(topics[i].qos == 1U);
    xiaotai_err_t err;
    if ((wire_split_request_failure && slot == 1U) ||
        ((wire_split_first_request_fails || wire_batch_first_request_fails) && slot == 0U)) {
        *id = 0;
        err = XIAOTAI_FAIL;
    } else err = __real_xiaotai_mqtt_subscribe(client, topics, count, id);
    wire_ids[slot] = *id;
    atomic_fetch_add(&wire_subscribes, 1);
    return err;
}

xiaotai_err_t __wrap_xiaotai_mqtt_destroy(xiaotai_mqtt_t *client);
xiaotai_err_t __real_xiaotai_mqtt_destroy(xiaotai_mqtt_t *client);
xiaotai_err_t __wrap_xiaotai_mqtt_destroy(xiaotai_mqtt_t *client)
{
    assert(!xiaotai_on_owner());
    atomic_fetch_add(&wire_destroy_attempts, 1U);
    if (atomic_exchange(&wire_fail_destroy, false)) return XIAOTAI_ERR_TIMEOUT;
    const xiaotai_err_t err = __real_xiaotai_mqtt_destroy(client);
    if (err == XIAOTAI_OK && client) {
        atomic_store(&wire_client, NULL);
        ++wire_destroys;
    }
    return err;
}

xiaotai_err_t __wrap_xiaotai_mqtt_start(xiaotai_mqtt_t *client);
xiaotai_err_t __real_xiaotai_mqtt_start(xiaotai_mqtt_t *client);
xiaotai_err_t __wrap_xiaotai_mqtt_start(xiaotai_mqtt_t *client)
{
    if (binding_during_start) {
        xiaotai_signal_give(&start_entered);
        assert(xiaotai_signal_take(&start_release, WAIT));
    }
    return atomic_exchange(&wire_fail_start, false) ? XIAOTAI_FAIL : __real_xiaotai_mqtt_start(client);
}

xiaotai_err_t __wrap_xiaotai_mqtt_stop(xiaotai_mqtt_t *client);
xiaotai_err_t __real_xiaotai_mqtt_stop(xiaotai_mqtt_t *client);
xiaotai_err_t __wrap_xiaotai_mqtt_stop(xiaotai_mqtt_t *client)
{
    ++bounded_stop_attempts;
    if (atomic_exchange(&wire_block_stop, false)) {
        xiaotai_signal_give(&stop_entered);
        assert(xiaotai_signal_take(&stop_release, WAIT));
    }
    if (atomic_load(&bounded_stop_failures)) {
        (void)atomic_fetch_sub(&bounded_stop_failures, 1U);
        return XIAOTAI_FAIL;
    }
    return atomic_exchange(&wire_fail_stop, false) ? XIAOTAI_FAIL : __real_xiaotai_mqtt_stop(client);
}

static xiaotai_platform_status_t status(void)
{
    xiaotai_platform_status_t s;
    xiaotai_platform_status(&s);
    return s;
}

bool __real_xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout);
bool __wrap_xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout);
bool __wrap_xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout)
{
    if ((wire_fixture || atomic_load(&refresh_clock_us)) && signal == &platform.wake) ++wire_steps;
    if (profile_fast_hold && signal == &platform.wake && !atomic_load(&profile_worker_paused)) {
        platform_lock();
        const bool posted = platform.profile.phase == PROFILE_POSTED;
        platform_unlock();
        if (posted) {
            atomic_store(&profile_worker_paused, true);
            xiaotai_signal_give(&start_entered);
            assert(__real_xiaotai_signal_take(&start_release, WAIT));
        }
    }
    if (signal == &platform.wake && timeout == XIAOTAI_WAIT_FOREVER && atomic_load(&observe_gate_idle)) {
        const xiaotai_platform_status_t s = status();
        if (s.gated && !s.signaling && atomic_exchange(&observe_gate_idle, false)) ++gate_idle;
    }
    return __real_xiaotai_signal_take(signal, timeout);
}

static void waits(const atomic_uint *count, unsigned n)
{
    const int64_t until = __real_xiaotai_time_us() + (int64_t)WAIT * 1000;
    while (atomic_load(count) < n && __real_xiaotai_time_us() < until) xiaotai_delay_ms(1);
    assert(atomic_load(count) == n);
}

static void settle_exchanges(void)
{
    const int64_t until = __real_xiaotai_time_us() + WAIT * INT64_C(1000);
    while (xiaotai_platform_exchanges_busy() && __real_xiaotai_time_us() < until) xiaotai_delay_ms(1U);
    assert(!xiaotai_platform_exchanges_busy());
}

static void phase(xiaotai_platform_phase_t expected)
{
    const int64_t until = __real_xiaotai_time_us() + (int64_t)WAIT * 1000;
    while (status().phase != expected && __real_xiaotai_time_us() < until) xiaotai_delay_ms(1);
    assert(status().phase == expected);
}

static const char *header(const xiaotai_http_host_request_t *r, const char *key)
{
    for (size_t i = 0; i < r->header_count; ++i)
        if (!strcmp(r->headers[i].name, key)) return r->headers[i].value;
    return NULL;
}

static void response(void *ctx, const xiaotai_http_host_request_t *r, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && !strncmp(r->url, "http://", 7));
    const char *body;
    if (!strcmp(r->url, "http://fixture/services")) {
        assert(r->method == XIAOTAI_HTTP_GET && !r->header_count && !r->has_content_length && status().clock_synced);
        ++discoveries;
        if (block_discovery && atomic_load(&discoveries) == 1U) {
            xiaotai_signal_give(&http_entered);
            assert(xiaotai_signal_take(&http_release, WAIT));
        }
        body = services_json;
    } else if (!strcmp(r->url, "http://fixture/device/v1/device/token")) {
        assert(r->method == XIAOTAI_HTTP_POST && r->has_content_length && !r->body_size && r->header_count == 6);
        assert(!strcmp(header(r, "Content-Type"), "application/json"));
        assert(status().phase == XIAOTAI_PLATFORM_LOGIN && !status().authorized);
        assert(!atomic_load(&client) && atomic_load(&barriers) == atomic_load(&logins) + 1U);
        assert(!strcmp(header(r, "X-Mac"), expected_mac));
        assert(!strcmp(header(r, "X-Device-Id"), expected_device));
        assert(strlen(header(r, "X-Timestamp")) == 10 && strlen(header(r, "X-Nonce")) == 16 &&
            strlen(header(r, "X-Signature")) == 44 && !header(r, "Authorization"));
        if (signature_fixture) {
            assert(!strcmp(header(r, "X-Timestamp"), "1700000100") && !strcmp(header(r, "X-Nonce"), "aabbccdd12345678"));
            assert(!strcmp(header(r, "X-Signature"), "CWBQop3ewwmS2xbk/6XnbaQONyuV8RP3d+C6eBADPo0="));
        }
        ++logins;
        reply->status = login_http;
        const unsigned n = atomic_load(&logins);
        if (short_fixture) body = n == 1 ? short_login : n == 2 ? short_second_login : refreshed_login;
        else body = refresh_fixture ? n == 1 ? refreshing_login : refreshed_login : login_json;
        if (binding_verify_unbound && n == 2U) { reply->status = 410; body = "{\"code\":6006}"; }
        if (transport_error) {
            reply->fail_stage = XIAOTAI_HTTP_STAGE_RESOLVE;
            reply->error = transport_error;
        }
    } else if (!strcmp(r->url, "http://fixture/device/v1/device/profile")) {
        assert(r->method == XIAOTAI_HTTP_POST && !strncmp(header(r, "Authorization"), "Bearer ", 7));
        if (profile_hold_at != PROFILE_NO_HOLD) assert(!status().held);
        const int status = profile_fixture ? atomic_load(&profile_http) : 200;
        const int code = profile_fixture ? atomic_load(&profile_code) : 200;
        if (profile_fixture) {
            assert(!strcmp(header(r, "Authorization"), "Bearer fixture-token"));
            assert(r->body_size < sizeof(profile_body) && r->body_size > 0 && atomic_load(&online));
            memcpy(profile_body, r->body, r->body_size);
            profile_body[r->body_size] = '\0';
            ++profile_requests;
            if (profile_fill_slot || profile_reply_pressure) {
                xiaotai_signal_give(&start_entered);
                assert(xiaotai_signal_take(&start_release, WAIT));
            }
        }
        reply->status = status;
        if (!status) { reply->fail_stage = XIAOTAI_HTTP_STAGE_RESOLVE; reply->error = XIAOTAI_FAIL; }
        static char answer[48];
        const char *raw = atomic_load(&profile_raw_reply);
        if (raw) body = raw;
        else if (code) {
            assert(snprintf(answer, sizeof(answer), "{\"code\":%d}", code) > 0);
            body = answer;
        } else body = "{}";
    } else if (active_media_fixture && !strcmp(r->url, media_presence_url)) {
        const char *auth = header(r, "Authorization");
        assert(xiaotai_activity_active() && auth && !strncmp(auth, "Bearer ", 7));
        assert(r->method == XIAOTAI_HTTP_POST && r->body_size == sizeof(media_presence_body) - 1U &&
               !memcmp(r->body, media_presence_body, r->body_size));
        xiaotai_signal_give(&http_entered);
        assert(xiaotai_signal_take(&http_release, WAIT));
        reply->status = 200;
        body = "{\"code\":200}";
    } else if ((profile_fill_slot || bounded_exchange_fixture) &&
               !strcmp(r->url, "http://fixture/device/occupy-profile-slot")) {
        xiaotai_signal_give(&http_entered);
        assert(xiaotai_signal_take(&http_release, WAIT));
        reply->status = 200;
        body = "{\"code\":200}";
    } else if (binding_http_unbound && !strcmp(r->url, "http://fixture/device/binding-loss")) {
        assert(!strcmp(header(r, "Authorization"), "Bearer fixture-token"));
        xiaotai_signal_give(&http_entered);
        assert(xiaotai_signal_take(&http_release, WAIT));
        reply->status = 410;
        body = "{\"code\":6006}";
    } else {
        assert(!strcmp(r->url, "http://fixture/device/unauth"));
        assert(!strcmp(header(r, "Authorization"), "Bearer fixture-token"));
        reply->status = 401;
        body = "{\"code\":401}\n";
    }
    reply->body = (const uint8_t *)body;
    reply->body_size = strlen(body);
    reply->content_length = (int64_t)reply->body_size;
}

static xiaotai_err_t start_link(void *ctx, uint32_t epoch, const char *broker, const char *device, const char *token)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && !atomic_exchange(&client, true));
    assert(epoch && epoch != atomic_load(&last_epoch));
    assert(!strcmp(broker, expected_broker) && !strcmp(device, expected_device) && *token);
    atomic_store(&last_epoch, epoch);
    ++starts;
    if (block_start) {
        xiaotai_signal_give(&start_entered);
        assert(xiaotai_signal_take(&start_release, WAIT));
    }
    platform_session_link_event(epoch, PLATFORM_LINK_READY);
    return start_fails ? XIAOTAI_FAIL : XIAOTAI_OK;
}

static xiaotai_err_t destroy_link(void *ctx)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && atomic_exchange(&client, false));
    ++destroys;
    return XIAOTAI_OK;
}

static xiaotai_err_t guest(void *ctx)
{
    (void)ctx;
    assert(!xiaotai_on_owner());
    ++barriers;
    return XIAOTAI_OK;
}

static void occupy_slot_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && reply->result == XIAOTAI_OK && reply->http_status == 200);
    ++request_done;
}

static void observe(const xiaotai_event_t *e, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    if (!strcmp(e->name, XIAOTAI_EVENT_PLATFORM_CLOCK)) ++clocks;
    else if (!strcmp(e->name, XIAOTAI_EVENT_PLATFORM_ONLINE)) {
        atomic_store(&wire_online_at, 1U + atomic_fetch_add(&wire_event_serial, 1U));
        ++online;
        if (profile_fill_slot) {
            const xiaotai_platform_request_t request = {.service = "device", .path = "/occupy-profile-slot",
                .auth = XIAOTAI_PLATFORM_AUTH_SESSION};
            assert(xiaotai_platform_request(&request, occupy_slot_reply, NULL) == XIAOTAI_OK);
        }
        if (profile_fast_hold) {
            xiaotai_signal_give(&http_entered);
            assert(xiaotai_signal_take(&http_release, WAIT));
        }
    }
    else if (!strcmp(e->name, XIAOTAI_EVENT_PLATFORM_OFFLINE)) {
        atomic_store(&wire_offline_at, 1U + atomic_fetch_add(&wire_event_serial, 1U));
        atomic_store(&offline_reason, (unsigned)e->args.value);
        ++offline;
        if (wire_binding && e->args.value == XIAOTAI_PLATFORM_OFFLINE_BINDING) {
            assert(atomic_load(&wire_client) && !status().authorized);
            platform_lock();
            assert(!strcmp(platform.token, "fixture-token"));
            platform_unlock();
            assert(xiaotai_signal_take(&binding_release, WAIT));
            ++binding_callbacks_finished;
        }
    } else if (!strcmp(e->name, XIAOTAI_EVENT_PLATFORM_PAUSED)) ++paused;
    else if (!strcmp(e->name, XIAOTAI_EVENT_PLATFORM_PROFILE)) {
        if (e->args.value == 1) ++profile_ready_events;
        ++profile_events;
    }
    else if (!strcmp(e->name, XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED)) {
        atomic_store(&wire_binding_at, 1U + atomic_fetch_add(&wire_event_serial, 1U));
        atomic_store(&last_binding, (unsigned)e->args.value);
        ++needed;
    }
}

static const char signal_json[] = "{\"type\":\"fixture\",\"channel\":\"lab\",\"value\":42}";

static void receive_signal(const xiaotai_platform_signal_t *s, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == &wire_received);
    assert(s->command && s->generation == xiaotai_identity_generation());
    assert(!strcmp(s->type, "fixture") && !strcmp(s->channel, "lab") &&
           s->size == sizeof(signal_json) - 1U && !strcmp(s->json, signal_json));
    ++wire_received;
}

static int write_part(char *out, size_t capacity, void *ctx)
{
    const char *part = ctx;
    const size_t length = strlen(part);
    if (!strcmp(part, "!")) {
        if (capacity) *out = 'X';
        return -1;
    }
    if (!strcmp(part, "?")) return (int)capacity + 1;
    if (length > capacity) return -1;
    memcpy(out, part, length);
    return (int)length;
}

static int write_scene(char *out, size_t capacity, void *ctx)
{
    assert(xiaotai_on_owner());
    if (!ctx) return 0;
    assert(profile_fixture && atomic_load(&online) && status().exchanges == 1);
    ++profile_writes;
    if (profile_fail_writer) return -1;
    if (profile_hold_writer) {
        xiaotai_signal_give(&http_entered);
        assert(xiaotai_signal_take(&http_release, WAIT));
    }
    const char initial[] = "{\"own\":1";
    size_t size = sizeof(initial) - 1U;
    assert(capacity > size + 1U);
    memcpy(out, initial, size);
    assert(xiaotai_platform_scene_parts("call", ctx, out, capacity - 1U, &size) == 2);
    out[size++] = '}';
    return (int)size;
}

static int write_dense_scene(char *out, size_t capacity, void *ctx)
{
    const size_t size = (size_t)(uintptr_t)ctx;
    static const char prefix[] = "{\"pad\":\"", suffix[] = "\"}";
    assert(xiaotai_on_owner() && status().exchanges == 1 && size > 10U);
    ++profile_writes;
    if (capacity < size) return -1;
    memcpy(out, prefix, sizeof(prefix) - 1U);
    memset(out + sizeof(prefix) - 1U, 'x', size - 10U);
    memcpy(out + size - 2U, suffix, 2U);
    return (int)size;
}

static void attach_profile(void)
{
    static const xiaotai_platform_scene_part_t items[] = {
        {write_part, "\"first\":2"}, {write_part, ""}, {write_part, "\"second\":3"},
        {write_part, "!"}, {write_part, "\"extra\":4"}, {write_part, "?"},
    };
    static const xiaotai_contribution_t first[] = {
        {XIAOTAI_PLATFORM_SLOT_SCENE, NULL, &items[0]}, {XIAOTAI_PLATFORM_SLOT_SCENE, "call", &items[1]},
        {XIAOTAI_PLATFORM_SLOT_SCENE, "overflow", &items[5]}, {0},
    };
    static const xiaotai_contribution_t second[] = {
        {XIAOTAI_PLATFORM_SLOT_SCENE, "call", &items[2]}, {XIAOTAI_PLATFORM_SLOT_SCENE, "call", &items[3]},
        {XIAOTAI_PLATFORM_SLOT_SCENE, "call", &items[4]}, {0},
    };
    static const xiaotai_attachment_t a = {.contributions = first}, b = {.contributions = second};
    assert(xiaotai_activity_attach("call", &a) == XIAOTAI_OK && xiaotai_activity_attach("call", &b) == XIAOTAI_OK);
}

static xiaotai_err_t profile_preflight(void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    if (profile_preflight_gated) return XIAOTAI_ERR_PLATFORM_GATED;
    take_profile_hold();
    return XIAOTAI_OK;
}

static xiaotai_err_t registrations(void)
{
    static const char *const names[] = {XIAOTAI_EVENT_PLATFORM_CLOCK, XIAOTAI_EVENT_PLATFORM_DISCOVERED,
        XIAOTAI_EVENT_PLATFORM_ONLINE, XIAOTAI_EVENT_PLATFORM_OFFLINE, XIAOTAI_EVENT_PLATFORM_PAUSED,
        XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, XIAOTAI_EVENT_PLATFORM_PROFILE};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
        assert(xiaotai_event_subscribe(names[i], observe, NULL) == XIAOTAI_OK);
    if (wire_receive)
        assert(xiaotai_platform_signal_subscribe("fixture", "lab", receive_signal, &wire_received) == XIAOTAI_OK);
    if (wire_binding)
        assert(xiaotai_platform_require(&(const xiaotai_platform_requirement_t){.concurrent_requests = 3}) == XIAOTAI_OK);
    if (profile_fixture) {
        if (profile_hold_at == PROFILE_HOLD_PREFLIGHT || profile_preflight_gated)
            assert(xiaotai_platform_exchange_preflight_register(profile_preflight, NULL) == XIAOTAI_OK);
        if (profile_large) {
            assert(xiaotai_platform_require(&(const xiaotai_platform_requirement_t){.request_body_bytes = 768,
                       .concurrent_requests = 3}) == XIAOTAI_OK);
            assert(xiaotai_platform_profile_scene("stream", write_dense_scene, (void *)(uintptr_t)350) == XIAOTAI_OK);
            assert(xiaotai_platform_profile_scene("call", write_dense_scene, (void *)(uintptr_t)254) == XIAOTAI_OK);
            assert(xiaotai_platform_profile_scene("voip", write_dense_scene, (void *)(uintptr_t)340) == XIAOTAI_OK);
        } else {
            attach_profile();
            assert(xiaotai_platform_profile_scene("call", write_scene, "call") == XIAOTAI_OK);
            assert(xiaotai_platform_profile_scene("call", write_scene, "call") == XIAOTAI_ERR_INVALID_STATE);
            assert(xiaotai_platform_profile_scene("omitted", write_scene, NULL) == XIAOTAI_OK);
        }
    } else if (provision) {
        const uint32_t allocations = xiaotai_mem_host_allocations();
        for (unsigned i = 0; i < 10; ++i)
            assert(xiaotai_platform_profile_scene("unused", write_scene, NULL) == XIAOTAI_OK);
        assert(xiaotai_mem_host_allocations() == allocations);
    }
    if (require_ai) assert(xiaotai_platform_require_service("ai") == XIAOTAI_OK);
    if (!wire_fixture && !atomic_load(&no_provider)) assert(platform_session_link_register(&(platform_session_link_t){
        .start = start_link, .destroy = destroy_link}) == XIAOTAI_OK);
    assert(platform_session_guest_register(guest, NULL) == XIAOTAI_OK);
    return XIAOTAI_OK;
}

static xiaotai_err_t init_platform(void)
{
    const uint32_t before = xiaotai_mem_host_allocations();
    const xiaotai_err_t err = xiaotai_engine_platform_init();
    if (provision) assert(xiaotai_mem_host_allocations() == before);
    return err;
}

static void start_platform(void)
{
    const uint32_t before = xiaotai_mem_host_allocations();
    xiaotai_engine_platform_start();
    if (provision) assert(xiaotai_mem_host_allocations() == before);
}

static void store_id(const char *client_id)
{
    assert(xiaotai_kv_set_str("tirtc_cfg", "device_id", "fixture", WAIT) == XIAOTAI_OK);
    assert(xiaotai_kv_set_str("tirtc_cfg", "secret", "fixture-key", WAIT) == XIAOTAI_OK);
    if (client_id) assert(xiaotai_kv_set_str("tirtc_cfg", "client_id", client_id, WAIT) == XIAOTAI_OK);
}

static void boot(const xiaotai_platform_config_t *c, bool identity, xiaotai_fingerprint_format_t format, const char *client_id)
{
    static const char *const identity_dep[] = {"identity", "at", NULL}, *const platform_dep[] = {"platform", NULL};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init};
    static const xiaotai_module_t id = {.name = "identity", .init = xiaotai_engine_identity_init};
    static const xiaotai_module_t platform_engine = {.name = "platform", .depends = identity_dep,
        .init = init_platform, .start = start_platform};
    static const xiaotai_module_t fixture = {.name = "fixture", .depends = platform_dep, .init = registrations};
    static const xiaotai_module_t *const modules[] = {&at, &id, &platform_engine, &fixture};
    const xiaotai_framework_config_t fw = {.modules = modules, .module_count = sizeof(modules) / sizeof(*modules)};
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    if (!wire_at) assert(xiaotai_at_configure(&(const xiaotai_at_config_t){.no_platform_console = true}) == XIAOTAI_OK);
    if (identity) store_id(client_id);
    assert(xiaotai_identity_configure(&(xiaotai_identity_config_t){.fingerprint = format}) == XIAOTAI_OK);
    assert(xiaotai_platform_configure(c) == XIAOTAI_OK);
    xiaotai_http_host_set_responder(response, NULL, false);
    assert(xiaotai_framework_start(&fw) == XIAOTAI_OK);
}

static void up(uint32_t generation, bool valid)
{
    xiaotai_net_status_t net = {.state = XIAOTAI_NET_UP, .generation = generation, .mac_valid = valid};
    memcpy(net.mac, mac, sizeof(net.mac));
    assert(xiaotai_event_emit(XIAOTAI_EVENT_NET_UP, NULL, &net, sizeof(net)) == XIAOTAI_OK);
}

static void network(xiaotai_platform_config_t *c)
{
    xiaotai_clock_host_fail_starts(1);
    c->retry_ms = 20;
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    up(11, false);
    xiaotai_delay_ms(30);
    assert(!atomic_load(&clocks) && !atomic_load(&discoveries) && status().phase == XIAOTAI_PLATFORM_STOPPED);
    up(11, true);
    waits(&online, 1);
    assert(status().clock_synced && status().discovered && status().authorized && status().signaling &&
        status().notify_revision == 1);
    up(11, true);
    up(11, false);
    xiaotai_delay_ms(20);
    assert(atomic_load(&starts) == 1 && atomic_load(&destroys) == 0);
    const uint32_t stale = atomic_load(&last_epoch);
    up(12, true);
    waits(&online, 2);
    waits(&offline, 1);
    assert(atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_NETWORK);
    assert(atomic_load(&destroys) == 1 && atomic_load(&starts) == 2 && atomic_load(&logins) == 2 &&
        atomic_load(&discoveries) == 1 && atomic_load(&clocks) == 1);
    platform_session_link_event(stale, PLATFORM_LINK_DROPPED);
    assert(status().signaling);
    xiaotai_platform_set_network(false, NULL);
    waits(&destroys, 2);
    phase(XIAOTAI_PLATFORM_STOPPED);
    assert(status().clock_synced && status().discovered && !status().authorized);
    xiaotai_platform_set_network(true, mac);
    waits(&online, 3);
    xiaotai_platform_set_network(true, mac);
    xiaotai_delay_ms(20);
    assert(atomic_load(&starts) == 3 && atomic_load(&logins) == 3);
}

static void reply_401(const xiaotai_platform_reply_t *reply, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && reply->result == XIAOTAI_OK && reply->http_status == 401 && reply->code == 401);
    assert(reply->size == strlen("{\"code\":401}\n") && !memcmp(reply->body, "{\"code\":401}\n", reply->size));
    ++request_done;
}

static void unauthorized(void)
{
    settle_exchanges(); /* This fixture exercises the subsequent authorized HTTP exchange. */
    const xiaotai_platform_request_t req = {.method = XIAOTAI_PLATFORM_GET, .auth = XIAOTAI_PLATFORM_AUTH_SESSION,
        .service = "device", .path = "/unauth"};
    assert(xiaotai_platform_request(&req, reply_401, NULL) == XIAOTAI_OK);
}

static void broker_gate(const xiaotai_platform_config_t *c)
{
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    xiaotai_platform_set_network(true, mac);
    waits(&online, 1);
    const uint32_t epoch = atomic_load(&last_epoch);
    platform_session_link_event(epoch, PLATFORM_LINK_DROPPED);
    waits(&offline, 1);
    assert(atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_SIGNALING);
    assert(status().phase == XIAOTAI_PLATFORM_CONNECTING);
    atomic_store(&observe_gate_idle, true);
    uint32_t gate;
    assert(xiaotai_platform_gate_close(&gate) == XIAOTAI_OK);
    waits(&gate_idle, 1);
    assert(xiaotai_platform_gate_drained(gate));
    assert(status().phase == XIAOTAI_PLATFORM_CONNECTING && status().authorized);
    assert(atomic_load(&client) && atomic_load(&destroys) == 0);

    /* The provider can finish resubscribing while HTTP is still gated. */
    platform_session_link_event(epoch, PLATFORM_LINK_READY);
    waits(&online, 2);
    const xiaotai_platform_status_t ready = status();
    assert(ready.gated && ready.signaling && ready.notify_revision == 2);
    const xiaotai_platform_request_t req = {.method = XIAOTAI_PLATFORM_GET, .auth = XIAOTAI_PLATFORM_AUTH_SESSION,
        .service = "device", .path = "/unauth"};
    assert(xiaotai_platform_request(&req, reply_401, NULL) == XIAOTAI_ERR_PLATFORM_GATED);
    assert(xiaotai_platform_gate_open(gate) == XIAOTAI_OK);
    unauthorized();
    waits(&request_done, 1);
    assert(atomic_load(&starts) == 1 && atomic_load(&logins) == 1 && atomic_load(&last_epoch) == epoch &&
        atomic_load(&destroys) == 0);
}

static void broker_start_gate(const xiaotai_platform_config_t *c)
{
    block_start = true;
    assert(xiaotai_signal_init(&start_entered) == XIAOTAI_OK && xiaotai_signal_init(&start_release) == XIAOTAI_OK);
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    xiaotai_platform_set_network(true, mac);
    assert(xiaotai_signal_take(&start_entered, WAIT));
    assert(status().phase == XIAOTAI_PLATFORM_CONNECTING && status().authorized);
    const uint32_t epoch = atomic_load(&last_epoch);
    uint32_t gate;
    assert(xiaotai_platform_gate_close(&gate) == XIAOTAI_OK);
    xiaotai_signal_give(&start_release);
    waits(&online, 1);
    assert(status().gated && status().signaling && atomic_load(&last_epoch) == epoch);
    assert(xiaotai_platform_gate_open(gate) == XIAOTAI_OK);
    assert(atomic_load(&starts) == 1 && atomic_load(&logins) == 1 && atomic_load(&destroys) == 0);
    xiaotai_signal_deinit(&start_release);
    xiaotai_signal_deinit(&start_entered);
}

static void auth_case(xiaotai_platform_config_t *c)
{
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    xiaotai_platform_set_network(true, mac);
    waits(&online, 1);
    if (auth_link) platform_session_link_event(atomic_load(&last_epoch), PLATFORM_LINK_AUTH_REFUSED);
    else {
        unauthorized();
        waits(&request_done, 1);
    }
    if (c->auth_failure == XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE) {
        platform_session_link_event(atomic_load(&last_epoch), PLATFORM_LINK_AUTH_REFUSED);
        xiaotai_delay_ms(20);
        assert(atomic_load(&logins) == 1 && !atomic_load(&offline) && status().signaling);
        return;
    }
    unsigned count = 1;
    if (c->auth_failure == XIAOTAI_PLATFORM_AUTH_FAILURE_RELOGIN) {
        waits(&online, ++count); /* First auth error has no autologin cooldown. */
        atomic_fetch_add(&warp_us, 6000000);
        platform_session_link_event(atomic_load(&last_epoch), PLATFORM_LINK_AUTH_REFUSED);
        waits(&online, ++count);
        unauthorized(); /* Immediately after the last automated login: pause. */
        waits(&request_done, 2);
    }
    waits(&destroys, count);
    waits(&paused, 1);
    waits(&offline, count);
    assert(atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_AUTH && status().phase == XIAOTAI_PLATFORM_PAUSED);
    assert(!status().authorized && !atomic_load(&client));
    atomic_fetch_add(&warp_us, 60000000);
    platform_wake();
    xiaotai_delay_ms(20);
    assert(atomic_load(&logins) == count);
    assert(xiaotai_platform_retry() == XIAOTAI_OK);
    waits(&online, count + 1U);
    assert(atomic_load(&logins) == count + 1U);
}

static void refresh(xiaotai_platform_config_t *c)
{
    c->token_needs_expiry = true;
    refresh_fixture = true;
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    xiaotai_platform_set_network(true, mac);
    waits(&online, 1);
    xiaotai_clock_host_advance_ms(22000);
    platform_wake();
    xiaotai_delay_ms(20);
    assert(atomic_load(&logins) == 1);
    uint32_t gate;
    assert(xiaotai_platform_gate_close(&gate) == XIAOTAI_OK);
    xiaotai_clock_host_advance_ms(1500);
    platform_wake();
    xiaotai_delay_ms(20);
    assert(atomic_load(&logins) == 1 && status().signaling);
    assert(xiaotai_platform_gate_open(gate) == XIAOTAI_OK);
    if (c->token_refresh) {
        waits(&online, 2);
        waits(&offline, 1);
        assert(atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_REFRESH);
    } else {
        xiaotai_clock_host_advance_ms(120000);
        platform_wake();
        xiaotai_delay_ms(20);
        assert(atomic_load(&online) == 1 && atomic_load(&logins) == 1);
    }
}

static void expiry(void)
{
    int64_t exp = 0;
    assert(xiaotai_platform_token_expiry("a.eyJleHAiOjE3MDAwMDAxMzB9.b", &exp) == XIAOTAI_OK && exp == 1700000130LL);
    const char *const bad[] = {"opaque", "a.e30.b", "a.eyJleHAiOiIxIn0.b", "a.eyJleHAiOjEuMX0.b",
        "a.eyJleHAiOi0xfQ.b", "a.eyJleHAiOjEsImV4cCI6Mn0.b", "a.eyJleHAiOjF9AA.b", "a.eyJleHAiOjF9.b.c"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        assert(xiaotai_platform_token_expiry(bad[i], &exp) == XIAOTAI_ERR_NOT_FOUND);
    char over[1025];
    memset(over, 'a', sizeof(over)); over[1024] = '\0';
    assert(xiaotai_platform_token_expiry(over, &exp) == XIAOTAI_ERR_INVALID_ARG);
    assert(platform_token_refresh_at(1030, 1000) == 1023);
    assert(platform_token_refresh_at(1000, 1000) == 0 && platform_token_refresh_at(1500, 1000) == 1440);
    assert(platform_token_refresh_at(1700 + 604800, 1700) == 1700 + 604740);
    assert(platform_token_refresh_at(1701, 1700) == 1701);
}

static void c3_token_admission(xiaotai_platform_config_t *c, const char *which)
{
    /* The legacy wall clock is 1700; shift its numeric claims by the same
     * offset as the host clock to preserve all expiry and refresh edges. */
    static const struct { const char *scenario, *token; } cases[] = {
        {"valid", "e30.eyJleHAiOjE3MDAwMDAyMTB9.Zml4dHVyZQ"},
        {"expired", "e30.eyJleHAiOjE3MDAwMDAwOTl9.Zml4dHVyZQ"},
        {"same_second", "e30.eyJleHAiOjE3MDAwMDAxMDB9.Zml4dHVyZQ"},
        {"missing", "e30.eyJzdWIiOiJmaXh0dXJlIn0.Zml4dHVyZQ"},
        {"text_expiry", "e30.eyJleHAiOiIxNzAwMDAwMjEwIn0.Zml4dHVyZQ"},
        {"fractional", "e30.eyJleHAiOjE3MDAwMDAyMTAuNX0.Zml4dHVyZQ"},
        {"overflow", "e30.eyJleHAiOjFlMzB9.Zml4dHVyZQ"},
        {"trailing", "e30.eyJleHAiOjE3MDAwMDAyMTB9IHRyYWlsaW5n.Zml4dHVyZQ"},
        {"embedded_nul", "e30.eyJleHAiOjE3MDAwMDAyMTB9AHRyYWlsaW5n.Zml4dHVyZQ"},
        {"bad_jwt", "bad.jwt"},
    };
    const bool wrong_status = !strcmp(which, "success_http201");
    const bool fractional_code = !strcmp(which, "success_code_fractional");
    size_t at = 0;
    if (!wrong_status && !fractional_code) {
        while (at < sizeof(cases) / sizeof(*cases) && strcmp(which, cases[at].scenario)) ++at;
        assert(at < sizeof(cases) / sizeof(*cases));
    }

    static char body[240];
    int size = snprintf(body, sizeof(body), "{\"code\":%s,\"data\":{\"mqtt_token\":\"%s\"}}",
        fractional_code ? "200.5" : "200", cases[at].token);
    assert(size > 0 && (size_t)size < sizeof(body));
    login_json = body;
    login_http = wrong_status ? 201 : 200;
    c->token_needs_expiry = c->token_refresh = true;
    xiaotai_clock_host_set_wall_ms(WALL_SECONDS * 1000);
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    xiaotai_platform_set_network(true, mac);

    const bool admitted = !at && !wrong_status && !fractional_code;
    if (admitted) {
        waits(&online, 1);
        assert(status().authorized && status().signaling && atomic_load(&starts) == 1);
    } else {
        phase(XIAOTAI_PLATFORM_PAUSED);
        assert(status().last_error == XIAOTAI_FAIL && !status().authorized && !status().signaling);
        assert(!atomic_load(&starts) && !atomic_load(&online));
    }
    platform_lock();
    assert(platform.session.expiry == (admitted ? WALL_SECONDS + 110 : 0));
    assert(platform.session.refresh_at == (admitted ? WALL_SECONDS + 83 : 0));
    assert(!strcmp(platform.token, admitted ? cases[at].token : ""));
    platform_unlock();
    assert(atomic_load(&logins) == 1 && !atomic_load(&needed) && xiaotai_identity_matches("fixture", "fixture-key"));
}

static void signal_pump(void);

static void media_presence_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && xiaotai_activity_current(NULL) == media_id);
    assert(reply->result == XIAOTAI_OK && reply->http_status == 200 && reply->code == 200 &&
           reply->generation == xiaotai_identity_generation());
    ++media_reply;
}

static void media_begin_and_renew(void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() &&
           xiaotai_activity_begin(&(const xiaotai_activity_t){.name = "room"}, &media_id) == XIAOTAI_OK);
    xiaotai_platform_request_t request = {.method = XIAOTAI_PLATFORM_POST, .service = "call",
        .path = media_presence_path, .body = media_presence_body, .auth = XIAOTAI_PLATFORM_AUTH_SESSION,
        .timeout_ms = 8000U, .flags = XIAOTAI_PLATFORM_BACKGROUND};
    assert(xiaotai_platform_background_deferred());
    assert(xiaotai_platform_request(&request, media_presence_reply, NULL) == XIAOTAI_ERR_BUSY);
    request.flags = 0; /* The production ROOM presence request is foreground. */
    const xiaotai_err_t admitted = xiaotai_platform_request(&request, media_presence_reply, NULL);
    if (admitted != XIAOTAI_OK) fprintf(stderr, "presence admit=%d, exchanges=%u\n", admitted, status().exchanges);
    assert(admitted == XIAOTAI_OK);
    ++media_submit;
}

static void media_owner_progress(void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && media_id && xiaotai_activity_current(NULL) == media_id);
    ++media_owner_turn;
}

static void media_end(void *ctx)
{
    media_owner_progress(ctx);
    xiaotai_activity_end(media_id);
}

static void refresh_c3_pause(xiaotai_platform_config_t *c, bool active_media)
{
    c->token_needs_expiry = c->token_refresh = true;
    c->auth_failure = XIAOTAI_PLATFORM_AUTH_FAILURE_PAUSE;
    refresh_fixture = true;
    active_media_fixture = active_media;
    if (active_media) {
        c->requests_need_signaling = c->defer_background_while_active = true;
        services_json = "{\"device-srv\":\"http://fixture/device\",\"call-srv\":\"http://fixture/call\","
            "\"mqtt-srv\":\"mqtts://fixture\"}";
        assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK);
    }
    atomic_store(&refresh_clock_us, 1000000);
    xiaotai_clock_host_set_wall_ms(WALL_SECONDS * 1000);
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    xiaotai_platform_set_network(true, mac);
    waits(&online, 1);
    if (active_media) {
        settle_exchanges();
        assert(xiaotai_run(media_begin_and_renew, NULL) == XIAOTAI_OK);
        waits(&media_submit, 1);
        assert(xiaotai_signal_take(&http_entered, WAIT) && "accepted room presence must reach HTTP before media end");
        assert(xiaotai_run(media_owner_progress, NULL) == XIAOTAI_OK);
        waits(&media_owner_turn, 1);
        assert(!atomic_load(&media_reply) && xiaotai_platform_exchanges_busy() && status().signaling);
        xiaotai_signal_give(&http_release);
        waits(&media_reply, 1);
        settle_exchanges();
    }
    xiaotai_clock_host_advance_ms(22000);
    signal_pump();
    assert(atomic_load(&logins) == 1 && !atomic_load(&offline) && status().signaling);
    xiaotai_clock_host_advance_ms(1000);
    signal_pump();
    if (active_media) {
        assert(atomic_load(&logins) == 2 && "renewal is due while the current room activity is still running");
    }
    waits(&online, 2);
    waits(&destroys, 1);
    assert(atomic_load(&logins) == 2 && atomic_load(&offline) == 1 && !atomic_load(&paused));
    assert(atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_REFRESH);
    if (active_media) {
        assert(xiaotai_activity_active() && status().authorized && status().signaling);
        assert(xiaotai_run(media_end, NULL) == XIAOTAI_OK);
        waits(&media_owner_turn, 2);
        xiaotai_signal_deinit(&http_release);
        xiaotai_signal_deinit(&http_entered);
        printf("same-session foreground ROOM presence completed on the worker; owner ran while HTTP blocked; "
            "token renewed before activity ended (23s, 30s expiry)\n");
    }
}

static void refresh_cooldown(xiaotai_platform_config_t *c)
{
    c->token_needs_expiry = c->token_refresh = true;
    c->auth_failure = XIAOTAI_PLATFORM_AUTH_FAILURE_RELOGIN;
    short_fixture = true;
    atomic_store(&refresh_clock_us, 1000000);
    xiaotai_clock_host_set_wall_ms(WALL_SECONDS * 1000);
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    xiaotai_platform_set_network(true, mac);
    waits(&online, 1);
    xiaotai_clock_host_advance_ms(2000);
    signal_pump();
    waits(&online, 2);
    atomic_store(&refresh_clock_us, 3000000);
    xiaotai_clock_host_advance_ms(2000);
    signal_pump();
    waits(&offline, 2);
    waits(&destroys, 2);
    assert(atomic_load(&logins) == 2 && !status().authorized);
    atomic_store(&refresh_clock_us, 5999000);
    signal_pump();
    assert(atomic_load(&logins) == 2 && !status().authorized);
    atomic_store(&refresh_clock_us, 6000000);
    signal_pump();
    waits(&online, 3);
    atomic_store(&refresh_clock_us, 7000000);
    signal_pump();
    assert(atomic_load(&logins) == 3 && atomic_load(&destroys) == 2 && !atomic_load(&paused));
    assert(atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_REFRESH);
}

static void signal_pump(void)
{
    /* Complete two worker turns so a turn already entering its wait cannot
     * stand for the wake after a change to the test clock. */
    for (unsigned i = 0; i < 2U; ++i) {
        const unsigned next = atomic_load(&wire_steps) + 1U;
        platform_wake();
        const int64_t until = __real_xiaotai_time_us() + (int64_t)WAIT * 1000;
        while (atomic_load(&wire_steps) < next && __real_xiaotai_time_us() < until) xiaotai_delay_ms(1);
        assert(atomic_load(&wire_steps) >= next);
    }
}

static void suback(xiaotai_mqtt_t *c, size_t index)
{
    assert(wire_ids[index] > 0);
    assert(xiaotai_mqtt_host_suback_reply(c, (uint16_t)wire_ids[index]) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(c, WAIT) == XIAOTAI_OK);
}

static void wire_boot(xiaotai_platform_config_t *c)
{
    c->subscribe_per_topic = wire_split;
    xiaotai_clock_host_set_wall_ms(WALL_SECONDS * 1000);
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    xiaotai_platform_set_network(true, mac);
    waits(&wire_creates, 1);
    waits(&wire_subscribes, wire_split ? 2U : 1U);
}

static void wire_subscribe(xiaotai_platform_config_t *c, bool split)
{
    wire_split = split;
    wire_manual = true;
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    assert(status().phase == XIAOTAI_PLATFORM_CONNECTING && !status().signaling);
    suback(m, 0);
    if (split) {
        signal_pump();
        assert(atomic_load(&wire_duplicate) && !status().signaling && !atomic_load(&online));
        suback(m, 1);
    }
    waits(&online, 1);
    assert(status().notify_revision == 1 && status().signaling);
    assert(xiaotai_mqtt_host_disconnect(m) == XIAOTAI_OK);
    waits(&offline, 1);
    assert(atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_SIGNALING);
    xiaotai_mqtt_host_advance_ms(m, 4000);
    waits(&wire_subscribes, split ? 4U : 2U);
    suback(m, split ? 2U : 1U);
    if (split) suback(m, 3);
    waits(&online, 2);
    assert(status().notify_revision == 2 && atomic_load(&wire_creates) == 1 && !atomic_load(&wire_destroys));
}

static void wire_retry(xiaotai_platform_config_t *c)
{
    wire_manual = true;
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    assert(xiaotai_mqtt_host_flush(m, WAIT) == XIAOTAI_OK);
    atomic_fetch_add(&warp_us, 13000000);
    signal_pump();
    assert(xiaotai_mqtt_host_flush(m, WAIT) == XIAOTAI_OK);
    suback(m, 0); /* too late */
    signal_pump();
    assert(!status().signaling && atomic_load(&wire_subscribes) == 1);
    xiaotai_mqtt_host_next_id(m, (uint16_t)wire_ids[0]);
    atomic_fetch_add(&warp_us, 3200000);
    signal_pump();
    waits(&wire_subscribes, 2);
    assert(wire_ids[1] == wire_ids[0]);
    suback(m, 1); /* a reused id must not mark ready */
    signal_pump();
    assert(!status().signaling);
    atomic_fetch_add(&warp_us, 3200000);
    signal_pump();
    waits(&wire_subscribes, 3);
    suback(m, 2);
    waits(&online, 1);
    xiaotai_platform_stats_t stats;
    xiaotai_platform_stats(&stats);
    assert(stats.subscribe_retries == 2);
}

static void wire_hosted_boot(xiaotai_platform_config_t *c)
{
    /* Waveshare P4 and WT99 select the same batch, deadline and refusal policy. */
    c->subscribe_timeout_s = 30;
    c->auth_failure = XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE;
    wire_manual = wire_batch_first_request_fails = true;
    atomic_store(&refresh_clock_us, 1000000);
    wire_boot(c);
    assert(!status().signaling && !atomic_load(&online) && wire_ids[0] == 0);
}

static void wire_hosted_tick(xiaotai_mqtt_t *client, unsigned ms)
{
    atomic_fetch_add(&refresh_clock_us, (int64_t)ms * 1000);
    /* Deliver the old tick time through the real serial broker callback. The
     * stock port now coalesces wake requests instead of queueing tick events. */
    assert(xiaotai_mqtt_wake(client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_wake(client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(client, WAIT) == XIAOTAI_OK);
    signal_pump();
}

static void wire_hosted_batch_retry(xiaotai_platform_config_t *c)
{
    wire_hosted_boot(c);
    xiaotai_mqtt_t *const client = atomic_load(&wire_client);
    xiaotai_mqtt_host_next_id(client, 41);
    wire_hosted_tick(client, 2999);
    assert(atomic_load(&wire_subscribes) == 1);
    wire_hosted_tick(client, 1);
    assert(atomic_load(&wire_subscribes) == 2 && wire_ids[1] == 41 && !status().signaling);
    suback(client, 1);
    waits(&online, 1);
    puts("batch retry: one attempt at 3000 ms, lower QoS accepted, same client");

    assert(xiaotai_mqtt_host_disconnect(client) == XIAOTAI_OK);
    waits(&offline, 1);
    assert(!status().signaling && status().phase == XIAOTAI_PLATFORM_CONNECTING);
    xiaotai_mqtt_host_next_id(client, 51);
    xiaotai_mqtt_host_advance_ms(client, 4000);
    waits(&wire_subscribes, 3);
    assert(wire_ids[2] == 51);
    atomic_store(&wire_batch_bad_grant_id, 51);
    suback(client, 2);
    assert(!atomic_load(&wire_batch_bad_grant_id) && !status().signaling);
    xiaotai_mqtt_host_next_id(client, 51);
    wire_hosted_tick(client, 3000);
    assert(atomic_load(&wire_subscribes) == 4 && wire_ids[3] == 51 && !status().signaling);
    suback(client, 3);
    xiaotai_mqtt_host_next_id(client, 61);
    wire_hosted_tick(client, 3000);
    assert(atomic_load(&wire_subscribes) == 5 && wire_ids[4] == 61 && !status().signaling);
    wire_hosted_tick(client, 29999);
    wire_hosted_tick(client, 1);
    suback(client, 4);
    wire_hosted_tick(client, 60000);
    assert(atomic_load(&wire_subscribes) == 5 && !status().signaling && !atomic_load(&paused));
    puts("batch retry: bad grant rejected immediately; recycled ID and late ACK ignored; three attempts exhausted");

    assert(xiaotai_mqtt_host_disconnect(client) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(client, WAIT) == XIAOTAI_OK);
    xiaotai_mqtt_host_next_id(client, 71);
    xiaotai_mqtt_host_advance_ms(client, 4000);
    waits(&wire_subscribes, 6);
    assert(wire_ids[5] == 71);
    wire_hosted_tick(client, 30000);
    xiaotai_mqtt_host_next_id(client, 72);
    wire_hosted_tick(client, 3000);
    assert(atomic_load(&wire_subscribes) == 7 && wire_ids[6] == 72);
    const uint8_t grants[] = {1, 1};
    retained_handler(NULL, &(const xiaotai_mqtt_event_t){
        .kind = XIAOTAI_MQTT_SUBSCRIBED, .message_id = 72, .granted = grants, .granted_count = sizeof(grants),
    });
    suback(client, 5);
    signal_pump();
    assert(!status().signaling && atomic_load(&online) == 1);
    suback(client, 6);
    waits(&online, 2);
    assert(status().signaling && atomic_load(&wire_creates) == 1 && !atomic_load(&wire_destroys));
    puts("batch retry: obsolete client and timed-out attempt cannot confirm the next attempt");
}

static void wire_hosted_batch_stop(xiaotai_platform_config_t *c)
{
    wire_hosted_boot(c);
    xiaotai_mqtt_t *const client = atomic_load(&wire_client);
    atomic_store(&wire_fail_stop, true);
    xiaotai_platform_set_network(false, NULL);
    phase(XIAOTAI_PLATFORM_PAUSED);
    assert(status().last_error == XIAOTAI_FAIL && atomic_load(&wire_client) == client &&
           atomic_load(&bounded_stop_attempts) == 1 && !atomic_load(&wire_destroys));
    assert(xiaotai_platform_retry() == XIAOTAI_OK);
    waits(&wire_destroys, 1);
    assert(!atomic_load(&wire_client) && atomic_load(&bounded_stop_attempts) == 2);
    atomic_fetch_add(&refresh_clock_us, INT64_C(60000000));
    retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_WAKE});
    signal_pump();
    assert(atomic_load(&wire_subscribes) == 1 && atomic_load(&wire_creates) == 1 && !status().signaling);
    puts("batch stop: a failed stop retains its handle; a successful stop rejects delayed retry");
}

static void wire_hosted_batch_binding(xiaotai_platform_config_t *c)
{
    wire_binding = true;
    assert(xiaotai_signal_init(&binding_release) == XIAOTAI_OK);
    wire_hosted_boot(c);
    xiaotai_mqtt_t *const client = atomic_load(&wire_client);
    assert(xiaotai_mqtt_host_flush(client, WAIT) == XIAOTAI_OK);
    const uint32_t generation = status().generation;
    xiaotai_platform_report_unbound(true);
    waits(&offline, 1);
    assert(status().generation != generation && !status().signaling && !status().authorized &&
           atomic_load(&wire_client) == client && !atomic_load(&wire_destroys));
    wire_hosted_tick(client, 3000);
    const unsigned attempts = atomic_load(&wire_subscribes);
    xiaotai_signal_give(&binding_release);
    waits(&needed, 1);
    assert(atomic_load(&wire_destroys) == 1 && status().phase == XIAOTAI_PLATFORM_UNBOUND);
    xiaotai_signal_deinit(&binding_release);
    printf("batch binding fence: %u subscribe attempt(s) before old client stop\n", attempts);
    fflush(stdout);
    assert(attempts == 1);
}

static void wire_split_reconnect_cycles(xiaotai_platform_config_t *c)
{
    wire_split = wire_manual = wire_split_reconnect = true;
    wire_codes[0] = 0; /* A lower granted QoS is valid for a QoS 1 request. */
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    for (unsigned i = 0; i < 40U; ++i) {
        if (i) {
            xiaotai_mqtt_host_advance_ms(m, 4000);
            waits(&wire_subscribes, 2U * i + 2U);
        }
        assert(xiaotai_mqtt_host_flush(m, WAIT) == XIAOTAI_OK);
        signal_pump();
        assert(!status().signaling && atomic_load(&online) == i);
        suback(m, 2U * i + 1U);
        signal_pump();
        assert(!status().signaling && atomic_load(&online) == i);
        suback(m, 2U * i);
        waits(&online, i + 1U);
        assert(status().signaling && status().notify_revision == i + 1U);
        assert(xiaotai_mqtt_host_disconnect(m) == XIAOTAI_OK);
        waits(&offline, i + 1U);
        assert(!status().signaling);
    }
    assert(atomic_load(&wire_client) == m && atomic_load(&wire_creates) == 1 && !atomic_load(&wire_destroys));
}

static void wire_split_failed_subscribe(xiaotai_platform_config_t *c)
{
    wire_split = wire_manual = wire_split_request_failure = true;
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    assert(wire_ids[0] > 0 && !wire_ids[1]);
    suback(m, 0);
    signal_pump();
    assert(!status().signaling && !atomic_load(&online));
    atomic_fetch_add(&warp_us, INT64_C(3200000));
    signal_pump();
    waits(&wire_subscribes, 4);
    suback(m, 3);
    signal_pump();
    assert(!status().signaling);
    suback(m, 2);
    waits(&online, 1);
    assert(status().signaling && atomic_load(&wire_creates) == 1 && !atomic_load(&wire_destroys));
}

static void wire_split_legacy_timing(xiaotai_platform_config_t *c, bool recovery)
{
    wire_split = wire_manual = true;
    wire_split_first_request_fails = recovery;
    c->subscribe_timeout_s = 12;
    c->auth_failure = XIAOTAI_PLATFORM_AUTH_FAILURE_PAUSE;
    atomic_store(&refresh_clock_us, 1000000);
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    if (recovery) {
        assert(!wire_ids[0] && wire_ids[1] > 0);
        suback(m, 1);
        wire_hosted_tick(m, 2999);
        assert(atomic_load(&wire_subscribes) == 2 && !status().signaling);
        xiaotai_mqtt_host_next_id(m, 21);
        wire_hosted_tick(m, 1);
        assert(atomic_load(&wire_subscribes) == 4 && wire_ids[2] == 21 && wire_ids[3] == 22);
        const uint8_t grant = 0;
        const xiaotai_mqtt_event_t earlier = {.kind = XIAOTAI_MQTT_SUBSCRIBED, .message_id = wire_ids[1],
            .granted = &grant, .granted_count = 1};
        retained_handler(retained_context, &earlier);
        assert(xiaotai_mqtt_host_suback(m, false, &grant, 1) == XIAOTAI_OK);
        suback(m, 3);
        signal_pump();
        assert(!status().signaling && !atomic_load(&online));
        suback(m, 2);
        waits(&online, 1);
        wire_hosted_tick(m, 60000);
        assert(status().signaling && atomic_load(&wire_subscribes) == 4);
    } else {
        suback(m, 0);
        wire_hosted_tick(m, 11999);
        assert(atomic_load(&wire_subscribes) == 2 && !status().signaling);
        wire_hosted_tick(m, 1);
        suback(m, 1); /* Both the late grant and its packet number are retired. */
        xiaotai_mqtt_host_next_id(m, (uint16_t)wire_ids[0]);
        wire_hosted_tick(m, 2999);
        assert(atomic_load(&wire_subscribes) == 2 && !status().signaling);
        wire_hosted_tick(m, 1);
        assert(atomic_load(&wire_subscribes) == 4 && wire_ids[2] == wire_ids[0]);
        suback(m, 2);
        suback(m, 3);
        signal_pump();
        assert(!status().signaling && !atomic_load(&online));
        xiaotai_mqtt_host_next_id(m, 51);
        wire_hosted_tick(m, 3000);
        assert(atomic_load(&wire_subscribes) == 6 && wire_ids[4] == 51 && wire_ids[5] == 52);
        suback(m, 4);
        wire_hosted_tick(m, 12000);
        suback(m, 5);
        wire_hosted_tick(m, 60000);
        assert(atomic_load(&wire_subscribes) == 6 && !status().signaling && !atomic_load(&online));
    }
    assert(atomic_load(&logins) == 1 && atomic_load(&wire_creates) == 1 && !atomic_load(&wire_destroys));
}

static void wire_split_legacy_refusal(xiaotai_platform_config_t *c)
{
    wire_split = wire_manual = true;
    c->auth_failure = XIAOTAI_PLATFORM_AUTH_FAILURE_PAUSE;
    atomic_store(&refresh_clock_us, 1000000);
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    const uint8_t refusal = XIAOTAI_MQTT_REFUSED;
    retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_SUBSCRIBED,
        .message_id = 99, .granted = &refusal, .granted_count = 1});
    signal_pump();
    assert(status().phase == XIAOTAI_PLATFORM_CONNECTING && !atomic_load(&paused));
    suback(m, 0);
    /* A matched refusal can destroy m. Inject the captured callback after the
     * first ACK's flush and fence on the owner's PAUSED event, not on m. */
    assert(wire_ids[1] > 0);
    retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_SUBSCRIBED,
        .message_id = wire_ids[1], .granted = &refusal, .granted_count = 1});
    waits(&paused, 1);
    phase(XIAOTAI_PLATFORM_PAUSED);
    waits(&wire_destroys, 1);
    assert(!status().signaling && !atomic_load(&online) && atomic_load(&wire_subscribes) == 2);
}

static void wire_split_legacy_stop(xiaotai_platform_config_t *c)
{
    wire_split = wire_manual = wire_split_first_request_fails = true;
    c->subscribe_timeout_s = 12;
    atomic_store(&refresh_clock_us, 1000000);
    wire_boot(c);
    xiaotai_platform_set_network(false, NULL);
    waits(&wire_destroys, 1);
    phase(XIAOTAI_PLATFORM_STOPPED);
    atomic_fetch_add(&refresh_clock_us, INT64_C(3000000));
    retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_WAKE});
    const uint8_t qos = 1;
    retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_SUBSCRIBED,
        .message_id = wire_ids[1], .granted = &qos, .granted_count = 1});
    signal_pump();
    assert(!status().signaling && !atomic_load(&online) && !atomic_load(&wire_client) &&
           atomic_load(&wire_subscribes) == 2 && atomic_load(&wire_creates) == 1);
}

static void wire_split_declined_ack(xiaotai_platform_config_t *c)
{
    wire_split = wire_manual = true;
    c->auth_failure = XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE;
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    suback(m, 0);
    suback(m, 1);
    if (wire_ack_case >= WIRE_ACK_STALE_ERROR) {
        waits(&online, 1);
        assert(status().signaling && status().notify_revision == 1 && atomic_load(&wire_subscribes) == 2);
        return;
    }
    signal_pump();
    assert(!status().signaling && !atomic_load(&online));
    atomic_fetch_add(&warp_us, INT64_C(3200000));
    signal_pump();
    waits(&wire_subscribes, 4);
    suback(m, 2);
    suback(m, 3);
    waits(&online, 1);
    assert(status().signaling && status().notify_revision == 1 &&
           atomic_load(&wire_creates) == 1 && !atomic_load(&wire_destroys));
}

static void hold_done(xiaotai_err_t result, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == &hold_results);
    atomic_store(&hold_error, result);
    ++hold_results;
}

static void wire_hold(xiaotai_platform_config_t *c, bool relogin)
{
    c->hold_relogin_on_release = relogin;
    wire_boot(c);
    waits(&online, 1);
    uint32_t holds[4], extra;
    for (size_t i = 0; i < 4U; ++i)
        assert(xiaotai_platform_signaling_hold(hold_done, &hold_results, &holds[i]) == XIAOTAI_OK && holds[i]);
    assert(xiaotai_platform_signaling_hold(NULL, NULL, &extra) == XIAOTAI_ERR_NO_MEM);
    waits(&hold_results, 4);
    assert(atomic_load(&hold_error) == XIAOTAI_OK && atomic_load(&wire_destroys) == 1);
    assert(status().held && status().authorized && !status().signaling && !atomic_load(&offline));
    for (size_t i = 0; i < 3U; ++i) xiaotai_platform_signaling_release(holds[i]);
    signal_pump();
    assert(atomic_load(&wire_creates) == 1 && status().held);
    xiaotai_platform_signaling_release(holds[3]);
    waits(&online, 2);
    assert(atomic_load(&wire_creates) == 2 && atomic_load(&logins) == (relogin ? 2U : 1U));
    assert(!status().held && status().notify_revision == 2 && !atomic_load(&offline));
    retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_DISCONNECTED});
    signal_pump();
    assert(status().signaling && !atomic_load(&offline));
}

static void wire_hold_early(xiaotai_platform_config_t *c)
{
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    uint32_t hold;
    assert(xiaotai_platform_signaling_hold(hold_done, &hold_results, &hold) == XIAOTAI_OK && hold);
    waits(&hold_results, 1);
    assert(atomic_load(&hold_error) == XIAOTAI_OK);
    xiaotai_platform_set_network(true, mac);
    signal_pump();
    assert(status().held && !status().discovered && !atomic_load(&discoveries) &&
           !atomic_load(&logins) && !atomic_load(&wire_creates));
    xiaotai_platform_signaling_release(hold);
    waits(&online, 1);
    assert(atomic_load(&logins) == 1);
}

static void wire_hold_startup(xiaotai_platform_config_t *c)
{
    services_json = "{\"device-srv\":\"https://fixture/device/\",\"mqtt-srv\":\"mqtts://fixture\","
        "\"tirtc-srv\":\"http://fixture/rtc\"}";
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    uint32_t startup, ordinary;
    assert(xiaotai_platform_signaling_hold_startup(hold_done, &hold_results, &startup) == XIAOTAI_OK && startup);
    assert(xiaotai_platform_signaling_hold(hold_done, &hold_results, &ordinary) == XIAOTAI_OK && ordinary != startup);
    waits(&hold_results, 2);
    xiaotai_platform_set_network(true, mac);
    signal_pump();
    assert(status().held && !status().discovered && !atomic_load(&discoveries) && !atomic_load(&logins));

    xiaotai_platform_signaling_release(ordinary);
    waits(&discoveries, 1);
    char endpoint[XIAOTAI_PLATFORM_URL_MAX];
    for (unsigned i = 0; i < WAIT && !status().discovered; ++i) xiaotai_delay_ms(1);
    assert(status().discovered && status().clock_synced && status().held && !status().authorized &&
           xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_TIRTC, endpoint, sizeof(endpoint)) == XIAOTAI_OK &&
           !strcmp(endpoint, "http://fixture/rtc"));
    signal_pump();
    assert(!atomic_load(&logins) && !atomic_load(&wire_creates));
    xiaotai_platform_signaling_release(startup);
    waits(&online, 1);
    assert(!status().held && status().signaling && atomic_load(&logins) == 1 && atomic_load(&wire_creates) == 1 &&
           atomic_load(&discoveries) == 1);
}

static void wire_hold_failure(xiaotai_platform_config_t *c, bool stop_failure)
{
    wire_boot(c);
    waits(&online, 1);
    uint32_t hold;
    atomic_store(&wire_fail_stop, stop_failure);
    assert(xiaotai_platform_signaling_hold(hold_done, &hold_results, &hold) == XIAOTAI_OK && hold);
    waits(&hold_results, 1);
    if (stop_failure) {
        assert(atomic_load(&hold_error) == XIAOTAI_FAIL && !atomic_load(&wire_destroys) && atomic_load(&wire_client));
        uint32_t gate;
        assert(xiaotai_platform_gate_close(&gate) == XIAOTAI_OK && xiaotai_platform_gate_open(gate) == XIAOTAI_OK);
        signal_pump();
        assert(status().phase == XIAOTAI_PLATFORM_PAUSED && !atomic_load(&wire_destroys) &&
               atomic_load(&bounded_stop_attempts) == 1);
        xiaotai_platform_signaling_release(hold);
        assert(xiaotai_platform_retry() == XIAOTAI_OK);
    } else {
        assert(atomic_load(&hold_error) == XIAOTAI_OK && atomic_load(&wire_destroys) == 1);
        atomic_store(&wire_fail_start, true);
        xiaotai_platform_signaling_release(hold);
        waits(&wire_destroys, 2);
        assert(atomic_load(&wire_creates) == 2 && status().authorized && !status().signaling);
        atomic_fetch_add(&warp_us, 500000);
        signal_pump();
        assert(atomic_load(&wire_creates) == 2 && atomic_load(&online) == 1);
        atomic_fetch_add(&warp_us, 550000);
        signal_pump();
    }
    waits(&online, 2);
    assert(status().signaling && atomic_load(&logins) == 1 && !atomic_load(&offline));
    assert(atomic_load(&wire_creates) == (stop_failure ? 2U : 3U));
}

static void wire_heartbeat(xiaotai_platform_config_t *c, bool ready_only)
{
    c->subscribe_timeout_s = 90;
    c->heartbeat_when_ready_only = ready_only;
    wire_manual = true;
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    atomic_fetch_add(&warp_us, 31000000);
    signal_pump();
    assert(!status().signaling);
    suback(m, 0);
    waits(&online, 1);
    if (!ready_only) {
        atomic_fetch_add(&warp_us, 31000000);
        signal_pump();
    }
    assert(xiaotai_mqtt_host_flush(m, WAIT) == XIAOTAI_OK);
    xiaotai_mqtt_host_publication_t out;
    char topic[96], data[90];
    assert(xiaotai_mqtt_host_take(m, &out, topic, sizeof(topic), data, sizeof(data) - 1U) == XIAOTAI_OK);
    assert(!strcmp(topic, "device/sn_fixture/up") && !out.qos && out.size < sizeof(data));
    data[out.size] = '\0';
    assert(!strcmp(data, ready_only ? "{\"type\":\"heartbeat\",\"seq\":1,\"ts\":1700000100}" :
        "{\"type\":\"heartbeat\",\"seq\":2,\"ts\":1700000100}"));
}

static void wire_hold_mqtt_lifetime(xiaotai_platform_config_t *c)
{
    wire_split = wire_manual = true; /* Both S3 products subscribe one topic at a time. */
    assert(xiaotai_signal_init(&stop_entered) == XIAOTAI_OK && xiaotai_signal_init(&stop_release) == XIAOTAI_OK);
    wire_boot(c);
    xiaotai_mqtt_t *const original = atomic_load(&wire_client);
    suback(original, 0);
    suback(original, 1);
    waits(&online, 1);
    atomic_fetch_add(&warp_us, INT64_C(31000000));
    signal_pump();
    assert(atomic_load(&wire_heartbeat_publishes) == 1);

    atomic_store(&wire_block_stop, true);
    atomic_store(&wire_fail_stop, true);
    uint32_t hold;
    assert(xiaotai_platform_signaling_hold(hold_done, &hold_results, &hold) == XIAOTAI_OK && hold);
    assert(xiaotai_signal_take(&stop_entered, WAIT));
    assert(status().held && !status().signaling && atomic_load(&wire_client) == original);
    atomic_fetch_add(&warp_us, INT64_C(31000000)); /* A heartbeat is due while stop owns the handle. */
    platform_wake();
    assert(atomic_load(&wire_heartbeat_publishes) == 1 && !atomic_load(&wire_destroy_attempts));
    xiaotai_signal_give(&stop_release);
    waits(&hold_results, 1);
    waits(&paused, 1);
    assert(atomic_load(&hold_error) == XIAOTAI_FAIL && atomic_load(&wire_client) == original);
    assert(!atomic_load(&wire_destroy_attempts) && atomic_load(&wire_subscribes) == 2U);
    xiaotai_platform_signaling_release(hold);

    atomic_store(&wire_fail_destroy, true);
    assert(xiaotai_platform_retry() == XIAOTAI_OK);
    waits(&paused, 2);
    assert(status().last_error == XIAOTAI_ERR_TIMEOUT && !status().signaling && !status().held);
    assert(atomic_load(&wire_client) == original && atomic_load(&wire_destroy_attempts) == 1 &&
           !atomic_load(&wire_destroys) && atomic_load(&bounded_stop_attempts) == 2);
    const uint8_t qos = 1;
    retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_CONNECTED});
    retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_SUBSCRIBED,
        .message_id = wire_ids[1], .granted = &qos, .granted_count = 1});
    signal_pump();
    assert(atomic_load(&wire_subscribes) == 2 && atomic_load(&online) == 1 &&
           atomic_load(&wire_heartbeat_publishes) == 1);

    assert(xiaotai_platform_retry() == XIAOTAI_OK);
    waits(&wire_subscribes, 4);
    assert(atomic_load(&wire_destroys) == 1 && atomic_load(&wire_destroy_attempts) == 2 &&
           atomic_load(&bounded_stop_attempts) == 2 && atomic_load(&wire_creates) == 2);
    xiaotai_mqtt_t *const replacement = atomic_load(&wire_client);
    suback(replacement, 2);
    retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_SUBSCRIBED,
        .message_id = wire_ids[3], .granted = &qos, .granted_count = 1});
    signal_pump();
    assert(!status().signaling && atomic_load(&online) == 1);
    suback(replacement, 3);
    waits(&online, 2);
    assert(status().signaling && status().notify_revision == 2 && atomic_load(&logins) == 1);
    xiaotai_signal_deinit(&stop_entered);
    xiaotai_signal_deinit(&stop_release);
    puts("MQTT lifetime: no publish in stop, stop/destroy errors retained the client, fresh SUBACKs restored readiness");
}

static void owner_block(void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    xiaotai_signal_give(&http_entered);
    assert(xiaotai_signal_take(&http_release, WAIT));
}

static void owner_noop(void *ctx) { (void)ctx; }

static void wire_lifecycle(xiaotai_platform_config_t *c)
{
    wire_manual = true;
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK);
    assert(xiaotai_run(owner_block, NULL) == XIAOTAI_OK && xiaotai_signal_take(&http_entered, WAIT));
    unsigned filled = 0;
    xiaotai_err_t enqueue;
    while ((enqueue = xiaotai_run(owner_noop, NULL)) == XIAOTAI_OK) assert(++filled < 1024U);
    assert(filled && enqueue == XIAOTAI_ERR_NO_MEM);
    suback(m, 0);
    phase(XIAOTAI_PLATFORM_ONLINE);
    signal_pump();
    assert(!atomic_load(&online));
    assert(xiaotai_mqtt_host_disconnect(m) == XIAOTAI_OK);
    phase(XIAOTAI_PLATFORM_CONNECTING);
    signal_pump();
    assert(!atomic_load(&online) && !atomic_load(&offline));
    platform_lock();
    /* This injects the future binding state's event into the actual latch;
     * this test does not cover the P-5 binding lifecycle. */
    platform_event_publish(PLATFORM_EVENT_BINDING, 1);
    platform_unlock();
    xiaotai_mqtt_host_advance_ms(m, 4000);
    waits(&wire_subscribes, 2);
    suback(m, 1);
    phase(XIAOTAI_PLATFORM_ONLINE);
    assert(status().notify_revision == 2);
    xiaotai_signal_give(&http_release);
    waits(&needed, 1);
    waits(&offline, 1);
    waits(&online, 1); /* The superseded first ONLINE never escapes the latch. */
    assert(atomic_load(&wire_binding_at) < atomic_load(&wire_offline_at) &&
           atomic_load(&wire_offline_at) < atomic_load(&wire_online_at));
    xiaotai_platform_stats_t stats;
    xiaotai_platform_stats(&stats);
    assert(stats.event_post_retries > 0);
    xiaotai_signal_deinit(&http_release);
    xiaotai_signal_deinit(&http_entered);
}

static void wire_signal(xiaotai_platform_config_t *c)
{
    wire_receive = true;
    wire_boot(c);
    waits(&online, 1);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    const size_t fragments[] = {0, 7, 1, sizeof(signal_json) - 9U};
    assert(xiaotai_mqtt_host_message(m, "device/sn_fixture/cmd", signal_json, sizeof(signal_json) - 1U,
        1, 5, false, false, fragments, sizeof(fragments) / sizeof(*fragments)) == XIAOTAI_OK);
    waits(&wire_received, 1);
    assert(xiaotai_mqtt_host_flush(m, WAIT) == XIAOTAI_OK);
    xiaotai_mqtt_host_publication_t out;
    char topic[96], data[32];
    assert(xiaotai_mqtt_host_take(m, &out, topic, sizeof(topic), data, sizeof(data)) == XIAOTAI_OK);
    assert(!strcmp(topic, "device/sn_fixture/ack") && out.qos == 1 && out.message_id > 0 &&
           out.size == sizeof("{\"ack\":true}") - 1U && !memcmp(data, "{\"ack\":true}", out.size));
    signal_pump();
    xiaotai_platform_stats_t stats;
    xiaotai_platform_stats(&stats);
    assert(stats.signals == 1 && stats.signals_delivered == 1 && stats.signals_acknowledged == 1);
}

static void binding_reply(const xiaotai_platform_reply_t *r, void *ctx)
{
    assert(xiaotai_on_owner());
    if (ctx == &binding_request_reply) {
        assert(r->result == XIAOTAI_OK && r->http_status == 410 && r->code == 6006 && r->body);
        ++binding_request_reply;
    } else {
        assert((ctx == &binding_stale_reply || ctx == &binding_second_stale_reply) &&
               r->result == XIAOTAI_ERR_PLATFORM_STALE && !r->body && !r->http_status);
        /* Old queue is released only after owner quiescence, destruction and wipe. */
        assert(atomic_load(&binding_callbacks_finished) == 1 && atomic_load(&wire_destroys) == 1);
        platform_lock();
        assert(!platform.token[0]);
        platform_unlock();
        ++*(atomic_uint *)ctx;
    }
}

static void binding_inject(xiaotai_mqtt_t *client, const char *topic)
{
    static const char body[] = "{\"type\":\"unbind\"}";
    assert(xiaotai_mqtt_host_message(client, topic, body, sizeof(body) - 1U, 1, 31, false, false, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_mqtt_host_flush(client, WAIT) == XIAOTAI_OK);
}

static int32_t binding_publication(xiaotai_mqtt_t *client, const char *wanted)
{
    char topic[96], data[32];
    xiaotai_mqtt_host_publication_t out;
    assert(xiaotai_mqtt_host_take(client, &out, topic, sizeof(topic), data, sizeof(data)) == XIAOTAI_OK);
    assert(out.qos == 1 && out.message_id > 0 && !strcmp(topic, wanted));
    if (!strcmp(wanted, "device/sn_fixture/ack"))
        assert(out.size == sizeof("{\"ack\":true}") - 1U && !memcmp(data, "{\"ack\":true}", out.size));
    return out.message_id;
}

static void binding_block_owner(void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    xiaotai_signal_give(&http_entered);
    assert(xiaotai_signal_take(&http_release, WAIT));
}

static void binding_noop(void *ctx) { (void)ctx; }

static bool wire_split_legacy_unbound(xiaotai_platform_config_t *c, bool retry)
{
    wire_split = wire_manual = true;
    wire_split_first_request_fails = retry;
    c->subscribe_timeout_s = 12;
    atomic_store(&refresh_clock_us, 1000000);
    assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK);
    wire_boot(c);
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    if (!retry) suback(m, 0);
    assert(xiaotai_run(binding_block_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&http_entered, WAIT));
    const uint32_t before = xiaotai_identity_generation();
    xiaotai_platform_report_unbound(true);
    assert(before != xiaotai_identity_generation() && !atomic_load(&platform.session.receive_generation));
    assert(atomic_load(&wire_client) == m && !atomic_load(&wire_destroys));
    if (retry) wire_hosted_tick(m, 3000);
    else suback(m, 1);
    assert(!status().signaling && !atomic_load(&online));
    const unsigned submits = atomic_load(&wire_subscribes);
    fprintf(stderr, "split after unbind: recv_generation=0, submits before=2 after=%u\n", submits);
    xiaotai_signal_give(&http_release);
    waits(&wire_destroys, 1);
    waits(&needed, 1);
    xiaotai_signal_deinit(&http_entered);
    xiaotai_signal_deinit(&http_release);
    return submits == 2;
}

static void wire_hold_bounded(xiaotai_platform_config_t *c, const char *which)
{
    const bool ttl = !strcmp(which, "wire_hold_bounded_ttl");
    const bool refresh = !strcmp(which, "wire_hold_bounded_refresh");
    const bool opaque = !strcmp(which, "wire_hold_bounded_opaque");
    const bool network = !strcmp(which, "wire_hold_bounded_network");
    const bool scoped = !strcmp(which, "wire_hold_bounded_scoped");
    const bool persistent = !strcmp(which, "wire_hold_bounded_stop_persistent");
    const bool stop_failure = persistent || !strcmp(which, "wire_hold_bounded_stop_failure");
    bounded_exchange_fixture = !strcmp(which, "wire_hold_bounded_exchange");
    c->token_needs_expiry = !opaque;
    c->token_refresh = refresh;
    c->profile_requires_ready = true;
    refresh_fixture = !opaque;
    if (!ttl && !opaque && !network && !bounded_exchange_fixture) atomic_store(&refresh_clock_us, 1000000);
    if (ttl || bounded_exchange_fixture)
        assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK);
    uint32_t ticket = 0;
    assert(xiaotai_platform_signaling_hold_bounded(0, NULL, NULL, &ticket) == XIAOTAI_ERR_INVALID_ARG && !ticket);
    assert(xiaotai_platform_signaling_hold_bounded(60001, NULL, NULL, &ticket) == XIAOTAI_ERR_INVALID_ARG && !ticket);
    assert(xiaotai_platform_signaling_hold_bounded(60000, NULL, NULL, &ticket) == XIAOTAI_ERR_PLATFORM_OFFLINE && !ticket);
    wire_boot(c);
    waits(&online, 1);
    if (opaque) {
        assert(xiaotai_platform_signaling_hold_bounded(60000, NULL, NULL, &ticket) == XIAOTAI_ERR_BUSY && !ticket);
        assert(status().signaling && !status().held && !atomic_load(&wire_destroys));
        return;
    }
    settle_exchanges();
    if (ttl) {
        assert(xiaotai_run(binding_block_owner, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&http_entered, WAIT));
    }
    if (stop_failure) atomic_store(&bounded_stop_failures, persistent ? 2U : 1U);
    assert(xiaotai_platform_signaling_hold_bounded(ttl ? 250U : 60000U, hold_done, &hold_results, &ticket) ==
           XIAOTAI_OK && ticket);
    if (stop_failure) {
        waits(&hold_results, 1U);
        assert(atomic_load(&hold_error) == XIAOTAI_FAIL && status().phase == XIAOTAI_PLATFORM_PAUSED &&
               status().held && atomic_load(&wire_creates) == 1 && !atomic_load(&wire_destroys));
        xiaotai_platform_signaling_release(ticket);
        waits(&bounded_stop_attempts, 2U);
        if (persistent) {
            signal_pump();
            assert(status().phase == XIAOTAI_PLATFORM_PAUSED && status().last_error == XIAOTAI_FAIL &&
                   !status().held && !status().signaling && atomic_load(&wire_creates) == 1 &&
                   !atomic_load(&wire_destroys) && atomic_load(&bounded_stop_attempts) == 2);
        } else {
            waits(&online, 2U);
            assert(status().signaling && !status().held && atomic_load(&wire_destroys) == 1);
        }
        return;
    }
    waits(&wire_destroys, 1);
    if (ttl) {
        /* No explicit wake or owner turn: the worker's timed wait releases. */
        phase(XIAOTAI_PLATFORM_ONLINE);
        assert(!status().held && status().signaling && !atomic_load(&hold_results));
        xiaotai_signal_give(&http_release);
        waits(&online, 2);
        assert(!atomic_load(&hold_results));
    } else {
        waits(&hold_results, 1);
        assert(atomic_load(&hold_error) == XIAOTAI_OK && status().held && !status().signaling);
        if (bounded_exchange_fixture) {
            const xiaotai_platform_request_t req = {.method = XIAOTAI_PLATFORM_GET,
                .service = XIAOTAI_PLATFORM_SERVICE_DEVICE, .path = "/occupy-profile-slot",
                .auth = XIAOTAI_PLATFORM_AUTH_NONE, .timeout_ms = WAIT};
            assert(xiaotai_platform_request(&req, occupy_slot_reply, NULL) == XIAOTAI_OK);
            assert(xiaotai_signal_take(&http_entered, WAIT) && !status().held);
            uint32_t another = 0;
            assert(xiaotai_platform_signaling_hold_bounded(1000, NULL, NULL, &another) != XIAOTAI_OK && !another);
            xiaotai_signal_give(&http_release);
            waits(&request_done, 1);
            waits(&online, 2);
        } else if (scoped) {
            uint32_t other = 0;
            assert(xiaotai_platform_signaling_hold(NULL, NULL, &other) == XIAOTAI_OK && other != ticket);
            assert(xiaotai_platform_signaling_bounded_hold_active(ticket, &hold_results));
            assert(!xiaotai_platform_signaling_bounded_hold_active(ticket, &other));
            xiaotai_platform_signaling_release_bounded(ticket, &other);
            assert(xiaotai_platform_signaling_bounded_hold_active(ticket, &hold_results));
            xiaotai_platform_signaling_release_bounded(ticket, &hold_results);
            assert(!xiaotai_platform_signaling_bounded_hold_active(ticket, &hold_results));
            assert(status().held && !status().signaling && atomic_load(&wire_creates) == 1);
            xiaotai_platform_signaling_release(other);
            waits(&online, 2);
            settle_exchanges();
            /* Force the ID reuse that a reset permits, under a new MQTT epoch
             * and the same identity generation. The old context cannot drop it. */
            platform_lock();
            platform.session.hold_next = ticket - 1U;
            platform_unlock();
            uint32_t reused = 0;
            assert(xiaotai_platform_signaling_hold_bounded(60000U, NULL, &other, &reused) == XIAOTAI_OK &&
                   reused == ticket);
            xiaotai_platform_signaling_release_bounded(ticket, &hold_results);
            assert(xiaotai_platform_signaling_bounded_hold_active(reused, &other));
            xiaotai_platform_signaling_release_bounded(reused, &other);
            phase(XIAOTAI_PLATFORM_ONLINE);
        } else if (network) {
            xiaotai_platform_set_network(false, NULL);
            waits(&offline, 1);
            assert(!status().held);
            xiaotai_platform_set_network(true, mac);
            waits(&online, 2);
            assert(atomic_load(&logins) == 2);
        } else {
            /* A forward wall jump must shorten the initial monotonic cap. */
            xiaotai_clock_host_advance_ms(refresh ? 20999 : 27999);
            signal_pump();
            assert(status().held && atomic_load(&logins) == 1);
            xiaotai_clock_host_advance_ms(1);
            signal_pump();
            waits(&online, 2);
            assert(!status().held && atomic_load(&logins) == 1);
            if (refresh) {
                xiaotai_clock_host_advance_ms(2000);
                signal_pump();
                waits(&online, 3);
                assert(atomic_load(&logins) == 2 && atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_REFRESH);
            }
        }
    }
    xiaotai_platform_signaling_release(ticket); /* a late owner never revives the retired hold */
    assert(!status().held && status().signaling);
    if (ttl || bounded_exchange_fixture) {
        xiaotai_signal_deinit(&http_release);
        xiaotai_signal_deinit(&http_entered);
    }
}

static void wire_binding_start(xiaotai_platform_config_t *c)
{
    wire_binding = binding_during_start = true;
    assert(xiaotai_signal_init(&start_entered) == XIAOTAI_OK && xiaotai_signal_init(&start_release) == XIAOTAI_OK &&
           xiaotai_signal_init(&binding_release) == XIAOTAI_OK);
    boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    xiaotai_platform_set_network(true, mac);
    assert(xiaotai_signal_take(&start_entered, WAIT));
    const uint32_t old_generation = status().generation;
    xiaotai_platform_report_unbound(true);
    xiaotai_signal_give(&start_release);
    waits(&offline, 1);
    signal_pump();
    assert(old_generation != status().generation && !atomic_load(&online) && !atomic_load(&wire_destroys));
    xiaotai_signal_give(&binding_release);
    waits(&needed, 1);
    assert(status().phase == XIAOTAI_PLATFORM_UNBOUND && atomic_load(&wire_destroys) == 1 &&
           atomic_load(&binding_callbacks_finished) == 1);
    xiaotai_signal_deinit(&binding_release);
    xiaotai_signal_deinit(&start_release);
    xiaotai_signal_deinit(&start_entered);
}

static void wire_binding_case(xiaotai_platform_config_t *c, const char *which)
{
    const bool timeout = !strcmp(which, "wire_binding_timeout");
    const bool displaced_client = !strcmp(which, "wire_binding_old_client");
    const bool queued_upgrade = !strcmp(which, "wire_binding_queued_upgrade");
    const bool owner_pressure = !strcmp(which, "wire_binding_owner_pressure");
    const bool delayed = !strcmp(which, "wire_binding_ack") || timeout || displaced_client || queued_upgrade;
    const bool notify = !strcmp(which, "wire_binding_notify");
    const bool verify_online = !strcmp(which, "wire_binding_verify_online");
    const bool upgrade = !strcmp(which, "wire_binding_upgrade");
    const bool stop_gate_inflight = !strcmp(which, "wire_binding_http_stop_gate_inflight");
    const bool stop_gate_persistent = !strcmp(which, "wire_binding_http_stop_gate_persistent");
    const bool stop_gate = stop_gate_inflight || stop_gate_persistent || !strcmp(which, "wire_binding_http_stop_gate");
    const bool stop_retry = stop_gate || !strcmp(which, "wire_binding_http_stop_retry");
    binding_verify_unbound = !strcmp(which, "wire_binding_verify_unbound");
    binding_early_ack = !strcmp(which, "wire_binding_early_ack");
    binding_http_unbound = !strcmp(which, "wire_binding_http") || stop_retry;
    assert(xiaotai_signal_init(&binding_release) == XIAOTAI_OK);
    if (stop_gate_inflight)
        assert(xiaotai_signal_init(&stop_entered) == XIAOTAI_OK && xiaotai_signal_init(&stop_release) == XIAOTAI_OK);
    wire_binding = true;
    if (stop_retry) {
        /* S3 has external memory, split SUBACKs and the legacy pool-full result. */
        wire_binding_s3 = wire_split = c->request_full_timeout = true;
    }
    if (delayed || binding_early_ack) {
        c->unbind_puback_wait_ms = 3000;
        c->unbind_command_only = true;
    }
    wire_boot(c);
    waits(&online, 1);
    settle_exchanges();
    if (displaced_client) {
        xiaotai_platform_set_network(false, NULL);
        waits(&offline, 1);
        waits(&wire_destroys, 1);
        xiaotai_platform_set_network(true, mac);
        waits(&online, 2);
        settle_exchanges();
    }
    xiaotai_mqtt_t *const m = atomic_load(&wire_client);
    const unsigned prior_offline = atomic_load(&offline), prior_destroy = atomic_load(&wire_destroys);
    const uint32_t generation = xiaotai_identity_generation();
    int32_t ack = 0, other = 0;
    uint32_t http_gate = 0;

    if (owner_pressure || queued_upgrade) {
        assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK);
        assert(xiaotai_run(binding_block_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&http_entered, WAIT));
    }

    if (binding_http_unbound) {
        assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK);
        xiaotai_platform_request_t r = {.path = "/binding-loss", .service = "device", .auth = XIAOTAI_PLATFORM_AUTH_SESSION};
        assert(xiaotai_platform_request(&r, binding_reply, &binding_request_reply) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&http_entered, WAIT));
        r = (xiaotai_platform_request_t){.path = "/never-sent", .service = "device", .auth = XIAOTAI_PLATFORM_AUTH_NONE};
        assert(xiaotai_platform_request(&r, binding_reply, &binding_stale_reply) == XIAOTAI_OK);
        if (stop_retry) {
            assert(xiaotai_platform_request(&r, binding_reply, &binding_second_stale_reply) == XIAOTAI_OK);
            if (stop_gate) assert(xiaotai_platform_gate_close(&http_gate) == XIAOTAI_OK && http_gate);
            atomic_store(&wire_block_stop, stop_gate_inflight);
            atomic_store(&bounded_stop_failures, stop_gate_persistent ? 2U : 1U);
        }
        xiaotai_signal_give(&http_release);
    } else if (owner_pressure) {
        size_t fills = 0;
        while (fills < 256U && xiaotai_run(binding_noop, NULL) == XIAOTAI_OK) ++fills;
        assert(fills > 0 && fills < 256U);
        xiaotai_platform_report_unbound(true);
        signal_pump();
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        assert(stats.event_post_retries && atomic_load(&offline) == prior_offline && atomic_load(&wire_destroys) == prior_destroy);
        xiaotai_signal_give(&http_release);
    } else if (verify_online || binding_verify_unbound || upgrade) {
        xiaotai_platform_report_unbound(false);
        xiaotai_platform_report_unbound(false);
    } else {
        xiaotai_mqtt_host_puback(m, false);
        if (delayed) {
            binding_inject(m, "device/sn_fixture/notify");
            signal_pump();
            assert(status().signaling && atomic_load(&offline) == prior_offline);
            assert(xiaotai_mqtt_publish(m, "fixture/unrelated", "x", 1, 1, XIAOTAI_MQTT_SEND_QUEUED, &other) == XIAOTAI_OK);
            assert(xiaotai_mqtt_host_flush(m, WAIT) == XIAOTAI_OK);
            assert(binding_publication(m, "fixture/unrelated") == other);
        }
        binding_inject(m, notify ? "device/sn_fixture/notify" : "device/sn_fixture/cmd");
        if (!notify) ack = binding_publication(m, "device/sn_fixture/ack");
        if (queued_upgrade) {
            xiaotai_platform_report_unbound(false);
            assert(status().verifying && xiaotai_identity_generation() != generation);
            xiaotai_signal_give(&http_release);
        }
    }

    waits(&offline, prior_offline + 1U);
    signal_pump();
    const uint32_t advanced = xiaotai_identity_generation();
    assert(advanced != generation && status().generation == advanced && !status().signaling);
    assert(atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_BINDING);
    assert(atomic_load(&wire_destroys) == prior_destroy && !atomic_load(&needed) && !atomic_load(&binding_stale_reply));
    if (upgrade) {
        assert(status().verifying);
        xiaotai_platform_report_unbound(true);
        xiaotai_platform_report_unbound(false);
        assert(!status().verifying && xiaotai_identity_generation() == advanced);
    }
    if (delayed) {
        assert(ack != other && xiaotai_mqtt_host_ack(m, (uint16_t)other) == XIAOTAI_OK);
        assert(xiaotai_mqtt_host_flush(m, WAIT) == XIAOTAI_OK);
    }
    xiaotai_signal_give(&binding_release);
    waits(&binding_callbacks_finished, 1);
    if (stop_gate_inflight) {
        assert(xiaotai_signal_take(&stop_entered, WAIT));
        assert(status().gated && atomic_load(&wire_client) == m && !atomic_load(&wire_destroys));
        assert(xiaotai_platform_gate_open(http_gate) == XIAOTAI_OK);
        xiaotai_signal_give(&stop_release);
        puts("Unbind gate opened while the failed stop was in flight");
    } else if (stop_retry) {
        waits(&binding_request_reply, 1);
        waits(&paused, 1);
        atomic_fetch_add(&warp_us, INT64_C(31000000));
        signal_pump(); /* A terminal stop error may only be retried explicitly. */
        assert(status().phase == XIAOTAI_PLATFORM_PAUSED && status().last_error == XIAOTAI_FAIL &&
               !status().signaling && !status().authorized && status().gated == stop_gate);
        assert(atomic_load(&wire_client) == m && atomic_load(&bounded_stop_attempts) == 1 &&
               !atomic_load(&wire_destroy_attempts) && !atomic_load(&needed) &&
               !atomic_load(&binding_stale_reply) && !atomic_load(&binding_second_stale_reply));
        platform_lock();
        assert(platform.queue_count == 2 && !strcmp(platform.token, "fixture-token"));
        platform_unlock();
        if (stop_gate) {
            const xiaotai_platform_request_t req = {.path = "/never-sent", .service = "device",
                .auth = XIAOTAI_PLATFORM_AUTH_NONE};
            assert(xiaotai_platform_request(&req, binding_reply, &binding_stale_reply) == XIAOTAI_ERR_PLATFORM_GATED);
            if (stop_gate_persistent)
                assert(xiaotai_platform_retry() == XIAOTAI_ERR_PLATFORM_GATED);
            assert(atomic_load(&bounded_stop_attempts) == 1 && !atomic_load(&wire_destroy_attempts));
            assert(xiaotai_platform_gate_open(http_gate) == XIAOTAI_OK);
            if (stop_gate_persistent) {
                waits(&paused, 2);
                atomic_fetch_add(&warp_us, INT64_C(31000000));
                signal_pump();
                assert(status().phase == XIAOTAI_PLATFORM_PAUSED && status().last_error == XIAOTAI_FAIL);
                assert(atomic_load(&bounded_stop_attempts) == 2 && !atomic_load(&wire_destroy_attempts) &&
                       !atomic_load(&binding_stale_reply) && !atomic_load(&binding_second_stale_reply));
                puts("Unbind gate release retried once; persistent failure still requires explicit retry");
                assert(xiaotai_platform_retry() == XIAOTAI_OK);
            } else {
                puts("Unbind gate opened; no explicit retry after release");
                fflush(stdout);
            }
        } else {
            assert(xiaotai_platform_retry() == XIAOTAI_OK);
        }
    }

    if (delayed) {
        const int64_t until = __real_xiaotai_time_us() + WAIT * INT64_C(1000);
        int64_t due;
        do {
            platform_wake();
            platform_lock();
            due = platform.binding.ack_due;
            platform_unlock();
            if (!due) xiaotai_delay_ms(1U);
        } while (!due && __real_xiaotai_time_us() < until);
        platform_lock();
        assert(platform.binding.ack_id == ack && due > xiaotai_time_us());
        platform_unlock();
        assert(atomic_load(&wire_destroys) == prior_destroy && !atomic_load(&needed));
        if (displaced_client) {
            retained_handler(retained_context, &(const xiaotai_mqtt_event_t){.kind = XIAOTAI_MQTT_PUBLISHED, .message_id = ack});
            signal_pump();
            assert(atomic_load(&wire_destroys) == prior_destroy);
        }
        if (timeout) {
            atomic_fetch_add(&warp_us, 2800000);
            signal_pump();
            assert(atomic_load(&wire_destroys) == prior_destroy);
            atomic_fetch_add(&warp_us, 250000);
            platform_wake();
        } else assert(xiaotai_mqtt_host_ack(m, (uint16_t)ack) == XIAOTAI_OK);
    }
    waits(&wire_destroys, prior_destroy + 1U);
    if (verify_online) {
        waits(&online, 2);
        assert(status().signaling && !status().verifying && !atomic_load(&needed));
    } else {
        waits(&needed, 1);
        assert(status().phase == XIAOTAI_PLATFORM_UNBOUND && atomic_load(&last_binding) == 1U);
        assert(atomic_load(&wire_binding_at) > atomic_load(&wire_offline_at));
    }
    const unsigned expected = verify_online || binding_verify_unbound || displaced_client ? 2U : 1U;
    assert(atomic_load(&logins) == expected && xiaotai_identity_generation() == advanced);
    assert(xiaotai_identity_matches("fixture", "fixture-key"));
    if (binding_http_unbound) {
        waits(&binding_request_reply, 1);
        waits(&binding_stale_reply, 1);
    }
    if (stop_retry) {
        waits(&binding_second_stale_reply, 1);
        settle_exchanges();
        assert(atomic_load(&binding_stale_reply) == 1 && atomic_load(&binding_second_stale_reply) == 1 &&
               atomic_load(&bounded_stop_attempts) == (stop_gate_persistent ? 3U : 2U) &&
               atomic_load(&wire_destroy_attempts) == 1 &&
               atomic_load(&needed) == 1 && !status().exchanges && !atomic_load(&wire_client));
        puts("Unbind completed with two stale callbacks; original identity retained");
    }
    if (binding_http_unbound || owner_pressure || queued_upgrade) {
        xiaotai_signal_deinit(&http_release);
        xiaotai_signal_deinit(&http_entered);
    }
    if (upgrade) {
        assert(xiaotai_platform_retry() == XIAOTAI_OK);
        waits(&online, 2);
        assert(atomic_load(&logins) == 2 && xiaotai_identity_generation() == advanced && !status().verifying);
    }
    if (stop_gate_inflight) {
        xiaotai_signal_deinit(&stop_release);
        xiaotai_signal_deinit(&stop_entered);
    }
    xiaotai_signal_deinit(&binding_release);
}

static atomic_uint profile_owner_jobs;
static uint32_t activity;

static void profile_owner_job(void *ctx)
{
    const char *which = ctx;
    if (!strcmp(which, "begin")) {
        assert(xiaotai_activity_begin(&(const xiaotai_activity_t){.name = "call"}, &activity) == XIAOTAI_OK);
    } else if (!strcmp(which, "end")) xiaotai_activity_end(activity);
    else if (!strcmp(which, "rtc-ready")) xiaotai_platform_profile_ready(true);
    else if (!strcmp(which, "rtc-down")) xiaotai_platform_profile_ready(false);
    else if (!strcmp(which, "parts")) {
        char out[64] = "{";
        size_t length = 1;
        assert(xiaotai_platform_scene_parts("call", "call", out, sizeof(out), &length) == 2);
        assert(length == strlen("{\"first\":2,\"second\":3") && !memcmp(out, "{\"first\":2,\"second\":3", length));
        strcpy(out, "{\"own\":1");
        length = strlen(out);
        assert(xiaotai_platform_scene_parts("call", "overflow", out, length + 1U, &length) == 0);
        assert(length == strlen("{\"own\":1") && !memcmp(out, "{\"own\":1", length));
        strcpy(out, "{");
        length = 1;
        assert(xiaotai_platform_scene_parts("call", "overflow", out, sizeof(out), &length) == 1);
        assert(length == strlen("{\"first\":2") && !memcmp(out, "{\"first\":2", length));
    } else xiaotai_platform_profile_refresh();
    ++profile_owner_jobs;
}

static void profile_job(const char *name)
{
    const unsigned next = 1U + atomic_load(&profile_owner_jobs);
    assert(xiaotai_run(profile_owner_job, (void *)name) == XIAOTAI_OK);
    waits(&profile_owner_jobs, next);
}

static void profile_advance(int64_t delta)
{
    atomic_fetch_add(&warp_us, delta);
    signal_pump();
}

static void profile_response_matrix(xiaotai_platform_config_t *config)
{
    static const struct {
        int status;
        const char *body;
    } terminal[] = {
        {200, "{\"code\":0}"}, {200, "{\"code\":200.5}"}, {200, "{\"code\":200}x"},
        {200, "{\"code\":\"200\"}"}, {200, "{\"Code\":200}"}, {400, "{\"code\":40000}"},
        {401, "{\"code\":401}"}, {410, "{\"code\":200}"},
    };
    profile_fixture = true;
    config->profile_requires_ready = true; /* Control when the first attempt takes its budget. */
    assert(config->auth_failure == XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE);
    wire_boot(config);
    for (size_t i = 0; i < sizeof(terminal) / sizeof(terminal[0]); ++i) {
        atomic_store(&profile_http, terminal[i].status);
        atomic_store(&profile_raw_reply, terminal[i].body);
        if (i == 0) {
            /* The first real port call observes the admission budget, not
             * ordinary host scheduling time between admission and I/O. */
            atomic_store(&profile_budget_clock_us, xiaotai_time_us());
            profile_job("rtc-ready");
        } else profile_job("refresh");
        waits(&profile_events, (unsigned)i + 1U);
        settle_exchanges();
        profile_advance(INT64_C(15100000));
        assert(atomic_load(&profile_budget_checked) && atomic_load(&profile_requests) == i + 1U);
        assert(!atomic_load(&profile_ready_events) && !atomic_load(&needed) && !atomic_load(&offline));
        assert(!status().profile_ready && status().phase == XIAOTAI_PLATFORM_ONLINE);
    }

    const unsigned first = sizeof(terminal) / sizeof(terminal[0]) + 1U;
    atomic_store(&profile_http, 500);
    atomic_store(&profile_raw_reply, "{\"code\":200}");
    profile_job("refresh");
    waits(&profile_requests, first);
    signal_pump();
    assert(atomic_load(&profile_events) == first - 1U && !status().profile_ready);
    profile_advance(INT64_C(14500000));
    assert(atomic_load(&profile_requests) == first);
    atomic_store(&profile_http, 429);
    atomic_store(&profile_raw_reply, "");
    profile_advance(INT64_C(600000));
    waits(&profile_requests, first + 1U);
    signal_pump();
    profile_advance(INT64_C(14500000));
    assert(atomic_load(&profile_requests) == first + 1U && atomic_load(&profile_events) == first - 1U);
    atomic_store(&profile_http, 0);
    profile_advance(INT64_C(600000));
    waits(&profile_events, first);
    settle_exchanges();
    profile_advance(INT64_C(15100000));
    assert(atomic_load(&profile_requests) == first + 2U && !atomic_load(&profile_ready_events));
    assert(!status().profile_ready && !atomic_load(&needed));
}

typedef enum { PROFILE_PRESSURE_SUCCESS, PROFILE_PRESSURE_STALE, PROFILE_PRESSURE_RETRY } profile_pressure_t;

static unsigned profile_count_after(const unsigned minimum, bool post)
{
    const int64_t until = __real_xiaotai_time_us() + WAIT * INT64_C(1000);
    unsigned count;
    do {
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        count = post ? stats.reply_post_retries : stats.profile_failures;
        if (count > minimum) return count;
        xiaotai_delay_ms(1);
    } while (__real_xiaotai_time_us() < until);
    assert(count > minimum);
    return count;
}

static void profile_pressure_retry_exhausted(void)
{
    for (unsigned attempt = 1; attempt < 3U; ++attempt) {
        assert(profile_count_after(attempt - 1U, false) == attempt);
        profile_advance(INT64_C(14500000));
        assert(atomic_load(&profile_requests) == attempt && !atomic_load(&profile_events));
        atomic_fetch_add(&warp_us, INT64_C(600000));
        platform_wake();
        assert(xiaotai_signal_take(&start_entered, WAIT));
        assert(atomic_load(&profile_requests) == attempt + 1U);
        xiaotai_signal_give(&start_release);
    }
    waits(&profile_events, 1);
    settle_exchanges();
    assert(profile_count_after(2U, false) == 3U);
    profile_advance(INT64_C(15100000));
    assert(atomic_load(&profile_requests) == 3 && atomic_load(&profile_events) == 1);
    assert(!status().profile_ready && !atomic_load(&profile_ready_events));

    atomic_store(&profile_http, 200);
    profile_job("refresh");
    assert(xiaotai_signal_take(&start_entered, WAIT));
    xiaotai_signal_give(&start_release);
    waits(&profile_ready_events, 1);
    settle_exchanges();
    assert(atomic_load(&profile_requests) == 4 && atomic_load(&profile_events) == 2 && status().profile_ready);
}

static void profile_response_pressure(xiaotai_platform_config_t *config, profile_pressure_t pressure)
{
    const bool stale = pressure == PROFILE_PRESSURE_STALE;
    profile_fixture = profile_reply_pressure = true;
    atomic_store(&profile_http, pressure == PROFILE_PRESSURE_RETRY ? 503 : 200);
    atomic_store(&profile_raw_reply, "{\"code\":200}\n");
    assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK &&
           xiaotai_signal_init(&start_entered) == XIAOTAI_OK && xiaotai_signal_init(&start_release) == XIAOTAI_OK);
    wire_boot(config);
    assert(xiaotai_signal_take(&start_entered, WAIT));
    profile_job("refresh");
    assert(xiaotai_run(owner_block, NULL) == XIAOTAI_OK && xiaotai_signal_take(&http_entered, WAIT));
    unsigned filled = 0;
    xiaotai_err_t enqueue;
    while ((enqueue = xiaotai_run(owner_noop, NULL)) == XIAOTAI_OK) assert(++filled < 1024U);
    assert(filled && enqueue == XIAOTAI_ERR_NO_MEM);
    xiaotai_signal_give(&start_release);
    const unsigned posted_failures = profile_count_after(0, true);
    assert(!atomic_load(&profile_events) && status().exchanges == 1);
    if (pressure == PROFILE_PRESSURE_RETRY) {
        /* The completed HTTP failure waits for its owner, even past a retry interval. */
        atomic_fetch_add(&warp_us, INT64_C(15100000));
        platform_wake();
        (void)profile_count_after(posted_failures, true);
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        assert(atomic_load(&profile_requests) == 1 && !stats.profile_failures && !atomic_load(&profile_events));
        xiaotai_signal_give(&http_release);
        profile_pressure_retry_exhausted();
    } else {
        if (stale) xiaotai_platform_set_network(false, NULL);
        xiaotai_signal_give(&http_release);
        if (stale) waits(&offline, 1);
        else waits(&profile_ready_events, 1);
        settle_exchanges();
        profile_advance(INT64_C(15100000));
        assert(atomic_load(&profile_requests) == 1 && !atomic_load(&needed) && status().profile_ready != stale);
        assert(atomic_load(&profile_events) == (stale ? 0U : 1U));
    }
    xiaotai_signal_deinit(&start_release);
    xiaotai_signal_deinit(&start_entered);
    xiaotai_signal_deinit(&http_release);
    xiaotai_signal_deinit(&http_entered);
}

static void wire_profile_case(xiaotai_platform_config_t *c, const char *which)
{
    if (!strcmp(which, "wire_profile_response_matrix")) return profile_response_matrix(c);
    if (!strcmp(which, "wire_profile_reply_pressure")) return profile_response_pressure(c, PROFILE_PRESSURE_SUCCESS);
    if (!strcmp(which, "wire_profile_reply_pressure_stale")) return profile_response_pressure(c, PROFILE_PRESSURE_STALE);
    if (!strcmp(which, "wire_profile_reply_pressure_retry")) return profile_response_pressure(c, PROFILE_PRESSURE_RETRY);
    const bool mz28 = !strcmp(which, "wire_profile_mz28");
    const bool retry = !strcmp(which, "wire_profile_retry");
    const bool biz_retry = !strcmp(which, "wire_profile_biz_retry");
    const bool no_biz_retry = !strcmp(which, "wire_profile_no_biz_retry");
    const bool no_retry = !strcmp(which, "wire_profile_no_retry");
    const bool unbound = !strcmp(which, "wire_profile_unbound");
    const bool defer = !strcmp(which, "wire_profile_defer");
    const bool capped = !strcmp(which, "wire_profile_large_cap");
    const bool needs_rtc = !strcmp(which, "wire_profile_wait_rtc");
    const bool rtc_first = !strcmp(which, "wire_profile_rtc_first");
    profile_fast_hold = !strcmp(which, "wire_profile_fast_hold");
    profile_preflight_gated = !strcmp(which, "wire_profile_preflight_gated");
    if (!strcmp(which, "wire_profile_preflight_hold")) profile_hold_at = PROFILE_HOLD_PREFLIGHT;
    if (!strcmp(which, "wire_profile_prewire_hold")) profile_hold_at = PROFILE_HOLD_PREWIRE;
    profile_large = capped || !strcmp(which, "wire_profile_large");
    profile_fill_slot = !strcmp(which, "wire_profile_queue");
    profile_fail_writer = !strcmp(which, "wire_profile_writer_error");
    profile_hold_writer = !strcmp(which, "wire_profile_stale_writer");
    profile_fixture = true;
    atomic_store(&profile_http, unbound ? 410 : retry ? 503 : no_retry ? 503 : 200);
    atomic_store(&profile_code, unbound ? 6006 : biz_retry || no_biz_retry ? 50000 : retry || no_retry ? 0 : 200);
    c->profile_omit_firmware_version = mz28;
    if (mz28) c->chip_model = c->board_model = NULL;
    c->profile_attempts = no_retry ? 1 : 3;
    c->profile_retry_business_errors = biz_retry;
    c->defer_background_while_active = defer;
    c->profile_requires_ready = needs_rtc || rtc_first;
    if (profile_hold_at != PROFILE_NO_HOLD || profile_preflight_gated) {
        c->requests_need_signaling = true;
        c->defer_background_while_active = true;
        c->profile_attempts = 1;
    }
    if (profile_large) {
        c->chip_model = "ESP32-P4";
        c->board_model = "Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5";
        if (capped) c->request_body_bytes = 1024;
    }

    if (profile_hold_writer)
        assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK);
    if (profile_fill_slot || profile_fast_hold)
        assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK &&
               xiaotai_signal_init(&start_entered) == XIAOTAI_OK && xiaotai_signal_init(&start_release) == XIAOTAI_OK);
    if (defer || rtc_first || needs_rtc) {
        xiaotai_clock_host_set_wall_ms(WALL_SECONDS * 1000);
        boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
        if (defer) profile_job("begin");
        if (rtc_first) profile_job("rtc-ready");
        xiaotai_platform_set_network(true, mac);
        waits(&online, 1);
        if (defer) {
            signal_pump();
            xiaotai_platform_stats_t stats;
            xiaotai_platform_stats(&stats);
            assert(stats.profile_deferrals == 1 && status().exchanges == 1 && xiaotai_platform_exchanges_busy());
            assert(!atomic_load(&profile_requests) && !atomic_load(&profile_writes));
            profile_job("end");
        } else if (needs_rtc) {
            profile_job("refresh");
            signal_pump();
            assert(!xiaotai_platform_exchanges_busy() && !atomic_load(&profile_requests) && !status().profile_ready);
            profile_job("rtc-ready");
        }
    } else wire_boot(c);
    waits(&online, 1);

    if (profile_preflight_gated) {
        waits(&profile_events, 1);
        settle_exchanges();
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        assert(stats.profile_failures == 1 && !atomic_load(&profile_requests));
        assert(status().signaling && !status().held && !status().profile_ready);
        return;
    }

    if (profile_hold_at != PROFILE_NO_HOLD) {
        waits(&hold_results, 1);
        assert(atomic_load(&hold_error) == XIAOTAI_OK && atomic_load(&wire_destroys) == 1);
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        assert(!atomic_load(&profile_requests) && !atomic_load(&profile_events) && !stats.profile_failures);
        assert(!status().profile_ready && !status().signaling && status().held);
        const uint32_t generation = atomic_load(&profile_hold_generation);
        assert(generation == xiaotai_identity_generation());
        const uint32_t ticket = atomic_load(&profile_hold_ticket);
        assert(ticket);
        xiaotai_platform_signaling_release(ticket);
        waits(&online, 2);
        waits(&profile_events, 1);
        settle_exchanges();
        assert(status().profile_ready && status().generation == generation);
        assert(atomic_load(&profile_requests) == 1 && atomic_load(&profile_ready_events) == 1);
        assert(atomic_load(&wire_creates) == 2 && atomic_load(&logins) == 1);
        return;
    }

    if (profile_large) {
        waits(&profile_events, 1);
        char payload[800];
        memset(payload, 'a', sizeof(payload) - 1U);
        payload[sizeof(payload) - 1U] = '\0';
        const xiaotai_platform_request_t ordinary = {.method = XIAOTAI_PLATFORM_POST, .service = "device",
            .path = "/ordinary-cannot-use-extra-capacity", .auth = XIAOTAI_PLATFORM_AUTH_SESSION, .body = payload};
        assert(xiaotai_platform_request(&ordinary, occupy_slot_reply, NULL) == XIAOTAI_ERR_INVALID_ARG);
        assert(platform.request_limit == 768 && platform.profile_limit == (capped ? 1024U : 2048U));
        if (capped) assert(!status().profile_ready && !atomic_load(&profile_requests));
        else {
            assert(status().profile_ready && atomic_load(&profile_requests) == 1 && atomic_load(&profile_writes) == 3);
            assert(strlen(profile_body) == 1102 && strstr(profile_body, "\"stream\":{\"pad\":") &&
                   strstr(profile_body, "\"call\":{\"pad\":") && strstr(profile_body, "\"voip\":{\"pad\":"));
        }
        settle_exchanges();
        return;
    }
    if (needs_rtc || rtc_first) {
        waits(&profile_events, 1);
        assert(status().profile_ready && atomic_load(&profile_requests) == 1);
        settle_exchanges();
        /* Broker reconnect and physical RTC readiness are independent. */
        if (needs_rtc) profile_job("rtc-down");
        xiaotai_mqtt_t *m = atomic_load(&wire_client);
        assert(xiaotai_mqtt_host_disconnect(m) == XIAOTAI_OK && xiaotai_mqtt_host_flush(m, WAIT) == XIAOTAI_OK);
        waits(&offline, 1);
        xiaotai_mqtt_host_advance_ms(m, 4000);
        waits(&online, 2);
        if (needs_rtc) {
            signal_pump();
            assert(!status().profile_ready && !xiaotai_platform_exchanges_busy() && atomic_load(&profile_requests) == 1);
            profile_job("rtc-ready");
        }
        waits(&profile_events, 2);
        assert(status().profile_ready && atomic_load(&profile_requests) == 2);
        settle_exchanges();
        return;
    }
    if (profile_fast_hold) {
        assert(xiaotai_signal_take(&http_entered, WAIT) && xiaotai_signal_take(&start_entered, WAIT));
        uint32_t hold;
        assert(xiaotai_platform_signaling_hold(NULL, NULL, &hold) == XIAOTAI_OK && hold);
        xiaotai_signal_give(&http_release);
        profile_job("rtc-down"); /* Owner fence: the already posted report ran first. */
        platform_lock();
        assert(platform.profile.phase == PROFILE_WAIT && !atomic_load(&profile_writes));
        platform_unlock();
        xiaotai_platform_signaling_release(hold);
        xiaotai_signal_give(&start_release);
        waits(&profile_events, 1);
        settle_exchanges();
        assert(status().profile_ready && !status().held && atomic_load(&wire_creates) == 1 && !atomic_load(&wire_destroys));
        xiaotai_signal_deinit(&start_release);
        xiaotai_signal_deinit(&start_entered);
        xiaotai_signal_deinit(&http_release);
        xiaotai_signal_deinit(&http_entered);
        return;
    }

    if (profile_fill_slot) {
        assert(xiaotai_signal_take(&http_entered, WAIT));
        const int64_t until = __real_xiaotai_time_us() + WAIT * INT64_C(1000);
        xiaotai_platform_stats_t stats;
        do {
            xiaotai_platform_stats(&stats);
            if (stats.profile_failures) break;
            xiaotai_delay_ms(1U);
        } while (__real_xiaotai_time_us() < until);
        assert(stats.profile_failures == 1 && status().exchanges == 2 && !atomic_load(&profile_writes));
        xiaotai_signal_give(&http_release);
        waits(&request_done, 1);
        atomic_fetch_add(&warp_us, INT64_C(15100000));
        platform_wake();
        assert(xiaotai_signal_take(&start_entered, WAIT));
        profile_job("refresh");
        assert(status().exchanges == 1 && atomic_load(&profile_requests) == 1);
        xiaotai_signal_give(&start_release);
        waits(&profile_events, 1);
        profile_advance(INT64_C(15100000));
        assert(atomic_load(&profile_requests) == 1 && status().profile_ready && !xiaotai_platform_exchanges_busy());
        xiaotai_signal_deinit(&start_release);
        xiaotai_signal_deinit(&start_entered);
        xiaotai_signal_deinit(&http_release);
        xiaotai_signal_deinit(&http_entered);
        return;
    }

    if (profile_hold_writer) {
        assert(xiaotai_signal_take(&http_entered, WAIT));
        xiaotai_platform_set_network(false, NULL);
        xiaotai_signal_give(&http_release);
        waits(&offline, 1);
        signal_pump();
        assert(!atomic_load(&profile_requests) && !atomic_load(&profile_events) && status().exchanges == 0);
        xiaotai_signal_deinit(&http_release);
        xiaotai_signal_deinit(&http_entered);
        return;
    }
    if (retry || biz_retry) {
        waits(&profile_requests, 1);
        signal_pump();
        assert(!status().profile_ready && !atomic_load(&profile_events));
        atomic_store(&profile_http, retry ? 429 : 200);
        atomic_store(&profile_code, 0);
        profile_advance(INT64_C(14500000));
        assert(atomic_load(&profile_requests) == 1);
        profile_advance(INT64_C(600000));
        waits(&profile_requests, 2);
        signal_pump();
        atomic_store(&profile_http, 200);
        atomic_store(&profile_code, 200);
        profile_advance(INT64_C(15100000));
        waits(&profile_events, 1);
        assert(status().profile_ready && atomic_load(&profile_requests) == 3);
    } else if (no_retry || no_biz_retry || profile_fail_writer) {
        waits(&profile_events, 1);
        assert(!status().profile_ready);
        atomic_store(&profile_http, 200);
        atomic_store(&profile_code, 200);
        profile_advance(INT64_C(20000000));
        assert(atomic_load(&profile_requests) == (profile_fail_writer ? 0U : 1U));
        if (no_retry) {
            profile_job("refresh");
            waits(&profile_events, 2);
            assert(status().profile_ready && atomic_load(&profile_requests) == 2);
        }
    } else if (unbound) {
        waits(&needed, 1);
        assert(status().phase == XIAOTAI_PLATFORM_UNBOUND && !status().profile_ready &&
               atomic_load(&offline_reason) == XIAOTAI_PLATFORM_OFFLINE_BINDING);
        assert(xiaotai_identity_matches("fixture", "fixture-key"));
    } else {
        waits(&profile_events, 1);
        assert(status().profile_ready && atomic_load(&profile_ready_events) == 1 && atomic_load(&profile_requests) == 1);
        if (mz28) assert(!strcmp(profile_body, "{\"profiles\":{\"call\":{\"own\":1,\"first\":2,\"second\":3}}}"));
        else assert(!strcmp(profile_body, "{\"hardware\":{\"chip_model\":\"Fixture chip\",\"board_model\":\"Fixture board\"},"
                    "\"firmware_version\":\"host\",\"profiles\":{\"call\":{\"own\":1,\"first\":2,\"second\":3}}}"));
        xiaotai_platform_stats_t stats;
        xiaotai_platform_stats(&stats);
        assert(stats.profile_parts_dropped == 2 && stats.profile_reports == 1 && !stats.profile_failures);
        if (!mz28 && !defer) {
            profile_job("parts");
            profile_job("rtc-ready");
            profile_job("rtc-down");
            signal_pump();
            assert(atomic_load(&profile_requests) == 1 && status().profile_ready);
            xiaotai_platform_stats(&stats);
            assert(stats.profile_parts_dropped == 7);
        }
    }
    signal_pump();
    assert(!xiaotai_platform_exchanges_busy());
}

static size_t console_read(void *self, uint8_t *out, size_t max, uint32_t timeout_ms)
{
    (void)self;
    const size_t bytes = at_input ? strlen(at_input) : 0;
    const size_t copied = bytes < max ? bytes : max;
    if (copied) { memcpy(out, at_input, copied); at_input += copied; }
    else __real_xiaotai_delay_ms(timeout_ms);
    return copied;
}

static xiaotai_err_t console_write(void *self, const char *line, size_t bytes)
{
    (void)self;
    assert(console_running && !xiaotai_on_owner() && bytes + at_used < sizeof(at_output));
    memcpy(at_output + at_used, line, bytes);
    at_used += bytes;
    at_output[at_used] = '\0';
    return XIAOTAI_OK;
}

static void issue(const char *text)
{
    at_input = text;
    at_used = 0;
    at_output[0] = '\0';
    console_running = true;
    do { assert(xiaotai_at_service(3) == XIAOTAI_OK); } while (*at_input);
    console_running = false;
}

static void wire_reconnect_command(xiaotai_platform_config_t *c, const char *which)
{
    wire_at = true;
    wire_no_reap = !strcmp(which, "wire_at_unavailable");
    wire_timeout = !strcmp(which, "wire_at_timeout");
    static const xiaotai_at_io_t source = {.name = "fixture", .read = console_read, .write = console_write};
    const xiaotai_at_config_t at = {.no_platform_console = true, .task_stack_bytes = XIAOTAI_AT_NO_TASK};
    assert(xiaotai_at_configure(&at) == XIAOTAI_OK && xiaotai_at_attach(&source) == XIAOTAI_OK &&
           xiaotai_at_open() == XIAOTAI_OK);
    if (!strcmp(which, "wire_at_offline")) boot(c, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
    else {
        wire_boot(c);
        waits(&online, 1);
    }
    xiaotai_engine_at_start();
    issue("AT+HELP\r\n");
    const char *const help = "AT+PLATFORM RECONNECT [<1..50>]";
    assert((strstr(at_output, help) != NULL) == (XIAOTAI_PLATFORM_RECONNECT_COMMAND != 0));
    if (!XIAOTAI_PLATFORM_RECONNECT_COMMAND || !strcmp(which, "wire_at_help")) return;
    if (!strcmp(which, "wire_at_invalid")) {
        issue("AT+PLATFORM RECONNECT 0\r\n");
        assert(strstr(at_output, "ERROR: INVALID_ARG"));
        issue("AT+PLATFORM RECONNECT 51\r\n");
        assert(strstr(at_output, "ERROR: INVALID_ARG"));
        assert(atomic_load(&wire_creates) == 1 && !atomic_load(&wire_destroys));
        return;
    }
    if (!strcmp(which, "wire_at_offline")) {
        issue("AT+PLATFORM RECONNECT\r\n");
        assert(strstr(at_output, "ERROR:") && strstr(at_output, "RECONNECT") && !atomic_load(&wire_creates));
        return;
    }
    if (!strcmp(which, "wire_at_busy")) {
        uint32_t ticket;
        assert(xiaotai_platform_signaling_hold(hold_done, &hold_results, &ticket) == XIAOTAI_OK);
        waits(&hold_results, 1);
        issue("AT+PLATFORM RECONNECT\r\n");
        assert(strstr(at_output, "ERROR: BUSY RECONNECT") && status().held && !wire_samples);
        xiaotai_platform_signaling_release(ticket);
        waits(&online, 2);
        return;
    }
    if (!strcmp(which, "wire_at_stop_failure")) {
        atomic_store(&wire_fail_stop, true);
        issue("AT+PLATFORM RECONNECT\r\n");
        assert(strstr(at_output, "ERROR: FAIL RECONNECT") && !status().held && !wire_samples);
        assert(xiaotai_platform_retry() == XIAOTAI_OK);
        waits(&online, 2);
        return;
    }
    const bool stress = !strcmp(which, "wire_at_stress");
    issue(stress ? "AT+PLATFORM RECONNECT 50\r\n" : "AT+PLATFORM RECONNECT\r\n");
    assert(!status().held);
    if (wire_timeout) {
        assert(strstr(at_output, "ERROR: TIMEOUT RECONNECT") && !wire_samples && atomic_load(&warp_us) >= 30000000);
        assert(!status().signaling && atomic_load(&wire_creates) == 2);
        return;
    }
    assert(strstr(at_output, "OK\r\n") && status().signaling && status().notify_revision == (stress ? 51U : 2U));
    assert(strstr(at_output, wire_no_reap ? "round=1 free=unavailable largest=unavailable" :
                                          "round=1 free=8192 largest=4096"));
    assert(wire_samples == (wire_no_reap ? 0U : stress ? 50U : 1U));
    if (stress) assert(strstr(at_output, "round=50 free=8192 largest=4096"));
    if (!wire_no_reap) {
        xiaotai_eventlog_topic_snapshot_t topic;
        assert(xiaotai_eventlog_topic_read(0, &topic) && !strcmp(topic.desc->name, "reconnect"));
        assert(topic.posts == wire_samples && topic.values[0] == wire_samples && topic.values[1] == 8192 &&
               topic.values[2] == 4096);
    }
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "network";
    bool matches_old = true;
    const xiaotai_platform_config_t invalid = {.chip_model = "bad\\model", .board_model = "valid"};
    assert(xiaotai_platform_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    xiaotai_clock_host_answer(WALL_SECONDS * 1000, 0);
    xiaotai_platform_config_t config = {.discovery_url = "https://fixture/services", .retry_ms = XIAOTAI_PLATFORM_NO_RETRY,
        .chip_model = "Fixture chip", .board_model = "Fixture board"};
    if (!strcmp(which, "expiry")) { expiry(); return 0; }
    if (!strncmp(which, "wire_", 5)) {
        wire_fixture = true;
        if (!strncmp(which, "wire_at_", 8)) wire_reconnect_command(&config, which);
        else if (!strncmp(which, "wire_profile_", 13)) wire_profile_case(&config, which);
        else if (!strcmp(which, "wire_binding_start")) wire_binding_start(&config);
        else if (!strncmp(which, "wire_binding_", 13)) wire_binding_case(&config, which);
        else if (!strcmp(which, "wire_batch") || !strcmp(which, "wire_split")) wire_subscribe(&config, !strcmp(which, "wire_split"));
        else if (!strcmp(which, "wire_split_reconnect")) wire_split_reconnect_cycles(&config);
        else if (!strcmp(which, "wire_split_request_failure")) wire_split_failed_subscribe(&config);
        else if (!strcmp(which, "wire_split_legacy_recovery")) wire_split_legacy_timing(&config, true);
        else if (!strcmp(which, "wire_split_legacy_exhaustion")) wire_split_legacy_timing(&config, false);
        else if (!strcmp(which, "wire_split_legacy_refusal")) wire_split_legacy_refusal(&config);
        else if (!strcmp(which, "wire_split_legacy_stop")) wire_split_legacy_stop(&config);
        else if (!strcmp(which, "wire_split_legacy_unbound_ack")) assert(wire_split_legacy_unbound(&config, false));
        else if (!strcmp(which, "wire_split_legacy_unbound_retry")) matches_old = wire_split_legacy_unbound(&config, true);
        else if (!strcmp(which, "wire_split_malformed_ack") || !strcmp(which, "wire_split_stale_ack_error") ||
                 !strcmp(which, "wire_split_uncorrelated_error")) {
            wire_ack_case = !strcmp(which, "wire_split_malformed_ack") ? WIRE_ACK_CORRELATED_ERROR :
                (!strcmp(which, "wire_split_stale_ack_error") ? WIRE_ACK_STALE_ERROR : WIRE_ACK_UNCORRELATED_ERROR);
            wire_split_declined_ack(&config);
        }
        else if (!strcmp(which, "wire_split_refused") || !strcmp(which, "wire_split_excess_qos") ||
                 !strcmp(which, "wire_split_excess_count")) {
            if (!strcmp(which, "wire_split_refused")) wire_ack_case = WIRE_ACK_REFUSED;
            else if (!strcmp(which, "wire_split_excess_qos")) wire_ack_case = WIRE_ACK_EXCESS_QOS;
            else wire_ack_case = WIRE_ACK_EXCESS_COUNT;
            wire_split_declined_ack(&config);
        }
        else if (!strcmp(which, "wire_retry")) wire_retry(&config);
        else if (!strcmp(which, "wire_batch_hosted_retry")) wire_hosted_batch_retry(&config);
        else if (!strcmp(which, "wire_batch_hosted_stop")) wire_hosted_batch_stop(&config);
        else if (!strcmp(which, "wire_batch_hosted_binding")) wire_hosted_batch_binding(&config);
        else if (!strcmp(which, "wire_lifecycle")) wire_lifecycle(&config);
        else if (!strcmp(which, "wire_signal")) wire_signal(&config);
        else if (!strcmp(which, "wire_refused")) {
            wire_codes[1] = XIAOTAI_MQTT_REFUSED;
            config.auth_failure = XIAOTAI_PLATFORM_AUTH_FAILURE_PAUSE;
            wire_boot(&config);
            phase(XIAOTAI_PLATFORM_PAUSED);
            assert(atomic_load(&wire_destroys) == 1 && !status().authorized && !atomic_load(&online));
        } else if (!strcmp(which, "wire_hold_early")) wire_hold_early(&config);
        else if (!strcmp(which, "wire_hold_startup")) wire_hold_startup(&config);
        else if (!strncmp(which, "wire_hold_bounded_", sizeof("wire_hold_bounded_") - 1U))
            wire_hold_bounded(&config, which);
        else if (!strcmp(which, "wire_hold_stop_failure") || !strcmp(which, "wire_hold_start_failure"))
            wire_hold_failure(&config, !strcmp(which, "wire_hold_stop_failure"));
        else if (!strcmp(which, "wire_hold_mqtt_lifetime")) wire_hold_mqtt_lifetime(&config);
        else if (!strncmp(which, "wire_hold_", 10)) wire_hold(&config, !strcmp(which, "wire_hold_relogin"));
        else if (!strncmp(which, "wire_heartbeat_", 15)) wire_heartbeat(&config, !strcmp(which, "wire_heartbeat_ready"));
        else assert(false);
    } else if (!strcmp(which, "network")) network(&config);
    else if (!strcmp(which, "broker_gate")) broker_gate(&config);
    else if (!strcmp(which, "broker_start_gate")) broker_start_gate(&config);
    else if (!strcmp(which, "discovery_stale")) {
        block_discovery = true;
        assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK && xiaotai_signal_init(&http_release) == XIAOTAI_OK);
        boot(&config, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
        up(11, true);
        assert(xiaotai_signal_take(&http_entered, WAIT));
        platform_session_network(true, mac, true, 12);
        char url[XIAOTAI_PLATFORM_URL_MAX];
        assert(xiaotai_platform_endpoint("mqtt", url, sizeof(url)) == XIAOTAI_ERR_PLATFORM_OFFLINE);
        xiaotai_signal_give(&http_release);
        waits(&online, 1);
        assert(atomic_load(&discoveries) == 2 && atomic_load(&logins) == 1);
        xiaotai_signal_deinit(&http_release);
        xiaotai_signal_deinit(&http_entered);
    } else if (!strcmp(which, "provision") || !strcmp(which, "boot_error") ||
               !strcmp(which, "boot_error_type") || !strcmp(which, "clock_config")) {
        provision = !strcmp(which, "provision");
        const bool corrupt_type = !strcmp(which, "boot_error_type");
        if (!strcmp(which, "boot_error"))
            assert(xiaotai_kv_set_str("service_env", "ep_base", "ftp://invalid", WAIT) == XIAOTAI_OK);
        if (corrupt_type) {
            static const char wrong_type[] = "http://valid-looking-blob";
            assert(xiaotai_kv_set("service_env", "ep_base", wrong_type, sizeof(wrong_type), WAIT) == XIAOTAI_OK);
        }
        config_clock = !strcmp(which, "clock_config");
        config.timezone = "UTC0";
        config.clock_probe = true;
        config.clock_release_after_sync = true;
        config.clock_trust_kept_time = config_clock;
        if (config_clock) xiaotai_clock_host_set_wall_ms(WALL_SECONDS * 1000);
        boot(&config, true, XIAOTAI_FINGERPRINT_UPPER_COLON, NULL);
        char zone[XIAOTAI_CLOCK_NAME_MAX];
        assert(xiaotai_clock_timezone(zone, sizeof(zone)) == XIAOTAI_OK && !strcmp(zone, "UTC0"));
        xiaotai_platform_set_network(true, mac);
        if (config_clock) {
            waits(&online, 1);
            xiaotai_clock_stats_t cs;
            xiaotai_clock_stats(&cs);
            assert(!cs.starts && !cs.running);
        } else {
            xiaotai_delay_ms(20);
            assert(status().phase == XIAOTAI_PLATFORM_STOPPED && !atomic_load(&discoveries));
            if (!provision) {
                xiaotai_platform_environment_t env;
                xiaotai_platform_environment(&env);
                assert(env.boot_error != XIAOTAI_OK && xiaotai_platform_retry() == XIAOTAI_ERR_PLATFORM_OFFLINE);
                if (corrupt_type) {
                    assert(env.boot_error == XIAOTAI_ERR_INVALID_ARG && status().last_error == XIAOTAI_ERR_INVALID_ARG);
                    assert(!env.active[0] && !env.saved[0] && !env.next[0] && !atomic_load(&logins));
                }
            }
        }
    } else if (!strncmp(which, "auth", 4)) {
        config.retry_ms = 5000;
        auth_link = !strcmp(which, "auth_pause_link");
        config.auth_failure = !strcmp(which, "auth_pause") || auth_link ? XIAOTAI_PLATFORM_AUTH_FAILURE_PAUSE :
            !strcmp(which, "auth_relogin") ? XIAOTAI_PLATFORM_AUTH_FAILURE_RELOGIN : XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE;
        auth_case(&config);
    } else if (!strcmp(which, "refresh_cooldown")) refresh_cooldown(&config);
    else if (!strncmp(which, "c3_token_", sizeof("c3_token_") - 1U))
        c3_token_admission(&config, which + sizeof("c3_token_") - 1U);
    else if (!strcmp(which, "refresh_c3_pause") || !strcmp(which, "refresh_c3_media"))
        refresh_c3_pause(&config, !strcmp(which, "refresh_c3_media"));
    else if (!strncmp(which, "refresh", 7)) {
        config.token_refresh = !strcmp(which, "refresh_on");
        refresh(&config);
    } else {
        const bool missing = !strcmp(which, "missing");
        const bool unbound = !strcmp(which, "unbound");
        const bool strict = !strcmp(which, "mqtt_bad");
        const bool unchecked = !strcmp(which, "mqtt_unchecked");
        const bool empty = !strcmp(which, "mqtt_empty");
        const bool over = !strcmp(which, "mqtt_over");
        const bool required = !strcmp(which, "missing_service");
        const bool duplicate = !strcmp(which, "login_duplicate");
        const bool require_expiry = !strcmp(which, "login_requires_expiry");
        const bool forwarded_error = !strcmp(which, "transport_not_found");
        const bool bad_login = !strncmp(which, "login_wire_", sizeof("login_wire_") - 1U);
        const bool lower = !strcmp(which, "fingerprint_lower");
        const bool compact = !strcmp(which, "fingerprint_compact");
        signature_fixture = !strcmp(which, "signature");
        config.token_needs_expiry = require_expiry;
        if (signature_fixture) xiaotai_clock_host_set_wall_ms(WALL_SECONDS * 1000);
        start_fails = !strcmp(which, "start_fails");
        require_ai = required;
        if (strict || unchecked) {
            expected_broker = "ws://h";
            config.mqtt_address_unchecked = unchecked;
            services_json = "{\"device-srv\":\"http://fixture/device\",\"mqtt-srv\":\"ws://h\"}";
        }
        static char big_services[512];
        if (empty || over) {
            char address[257] = {0};
            if (over) memset(address, 'a', sizeof(address) - 1U);
            (void)snprintf(big_services, sizeof(big_services),
                "{\"device-srv\":\"http://fixture/device\",\"mqtt-srv\":\"%s\"}", address);
            services_json = big_services;
            config.mqtt_address_unchecked = true;
        }
        if (required) services_json = "{\"device-srv\":\"http://fixture/device\",\"mqtt-srv\":\"mqtt://h\"}";
        if (unbound || forwarded_error) { login_http = 410; login_json = "{\"code\":6006}"; }
        if (duplicate) login_json = "{\"code\":200,\"code\":200,\"data\":{\"mqtt_token\":\"ignored\"}}";
        if (forwarded_error) transport_error = XIAOTAI_ERR_NOT_FOUND;
        if (bad_login) {
            static const struct {
                const char *scenario, *body;
                int status;
                xiaotai_err_t transfer;
            } cases[] = {
                {"login_wire_server_error", "{\"code\":6006}", 503, XIAOTAI_OK},
                {"login_wire_wrong_status", "{\"code\":6006}", 200, XIAOTAI_OK},
                {"login_wire_fractional", "{\"code\":6006.5}", 410, XIAOTAI_OK},
                {"login_wire_string", "{\"code\":\"6006\"}", 410, XIAOTAI_OK},
                {"login_wire_duplicate", "{\"code\":6006,\"code\":0}", 410, XIAOTAI_OK},
                {"login_wire_trailing", "{\"code\":6006}tail", 410, XIAOTAI_OK},
                {"login_wire_transport_timeout", "{\"code\":6006}", 410, XIAOTAI_ERR_TIMEOUT},
                {"login_wire_success_fractional", "{\"code\":200.5,\"data\":{\"mqtt_token\":\"ignored\"}}", 200, XIAOTAI_OK},
            };
            size_t at = 0;
            while (at < sizeof(cases) / sizeof(cases[0]) && strcmp(which, cases[at].scenario)) ++at;
            assert(at < sizeof(cases) / sizeof(cases[0]));
            login_http = cases[at].status;
            login_json = cases[at].body;
            transport_error = cases[at].transfer;
        }
        if (lower) expected_mac = "02:ab:cd:ee:ff:10";
        if (compact) expected_mac = "02ABCDEEFF10";
        atomic_store(&no_provider, !strcmp(which, "no_provider"));
        boot(&config, !missing, lower ? XIAOTAI_FINGERPRINT_LOWER_COLON : XIAOTAI_FINGERPRINT_UPPER_COLON,
            compact ? expected_mac : NULL);
        xiaotai_platform_set_network(true, mac);
        char url[256];
        if (start_fails) {
            phase(XIAOTAI_PLATFORM_PAUSED);
            waits(&destroys, 1);
            assert(!status().authorized && !status().signaling && !atomic_load(&online));
        } else if (strict || required || duplicate || forwarded_error || empty || over || require_expiry || bad_login) {
            phase(XIAOTAI_PLATFORM_PAUSED);
            assert(!status().authorized && !atomic_load(&starts));
            assert(xiaotai_platform_endpoint("mqtt", url, sizeof(url)) == (strict || required || empty || over ?
                XIAOTAI_ERR_PLATFORM_OFFLINE : XIAOTAI_OK));
            assert(!atomic_load(&needed));
            if (bad_login || forwarded_error) {
                assert(status().last_error == (transport_error ? transport_error : XIAOTAI_FAIL));
                assert(atomic_load(&logins) == 1U && xiaotai_identity_matches("fixture", "fixture-key"));
            }
        } else if (missing || unbound) {
            phase(missing ? XIAOTAI_PLATFORM_NO_IDENTITY : XIAOTAI_PLATFORM_UNBOUND);
            waits(&needed, 1);
            assert(atomic_load(&last_binding) == (unbound ? 1U : 0U));
            assert(!status().authorized && atomic_load(&logins) == (unbound ? 1U : 0U));
            if (unbound) assert(xiaotai_identity_matches("fixture", "fixture-key") && status().generation > 1U);
        } else if (atomic_load(&no_provider)) {
            phase(XIAOTAI_PLATFORM_CONNECTING);
            assert(status().authorized && !status().signaling && !atomic_load(&online));
        } else {
            waits(&online, 1);
            assert(xiaotai_platform_endpoint("mqtt", url, sizeof(url)) == XIAOTAI_OK && !strcmp(url, expected_broker));
            assert(xiaotai_platform_endpoint("recognition", url, sizeof(url)) == (unchecked ?
                XIAOTAI_ERR_NOT_SUPPORTED : XIAOTAI_ERR_PLATFORM_BAD_SERVICE));
            if (!unchecked) {
                xiaotai_platform_stats_t stats;
                xiaotai_platform_stats(&stats);
                assert(stats.services_invalid == 1);
            }
        }
    }
    xiaotai_platform_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    xiaotai_framework_reset();
    xiaotai_identity_reset();
    if (!matches_old) return 1;
    printf("platform_session %s: ok\n", which);
    return 0;
}
