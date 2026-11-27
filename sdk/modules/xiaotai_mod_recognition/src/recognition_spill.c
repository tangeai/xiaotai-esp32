// SPDX-License-Identifier: MIT
#include "recognition_spill.h"

#include <stdatomic.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/scratch.h"

/* Erased ahead a block at a time (a block erase is far quicker than sixteen
 * sector erases; 64 KB is two seconds of 16 kHz audio), written in pieces. */
#define BLOCK (64U * 1024U)
#define PIECE_SAMPLES 2048U
#define PIECE_BYTES 1024U           /* one read: the upload asks for no more (RC_STREAM_CHUNK) */
#define STACK_BYTES 3072U
#define PRIORITY 2U
#define READ_WAIT_MS 3000U
#define IDLE_MS 200U

struct rc_spill {
    xiaotai_task_t task;
    xiaotai_signal_t wake, answered;
    xiaotai_mutex_t reading;
    int16_t *ring;
    uint8_t *piece;             /* internal: what a read brings back */
    uint32_t ring_samples, total;
    size_t area, erased;
    atomic_uint record_id, valid, written;
    atomic_bool finishing, stop, done, posted, read_pending;
    atomic_int error;
    size_t read_offset, read_bytes;
    xiaotai_err_t read_error;
    void (*flushed)(void *ctx);
    void *ctx;
};

static size_t rounded(size_t bytes) { return (bytes + XIAOTAI_SCRATCH_SECTOR - 1U) / XIAOTAI_SCRATCH_SECTOR * XIAOTAI_SCRATCH_SECTOR; }

bool rc_spill_possible(size_t bytes, uint32_t rate_hz)
{
    return rate_hz && bytes && xiaotai_scratch_size() >= rounded(bytes);
}

static void fail(rc_spill_t *s, xiaotai_err_t error)
{
    int none = XIAOTAI_OK;
    (void)atomic_compare_exchange_strong(&s->error, &none, (int)error);
}

/* One piece of what is recorded and not yet in flash; false: nothing to write. */
static bool write_some(rc_spill_t *s)
{
    const uint32_t valid = atomic_load(&s->valid), written = atomic_load(&s->written);
    if (written >= valid || atomic_load(&s->error) != XIAOTAI_OK) return false;
    const uint32_t at = written % s->ring_samples;
    uint32_t count = valid - written;
    if (count > PIECE_SAMPLES) count = PIECE_SAMPLES;
    if (count > s->ring_samples - at) count = s->ring_samples - at;
    const size_t end = (size_t)(written + count) * sizeof(int16_t);
    xiaotai_err_t error = XIAOTAI_OK;
    while (error == XIAOTAI_OK && s->erased < end) {
        const size_t bytes = s->area - s->erased < BLOCK ? s->area - s->erased : BLOCK;
        error = xiaotai_scratch_erase(s->erased, bytes);
        s->erased += bytes;
    }
    if (error == XIAOTAI_OK)
        error = xiaotai_scratch_write((size_t)written * sizeof(int16_t), s->ring + at, (size_t)count * sizeof(int16_t));
    if (error != XIAOTAI_OK) {
        fail(s, error);
        return false;
    }
    atomic_store(&s->written, written + count);
    xiaotai_media_record_taken(atomic_load(&s->record_id), written + count);
    return true;
}

static void release(rc_spill_t *s)
{
    xiaotai_signal_deinit(&s->wake);
    xiaotai_signal_deinit(&s->answered);
    xiaotai_mutex_deinit(&s->reading);
    xiaotai_mem_free(s->ring);
    xiaotai_mem_free(s->piece);
    xiaotai_mem_free(s);
}

static void run(void *arg)
{
    rc_spill_t *s = arg;
    while (!atomic_load(&s->stop)) {
        if (atomic_load(&s->read_pending)) {        /* the upload waits: it goes first */
            s->read_error = xiaotai_scratch_read(s->read_offset, s->piece, s->read_bytes);
            atomic_store(&s->read_pending, false);
            xiaotai_signal_give(&s->answered);
            continue;
        }
        if (write_some(s)) continue;
        if (atomic_load(&s->finishing) && !atomic_load(&s->posted) &&
            (atomic_load(&s->error) != XIAOTAI_OK || atomic_load(&s->written) >= atomic_load(&s->valid))) {
            /* Nothing more is written: the ring goes back before the upload, so the memory is
             * whole again for what follows it (the answer's speech wants one large block). */
            xiaotai_mem_free(s->ring);
            s->ring = NULL;
            atomic_store(&s->done, true);
            if (xiaotai_run(s->flushed, s->ctx) == XIAOTAI_OK) atomic_store(&s->posted, true);
        }
        (void)xiaotai_signal_take(&s->wake, IDLE_MS);
    }
    release(s);
    xiaotai_task_exit();
}

xiaotai_err_t rc_spill_start(rc_spill_t **spill, uint32_t samples, uint32_t rate_hz, int16_t **ring,
                             uint32_t *ring_samples, void (*flushed)(void *ctx), void *ctx)
{
    if (!spill || !ring || !ring_samples || !flushed || !samples || !rate_hz) return XIAOTAI_ERR_INVALID_ARG;
    *spill = NULL;
    const size_t area = rounded((size_t)samples * sizeof(int16_t));
    if (xiaotai_scratch_size() < area) return XIAOTAI_ERR_NOT_SUPPORTED;
    rc_spill_t *s = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, sizeof(*s));
    if (!s) return XIAOTAI_ERR_NO_MEM;
    s->ring_samples = RC_SPILL_RING_SECONDS * rate_hz < samples ? RC_SPILL_RING_SECONDS * rate_hz : samples;
    s->total = samples;
    s->area = area;
    s->flushed = flushed;
    s->ctx = ctx;
    atomic_init(&s->error, XIAOTAI_OK);
    s->ring = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (size_t)s->ring_samples * sizeof(int16_t));
    s->piece = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, PIECE_BYTES);
    xiaotai_err_t error = s->ring && s->piece ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
    bool wake = false, answered = false, reading = false;
    if (error == XIAOTAI_OK) error = xiaotai_signal_init(&s->wake);
    wake = error == XIAOTAI_OK;
    if (error == XIAOTAI_OK) error = xiaotai_signal_init(&s->answered);
    answered = wake && error == XIAOTAI_OK;
    if (error == XIAOTAI_OK) error = xiaotai_mutex_init(&s->reading);
    reading = answered && error == XIAOTAI_OK;
    if (error == XIAOTAI_OK) {
        /* Flash wants an internal stack. */
        const xiaotai_task_config_t task = {.name = "rc_spill", .entry = run, .arg = s, .stack_bytes = STACK_BYTES,
            .stack = XIAOTAI_MEM_INTERNAL, .priority = PRIORITY, .core = XIAOTAI_CORE_ANY};
        error = xiaotai_task_create(&task, &s->task);
    }
    if (error != XIAOTAI_OK) {
        if (wake) xiaotai_signal_deinit(&s->wake);
        if (answered) xiaotai_signal_deinit(&s->answered);
        if (reading) xiaotai_mutex_deinit(&s->reading);
        xiaotai_mem_free(s->ring);
        xiaotai_mem_free(s->piece);
        xiaotai_mem_free(s);
        return error;
    }
    *ring = s->ring;
    *ring_samples = s->ring_samples;
    *spill = s;
    return XIAOTAI_OK;
}

void rc_spill_recording(rc_spill_t *spill, uint32_t record_id)
{
    if (spill) atomic_store(&spill->record_id, record_id);
}

void rc_spill_feed(rc_spill_t *spill, uint32_t samples)
{
    if (!spill || samples > spill->total || samples <= atomic_load(&spill->valid)) return;
    atomic_store(&spill->valid, samples);
    xiaotai_signal_give(&spill->wake);
}

void rc_spill_finish(rc_spill_t *spill, uint32_t samples)
{
    if (!spill) return;
    rc_spill_feed(spill, samples);
    atomic_store(&spill->finishing, true);
    xiaotai_signal_give(&spill->wake);
}

bool rc_spill_flushed(const rc_spill_t *spill)
{
    return spill && atomic_load(&spill->done) && atomic_load(&spill->error) == XIAOTAI_OK;
}

xiaotai_err_t rc_spill_error(const rc_spill_t *spill)
{
    return spill ? (xiaotai_err_t)atomic_load(&spill->error) : XIAOTAI_ERR_INVALID_ARG;
}

xiaotai_err_t rc_spill_read(rc_spill_t *spill, size_t offset, void *data, size_t bytes)
{
    if (!spill || (!data && bytes)) return XIAOTAI_ERR_INVALID_ARG;
    if (offset > (size_t)atomic_load(&spill->written) * sizeof(int16_t) ||
        bytes > (size_t)atomic_load(&spill->written) * sizeof(int16_t) - offset) return XIAOTAI_ERR_INVALID_STATE;
    if (!xiaotai_mutex_lock(&spill->reading, READ_WAIT_MS)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_err_t error = XIAOTAI_OK;
    uint8_t *out = data;
    while (error == XIAOTAI_OK && bytes) {
        const size_t some = bytes < PIECE_BYTES ? bytes : PIECE_BYTES;
        (void)xiaotai_signal_take(&spill->answered, 0);      /* an answer that came too late */
        spill->read_offset = offset;
        spill->read_bytes = some;
        atomic_store(&spill->read_pending, true);
        xiaotai_signal_give(&spill->wake);
        if (!xiaotai_signal_take(&spill->answered, READ_WAIT_MS)) error = XIAOTAI_ERR_TIMEOUT;
        else if ((error = spill->read_error) == XIAOTAI_OK) memcpy(out, spill->piece, some);
        out += some;
        offset += some;
        bytes -= some;
    }
    xiaotai_mutex_unlock(&spill->reading);
    return error;
}

void rc_spill_stop(rc_spill_t *spill)
{
    if (!spill) return;
    atomic_store(&spill->stop, true);
    xiaotai_signal_give(&spill->wake);
}
