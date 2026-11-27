// SPDX-License-Identifier: MIT
#pragma once
/* Test controls for the host KV backend. A job opens its namespace once, so
 * the delay, the gate and the general failure apply once per job (at open);
 * the key failure applies to each operation on that key. */
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every job waits this long first (0: no delay). */
void xiaotai_kv_host_delay(uint32_t ms);
/* Every job fails with this error when it opens its namespace (XIAOTAI_OK: no
 * failure). */
void xiaotai_kv_host_fail(xiaotai_err_t error);
/* Only read-only opens of this namespace fail; NULL or XIAOTAI_OK clears it. */
void xiaotai_kv_host_fail_open_space(const char *space, xiaotai_err_t error);
/* Every read or write of this key (in any namespace) fails with error; NULL
 * or XIAOTAI_OK: no failure. The other operations of a job still run. */
void xiaotai_kv_host_fail_key(const char *key, xiaotai_err_t error);
/* Closed: the next job stops when it opens its namespace until the gate is
 * opened again. Open by default. */
void xiaotai_kv_host_gate(bool open);
/* True once a job is held at the closed gate, within timeout_ms. */
bool xiaotai_kv_host_wait_held(uint32_t timeout_ms);

/* What reached the backend since the last clear. */
typedef struct {
    uint32_t opens, closes, commits;
    uint32_t writes;     /* set, set_int, erase and erase_all calls */
    uint32_t erase_alls;
} xiaotai_kv_host_counts_t;
void xiaotai_kv_host_counts(xiaotai_kv_host_counts_t *counts);

/* Forgets every entry and namespace and the counts; no delay, no failures, the
 * gate open. Call it while no job runs. */
void xiaotai_kv_host_clear(void);

#ifdef __cplusplus
}
#endif
