// SPDX-License-Identifier: MIT
#pragma once
/* Scratch: a stretch of flash for one large piece of temporary data that does
 * not fit in memory, such as a long recording on its way to a service. It is
 * erased and written in order, read back, and forgotten: nothing in it
 * survives a restart as far as anybody knows, and nobody else uses it.
 *
 * Where a port or a board has none, xiaotai_scratch_size() is 0 and every
 * other call returns XIAOTAI_ERR_NOT_SUPPORTED. On ESP-IDF it is the data
 * partition named XIAOTAI_SCRATCH_PARTITION, of any subtype, where the
 * board's partition table has one (a board adds it to have scratch).
 *
 * Every call needs an internal stack (xiaotai_require_internal_stack); erasing
 * and writing take the flash for milliseconds at a time: call them from a task
 * of your own, never from the owner. One user at a time: the caller keeps
 * track of what it wrote. */
#include <stddef.h>

#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The partition's name on ESP-IDF. */
#define XIAOTAI_SCRATCH_PARTITION "xt_scratch"
/* Erase unit: offsets and lengths of xiaotai_scratch_erase are multiples of it. */
#define XIAOTAI_SCRATCH_SECTOR 4096U

/* [any] Bytes of scratch; 0: none. */
size_t xiaotai_scratch_size(void);
/* [task with an internal stack] Erases [offset, offset + bytes), both multiples of the sector. */
xiaotai_err_t xiaotai_scratch_erase(size_t offset, size_t bytes);
/* [task with an internal stack] Writes into erased scratch. */
xiaotai_err_t xiaotai_scratch_write(size_t offset, const void *data, size_t bytes);
/* [task with an internal stack] Reads what was written. */
xiaotai_err_t xiaotai_scratch_read(size_t offset, void *data, size_t bytes);

#ifdef __cplusplus
}
#endif
