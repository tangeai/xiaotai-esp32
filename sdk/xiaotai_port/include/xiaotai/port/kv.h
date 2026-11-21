// SPDX-License-Identifier: MIT
#pragma once
/* Persistent key-value storage. Every call runs on the port's storage worker,
 * so flash writes never run on the caller's stack (which may be in PSRAM).
 *
 * Synchronous calls copy the value into a worker-owned slot and wait up to
 * timeout_ms. A timeout abandons a queued job, but a write already running may
 * still complete: callers re-read before treating a timed-out write as failed.
 *
 * Types. Strings, blobs and integers of each width and sign are separate types,
 * as stored by ESP-IDF NVS, so values written by earlier firmware remain
 * readable: a key is read with the type it was written with, and reading it as
 * another type finds nothing (XIAOTAI_ERR_NOT_FOUND). Every NVS type is
 * covered once here, so a component with deployed data of any type reads it
 * without a change to this port.
 *
 * Memory. Values up to XIAOTAI_KV_SLOT_BYTES travel in the worker's permanent
 * job slots (XIAOTAI_KV_SLOTS of them, XIAOTAI_MEM_BULK). A larger value, up
 * to XIAOTAI_KV_VALUE_MAX, takes a buffer of its own size from
 * XIAOTAI_MEM_BULK for the life of its job (XIAOTAI_ERR_NO_MEM when there is
 * none), so raising the limit costs nothing permanently. Slots and buffers are
 * wiped when a job ends (values may be credentials). Values are never logged.
 * The worker's stack (XIAOTAI_KV_WORKER_STACK_BYTES, internal RAM) and the
 * slot count are build options (xiaotai/port_config.h). The worker is the one
 * storage task of the system: components with writes to delay or coalesce,
 * or reads they must not wait for, use the asynchronous calls below (single
 * values, a batch with a completion: xiaotai_kv_commit_async, a read with a
 * completion: xiaotai_kv_read_async) instead of a storage task of their own.
 *
 * Callers: every function is [any] (xiaotai/framework.h) and none is [rt]:
 * never from an interrupt, a real-time task or the storage worker itself.
 * Synchronous calls wait up to their timeout_ms, so the framework owner task
 * uses them only during init and otherwise prefers the asynchronous calls
 * (from a timer on the owner task, for example), which never wait for
 * storage. This header adds one tag of its own: [storage worker], a callback
 * the storage worker runs (xiaotai_kv_done_fn_t). */
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Namespace and key: at most 15 characters each. */
#define XIAOTAI_KV_NAME_MAX 15U
/* Largest value (string including its terminator, or blob), and the most data
 * one xiaotai_kv_commit carries in total. Deployed firmware stores strings of
 * up to 2048 bytes (a pending sound-recognition record). */
#define XIAOTAI_KV_VALUE_MAX 2048U
/* Values up to this size use the worker's permanent slots (see above). */
#define XIAOTAI_KV_SLOT_BYTES 512U
/* Longest wait a synchronous call accepts. */
#define XIAOTAI_KV_WAIT_MAX_MS 5000U
/* Most operations in one xiaotai_kv_commit. */
#define XIAOTAI_KV_OPS_MAX 8U

/* Integer entry types, one per NVS integer type. */
typedef enum {
    XIAOTAI_KV_U8 = 0,
    XIAOTAI_KV_I8,
    XIAOTAI_KV_U16,
    XIAOTAI_KV_I16,
    XIAOTAI_KV_U32,
    XIAOTAI_KV_I32,
    XIAOTAI_KV_U64,
    XIAOTAI_KV_I64,
} xiaotai_kv_int_t;

/* [any] Once, before any other call (the framework does this at start);
 * later calls return XIAOTAI_OK. */
xiaotai_err_t xiaotai_kv_init(void);
/* As init, also identifies an original backend error that allows a local
 * recovery boot. A queue, task or other initialization failure is not
 * eligible. The failed worker remains closed in either case. */
xiaotai_err_t xiaotai_kv_init_boot(xiaotai_err_t *local_error);

/* Synchronous calls: [any], wait up to timeout_ms.
 *
 * Blob. *size: capacity in, stored length out (also on XIAOTAI_ERR_INVALID_SIZE,
 * when the stored value is larger than the capacity). XIAOTAI_ERR_NOT_FOUND
 * when absent. */
xiaotai_err_t xiaotai_kv_get(const char *space, const char *key, void *data, size_t *size,
                             uint32_t timeout_ms);
/* Same as xiaotai_kv_get, but an existing key of a different type is
 * INVALID_ARG, not NOT_FOUND. For versioned readers that may only fall back
 * to an older key when there is no entry at all. */
xiaotai_err_t xiaotai_kv_get_blob_strict(const char *space, const char *key, void *data, size_t *size,
                                         uint32_t timeout_ms);
/* String, terminator included in capacity. XIAOTAI_ERR_INVALID_SIZE when it
 * does not fit; XIAOTAI_FAIL when the stored bytes are not a terminated string. */
xiaotai_err_t xiaotai_kv_get_str(const char *space, const char *key, char *out, size_t capacity,
                                 uint32_t timeout_ms);
/* A differently typed key is INVALID_ARG, not NOT_FOUND. Returns XIAOTAI_FAIL
 * when the stored span does not end at its first NUL. */
xiaotai_err_t xiaotai_kv_get_str_strict(const char *space, const char *key, char *out, size_t capacity,
                                        uint32_t timeout_ms);
/* Integer of the given type. value carries the number; U64 values above
 * INT64_MAX are carried as the same 64 bits (cast through uint64_t). */
xiaotai_err_t xiaotai_kv_get_int(const char *space, const char *key, xiaotai_kv_int_t type, int64_t *value,
                                 uint32_t timeout_ms);

/* Optional diagnostic phase for a typed get. The return code and the value
 * follow the ordinary get. JOB: no result from the worker (including a
 * timeout). OPEN: the namespace could not be opened. ENTRY: open succeeded
 * and the worker attempted the key, whether or not the key could be read. */
typedef enum {
    XIAOTAI_KV_READ_STAGE_JOB = 0,
    XIAOTAI_KV_READ_STAGE_OPEN,
    XIAOTAI_KV_READ_STAGE_ENTRY,
} xiaotai_kv_read_stage_t;

xiaotai_err_t xiaotai_kv_get_int_with_stage(const char *space, const char *key, xiaotai_kv_int_t type, int64_t *value,
                                           xiaotai_kv_read_stage_t *stage, uint32_t timeout_ms);
xiaotai_err_t xiaotai_kv_get_str_with_stage(const char *space, const char *key, char *out, size_t capacity,
                                           xiaotai_kv_read_stage_t *stage, uint32_t timeout_ms);
/* Same as xiaotai_kv_get_int with XIAOTAI_KV_U8. */
xiaotai_err_t xiaotai_kv_get_u8(const char *space, const char *key, uint8_t *value, uint32_t timeout_ms);
xiaotai_err_t xiaotai_kv_set(const char *space, const char *key, const void *data, size_t size,
                             uint32_t timeout_ms);
xiaotai_err_t xiaotai_kv_set_str(const char *space, const char *key, const char *value,
                                 uint32_t timeout_ms);
/* XIAOTAI_ERR_INVALID_ARG when value is outside the type's range (U64: see
 * xiaotai_kv_get_int). */
xiaotai_err_t xiaotai_kv_set_int(const char *space, const char *key, xiaotai_kv_int_t type, int64_t value,
                                 uint32_t timeout_ms);
/* Same as xiaotai_kv_set_int with XIAOTAI_KV_U8. */
xiaotai_err_t xiaotai_kv_set_u8(const char *space, const char *key, uint8_t value, uint32_t timeout_ms);
/* Removing an absent key succeeds; removes the key whatever its type. */
xiaotai_err_t xiaotai_kv_erase(const char *space, const char *key, uint32_t timeout_ms);
/* Removes every key of the namespace. An absent namespace succeeds. */
xiaotai_err_t xiaotai_kv_erase_space(const char *space, uint32_t timeout_ms);

typedef enum {
    XIAOTAI_KV_OP_SET = 0,     /* blob: data, size */
    XIAOTAI_KV_OP_SET_STR,     /* string: data is the terminated text, size ignored */
    XIAOTAI_KV_OP_SET_U8,      /* data points to one byte, size ignored */
    XIAOTAI_KV_OP_ERASE,       /* removes key; absent succeeds; data ignored */
    XIAOTAI_KV_OP_SET_INT,     /* int_type, number; data and size ignored */
} xiaotai_kv_op_kind_t;

typedef struct {
    xiaotai_kv_op_kind_t kind;
    const char *key;
    const void *data;
    size_t size;
    xiaotai_kv_int_t int_type;   /* XIAOTAI_KV_OP_SET_INT */
    int64_t number;              /* XIAOTAI_KV_OP_SET_INT, as for xiaotai_kv_set_int */
} xiaotai_kv_op_t;

/* [any] Several writes to one namespace as one storage job: the operations run in
 * order on the worker, stop at the first failure, and are made durable together
 * once all succeeded (one commit). They are copied before the call returns
 * (1..XIAOTAI_KV_OPS_MAX operations, XIAOTAI_KV_VALUE_MAX bytes of data in
 * total, otherwise XIAOTAI_ERR_INVALID_SIZE; an integer counts 8 bytes). This
 * is not a transaction: after a failure or a power loss part of the operations
 * may have been applied, so order them so that every prefix is a state the
 * reader accepts. */
xiaotai_err_t xiaotai_kv_commit(const char *space, const xiaotai_kv_op_t *ops, size_t count,
                                uint32_t timeout_ms);

/* [storage worker] Outcome of xiaotai_kv_commit_async: the result
 * xiaotai_kv_commit would have returned for the same job. Runs on the storage
 * worker (internal RAM stack, low priority) and must stay short: it may take
 * a lock that is only ever held briefly and post to a queue (for example
 * xiaotai_run in xiaotai/framework.h), but never waits, never calls a
 * synchronous function of this header or xiaotai_kv_flush, and never queues
 * another storage job from inside. */
typedef void (*xiaotai_kv_done_fn_t)(xiaotai_err_t result, void *ctx);

/* [any] xiaotai_kv_commit without waiting: what a component uses to write a
 * record it batched itself (several keys of one namespace, typed values) and
 * learn the result later, instead of keeping a storage task of its own. The
 * operations and their data are copied into a job before the call returns,
 * with the limits of xiaotai_kv_commit (XIAOTAI_ERR_INVALID_SIZE, nothing
 * queued); the job is never merged with another and runs in queue order.
 * done (may be NULL) is called exactly once on the storage worker after the
 * job ran, whatever its result; a failure is also counted (failures) and
 * logged without values. XIAOTAI_ERR_NO_MEM: no free slot (counted in full)
 * or no buffer for the data; XIAOTAI_ERR_TIMEOUT: the worker's lock stayed
 * busy for a few milliseconds; XIAOTAI_ERR_INVALID_ARG: an operation outside
 * the rules of xiaotai_kv_commit (an integer out of its type's range among
 * them); XIAOTAI_ERR_INVALID_STATE: the port is not initialized or the call
 * comes from an interrupt. After any error nothing was queued and done is not
 * called. xiaotai_kv_flush waits for the job like for any other, its done
 * included. */
xiaotai_err_t xiaotai_kv_commit_async(const char *space, const xiaotai_kv_op_t *ops, size_t count,
                                      xiaotai_kv_done_fn_t done, void *ctx);

/* Most entries in one xiaotai_kv_read_async. */
#define XIAOTAI_KV_READS_MAX 4U

typedef enum {
    XIAOTAI_KV_READ_BLOB = 0,  /* up to capacity bytes */
    XIAOTAI_KV_READ_STR,       /* terminated text, terminator included in capacity */
    XIAOTAI_KV_READ_INT,       /* integer of int_type; capacity ignored, counts 8 bytes */
    XIAOTAI_KV_READ_STR_STRICT, /* same as xiaotai_kv_get_str_strict */
} xiaotai_kv_read_kind_t;

typedef struct {
    xiaotai_kv_read_kind_t kind;
    const char *key;
    size_t capacity;             /* BLOB and string reads: at least 1 */
    xiaotai_kv_int_t int_type;   /* INT */
} xiaotai_kv_read_t;

/* One entry's outcome, as the synchronous get of the same kind would give it:
 * XIAOTAI_OK; XIAOTAI_ERR_NOT_FOUND (absent, or another type for non-strict reads);
 * XIAOTAI_ERR_INVALID_ARG (stored with another type for STR_STRICT);
 * XIAOTAI_ERR_INVALID_SIZE (larger than capacity; size is the stored length);
 * XIAOTAI_FAIL for malformed text (STR_STRICT also rejects an interior NUL); or
 * the storage error. */
typedef struct {
    xiaotai_err_t result;
    /* XIAOTAI_OK, blob and string reads: read-only data inside the job's slot
     * (strings are terminated). The slot is wiped when done returns. NULL otherwise. */
    const void *data;
    size_t size;                 /* blob/string bytes read (terminator included for text) */
    int64_t number;              /* INT with XIAOTAI_OK, as xiaotai_kv_get_int gives it */
} xiaotai_kv_value_t;

/* [storage worker] Outcome of xiaotai_kv_read_async: values[i] answers
 * reads[i], count is the request's count. The rules of xiaotai_kv_done_fn_t
 * apply; done copies what it keeps (into memory its component owns) and keeps
 * no pointer into values. */
typedef void (*xiaotai_kv_read_fn_t)(const xiaotai_kv_value_t *values, size_t count, void *ctx);

/* [any] Reads 1..XIAOTAI_KV_READS_MAX entries of one namespace as one storage
 * job without waiting: what a component uses to reload stored state from a
 * task that must not wait for flash (the owner task), instead of a storage
 * task of its own. The requests (keys included) are copied before the call
 * returns. The values travel in one permanent slot: the capacities together
 * (an integer counts 8 bytes) are at most XIAOTAI_KV_SLOT_BYTES, otherwise
 * XIAOTAI_ERR_INVALID_SIZE and nothing queued; no extra buffer is ever taken.
 * The job is never merged with another and runs in queue order, so it sees
 * every write queued before it. Each entry is read on its own: one entry's
 * failure does not stop the others. done (required) is called exactly once on
 * the storage worker after the job ran. Errors and their "nothing queued,
 * done not called" rule as for xiaotai_kv_commit_async (XIAOTAI_ERR_NO_MEM,
 * XIAOTAI_ERR_TIMEOUT, XIAOTAI_ERR_INVALID_STATE); XIAOTAI_ERR_INVALID_ARG for
 * a bad name, a count outside 1..XIAOTAI_KV_READS_MAX, a capacity of 0, an
 * unknown kind or integer type, or no done. xiaotai_kv_flush waits for the
 * job, its done included. */
xiaotai_err_t xiaotai_kv_read_async(const char *space, const xiaotai_kv_read_t *reads, size_t count,
                                    xiaotai_kv_read_fn_t done, void *ctx);

/* Asynchronous writes, [any]: queued without waiting for storage, so the owner task
 * may call them at any time. The value is copied before the call returns.
 * A write still queued for the same namespace, key and type is replaced, so
 * only the latest value reaches flash and a burst of changes to one key costs
 * one slot and one flash write (counted in coalesced). A queued write is never
 * replaced by one of another type: a key written as an integer and then as a
 * blob gets two jobs, run in order. Success means queued; a failure of the
 * later write is counted (failures) and logged without the value, and
 * xiaotai_kv_flush lets the caller wait for it. XIAOTAI_ERR_NO_MEM: no free
 * slot (counted in full) or no buffer for a large value; XIAOTAI_ERR_TIMEOUT:
 * the worker's lock stayed busy for a few milliseconds (nothing queued). */

/* Blob. */
xiaotai_err_t xiaotai_kv_set_async(const char *space, const char *key, const void *data, size_t size);
/* String (terminator included, at most XIAOTAI_KV_VALUE_MAX bytes). */
xiaotai_err_t xiaotai_kv_set_str_async(const char *space, const char *key, const char *value);
/* Integer of the given type; XIAOTAI_ERR_INVALID_ARG when value is outside
 * the type's range (checked before anything is queued). */
xiaotai_err_t xiaotai_kv_set_int_async(const char *space, const char *key, xiaotai_kv_int_t type,
                                       int64_t value);

/* [any] Waits until every job queued before this call has run (whether or not it
 * succeeded), including asynchronous writes: what a caller does before
 * restarting the device or switching what the stored values mean. Jobs queued
 * after the call are not waited for. XIAOTAI_OK at once when the port is not
 * initialized (nothing can be queued). XIAOTAI_ERR_TIMEOUT when they have not
 * all run within timeout_ms (0 only checks); XIAOTAI_ERR_INVALID_STATE on the
 * storage worker itself, which would wait for its own job. */
xiaotai_err_t xiaotai_kv_flush(uint32_t timeout_ms);

typedef struct {
    uint32_t submitted, completed, coalesced, failures, timeouts, full;
} xiaotai_kv_stats_t;
/* [any] Counters since start. */
void xiaotai_kv_stats(xiaotai_kv_stats_t *stats);

#ifdef __cplusplus
}
#endif
