// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_voice_diag_command
// Replaces legacy: tests/regression/test_voice_reliability.py::test_wake_diagnostics
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "at_test_help.h"
#include "fakes.h"
#include "internal.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_mem_host.h"

int64_t g_fake_now_us;
static int64_t now_us(void) { return g_fake_now_us; }
static const char *input;
static char output[32768];
static size_t used;
static bool measuring_watch;
static unsigned watch_samples, watch_delays, stop_at_sample;

void __real_xiaotai_delay_ms(uint32_t ms);
void __wrap_xiaotai_delay_ms(uint32_t ms)
{
    if (!measuring_watch || ms != 1000) {
        __real_xiaotai_delay_ms(ms);
        return;
    }
    assert(!xiaotai_on_owner() && watch_samples == watch_delays + 1U);
    ++watch_delays;
    g_fake_now_us += 1000000;
}

static size_t console_read(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout)
{
    (void)ctx;
    (void)timeout;
    const size_t n = strlen(input);
    assert(n <= capacity);
    memcpy(data, input, n);
    input += n;
    return n;
}

static xiaotai_err_t console_write(void *ctx, const char *line, size_t size)
{
    (void)ctx;
    assert(size <= XIAOTAI_AT_OUT_LINE_MAX + 2U && size >= 2 && !memcmp(line + size - 2, "\r\n", 2));
    assert(!memchr(line, '\r', size - 2) && !memchr(line, '\n', size - 2));
    if (measuring_watch && size > 7 && !memcmp(line, "+WAKE: ", 7)) {
        assert(!xiaotai_on_owner() && watch_samples == watch_delays);
        if (++watch_samples == stop_at_sample) return XIAOTAI_FAIL;
    }
    assert(used + size < sizeof(output));
    memcpy(output + used, line, size);
    used += size;
    output[used] = '\0';
    return XIAOTAI_OK;
}

static void issue(const char *command)
{
    input = command;
    used = 0;
    output[0] = '\0';
    do { assert(xiaotai_at_service(1) == XIAOTAI_OK); } while (*input);
}

static void expect_records(bool aggregate, bool ready, int error, unsigned resets, bool raw)
{
    char status[160];
    snprintf(status, sizeof(status), "+WAKE_ENGINE: ready=%d listening=0 init_error=%d threshold=950 "
             "frontend=%s fed=0 dropped=0 resets=%u ", (int)ready, error,
             raw ? "aec-clean" : "aec-clean-agc", resets);
    const char *record = strstr(output, status);
    if (!record) fprintf(stderr, "Expected %s, got %s\n", status, output);
    assert(record && (aggregate ? record > output : record == output));
    const char *score = strstr(record, "\r\n+WAKE_ENGINE-SCORE: last=0 at_ms=0 age_ms=0 avg_us=0 max_us=0 over_hop=0 ");
    assert(score && !strstr(score, "+WAKE_ENGINE: "));
    assert(!strstr(output, "+WAKE: ") && !strstr(output, "ERROR:"));
    assert(used >= 4 && !strcmp(output + used - 4, "OK\r\n"));
}

static unsigned instances(const char *fragment)
{
    unsigned count = 0;
    for (const char *next = output; (next = strstr(next, fragment)); next += strlen(fragment)) ++count;
    return count;
}

static void watch(const char *command, bool success, unsigned samples, unsigned delays)
{
    const int64_t allocated = xiaotai_mem_host_outstanding();
    const int64_t before = g_fake_now_us;
    watch_samples = watch_delays = 0;
    measuring_watch = true;
    issue(command);
    measuring_watch = false;
    assert(watch_samples == samples && watch_delays == delays);
    assert(g_fake_now_us - before == (int64_t)delays * 1000000);
    if (success) {
        assert(instances("+BUILD: elf_sha256=") == 1 && !strncmp(output, "+BUILD: elf_sha256=", 19));
        assert(instances("+WAKE-SCORE: ") == samples && instances("+WAKE_ENGINE: ") == 0);
        assert(used >= 4 && !strcmp(output + used - 4, "OK\r\n"));
    } else {
        assert(strstr(output, "ERROR: ") && !strstr(output, "\r\nOK\r\n"));
        if (!samples) assert(!strstr(output, "+BUILD: ") && !strstr(output, "+WAKE"));
    }
    assert(xiaotai_mem_host_outstanding() == allocated);
}

/* Replaces legacy: tests/regression/test_voice_reliability.py::test_voice_diag_command */
static void expect_wake_commands(void)
{
    issue("AT+WAKE\r");
    assert(strstr(output, "+WAKE: ready=1 listening=0 init_error=0 threshold=950 ")
           && instances("+WAKE-SCORE: ") == 1 && !strstr(output, "+WAKE_ENGINE: "));
    assert(used >= 4 && !strcmp(output + used - 4, "OK\r\n"));

    watch("AT+WAKE WATCH\r", true, 21, 20);
    watch("AT+WAKE WATCH 5\r", true, 6, 5);
    watch("AT+WAKE WATCH 30\r", true, 31, 30);
    static const char *const malformed[] = {"4", "31", "-1", "5x", "\"\"", "99999999999999999999999", "5 extra"};
    for (size_t i = 0; i < sizeof(malformed) / sizeof(*malformed); ++i) {
        char command[64];
        snprintf(command, sizeof(command), "AT+WAKE WATCH %s\r", malformed[i]);
        watch(command, false, 0, 0);
    }
    xiaotai_mem_host_fail_nth(1);
    watch("AT+WAKE WATCH 5\r", false, 0, 0);
    assert(!xiaotai_mem_host_fail_pending() && strstr(output, "ERROR: NO_MEM"));
    stop_at_sample = 2;
    watch("AT+WAKE WATCH 5\r", false, 2, 1);
    stop_at_sample = 0;

    xiaotai_wake_status_t status;
    xiaotai_wake_status(&status);
    assert(status.threshold_milli == 950 && status.resets == 1 && !status.wanted);
    assert(g_script.opens == 2 && g_script.call_count == 1);
    issue("AT+WAKE THRESHOLD 49\r");
    assert(strstr(output, "ERROR: INVALID_ARG"));
    issue("AT+WAKE THRESHOLD 991\r");
    assert(strstr(output, "ERROR: INVALID_ARG"));
    issue("AT+WAKE THRESHOLD 50\r");
    assert(!strcmp(output, "OK\r\n"));
    xiaotai_wake_status(&status);
    assert(status.threshold_milli == 50 && status.resets == 2 && !status.wanted);
    issue("AT+WAKE THRESHOLD 990\r");
    assert(!strcmp(output, "OK\r\n"));
    xiaotai_wake_status(&status);
    assert(status.threshold_milli == 990 && status.resets == 3 && !status.wanted);
    int32_t value;
    assert(xiaotai_setting_get_int("wake_engine.threshold_milli", &value) == XIAOTAI_ERR_NOT_FOUND);
}

static void exercise(bool selected, bool raw, bool commands, bool help)
{
    static const xiaotai_at_io_t wire = {.name = "wake_diag", .read = console_read, .write = console_write};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    /* Drive deterministic model initialization on a virtual clock. */
    static const xiaotai_module_t wake = {.name = "wake_engine", .init = xiaotai_engine_wake_engine_init};
    static const xiaotai_module_t *const modules[] = {&at, &wake};
    assert(xiaotai_kv_init() == XIAOTAI_OK && xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    const xiaotai_at_config_t at_config = {.no_platform_console = true, .task_stack_bytes = XIAOTAI_AT_NO_TASK,
        .reply_buffer_bytes = 4096, .notify_bytes = 512, .line_max = 512};
    assert(xiaotai_at_configure(&at_config) == XIAOTAI_OK && xiaotai_at_attach(&wire) == XIAOTAI_OK);
    static const xiaotai_media_param_t tuning[] = {{WAKE_TUNE_THRESHOLD, 950}, {WAKE_TUNE_RAW, 1}};
    assert(xiaotai_media_tune(tuning, raw ? 2U : 1U) == XIAOTAI_OK);
    scripted_model_reset();
    g_script.open_results[0] = XIAOTAI_ERR_NO_MEM;
    g_script.open_result_count = 1;
    g_fake_now_us = 1000000;
    wake_engine_set_clock(now_us);
    if (selected) assert(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules,
        .module_count = selected ? 2U : 1U}) == XIAOTAI_OK);
    issue("");
    assert(!strcmp(output, "+READY: at=1 source=wake_diag locked=0\r\n"));

    issue("AT+STATUS wake_engine\r");
    if (selected) {
        expect_records(false, false, XIAOTAI_OK, 0, raw);
        if (commands) {
            issue("AT+WAKE THRESHOLD 700\r");
            assert(strstr(output, "ERROR: INVALID_STATE"));
        }
        assert(wake_engine_step() == WAKE_RETRY_FIRST_MS);
        assert(g_script.opens == 1 && !g_script.is_open);
        issue("AT+STATUS wake_engine\r");
        expect_records(false, false, XIAOTAI_ERR_NO_MEM, 0, raw);
        g_fake_now_us += (int64_t)WAKE_RETRY_FIRST_MS * 1000;
        assert(wake_engine_step() == WAKE_POLL_MS);
        assert(g_script.opens == 2 && g_script.is_open);
        const uint32_t allocations = xiaotai_mem_host_allocations();
        issue("AT+STATUS wake_engine\r");
        expect_records(false, true, XIAOTAI_OK, 1, raw);
        assert(xiaotai_mem_host_allocations() == allocations);
        issue("AT+STATUS\r");
        expect_records(true, true, XIAOTAI_OK, 1, raw);
        assert(g_script.opens == 2 && g_script.call_count == 1);
        if (commands) expect_wake_commands();
        if (help) {
            issue("AT+HELP\r");
            assert(at_test_help_write(stdout, output));
        }
    } else {
        assert(!strcmp(output, "ERROR: NOT_FOUND SECTION\r\n"));
        issue("AT+STATUS\r");
        assert(!strstr(output, "+WAKE_ENGINE") && strstr(output, "+FRAMEWORK:") && strstr(output, "\r\nOK\r\n"));
        issue("AT+HELP\r");
        assert(!strstr(output, "+HELP: AT+WAKE"));
    }
    xiaotai_wake_reset();
    xiaotai_media_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    xiaotai_framework_reset();
}

int main(int argc, char **argv)
{
    if (argc == 1) exercise(true, false, false, false);
    else if (argc == 2 && !strcmp(argv[1], "absent")) exercise(false, false, false, false);
    else if (argc == 2 && !strcmp(argv[1], "raw")) exercise(true, true, false, false);
    else if (argc == 2 && !strcmp(argv[1], "commands")) exercise(true, false, true, false);
    else if (argc == 2 && !strcmp(argv[1], "help")) exercise(true, false, false, true);
    else return 1;
}
