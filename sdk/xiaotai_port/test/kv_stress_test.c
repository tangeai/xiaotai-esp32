// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_native_nvs_barrier.py
// Replaces legacy: tests/regression/test_nvs_worker.py
/* The KV worker under load and at its edges, ported from the S3 storage
 * worker's regression test (tests/regression/test_nvs_worker.py): calls before
 * init, a start that fails at each allocation and leaves nothing behind,
 * argument checks, interrupt context refused, calls from the worker itself
 * refused, a timed-out queued job skipped and its slot not reused before the
 * worker dequeues it, a timed-out running job keeping the worker's own copy, a
 * full queue, three tasks writing 1500 times at once, and every slot all zero
 * bytes (names included) at the end.
 *
 * The worker's source is compiled into this test so the test can read its
 * slots and simulate interrupt context; the host backend and os port come
 * from the component's library. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kv_backend.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port_config.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_mem_host.h"

static bool s_isr;
static bool test_in_isr(void) { return s_isr; }
static bool s_enqueue_after_flush_snapshot;
static xiaotai_err_t s_after_snapshot_result;
static const xiaotai_task_t *s_boot_task;
static xiaotai_err_t s_boot_error;
static unsigned s_boot_calls;
static bool s_boot_on_worker, s_fail_task_create;

static xiaotai_err_t test_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (s_fail_task_create) return XIAOTAI_ERR_NO_MEM;
    assert(config->stack == XIAOTAI_MEM_INTERNAL && config->stack_bytes == XIAOTAI_KV_WORKER_STACK_BYTES);
    if (config->stack == XIAOTAI_MEM_INTERNAL) s_boot_task = task;
    return xiaotai_task_create(config, task);
}

static xiaotai_err_t test_backend_init(void)
{
    ++s_boot_calls;
    s_boot_on_worker = xiaotai_task_is_current(s_boot_task);
    return s_boot_error == XIAOTAI_OK ? xiaotai_kv_backend_init() : s_boot_error;
}

static void test_delay_ms(uint32_t ms)
{
    if (s_enqueue_after_flush_snapshot) {
        s_enqueue_after_flush_snapshot = false;
        s_after_snapshot_result = xiaotai_kv_set_int_async("barrier", "after", XIAOTAI_KV_U8, 99);
        xiaotai_kv_host_gate(true);
    }
    xiaotai_delay_ms(ms);
}

#define NATIVE_BLOB_SIZE ((size_t)UINT16_MAX + 30u)

static xiaotai_err_t test_backend_get(xiaotai_kv_handle_t handle, const char *key, bool text,
                                    void *data, size_t *size)
{
    if (!text && strcmp(key, "raw_large") == 0) {
        /* Native NVS returns the full stored length even when that value
         * exceeds the port's write limit. No oversized write goes through
         * the port or the in-memory backend's small entry buffers. */
        *size = NATIVE_BLOB_SIZE;
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    return xiaotai_kv_backend_get(handle, key, text, data, size);
}

#define xiaotai_in_isr test_in_isr
#define xiaotai_delay_ms test_delay_ms
#define xiaotai_task_create test_task_create
#define xiaotai_kv_backend_init test_backend_init
#define xiaotai_kv_backend_get test_backend_get
#include "kv_worker.c"
#undef xiaotai_kv_backend_get
#undef xiaotai_kv_backend_init
#undef xiaotai_task_create
#undef xiaotai_delay_ms
#undef xiaotai_in_isr

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

#define PRODUCERS 3U
#define WRITES 500U
#define LARGE_BYTES 1000U

typedef struct {
    int calls;
    xiaotai_err_t result, set_inside, async_inside, commit_inside, flush_inside;
} nested_t;

static void on_commit(xiaotai_err_t result, void *ctx)
{
    nested_t *nested = ctx;
    const uint8_t byte = 1;
    const xiaotai_kv_op_t op = {.kind = XIAOTAI_KV_OP_SET_U8, .key = "inner", .data = &byte};
    nested->set_inside = xiaotai_kv_set("nest", "inner", &byte, 1, 20);
    nested->async_inside = xiaotai_kv_set_async("nest", "inner", &byte, 1);
    nested->commit_inside = xiaotai_kv_commit_async("nest", &op, 1, NULL, NULL);
    nested->flush_inside = xiaotai_kv_flush(10);
    nested->result = result;
    ++nested->calls;
}

static void on_read(const xiaotai_kv_value_t *values, size_t count, void *ctx)
{
    (void)values;
    (void)count;
    ++*(int *)ctx;
}

static void pause_after_committed(xiaotai_err_t result, void *ctx)
{
    *(xiaotai_err_t *)ctx = result;
    xiaotai_kv_host_gate(false);
}

typedef struct {
    unsigned index;
    unsigned accepted, rejected;
    bool unexpected;
    xiaotai_signal_t finished;
    uint8_t value[LARGE_BYTES];
} producer_t;

/* Writes its own key WRITES times with synchronous calls; the last producer
 * writes values above the slot size, so large buffers come and go under load.
 * The submit lock waits only a few milliseconds and the slots are few: a
 * refused attempt (TIMEOUT or NO_MEM) is retried and never counted as a
 * write. */
static void producer(void *arg)
{
    producer_t *p = arg;
    char key[16];
    snprintf(key, sizeof(key), "t%u", p->index);
    const size_t size = p->index == PRODUCERS - 1 ? LARGE_BYTES : sizeof(uint32_t);
    for (unsigned attempt = 0; p->accepted < WRITES && attempt < 4 * WRITES; ++attempt) {
        const uint32_t value = p->accepted + 1;
        memcpy(p->value, &value, sizeof(value));
        const xiaotai_err_t err = xiaotai_kv_set("stress", key, p->value, size, 1000);
        if (err == XIAOTAI_OK) {
            ++p->accepted;
        } else if (err == XIAOTAI_ERR_TIMEOUT || err == XIAOTAI_ERR_NO_MEM) {
            ++p->rejected;
            xiaotai_delay_ms(1);
        } else {
            p->unexpected = true;
            break;
        }
    }
    xiaotai_signal_give(&p->finished);
}

int main(void)
{
    uint32_t value = 7, read = 0;
    size_t size = sizeof(read);
    int read_calls = 0;
    nested_t nested;
    memset(&nested, 0, sizeof(nested));
    const xiaotai_kv_op_t op = {.kind = XIAOTAI_KV_OP_SET, .key = "k", .data = &value, .size = 4};
    const xiaotai_kv_read_t one_read = {.kind = XIAOTAI_KV_READ_BLOB, .key = "k", .capacity = 4};

    /* Before init: nothing can be queued, and there is nothing to wait for. */
    CHECK(xiaotai_kv_set("ns", "k", &value, 4, 20) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_set_async("ns", "k", &value, 4) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_commit_async("ns", &op, 1, on_commit, &nested) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_read_async("ns", &one_read, 1, on_read, &read_calls) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_flush(10) == XIAOTAI_OK);
    xiaotai_kv_stats_t stats;
    xiaotai_kv_stats(&stats);
    CHECK(stats.submitted == 0 && stats.full == 0);

    /* The backend runs on the internal-stack worker, and an initialization
     * failure comes back unchanged to the caller. */
    s_boot_error = XIAOTAI_ERR_NOT_SUPPORTED;
    CHECK(xiaotai_kv_init() == s_boot_error);
    CHECK(s_boot_calls == 1 && s_boot_on_worker);
    CHECK(!s_ready && s_jobs == NULL && xiaotai_mem_host_outstanding() == 0);
    CHECK(xiaotai_kv_set_async("ns", "k", &value, 4) == XIAOTAI_ERR_INVALID_STATE);
    s_boot_error = XIAOTAI_OK;

    s_fail_task_create = true;
    CHECK(xiaotai_kv_init() == XIAOTAI_ERR_NO_MEM);
    CHECK(s_boot_calls == 1 && !s_ready && s_jobs == NULL && xiaotai_mem_host_outstanding() == 0);
    s_fail_task_create = false;

    /* A start that fails at any allocation returns NO_MEM, keeps nothing, and
     * a later start succeeds. */
    unsigned failed_steps = 0;
    for (uint32_t step = 1; step < 16; ++step) {
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(step);
        const xiaotai_err_t err = xiaotai_kv_init();
        const bool reached = !xiaotai_mem_host_fail_pending();
        xiaotai_mem_host_fail_nth(0);
        if (!reached) {
            CHECK(err == XIAOTAI_OK);
            break;
        }
        CHECK(err == XIAOTAI_ERR_NO_MEM && !s_ready && s_jobs == NULL);
        CHECK(xiaotai_mem_host_outstanding() == 0);
        ++failed_steps;
    }
    CHECK(failed_steps == 2 && s_ready); /* the slots, the queue's storage */
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    CHECK(s_boot_calls == 2 && s_boot_on_worker); /* no re-init once ready */
    /* From here every large job buffer must come back. */
    xiaotai_mem_host_clear();

    /* Arguments are checked before anything is queued. */
    static uint8_t big[XIAOTAI_KV_VALUE_MAX + 1];
    CHECK(xiaotai_kv_set("", "k", &value, 4, 20) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_set("ns", NULL, &value, 4, 20) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_set("ns", "k", NULL, 4, 20) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_set("ns", "k", big, sizeof(big), 20) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_kv_set("ns", "k", &value, 4, 0) == XIAOTAI_ERR_INVALID_ARG);

    /* Interrupt context: every call is refused, nothing is queued. */
    s_isr = true;
    CHECK(xiaotai_kv_set_async("ns", "k", &value, 4) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_set_int_async("ns", "k", XIAOTAI_KV_U8, 1) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_set("ns", "k", &value, 4, 20) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_get("ns", "k", &read, &size, 20) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_commit_async("ns", &op, 1, on_commit, &nested) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_read_async("ns", &one_read, 1, on_read, &read_calls) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_kv_flush(10) == XIAOTAI_ERR_INVALID_STATE);
    s_isr = false;
    xiaotai_kv_stats(&stats);
    CHECK(stats.submitted == 0);

    /* A write and its read; a storage failure reaches the caller. */
    CHECK(xiaotai_kv_set("ns", "k", &value, 4, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_get("ns", "k", &read, &size, 1000) == XIAOTAI_OK && read == 7 && size == 4);
    CHECK(xiaotai_kv_get("ns", "raw_large", &read, &size, 1000) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(size == NATIVE_BLOB_SIZE && read == 7);
    xiaotai_kv_host_fail_key("fails", XIAOTAI_FAIL);
    value = 99;
    CHECK(xiaotai_kv_set("ns", "fails", &value, 4, 1000) == XIAOTAI_FAIL);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);

    /* On the worker itself (a completion) every call that would queue or wait
     * is refused. */
    CHECK(xiaotai_kv_commit_async("ns", &op, 1, on_commit, &nested) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(nested.calls == 1 && nested.result == XIAOTAI_OK);
    CHECK(nested.set_inside == XIAOTAI_ERR_INVALID_STATE && nested.async_inside == XIAOTAI_ERR_INVALID_STATE);
    CHECK(nested.commit_inside == XIAOTAI_ERR_INVALID_STATE && nested.flush_inside == XIAOTAI_ERR_INVALID_STATE);
    uint8_t byte = 0;
    size = 1;
    CHECK(xiaotai_kv_get("nest", "inner", &byte, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);

    /* A synchronous call that times out while queued is skipped, and its slot
     * is not reused before the worker dequeues it. */
    xiaotai_kv_host_counts_t counts_before, counts_after;
    xiaotai_kv_host_counts(&counts_before);
    xiaotai_kv_host_gate(false);
    const uint32_t ten = 10, twenty = 20, twenty_one = 21, other = 999, seven = 7;
    CHECK(xiaotai_kv_set_async("q", "run", &ten, 4) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(1000));
    CHECK(xiaotai_kv_set_async("q", "latest", &twenty, 4) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_async("q", "latest", &twenty_one, 4) == XIAOTAI_OK); /* coalesced */
    CHECK(xiaotai_kv_set_async("q", "other", &other, 4) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set("q", "skip", &seven, 4, 20) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_kv_set_async("q", "more", &seven, 4) == XIAOTAI_ERR_NO_MEM);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_host_counts(&counts_after);
    CHECK(counts_after.writes == counts_before.writes + 3); /* run, latest, other */
    size = sizeof(read);
    CHECK(xiaotai_kv_get("q", "skip", &read, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get("q", "run", &read, &size, 1000) == XIAOTAI_OK && read == 10);
    CHECK(xiaotai_kv_get("q", "latest", &read, &size, 1000) == XIAOTAI_OK && read == 21);
    CHECK(xiaotai_kv_get("q", "other", &read, &size, 1000) == XIAOTAI_OK && read == 999);
    CHECK(xiaotai_kv_set_async("q", "more", &seven, 4) == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);

    /* A synchronous call that times out while its write runs: the worker keeps
     * its own copy, so the caller may reuse its buffer at once. */
    uint32_t mine = 42;
    xiaotai_kv_host_gate(false);
    CHECK(xiaotai_kv_set("r", "k", &mine, 4, 20) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_kv_host_wait_held(1000));
    mine = 888;
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_get("r", "k", &read, &size, 1000) == XIAOTAI_OK && read == 42);
    value = 17;
    CHECK(xiaotai_kv_set("r", "k", &value, 4, 1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_get("r", "k", &read, &size, 1000) == XIAOTAI_OK && read == 17);

    /* The V3 equivalent of the P4 FIFO barrier: a mandatory async commit
     * survives the waiter's timeout. Later work stays outside its marker. */
    xiaotai_kv_host_gate(false);
    xiaotai_err_t completion = XIAOTAI_FAIL;
    const xiaotai_kv_op_t mandatory = {.kind = XIAOTAI_KV_OP_SET_INT, .key = "before",
                                       .int_type = XIAOTAI_KV_U8, .number = 42};
    CHECK(xiaotai_kv_commit_async("barrier", &mandatory, 1, pause_after_committed, &completion) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(1000));
    CHECK(xiaotai_kv_flush(20) == XIAOTAI_ERR_TIMEOUT);
    s_enqueue_after_flush_snapshot = true;
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(!s_enqueue_after_flush_snapshot && s_after_snapshot_result == XIAOTAI_OK && completion == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(0) == XIAOTAI_ERR_TIMEOUT); /* The later write still has its own marker. */
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    CHECK(xiaotai_kv_get_u8("barrier", "before", &byte, 1000) == XIAOTAI_OK && byte == 42);
    CHECK(xiaotai_kv_get_u8("barrier", "after", &byte, 1000) == XIAOTAI_OK && byte == 99);

    /* Three tasks write 1500 times at once. */
    xiaotai_kv_stats_t before;
    xiaotai_kv_stats(&before);
    static producer_t producers[PRODUCERS];
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        producers[i].index = i;
        CHECK(xiaotai_signal_init(&producers[i].finished) == XIAOTAI_OK);
        const xiaotai_task_config_t task = {
            .name = "kv_producer", .entry = producer, .arg = &producers[i], .stack_bytes = 8192,
            .stack = XIAOTAI_MEM_INTERNAL, .priority = 2, .core = XIAOTAI_CORE_ANY,
        };
        xiaotai_task_t handle;
        CHECK(xiaotai_task_create(&task, &handle) == XIAOTAI_OK);
    }
    unsigned accepted = 0, rejected = 0;
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        CHECK(xiaotai_signal_take(&producers[i].finished, 60000));
        CHECK(!producers[i].unexpected && producers[i].accepted == WRITES);
        accepted += producers[i].accepted;
        rejected += producers[i].rejected;
    }
    if (rejected) fprintf(stderr, "producers completed=%u rejected=%u\n", accepted, rejected);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_stats(&stats);
    /* Every accepted write ran; a call that timed out after queueing also ran. */
    CHECK(stats.submitted - before.submitted == accepted + (stats.timeouts - before.timeouts));
    static uint8_t large[LARGE_BYTES];
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "t%u", i);
        size = sizeof(large);
        CHECK(xiaotai_kv_get("stress", key, large, &size, 1000) == XIAOTAI_OK);
        memcpy(&read, large, sizeof(read));
        CHECK(read == WRITES && size == (i == PRODUCERS - 1 ? LARGE_BYTES : sizeof(uint32_t)));
    }

    /* The totals of the whole run, then every slot is all zero bytes (values,
     * namespace and key names, completion) and every large buffer is back. */
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    xiaotai_kv_stats(&stats);
    CHECK(stats.coalesced == 1 && stats.timeouts >= 2 && stats.full >= 1 && stats.failures == 1);
    CHECK(read_calls == 0);
    xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
    for (unsigned i = 0; i < KV_SLOTS; ++i) {
        const uint8_t *bytes = (const uint8_t *)&s_jobs[i];
        for (size_t j = 0; j < sizeof(s_jobs[i]); ++j) CHECK(bytes[j] == 0);
    }
    xiaotai_mutex_unlock(&s_mutex);
    CHECK(xiaotai_mem_host_outstanding() == 0);

    printf("PASS: kv stress: before init, failed starts, arguments, interrupts, calls on the worker, queued and "
           "running timeouts, full queue, %u concurrent writes, wiped slots\n", accepted);
    return 0;
}
