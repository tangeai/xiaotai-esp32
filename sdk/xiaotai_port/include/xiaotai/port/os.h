// SPDX-License-Identifier: MIT
#pragma once
/* Operating system primitives used by SDK code. SDK code includes this header,
 * never an RTOS header. The linked port supplies the object types through
 * xiaotai_os_port.h (FreeRTOS on ESP-IDF, pthreads on the host), so objects can
 * be embedded in structs and static storage without heap allocation.
 *
 * Timeouts are milliseconds; XIAOTAI_WAIT_FOREVER blocks, 0 polls. A non-zero
 * timeout or delay is rounded up to whole scheduler ticks and is at least one
 * tick, so it never ends early and always lets tasks of lower priority run;
 * the tick rate is the platform's (1 kHz on every ESP-IDF build today, but no
 * caller may rely on that).
 *
 * Callers: [any] and [rt] as defined once in xiaotai/framework.h (a [rt] call
 * that takes a timeout may block, which a real-time task does only for its
 * own input or pacing), plus the port's own
 *   [isr]  also interrupt handlers, but not those that run while the flash
 *          cache is disabled: port functions are not placed in internal
 *          instruction memory
 * Every function of this header is [any] at least. Object init and deinit
 * functions allocate or release kernel state: [any], not [rt]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/port/mem.h"
#include "xiaotai_os_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_WAIT_FOREVER UINT32_MAX
#define XIAOTAI_CORE_ANY (-1)

/* ---- time ---- */

/* [any, rt, isr] Monotonic microseconds since boot. */
int64_t xiaotai_time_us(void);
/* [any, rt] Block the calling task. ms > 0 blocks at least until the next
 * tick (see above), so a loop that delays 1 ms per pass yields to every
 * lower-priority task whatever the tick rate. 0 only yields to ready tasks of
 * equal priority. */
void xiaotai_delay_ms(uint32_t ms);
/* [any, rt, isr] True inside an interrupt handler. */
bool xiaotai_in_isr(void);

/* ---- tasks ---- */

typedef void (*xiaotai_task_fn_t)(void *arg);

typedef struct {
    const char *name;
    xiaotai_task_fn_t entry;
    void *arg;
    uint32_t stack_bytes;
    /* XIAOTAI_MEM_BULK for stacks that may live in PSRAM, XIAOTAI_MEM_INTERNAL
     * otherwise. The task control block always stays internal. */
    xiaotai_mem_kind_t stack;
    /* RTOS priority number, as on ESP-IDF FreeRTOS (higher runs first). */
    unsigned priority;
    /* XIAOTAI_CORE_ANY or a core index; only a hint on single-core chips. */
    int core;
} xiaotai_task_config_t;

/* [any] Starts the task; *task identifies it until it exits. Allocates. */
xiaotai_err_t xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
/* [any] Ends the calling task. Its stack is released after it has stopped running. */
void xiaotai_task_exit(void) __attribute__((noreturn));
/* [any, rt] True when the calling task is this one. */
bool xiaotai_task_is_current(const xiaotai_task_t *task);
/* [any] Borrow the current task identity without allocating a task or a stack.
 * The caller remains responsible for its lifetime. */
xiaotai_err_t xiaotai_task_borrow_current(xiaotai_task_t *task);
/* [any] Change the current priority, optionally returning its previous value.
 * The host port does not model scheduling priorities. */
xiaotai_err_t xiaotai_task_set_current_priority(unsigned priority, unsigned *previous);
/* [any, current task only] Least free stack in bytes, or 0 if unavailable.
 * Call at a diagnostic checkpoint, not on the realtime path. */
uint32_t xiaotai_task_current_stack_free(void);
/* [any] Call before operations that can disable the external memory cache.
 * Logs the task and operation and returns INVALID_STATE for an unsafe stack. */
xiaotai_err_t xiaotai_require_internal_stack(const char *operation);
/* [any, rt] Wake a task waiting in xiaotai_task_wait_notify (counts are not kept). */
void xiaotai_task_notify(xiaotai_task_t *task);
/* [any, rt] True when notified before the timeout; clears the notification. */
bool xiaotai_task_wait_notify(uint32_t timeout_ms);

/* ---- mutex (owner-tracked, not for interrupts) ---- */

/* [any] */
xiaotai_err_t xiaotai_mutex_init(xiaotai_mutex_t *mutex);
/* [any, rt] A real-time task only takes a mutex whose holders keep it for a
 * few instructions; priority inheritance applies. */
bool xiaotai_mutex_lock(xiaotai_mutex_t *mutex, uint32_t timeout_ms);
/* [any, rt] */
void xiaotai_mutex_unlock(xiaotai_mutex_t *mutex);
/* [any] */
void xiaotai_mutex_deinit(xiaotai_mutex_t *mutex);

/* ---- binary semaphore (signal between tasks or from an interrupt) ---- */

/* [any] */
xiaotai_err_t xiaotai_signal_init(xiaotai_signal_t *signal);
/* [any, rt] */
void xiaotai_signal_give(xiaotai_signal_t *signal);
/* [isr] only: from an interrupt handler. */
void xiaotai_signal_give_from_isr(xiaotai_signal_t *signal);
/* [any, rt] */
bool xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout_ms);
/* [any] */
void xiaotai_signal_deinit(xiaotai_signal_t *signal);

/* ---- queue of fixed-size items, copied in and out ---- */

/* [any] Item storage comes from the given memory kind; the queue object itself
 * is wherever the caller placed xiaotai_queue_t. Allocates. */
xiaotai_err_t xiaotai_queue_init(xiaotai_queue_t *queue, size_t depth, size_t item_size,
                                 xiaotai_mem_kind_t storage);
/* [any, rt] A real-time producer passes timeout 0 and counts a full queue. */
bool xiaotai_queue_send(xiaotai_queue_t *queue, const void *item, uint32_t timeout_ms);
/* [any, rt] Replaces the item of a depth-1 queue. */
void xiaotai_queue_overwrite(xiaotai_queue_t *queue, const void *item);
/* [any, rt] */
bool xiaotai_queue_receive(xiaotai_queue_t *queue, void *item, uint32_t timeout_ms);
/* [any, rt] */
size_t xiaotai_queue_count(xiaotai_queue_t *queue);
/* [any] */
void xiaotai_queue_reset(xiaotai_queue_t *queue);
/* [any] Releases the item storage. */
void xiaotai_queue_deinit(xiaotai_queue_t *queue);

/* ---- event bits (24 usable bits) ---- */

/* [any] */
xiaotai_err_t xiaotai_events_init(xiaotai_events_t *events);
/* [any, rt] */
void xiaotai_events_set(xiaotai_events_t *events, uint32_t bits);
/* [any, rt] */
void xiaotai_events_clear(xiaotai_events_t *events, uint32_t bits);
/* [any, rt] */
uint32_t xiaotai_events_get(xiaotai_events_t *events);
/* [any, rt] Returns the bits at wake-up (or timeout). With clear, the awaited
 * bits that were set are cleared atomically on return. */
uint32_t xiaotai_events_wait(xiaotai_events_t *events, uint32_t bits, bool all, bool clear,
                             uint32_t timeout_ms);
/* [any] */
void xiaotai_events_deinit(xiaotai_events_t *events);

/* ---- critical section: a few instructions, never blocks, usable from interrupts ---- */

/* [any, rt, isr] Nothing between enter and exit may block, allocate, log or
 * call another port function except xiaotai_time_us and xiaotai_in_isr. */
void xiaotai_critical_init(xiaotai_critical_t *lock);
void xiaotai_critical_enter(xiaotai_critical_t *lock);
void xiaotai_critical_exit(xiaotai_critical_t *lock);

#ifdef __cplusplus
}
#endif
