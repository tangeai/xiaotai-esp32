// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/platform_config.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_mem_host.h"

static const xiaotai_platform_status_t online = {.phase = XIAOTAI_PLATFORM_ONLINE, .clock_synced = true,
    .discovered = true, .authorized = true, .signaling = true, .profile_ready = true, .generation = 987,
    .notify_revision = 42, .last_error = -1, .exchanges = 2};
static const char online_line[] = "+PLATFORM: phase=6 online=1 clock=1 discovered=1 authorized=1 signaling=1 verifying=0"
    " held=0 gated=0 profile=1 gen=987 revision=42 error=-1 exchanges=2";
static const char offline_line[] = "+PLATFORM: phase=0 online=0 clock=0 discovered=0 authorized=0 signaling=0 verifying=0"
    " held=0 gated=0 profile=0 gen=1 revision=0 error=0 exchanges=0";
static const char provisioning_line[] = "+PLATFORM: phase=0 online=0 clock=0 discovered=0 authorized=0 signaling=0 verifying=0"
    " held=0 gated=0 profile=0 gen=0 revision=0 error=0 exchanges=0";
static const char *const row_keys[] = {
    "clock_attempts discoveries services_invalid logins login_failures token_renewals signaling_connects signaling_drops"
    " signaling_rebuilds subscribe_retries heartbeats",
    "requests replies request_failures requests_stale requests_refused requests_busy transfers transfers_cancelled"
    " reply_post_retries slot_clamps",
    "signals signals_delivered signals_acknowledged signal_post_retries signals_malformed signals_oversized"
    " signals_fragment_errors signals_no_slot signals_stale signals_unauthorized unbound_reports",
    "profile_reports profile_failures profile_deferrals profile_parts_dropped event_post_retries worker_stack_free",
};
static const size_t widths[] = {301, 267, 331, 192};
static const char prefix[] = "+PLATFORM-STATS:";

static size_t check_row(const char *line, unsigned row, const char *value)
{
    assert(strncmp(line, prefix, sizeof(prefix) - 1U) == 0);
    line += sizeof(prefix) - 1U;
    size_t length = sizeof(prefix) - 1U;
    const size_t digits = strlen(value);
    for (const char *key = row_keys[row]; *key;) {
        const char *end = strchr(key, ' ');
        const size_t n = end ? (size_t)(end - key) : strlen(key);
        assert(*line++ == ' ' && strncmp(line, key, n) == 0);
        line += n;
        assert(*line++ == '=' && strncmp(line, value, digits) == 0);
        line += digits;
        length += 2U + n + digits;
        key = end ? end + 1 : key + n;
    }
    assert(length <= XIAOTAI_AT_OUT_LINE_MAX);
    return length;
}

static void format(void)
{
    char line[XIAOTAI_AT_OUT_LINE_MAX + 1U];
    assert(platform_diagnostic_status(&online, line, sizeof(line)) && !strcmp(line, online_line));
    assert(!platform_diagnostic_status(&online, line, 5));
    xiaotai_platform_stats_t stats = {0};
    for (unsigned max = 0; max < 2; ++max) {
        if (max) memset(&stats, 0xff, sizeof(stats));
        for (unsigned row = 0; row < PLATFORM_DIAGNOSTIC_STATS_ROWS; ++row) {
            assert(platform_diagnostic_stats_row(&stats, row, line, sizeof(line)));
            const size_t length = check_row(line, row, max ? "4294967295" : "0");
            assert(length == strlen(line));
            if (max) assert(length == widths[row]);
            assert(!platform_diagnostic_stats_row(&stats, row, line, length));
        }
    }
    assert(!platform_diagnostic_stats_row(&stats, PLATFORM_DIAGNOSTIC_STATS_ROWS, line, sizeof(line)));
}

static bool provisioning, mutate, fail;
static const char *input;
static char wire[16000];
static size_t used, owner_writes;

bool __wrap_xiaotai_net_provisioning_boot(void) { return provisioning; }

static size_t read_io(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout)
{
    (void)ctx;
    (void)timeout;
    const size_t n = strlen(input);
    assert(n <= capacity);
    memcpy(data, input, n);
    input += n;
    return n;
}

static xiaotai_err_t write_io(void *ctx, const char *line, size_t size)
{
    (void)ctx;
    assert(size <= XIAOTAI_AT_OUT_LINE_MAX + 2U && size >= 2 && !memcmp(line + size - 2, "\r\n", 2));
    assert(!memchr(line, '\r', size - 2) && !memchr(line, '\n', size - 2));
    if (xiaotai_on_owner()) ++owner_writes;
    if (size >= sizeof(prefix) - 1U && !memcmp(line, prefix, sizeof(prefix) - 1U)) {
        if (fail) { fail = false; return XIAOTAI_FAIL; }
        if (mutate) {
            assert(xiaotai_on_owner());
            mutate = false;
            platform_lock();
            platform.stats = (xiaotai_platform_stats_t){0};
            platform_unlock();
        }
    }
    assert(used + size < sizeof(wire));
    memcpy(wire + used, line, size);
    used += size;
    wire[used] = '\0';
    return XIAOTAI_OK;
}

static xiaotai_err_t init_platform(void)
{
    const uint32_t before = xiaotai_mem_host_allocations();
    const xiaotai_err_t err = xiaotai_engine_platform_init();
    if (provisioning) assert(xiaotai_mem_host_allocations() == before);
    return err;
}

static void after_report(const xiaotai_reply_t *reply)
{
    xiaotai_at_printf(reply, "+FOLLOWING: count=1");
}

static xiaotai_err_t follower(void) { return xiaotai_diag_register("following", after_report); }

static void issue(const char *command)
{
    input = command;
    wire[0] = '\0';
    used = owner_writes = 0;
    do { assert(xiaotai_at_service(1) == XIAOTAI_OK); } while (*input);
}

static void expect_report(bool aggregate, bool max)
{
    const char *cursor = strstr(wire, "+PLATFORM: ");
    const char *state = strstr(wire, "+STATE: ");
    assert(cursor && (aggregate ? state && state < cursor : cursor == wire));
    const char *status = max ? online_line : provisioning ? provisioning_line : offline_line;
    const size_t length = strlen(status);
    assert(!strncmp(cursor, status, length) && !strncmp(cursor + length, "\r\n", 2));
    cursor += length + 2U;
    for (unsigned row = 0; row < PLATFORM_DIAGNOSTIC_STATS_ROWS; ++row) {
        cursor += check_row(cursor, row, max ? "4294967295" : "0");
        assert(!strncmp(cursor, "\r\n", 2));
        cursor += 2U;
    }
    assert(!strcmp(cursor, aggregate ? "+FOLLOWING: count=1\r\nOK\r\n" : "OK\r\n"));
    assert(!strstr(wire, "ERROR:") && !strstr(wire, "token=") && !strstr(wire, "secret="));
}

static void query(bool max, const char *command, bool aggregate)
{
    if (max) {
        platform_lock();
        platform.status = online;
        memset(&platform.stats, 0xff, sizeof(platform.stats));
        atomic_store(&platform.running, true);
        platform_unlock();
        mutate = true;
    }
    const uint32_t allocations = xiaotai_mem_host_allocations();
    issue(command);
    expect_report(aggregate, max);
    if (!aggregate) assert(xiaotai_mem_host_allocations() == allocations);
    if (max) {
        assert(!mutate && owner_writes > 0);
        atomic_store(&platform.running, false);
    }
}

static void dispatch(bool max)
{
    static const xiaotai_at_io_t source = {.name = "p6", .read = read_io, .write = write_io};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    static const xiaotai_module_t engine = {.name = "platform", .init = init_platform};
    static const xiaotai_module_t fixture = {.name = "fixture", .init = follower};
    static const xiaotai_module_t *const modules[] = {&at, &engine, &fixture};
    assert(xiaotai_kv_init() == XIAOTAI_OK && xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    const xiaotai_at_config_t at_config = {.no_platform_console = true, .task_stack_bytes = XIAOTAI_AT_NO_TASK,
        .reply_buffer_bytes = 1024, .notify_bytes = 512, .line_max = 768};
    assert(xiaotai_at_configure(&at_config) == XIAOTAI_OK && xiaotai_at_attach(&source) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 3}) == XIAOTAI_OK);
    issue("");
    assert(!strcmp(wire, "+READY: at=1 source=p6 locked=0\r\n"));
    issue("AT+HELP\r");
    assert(strstr(wire, "+HELP: AT+PLATFORM\r\n"));
    assert((strstr(wire, "+HELP: AT+PLATFORM RECONNECT ") != NULL) == (!provisioning && XIAOTAI_PLATFORM_RECONNECT_COMMAND));
    if (provisioning) {
        const uint32_t before = xiaotai_mem_host_allocations();
        xiaotai_engine_platform_start();
        assert(!platform.objects && !atomic_load(&platform.running) && xiaotai_mem_host_allocations() == before);
    }
    query(max, "AT+PLATFORM\r", false);
    query(max, "AT+STATUS platform\r", false);
    query(max, "AT+STATUS\r", true);
    if (!max) {
        const char *const commands[] = {"AT+PLATFORM\r", "AT+STATUS platform\r", "AT+STATUS\r"};
        for (size_t i = 0; i < sizeof(commands) / sizeof(*commands); ++i) {
            fail = true;
            issue(commands[i]);
            const char *const terminal = strstr(wire, "ERROR: FAIL OUTPUT\r\n");
            assert(!fail && terminal && !strcmp(terminal, "ERROR: FAIL OUTPUT\r\n"));
            assert(!strstr(wire, "\r\nOK\r\n") && !strstr(wire, prefix));
        }
    }
    xiaotai_platform_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    xiaotai_framework_reset();
}

int main(int argc, char **argv)
{
    if (argc == 1) format();
    else if (!strcmp(argv[1], "default")) dispatch(false);
    else if (!strcmp(argv[1], "provisioning")) { provisioning = true; dispatch(false); }
    else if (!strcmp(argv[1], "max")) dispatch(true);
    else return 1;
}
