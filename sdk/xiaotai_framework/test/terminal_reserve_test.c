// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_xmini_ai_token_http.py
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"

enum { DEPTH = 8, WAIT_MS = 2000, PRODUCERS = 4, GENERATION = 73 };
static const char event_name[] = "test.terminal";
static xiaotai_signal_t parked, post_terminal, posted_terminal, release;
static atomic_uint admitted, jobs, observed;
static _Thread_local bool refuse_native;
static bool send_first;

bool __real_xiaotai_queue_send(xiaotai_queue_t *, const void *, uint32_t);
bool __wrap_xiaotai_queue_send(xiaotai_queue_t *queue, const void *message, uint32_t timeout)
{
    if (refuse_native) {
        assert(timeout == 0);
        return false;
    }
    return __real_xiaotai_queue_send(queue, message, timeout);
}

static void observe(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && event->args.value == GENERATION && event->size == sizeof(uint32_t));
    assert(*(const uint32_t *)event->data == GENERATION && atomic_load(&observed) == 0);
    assert(atomic_load(&jobs) == (send_first ? 0U : DEPTH - 1U));
    atomic_fetch_add(&observed, 1U);
}

static xiaotai_err_t init(void) { return xiaotai_event_subscribe(event_name, observe, NULL); }

static void job(void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    atomic_fetch_add(&jobs, 1U);
}

static void full_owner(void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    xiaotai_signal_give(&parked);
    assert(xiaotai_signal_take(&post_terminal, WAIT_MS));
    uint32_t value = GENERATION;
    xiaotai_args_t args = {.value = GENERATION};
    refuse_native = true;
    assert(xiaotai_event_emit_owner_terminal(event_name, &args, &value, sizeof(value)) == XIAOTAI_ERR_NO_MEM);
    refuse_native = false;
    assert(xiaotai_event_emit_owner_terminal(event_name, &args, &value, sizeof(value)) == XIAOTAI_OK);
    args.value = 82;
    value = 82;
    assert(xiaotai_event_emit_owner_terminal(event_name, &args, &value, sizeof(value)) == XIAOTAI_ERR_NO_MEM);
    xiaotai_signal_give(&posted_terminal);
    assert(xiaotai_signal_take(&release, WAIT_MS));
}

static void *producer(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < DEPTH; ++i) {
        const xiaotai_err_t err = xiaotai_run(job, NULL);
        assert(err == XIAOTAI_OK || err == XIAOTAI_ERR_NO_MEM);
        if (err == XIAOTAI_OK) atomic_fetch_add(&admitted, 1U);
    }
    return NULL;
}

static void fill(void)
{
    pthread_t threads[PRODUCERS];
    for (unsigned i = 0; i < PRODUCERS; ++i) assert(!pthread_create(&threads[i], NULL, producer, NULL));
    for (unsigned i = 0; i < PRODUCERS; ++i) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&admitted) == DEPTH - 1U && atomic_load(&jobs) == 0);
}

static void fence(void *ctx) { xiaotai_signal_give(ctx); }

static void exercise(bool terminal_first)
{
    atomic_store(&admitted, 0);
    atomic_store(&jobs, 0);
    atomic_store(&observed, 0);
    send_first = terminal_first;
    const int64_t idle = xiaotai_mem_host_outstanding();
    assert(xiaotai_run(full_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&parked, WAIT_MS));
    if (!terminal_first) fill();
    assert(xiaotai_event_emit_owner_terminal(event_name, NULL, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_give(&post_terminal);
    assert(xiaotai_signal_take(&posted_terminal, WAIT_MS));
    if (terminal_first) fill();
    assert(xiaotai_run(job, NULL) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_mem_host_outstanding() == idle + 1);
    xiaotai_signal_give(&release);
    xiaotai_err_t posted = XIAOTAI_ERR_NO_MEM;
    for (unsigned i = 0; i < WAIT_MS && posted == XIAOTAI_ERR_NO_MEM; ++i) {
        posted = xiaotai_run(fence, &parked);
        if (posted == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1);
    }
    assert(posted == XIAOTAI_OK && xiaotai_signal_take(&parked, WAIT_MS));
    assert(atomic_load(&observed) == 1U && atomic_load(&jobs) == DEPTH - 1U);
    assert(xiaotai_mem_host_outstanding() == idle);
}

int main(void)
{
    static const xiaotai_module_t module = {.name = "terminal_test", .init = init};
    const xiaotai_module_t *const modules[] = {&module};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1U,
        .queue_depth = DEPTH, .reserve_owner_terminal = true};
    xiaotai_signal_t *const signals[] = {&parked, &post_terminal, &posted_terminal, &release};
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    for (unsigned i = 0; i < sizeof(signals) / sizeof(*signals); ++i)
        assert(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    exercise(false);
    exercise(true);
    xiaotai_framework_reset();
    for (unsigned i = 0; i < sizeof(signals) / sizeof(*signals); ++i) xiaotai_signal_deinit(signals[i]);
    printf("%u ordinary/1 terminal; FIFO, concurrent admission, rollback, and payload ownership verified\n", DEPTH - 1U);
    return 0;
}
