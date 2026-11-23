// SPDX-License-Identifier: MIT
/* The plain maintenance commands (src/at_plain.c): each is translated into this
 * vocabulary's AT lines and answered with their data lines and one final
 * line. Fake framework commands stand in for the modules: AT+NET, AT+CALL,
 * AT+CONTACTS, AT+PLATFORM. */
#include <stdlib.h>
#include "at_test.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"

typedef struct {
    const char *input;
    size_t unread, used;
    char output[8192];
} fixture_t;

static fixture_t s_console;

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
    if (source->used + length >= sizeof(source->output)) return XIAOTAI_FAIL;
    memcpy(source->output + source->used, text, length);
    source->used += length;
    source->output[source->used] = '\0';
    return XIAOTAI_OK;
}

static const xiaotai_at_io_t s_io = {"plain_test", read_source, write_source, NULL, &s_console};

static const char *run_line(const char *line)
{
    s_console.used = 0;
    s_console.output[0] = '\0';
    s_console.input = line;
    s_console.unread = strlen(line);
    for (int i = 0; i < 200 && s_console.unread; ++i) CHECK(xiaotai_at_service(10) == XIAOTAI_OK);
    CHECK(xiaotai_at_service(10) == XIAOTAI_OK);
    return s_console.output;
}

static void expect(const char *line, const char *answer)
{
    const char *got = run_line(line);
    if (strcmp(got, answer)) fprintf(stderr, "reply to [%s] was [%s], expected [%s]\n", line, got, answer);
    CHECK_TEXT(!strcmp(got, answer), line);
}

/* ------------------------------------------------------------ fake modules */

static char s_last[128];
static const char *s_device_state = "idle", *s_wechat_state = "idle";
static unsigned s_page;

static xiaotai_err_t net_show(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    xiaotai_at_printf(reply, "+NET: state=up");
    return XIAOTAI_OK;
}

static xiaotai_err_t net_wifi(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    xiaotai_at_printf(reply, "+NET-ARGS: ssid=%s password=%s", argv[0], argv[1]);
    return XIAOTAI_OK;
}

static xiaotai_err_t platform_show(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    xiaotai_at_printf(reply, "+PLATFORM: online=1");
    return XIAOTAI_OK;
}

static xiaotai_err_t call_show(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    xiaotai_at_printf(reply, "+CALL: kind=device state=%s gen=1", s_device_state);
    xiaotai_at_printf(reply, "+CALL: kind=wechat state=%s gen=2", s_wechat_state);
    return XIAOTAI_OK;
}

static xiaotai_err_t call_record(const char *what, int argc, const char *const *argv)
{
    (void)snprintf(s_last, sizeof(s_last), "%s%s%s", what, argc ? " " : "", argc ? argv[0] : "");
    return XIAOTAI_OK;
}
static xiaotai_err_t call_accept(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{ (void)reply; return call_record("ACCEPT", argc, argv); }
static xiaotai_err_t call_reject(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{ (void)reply; return call_record("REJECT", argc, argv); }
static xiaotai_err_t call_hangup(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{ (void)reply; return call_record("HANGUP", argc, argv); }
static xiaotai_err_t call_index(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)reply;
    char what[24];
    (void)snprintf(what, sizeof(what), "INDEX page=%u", s_page);
    return call_record(what, argc, argv);
}

/* Two pages of three: page 0 has two devices and a WeChat contact, page 1 a
 * device named "Front" (46726F6E74) and a contact named "家" (E5AEB6). */
static void list_page(const xiaotai_reply_t *reply)
{
    xiaotai_at_printf(reply, "+CONTACTS: cached=3 total=5 filter=all page=%u loading=0 error=0", s_page);
    if (s_page == 0) {
        xiaotai_at_printf(reply, "+CONTACT: index=0 source=device online=1 id=TIRAAA name_hex=-");
        xiaotai_at_printf(reply, "+CONTACT: index=1 source=device online=0 id=TIRBBB name_hex=4F6666696365");
        xiaotai_at_printf(reply, "+CONTACT: index=2 source=wechat online=- id=- name_hex=-");
    } else if (s_page == 1) {
        xiaotai_at_printf(reply, "+CONTACT: index=0 source=device online=1 id=TIRCCC name_hex=46726F6E74");
        xiaotai_at_printf(reply, "+CONTACT: index=1 source=wechat online=- id=- name_hex=E5AEB6");
    }
}

static xiaotai_err_t contacts_show(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    list_page(reply);
    return XIAOTAI_OK;
}

static xiaotai_err_t contacts_page(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    CHECK(!strcmp(argv[0], "ALL"));
    s_page = argc == 2 ? (unsigned)atoi(argv[1]) : 0;
    list_page(reply);
    return XIAOTAI_OK;
}

static xiaotai_err_t modules_init(void)
{
    static const xiaotai_at_verb_t net[] = {
        {.run = net_show},
        {.word = "WIFI", .min_args = 2, .max_args = 2, .run = net_wifi, .usage = "<ssid-hex> <password-hex|->"},
    };
    static const xiaotai_at_verb_t platform[] = {{.run = platform_show}};
    static const xiaotai_at_verb_t call[] = {
        {.run = call_show}, {.word = "ACCEPT", .run = call_accept}, {.word = "REJECT", .run = call_reject},
        {.word = "HANGUP", .run = call_hangup},
        {.word = "INDEX", .min_args = 1, .max_args = 1, .run = call_index, .usage = "<n>"},
    };
    static const xiaotai_at_verb_t contacts[] = {
        {.run = contacts_show},
        {.word = "PAGE", .min_args = 1, .max_args = 2, .run = contacts_page, .usage = "<ALL|kind> [<page>]"},
    };
    xiaotai_err_t err = xiaotai_eventlog_init(NULL);
    if (err == XIAOTAI_OK) err = xiaotai_engine_at_init();
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+NET", net, 2);
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+PLATFORM", platform, 1);
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+CALL", call, 5);
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+CONTACTS", contacts, 2);
    return err;
}

/* ------------------------------------------------------------------- tests */

int main(void)
{
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){
        .task_stack_bytes = XIAOTAI_AT_NO_TASK, .no_platform_console = true, .command_timeout_ms = 1000,
    }) == XIAOTAI_OK);
    CHECK(xiaotai_at_attach(&s_io) == XIAOTAI_OK);
    CHECK(xiaotai_at_open() == XIAOTAI_OK);
    (void)run_line("AT\r");

    /* Before the framework runs only the console's own commands answer:
     * status prints what it can (memory), a module's command is not there yet. */
    const char *early = run_line("status\r");
    CHECK(!strncmp(early, "+MEM: kind=internal", 19) && strstr(early, "+NET:") == NULL);
    CHECK(strlen(early) > 4 && !strcmp(early + strlen(early) - 4, "OK\r\n"));
    expect("call-status\r", "ERROR: INVALID_STATE STARTING\r\n");

    const xiaotai_module_t mod = {.name = "fixture", .init = modules_init, .start = xiaotai_engine_at_start};
    const xiaotai_module_t *const mods[] = {&mod};
    CHECK(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = mods, .module_count = 1}) == XIAOTAI_OK);

    /* Names, case and unknown commands. */
    expect("no-such-thing\r", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n");
    expect("room-status\r", "ERROR: NOT_SUPPORTED NOT_IN_BUILD\r\n");
    expect("Call-Status\r", "+CALL: kind=device state=idle gen=1\r\n+CALL: kind=wechat state=idle gen=2\r\nOK\r\n");

    /* Text arguments go hex-encoded; quotes keep spaces and the empty text. */
    expect("wifi-set \"Home WiFi\" pass\r", "+NET-ARGS: ssid=486F6D652057694669 password=70617373\r\nOK\r\n");
    expect("wifi-set \"\xe5\xae\xb6\" \"\"\r", "+NET-ARGS: ssid=E5AEB6 password=-\r\nOK\r\n");
    expect("wifi-set one\r", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
    expect("wifi-set \"open quote\r", "ERROR: INVALID_ARG QUOTE\r\n");
    expect("wifi-set a\"b c\r", "ERROR: INVALID_ARG QUOTE\r\n");
    /* UTF-8 belongs in a plain command's text; an AT line with it is damaged. */
    expect("AT+NET WIFI \xe5\xae\xb6 -\r", "ERROR: INVALID_ARG BAD_CHARACTER\r\n");

    /* status runs the queries this build has, in order. */
    const char *status = run_line("status\r");
    CHECK(strstr(status, "+NET: state=up\r\n+PLATFORM: online=1\r\n+STATE: ") != NULL);
    CHECK(strstr(status, "+MEM: kind=internal") != NULL);
    CHECK(strlen(status) > 4 && !strcmp(status + strlen(status) - 4, "OK\r\n"));

    /* Answering a kind answers only that kind; hanging up a ringing call rejects it. */
    s_device_state = "incoming";
    expect("wechat-answer\r", "ERROR: INVALID_STATE NO_INCOMING\r\n");
    expect("device-answer\r", "OK\r\n");
    CHECK(!strcmp(s_last, "ACCEPT"));
    expect("call-hangup\r", "OK\r\n");
    CHECK(!strcmp(s_last, "REJECT"));
    s_device_state = "active";
    expect("call-hangup\r", "OK\r\n");
    CHECK(!strcmp(s_last, "HANGUP"));
    s_device_state = "idle";

    /* Contacts are found by name on whichever page they are, then dialled there. */
    expect("wechat-call\r", "OK\r\n");
    CHECK(!strcmp(s_last, "INDEX page=0 2"));
    expect("device-call Front\r", "OK\r\n");
    CHECK(!strcmp(s_last, "INDEX page=1 0"));
    expect("wechat-call \"\xe5\xae\xb6\"\r", "OK\r\n");
    CHECK(!strcmp(s_last, "INDEX page=1 1"));
    expect("device-call TIRBBB\r", "OK\r\n");
    CHECK(!strcmp(s_last, "INDEX page=0 1"));
    expect("device-call Nobody\r", "ERROR: NOT_FOUND CONTACT\r\n");

    /* Log levels: the system log's line, and its threshold. */
    expect("log-level\r", "+LOGLEVEL: target=app level=3 max=4\r\nOK\r\n");
    expect("log-level app 2\r", "OK\r\n");
    CHECK(xiaotai_log_level() == 2);
    expect("log-level app 9\r", "ERROR: INVALID_ARG LEVEL\r\n");
    expect("log-level speaker 1\r", "ERROR: INVALID_ARG LOG_TARGET\r\n");
    /* No RTC stack in this build, so no verb for its level. */
    expect("log-level tirtc 3\r", "ERROR: NOT_SUPPORTED NOT_IN_BUILD\r\n");
    CHECK(xiaotai_log_set_level(3) == XIAOTAI_OK);

    /* help lists what this build can run. */
    const char *help = run_line("help\r");
    CHECK(strstr(help, "+HELP: wifi-set <ssid> <password>\r\n") != NULL);
    CHECK(strstr(help, "+HELP: status\r\n") != NULL);
    CHECK(strstr(help, "room-status") == NULL);
    CHECK(strstr(help, "+HELP: AT+HELP lists the AT commands\r\nOK\r\n") != NULL);

    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    return at_test_result("at_plain");
}
