#!/usr/bin/env python3
"""Check startup ordering and real clock functions with host C stubs.

Requires a host C11 compiler (CC or cc). No board, network or firmware build.
The stubs validate application contracts, not actual SNTP or SDK behavior.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = (ROOT / "components/platform_client/src/platform_client.c").read_text(encoding="utf-8")
APP = (ROOT / "main/xiaotai_main.c").read_text(encoding="utf-8")
RUNTIME = (ROOT / "components/starter_runtime/src/starter_runtime.c").read_text(encoding="utf-8")


def function(source, name):
    match = re.search(r"^(?:static )?(?:esp_err_t|void) " + name + r"\([^;{}]*\)\s*\{", source, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


startup = function(APP, "starter_start_task")
first_clock = startup.index("wait_for_network_clock();")
assert first_clock < startup.index("if (!credentials_valid)")
assert first_clock < startup.index("starter_runtime_start(")
assert startup.count("wait_for_network_clock();") == 2
retry_clock = startup.index("wait_for_network_clock();", first_clock + 1)
assert retry_clock < startup.index("heap_caps_free(s_tirtc_internal_reserve)")
assert retry_clock < startup.index("starter_tirtc_start(&tirtc)")
for name in ("platform_client_provision", "platform_client_start"):
    body = function(PLATFORM, name)
    assert body.index("platform_client_sync_clock()") < body.index("discover_services(")

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE,
       ESP_ERR_INVALID_RESPONSE, ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG };
typedef struct { unsigned num_of_servers; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n, ...) { .num_of_servers = (n) }
#define SNTP_RECV_TIMEOUT 15000U
#define PLATFORM_CLOCK_PEERS 2U
#define pdMS_TO_TICKS(ms) (ms)
#define START_RETRY_DELAY_MS 5000U
#define TAG "test"
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
static bool s_clock_initialized, s_clock_synchronized;
static time_t wall_time;
static int init_calls, wait_calls, init_result, wait_result;
static unsigned diagnostics;
static void clock_log_failure(void) { diagnostics++; }
static bool valid_on_reply, timeout_once, wifi_wait_once;
static unsigned delays_100, delays_retry;
static int64_t elapsed_us;
static time_t fake_time(time_t *out) { if (out) *out = wall_time; return wall_time; }
#define time fake_time
static int64_t esp_timer_get_time(void) { return elapsed_us; }
static const char *esp_err_to_name(int err) { (void)err; return "injected"; }
static int esp_netif_sntp_init(const esp_sntp_config_t *config) {
    assert(config->num_of_servers == 2);
    init_calls++;
    return init_result;
}
static int esp_netif_sntp_sync_wait(unsigned ticks) {
    assert(ticks == 3U * SNTP_RECV_TIMEOUT);
    wait_calls++;
    int result = timeout_once && wait_calls == 1 ? ESP_ERR_TIMEOUT : wait_result;
    elapsed_us += (result == ESP_ERR_TIMEOUT ? ticks : 50U) * 1000LL;
    if (result == ESP_OK && valid_on_reply) wall_time = 1800000000;
    return result;
}
static bool wifi_manager_connected(void) {
    if (wifi_wait_once) { wifi_wait_once = false; return false; }
    return true;
}
static void vTaskDelay(unsigned ticks) {
    if (ticks == 100) delays_100++;
    else { assert(ticks == START_RETRY_DELAY_MS); delays_retry++; }
    elapsed_us += ticks * 1000LL;
}
typedef enum { EVENT_AI_START } runtime_event_type_t;
typedef struct { runtime_event_type_t type; uint32_t generation; } runtime_event_t;
static void *s_queue;
static bool network_ready, queue_space;
static unsigned queue_calls;
static runtime_event_t queued_event;
static bool ai_network_ready(void) { return network_ready; }
static bool queue_event(const runtime_event_t *event) {
    queue_calls++;
    if (s_queue == NULL || !queue_space) return false;
    queued_event = *event;
    return true;
}
static void reset(void) {
    s_clock_initialized = s_clock_synchronized = false;
    wall_time = 0;
    init_calls = wait_calls = 0;
    diagnostics = 0;
    init_result = wait_result = ESP_OK;
    valid_on_reply = true;
    timeout_once = wifi_wait_once = false;
    delays_100 = delays_retry = 0;
    elapsed_us = 0;
}
'''
code += function(PLATFORM, "platform_client_sync_clock")
code += function(APP, "wait_for_network_clock")
for name in ("enqueue_simple", "starter_runtime_ai_start", "starter_runtime_ai_start_from_wake"):
    code += function(RUNTIME, name)
code += r'''
int main(void) {
    reset();
    assert(platform_client_sync_clock() == ESP_OK);
    assert(s_clock_synchronized && init_calls == 1 && wait_calls == 1);
    assert(diagnostics == 0);
    assert(platform_client_sync_clock() == ESP_OK);
    assert(init_calls == 1 && wait_calls == 1);

    /* A retained date is not a successful synchronization this boot. */
    reset(); wall_time = 1800000000; wait_result = ESP_ERR_TIMEOUT;
    assert(platform_client_sync_clock() == ESP_ERR_TIMEOUT);
    assert(!s_clock_synchronized && wait_calls == 1);
    assert(diagnostics == 1);
    wait_result = ESP_OK;
    assert(platform_client_sync_clock() == ESP_OK);
    assert(init_calls == 1 && wait_calls == 2);
    assert(diagnostics == 1);

    reset(); init_result = ESP_ERR_NO_MEM;
    assert(platform_client_sync_clock() == ESP_ERR_NO_MEM);
    assert(!s_clock_initialized && wait_calls == 0);
    init_result = ESP_OK;
    assert(platform_client_sync_clock() == ESP_OK);
    assert(init_calls == 2 && wait_calls == 1);

    reset(); init_result = ESP_ERR_INVALID_STATE;
    assert(platform_client_sync_clock() == ESP_OK);
    assert(s_clock_synchronized && wait_calls == 1);

    reset(); valid_on_reply = false;
    assert(platform_client_sync_clock() == ESP_ERR_INVALID_RESPONSE);
    assert(!s_clock_synchronized);
    valid_on_reply = true;
    assert(platform_client_sync_clock() == ESP_OK);
    wall_time = 0; valid_on_reply = false;
    assert(platform_client_sync_clock() == ESP_ERR_INVALID_RESPONSE);
    assert(!s_clock_synchronized && init_calls == 1 && wait_calls == 3);

    reset(); timeout_once = wifi_wait_once = true;
    wait_for_network_clock();
    assert(s_clock_synchronized && init_calls == 1 && wait_calls == 2);
    assert(delays_100 == 1 && delays_retry == 1);

    /* GOT_IP precedes clock/binding and creation of the session queue. */
    network_ready = true; queue_space = true; s_queue = NULL;
    assert(starter_runtime_ai_start() == ESP_ERR_INVALID_STATE);
    assert(starter_runtime_ai_start_from_wake(7) == ESP_ERR_INVALID_STATE);
    assert(queue_calls == 0);
    s_queue = &queued_event; queue_space = false;
    assert(starter_runtime_ai_start() == ESP_ERR_TIMEOUT);
    assert(starter_runtime_ai_start_from_wake(7) == ESP_ERR_TIMEOUT);
    queue_space = true;
    assert(starter_runtime_ai_start() == ESP_OK);
    assert(queued_event.type == EVENT_AI_START && queued_event.generation == 0);
    assert(starter_runtime_ai_start_from_wake(7) == ESP_OK);
    assert(queued_event.generation == 7);
    assert(starter_runtime_ai_start_from_wake(0) == ESP_ERR_INVALID_ARG);
    network_ready = false;
    assert(starter_runtime_ai_start() == ESP_ERR_INVALID_STATE);
    assert(starter_runtime_ai_start_from_wake(7) == ESP_ERR_INVALID_STATE);
    puts("startup clock contracts: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="p4-startup-clock-") as directory:
    source = Path(directory) / "clock.c"
    binary = Path(directory) / ("clock.exe" if os.name == "nt" else "clock")
    source.write_text(code, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
