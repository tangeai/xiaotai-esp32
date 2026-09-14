#ifndef NVS_WORKER_H
#define NVS_WORKER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_WORKER_DATA_BYTES 512U
#define NVS_WORKER_SLOTS 4U
#define NVS_WORKER_WAIT_MS 5000U

/* NVS adapters only: no network/UI/media calls, nested worker calls or borrowed
 * pointers in data. The service owns this copy until the callback returns. */
typedef esp_err_t (*nvs_worker_fn_t)(void *data, size_t size);

/* Called once by the composition root, after nvs_flash_init and before users.
 * One internal stack/RTOS control set and a fixed PSRAM request pool. */
esp_err_t nvs_worker_init(void);

/* Copy, enqueue and wait for completion. Copy the result back only if completed.
 * A timeout abandons a queued job; an already-running write may still finish.
 * Never equate timeout with rollback or enqueue success with persistence. */
esp_err_t nvs_worker_call(nvs_worker_fn_t function, void *data, size_t size,
                          uint32_t timeout_ms);

/* Task-context, bounded submission. Replace only a pending async job with the
 * same callback; never modify its in-flight copy. Success means queued only.
 * Completion errors are logged by the worker, without logging data/credentials. */
esp_err_t nvs_worker_submit_latest(nvs_worker_fn_t function,
                                   const void *data, size_t size);

typedef struct {
    uint32_t submitted, completed, coalesced, failures, timeouts, full;
    uint32_t in_use, stack_free_min;
} nvs_worker_status_t;

/* Diagnostic snapshot only; not a persistence acknowledgment. */
esp_err_t nvs_worker_get_status(nvs_worker_status_t *status);

#ifdef __cplusplus
}
#endif
#endif
