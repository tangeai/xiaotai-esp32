// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/heapcap_status_fixture.c
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "at_test_help.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/hal.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"
#include "xiaotai_console_host.h"

static char s_output[16384];
static bool s_probe, s_mem_failure;
static unsigned s_rtc_calls;
static const xiaotai_mem_rtc_stats_t *s_rtc_stats;
static atomic_uint s_direct_calls, s_orderly_calls, s_intent_calls;
static _Atomic int64_t s_restart_at;
static atomic_bool s_activity_ready;
static uint32_t s_activity_id, s_quiesce_ticket;

void xiaotai_hal_identity(xiaotai_board_identity_t *identity)
{
    *identity = (xiaotai_board_identity_t){.model = "test-board", .revision = "rev3"};
}

void xiaotai_hal_report(xiaotai_hal_write_fn_t write, void *ctx) { write(ctx, "model=test-board"); }

void __wrap_xiaotai_firmware(xiaotai_firmware_t *firmware)
{
    *firmware = (xiaotai_firmware_t){.version = "1.0.0", .platform = "ESP IDF", .chip = "fixture", .cores = 2,
        .elf_sha256 = "aabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccdd"};
}

void __wrap_xiaotai_port_report(xiaotai_port_write_fn_t write, void *ctx) { write(ctx, "cache=0"); }

const char *const *__wrap_xiaotai_port_probes(void)
{
    static const char *const probes[] = {"fixture", NULL};
    return s_probe ? probes : probes + 1;
}

xiaotai_err_t __wrap_xiaotai_port_probe(const char *name, xiaotai_port_write_fn_t write, void *ctx)
{
    assert(!strcmp(name, "fixture"));
    write(ctx, "value=7");
    return XIAOTAI_OK;
}

void __wrap_xiaotai_mem_failure(xiaotai_mem_failure_t *out)
{
    *out = (xiaotai_mem_failure_t){.installed = s_mem_failure, .has_first = s_mem_failure,
        .failures = 4, .first_at_us = 650, .requested_size = 1024, .platform_caps = 8, .cache_enabled = true,
        .function = "call", .task = "test"};
}

xiaotai_err_t __real_xiaotai_mem_rtc_stats(xiaotai_mem_rtc_stats_t *stats);
xiaotai_err_t __wrap_xiaotai_mem_rtc_stats(xiaotai_mem_rtc_stats_t *stats)
{
    ++s_rtc_calls;
    if (!s_rtc_stats) return __real_xiaotai_mem_rtc_stats(stats);
    *stats = *s_rtc_stats;
    return XIAOTAI_OK;
}

void __wrap_xiaotai_restart(void)
{
    atomic_store(&s_restart_at, xiaotai_time_us());
    atomic_fetch_add(&s_direct_calls, 1);
    xiaotai_task_exit();
}

static void orderly(void) { atomic_fetch_add(&s_orderly_calls, 1); }

static void on_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    assert(xiaotai_intent_from_external() && args->value == -3 && !strcmp(args->text, "hi"));
    atomic_fetch_add(&s_intent_calls, 1);
}

static void forbidden(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    assert(!"local-only intent ran from AT");
}

static xiaotai_err_t module_init(void)
{
    assert(xiaotai_engine_at_init() == XIAOTAI_OK);
    static const xiaotai_setting_t level = {.name = "fixture.level", .key = "test_level",
        .kind = XIAOTAI_SETTING_INT, .min = -1, .max = 10, .default_value = 4};
    static const xiaotai_setting_t word = {.name = "fixture.word", .key = "test_word",
        .kind = XIAOTAI_SETTING_TEXT, .default_text = "before"};
    assert(xiaotai_setting_register(&level) == XIAOTAI_OK);
    assert(xiaotai_setting_register_text(&word, 10) == XIAOTAI_OK);
    assert(xiaotai_intent_handle("fixture.open", on_intent, NULL) == XIAOTAI_OK);
    assert(xiaotai_intent_handle("fixture.other", forbidden, NULL) == XIAOTAI_OK);
    assert(xiaotai_intent_handle_ex("binding.forget", forbidden, NULL, XIAOTAI_INTENT_LOCAL) == XIAOTAI_OK);
    return XIAOTAI_OK;
}

static xiaotai_err_t fail_init(void) { return XIAOTAI_FAIL; }

static const char *command(const char *line)
{
    if (line) assert(xiaotai_console_host_feed(line, strlen(line)) == strlen(line));
    assert(xiaotai_at_service(20) == XIAOTAI_OK);
    const size_t size = xiaotai_console_host_take(s_output, sizeof(s_output) - 1);
    s_output[size] = '\0';
    return s_output;
}

static void expect(const char *line, const char *expected)
{
    const char *reply = command(line);
    if (strcmp(reply, expected)) fprintf(stderr, "%s -> [%s], expected [%s]\n", line, reply, expected);
    assert(!strcmp(reply, expected));
}

static void contains(const char *reply, const char *pattern)
{
    if (!strstr(reply, pattern)) fprintf(stderr, "expected [%s] in [%s]\n", pattern, reply);
    assert(strstr(reply, pattern));
}

static void wait_count(const atomic_uint *counter, unsigned expected)
{
    for (unsigned i = 0; i < 100 && atomic_load(counter) < expected; ++i) xiaotai_delay_ms(5);
    assert(atomic_load(counter) == expected);
}

static void direct_restart(const char *line, uint32_t delay_ms)
{
    const unsigned previous = atomic_load(&s_direct_calls);
    const int64_t since = xiaotai_time_us();
    expect(line, "OK\r\n");
    wait_count(&s_direct_calls, previous + 1);
    assert(atomic_load(&s_restart_at) - since >= (int64_t)delay_ms * 1000);
}

static void guard_job(void *ctx)
{
    const char action = *(const char *)ctx;
    if (action == 'a') {
        static const xiaotai_activity_t activity = {.name = "fixture", .resources = XIAOTAI_RESOURCE_LINK};
        assert(xiaotai_activity_begin(&activity, &s_activity_id) == XIAOTAI_OK);
    } else if (action == 'q') {
        xiaotai_activity_end(s_activity_id);
        assert(xiaotai_quiesce_begin("fixture", &s_quiesce_ticket) == XIAOTAI_OK);
    } else {
        xiaotai_quiesce_end(s_quiesce_ticket);
        assert(!xiaotai_quiesced());
    }
    atomic_store(&s_activity_ready, true);
}

static void guard(char action)
{
    atomic_store(&s_activity_ready, false);
    assert(xiaotai_run(guard_job, &action) == XIAOTAI_OK);
    for (unsigned i = 0; i < 100 && !atomic_load(&s_activity_ready); ++i) xiaotai_delay_ms(5);
    assert(atomic_load(&s_activity_ready));
}

int main(int argc, char **argv)
{
    const bool show_help = argc > 1 && !strcmp(argv[1], "--help-records");
    const bool legacy = argc > 1 && !strcmp(argv[1], "--legacy-policy");
    xiaotai_console_host_pipe();
    static const char *const settings[] = {"fixture.level", NULL};
    static const char *const intents[] = {"fixture.open", "binding.forget", NULL};
    const xiaotai_at_config_t at = {.task_stack_bytes = XIAOTAI_AT_NO_TASK, .reply_buffer_bytes = 4096,
        .restart_direct_when_running = legacy, .restart_idle_only = legacy, .restart_delay_ms = legacy ? 150 : 0,
        .setting_write_allow = legacy ? settings : NULL, .intent_allow = legacy ? intents : NULL};
    assert(xiaotai_at_configure(&at) == XIAOTAI_OK);
    s_probe = show_help && argc == 2;
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_at_open() == XIAOTAI_OK);
    assert(!strcmp(command(NULL), "+READY: at=1 source=platform locked=0\r\n"));
    expect("AT+VERSION\r\n", "+VERSION: board=test-board revision=rev3 version=1.0.0 platform=ESP_IDF "
           "chip=fixture cores=2 at=1\r\n+BUILD: elf_sha256=aabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccdd\r\nOK\r\n");
    expect("AT+STATE\n", "ERROR: INVALID_STATE STARTING\r\n");
    const xiaotai_module_t failed = {.name = "bad", .init = fail_init};
    const xiaotai_module_t *const bad[] = {&failed};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = bad, .module_count = 1}) == XIAOTAI_FAIL);
    expect("AT\r", "OK\r\n");
    if (XIAOTAI_AT_RESTART_COMMAND && !show_help && !legacy) direct_restart("AT+RESTART\n", 200);

    const xiaotai_module_t owner = {.name = "at", .init = module_init, .start = xiaotai_engine_at_start};
    const xiaotai_module_t *const modules[] = {&owner};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 1,
        .restart_delay_ms = 2, .restart = orderly}) == XIAOTAI_OK);
    if (legacy) {
        expect("AT+INTENT fixture.open -3 6869\n", "OK\r\n");
        wait_count(&s_intent_calls, 1);
        expect("AT+INTENT fixture.other\n", "ERROR: NOT_ALLOWED\r\n");
        expect("AT+INTENT binding.forget\n", "ERROR: NOT_ALLOWED\r\n");
        expect("AT+SET fixture.level 7\n", "OK\r\n");
        expect("AT+SET fixture.word 6869\n", "ERROR: NOT_ALLOWED\r\n");
        expect("AT+SET fixture.word\n", "+SET: name=fixture.word kind=text value_hex=6265666F7265 min=0 max=0\r\nOK\r\n");
        guard('a');
        expect("AT+RESTART\n", "ERROR: INVALID_STATE\r\n");
        guard('q');
        expect("AT+RESTART\n", "ERROR: INVALID_STATE\r\n");
        expect("AT+RESTART FORCE\n", "ERROR: INVALID_STATE\r\n");
        guard('e');
        xiaotai_console_host_write_budget(0);
        assert(!*command("AT+RESTART\n"));
        xiaotai_console_host_write_budget(SIZE_MAX);
        xiaotai_delay_ms(160);
        assert(atomic_load(&s_direct_calls) == 0);
        direct_restart("AT+RESTART\n", 150);
        assert(atomic_load(&s_orderly_calls) == 0);
        goto done;
    }
    if (show_help) {
        assert(at_test_help_write(stdout, command("AT+HELP\n")));
        if (s_probe) {
            expect("AT+PORT PROBE\n", "+PORT-PROBE: name=fixture\r\nOK\r\n");
            expect("AT+PORT PROBE fixture\n", "+PORT-PROBE: value=7\r\nOK\r\n");
            expect("AT+PORT PROBE missing\n", "ERROR: NOT_FOUND\r\n");
        }
        goto done;
    }
    expect("AT+PORT PROBE\n", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n");
    contains(command("AT+CONSOLE\n"), "+CONSOLE: lines=");
    contains(s_output, "+CONSOLE-SOURCE: name=platform device=stdio baud=0 rx_bytes=");
    contains(command("AT+MEM\n"), "+MEM: kind=internal total=");
    contains(s_output, "+MEM: kind=bulk total=");
    contains(s_output, "+MEM: kind=dma total=");
    contains(s_output, "first_us=- size=- caps=- isr=- cache=- function_hex=- task_hex=-\r\n+MEM-INFO:");
    char ordinary_mem[1024];
    assert(strlen(s_output) < sizeof(ordinary_mem) && !s_rtc_calls && !strstr(s_output, "+MEM-RTC:"));
    strcpy(ordinary_mem, s_output);
    expect("AT+MEM RTC\n", "ERROR: NOT_SUPPORTED\r\n");
    expect("AT+MEM RTC EXTRA\n", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
    assert(s_rtc_calls == 1);
    s_rtc_stats = &(const xiaotai_mem_rtc_stats_t){3100, 3070, 3030};
    expect("AT+MEM RTC\n", "+MEM-RTC: free=3100 largest=3070 min_free=3030\r\nOK\r\n");
    s_rtc_stats = &(const xiaotai_mem_rtc_stats_t){0};
    expect("AT+MEM RTC\n", "+MEM-RTC: free=0 largest=0 min_free=0\r\nOK\r\n");
    s_rtc_stats = &(const xiaotai_mem_rtc_stats_t){UINT32_MAX, UINT32_MAX, UINT32_MAX};
    expect("AT+MEM RTC\n", "+MEM-RTC: free=4294967295 largest=4294967295 min_free=4294967295\r\nOK\r\n");
    expect("AT+MEM\n", ordinary_mem);
    assert(s_rtc_calls == 4);
    s_mem_failure = true;
    contains(command("AT+STATUS mem\n"), "+MEM-FAIL: installed=1 error=0 failures=4 first_us=650 size=1024 caps=8"
             " isr=0 cache=1 function_hex=63616C6C task_hex=74657374\r\n");
    assert(s_rtc_calls == 4 && !strstr(s_output, "+MEM-RTC:"));
    expect("AT+STATUS board\n", "+BOARD: model=test-board\r\nOK\r\n");
    expect("AT+STATUS port\n", "+PORT: cache=0\r\nOK\r\n");
    contains(command("AT+STATUS framework\n"), "+FRAMEWORK: owner_stack=");
    static const char ready[] = "+STATE: started=1 app=- activity=none id=0 uptime_ms=";
    assert(!strncmp(command("AT+STATE\n"), ready, sizeof(ready) - 1));
    expect("AT+STATUS missing\n", "ERROR: NOT_FOUND SECTION\r\n");
    expect("AT+SET fixture.level\n", "+SET: name=fixture.level kind=int value=4 min=-1 max=10\r\nOK\r\n");
    expect("AT+SET missing\n", "ERROR: NOT_FOUND SETTING\r\n");
    if (XIAOTAI_AT_SETTING_WRITE) {
        expect("AT+SET fixture.level -1\n", "OK\r\n");
        expect("AT+SET fixture.level -2\n", "ERROR: INVALID_ARG VALUE\r\n");
        expect("AT+SET fixture.level\n", "+SET: name=fixture.level kind=int value=-1 min=-1 max=10\r\nOK\r\n");
        expect("AT+SET fixture.word 410042\n", "ERROR: INVALID_ARG VALUE\r\n");
        expect("AT+SET fixture.word 6869\n", "OK\r\n");
        expect("AT+SET fixture.word\n", "+SET: name=fixture.word kind=text value_hex=6869 min=0 max=0\r\nOK\r\n");
    } else {
        expect("AT+SET fixture.level -1\n", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
        expect("AT+SET fixture.level\n", "+SET: name=fixture.level kind=int value=4 min=-1 max=10\r\nOK\r\n");
    }
    if (XIAOTAI_AT_INTENT_COMMAND) {
        expect("AT+INTENT binding.forget\n", "ERROR: NOT_ALLOWED\r\n");
        expect("AT+INTENT fixture.missing\n", "ERROR: NOT_FOUND\r\n");
        expect("AT+INTENT fixture.open -3 6869\n", "OK\r\n");
        wait_count(&s_intent_calls, 1);
    } else expect("AT+INTENT fixture.open -3 6869\n", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n");

    const uint32_t first = xiaotai_eventlog_latest();
    xiaotai_eventlog_note("fixture", "history", 1, 3);
    char query[32];
    assert(snprintf(query, sizeof(query), "AT+LOG %u\n", (unsigned)first) > 0);
    contains(command(query), "level=I src=fixture label=history gen=1 kind=n value=3\r\n+LOG-END: latest=");
    expect("AT+LOG nope\n", "ERROR: INVALID_ARG SEQUENCE\r\n");
    contains(command("AT+LOG STATS\n"), "+LOG-STATS: emitted=");
    static const char *const fields[] = {"count"};
    static const xiaotai_eventlog_topic_desc_t topic = {.source = "fixture", .name = "stats", .fields = fields, .count = 1};
    xiaotai_eventlog_topic_t *sample;
    assert(xiaotai_eventlog_topic_register(&topic, &sample) == XIAOTAI_OK);
    xiaotai_eventlog_post(sample, (const uint32_t[]){9});
    contains(command("AT+LOG TOPICS\n"), "+TOPIC: src=fixture name=stats posts=1 missed=0 age_ms=");
    contains(s_output, " count=9\r\nOK\r\n");
    expect("AT+LOG FOLLOW 1\n", "OK\r\n");
    xiaotai_eventlog_warn("fixture", "follow", 1, -4);
    xiaotai_eventlog_flush();
    contains(command(NULL), "level=W src=fixture label=follow gen=1 kind=n value=-4\r\n");
    expect("AT+LOG FOLLOW 0\n", "OK\r\n");
    xiaotai_eventlog_error("fixture", "silent", 1, XIAOTAI_FAIL);
    assert(!*command(NULL));

    if (XIAOTAI_AT_RESTART_FORCE) direct_restart("AT+RESTART FORCE\n", 200);
    else expect("AT+RESTART FORCE\n", XIAOTAI_AT_RESTART_COMMAND ? "ERROR: INVALID_ARG UNKNOWN_VERB\r\n" :
                "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n");
    if (XIAOTAI_AT_RESTART_COMMAND) {
        expect("AT+RESTART\n", "OK\r\n");
        wait_count(&s_orderly_calls, 1);
        contains(command("AT+STATUS framework\n"), " reason=console restart=1");
    } else expect("AT+RESTART\n", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n");
done:
    xiaotai_at_reset();
    xiaotai_framework_reset();
    xiaotai_eventlog_reset();
    return 0;
}
