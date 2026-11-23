// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_command.c::command
// Replaces legacy: tests/test_at_diagnostics.py
#include "at_test.h"
#include "internal.h"
#include "xiaotai/at.h"
#include "xiaotai/view.h"
#include "xiaotai_mem_host.h"

static const char *s_reason;
static unsigned s_action, s_help;
static xiaotai_err_t s_app_reg;
static xiaotai_err_t s_switch_result;

xiaotai_err_t __wrap_xiaotai_at_fail(const xiaotai_reply_t *reply, xiaotai_err_t error, const char *reason)
{
    (void)reply;
    s_reason = reason;
    return error;
}

static xiaotai_err_t status_a(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argv;
    CHECK(argc == 0);
    reply->write(reply->ctx, "a");
    return XIAOTAI_AT_PASS;
}

static xiaotai_err_t status_b(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argv;
    CHECK(argc == 0);
    xiaotai_at_printf(reply, "b");
    return XIAOTAI_AT_PASS;
}

static xiaotai_err_t accept_a(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argv;
    (void)reply;
    CHECK(argc == 0);
    s_action = s_action * 10 + 1;
    return XIAOTAI_AT_PASS;
}

static xiaotai_err_t accept_b(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argv;
    (void)reply;
    CHECK(argc == 0);
    s_action = s_action * 10 + 2;
    xiaotai_view_show(&(xiaotai_view_t){0});
    return XIAOTAI_ERR_BUSY;
}

static xiaotai_err_t alternate(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)reply;
    CHECK(argc == 2 && strcmp(argv[0], "FORGET") == 0 && strcmp(argv[1], "FORCE") == 0);
    ++s_action;
    return XIAOTAI_OK;
}

static xiaotai_err_t positional(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    CHECK(argc == 1);
    xiaotai_at_printf(reply, "pos=%s", argv[0]);
    return XIAOTAI_OK;
}

static const xiaotai_at_verb_t before[] = {
    {.run = status_a},
    {.word = "ACCEPT", .run = accept_a},
    {.word = "TRY", .run = accept_a},
    {.word = "PROVISION", .run = accept_a, .usage = "[FORCE]"},
};
static const xiaotai_at_verb_t app_verbs[] = {
    {.run = status_b},
    {.word = "ACCEPT", .run = accept_b},
    {.word = "PROVISION", .min_args = 2, .max_args = 2, .run = alternate, .usage = "FORGET [FORCE]"},
};
static const xiaotai_at_verb_t after[] = {
    {.run = positional, .min_args = 1, .max_args = 1, .usage = "<index>"},
};
static const xiaotai_at_verb_t named_only[] = {
    {.word = "SELECT", .run = positional, .min_args = 1, .max_args = 1, .usage = "<index>"},
};

static xiaotai_err_t aborted_init(void)
{
    CHECK(xiaotai_at_verbs_register("AT+NAMED", before, sizeof(before) / sizeof(before[0])) == XIAOTAI_OK);
    return XIAOTAI_FAIL;
}

static void help(const xiaotai_at_verb_t *verb, void *ctx)
{
    (void)ctx;
    if (verb->usage && strcmp(verb->usage, "<index>") == 0) ++s_help;
}

static xiaotai_err_t module_init(void)
{
    static const xiaotai_at_verb_t dup[] = {{.run = status_a}, {.run = status_b}};
    static const xiaotai_at_verb_t dup_named[] = {{.word = "FOO", .run = status_a}, {.word = "FOO", .run = status_b}};
    static const xiaotai_at_verb_t bad[] = {{.word = "select", .run = positional}};
    static const xiaotai_at_verb_t no_handler[] = {{.word = "NOTHING"}};
    static const xiaotai_at_verb_t bad_range[] = {{.word = "EMPTY", .run = positional, .min_args = 2, .max_args = 1}};
    static const xiaotai_at_verb_t exceeds_parser[] = {{.word = "LONG", .run = positional, .max_args = 11}};
    CHECK(xiaotai_at_verbs_register("AT+BAD", dup, 2) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_verbs_register("AT+BAD", dup_named, 2) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_verbs_register("AT+BAD", bad, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_verbs_register("AT+BAD", no_handler, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_verbs_register("AT+BAD", bad_range, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_verbs_register("AT+BAD", exceeds_parser, 1) == XIAOTAI_ERR_INVALID_ARG);
    const size_t before_count = sizeof(before) / sizeof(before[0]);
    CHECK(xiaotai_at_verbs_register("AT+bad", before, before_count) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_at_verbs_register("AT+JOIN", before, before_count) == XIAOTAI_ERR_NO_MEM);
    CHECK(!at_verbs_visit("AT+JOIN", NULL, NULL));
    CHECK(xiaotai_at_verbs_register("AT+JOIN", before, before_count) == XIAOTAI_OK);
    CHECK(xiaotai_at_verbs_register("AT+JOIN", after, 1) == XIAOTAI_OK);
    CHECK(xiaotai_at_verbs_register("AT+NAMED", named_only, 1) == XIAOTAI_OK);
    CHECK(xiaotai_command_register("AT+JOIN", NULL, positional) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_command_register("AT+DIRECT", NULL, positional) == XIAOTAI_OK);
    CHECK(xiaotai_at_verbs_register("AT+DIRECT", before, before_count) == XIAOTAI_ERR_INVALID_STATE);
    return XIAOTAI_OK;
}

static xiaotai_err_t background_init(void)
{
    s_app_reg = xiaotai_at_verbs_register("AT+JOIN", app_verbs, sizeof(app_verbs) / sizeof(app_verbs[0]));
    return s_app_reg;
}

static void fill_and_check_cap(void)
{
    /* Outside init the error must remain INVALID_STATE even for an existing command. */
    CHECK(xiaotai_at_verbs_register("AT+JOIN", after, 1) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_at_verbs_register("AT+NEW", after, 1) == XIAOTAI_ERR_INVALID_STATE);
}

static xiaotai_err_t third_init(void)
{
    for (size_t i = 4; i < XIAOTAI_AT_VERB_SETS_MAX; ++i) {
        CHECK(xiaotai_at_verbs_register("AT+FILL", named_only, 1) == XIAOTAI_OK);
    }
    CHECK(xiaotai_at_verbs_register("AT+FULL", named_only, 1) == XIAOTAI_ERR_NO_MEM);
    return XIAOTAI_OK;
}

static void view(const xiaotai_view_t *shown, void *ctx)
{
    (void)shown;
    ++*(unsigned *)ctx;
}

static void check_run(const char *line, xiaotai_err_t expected, const char *reason, const char *body)
{
    at_test_sink_t sink;
    const xiaotai_reply_t out = at_test_reply(&sink);
    s_reason = NULL;
    CHECK_TEXT(xiaotai_command_run(line, &out, 1000) == expected, line);
    CHECK_TEXT((!reason && !s_reason) || (reason && s_reason && strcmp(reason, s_reason) == 0), line);
    CHECK_TEXT(strcmp(body, sink.text) == 0, line);
}

static void switch_app(void *ctx)
{
    (void)ctx;
    s_switch_result = xiaotai_app_switch("back");
}

int main(void)
{
    at_test_sink_t sink;
    const xiaotai_reply_t out = at_test_reply(&sink);
    const char *args[] = {"AT+JOIN", "99"};
    CHECK(xiaotai_at_dispatch(after, 1, 2, args, &out) == XIAOTAI_OK);
    CHECK(strcmp(sink.text, "pos=99") == 0);
    CHECK(at_reply_original(&out) == &out);
    fill_and_check_cap();
    static const xiaotai_module_t aborted = {.name = "aborted", .init = aborted_init};
    static const xiaotai_module_t *const first[] = {&aborted};
    CHECK(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = first, .module_count = 1}) == XIAOTAI_FAIL);
    static const xiaotai_module_t verbs = {.name = "atfixture", .init = module_init};
    static const xiaotai_module_t *const modules[] = {&verbs};
    static const xiaotai_application_t home = {.name = "home"};
    static const xiaotai_application_t back = {.name = "back", .init = background_init};
    static const xiaotai_application_t fill = {.name = "fill", .init = third_init};
    static const xiaotai_application_t *const apps[] = {&home, &back, &fill};
    CHECK(xiaotai_framework_start(&(xiaotai_framework_config_t){
        .modules = modules, .module_count = 1, .applications = apps, .application_count = 3}) == XIAOTAI_OK);
    CHECK(s_app_reg == XIAOTAI_OK);
    fill_and_check_cap();
    unsigned views = 0;
    xiaotai_view_set_sink(view, &views);
    check_run("AT+JOIN", XIAOTAI_OK, NULL, "a|b");
    check_run("AT+JOIN 11", XIAOTAI_OK, NULL, "pos=11");
    check_run("AT+JOIN TRY", XIAOTAI_ERR_INVALID_STATE, "NO_OWNER", "");
    CHECK(s_action == 1);
    check_run("AT+JOIN ACCEPT 9", XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT", "");
    CHECK(s_action == 1);
    check_run("AT+JOIN ACCEPT", XIAOTAI_ERR_BUSY, NULL, "");
    CHECK(s_action == 112 && views == 0);
    CHECK(xiaotai_run(switch_app, NULL) == XIAOTAI_OK);
    check_run("AT+JOIN ACCEPT", XIAOTAI_ERR_BUSY, NULL, "");
    CHECK(s_switch_result == XIAOTAI_OK && views == 1);
    const unsigned before_alt = s_action;
    check_run("AT+JOIN PROVISION FORGET FORCE", XIAOTAI_OK, NULL, "");
    CHECK(s_action == before_alt + 1);
    check_run("AT+NAMED", XIAOTAI_ERR_INVALID_ARG, "MISSING_VERB", "");
    check_run("AT+NAMED unknown", XIAOTAI_ERR_INVALID_ARG, "UNKNOWN_VERB", "");
    check_run("AT+NAMED SELECT 7", XIAOTAI_OK, NULL, "pos=7");
    check_run("AT+FULL", XIAOTAI_ERR_NOT_FOUND, NULL, "");
    CHECK(at_verbs_visit("AT+JOIN", help, NULL) && s_help == 1);
    CHECK(at_verbs_visit("AT+NAMED", help, NULL) && s_help == 2);
    CHECK(!at_verbs_visit("AT+DIRECT", help, NULL));
    xiaotai_framework_reset();
    const int64_t allocations = xiaotai_mem_host_outstanding();
    xiaotai_at_reset();
    CHECK(!at_verbs_visit("AT+JOIN", NULL, NULL));
    CHECK(xiaotai_mem_host_outstanding() + 1 == allocations);
    return at_test_result("at_verbs");
}
