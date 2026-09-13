#!/usr/bin/env python3
"""Execute the production clock gate with deterministic SNTP timing."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / "components/platform_client/src/platform_client.c").read_text(encoding="utf-8")
assert 'ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org")' in source
provision = source[source.index("esp_err_t platform_client_provision("):
                   source.index("esp_err_t platform_client_start(")]
assert "snprintf(s_verification_code" not in provision
wait = source[source.index("static esp_err_t wait_for_auth_grant("):
              source.index("esp_err_t platform_client_provision(")]
assert wait.index("if ((bits & PROVISION_READY_BIT)") < wait.index("snprintf(s_verification_code")
assert wait.index("snprintf(s_verification_code") < wait.index("play_verification_prompt(report")
function = source[source.index("static esp_err_t sync_clock(void)"):
                  source.index("typedef struct {\n    char code[17];")]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
typedef int esp_err_t;
typedef struct { unsigned num_of_servers; } esp_sntp_config_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_TIMEOUT -3
#define ESP_ERR_INVALID_RESPONSE -4
#define SNTP_RECV_TIMEOUT 15000U
#define SNTP_STARTUP_DELAY 1
#define CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY 5000U
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_SNTP_SERVER_LIST(...) __VA_ARGS__
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n, ...) ((esp_sntp_config_t){n})
static const char *TAG = "test";
static void log_stub(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI log_stub
#define ESP_LOGE log_stub
static unsigned now_ms, answer_ms, wait_calls, init_calls, budget;
static int init_result, wait_result;
static bool valid;
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
static time_t fake_time(time_t *unused) { (void)unused; return valid ? 1800000000 : 0; }
#define time fake_time
static int64_t esp_timer_get_time(void) { return (int64_t)now_ms * 1000; }
static esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) {
    assert(config->num_of_servers == 2); ++init_calls; return init_result;
}
static esp_err_t esp_netif_sntp_sync_wait(unsigned ticks) {
    ++wait_calls; budget = ticks;
    assert(ticks == 2 * SNTP_RECV_TIMEOUT + SNTP_RECV_TIMEOUT + 5000);
    if (wait_result != ESP_ERR_TIMEOUT) {
        now_ms = answer_ms; valid = answer_ms != 0; return wait_result;
    }
    if (answer_ms && answer_ms <= ticks) { now_ms = answer_ms; valid = true; return ESP_OK; }
    now_ms = ticks; return ESP_ERR_TIMEOUT;
}
''' + function + r'''
static void reset(unsigned answer) {
    now_ms = wait_calls = init_calls = budget = 0;
    answer_ms = answer; init_result = ESP_OK; wait_result = ESP_ERR_TIMEOUT; valid = false;
}
int main(void) {
    const unsigned successful[] = {60, 15060, 27000, 45000, 49999, 50000};
    for (unsigned i = 0; i < sizeof(successful) / sizeof(successful[0]); ++i) {
        reset(successful[i]); assert(sync_clock() == ESP_OK);
        assert(now_ms == successful[i] && wait_calls == 1 && init_calls == 1);
    }
    reset(50001); assert(sync_clock() == ESP_ERR_TIMEOUT && now_ms == 50000);
    reset(0); assert(sync_clock() == ESP_ERR_TIMEOUT && wait_calls == 1);
    reset(0); valid = true; assert(sync_clock() == ESP_OK && wait_calls == 0 && init_calls == 1);
    reset(15060); init_result = ESP_ERR_INVALID_STATE; assert(sync_clock() == ESP_OK);
    reset(0); init_result = ESP_FAIL; assert(sync_clock() == ESP_FAIL && wait_calls == 0);
    reset(0); wait_result = ESP_OK; assert(sync_clock() == ESP_ERR_INVALID_RESPONSE);
    reset(0); wait_result = ESP_FAIL; assert(sync_clock() == ESP_FAIL);
    puts("PASS: real clock gate, delayed fallback, bounded failure, warm clock, invalid reply");
}
'''
with tempfile.TemporaryDirectory(prefix="xiaotai-clock-") as folder:
    path = Path(folder)
    (path / "test.c").write_text(harness, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
