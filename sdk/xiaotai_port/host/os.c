// SPDX-License-Identifier: MIT
/* xiaotai/port/os.h on POSIX threads, for host builds and tests. Same blocking
 * and timeout semantics as the ESP-IDF port; priorities and cores are ignored. */
#define _GNU_SOURCE
#include "xiaotai/port/os.h"
#include "xiaotai_os_host.h"
#include "mem_ledger.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct xiaotai_host_task {
    xiaotai_task_fn_t entry;
    void *arg;
    char name[24];
    xiaotai_mem_kind_t stack_kind;
    bool stack_external;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool notified;
};

static __thread struct xiaotai_host_task *s_current;
static atomic_int_fast64_t s_time_offset;

void xiaotai_os_host_advance_us(int64_t delta)
{
    if (delta <= 0) return;
    int_fast64_t old = atomic_load_explicit(&s_time_offset, memory_order_relaxed);
    for (;;) {
        int_fast64_t next = old > INT64_MAX - delta ? INT64_MAX : old + delta;
        if (atomic_compare_exchange_weak_explicit(&s_time_offset, &old, next,
                                                  memory_order_relaxed, memory_order_relaxed)) return;
    }
}

int64_t xiaotai_time_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t real = (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
    const int64_t offset = atomic_load_explicit(&s_time_offset, memory_order_relaxed);
    return offset > INT64_MAX - real ? INT64_MAX : real + offset;
}

void xiaotai_delay_ms(uint32_t ms)
{
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

bool xiaotai_in_isr(void) { return false; }

/* Absolute CLOCK_MONOTONIC deadline for condition waits. */
static struct timespec deadline(uint32_t ms)
{
    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_sec += ms / 1000;
    at.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (at.tv_nsec >= 1000000000L) {
        at.tv_sec += 1;
        at.tv_nsec -= 1000000000L;
    }
    return at;
}

static void cond_init(pthread_cond_t *cond)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

/* Wait until ready() holds or the timeout passes; mutex held on entry and exit. */
#define WAIT_UNTIL(cond, mutex, ready, timeout_ms)                                 \
    do {                                                                           \
        if ((timeout_ms) == XIAOTAI_WAIT_FOREVER) {                                \
            while (!(ready)) pthread_cond_wait((cond), (mutex));                   \
        } else {                                                                   \
            struct timespec until_ = deadline(timeout_ms);                         \
            while (!(ready) && pthread_cond_timedwait((cond), (mutex), &until_) == 0) { \
            }                                                                      \
        }                                                                          \
    } while (0)

static void *task_main(void *argument)
{
    struct xiaotai_host_task *task = argument;
    s_current = task;
    task->entry(task->arg);
    xiaotai_task_exit();
}

xiaotai_err_t xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (!config || !config->entry || !config->name || config->stack_bytes == 0 || !task) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    struct xiaotai_host_task *created = calloc(1, sizeof(*created));
    if (!created) return XIAOTAI_ERR_NO_MEM;
    created->entry = config->entry;
    created->arg = config->arg;
    snprintf(created->name, sizeof(created->name), "%s", config->name);
    created->stack_kind = config->stack;
    created->stack_external = created->stack_kind == XIAOTAI_MEM_BULK && xiaotai_mem_has_external();
    pthread_mutex_init(&created->mutex, NULL);
    cond_init(&created->changed);
    task->task = created;
    pthread_t thread;
    xiaotai_mem_host_task_open(created, config->name, config->stack, config->stack_bytes, XIAOTAI_LEDGER_CALLER);
    if (pthread_create(&thread, NULL, task_main, created) != 0) {
        xiaotai_mem_host_task_close(created);
        task->task = NULL;
        free(created);
        return XIAOTAI_ERR_NO_MEM;
    }
    pthread_detach(thread);
    return XIAOTAI_OK;
}

void xiaotai_task_exit(void)
{
    /* The task record stays allocated: handles held by other tasks may still
     * notify it, which the ESP-IDF port also tolerates until reuse. */
    xiaotai_mem_host_task_close(s_current);
    pthread_exit(NULL);
}

bool xiaotai_task_is_current(const xiaotai_task_t *task)
{
    return task && task->task && task->task == s_current;
}

xiaotai_err_t xiaotai_task_borrow_current(xiaotai_task_t *task)
{
    if (!task) return XIAOTAI_ERR_INVALID_ARG;
    static _Thread_local struct xiaotai_host_task caller;
    if (!s_current) {
        if (pthread_mutex_init(&caller.mutex, NULL) != 0) return XIAOTAI_FAIL;
        cond_init(&caller.changed);
        s_current = &caller;
    }
    task->task = s_current;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_task_set_current_priority(unsigned priority, unsigned *previous)
{
    (void)priority;
    if (previous) *previous = 0;
    return XIAOTAI_OK;
}

uint32_t xiaotai_task_current_stack_free(void) { return 0; }

xiaotai_err_t xiaotai_require_internal_stack(const char *operation)
{
    if (!s_current || !s_current->stack_external) return XIAOTAI_OK;
    fprintf(stderr, "cache-unsafe task stack: task=%s operation=%s\n",
            s_current->name, operation ? operation : "?");
    /* Explicit abort also works in Release (NDEBUG). Product tests opt in. */
    const char *strict = getenv("XIAOTAI_HOST_STACK_STRICT");
    if (strict && strcmp(strict, "1") == 0) abort();
    return XIAOTAI_ERR_INVALID_STATE;
}

void xiaotai_task_notify(xiaotai_task_t *handle)
{
    if (!handle || !handle->task) return;
    struct xiaotai_host_task *task = handle->task;
    pthread_mutex_lock(&task->mutex);
    task->notified = true;
    pthread_cond_broadcast(&task->changed);
    pthread_mutex_unlock(&task->mutex);
}

bool xiaotai_task_wait_notify(uint32_t timeout_ms)
{
    struct xiaotai_host_task *task = s_current;
    if (!task) {
        /* A thread not created by the port (the test's main): nothing can notify it. */
        if (timeout_ms != XIAOTAI_WAIT_FOREVER) xiaotai_delay_ms(timeout_ms);
        return false;
    }
    pthread_mutex_lock(&task->mutex);
    WAIT_UNTIL(&task->changed, &task->mutex, task->notified, timeout_ms);
    const bool notified = task->notified;
    task->notified = false;
    pthread_mutex_unlock(&task->mutex);
    return notified;
}

xiaotai_err_t xiaotai_mutex_init(xiaotai_mutex_t *mutex)
{
    if (!mutex) return XIAOTAI_ERR_INVALID_ARG;
    memset(mutex, 0, sizeof(*mutex));
    pthread_mutex_init(&mutex->mutex, NULL);
    cond_init(&mutex->changed);
    return XIAOTAI_OK;
}

bool xiaotai_mutex_lock(xiaotai_mutex_t *mutex, uint32_t timeout_ms)
{
    pthread_mutex_lock(&mutex->mutex);
    WAIT_UNTIL(&mutex->changed, &mutex->mutex, !mutex->locked, timeout_ms);
    const bool acquired = !mutex->locked;
    if (acquired) {
        mutex->locked = true;
        mutex->owner = pthread_self();
    }
    pthread_mutex_unlock(&mutex->mutex);
    return acquired;
}

void xiaotai_mutex_unlock(xiaotai_mutex_t *mutex)
{
    pthread_mutex_lock(&mutex->mutex);
    mutex->locked = false;
    pthread_cond_broadcast(&mutex->changed);
    pthread_mutex_unlock(&mutex->mutex);
}

void xiaotai_mutex_deinit(xiaotai_mutex_t *mutex)
{
    if (!mutex) return;
    pthread_cond_destroy(&mutex->changed);
    pthread_mutex_destroy(&mutex->mutex);
}

xiaotai_err_t xiaotai_signal_init(xiaotai_signal_t *signal)
{
    if (!signal) return XIAOTAI_ERR_INVALID_ARG;
    memset(signal, 0, sizeof(*signal));
    pthread_mutex_init(&signal->mutex, NULL);
    cond_init(&signal->changed);
    return XIAOTAI_OK;
}

void xiaotai_signal_give(xiaotai_signal_t *signal)
{
    pthread_mutex_lock(&signal->mutex);
    signal->given = true;
    pthread_cond_broadcast(&signal->changed);
    pthread_mutex_unlock(&signal->mutex);
}

void xiaotai_signal_give_from_isr(xiaotai_signal_t *signal) { xiaotai_signal_give(signal); }

bool xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout_ms)
{
    pthread_mutex_lock(&signal->mutex);
    WAIT_UNTIL(&signal->changed, &signal->mutex, signal->given, timeout_ms);
    const bool taken = signal->given;
    signal->given = false;
    pthread_mutex_unlock(&signal->mutex);
    return taken;
}

void xiaotai_signal_deinit(xiaotai_signal_t *signal)
{
    if (!signal) return;
    pthread_cond_destroy(&signal->changed);
    pthread_mutex_destroy(&signal->mutex);
}

xiaotai_err_t xiaotai_queue_init(xiaotai_queue_t *queue, size_t depth, size_t item_size,
                                 xiaotai_mem_kind_t storage)
{
    if (!queue || depth == 0 || item_size == 0 || depth > SIZE_MAX / item_size) return XIAOTAI_ERR_INVALID_ARG;
    memset(queue, 0, sizeof(*queue));
    queue->items = xiaotai_mem_alloc(storage, depth * item_size);
    if (!queue->items) return XIAOTAI_ERR_NO_MEM;
    xiaotai_mem_host_buffer_site(queue->items, XIAOTAI_LEDGER_CALLER);
    queue->depth = depth;
    queue->item_size = item_size;
    pthread_mutex_init(&queue->mutex, NULL);
    cond_init(&queue->changed);
    return XIAOTAI_OK;
}

bool xiaotai_queue_send(xiaotai_queue_t *queue, const void *item, uint32_t timeout_ms)
{
    pthread_mutex_lock(&queue->mutex);
    WAIT_UNTIL(&queue->changed, &queue->mutex, queue->count < queue->depth, timeout_ms);
    const bool sent = queue->count < queue->depth;
    if (sent) {
        const size_t tail = (queue->head + queue->count) % queue->depth;
        memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
        ++queue->count;
        pthread_cond_broadcast(&queue->changed);
    }
    pthread_mutex_unlock(&queue->mutex);
    return sent;
}

void xiaotai_queue_overwrite(xiaotai_queue_t *queue, const void *item)
{
    pthread_mutex_lock(&queue->mutex);
    queue->head = 0;
    queue->count = 1;
    memcpy(queue->items, item, queue->item_size);
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->mutex);
}

bool xiaotai_queue_receive(xiaotai_queue_t *queue, void *item, uint32_t timeout_ms)
{
    pthread_mutex_lock(&queue->mutex);
    WAIT_UNTIL(&queue->changed, &queue->mutex, queue->count > 0, timeout_ms);
    const bool received = queue->count > 0;
    if (received) {
        memcpy(item, queue->items + queue->head * queue->item_size, queue->item_size);
        queue->head = (queue->head + 1) % queue->depth;
        --queue->count;
        pthread_cond_broadcast(&queue->changed);
    }
    pthread_mutex_unlock(&queue->mutex);
    return received;
}

size_t xiaotai_queue_count(xiaotai_queue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);
    const size_t count = queue->count;
    pthread_mutex_unlock(&queue->mutex);
    return count;
}

void xiaotai_queue_reset(xiaotai_queue_t *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->head = queue->count = 0;
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->mutex);
}

void xiaotai_queue_deinit(xiaotai_queue_t *queue)
{
    if (!queue) return;
    xiaotai_mem_free(queue->items);
    queue->items = NULL;
    pthread_cond_destroy(&queue->changed);
    pthread_mutex_destroy(&queue->mutex);
}

xiaotai_err_t xiaotai_events_init(xiaotai_events_t *events)
{
    if (!events) return XIAOTAI_ERR_INVALID_ARG;
    memset(events, 0, sizeof(*events));
    pthread_mutex_init(&events->mutex, NULL);
    cond_init(&events->changed);
    return XIAOTAI_OK;
}

void xiaotai_events_set(xiaotai_events_t *events, uint32_t bits)
{
    pthread_mutex_lock(&events->mutex);
    events->bits |= bits & 0x00ffffffU;
    pthread_cond_broadcast(&events->changed);
    pthread_mutex_unlock(&events->mutex);
}

void xiaotai_events_clear(xiaotai_events_t *events, uint32_t bits)
{
    pthread_mutex_lock(&events->mutex);
    events->bits &= ~bits;
    pthread_mutex_unlock(&events->mutex);
}

uint32_t xiaotai_events_get(xiaotai_events_t *events)
{
    pthread_mutex_lock(&events->mutex);
    const uint32_t bits = events->bits;
    pthread_mutex_unlock(&events->mutex);
    return bits;
}

uint32_t xiaotai_events_wait(xiaotai_events_t *events, uint32_t bits, bool all, bool clear, uint32_t timeout_ms)
{
    pthread_mutex_lock(&events->mutex);
    WAIT_UNTIL(&events->changed, &events->mutex,
               all ? (events->bits & bits) == bits : (events->bits & bits) != 0, timeout_ms);
    const uint32_t seen = events->bits;
    const bool met = all ? (seen & bits) == bits : (seen & bits) != 0;
    if (met && clear) events->bits &= ~bits;
    pthread_mutex_unlock(&events->mutex);
    return seen;
}

void xiaotai_events_deinit(xiaotai_events_t *events)
{
    if (!events) return;
    pthread_cond_destroy(&events->changed);
    pthread_mutex_destroy(&events->mutex);
}

void xiaotai_critical_init(xiaotai_critical_t *lock) { pthread_mutex_init(&lock->mutex, NULL); }

void xiaotai_critical_enter(xiaotai_critical_t *lock) { pthread_mutex_lock(&lock->mutex); }

void xiaotai_critical_exit(xiaotai_critical_t *lock) { pthread_mutex_unlock(&lock->mutex); }
