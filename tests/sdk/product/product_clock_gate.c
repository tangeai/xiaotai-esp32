// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_clock_fallback.py
// Replaces legacy: tests/regression/test_s3_discovered_endpoint.py
#include "product_platform.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/os.h"
#include "xiaotai_clock_host.h"
#include "xiaotai_mqtt_host.h"
#include "xiaotai_transport_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

void product_clock_gate_prepare(bool bound);
void product_clock_gate(bool bound);

enum { WAIT_MS = 10000 };
static const int64_t wall_ms = INT64_C(1700000100000);
static bool delayed_guest;
static atomic_uint reports, prompts, guest_packet;
static _Atomic(xiaotai_mqtt_t *) guest;

xiaotai_err_t __real_xiaotai_mqtt_subscribe(xiaotai_mqtt_t *, const xiaotai_mqtt_topic_t *, size_t, int32_t *);
xiaotai_err_t __wrap_xiaotai_mqtt_subscribe(xiaotai_mqtt_t *, const xiaotai_mqtt_topic_t *, size_t, int32_t *);
xiaotai_err_t __wrap_xiaotai_mqtt_subscribe(xiaotai_mqtt_t *client, const xiaotai_mqtt_topic_t *topics,
                                          size_t count, int32_t *id)
{
    const bool binding = delayed_guest && count == 1 &&
                         !strcmp(topics[0].topic, "device/" PRODUCT_FIXTURE_GUEST "/cmd");
    if (binding) assert(xiaotai_mqtt_host_suback(client, false, NULL, 0) == XIAOTAI_OK);
    const xiaotai_err_t result = __real_xiaotai_mqtt_subscribe(client, topics, count, id);
    if (binding && result == XIAOTAI_OK) {
        assert(id && *id > 0 && *id <= UINT16_MAX);
        atomic_store(&guest, client);
        atomic_store(&guest_packet, (unsigned)*id);
    }
    return result;
}

static xiaotai_binding_status_t binding_status(void)
{
    xiaotai_binding_status_t state;
    xiaotai_binding_status(&state);
    return state;
}

static void response(void *context, const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    (void)context;
    if (!strcmp(req->url, "http://fixture.invalid/device/v1/device/report")) {
        static const char report[] = "{\"code\":200,\"data\":{\"code\":\"123456\","
            "\"temp_token\":\"fixture-guest-token\",\"temp_client_id\":\"" PRODUCT_FIXTURE_GUEST "\"}}";
        assert(req->method == XIAOTAI_HTTP_POST && xiaotai_clock_synced());
        assert(!binding_status().verification[0] && atomic_fetch_add(&reports, 1) == 0);
        reply->status = 200;
        reply->body = (const uint8_t *)report;
        reply->body_size = sizeof(report) - 1;
        return;
    }
    if (!strcmp(req->url, "http://fixture.invalid/device/v1/device/tts?code=123456")) {
        const xiaotai_binding_status_t state = binding_status();
        assert(xiaotai_clock_synced() && state.phase == XIAOTAI_BINDING_WAITING &&
               !strcmp(state.verification, "123456"));
        ++prompts;
    }
    reply->status = 404; /* Assert dispatch ordering, not synthesis or acoustic output. */
}

void product_clock_gate_prepare(bool bound)
{
    delayed_guest = !bound;
    product_platform_prepare(bound);
    /* A plausible retained clock must not authorize the first boot. */
    xiaotai_clock_host_hold(true);
    if (!bound) product_platform_http_responder(response, NULL);
}

static void assert_blocked(void)
{
    xiaotai_platform_stats_t platform;
    xiaotai_platform_stats(&platform);
    assert(!xiaotai_clock_synced() && !platform.discoveries && !platform.logins &&
           !xiaotai_transport_host_calls(NULL, 0));
    assert(!atomic_load(&reports) && !atomic_load(&prompts) && !binding_status().verification[0]);
}

void product_clock_gate(bool bound)
{
    xiaotai_clock_stats_t clock;
    assert(xiaotai_clock_sync_budget_ms() == 50000);
    assert(product_platform_network_token());
    assert_blocked();
    xiaotai_clock_stats(&clock);
    assert(!clock.starts);
    product_platform_link_ready();
    for (unsigned i = 0; i < WAIT_MS; i += 5) {
        xiaotai_clock_stats(&clock);
        if (clock.timeouts) break;
        xiaotai_delay_ms(5);
    }
    assert(clock.starts == 1 && clock.timeouts && !clock.syncs && clock.running);
    assert_blocked();
    xiaotai_clock_host_answer(wall_ms, 0);
    if (bound) {
        product_platform_online();
        xiaotai_platform_stats_t platform;
        xiaotai_platform_stats(&platform);
        assert(platform.clock_attempts == 1 && platform.discoveries == 1 && platform.logins == 1);
    } else {
        for (unsigned i = 0; i < WAIT_MS && !atomic_load(&guest_packet); i += 5) xiaotai_delay_ms(5);
        assert(atomic_load(&reports) == 1 && atomic_load(&guest_packet));
        assert(binding_status().phase == XIAOTAI_BINDING_CONNECTING &&
               !binding_status().verification[0] && !atomic_load(&prompts));
        assert(xiaotai_mqtt_host_suback_reply(atomic_load(&guest), (uint16_t)atomic_load(&guest_packet)) == XIAOTAI_OK);
        for (unsigned i = 0; i < WAIT_MS && !atomic_load(&prompts); i += 5) xiaotai_delay_ms(5);
        assert(atomic_load(&prompts) && binding_status().phase == XIAOTAI_BINDING_WAITING &&
               !strcmp(binding_status().verification, "123456"));
    }
    xiaotai_clock_stats(&clock);
    assert(clock.starts == 1 && clock.syncs == 1 && !xiaotai_clock_host_probes());
    puts(bound ? "Clock gate: Wi-Fi then retained-time gate, late answer then service/RTC/login"
               : "Clock gate: late answer then report, SUBACK then code and TTS dispatch");
}
