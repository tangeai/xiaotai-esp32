// SPDX-License-Identifier: MIT
/* Storage barriers of the environment module, on a short-lived task.
 *
 * Both flushes wait (for the storage worker, for queued setting writes), and
 * the owner task never waits, so each barrier runs on a task of its own that
 * exists only while it runs. The task touches no storage itself: the flash
 * work stays on the storage port's worker, so its stack may live in external
 * RAM (BULK). It runs below the owner task so it never delays a handler.
 *
 * Hand-over without a lock: the owner writes s_request before it creates the
 * task (task creation orders the two) and starts the next barrier only after
 * it consumed the previous result, so s_request and s_result are never
 * written by two parties at once. */
#include <stdatomic.h>
#include <stddef.h>
#include "environment_internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"
#include "xiaotai/preferences.h"

typedef struct {
    environment_barrier_kind_t kind;
    uint32_t timeout_ms;
    uint32_t sequence;
    xiaotai_err_t result;
} barrier_job_t;

static barrier_job_t s_request;
static barrier_job_t s_result;
static atomic_uint s_tasks;      /* barrier tasks that have not returned yet */
static xiaotai_task_t s_task;

/* Retry pause when the owner's queue is full; the result must arrive. */
#define BARRIER_POST_RETRY_MS 10U
/* Longest wait of environment_barrier_wait_idle (host reset). */
#define BARRIER_IDLE_WAIT_MS 15000U

static void deliver(void *ctx)
{
    (void)ctx;
    const barrier_job_t job = s_result;
    environment_barrier_done(job.kind, job.sequence, job.result);
}

static uint32_t elapsed_ms(int64_t since_us)
{
    const int64_t elapsed = (xiaotai_time_us() - since_us) / 1000;
    return elapsed <= 0 ? 0U : elapsed >= (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void barrier_task(void *arg)
{
    (void)arg;
    barrier_job_t job = s_request;
    const int64_t started = xiaotai_time_us();
    if (job.kind == ENVIRONMENT_BARRIER_BEFORE_WRITE) {
        /* Settings first: their flush queues the last accepted writes, and
         * the key-value flush that follows waits for those and every other
         * job queued before it. */
        job.result = xiaotai_preferences_maintenance_flush(job.timeout_ms);
        if (job.result == XIAOTAI_OK) {
            const uint32_t spent = elapsed_ms(started);
            job.result = spent >= job.timeout_ms ? XIAOTAI_ERR_TIMEOUT : xiaotai_kv_flush(job.timeout_ms - spent);
        }
    } else {
        job.result = xiaotai_kv_flush(job.timeout_ms);
    }
    s_result = job;
    xiaotai_err_t err;
    while ((err = xiaotai_run(deliver, NULL)) == XIAOTAI_ERR_NO_MEM) {
        xiaotai_delay_ms(BARRIER_POST_RETRY_MS);
    }
    if (err != XIAOTAI_OK) XIAOTAI_LOGW(ENVIRONMENT_TAG, "barrier result not delivered: 0x%x", (unsigned)err);
    atomic_fetch_sub(&s_tasks, 1U);
    xiaotai_task_exit();
}

xiaotai_err_t environment_barrier_start(environment_barrier_kind_t kind, uint32_t timeout_ms, uint32_t sequence)
{
    s_request = (barrier_job_t){.kind = kind, .timeout_ms = timeout_ms, .sequence = sequence};
    const unsigned owner = XIAOTAI_FRAMEWORK_OWNER_PRIORITY;
    const xiaotai_task_config_t config = {
        .name = ENVIRONMENT_BARRIER_TASK_NAME,
        .entry = barrier_task,
        .arg = NULL,
        .stack_bytes = ENVIRONMENT_BARRIER_STACK_BYTES,
        .stack = XIAOTAI_MEM_BULK,
        .priority = owner > 0U ? owner - 1U : 0U,
        .core = XIAOTAI_CORE_ANY,
    };
    atomic_fetch_add(&s_tasks, 1U);
    const xiaotai_err_t err = xiaotai_task_create(&config, &s_task);
    if (err != XIAOTAI_OK) atomic_fetch_sub(&s_tasks, 1U);
    return err;
}

void environment_barrier_wait_idle(void)
{
    for (uint32_t waited = 0; atomic_load(&s_tasks) != 0U && waited < BARRIER_IDLE_WAIT_MS;
         waited += BARRIER_POST_RETRY_MS) {
        xiaotai_delay_ms(BARRIER_POST_RETRY_MS);
    }
}
