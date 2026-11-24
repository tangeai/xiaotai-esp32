// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_at_diagnostics.py
#include "at_long_fixture.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/kv.h"

typedef enum { BLOB, INLINE, SLOW, STALL, REJECT, NEXT_FAIL } work_mode_t;
typedef struct { work_mode_t mode; unsigned next, released; } work_t;

static long_sink_t sink;
static const xiaotai_at_io_t io = {"capture", long_read, long_write, long_bulk, &sink};
static work_t work;
static uint8_t payload[259];
static xiaotai_reply_t retired;
static unsigned rejected_release;

static void release(void *ctx)
{
    work_t *s = ctx;
    CHECK(!xiaotai_on_owner());
    CHECK(++s->released == 1);
    CHECK(sink.bulk == (s->mode != REJECT));
    if (s->mode == INLINE) CHECK(strstr(sink.text, "+DUMP-END: chunks=3 fnv="));
    CHECK(!strstr(sink.text, "\r\nOK\r\n"));
}

static void rejected(void *ctx) { (void)ctx; ++rejected_release; }

static bool next(void *ctx, const xiaotai_reply_t *reply)
{
    work_t *s = ctx;
    CHECK(!xiaotai_on_owner() && sink.bulk);
    ++s->next;
    if (s->mode == INLINE) {
        xiaotai_os_host_advance_us(1000);
        CHECK(xiaotai_at_blob_write(reply, "DUMP", "format=RAW", payload, sizeof(payload)) == XIAOTAI_OK);
        return false;
    }
    if (s->mode == NEXT_FAIL) {
        (void)xiaotai_at_fail(reply, XIAOTAI_ERR_NOT_FOUND, "FIXTURE");
        return false;
    }
    if (s->next == 1) retired = *reply;
    else if (s->next == 2) {
        CHECK(xiaotai_at_blob_write(&retired, "DUMP", "", payload, sizeof(payload)) == XIAOTAI_ERR_NOT_SUPPORTED);
        retired.write(retired.ctx, "+OLD: generation=1");
        if (s->mode == STALL) xiaotai_os_host_advance_us(51000);
    }
    xiaotai_at_printf(reply, "+STEP: n=%u", s->next);
    CHECK(xiaotai_at_stream_result(reply) == (s->mode == STALL && s->next == 2 ?
                                             XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK));
    return s->next < 9;
}

static xiaotai_err_t command(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    CHECK(xiaotai_on_owner() && argc == 1);
    if (argc != 1) return XIAOTAI_ERR_INVALID_ARG;
    work = (work_t){0};
    if (!strcmp(argv[0], "INLINE")) work.mode = INLINE;
    if (!strcmp(argv[0], "SLOW")) work.mode = SLOW;
    if (!strcmp(argv[0], "STALL")) work.mode = STALL;
    if (!strcmp(argv[0], "REJECT")) work.mode = REJECT;
    if (!strcmp(argv[0], "NEXTFAIL")) work.mode = NEXT_FAIL;
    CHECK(xiaotai_at_blob_write(reply, "DUMP", "", payload, sizeof(payload)) == XIAOTAI_ERR_NOT_SUPPORTED);
    if (work.mode == BLOB || work.mode == REJECT) {
        char header[] = "format=RAW";
        const xiaotai_at_blob_t blob = {"DUMP", header, payload, sizeof(payload), release, &work};
        CHECK(xiaotai_at_blob(reply, &blob) == XIAOTAI_OK);
        memset(header, 'x', sizeof(header));
        CHECK(xiaotai_at_stream(reply, &(xiaotai_at_stream_t){next, rejected, NULL}) == XIAOTAI_ERR_INVALID_STATE);
    } else {
        CHECK(xiaotai_at_stream(reply, &(xiaotai_at_stream_t){next, release, &work}) == XIAOTAI_OK);
    }
    return work.mode == REJECT ? xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "REJECTED") : XIAOTAI_OK;
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = xiaotai_eventlog_init(NULL);
    if (err == XIAOTAI_OK) err = xiaotai_engine_at_init();
    if (err != XIAOTAI_OK) return err;
    static const xiaotai_at_verb_t verbs[] = {{.min_args = 1, .max_args = 1, .run = command}};
    return xiaotai_at_verbs_register("AT+DUMP", verbs, sizeof(verbs) / sizeof(verbs[0]));
}

static void validate_reconstruction(void)
{
    char end[96];
    /* FNV calculated from the fixed payload by an independent reference. */
    (void)snprintf(end, sizeof(end), "+DUMP-END: chunks=3 fnv=%s\r\nOK\r\n", "99334328");
    const char *line = strstr(sink.text, "+DUMP-DATA:");
    size_t restored = 0;
    for (unsigned seq = 0; seq < 3 && line; ++seq) {
        char prefix[48];
        (void)snprintf(prefix, sizeof(prefix), "+DUMP-DATA: seq=%u hex=", seq);
        CHECK(!strncmp(line, prefix, strlen(prefix)));
        const char *first = line + strlen(prefix);
        const char *last = strstr(first, "\r\n");
        CHECK(last);
        if (!last) return;
        const size_t text = (size_t)(last - first);
        CHECK(text == (seq < 2 ? 256U : 6U));
        if (text > 256) return;
        char hex[257];
        memcpy(hex, first, text);
        hex[text] = '\0';
        uint8_t chunk[128];
        size_t bytes = 0;
        CHECK(xiaotai_at_hex_decode(hex, chunk, sizeof(chunk), &bytes) == XIAOTAI_OK);
        CHECK(bytes <= sizeof(payload) - restored);
        if (bytes > sizeof(payload) - restored) return;
        CHECK(!memcmp(payload + restored, chunk, bytes));
        restored += bytes;
        line = last + 2;
    }
    CHECK(restored == sizeof(payload));
    CHECK(line && !strcmp(line, end));
}

int main(void)
{
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)i;
    const xiaotai_at_blob_t blob = {"DUMP", "", payload, sizeof(payload), rejected, NULL};
    at_test_sink_t outside;
    const xiaotai_reply_t direct = at_test_reply(&outside);
    CHECK(xiaotai_at_stream_result(&direct) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_at_blob(&direct, &blob) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_at_stream(&direct, &(xiaotai_at_stream_t){next, rejected, NULL}) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_at_blob(&direct, &(xiaotai_at_blob_t){"bad", "", payload, 1, NULL, NULL}) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_blob(&direct, &(xiaotai_at_blob_t){"DUMP", "header=bad\r", payload, 1, NULL, NULL}) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){.task_stack_bytes = XIAOTAI_AT_NO_TASK,
                                                      .stream_timeout_ms = 50, .no_platform_console = true}) == XIAOTAI_OK);
    CHECK(xiaotai_at_attach(&io) == XIAOTAI_OK);
    const xiaotai_module_t owner = {.name = "fixture", .init = init, .start = xiaotai_engine_at_start};
    const xiaotai_module_t *mods[] = {&owner};
    CHECK(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = mods, .module_count = 1}) == XIAOTAI_OK);
    CHECK(xiaotai_at_service(2) == XIAOTAI_OK);

    long_input(&sink, "AT+DUMP BLOB\r");
    CHECK(strstr(sink.text, "+DUMP: format=RAW bytes=259 fnv=99334328\r\n") == sink.text);
    validate_reconstruction();
    CHECK(work.released == 1 && sink.begins == 1 && sink.ends == 1);
    char first[1024];
    CHECK(sink.used < sizeof(first));
    if (sink.used >= sizeof(first)) return at_test_result("at_stream");
    memcpy(first, sink.text, sink.used + 1);
    long_input(&sink, "AT+DUMP INLINE\r");
    CHECK(!strcmp(first, sink.text) && work.released == 1 && work.next == 1);
    long_input(&sink, "AT+DUMP REJECT\r");
    CHECK(!strcmp(sink.text, "ERROR: INVALID_ARG REJECTED\r\n") && work.released == 1);
    CHECK(sink.begins == 2 && sink.ends == 2 && !rejected_release);
    sink.fail_prefix = "+DUMP-DATA:";
    long_input(&sink, "AT+DUMP BLOB\r");
    CHECK(strstr(sink.text, "ERROR: FAIL OUTPUT\r\n") && !strstr(sink.text, "+DUMP-END:") && work.released == 1);
    CHECK(!strstr(sink.text, "\r\nOK\r\n") && !sink.bulk && sink.begins == sink.ends);
    long_input(&sink, "AT+DUMP NEXTFAIL\r");
    CHECK(!strcmp(sink.text, "ERROR: NOT_FOUND FIXTURE\r\n") && work.released == 1);

    sink.write_us = 17000;
    long_input(&sink, "AT+DUMP SLOW\r");
    CHECK(work.released == 1 && work.next == 9 && strstr(sink.text, "+STEP: n=9\r\nOK\r\n"));
    CHECK(!strstr(sink.text, "+OLD:"));
    sink.write_us = 0;
    long_input(&sink, "AT+DUMP STALL\r");
    CHECK(work.released == 1 && work.next == 2 && !strcmp(sink.text, "+STEP: n=1\r\nERROR: TIMEOUT STREAM\r\n"));
    CHECK(!sink.bulk && sink.begins == 6 && sink.ends == 6);
    xiaotai_at_stats_t stats;
    xiaotai_at_stats(&stats);
    CHECK(stats.streams == 3 && stats.commands_failed == 4 && stats.write_errors == 1);
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    return at_test_result("at_stream");
}
