// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_command.c::command
#include <stdatomic.h>
#include "at_test.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"
#include "xiaotai_console_host.h"
#include "xiaotai_mem_host.h"

typedef struct {
    const char *input, *fail_prefix;
    size_t unread, used;
    unsigned written_on_owner;
    char output[8192];
    xiaotai_signal_t *ready;
} fixture_t;

static fixture_t s_left, s_right;
static xiaotai_signal_t s_release_owner, s_late_written;
static atomic_uint s_task_attempts;
static uint32_t s_requested_stack;
static bool s_fail_task;

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);

xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (strcmp(config->name, "at") == 0) {
        atomic_fetch_add(&s_task_attempts, 1);
        s_requested_stack = config->stack_bytes;
        CHECK(config->stack == XIAOTAI_MEM_BULK && config->priority == 2);
        if (s_fail_task) return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    return __real_xiaotai_task_create(config, task);
}

static size_t read_source(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    fixture_t *source = ctx;
    if (!source->unread) {
        xiaotai_delay_ms(timeout_ms);
        return 0;
    }
    const size_t count = source->unread < capacity ? source->unread : capacity;
    memcpy(data, source->input, count);
    source->input += count;
    source->unread -= count;
    return count;
}

static xiaotai_err_t write_source(void *ctx, const char *text, size_t length)
{
    fixture_t *source = ctx;
    CHECK(length >= 2 && text[length - 2] == '\r' && text[length - 1] == '\n');
    CHECK(memchr(text, '\r', length - 2) == NULL && memchr(text, '\n', length - 2) == NULL);
    if (source->fail_prefix && length >= strlen(source->fail_prefix) &&
        !memcmp(text, source->fail_prefix, strlen(source->fail_prefix))) {
        source->fail_prefix = NULL;
        return XIAOTAI_FAIL;
    }
    CHECK(source->used + length < sizeof(source->output));
    if (source->used + length >= sizeof(source->output)) return XIAOTAI_FAIL;
    memcpy(source->output + source->used, text, length);
    source->used += length;
    source->output[source->used] = '\0';
    if (xiaotai_on_owner()) ++source->written_on_owner;
    if (source->ready && length >= 7 && !memcmp(text, "+READY:", 7)) xiaotai_signal_give(source->ready);
    return XIAOTAI_OK;
}

static const xiaotai_at_io_t s_a = {"left", read_source, write_source, NULL, &s_left};
static const xiaotai_at_io_t s_b = {"right", read_source, write_source, NULL, &s_right};
static const char *const s_allow[] = {"AT+ECHO", NULL};

static void feed(fixture_t *source, const char *text, size_t length)
{
    source->used = 0;
    source->output[0] = '\0';
    source->input = text;
    source->unread = length;
}

static void expect(const char *cmd, const char *answer)
{
    feed(&s_left, cmd, strlen(cmd));
    for (size_t i = 0; i < strlen(cmd) + 1 && s_left.unread; ++i) {
        CHECK(xiaotai_at_service(10) == XIAOTAI_OK);
    }
    CHECK(s_left.unread == 0);
    if (strcmp(s_left.output, answer)) fprintf(stderr, "AT reply was [%s], expected [%s]\n", s_left.output, answer);
    CHECK_TEXT(!strcmp(s_left.output, answer), cmd);
}

static xiaotai_err_t local_echo(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    CHECK(!xiaotai_on_owner());
    CHECK(xiaotai_at_service(1) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_at_printf(reply, "+ECHO: argc=%d last=%s", argc, argv[argc - 1]);
    return XIAOTAI_OK;
}

static xiaotai_err_t local_race(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_signal_give(&s_release_owner);
    CHECK(xiaotai_signal_take(&s_late_written, 1000));
    xiaotai_at_printf(reply, "+RACE: ok=1");
    return XIAOTAI_OK;
}

static xiaotai_err_t owner_command(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    CHECK(xiaotai_on_owner());
    CHECK(xiaotai_at_service(1) == XIAOTAI_ERR_INVALID_STATE);
    if (argc == 2 && !strcmp(argv[1], "ERR")) return XIAOTAI_ERR_NOT_FOUND;
    if (argc == 2 && !strcmp(argv[1], "BUSY")) return XIAOTAI_ERR_BUSY;
    if (argc == 2 && !strcmp(argv[1], "TIMEOUT")) return XIAOTAI_ERR_TIMEOUT;
    if (argc == 2 && !strcmp(argv[1], "REASON")) {
        (void)xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "FIRST");
        (void)xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "bad reason");
    }
    if (argc == 2 && !strcmp(argv[1], "WIDE")) xiaotai_at_printf(reply, "%0513d", 0);
    else if (argc == 2 && !strcmp(argv[1], "MALFORMED")) xiaotai_at_printf(reply, "%s", "line\r\n");
    else if (argc == 2 && !strcmp(argv[1], "FORGED")) reply->write(reply->ctx, "OK\n");
    else if (argc == 2 && !strcmp(argv[1], "FLUSH")) {
        xiaotai_at_printf(reply, "+OWNER: %0504d", 1);
        xiaotai_at_printf(reply, "+OWNER: %0504d", 2);
    } else {
        reply->write(reply->ctx, "+OWNER: stage=ready\n+OWNER: stage=accepted\n");
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t owner_hold(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_at_printf(reply, "+OLD: before=1");
    CHECK(xiaotai_signal_take(&s_release_owner, 1000));
    reply->write(reply->ctx, "+OLD: late=1\n+OLD: late=2");
    (void)xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "WRONG_GENERATION");
    xiaotai_signal_give(&s_late_written);
    return XIAOTAI_ERR_INVALID_ARG;
}

static xiaotai_err_t owner_init(void)
{
    xiaotai_err_t err = xiaotai_eventlog_init(NULL);
    if (err == XIAOTAI_OK) err = xiaotai_engine_at_init();
    if (err == XIAOTAI_OK) err = xiaotai_command_register("AT+OWNER", NULL, owner_command);
    if (err == XIAOTAI_OK) err = xiaotai_command_register("AT+HOLD", NULL, owner_hold);
    return err;
}

static void test_manual_console(void)
{
    const xiaotai_at_config_t conf = {
        .line_max = 1024, .reply_buffer_bytes = 514, .command_timeout_ms = 70,
        .task_stack_bytes = XIAOTAI_AT_NO_TASK, .start_locked = true,
        .allow = s_allow, .no_platform_console = true,
    };
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){.line_max = 95}) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){.reply_buffer_bytes = 513}) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_configure(&conf) == XIAOTAI_OK);
    CHECK(xiaotai_at_local_register("BAD", NULL, local_echo) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_local_register("AT+ECHO", NULL, local_echo) == XIAOTAI_OK);
    CHECK(xiaotai_at_local_register("AT+ECHO", NULL, local_echo) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_at_attach(&s_a) == XIAOTAI_OK);
    CHECK(xiaotai_at_attach(&s_a) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!xiaotai_at_active());
    CHECK(xiaotai_at_service(1) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_at_open() == XIAOTAI_OK && !xiaotai_at_active());
    CHECK(xiaotai_at_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
    expect("\r  \t\n AT \r\n", "+READY: at=1 source=left locked=1\r\nOK\r\n");
    expect("AT+CLI\r", "+CLI: enabled=0\r\nOK\r\n");
    expect("\tAT+ECX\bHO\tx\177abc,0/http://x \r", "+ECHO: argc=2 last=abc,0/http://x\r\nOK\r\n");
    expect("AT+OWNER\r", "ERROR: INVALID_STATE CLI_DISABLED\r\n");
    expect("AT+CLI ON\rAT+OWNER\r", "OK\r\nERROR: INVALID_STATE STARTING\r\n");
    CHECK(xiaotai_at_active());
    expect("AT+ECHO 1 2 3 4 5 6 7 8 9 0 1\r", "+ECHO: argc=12 last=1\r\nOK\r\n");
    expect("AT+ECHO 1 2 3 4 5 6 7 8 9 0 1 2\r", "ERROR: INVALID_ARG TOO_MANY_WORDS\r\n");
    const char damaged[] = {'A', 'T', 0, '\b', '\n'};
    feed(&s_left, damaged, sizeof(damaged));
    CHECK(xiaotai_at_service(10) == XIAOTAI_OK);
    CHECK(!strcmp(s_left.output, "ERROR: INVALID_ARG BAD_CHARACTER\r\n"));
    char long_line[1050];
    memcpy(long_line, "AT", 2);
    memset(long_line + 2, ' ', sizeof(long_line) - 5);
    memcpy(long_line + sizeof(long_line) - 3, "\b\r", 3);
    expect(long_line, "ERROR: INVALID_SIZE LINE_TOO_LONG\r\n");

    CHECK(xiaotai_at_attach(&s_b) == XIAOTAI_OK);
    feed(&s_left, "AT+EC", 5);
    feed(&s_right, "AT\r", 3);
    CHECK(xiaotai_at_service(25) == XIAOTAI_OK);
    CHECK(!s_left.used && !strcmp(s_right.output, "+READY: at=1 source=right locked=0\r\nOK\r\n"));
    feed(&s_left, "HO hai\r", 7);
    CHECK(xiaotai_at_service(25) == XIAOTAI_OK);
    CHECK(!strcmp(s_left.output, "+ECHO: argc=2 last=hai\r\nOK\r\n"));
    CHECK(!strcmp(s_right.output, "+READY: at=1 source=right locked=0\r\nOK\r\n"));

    CHECK(xiaotai_at_local_register("AT+RACE", NULL, local_race) == XIAOTAI_OK);
    const xiaotai_module_t mod = {.name = "fixture", .init = owner_init, .start = xiaotai_engine_at_start};
    const xiaotai_module_t *const mods[] = {&mod};
    CHECK(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = mods, .module_count = 1}) == XIAOTAI_OK);
    CHECK(xiaotai_at_attach(&s_b) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_at_local_register("AT+OTHER", NULL, local_echo) == XIAOTAI_ERR_INVALID_STATE);
    expect("AT+MISSING\r", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n");
    expect("AT+OWNER\r", "+OWNER: stage=ready\r\n+OWNER: stage=accepted\r\nOK\r\n");
    expect("AT+OWNER ERR\r", "ERROR: NOT_FOUND\r\n");
    expect("AT+OWNER BUSY\r", "ERROR: BUSY\r\n");
    expect("AT+OWNER TIMEOUT\r", "ERROR: TIMEOUT\r\n");
    expect("AT+OWNER REASON\r", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
    expect("AT+OWNER WIDE\r", "ERROR: INVALID_SIZE OUTPUT_TOO_LONG\r\n");
    expect("AT+OWNER MALFORMED\r", "ERROR: FAIL OUTPUT\r\n");
    expect("AT+OWNER FORGED\r", "ERROR: FAIL OUTPUT\r\n");

    char wide_framework[770];
    memcpy(wide_framework, "AT+OWNER ", 9);
    memset(wide_framework + 9, 'A', 759);
    memcpy(wide_framework + 768, "\r", 2);
    expect(wide_framework, "ERROR: INVALID_SIZE LINE_TOO_LONG\r\n");
    feed(&s_left, "AT+OWNER FLUSH\r", sizeof("AT+OWNER FLUSH\r") - 1);
    CHECK(xiaotai_at_service(10) == XIAOTAI_OK);
    CHECK(s_left.written_on_owner == 1 && s_left.used == 2 * 514 + 4);
    CHECK(s_left.output[511] == '1' && !strcmp(s_left.output + 1025, "2\r\nOK\r\n"));
    s_left.fail_prefix = "+OWNER:";
    expect("AT+OWNER\r", "ERROR: FAIL OUTPUT\r\n");
    s_left.fail_prefix = "OK";
    expect("AT\r", "ERROR: FAIL OUTPUT\r\n");
    CHECK(xiaotai_signal_init(&s_release_owner) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_late_written) == XIAOTAI_OK);
    expect("AT+HOLD\rAT+OWNER\rAT+RACE\r",
           "ERROR: TIMEOUT HANDLER\r\nERROR: BUSY PREVIOUS_COMMAND\r\n+RACE: ok=1\r\nOK\r\n");
    expect("AT+OWNER\r", "+OWNER: stage=ready\r\n+OWNER: stage=accepted\r\nOK\r\n");
    expect("AT+CLI OFF\rAT+OWNER\rAT+ECHO\r",
           "OK\r\nERROR: INVALID_STATE CLI_DISABLED\r\n+ECHO: argc=1 last=AT+ECHO\r\nOK\r\n");
    CHECK(!xiaotai_at_active());
    xiaotai_at_stats_t stats;
    xiaotai_at_stats(&stats);
    CHECK(stats.lines == stats.commands_ok + stats.commands_failed);
    CHECK(stats.gate_rejections == 2 && stats.line_overflows == 2 && stats.bad_characters == 1);
    CHECK(stats.handler_timeouts == 1 && stats.late_dropped == 2 && stats.owner_flushes == 1);
    CHECK(stats.write_errors == 2);
    xiaotai_signal_deinit(&s_late_written);
    xiaotai_signal_deinit(&s_release_owner);
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
}

static void test_source_task(void)
{
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){
        .no_platform_console = true, .task_stack_bytes = XIAOTAI_AT_NO_TASK,
    }) == XIAOTAI_OK);
    CHECK(xiaotai_at_open() == XIAOTAI_OK && !xiaotai_at_active());
    CHECK(xiaotai_at_service(1000) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(atomic_load(&s_task_attempts) == 0);
    xiaotai_at_reset();
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){.no_platform_console = true}) == XIAOTAI_OK);
    CHECK(xiaotai_at_open() == XIAOTAI_OK && !xiaotai_at_active());
    CHECK(xiaotai_at_service(1000) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(atomic_load(&s_task_attempts) == 0);
    size_t task_count = 99;
    CHECK(xiaotai_task_list(NULL, 0, &task_count) == XIAOTAI_ERR_NOT_SUPPORTED && task_count == 0);
    s_left = (fixture_t){0};
    s_fail_task = true;
    CHECK(xiaotai_at_attach(&s_a) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(!xiaotai_at_active() && s_left.used == 0 && atomic_load(&s_task_attempts) == 1);
    CHECK(s_requested_stack == 5120);
    xiaotai_signal_t ready;
    CHECK(xiaotai_signal_init(&ready) == XIAOTAI_OK);
    s_left.ready = &ready;
    s_fail_task = false;
    CHECK(xiaotai_at_attach(&s_a) == XIAOTAI_OK && xiaotai_at_active());
    CHECK(xiaotai_signal_take(&ready, 1000));
    CHECK(!strcmp(s_left.output, "+READY: at=1 source=left locked=0\r\n"));
    CHECK(xiaotai_at_service(1) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(atomic_load(&s_task_attempts) == 2);
    xiaotai_at_reset();
    xiaotai_signal_deinit(&ready);
    CHECK(!xiaotai_at_active());

    xiaotai_console_host_pipe();
    s_fail_task = true;
    CHECK(xiaotai_at_open() == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(atomic_load(&s_task_attempts) == 3 && !xiaotai_at_active());
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){.no_platform_console = true}) == XIAOTAI_OK);
    CHECK(xiaotai_at_open() == XIAOTAI_OK && !xiaotai_at_active());
    CHECK(atomic_load(&s_task_attempts) == 3);
    xiaotai_at_reset();

    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){
        .no_platform_console = true, .task_stack_bytes = 3072,
    }) == XIAOTAI_OK);
    CHECK(xiaotai_at_open() == XIAOTAI_OK && !xiaotai_at_active());
    CHECK(xiaotai_at_attach(&s_a) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(atomic_load(&s_task_attempts) == 4 && s_requested_stack == 3072);
    xiaotai_at_reset();
    s_fail_task = false;
}

static void test_registry_limits(void)
{
    static char labels[XIAOTAI_AT_LOCAL_MAX][20];
    static char channel_names[XIAOTAI_AT_SOURCES_MAX + 1][16];
    static xiaotai_at_io_t sources[XIAOTAI_AT_SOURCES_MAX + 1];
    xiaotai_at_io_t invalid = s_a;
    invalid.name = "Punctuated-name";
    CHECK(xiaotai_at_attach(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    invalid.name = "sixteen_char_str";
    CHECK(xiaotai_at_attach(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_attach(NULL) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_mem_host_ledger_start();
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_at_local_register("AT+SOMETHING", NULL, local_echo) == XIAOTAI_ERR_NO_MEM);
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_at_attach(&s_a) == XIAOTAI_ERR_NO_MEM);
    for (unsigned i = 0; i < XIAOTAI_AT_LOCAL_MAX - 1; ++i) {
        (void)snprintf(labels[i], sizeof(labels[i]), "AT+LABEL%u", i);
        CHECK(xiaotai_at_local_register(labels[i], NULL, local_echo) ==
              (i < XIAOTAI_AT_LOCAL_MAX - 3 ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM));
    }
    /* The table is the only live request since starting this snapshot. */
    xiaotai_mem_host_ledger_entry_t table = {0};
    size_t live = 0;
    CHECK(xiaotai_mem_host_ledger_snapshot(&table, 1, &live) && live == 1);
    const size_t slot_bytes = 2 * sizeof(const char *) + sizeof(xiaotai_command_fn_t) +
                              sizeof(const xiaotai_at_verb_t *) + sizeof(size_t);
    CHECK(table.kind == XIAOTAI_MEM_BULK && table.bytes == (XIAOTAI_AT_LOCAL_MAX - 3) * slot_bytes);
    for (unsigned i = 0; i <= XIAOTAI_AT_SOURCES_MAX; ++i) {
        (void)snprintf(channel_names[i], sizeof(channel_names[i]), "channel_%u", i);
        sources[i] = s_a;
        sources[i].name = channel_names[i];
        CHECK(xiaotai_at_attach(&sources[i]) == (i < XIAOTAI_AT_SOURCES_MAX ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM));
    }
    xiaotai_mem_host_fail_nth(2);
    CHECK(xiaotai_at_open() == XIAOTAI_ERR_NO_MEM && !xiaotai_at_active());
    xiaotai_at_reset();
}

static const xiaotai_at_verb_t sample_verb[] = {{.run = owner_command}};

static xiaotai_err_t init_without_at_tables(void)
{
    const xiaotai_err_t err = owner_init();  /* Includes direct framework commands. */
    if (err != XIAOTAI_OK) return err;
    static const xiaotai_at_verb_t broken_verb[] = {{.min_args = 1, .run = owner_command}};
    CHECK(xiaotai_at_verbs_register("AT+BAD", broken_verb, 1) == XIAOTAI_ERR_INVALID_ARG);
    const int64_t baseline = xiaotai_mem_host_outstanding();
    CHECK(xiaotai_at_verbs_register("AT+SAMPLE", sample_verb, 1) == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_outstanding() == baseline);
    CHECK(xiaotai_command_count() == 2);
    return XIAOTAI_OK;
}

static void record_reply(void *ctx, const char *line)
{
    if (*line) *(bool *)ctx = true;
}

static void test_omitted_at_tables(void)
{
    const xiaotai_at_config_t lean = {.no_platform_console = true, .omit_command_tables = true};
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){.omit_command_tables = true}) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_attach(&s_a) == XIAOTAI_OK);
    CHECK(xiaotai_at_configure(&lean) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_at_reset();
    CHECK(xiaotai_at_configure(&lean) == XIAOTAI_OK);
    CHECK(xiaotai_at_attach(&s_a) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_at_local_register("AT+ECHO", NULL, local_echo) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_at_verbs_register("AT+SAMPLE", sample_verb, 1) == XIAOTAI_ERR_INVALID_STATE);
    const int64_t baseline = xiaotai_mem_host_outstanding();
    CHECK(xiaotai_at_open() == XIAOTAI_OK && !xiaotai_at_active());
    CHECK(xiaotai_mem_host_outstanding() == baseline);

    static const xiaotai_module_t at = {.name = "at", .init = init_without_at_tables};
    static const xiaotai_module_t *const mods[] = {&at};
    CHECK(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = mods, .module_count = 1}) == XIAOTAI_OK);
    bool wrote = false;
    const xiaotai_reply_t reply = {.write = record_reply, .ctx = &wrote};
    CHECK(xiaotai_command_run("AT+SAMPLE", &reply, 1000) == XIAOTAI_ERR_NOT_FOUND && !wrote);
    xiaotai_diag_report("state", &reply);
    CHECK(wrote);
    wrote = false;
    CHECK(xiaotai_command_run("AT+OWNER", &reply, 1000) == XIAOTAI_OK && wrote);
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
}

int main(void)
{
    CHECK(xiaotai_kv_init() == XIAOTAI_OK); /* Framework shares the process-lifetime KV worker. */
    const int64_t allocations = xiaotai_mem_host_outstanding();
    test_manual_console();
    test_source_task();
    test_omitted_at_tables();
    test_registry_limits();
    CHECK(xiaotai_mem_host_outstanding() == allocations);
    return at_test_result("at_console");
}
