#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_STORE_MAX_OPS 8U
#define NVS_STORE_MAX_DATA 512U
#define NVS_STORE_SLOTS 8U

typedef enum {
    NVS_STORE_U8, NVS_STORE_STRING, NVS_STORE_BLOB,
    NVS_STORE_ERASE_KEY, NVS_STORE_ERASE_ALL,
    NVS_STORE_GET_U8, NVS_STORE_GET_STRING, NVS_STORE_GET_BLOB,
} nvs_store_kind_t;

typedef struct {
    nvs_store_kind_t kind;
    const char *key;      /* NULL only for ERASE_ALL; otherwise 1..15 bytes. */
    const void *value;    /* Copied before submit returns; never retained. */
    size_t size;         /* STRING includes NUL, U8 is 1, ERASE is 0. */
} nvs_store_op_t;

typedef uint32_t nvs_store_ticket_t; /* 0 is invalid. Do not reuse consumed tickets. */

/** Call once from the composition root, after nvs_flash_init and before users.
 * One permanent internal-stack storage task; fixed PSRAM payload pool.
 * This component never initializes or erases a whole NVS partition. */
esp_err_t nvs_store_init(void);

/** Nonblocking bounded submission. ESP_OK means queued, not committed.
 * Writes are serialized as one job, committed only if all operations pass.
 * GET operations require a single-key job and never commit (see read_async).
 * NVS multi-key writes are NOT a transaction and errors do not imply rollback.
 * Queue full returns ESP_ERR_NO_MEM. No automatic retry or coalescing: an owner
 * that needs last-value semantics keeps one pending snapshot and one ticket.
 * Every accepted ticket must be taken or abandoned, even on page/session exit. */
esp_err_t nvs_store_submit(const char *namespace_name, const nvs_store_op_t *ops,
                           size_t count, nvs_store_ticket_t *ticket);

/** Nonblocking completion. NOT_FINISHED retains ownership; ESP_OK consumes the
 * ticket and writes the actual NVS result to result (which may itself fail).
 * A stale/consumed ticket returns NOT_FOUND and cannot release a newer job. */
esp_err_t nvs_store_take_result(nvs_store_ticket_t ticket, esp_err_t *result);

/** Drop interest in a result; the accepted write is NOT cancelled. */
esp_err_t nvs_store_abandon(nvs_store_ticket_t ticket);

/** For non-UI owners that must confirm persistence (binding/Wi-Fi/reboot).
 * The write still runs on the same internal worker, safe for PSRAM callers.
 * Timeout abandons the result, not the write; data may already be persisted.
 * Does not enqueue a retry, and never queues caller stack/semaphore pointers. */
esp_err_t nvs_store_execute(const char *namespace_name, const nvs_store_op_t *ops,
                            size_t count, uint32_t timeout_ms);

/** Asynchronous read of one key (GET_U8/STRING/BLOB). Capacity is 1..512 bytes,
 * and 1 for U8. The caller's output pointer is never queued. Reads also require
 * the internal worker: the flash driver can disable cache while reading NVS. */
esp_err_t nvs_store_read_async(const char *namespace_name, const char *key,
                              nvs_store_kind_t kind, size_t capacity,
                              nvs_store_ticket_t *ticket);

/** On completion, copies successful read data and returns its size; the actual
 * NVS error is in result. NVS INVALID_LENGTH also returns the required size.
 * Output too small returns ESP_ERR_INVALID_SIZE without consuming the ticket.
 * Other ownership rules match take_result; take_result may discard read data. */
esp_err_t nvs_store_take_read_result(nvs_store_ticket_t ticket, esp_err_t *result,
                                    void *value, size_t *size);

/** Bounded wait on the same worker, including startup reads. Output is copied
 * only on success. GET_STRING size includes NUL. Timeout does not cancel work. */
esp_err_t nvs_store_read(const char *namespace_name, const char *key,
                        nvs_store_kind_t kind, void *value, size_t *size,
                        uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
