// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "at_test.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"

static xiaotai_signal_t started, progress, complete;
static atomic_bool input_ready;
static atomic_uint reader_starts;
static const char *input;
static char wire[384];
static size_t used;
static unsigned acks;
static bool released;
static enum { NO_SOURCE, EARLY_SOURCE, LATE_SOURCE } source_mode;

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);

xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (!strcmp(config->name, "at")) {
        assert(config->stack_bytes == 4096 && config->priority == 2 && config->stack == XIAOTAI_MEM_BULK);
        atomic_fetch_add(&reader_starts, 1U);
    }
    return __real_xiaotai_task_create(config, task);
}

static size_t read_source(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)ctx;
    size_t count = atomic_load(&input_ready) ? strlen(input) : 0;
    if (!count) {
        xiaotai_delay_ms(timeout_ms);
        return 0;
    }
    if (count > capacity) count = capacity;
    memcpy(data, input, count);
    input += count;
    return count;
}

static xiaotai_err_t write_source(void *ctx, const char *data, size_t size)
{
    (void)ctx;
    assert(!xiaotai_on_owner());
    assert(used + size < sizeof(wire));
    memcpy(wire + used, data, size);
    wire[used += size] = '\0';
    if (size == 4 && !memcmp(data, "OK\r\n", 4) && ++acks == 2) xiaotai_signal_give(&complete);
    return XIAOTAI_OK;
}

static const xiaotai_at_io_t io = {.name = "fixture", .read = read_source, .write = write_source};

static void owner_progress(void *ctx)
{
    (void)ctx;
    assert(xiaotai_framework_owner_on_caller());
    assert(xiaotai_at_notify("+NOTICE: owner=1") == XIAOTAI_OK);
    xiaotai_signal_give(&progress);
}

static bool next(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    /* Real streams may wait for a framework owner completion. A timer
       running the blocking console loop on the owner cannot service it. */
    assert(!xiaotai_on_owner());
    assert(xiaotai_run(owner_progress, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&progress, 1000));
    xiaotai_at_printf(reply, "+RESULT: owner_progress=1");
    return false;
}

static void release(void *ctx)
{
    (void)ctx;
    assert(!xiaotai_on_owner());
    assert(!released);
    released = true;
}

static xiaotai_err_t command(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    assert(xiaotai_framework_owner_on_caller());
    xiaotai_at_printf(reply, "+REQUEST: owner=1");
    return xiaotai_at_stream(reply, &(const xiaotai_at_stream_t){.next = next, .release = release});
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = xiaotai_engine_at_init();
    if (err == XIAOTAI_OK && source_mode == LATE_SOURCE) err = xiaotai_at_attach(&io);
    return err == XIAOTAI_OK ? xiaotai_command_register("AT+OWNER", "", command) : err;
}

static void start(void)
{
    xiaotai_engine_at_start();
    xiaotai_signal_give(&started);
}

static void *owner(void *ctx)
{
    assert(xiaotai_framework_run_on_caller(ctx) == XIAOTAI_OK);
    return NULL;
}

static void exercise(void)
{
    static const xiaotai_module_t module = {.name = "caller_at_boundary", .init = init, .start = start};
    const bool with_source = source_mode != NO_SOURCE;
    const int64_t allocations = xiaotai_mem_host_outstanding();
    const xiaotai_module_t *modules[] = {&module};
    xiaotai_framework_config_t config = {.modules = modules, .module_count = 1};
    atomic_store(&input_ready, false);
    atomic_store(&reader_starts, 0);
    used = acks = 0;
    wire[0] = '\0';
    released = false;
    CHECK(xiaotai_eventlog_init(&(const xiaotai_eventlog_config_t){
        .reader_stack_bytes = XIAOTAI_EVENTLOG_NO_READER,
    }) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&started) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&progress) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&complete) == XIAOTAI_OK);
    CHECK(xiaotai_at_configure(&(const xiaotai_at_config_t){
        .no_platform_console = true, .task_stack_bytes = XIAOTAI_AT_NO_TASK,
    }) == XIAOTAI_OK);
    input = "AT+OWNER\rAT\r";
    if (source_mode == EARLY_SOURCE) CHECK(xiaotai_at_attach(&io) == XIAOTAI_OK);
    CHECK(xiaotai_at_open() == XIAOTAI_OK);
    CHECK(atomic_load(&reader_starts) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, owner, &config) == 0);
    CHECK(xiaotai_signal_take(&started, 1000));
    CHECK(atomic_load(&reader_starts) == (with_source ? 1U : 0U));
    CHECK(xiaotai_at_service(20) == XIAOTAI_ERR_INVALID_STATE);
    if (with_source) {
        atomic_store(&input_ready, true);
        CHECK(xiaotai_signal_take(&complete, 3000));
    }
    xiaotai_at_reset();
    if (with_source) {
        CHECK(released && !*input);
        CHECK(!strcmp(wire, "+READY: at=1 source=fixture locked=0\r\n"
                            "+REQUEST: owner=1\r\n+RESULT: owner_progress=1\r\nOK\r\n"
                            "+NOTICE: owner=1\r\nOK\r\n"));
    } else CHECK(!used);
    xiaotai_framework_reset();
    assert(pthread_join(thread, NULL) == 0);
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&complete);
    xiaotai_signal_deinit(&progress);
    xiaotai_signal_deinit(&started);
    CHECK(xiaotai_mem_host_outstanding() == allocations);
}

int main(void)
{
    /* The boot-scoped KV worker survives framework reset. */
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    for (source_mode = NO_SOURCE; source_mode <= LATE_SOURCE; ++source_mode) exercise();
    return at_test_result("at_caller_owner");
}
