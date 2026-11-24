// SPDX-License-Identifier: MIT
#include "camera_csi_io.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

typedef struct {
    const char *stage;
    xiaotai_err_t error;
    int native_errno, slot;
    size_t residue;
} camera_csi_open_failure_t;

typedef struct {
    xiaotai_camera_t camera;
    xiaotai_camera_csi_config_t config;
    xiaotai_mutex_t control;
    atomic_uint opens, open_failures, frames, stale_frames, bad_frames, rate_refused;
    atomic_size_t pool_bytes;
    size_t visible, capacity, image_size, alignment;
    camera_csi_open_failure_t first_open_failure;
    uint16_t rows;
    int fd;
    xiaotai_err_t close_error;
    bool powered, ready, verified, requested, streaming, broken;
    xiaotai_camera_frame_t *loan;
    unsigned loan_index;
    void *pool[];
} camera_csi_t;

static xiaotai_err_t pool_prepare(camera_csi_t *camera)
{
    if (camera->pool[0]) return XIAOTAI_OK;
    size_t alignment = 0;
    xiaotai_err_t err = camera_csi_io_alignment(&alignment);
    if (err != XIAOTAI_OK) return err;
    if (!alignment || (alignment & (alignment - 1U))) return XIAOTAI_ERR_INVALID_SIZE;
    /* esp_video USERPTR uses the external cache line, not the CPU's L1 line. */
    camera->alignment = alignment < 64U ? 64U : alignment;
    const unsigned n = camera->config.buffers;
    for (unsigned i = 0; i < n; ++i) {
        camera->pool[i] = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, camera->alignment, camera->capacity);
        if (!camera->pool[i] || !xiaotai_mem_is_external(camera->pool[i])) {
            for (unsigned j = 0; j <= i; ++j) {
                xiaotai_mem_free(camera->pool[j]);
                camera->pool[j] = NULL;
            }
            return XIAOTAI_ERR_NO_MEM;
        }
    }
    atomic_store_explicit(&camera->pool_bytes, camera->capacity * n, memory_order_relaxed);
    return XIAOTAI_OK;
}

static xiaotai_err_t power_set(camera_csi_t *camera, bool on)
{
    if (camera->powered == on || (!on && camera->ready && camera->config.keep_powered)) return XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_line_set(&camera->config.power, on);
    if (err != XIAOTAI_OK) return err;
    camera->powered = on;
    camera->verified = false;
    if (on && camera->config.power_settle_ms) xiaotai_delay_ms(camera->config.power_settle_ms);
    return XIAOTAI_OK;
}

static xiaotai_err_t power_up(camera_csi_t *camera)
{
    xiaotai_err_t err = pool_prepare(camera);
    if (err == XIAOTAI_OK) err = power_set(camera, true);
    if (err == XIAOTAI_OK && !camera->ready) {
        err = camera_csi_io_start(camera, &camera->config, camera->config.sccb_hz);
        camera->ready = err == XIAOTAI_OK;
    }
    if (err == XIAOTAI_OK && !camera->verified) {
        uint8_t hi = 0, lo = 0;
        if (camera->config.sensor_id) {
            const uint16_t reg = camera->config.sensor_id_reg;
            err = camera_csi_io_read(camera, reg, &hi);
            if (err == XIAOTAI_OK) err = camera_csi_io_read(camera, reg + 1U, &lo);
            if (err == XIAOTAI_OK && (((unsigned)hi << 8) | lo) != camera->config.sensor_id) {
                err = XIAOTAI_ERR_NOT_SUPPORTED;
            }
        }
        camera->verified = err == XIAOTAI_OK;
    }
    return err;
}

static xiaotai_err_t csi_prepare(void *self)
{
    camera_csi_t *camera = self;
    xiaotai_mutex_lock(&camera->control, XIAOTAI_WAIT_FOREVER);
    xiaotai_err_t err = camera->close_error;
    if (err == XIAOTAI_OK && camera->fd >= 0) err = XIAOTAI_ERR_INVALID_STATE;
    if (err == XIAOTAI_OK) {
        err = power_up(camera);
        xiaotai_err_t off = power_set(camera, false);
        if (err == XIAOTAI_OK) err = off;
    }
    xiaotai_mutex_unlock(&camera->control);
    return err;
}

static xiaotai_err_t csi_power(void *self, bool on)
{
    camera_csi_t *camera = self;
    xiaotai_mutex_lock(&camera->control, XIAOTAI_WAIT_FOREVER);
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    if (camera->close_error != XIAOTAI_OK) {
        err = camera->close_error;
    } else if (camera->fd < 0) {
        err = on ? power_up(camera) : power_set(camera, false);
        if (on && err != XIAOTAI_OK) {
            xiaotai_err_t off = power_set(camera, false);
            if (off != XIAOTAI_OK) XIAOTAI_LOGW("camera.csi", "power rollback failed: 0x%x", (unsigned)off);
        }
    }
    xiaotai_mutex_unlock(&camera->control);
    return err;
}

static xiaotai_err_t apply_setup(camera_csi_t *camera)
{
    for (size_t i = 0; i < camera->config.setup_count; ++i) {
        const xiaotai_sensor_reg_t *change = &camera->config.setup[i];
        if (!change->mask) continue;
        uint8_t value = 0;
        xiaotai_err_t err = XIAOTAI_OK;
        if (change->mask != UINT8_MAX) err = camera_csi_io_read(camera, change->reg, &value);
        if (err != XIAOTAI_OK) return err;
        value = (value & (uint8_t)~change->mask) | (change->value & change->mask);
        err = camera_csi_io_write(camera, change->reg, value);
        if (err == XIAOTAI_OK) err = camera_csi_io_read(camera, change->reg, &value);
        if (err != XIAOTAI_OK) return err;
        if (((value ^ change->value) & change->mask) != 0) return XIAOTAI_FAIL;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t park_device(camera_csi_t *camera)
{
    if (camera->close_error != XIAOTAI_OK) return camera->close_error;
    xiaotai_err_t result = XIAOTAI_OK;
    if (camera->streaming) {
        result = camera_csi_io_stream(camera->fd, false);
        if (result != XIAOTAI_OK) {
            /* The producer can still own DMA/USERPTR storage. Do not free its
             * metadata, consume the fd, or power it down after a failed stop. */
            camera->broken = true;
            return result;
        }
        camera->streaming = false;
    }
    if (camera->requested) {
        xiaotai_err_t err = camera_csi_io_request(camera->fd, 0, NULL);
        if (err == XIAOTAI_OK) camera->requested = false;
        if (result == XIAOTAI_OK) result = err;
    }
    if (camera->fd >= 0) {
        xiaotai_err_t err = camera_csi_io_close(camera->fd);
        camera->fd = -1;
        if (err == XIAOTAI_OK) {
            camera->requested = false;
            camera->broken = false;
        } else {
            /* VFS consumes the descriptor even when device deinit fails;
             * opening or powering off now would assume ownership we lost. */
            camera->close_error = err;
        }
        if (result == XIAOTAI_OK) result = err;
    }
    return result;
}

static xiaotai_err_t open_failure(camera_csi_t *camera, const char *stage, xiaotai_err_t error,
                                  int native_errno, int slot, size_t residue)
{
    if (!camera->first_open_failure.stage) {
        camera->first_open_failure = (camera_csi_open_failure_t){.stage = stage, .error = error,
            .native_errno = native_errno, .slot = slot, .residue = residue};
    }
    return error;
}

static xiaotai_err_t open_stream(camera_csi_t *camera, uint8_t fps)
{
    xiaotai_err_t err = camera_csi_io_open(&camera->fd);
    if (err != XIAOTAI_OK) return open_failure(camera, "open", err, camera_csi_io_errno(), -1, 0);
    err = camera_csi_io_format(camera->fd, camera->config.width, camera->config.height,
                               camera->config.full_range, &camera->image_size);
    if (err != XIAOTAI_OK) return open_failure(camera, "format", err, 0, -1, 0);
    if (camera->image_size < camera->visible || camera->image_size > camera->capacity)
        return open_failure(camera, "format-size", XIAOTAI_ERR_INVALID_SIZE, 0, -1, 0);
    if (!camera->config.fixed_fps && fps && camera_csi_io_rate(camera->fd, fps) != XIAOTAI_OK) {
        atomic_fetch_add_explicit(&camera->rate_refused, 1, memory_order_relaxed);
    }
    err = apply_setup(camera);
    if (err != XIAOTAI_OK) return open_failure(camera, "setup", err, 0, -1, 0);
    unsigned actual = 0;
    const unsigned n = camera->config.buffers;
    err = camera_csi_io_request(camera->fd, n, &actual);
    if (err != XIAOTAI_OK) return open_failure(camera, "reqbufs", err, camera_csi_io_errno(), -1, 0);
    camera->requested = true;
    if (actual < n) return open_failure(camera, "reqbufs-count", XIAOTAI_ERR_NO_MEM, 0, -1, 0);
    for (unsigned i = 0; i < n; ++i) {
        size_t bytes;
        err = camera_csi_io_buffer_size(camera->fd, i, &bytes);
        if (err != XIAOTAI_OK)
            return open_failure(camera, "querybuf", err, camera_csi_io_errno(), (int)i, 0);
        if (bytes < camera->image_size || bytes > camera->capacity)
            return open_failure(camera, "querybuf-size", XIAOTAI_ERR_INVALID_SIZE, 0, (int)i, 0);
        err = camera_csi_io_queue(camera->fd, i, camera->pool[i], camera->capacity);
        if (err != XIAOTAI_OK) return open_failure(camera, "qbuf", err, camera_csi_io_errno(), (int)i,
            (uintptr_t)camera->pool[i] % camera->alignment);
    }
    err = camera_csi_io_stream(camera->fd, true);
    camera->streaming = err == XIAOTAI_OK;
    if (err != XIAOTAI_OK)
        return open_failure(camera, "streamon", err, camera_csi_io_errno(), -1, 0);
    return err;
}

static xiaotai_err_t csi_open(void *self, const xiaotai_camera_mode_t *mode)
{
    if (!mode) return XIAOTAI_ERR_INVALID_ARG;
    camera_csi_t *camera = self;
    xiaotai_mutex_lock(&camera->control, XIAOTAI_WAIT_FOREVER);
    xiaotai_err_t err = camera->close_error != XIAOTAI_OK ? camera->close_error : XIAOTAI_ERR_INVALID_STATE;
    if (!camera->close_error && camera->fd < 0 && camera->powered && camera->verified) {
        if (mode->format != XIAOTAI_CAMERA_YUV420_PAIRED || mode->width != camera->config.width ||
            mode->height != camera->config.height) {
            err = XIAOTAI_ERR_NOT_SUPPORTED;
        } else {
            err = open_stream(camera, mode->fps);
            if (err != XIAOTAI_OK) {
                xiaotai_err_t unwind = park_device(camera);
                if (unwind != XIAOTAI_OK) {
                    XIAOTAI_LOGW("camera.csi", "open failure 0x%x, cleanup failure 0x%x", (unsigned)err,
                                 (unsigned)unwind);
                }
            }
        }
    }
    atomic_fetch_add_explicit(err == XIAOTAI_OK ? &camera->opens : &camera->open_failures, 1, memory_order_relaxed);
    xiaotai_mutex_unlock(&camera->control);
    return err;
}

static xiaotai_err_t csi_close(void *self)
{
    camera_csi_t *camera = self;
    xiaotai_mutex_lock(&camera->control, XIAOTAI_WAIT_FOREVER);
    xiaotai_err_t err = camera->loan ? XIAOTAI_ERR_INVALID_STATE : park_device(camera);
    xiaotai_mutex_unlock(&camera->control);
    return err;
}

static xiaotai_err_t requeue(camera_csi_t *camera, unsigned index)
{
    xiaotai_err_t err = camera_csi_io_queue(camera->fd, index, camera->pool[index], camera->capacity);
    if (err != XIAOTAI_OK) camera->broken = true;
    return err;
}

static void pad_rows(const camera_csi_t *camera, unsigned index)
{
    if (camera->visible == camera->capacity) return;
    uint8_t *data = camera->pool[index];
    const size_t pair = (size_t)camera->config.width * 3U;
    /* A device-reported tail can contain stale DMA bytes; none are visible. */
    for (size_t offset = camera->visible; offset < camera->capacity; offset += pair) {
        memcpy(data + offset, data + camera->visible - pair, pair);
    }
}

static xiaotai_err_t borrow_latest(camera_csi_t *camera, xiaotai_camera_frame_t *frame)
{
    if (!camera->streaming || camera->loan || camera->broken) return XIAOTAI_ERR_INVALID_STATE;
    camera_csi_buffer_t candidate = {0};
    bool held = false;
    unsigned skipped = 0;
    xiaotai_err_t err = XIAOTAI_ERR_NOT_FOUND;
    /* A live producer cannot keep this drain running beyond one pool. */
    for (unsigned i = 0; i < camera->config.buffers; ++i) {
        camera_csi_buffer_t next;
        err = camera_csi_io_dequeue(camera->fd, &next);
        if (err == XIAOTAI_ERR_NOT_FOUND) {
            if (held) err = XIAOTAI_OK;
            break;
        }
        if (err != XIAOTAI_OK) break;
        if (next.index >= camera->config.buffers || (held && next.index == candidate.index)) {
            atomic_fetch_add_explicit(&camera->bad_frames, 1, memory_order_relaxed);
            camera->broken = true;
            err = XIAOTAI_FAIL;
            break;
        }
        if (next.error) {
            atomic_fetch_add_explicit(&camera->bad_frames, 1, memory_order_relaxed);
            err = requeue(camera, next.index);
            if (err == XIAOTAI_OK && !held) err = XIAOTAI_FAIL;
            break;
        }
        if (held) {
            err = requeue(camera, candidate.index);
            if (err != XIAOTAI_OK) {
                (void)requeue(camera, next.index);
                held = false;
                break;
            }
            ++skipped;
            atomic_fetch_add_explicit(&camera->stale_frames, 1, memory_order_relaxed);
        }
        candidate = next;
        held = true;
    }
    const size_t bytes = candidate.bytes ? candidate.bytes : camera->image_size;
    if (err == XIAOTAI_OK && (bytes < camera->visible || bytes > camera->capacity)) {
        atomic_fetch_add_explicit(&camera->bad_frames, 1, memory_order_relaxed);
        err = XIAOTAI_ERR_INVALID_SIZE;
    }
    if (err != XIAOTAI_OK) {
        if (held) {
            xiaotai_err_t recycle = requeue(camera, candidate.index);
            if (recycle != XIAOTAI_OK) err = recycle;
        }
        return err;
    }
    pad_rows(camera, candidate.index);
    *frame = (xiaotai_camera_frame_t){
        .data = camera->pool[candidate.index], .size = camera->capacity,
        .width = camera->config.width, .height = camera->config.height, .rows = camera->rows,
        .format = XIAOTAI_CAMERA_YUV420_PAIRED, .captured_us = candidate.captured_us,
        .sequence = candidate.sequence, .skipped = skipped,
    };
    camera->loan = frame;
    camera->loan_index = candidate.index;
    atomic_fetch_add_explicit(&camera->frames, 1, memory_order_relaxed);
    return XIAOTAI_OK;
}

static xiaotai_err_t csi_borrow(void *self, xiaotai_camera_frame_t *frame)
{
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    camera_csi_t *camera = self;
    if (!xiaotai_mutex_lock(&camera->control, 0)) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_err_t err = borrow_latest(camera, frame);
    xiaotai_mutex_unlock(&camera->control);
    return err;
}

static xiaotai_err_t csi_release(void *self, xiaotai_camera_frame_t *frame)
{
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    camera_csi_t *camera = self;
    xiaotai_mutex_lock(&camera->control, XIAOTAI_WAIT_FOREVER);
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    if (camera->loan == frame) {
        err = requeue(camera, camera->loan_index);
        camera->loan = NULL;
        *frame = (xiaotai_camera_frame_t){0};
    }
    xiaotai_mutex_unlock(&camera->control);
    return err;
}

xiaotai_err_t xiaotai_camera_csi_create(const xiaotai_camera_csi_config_t *config, const xiaotai_camera_t **out)
{
    if (out) *out = NULL;
    if (!config || !out || !config->sccb || !config->sensor_address || config->sensor_address > 0x7f ||
        !config->width || !config->height || (config->width % 2U) || (config->height % 2U) || config->rotation > 3U ||
        (config->setup_count && !config->setup) || (config->sensor_id && config->sensor_id_reg == UINT16_MAX) ||
        config->sccb_hz > 400000U) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    const unsigned align = config->row_align ? config->row_align : 1U;
    const unsigned rows = ((unsigned)config->height + align - 1U) / align * align;
    const size_t pair = (size_t)config->width * 3U;
    const unsigned n = config->buffers ? config->buffers : 3U;
    if (rows > UINT16_MAX || rows % 2U || rows / 2U > SIZE_MAX / pair || pair * (rows / 2U) > SIZE_MAX / n) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    if (!xiaotai_mem_has_external()) return XIAOTAI_ERR_NOT_SUPPORTED;
    camera_csi_t *camera = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*camera) + n * sizeof(void *));
    if (!camera) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t err = xiaotai_mutex_init(&camera->control);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(camera);
        return err;
    }
    err = xiaotai_line_set(&config->power, false);
    if (err != XIAOTAI_OK) {
        xiaotai_mutex_deinit(&camera->control);
        xiaotai_mem_free(camera);
        return err;
    }
    camera->config = *config;
    camera->config.buffers = n;
    if (!camera->config.sccb_hz) camera->config.sccb_hz = 100000;
    camera->rows = rows;
    camera->capacity = pair * (rows / 2U);
    camera->visible = pair * (config->height / 2U);
    camera->fd = -1;
    atomic_init(&camera->pool_bytes, 0);
    atomic_init(&camera->opens, 0);
    atomic_init(&camera->open_failures, 0);
    atomic_init(&camera->frames, 0);
    atomic_init(&camera->stale_frames, 0);
    atomic_init(&camera->bad_frames, 0);
    atomic_init(&camera->rate_refused, 0);
    camera->camera = (xiaotai_camera_t){
        .desc = {.formats = XIAOTAI_CAMERA_YUV420_PAIRED,
                 .min_width = config->width, .max_width = config->width,
                 .min_height = config->height, .max_height = config->height,
                 .max_fps = config->fps, .rotation = config->rotation, .row_align = config->row_align,
                 .flags = config->full_range ? XIAOTAI_CAMERA_FULL_RANGE : 0},
        .prepare = csi_prepare, .set_power = csi_power, .open = csi_open, .close = csi_close,
        .borrow = csi_borrow, .release = csi_release, .self = camera,
    };
    *out = &camera->camera;
    return XIAOTAI_OK;
}

void xiaotai_camera_csi_stats(const xiaotai_camera_t *part, xiaotai_camera_csi_stats_t *stats)
{
    if (!part || !stats) return;
    const camera_csi_t *camera = part->self;
    *stats = (xiaotai_camera_csi_stats_t){
        .opens = atomic_load_explicit(&camera->opens, memory_order_relaxed),
        .open_failures = atomic_load_explicit(&camera->open_failures, memory_order_relaxed),
        .frames = atomic_load_explicit(&camera->frames, memory_order_relaxed),
        .stale_frames = atomic_load_explicit(&camera->stale_frames, memory_order_relaxed),
        .bad_frames = atomic_load_explicit(&camera->bad_frames, memory_order_relaxed),
        .rate_refused = atomic_load_explicit(&camera->rate_refused, memory_order_relaxed),
        .pool_bytes = atomic_load_explicit(&camera->pool_bytes, memory_order_relaxed),
    };
}

void xiaotai_camera_csi_report(const xiaotai_camera_t *part, xiaotai_hal_write_fn_t write, void *ctx)
{
    if (!part || !write) return;
    camera_csi_t *camera = part->self;
    camera_csi_quality_t q = {0};
    camera_csi_open_failure_t failure = {0};
    size_t alignment = 0;
    xiaotai_err_t quality = XIAOTAI_ERR_BUSY;
    if (xiaotai_mutex_lock(&camera->control, 0)) {
        failure = camera->first_open_failure;
        alignment = camera->alignment;
        quality = XIAOTAI_ERR_INVALID_STATE;
        if (camera->close_error) {
            quality = camera->close_error;
        } else if (camera->powered && camera->verified && !camera->broken) {
            const bool idle = camera->fd < 0;
            quality = idle ? camera_csi_io_open(&camera->fd) : XIAOTAI_OK;
            if (quality == XIAOTAI_OK) quality = camera_csi_io_quality(camera->fd, &q);
            if (idle) {
                xiaotai_err_t cleanup = park_device(camera);
                if (quality == XIAOTAI_OK) quality = cleanup;
            }
        }
        xiaotai_mutex_unlock(&camera->control);
    }
    xiaotai_camera_csi_stats_t s;
    xiaotai_camera_csi_stats(part, &s);
    char line[216];
    (void)snprintf(line, sizeof(line), "camera.csi opens=%" PRIu32 " open_failures=%" PRIu32 " frames=%" PRIu32
                   " stale_frames=%" PRIu32 " bad_frames=%" PRIu32 " rate_refused=%" PRIu32 " pool_bytes=%zu",
                   s.opens, s.open_failures, s.frames, s.stale_frames, s.bad_frames, s.rate_refused, s.pool_bytes);
    write(ctx, line);
    if (failure.stage) {
        (void)snprintf(line, sizeof(line), "camera.csi first_open_failure=%s error=0x%x errno=%d"
                       " slot=%d userptr_alignment=%zu userptr_residue=%zu", failure.stage,
                       (unsigned)failure.error, failure.native_errno, failure.slot, alignment, failure.residue);
        write(ctx, line);
    } else if (alignment) {
        (void)snprintf(line, sizeof(line), "camera.csi userptr_alignment=%zu", alignment);
        write(ctx, line);
    }
    if (quality == XIAOTAI_OK) {
        (void)snprintf(line, sizeof(line), "camera.csi cached=1 same_frame=0 exposure=%" PRId32 " exposure_min=%" PRId64
                       " exposure_max=%" PRId64 " gain=%" PRId32 " gain_min=%" PRId64 " gain_max=%" PRId64,
                       q.exposure, q.exposure_min, q.exposure_max, q.gain, q.gain_min, q.gain_max);
        write(ctx, line);
        if (q.has_gain_menu) {
            (void)snprintf(line, sizeof(line), "camera.csi gain_menu=%" PRId64 " gain_base=%" PRId64
                           " gain_limit=%" PRId64, q.gain_value, q.gain_base, q.gain_limit);
            write(ctx, line);
        }
    } else if (quality != XIAOTAI_ERR_INVALID_STATE) {
        (void)snprintf(line, sizeof(line), "camera.csi quality_error=0x%x", (unsigned)quality);
        write(ctx, line);
    }
}
