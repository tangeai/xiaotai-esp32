#!/usr/bin/env python3
"""Exercise production report functions with HTTP/JSON stubs; no device or network.

Run with a host GCC supporting ASan/UBSan, as for test_runtime_resources.py.
JSON payload/schema checks live in test_voip_profile.py. This test checks the
state transitions, not TLS, actual server acceptance or MQTT delivery.
"""
from pathlib import Path

from test_runtime_resources import function, run_case

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_runtime/src/starter_runtime.c").read_text(encoding="utf-8")

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
typedef int esp_err_t;
typedef struct { uint32_t command; char *text; unsigned length; } runtime_event_t;
typedef struct { int valueint; } cJSON;
#define CONFIG_IDF_TARGET_ESP32P4 1
#define ESP_OK 0
#define PLATFORM_SERVICE_DEVICE 0
#define EVENT_DEVICE_PROFILE 8
#define VOIP_PROFILE_RETRY_MS 15000
#define DEVICE_PROFILE_MAX_ATTEMPTS 3U
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static bool s_voip_profile_ready, s_voip_profile_inflight;
static int64_t s_voip_profile_retry_at_ms;
static unsigned s_device_profile_attempts;
static atomic_uint s_voip_profile_delivery_failed;
static bool ready = true, mqtt = true, queued = true;
static int enqueue_error, status = 200, submits, deliveries, reconciles;
static uint32_t epoch, response_epoch;
static uint32_t delivered_status;
static int64_t clock_ms = 1000;
static cJSON business_code = {200};
static int64_t now_ms(void) { return clock_ms; }
static bool platform_client_ready(void) { return ready; }
static bool platform_client_mqtt_connected(void) { return mqtt; }
static uint32_t platform_client_epoch(void) { return epoch; }
static uint32_t platform_client_response_epoch(void) { return response_epoch; }
static int platform_client_response_status(void) { return status; }
static bool queue_http_result(int type, uint32_t tag, uint32_t stage, const char *body) {
    assert(type == EVENT_DEVICE_PROFILE && tag == 0);
    ++deliveries; delivered_status = stage; return queued;
}
static cJSON *cJSON_ParseWithLength(const char *text, unsigned length) { return &business_code; }
static const cJSON *cJSON_GetObjectItem(const cJSON *value, const char *key) {
    assert(!strcmp(key, "code")); return value;
}
static bool cJSON_IsNumber(const cJSON *value) { return value != NULL; }
static void cJSON_Delete(cJSON *value) {}
static void reconcile_platform_binding(bool unbound) { assert(unbound); ++reconciles; }
static void device_profile_response(const char *body, void *user_data);
static int platform_client_request_timeout(int service, const char *path, const char *body,
    unsigned timeout, void (*callback)(const char *, void *), void *user_data) {
    assert(service == PLATFORM_SERVICE_DEVICE && !strcmp(path, "/v1/device/profile"));
    assert(strncmp(body, "{\"profiles\":{", 13) == 0 && strlen(body) < 2048);
    assert(timeout == 10000 && callback == device_profile_response && user_data == NULL);
    ++submits; return enqueue_error;
}
'''

for name in ("device_profile_response", "recover_voip_profile_delivery_failure",
             "request_device_profile", "handle_device_profile"):
    code += function(source, name) + "\n"

code += r'''
static void reset(void) {
    s_voip_profile_ready = false; s_voip_profile_inflight = false;
    s_voip_profile_retry_at_ms = 0; s_device_profile_attempts = 0;
    atomic_store(&s_voip_profile_delivery_failed, 0);
    ready = mqtt = queued = true; enqueue_error = 0; epoch = response_epoch = 0;
    submits = deliveries = reconciles = 0; clock_ms = 1000;
}
static void reply(unsigned http, int code, bool body) {
    business_code.valueint = code;
    runtime_event_t event = {.command = http, .text = body ? "stub" : NULL, .length = 4};
    handle_device_profile(&event);
}
int main(void) {
    reset(); ready = false; request_device_profile(); assert(submits == 0);
    ready = true; mqtt = false; request_device_profile(); assert(submits == 0);
    mqtt = true; request_device_profile(); request_device_profile();
    assert(submits == 1 && s_voip_profile_inflight);
    reply(200, 200, true); assert(s_voip_profile_ready && !s_voip_profile_inflight);
    request_device_profile(); assert(submits == 1);

    reset();
    for (unsigned i = 1; i <= 3; ++i) {
        request_device_profile(); assert(submits == (int)i);
        reply(0, -1, false); assert(!s_voip_profile_ready);
        request_device_profile(); assert(submits == (int)i);
        clock_ms += 15000;
    }
    request_device_profile(); assert(submits == 3 && !s_voip_profile_retry_at_ms);

    const int codes[] = {0, 40000, 401, 6006, 200};
    const unsigned statuses[] = {200, 400, 401, 410, 500};
    for (unsigned i = 0; i < 5; ++i) {
        reset(); request_device_profile(); reply(statuses[i], codes[i], true);
        assert(!s_voip_profile_ready && !s_voip_profile_inflight);
        if (statuses[i] != 500) {
            clock_ms += 60000; request_device_profile(); assert(submits == 1);
        } else assert(s_voip_profile_retry_at_ms == clock_ms + 15000);
        assert(reconciles == (statuses[i] == 410));
    }

    reset(); enqueue_error = 1;
    for (unsigned i = 0; i < 4; ++i) { request_device_profile(); clock_ms += 15000; }
    assert(submits == 3 && !s_voip_profile_inflight);

    reset(); request_device_profile(); queued = false; status = 200;
    device_profile_response("stub", NULL);
    assert(recover_voip_profile_delivery_failure(clock_ms));
    assert(!s_voip_profile_inflight && s_voip_profile_retry_at_ms == clock_ms + 15000);
    assert(!recover_voip_profile_delivery_failure(clock_ms));
    queued = true; status = 410; device_profile_response("stub", NULL);
    assert(delivered_status == 410);
    ++epoch; device_profile_response("old binding", NULL); assert(deliveries == 2);
    atomic_store(&s_voip_profile_delivery_failed, response_epoch + 1);
    s_voip_profile_ready = true;
    assert(!recover_voip_profile_delivery_failure(clock_ms) && s_voip_profile_ready);
    puts("device profile: bounded async report, strict success, failure and stale-epoch paths OK");
}
'''

run_case("device profile lifecycle", code)
