// SPDX-License-Identifier: MIT
#pragma once
/* Internal interfaces of the environment module: the phase machine
 * (environment.c) and the storage barrier task (environment_barrier.c). The
 * console part (AT+EP, diagnostics) reads the same state through the public
 * header and environment_effective_config(). */
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/modules/environment.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENVIRONMENT_TAG "environment"

/* Step 4 checks the gate and the pending settings this often. */
#define ENVIRONMENT_POLL_MS 50U
#define ENVIRONMENT_WAIT_DEFAULT_MS 10000U
#define ENVIRONMENT_RELEASE_DEFAULT_MS 5000U
/* Longest single barrier when step 4 has no time limit, so a cancel is seen
 * within this time even when storage hangs. */
#define ENVIRONMENT_BARRIER_SLICE_MS 5000U
#define ENVIRONMENT_BARRIER_STACK_BYTES 3072U
#define ENVIRONMENT_BARRIER_TASK_NAME "xt_env_barrier"

typedef enum {
    /* Step 4: flush preferences for maintenance, then xiaotai_kv_flush, both within
     * the time given. */
    ENVIRONMENT_BARRIER_BEFORE_WRITE = 0,
    /* First release step after a store that timed out: xiaotai_kv_flush. */
    ENVIRONMENT_BARRIER_RELEASE,
} environment_barrier_kind_t;

/* [owner] Start the barrier on its own short-lived task. XIAOTAI_OK: the
 * result arrives later through environment_barrier_done() on the owner task;
 * otherwise nothing runs (no memory for the task). Only one barrier runs at a
 * time: the owner starts the next one only after the previous result. */
xiaotai_err_t environment_barrier_start(environment_barrier_kind_t kind, uint32_t timeout_ms, uint32_t sequence);

/* [owner] Implemented by environment.c: the barrier's result. sequence is the
 * one given to environment_barrier_start; a result for an older one is stale. */
void environment_barrier_done(environment_barrier_kind_t kind, uint32_t sequence, xiaotai_err_t result);

/* [any] Host reset only: wait (bounded) until no barrier task is left. */
void environment_barrier_wait_idle(void);

/* [any] The configuration in force, defaults applied. */
void environment_effective_config(xiaotai_environment_config_t *config);

/* [any] Asynchronous results (barrier, store) dropped because they belonged
 * to an older switch. */
uint32_t environment_stale_results(void);

/* [init] Register owner commands, diagnostics, and result notification. */
xiaotai_err_t environment_at_register(void);

#ifdef __cplusplus
}
#endif
