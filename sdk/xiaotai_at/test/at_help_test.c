// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/at.h"
#include "xiaotai/port/os.h"

static struct {
    const char *input;
    size_t left, used;
    char output[4096];
} wire;

static size_t read_bytes(void *ctx, uint8_t *data, size_t size, uint32_t wait)
{
    (void)ctx;
    if (!wire.left) {
        xiaotai_delay_ms(wait);
        return 0;
    }
    size_t n = wire.left < size ? wire.left : size;
    memcpy(data, wire.input, n);
    wire.input += n;
    wire.left -= n;
    return n;
}

static xiaotai_err_t write_record(void *ctx, const char *data, size_t size)
{
    (void)ctx;
    assert(!xiaotai_on_owner());
    assert(size >= 2 && !memcmp(data + size - 2, "\r\n", 2));
    assert(wire.used + size < sizeof(wire.output));
    memcpy(wire.output + wire.used, data, size);
    wire.used += size;
    wire.output[wire.used] = '\0';
    return XIAOTAI_OK;
}

static void expect(const char *line, const char *reply)
{
    wire.input = line;
    wire.left = strlen(line);
    wire.used = 0;
    wire.output[0] = '\0';
    for (unsigned i = 0; i < 5 && wire.left; ++i) assert(xiaotai_at_service(20) == XIAOTAI_OK);
    assert(!wire.left);
    if (strcmp(wire.output, reply)) fprintf(stderr, "%s -> [%s], expected [%s]\n", line, wire.output, reply);
    assert(!strcmp(wire.output, reply));
}

static xiaotai_err_t noop(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    return XIAOTAI_OK;
}

static xiaotai_err_t failing_init(void)
{
    static const xiaotai_at_verb_t abandoned[] = {{.run = noop}};
    assert(xiaotai_at_verbs_register("AT+ABANDONED", abandoned, 1) == XIAOTAI_OK);
    return XIAOTAI_FAIL;
}

static xiaotai_err_t owners_init(void)
{
    static const xiaotai_at_verb_t first[] = {
        {.run = noop, .usage = "[<name>]", .max_args = 1},
        {.word = "ON", .run = noop},
        {.word = "TAG", .run = noop, .min_args = 1, .max_args = 1, .usage = "<n>"},
    };
    static const xiaotai_at_verb_t second[] = {
        {.run = noop, .usage = "<name> <value>", .min_args = 2, .max_args = 2},
        {.word = "ON", .run = noop, .usage = ""},
        {.word = "TAG", .run = noop, .min_args = 1, .max_args = 1, .usage = "<id>"},
    };
    assert(xiaotai_at_verbs_register("AT+SHOW", first, 3) == XIAOTAI_OK);
    assert(xiaotai_at_verbs_register("AT+SHOW", second, 3) == XIAOTAI_OK);
    assert(xiaotai_command_register("AT+DIRECT", "<data>", noop) == XIAOTAI_OK);
    assert(xiaotai_command_register("AT+RESCUE", "<not-reachable>", noop) == XIAOTAI_OK);
    return XIAOTAI_OK;
}

#define HELP_CORE "+HELP: AT\r\n+HELP: AT+CLI\r\n+HELP: AT+CLI ON\r\n+HELP: AT+CLI OFF\r\n+HELP: AT+HELP\r\n"
#define HELP_LOCALS "+HELP: AT+VERSION\r\n+HELP: AT+LOG [<after>]\r\n+HELP: AT+LOG FOLLOW 0|1\r\n" \
                    "+HELP: AT+LOG TOPICS\r\n+HELP: AT+LOG STATS\r\n+HELP: AT+MEM\r\n+HELP: AT+MEM RTC\r\n+HELP: AT+CONSOLE\r\n" \
                    "+HELP: AT+RESTART\r\n+HELP: AT+RESTART FORCE\r\n"
#define HELP_SHOW "+HELP: AT+SHOW [<name>]\r\n+HELP: AT+SHOW ON\r\n+HELP: AT+SHOW TAG <n>\r\n" \
                  "+HELP: AT+SHOW <name> <value>\r\n+HELP: AT+SHOW TAG <id>\r\n"

int main(void)
{
    static const char *const allow[] = {"AT+HELP", "AT+RESCUE", "AT+SHOW", NULL};
    const xiaotai_at_config_t config = {.task_stack_bytes = XIAOTAI_AT_NO_TASK, .no_platform_console = true,
                                       .start_locked = true, .allow = allow};
    static const xiaotai_at_io_t io = {.name = "help", .read = read_bytes, .write = write_record};
    assert(xiaotai_at_configure(&config) == XIAOTAI_OK);
    assert(xiaotai_at_attach(&io) == XIAOTAI_OK);
    assert(xiaotai_at_local_register("AT+RESCUE", "<arg>", noop) == XIAOTAI_OK);
    assert(xiaotai_at_local_register("AT+HIDDEN", NULL, noop) == XIAOTAI_OK);
    assert(xiaotai_at_open() == XIAOTAI_OK);
    expect("AT+HELP\r", "+READY: at=1 source=help locked=1\r\n" HELP_CORE "+HELP: AT+RESCUE <arg>\r\nOK\r\n");
    expect("AT+HELP EXTRA\r", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");

    const xiaotai_module_t bad = {.name = "bad", .init = failing_init};
    const xiaotai_module_t *const bad_modules[] = {&bad};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = bad_modules, .module_count = 1}) ==
           XIAOTAI_FAIL);
    expect("AT+HELP\r", HELP_CORE "+HELP: AT+RESCUE <arg>\r\nOK\r\n");
    const xiaotai_module_t good = {.name = "owners", .init = owners_init, .start = xiaotai_engine_at_start};
    const xiaotai_module_t *const good_modules[] = {&good};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = good_modules, .module_count = 1}) ==
           XIAOTAI_OK);
    expect("AT+HELP\r", HELP_CORE "+HELP: AT+RESCUE <arg>\r\n" HELP_SHOW "OK\r\n");
    expect("AT+CLI ON\r", "OK\r\n");
    expect("AT+HELP\r", HELP_CORE "+HELP: AT+RESCUE <arg>\r\n+HELP: AT+HIDDEN\r\n" HELP_LOCALS HELP_SHOW
           "+HELP: AT+DIRECT <data>\r\nOK\r\n");
    xiaotai_at_reset();
    xiaotai_framework_reset();
    return 0;
}
