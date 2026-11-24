// SPDX-License-Identifier: MIT
#pragma once
/* Internals of the wake-word engine shared by its sources and its host tests.
 * Not a public interface: nothing outside sdk/xiaotai_wake includes it. */
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/os.h"
#include "xiaotai/wake.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Policy constants (the same for every model and board; window and result
 * limits are in wake_window.h). */
#define WAKE_POLL_MS 20U                   /* the task looks at its queue this often, never waits on it */
#define WAKE_ARM_US 4000000                /* no detection during the first 4 s after the model is ready */
#define WAKE_COOLDOWN_US 2500000           /* no detection during 2.5 s after one */
#define WAKE_RETRY_FIRST_MS 5000U          /* load retries: 5 s, 10 s, 20 s, then every 30 s, for good */
#define WAKE_RETRY_LAST_MS 30000U
#define WAKE_SUBSCRIBERS_MAX 4U
/* Accepted detections handed to the owner task whose job has not run yet. */
#define WAKE_POSTED_MAX 4U

/* Tuning names (xiaotai_media_param) and their defaults. */
#define WAKE_TUNE_THRESHOLD "wake_engine.threshold_milli"
#define WAKE_TUNE_RAW "wake_engine.raw"
#define WAKE_TUNE_PRIORITY "wake_engine.task_priority"
#define WAKE_TUNE_CORE "wake_engine.task_core"
#define WAKE_TASK_PRIORITY_DEFAULT 3
#define WAKE_TASK_CORE_DEFAULT 0
#define WAKE_TASK_NAME "xt_wake"

/* One pass of the engine task: loads the model when it is due (first pass,
 * then the retry schedule), otherwise delivers a pending detection, drains
 * the queue, scores the newest window and judges the score. Returns the
 * milliseconds to wait before the next pass. The task calls it in a loop;
 * host tests call it directly instead of xiaotai_engine_wake_engine_start, on
 * one thread, after xiaotai_engine_wake_engine_init. */
uint32_t wake_engine_step(void);

/* Host tests: the clock every timestamp, schedule and scoring duration of the
 * engine is read from. NULL restores xiaotai_time_us. Set before init. */
void wake_engine_set_clock(int64_t (*now_us)(void));

typedef struct {
    xiaotai_wake_status_t status;
    int64_t now_us;
    bool raw;
} wake_diagnostics_t;

void wake_engine_diagnostics(wake_diagnostics_t *sample);
xiaotai_err_t wake_diagnostics_register(void);

/* Bytes the engine itself holds (not the model's), by memory kind. */
typedef struct {
    size_t bulk_bytes, internal_bytes, dma_bytes;
    size_t ring_bytes, snapshot_bytes, queue_bytes;
} wake_engine_memory_t;
void wake_engine_memory(wake_engine_memory_t *memory);

/* The configuration start() created the task with; zeroed before. */
void wake_engine_task_config(xiaotai_task_config_t *config);

/* Registers the built-in model. Defined by the platform directory that embeds
 * it; called by init only when XIAOTAI_WAKE_BUILTIN_MODEL is on. */
xiaotai_err_t wake_builtin_model_register(void);

#ifdef __cplusplus
}
#endif
