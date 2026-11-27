// SPDX-License-Identifier: MIT
#pragma once
/* A recording that does not fit in memory goes to flash scratch
 * (xiaotai/port/scratch.h) while it is made: the media engine fills a ring of
 * a few seconds in memory, a task of the spill's own writes what is recorded
 * behind it and later reads it back for the upload. Flash wants a task with an
 * internal stack for both, so the upload's reader asks that task and waits. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiaotai/port/err.h"

typedef struct rc_spill rc_spill_t;

/* Seconds the ring holds: room for the flash being slow for a moment. */
#define RC_SPILL_RING_SECONDS 4U

/* [owner] Whether `bytes` of recording can go to scratch here, with the
 * memory the ring needs. */
bool rc_spill_possible(size_t bytes, uint32_t rate_hz);
/* [owner] Starts the writer for `samples` samples at `rate_hz`. *ring and
 * *ring_samples are what the media engine records into. `flushed` runs on
 * the owner once after rc_spill_finish(), when all of it is in flash or
 * writing failed (rc_spill_error()). */
xiaotai_err_t rc_spill_start(rc_spill_t **spill, uint32_t samples, uint32_t rate_hz, int16_t **ring,
                             uint32_t *ring_samples, void (*flushed)(void *ctx), void *ctx);
/* [owner] The media engine's recording id, for giving back what was written. */
void rc_spill_recording(rc_spill_t *spill, uint32_t record_id);
/* [owner] So many samples are recorded: write them. */
void rc_spill_feed(rc_spill_t *spill, uint32_t samples);
/* [owner] No more come after `samples`. */
void rc_spill_finish(rc_spill_t *spill, uint32_t samples);
/* [owner] All of it is in flash. */
bool rc_spill_flushed(const rc_spill_t *spill);
/* [any] The first failure, XIAOTAI_OK while there is none. */
xiaotai_err_t rc_spill_error(const rc_spill_t *spill);
/* [any task but the spill's] Reads written bytes back, waiting for the spill's
 * task (flash wants an internal stack, which the caller may not have). */
xiaotai_err_t rc_spill_read(rc_spill_t *spill, size_t offset, void *data, size_t bytes);
/* [owner] Done with it: the task ends and frees everything, the ring included,
 * once it has finished what it is doing. Nothing may use the ring after this. */
void rc_spill_stop(rc_spill_t *spill);
