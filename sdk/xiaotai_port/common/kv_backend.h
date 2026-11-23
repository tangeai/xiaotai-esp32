// SPDX-License-Identifier: MIT
#pragma once
/* Storage backend under the shared KV worker (port-private). Called only on the
 * worker task, one call at a time, in the shape of ESP-IDF NVS: a job opens a
 * namespace handle, reads or writes through it, commits once when it wrote,
 * and closes it.
 *
 * Types are kept apart as NVS keeps them: a string, a blob and each integer
 * type are different types, and a get with another type than the stored one
 * returns XIAOTAI_ERR_NOT_FOUND. A set replaces the key whatever type it had. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/port/kv.h"

typedef uint32_t xiaotai_kv_handle_t;

xiaotai_err_t xiaotai_kv_backend_init(void);
bool xiaotai_kv_backend_recovery_error(xiaotai_err_t error);
/* write: the namespace is created when absent. Read-only: XIAOTAI_ERR_NOT_FOUND
 * when the namespace does not exist. */
xiaotai_err_t xiaotai_kv_backend_open(const char *space, bool write, xiaotai_kv_handle_t *handle);
void xiaotai_kv_backend_close(xiaotai_kv_handle_t handle);
/* text: string type (stored and returned with its terminator) or blob type.
 * *size: capacity in, stored length out; XIAOTAI_ERR_INVALID_SIZE with the
 * stored length when it does not fit. */
xiaotai_err_t xiaotai_kv_backend_get(xiaotai_kv_handle_t handle, const char *key, bool text, void *data,
                                     size_t *size);
xiaotai_err_t xiaotai_kv_backend_get_blob_strict(xiaotai_kv_handle_t handle, const char *key, void *data,
                                                 size_t *size);
xiaotai_err_t xiaotai_kv_backend_get_str_strict(xiaotai_kv_handle_t handle, const char *key, void *data,
                                                size_t *size);
/* U64 values come back as the same 64 bits. */
xiaotai_err_t xiaotai_kv_backend_get_int(xiaotai_kv_handle_t handle, const char *key, xiaotai_kv_int_t type,
                                         int64_t *value);
xiaotai_err_t xiaotai_kv_backend_set(xiaotai_kv_handle_t handle, const char *key, bool text, const void *data,
                                     size_t size);
/* value is within the type's range (the worker checks before queueing). */
xiaotai_err_t xiaotai_kv_backend_set_int(xiaotai_kv_handle_t handle, const char *key, xiaotai_kv_int_t type,
                                         int64_t value);
/* Removing an absent key succeeds. */
xiaotai_err_t xiaotai_kv_backend_erase(xiaotai_kv_handle_t handle, const char *key);
/* Every key of the namespace, as one operation. */
xiaotai_err_t xiaotai_kv_backend_erase_all(xiaotai_kv_handle_t handle);
/* Makes the writes made through the handle durable. */
xiaotai_err_t xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle);
