// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_environment_console.py
// Replaces legacy: tests/regression/test_runtime_maintenance.py
#include "environment_internal.h"
#include "fakes.h"
#include "internal.h" /* The real AT-2 verb registry, consumed by AT-5 HELP. */
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/json.h"
#include "xiaotai/port/os.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static xiaotai_signal_t notified, owner_done;
static char notices[8][80];
static atomic_uint notice_count;
static atomic_int guard_result;
static const char *wire_input;
static char wire_output[2048];
static size_t wire_used;

typedef struct { const xiaotai_json_doc_t *doc; int array; size_t index; } manifest_t;

/* Capture this module's requests at the typed AT API, not UART bytes or
 * inter-command ordering, even when the real AT engine is linked. */
xiaotai_err_t __wrap_xiaotai_at_notifyf(const char *format, ...) __attribute__((format(printf, 1, 2)));
xiaotai_err_t __wrap_xiaotai_at_notifyf(const char *format, ...)
{
    unsigned pos = atomic_load(&notice_count);
    assert(pos < sizeof(notices) / sizeof(notices[0]));
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(notices[pos], sizeof(notices[pos]), format, args);
    va_end(args);
    assert(n > 0 && (size_t)n < sizeof(notices[pos]));
    atomic_fetch_add(&notice_count, 1U);
    xiaotai_signal_give(&notified);
    return XIAOTAI_OK;
}

typedef struct {
    char lines[4][XIAOTAI_AT_OUT_LINE_MAX + 1U];
    unsigned count;
    bool flip_snapshot;
} capture_t;

static void reply_line(void *arg, const char *line)
{
    capture_t *out = arg;
    assert(out->count < sizeof(out->lines) / sizeof(out->lines[0]) && strlen(line) <= XIAOTAI_AT_OUT_LINE_MAX);
    strcpy(out->lines[out->count++], line);
    if (out->flip_snapshot && out->count == 1U) atomic_store(&g_platform.unconfirmed, true);
}

static xiaotai_err_t run(const char *line, capture_t *out)
{
    out->count = 0;
    const xiaotai_reply_t reply = {.write = reply_line, .ctx = out};
    return xiaotai_command_run(line, &reply, 2000);
}

static void announce_done(void *ctx) { (void)ctx; xiaotai_signal_give(&owner_done); }

static void sync_owner(void)
{
    for (unsigned n = 0; n < 2; ++n) {
        assert(xiaotai_run(announce_done, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&owner_done, 2000));
    }
}

static size_t wire_read(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    const size_t left = wire_input ? strlen(wire_input) : 0U;
    const size_t n = left < capacity ? left : capacity;
    if (n) {
        memcpy(data, wire_input, n);
        wire_input += n;
    }
    return n;
}

static xiaotai_err_t wire_write(void *ctx, const char *data, size_t n)
{
    (void)ctx;
    assert(wire_used + n < sizeof(wire_output));
    memcpy(wire_output + wire_used, data, n);
    wire_used += n;
    wire_output[wire_used] = '\0';
    return XIAOTAI_OK;
}

static const char *wire(const char *command)
{
    wire_input = command;
    wire_used = 0;
    wire_output[0] = '\0';
    for (unsigned i = 0; i < 8U && *wire_input; ++i)
        assert(xiaotai_at_service(10) == XIAOTAI_OK);
    assert(!*wire_input);
    return wire_output;
}

static xiaotai_err_t guard(const char *why, void *ctx)
{
    (void)ctx;
    return !strcmp(why, XIAOTAI_QUIESCE_ENVIRONMENT) ? atomic_load(&guard_result) : XIAOTAI_OK;
}

static xiaotai_err_t setup_guard(void) { return xiaotai_quiesce_guard_register("test_guard", guard, NULL); }

static const xiaotai_module_t env = {.name = "environment", .init = xiaotai_module_environment_init,
                                    .start = xiaotai_module_environment_start};
static const xiaotai_module_t guard_module = {.name = "test_guard", .init = setup_guard};

static void boot(bool occupied_before_invalid_base)
{
    fake_platform_reset();
    fake_net_reset();
    fake_preferences_reset();
    fake_trace_clear();
    static const xiaotai_at_io_t io = {.name = "env_fixture", .read = wire_read, .write = wire_write};
    static const xiaotai_module_t log = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const char *const at_deps[] = {"eventlog", NULL};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init,
        .start = xiaotai_engine_at_start, .depends = at_deps};
    const xiaotai_module_t *modules[] = {&log, &at, &guard_module, &env};
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 4, .available_needs = UINT32_MAX};
    assert(xiaotai_signal_init(&notified) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&owner_done) == XIAOTAI_OK);
    assert(xiaotai_at_configure(&(const xiaotai_at_config_t){
        .task_stack_bytes = XIAOTAI_AT_NO_TASK, .no_platform_console = true, .start_locked = true,
    }) == XIAOTAI_OK);
    assert(xiaotai_environment_configure(&(const xiaotai_environment_config_t){
        .occupied_before_invalid_base = occupied_before_invalid_base,
    }) == XIAOTAI_OK);
    assert(xiaotai_at_attach(&io) == XIAOTAI_OK && xiaotai_at_open() == XIAOTAI_OK);
    assert(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    sync_owner();
}

static void test_console(void)
{
    const unsigned snapshots = atomic_load(&g_platform.snapshots);
    const unsigned stores = atomic_load(&g_platform.stores);
    (void)wire("AT\r");
    assert(!strcmp(wire("AT+EP\r"), "ERROR: INVALID_STATE CLI_DISABLED\r\n"));
    assert(!strcmp(wire("AT+EP SET http://test.example/pre\r"), "ERROR: INVALID_STATE CLI_DISABLED\r\n"));
    assert(atomic_load(&g_platform.snapshots) == snapshots && atomic_load(&g_platform.stores) == stores);
    assert(!strcmp(wire("AT+CLI ON\r"), "OK\r\n"));
    assert(!strcmp(wire("AT+EPOFF\r"), "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n"));
    assert(!strcmp(wire("AT+EP SET http://test.example/pre extra\r"), "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n"));
    assert(!strcmp(wire("AT+EP SET\r"), "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n"));
    assert(!strcmp(wire("AT+EP CANCEL\r"), "ERROR: INVALID_STATE\r\n"));

    atomic_store(&g_platform.unconfirmed, true);
    const char *output = wire("AT+EP\r");
    assert(strstr(output, "active=http://active.example.test/services"));
    assert(strstr(output, "+EP-SAVED: saved=? next=?\r\nOK\r\n") && !strstr(output, "ERROR:"));
    atomic_store(&g_platform.unconfirmed, false);

    atomic_store(&g_platform.boot_error, XIAOTAI_ERR_NOT_FOUND);
    output = wire("AT+EP\r");
    assert(strstr(output, "boot_error=261") && strstr(output, "OK\r\n") && !strstr(output, "ERROR:"));
    atomic_store(&g_platform.boot_error, XIAOTAI_OK);
    assert(atomic_load(&g_platform.stores) == stores && !xiaotai_restart_pending());
}

static void validate_manifest_verb(const xiaotai_at_verb_t *verb, void *arg)
{
    manifest_t *m = arg;
    const int object = xiaotai_json_at(m->doc, m->array, m->index++);
    const int word = xiaotai_json_get(m->doc, object, "verb");
    const int usage = xiaotai_json_get(m->doc, object, "usage");
    const int access = xiaotai_json_get(m->doc, object, "access");
    assert(xiaotai_json_equals(m->doc, xiaotai_json_get(m->doc, object, "command"), "AT+EP"));
    assert(verb->word ? xiaotai_json_equals(m->doc, word, verb->word) : word < 0);
    assert(verb->usage ? xiaotai_json_equals(m->doc, usage, verb->usage) : usage < 0);
    const bool set = verb->word && !strcmp(verb->word, "SET");
    assert(verb->min_args == (unsigned)set && verb->max_args == (unsigned)set);
    const char *permission = !verb->word ? "read" : !strcmp(verb->word, "CANCEL") ? "action" : "guarded";
    assert(xiaotai_json_equals(m->doc, access, permission));
}

static void test_registration(void)
{
    char text[2048];
    FILE *file = fopen(XT_ENV_MANIFEST, "rb");
    assert(file);
    const size_t length = fread(text, 1, sizeof(text), file);
    assert(feof(file) && fclose(file) == 0);
    xiaotai_json_token_t tokens[100];
    xiaotai_json_doc_t doc;
    size_t n;
    assert(xiaotai_json_parse(text, length, tokens, 100, &n, &doc) == XIAOTAI_OK);
    manifest_t m = {
        .doc = &doc, .array = xiaotai_json_get(&doc, 0, "commands")};
    assert(at_verbs_visit("AT+EP", validate_manifest_verb, &m));
    assert(m.index == 4 && xiaotai_json_at(&doc, m.array, 4) < 0);
    assert(xiaotai_intent_post_external(XIAOTAI_INTENT_ENVIRONMENT_RESET, NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    assert(xiaotai_intent_post_external(XIAOTAI_INTENT_ENVIRONMENT_CANCEL, NULL) == XIAOTAI_ERR_NOT_ALLOWED);
}

static void make_base(char *base, size_t size)
{
    static const char prefix[] = "http://fixture.test/";
    assert(size >= sizeof(prefix));
    memset(base, 'a', size);
    memcpy(base, prefix, sizeof(prefix) - 1U);
    base[size - 1U] = '\0';
}

static void diagnostics(void *arg)
{
    const xiaotai_reply_t reply = {.write = reply_line, .ctx = arg};
    xiaotai_diag_report("environment", &reply);
}

static void run_diagnostics(capture_t *out)
{
    out->count = 0;
    assert(xiaotai_run(diagnostics, out) == XIAOTAI_OK);
    sync_owner();
}

static void assert_saved_records(const capture_t *out, bool diag, bool split, const char *saved, const char *next)
{
    const char *section = diag ? "ENVIRONMENT" : "EP";
    char expected[XIAOTAI_AT_OUT_LINE_MAX + 1U];
    const unsigned data_lines = split ? 3U : 2U;
    assert(out->count == data_lines + (unsigned)diag);
    int n = snprintf(expected, sizeof(expected), "+%s-SAVED: saved=%s%s%s", section, saved,
                     split ? "" : " next=", split ? "" : next);
    assert(n >= 0 && (size_t)n < sizeof(expected) && !strcmp(out->lines[1], expected));
    if (split) {
        n = snprintf(expected, sizeof(expected), "+%s-NEXT: next=%s", section, next);
        assert(n >= 0 && (size_t)n < sizeof(expected) && !strcmp(out->lines[2], expected));
    }
    if (diag) assert(!strncmp(out->lines[data_lines], "+ENVIRONMENT-STATS: ", sizeof("+ENVIRONMENT-STATS: ") - 1U));
}

static void test_show(void)
{
    capture_t out = {0};
    const unsigned before = atomic_load(&g_platform.snapshots);
    out.flip_snapshot = true;
    assert(run("AT+EP", &out) == XIAOTAI_OK && out.count == 2);
    assert(atomic_load(&g_platform.snapshots) == before + 1U);
    assert(!strcmp(out.lines[0], "+EP: active=http://active.example.test/services phase=0 error=0 boot_error=0 "
                                "cancel=0 write_attempted=0"));
    assert(!strcmp(out.lines[1], "+EP-SAVED: saved=default next=http://builtin.example.test/services"));
    out.flip_snapshot = false;
    assert(run("AT+EP", &out) == XIAOTAI_OK && out.count == 2);
    assert(!strcmp(out.lines[1], "+EP-SAVED: saved=? next=?") && strstr(out.lines[0], "write_attempted=0"));
    atomic_store(&g_platform.unconfirmed, false);
    atomic_store(&g_platform.boot_error, XIAOTAI_ERR_NOT_FOUND);
    fake_platform_saved("", "");
    assert(run("AT+EP", &out) == XIAOTAI_OK && out.count == 2);
    assert(strstr(out.lines[0], "active=-") && strstr(out.lines[0], "boot_error=261"));
    assert(!strcmp(out.lines[1], "+EP-SAVED: saved=default next=-"));
    run_diagnostics(&out);
    assert(!strcmp(out.lines[0], "active=- phase=0 error=0 boot_error=261 cancel=0 write_attempted=0"));
    assert_saved_records(&out, true, false, "default", "-");

    char near[241], maximum[XIAOTAI_PLATFORM_BASE_MAX + 1U], next[XIAOTAI_PLATFORM_URL_MAX];
    make_base(near, sizeof(near));
    make_base(maximum, sizeof(maximum));
    const int canonical = snprintf(next, sizeof(next), "%s/services", near);
    assert(canonical > 0 && (size_t)canonical < sizeof(next));
    const char last = next[canonical - 1];
    next[canonical - 1] = '\0';
    fake_platform_saved(near, next);
    assert(run("AT+EP", &out) == XIAOTAI_OK && strlen(out.lines[1]) == 511);
    assert_saved_records(&out, false, false, near, next);
    next[canonical - 1] = last;
    fake_platform_saved(near, next);
    assert(run("AT+EP", &out) == XIAOTAI_OK && strlen(out.lines[1]) == 512);
    assert_saved_records(&out, false, false, near, next);
    strcat(next, "x");
    fake_platform_saved(near, next);
    assert(run("AT+EP", &out) == XIAOTAI_OK);
    assert_saved_records(&out, false, true, near, next);

    /* The diagnostic prefix consumes nine more characters than +EP-SAVED. */
    const size_t diag_boundary = (size_t)canonical - (sizeof("ENVIRONMENT") - sizeof("EP"));
    const char next_byte = next[diag_boundary];
    next[diag_boundary] = '\0';
    fake_platform_saved(near, next);
    run_diagnostics(&out);
    assert(strlen(out.lines[1]) == 512);
    assert_saved_records(&out, true, false, near, next);
    next[diag_boundary] = next_byte;
    next[diag_boundary + 1U] = '\0';
    fake_platform_saved(near, next);
    run_diagnostics(&out);
    assert_saved_records(&out, true, true, near, next);

    assert(snprintf(next, sizeof(next), "%s/services", maximum) == (int)sizeof(next) - 1);
    fake_platform_saved(maximum, next);
    const unsigned before_split = atomic_load(&g_platform.snapshots);
    out.flip_snapshot = true;
    assert(run("AT+EP", &out) == XIAOTAI_OK);
    assert_saved_records(&out, false, true, maximum, next);
    assert(atomic_load(&g_platform.snapshots) == before_split + 1U);
    assert(atomic_load(&g_platform.unconfirmed));
    out.flip_snapshot = false;
    assert(run("AT+EP", &out) == XIAOTAI_OK && out.count == 2 && !strcmp(out.lines[1], "+EP-SAVED: saved=? next=?"));
    run_diagnostics(&out);
    assert_saved_records(&out, true, false, "?", "?");
    atomic_store(&g_platform.unconfirmed, false);
    run_diagnostics(&out);
    assert_saved_records(&out, true, true, maximum, next);
    fake_platform_saved(NULL, NULL);
    atomic_store(&g_platform.boot_error, XIAOTAI_OK);
}

static void notice(unsigned n, const char *line)
{
    assert(xiaotai_signal_take(&notified, 3500));
    assert(atomic_load(&notice_count) == n + 1U && !strcmp(notices[n], line));
}

typedef struct { xiaotai_signal_t entered, release, drained; } queue_fence_t;

static void block_owner(void *ctx)
{
    queue_fence_t *fence = ctx;
    xiaotai_signal_give(&fence->entered);
    assert(xiaotai_signal_take(&fence->release, 2000));
}

static void empty_job(void *ctx) { (void)ctx; }
static void drain_tail(void *ctx) { xiaotai_signal_give(&((queue_fence_t *)ctx)->drained); }

static void test_full_maintenance_queue(void)
{
    const bool held = xiaotai_quiesced();
    const unsigned stores = atomic_load(&g_platform.stores), notices_before = atomic_load(&notice_count);
    xiaotai_environment_status_t before, after;
    xiaotai_environment_status(&before);
    queue_fence_t fence;
    assert(xiaotai_signal_init(&fence.entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&fence.release) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&fence.drained) == XIAOTAI_OK);
    assert(xiaotai_run(block_owner, &fence) == XIAOTAI_OK && xiaotai_signal_take(&fence.entered, 1000));
    for (unsigned n = 1; n < XIAOTAI_FRAMEWORK_QUEUE_DEPTH; ++n)
        assert(xiaotai_run(empty_job, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(drain_tail, &fence) == XIAOTAI_OK);

    /* The old runtime returned TIMEOUT for this full-queue refusal. The
     * shared AT owner reports NO_MEM before the environment receives it. */
    const char *command = held ? "AT+EP CANCEL\r" : "AT+EP SET http://new.test\r";
    const char *response = wire(command);
    fprintf(stderr, "full environment queue (%s): %s", held ? "cancel" : "begin", response);
    assert(!strcmp(response, "ERROR: NO_MEM\r\n"));
    xiaotai_signal_give(&fence.release);
    assert(xiaotai_signal_take(&fence.drained, 1000));
    sync_owner();
    xiaotai_environment_status(&after);
    assert(after.phase == before.phase && after.revision == before.revision);
    assert(!after.cancel_requested && !after.write_attempted && held == xiaotai_quiesced());
    assert(atomic_load(&g_platform.stores) == stores && atomic_load(&notice_count) == notices_before);
    xiaotai_signal_deinit(&fence.drained);
    xiaotai_signal_deinit(&fence.release);
    xiaotai_signal_deinit(&fence.entered);
}

static void test_switch(void)
{
    capture_t out = {0};
    const int error = 0x19f4;
    const char *bad[] = {"AT+EP SHOW", "AT+EP SET", "AT+EP SET http://good.test extra", "AT+EP SET ftp://bad.test",
        "AT+EP SET http://fixture-user:fixture-pass@host.test/pre", "AT+EP SET http://host.test/a?token=fixture-token",
        "AT+EP RESET extra", "AT+EP CANCEL extra"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(run(bad[i], &out) == XIAOTAI_ERR_INVALID_ARG && out.count == 0);
    assert(run("ep-show", &out) == XIAOTAI_ERR_NOT_FOUND);
    char over[XIAOTAI_PLATFORM_BASE_MAX + 22];
    memset(over, 'a', sizeof(over));
    memcpy(over, "AT+EP SET http://", strlen("AT+EP SET http://"));
    over[sizeof(over) - 1] = '\0';
    assert(run(over, &out) == XIAOTAI_ERR_INVALID_ARG);
    assert(run("AT+EP CANCEL", &out) == XIAOTAI_ERR_INVALID_STATE);
    assert(!atomic_load(&g_platform.stores));
    assert(run("AT+EP", &out) == XIAOTAI_OK);
    assert_saved_records(&out, false, false, "default", "http://builtin.example.test/services");
    sync_owner();
    assert(!atomic_load(&notice_count));
    atomic_store(&guard_result, error);
    const xiaotai_err_t refused = run("AT+EP SET http://new.test", &out);
    char label[24];
    assert(refused == error && !strcmp(xiaotai_at_error_name(refused, label, sizeof(label)), "0x19F4"));
    assert(out.count == 0 && run("AT+EP", &out) == XIAOTAI_OK);
    assert(strstr(out.lines[0], "error=6644") && strstr(out.lines[0], "write_attempted=0"));
    sync_owner();
    assert(!atomic_load(&notice_count) && !atomic_load(&g_platform.stores));

    atomic_store(&guard_result, XIAOTAI_OK);
    atomic_store(&g_platform.drained, false);
    test_full_maintenance_queue();
    char maximum[XIAOTAI_PLATFORM_BASE_MAX + 1U], command[XIAOTAI_PLATFORM_BASE_MAX + 11U];
    make_base(maximum, sizeof(maximum));
    assert(snprintf(command, sizeof(command), "AT+EP SET %s", maximum) == (int)sizeof(command) - 1);
    assert(run(command, &out) == XIAOTAI_OK && !out.count);
    assert(run("AT+EP RESET", &out) == XIAOTAI_ERR_INVALID_STATE);
    assert(run("AT+EP SET ftp://bad.test", &out) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_intent_post_external(XIAOTAI_INTENT_ENVIRONMENT_CANCEL, NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    test_full_maintenance_queue();
    assert(run("AT+EP CANCEL", &out) == XIAOTAI_OK && !out.count);
    notice(0, "+EP: result=cancelled error=0");
    assert(!atomic_load(&g_platform.stores));

    atomic_store(&g_platform.drained, true);
    atomic_store(&g_platform.store_call_result, XIAOTAI_ERR_NOT_FOUND);
    assert(run("AT+EP SET https://new.test/path///", &out) == XIAOTAI_OK && !out.count);
    notice(1, "+EP: result=failed error=261");
    assert(!strcmp(g_platform.stored, "http://new.test/path"));
    assert(run("AT+EP", &out) == XIAOTAI_OK && strstr(out.lines[0], "write_attempted=1"));
    assert(strstr(out.lines[0], "error=261") && !xiaotai_restart_pending());
    assert(!strcmp(out.lines[1], "+EP-SAVED: saved=default next=http://builtin.example.test/services"));

    atomic_store(&g_platform.store_call_result, XIAOTAI_OK);
    xiaotai_restart_hold(); /* Test never calls a hardware restart. */
    assert(run("AT+EP RESET", &out) == XIAOTAI_OK && !out.count);
    notice(2, "+EP: result=saved error=0");
    assert(atomic_load(&g_platform.store_was_null));
    sync_owner();
    assert(xiaotai_restart_pending());
    run_diagnostics(&out);
    assert(out.count >= 2 && strstr(out.lines[out.count - 1U], "refused_guard=1"));
}

static void begin_test_call(void *ctx)
{
    static const xiaotai_activity_t call = {.name = "call", .resources = XIAOTAI_RESOURCE_AUDIO_OUT};
    assert(xiaotai_activity_begin(&call, ctx) == XIAOTAI_OK);
}

static void end_test_call(void *ctx)
{
    const uint32_t ticket = *(const uint32_t *)ctx;
    assert(xiaotai_activity_current(NULL) == ticket);
    xiaotai_activity_end(ticket);
}

static void test_busy_base_precedence(void)
{
    static const char *bad = "AT+EP SET ftp://bad.test\r";
    (void)wire("AT\r");
    assert(!strcmp(wire("AT+CLI ON\r"), "OK\r\n"));
    assert(!strcmp(wire(bad), "ERROR: INVALID_ARG\r\n"));
    uint32_t call = 0;
    assert(xiaotai_run(begin_test_call, &call) == XIAOTAI_OK);
    sync_owner();
    assert(call && !strcmp(wire(bad), "ERROR: INVALID_ARG\r\n"));
    assert(xiaotai_run(end_test_call, &call) == XIAOTAI_OK);
    sync_owner();

    atomic_store(&g_net.held, true);
    fake_trace_clear();
    assert(!strcmp(wire(bad), "ERROR: INVALID_STATE\r\n"));
    assert(atomic_load(&g_net.held) && !xiaotai_quiesced());
    assert(!fake_trace_count("net_begin") && !fake_trace_count("net_end"));
    atomic_store(&g_net.held, false);

    atomic_store(&g_platform.drained, false);
    assert(!strcmp(wire("AT+EP SET http://new.test\r"), "OK\r\n"));
    xiaotai_environment_status_t before, after;
    xiaotai_environment_status(&before);
    fake_trace_clear();
    const char *result = wire(bad);
    fprintf(stderr, "already switching + bad address: %s", result);
    assert(!strcmp(result, "ERROR: INVALID_STATE\r\n"));
    xiaotai_environment_status(&after);
    assert(after.phase == before.phase && after.revision == before.revision);
    assert(atomic_load(&g_net.held) && xiaotai_quiesced());
    assert(!atomic_load(&g_platform.stores) && !fake_trace_count("net_begin") && !fake_trace_count("net_end"));
    assert(!strcmp(wire("AT+EP CANCEL\r"), "OK\r\n"));
    notice(0, "+EP: result=cancelled error=0");
}

int main(int argc, char **argv)
{
    const bool busy_base = argc == 2 && !strcmp(argv[1], "busy-base-first");
    boot(busy_base);
    if (busy_base) {
        test_busy_base_precedence();
    } else {
        test_registration();
        test_show();
        test_console();
        test_switch();
    }
    xiaotai_at_reset();
    xiaotai_framework_reset();
    xiaotai_environment_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&owner_done);
    xiaotai_signal_deinit(&notified);
    return 0;
}
