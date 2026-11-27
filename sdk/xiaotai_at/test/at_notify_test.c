// SPDX-License-Identifier: MIT
#include <pthread.h>
#include "internal.h"
#include "at_long_fixture.h"
#include "xiaotai_mem_host.h"

static long_sink_t left, right;
static const xiaotai_at_io_t a = {"left", long_read, long_write, long_bulk, &left};
static const xiaotai_at_io_t b = {"right", long_read, long_write, long_bulk, &right};
static char message[XIAOTAI_AT_NOTIFY_MAX + 2];
static unsigned next_calls;

static bool next(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    CHECK(++next_calls <= 2);
    xiaotai_at_printf(reply, "+WORK: data=%u", next_calls);
    CHECK(xiaotai_at_notifyf("+NOTICE: value=%u", next_calls) == XIAOTAI_OK);
    CHECK(!strstr(left.text, "+NOTICE:"));
    return next_calls < 2;
}

static xiaotai_err_t command(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_at_printf(reply, "+GO: accepted=1");
    return xiaotai_at_stream(reply, &(xiaotai_at_stream_t){.next = next});
}

static xiaotai_err_t prepare(uint16_t capacity)
{
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){.task_stack_bytes = XIAOTAI_AT_NO_TASK,
                                                      .notify_bytes = capacity, .no_platform_console = true}) == XIAOTAI_OK);
    CHECK(xiaotai_at_local_register("AT+GO", NULL, command) == XIAOTAI_OK);
    CHECK(xiaotai_at_attach(&a) == XIAOTAI_OK);
    CHECK(xiaotai_at_attach(&b) == XIAOTAI_OK);
    return xiaotai_at_open();
}

static void reset_text(long_sink_t *sink) { sink->used = 0; sink->text[0] = '\0'; }

typedef struct { const char *line; xiaotai_err_t result; } waiter_t;

static void *wait_for_wire(void *ctx)
{
    waiter_t *waiter = ctx;
    waiter->result = xiaotai_at_notify_wait(waiter->line, 500);
    return NULL;
}

static void check_receipt(bool fail)
{
    waiter_t waiter = {.line = "+RECEIPT: accepted=1", .result = XIAOTAI_ERR_BUSY};
    pthread_t thread;
    if (fail) right.fail_prefix = "+RECEIPT:";
    CHECK(pthread_create(&thread, NULL, wait_for_wire, &waiter) == 0);
    /* Wait for queue admission (not for the writer), then drive real console
     * service. Previous timed-out queued records cannot satisfy this waiter. */
    for (unsigned i = 0; !at_notify_queued() && i < 100; ++i) xiaotai_delay_ms(1);
    CHECK(at_notify_queued() > 0);
    CHECK(xiaotai_at_notify_wait("+BUSY: retry=1", 1) == XIAOTAI_ERR_BUSY);
    CHECK(xiaotai_at_service(2) == XIAOTAI_OK);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.result == (fail ? XIAOTAI_FAIL : XIAOTAI_OK));
    CHECK(strstr(left.text, "+RECEIPT: accepted=1\r\n"));
    CHECK((strstr(right.text, "+RECEIPT: accepted=1\r\n") != NULL) == !fail);
}

int main(void)
{
    const int64_t allocations = xiaotai_mem_host_outstanding();
    CHECK(xiaotai_at_notify("+NOTICE: data=0") == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){.no_platform_console = true}) == XIAOTAI_OK);
    CHECK(xiaotai_at_open() == XIAOTAI_OK);
    CHECK(xiaotai_at_notifyf("+NOTICE: data=%d", 0) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_at_reset();
    CHECK(prepare(4096) == XIAOTAI_OK);
    /* One read contains two commands; no notice may appear inside a reply
     * or be delayed until after the next command in the same buffer. */
    long_input(&left, "AT+GO\rAT\r");
    CHECK(!strcmp(left.text, "+READY: at=1 source=left locked=0\r\n+GO: accepted=1\r\n"
                 "+WORK: data=1\r\n+WORK: data=2\r\nOK\r\n+NOTICE: value=1\r\n+NOTICE: value=2\r\nOK\r\n"));
    CHECK(!strcmp(right.text, "+READY: at=1 source=right locked=0\r\n+NOTICE: value=1\r\n+NOTICE: value=2\r\n"));
    CHECK(xiaotai_at_notify("OK") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_notify("+NOTICE: data=0\r\nOK") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_notify("+NOTICE") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_at_notifyf("+NOTICE: data=%cerror", 0) == XIAOTAI_ERR_INVALID_ARG);

    memcpy(message, "+NOTICE: text=", sizeof("+NOTICE: text=") - 1);
    memset(message + sizeof("+NOTICE: text=") - 1, 'A', sizeof(message) - sizeof("+NOTICE: text=") + 1);
    message[XIAOTAI_AT_NOTIFY_MAX + 1] = '\0';
    CHECK(xiaotai_at_notify(message) == XIAOTAI_ERR_INVALID_SIZE);
    message[XIAOTAI_AT_NOTIFY_MAX] = '\0';
    for (unsigned i = 0; i < 7; ++i) {
        message[XIAOTAI_AT_NOTIFY_MAX - 1] = (char)('0' + i);
        CHECK(xiaotai_at_notify(message) == XIAOTAI_OK);
    }
    CHECK(xiaotai_at_notify(message) == XIAOTAI_ERR_NO_MEM);
    reset_text(&left);
    reset_text(&right);
    CHECK(xiaotai_at_service(2) == XIAOTAI_OK);
    CHECK(left.used == 7 * (XIAOTAI_AT_NOTIFY_MAX + 2));
    CHECK(!strcmp(left.text, right.text));
    for (unsigned i = 0; i < 7; ++i) {
        const size_t offset = (size_t)i * (XIAOTAI_AT_NOTIFY_MAX + 2);
        CHECK(left.text[offset + XIAOTAI_AT_NOTIFY_MAX - 1] == '0' + (int)i);
    }
    /* The consumer cleared the records but the underlying indices stayed
     * near the end. Another maximum-sized record wraps in the byte ring. */
    CHECK(xiaotai_at_notify(message) == XIAOTAI_OK);
    CHECK(xiaotai_at_service(2) == XIAOTAI_OK);
    CHECK(left.used == 8 * (XIAOTAI_AT_NOTIFY_MAX + 2));
    CHECK(!memcmp(left.text + left.used - sizeof(message), message, XIAOTAI_AT_NOTIFY_MAX));
    xiaotai_at_stats_t stats;
    xiaotai_at_stats(&stats);
    CHECK(stats.notify_sent == 10 && stats.notify_dropped == 2);
    reset_text(&left);
    reset_text(&right);
    CHECK(xiaotai_at_notify_wait("+OLD: timed_out=1", 2) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_at_service(2) == XIAOTAI_OK);
    CHECK(!strcmp(left.text, "+OLD: timed_out=1\r\n"));
    check_receipt(false);
    reset_text(&left);
    reset_text(&right);
    check_receipt(true);
    xiaotai_at_reset();

    left = right = (long_sink_t){0};
    CHECK(prepare(258) == XIAOTAI_OK);
    CHECK(xiaotai_at_notify(message) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_at_notify("+NOTICE: data=1") == XIAOTAI_OK);
    xiaotai_at_stats(&stats);
    CHECK(stats.notify_dropped == 1);
    CHECK(xiaotai_at_service(2) == XIAOTAI_OK);
    CHECK(!strcmp(left.text, "+READY: at=1 source=left locked=0\r\n+NOTICE: data=1\r\n"));
    xiaotai_at_reset();
    CHECK(xiaotai_mem_host_outstanding() == allocations);
    return at_test_result("at_notify");
}
