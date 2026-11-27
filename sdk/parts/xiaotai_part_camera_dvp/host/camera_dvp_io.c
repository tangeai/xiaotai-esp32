// SPDX-License-Identifier: MIT
#include "camera_dvp_fake.h"
#include "xiaotai/port/mem.h"
#include <assert.h>

typedef struct {
    uint8_t *bytes;
    xt_dvp_frame_t frame;
    uint32_t order;
    bool queued, borrowed;
} buffer_t;

struct xt_dvp_io {
    buffer_t pool[XT_DVP_BUFFERS];
    uint32_t sequence, dropped;
    bool open;
};

static xt_dvp_io_t fake;
static xt_dvp_fake_script_t script;
static xt_dvp_fake_status_t state;

void xt_dvp_fake_script(const xt_dvp_fake_script_t *value) { script = value ? *value : (xt_dvp_fake_script_t){0}; }
xt_dvp_fake_status_t xt_dvp_fake_status(void) { return state; }

xiaotai_err_t xt_dvp_io_open(const xiaotai_camera_dvp_config_t *config, xt_dvp_size_t size, xt_dvp_io_t **io)
{
    static const uint32_t frame_bytes[] = {160U * 120U * 2U, 240U * 176U * 2U, 320U * 240U * 2U};
    *io = NULL;
    if (fake.open) return XIAOTAI_ERR_INVALID_STATE;
    ++state.opens;
    state.config = *config;
    state.size = size;
    if (script.open_error != XIAOTAI_OK && !script.partial_open) return script.open_error;
    for (unsigned i = 0; i < XT_DVP_BUFFERS; ++i) {
        fake.pool[i].bytes = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, frame_bytes[size]);
        if (!fake.pool[i].bytes) {
            for (unsigned j = 0; j < i; ++j) xiaotai_mem_free(fake.pool[j].bytes);
            fake = (xt_dvp_io_t){0};
            return XIAOTAI_ERR_NO_MEM;
        }
    }
    fake.open = true;
    *io = &fake;
    return script.open_error;
}

xiaotai_err_t xt_dvp_io_sensor(xt_dvp_io_t *io, uint16_t *sensor_id, bool *external_dma)
{
    assert(io == &fake && io->open);
    if (script.sensor_missing) return XIAOTAI_ERR_NOT_SUPPORTED;
    *sensor_id = script.sensor_id ? script.sensor_id : state.config.sensor_id;
    *external_dma = script.external_dma;
    return XIAOTAI_OK;
}

xiaotai_err_t xt_dvp_io_upright(xt_dvp_io_t *io)
{
    assert(io == &fake && io->open);
    ++state.mirror;
    if (script.fail_mirror) return XIAOTAI_FAIL;
    ++state.flips;
    return script.fail_flip ? XIAOTAI_FAIL : XIAOTAI_OK;
}

xiaotai_err_t xt_dvp_io_close(xt_dvp_io_t *io)
{
    assert(io == &fake && io->open);
    ++state.closes;
    if (script.close_error != XIAOTAI_OK) return script.close_error;
    for (unsigned i = 0; i < XT_DVP_BUFFERS; ++i) {
        assert(!io->pool[i].borrowed);
        xiaotai_mem_free(io->pool[i].bytes);
    }
    *io = (xt_dvp_io_t){0};
    return XIAOTAI_OK;
}

void xt_dvp_fake_frame(uint8_t first, size_t size, size_t width, size_t height, uint32_t format, int64_t stamp)
{
    assert(fake.open);
    buffer_t *use = NULL;
    for (unsigned i = 0; i < XT_DVP_BUFFERS; ++i) {
        buffer_t *b = &fake.pool[i];
        if (b->borrowed) continue;
        if (!use || b->order < use->order) use = b;
    }
    assert(use);
    /* The driver has two actual buffers, but GRAB_LATEST keeps just one
     * queued (frame count minus one). The producer discards an older one. */
    for (unsigned i = 0; i < XT_DVP_BUFFERS; ++i) {
        if (fake.pool[i].queued) ++fake.dropped;
        fake.pool[i].queued = false;
    }
    use->bytes[0] = first;
    use->frame = (xt_dvp_frame_t){.token = use, .data = use->bytes, .size = size, .width = width,
        .height = height, .format = format, .timestamp_us = stamp};
    use->order = ++fake.sequence;
    use->queued = true;
}

xiaotai_err_t xt_dvp_io_take(xt_dvp_io_t *io, xt_dvp_frame_t *out)
{
    assert(io == &fake && io->open);
    ++state.takes;
    buffer_t *newest = NULL;
    unsigned queued = 0;
    for (unsigned i = 0; i < XT_DVP_BUFFERS; ++i) {
        buffer_t *b = &io->pool[i];
        if (!b->queued) continue;
        ++queued;
        if (!newest || b->order > newest->order) newest = b;
        b->queued = false;
    }
    if (!newest) return XIAOTAI_ERR_NOT_FOUND;
    newest->borrowed = true;
    *out = newest->frame;
    out->skipped = io->dropped + queued - 1U;
    io->dropped = 0;
    return XIAOTAI_OK;
}

void xt_dvp_io_return(xt_dvp_io_t *io, void *token)
{
    assert(io == &fake && io->open);
    buffer_t *b = token;
    assert((b == &io->pool[0] || b == &io->pool[1]) && b->borrowed);
    b->borrowed = false;
    ++state.returns;
}
