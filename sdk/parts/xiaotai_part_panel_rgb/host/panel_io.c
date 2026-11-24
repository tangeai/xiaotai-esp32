// SPDX-License-Identifier: MIT
#include "panel_io.h"
#include "xiaotai/port/mem.h"
#include <string.h>

/* Frame memory for product host tests; the renderer is not given native
 * device handles. The step-by-step fault test supplies its own IO table. */
typedef struct {
    void *pixels[2];
    size_t bytes;
    uint8_t count, shown;
} fake_panel_t;

static xiaotai_err_t panel_close(void *handle)
{
    fake_panel_t *p = handle;
    for (size_t i = 0; i < p->count; ++i) xiaotai_mem_free(p->pixels[i]);
    xiaotai_mem_free(p);
    return XIAOTAI_OK;
}

static xiaotai_err_t panel_open(const xiaotai_panel_rgb_config_t *c, void **out)
{
    const uint64_t bytes = (uint64_t)c->width * c->height * 2;
    if (bytes > SIZE_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    fake_panel_t *p = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*p));
    if (!p) return XIAOTAI_ERR_NO_MEM;
    p->bytes = (size_t)bytes;
    p->count = c->buffers;
    for (size_t i = 0; i < p->count; ++i) {
        p->pixels[i] = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, p->bytes);
        if (!p->pixels[i]) {
            (void)panel_close(p);
            return XIAOTAI_ERR_NO_MEM;
        }
    }
    *out = p;
    return XIAOTAI_OK;
}

static xiaotai_err_t enable(void *panel)
{
    (void)panel;
    return XIAOTAI_OK;
}

static xiaotai_err_t framebuffers(void *handle, uint8_t count, void *out[2])
{
    fake_panel_t *p = handle;
    if (count != p->count) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; ++i) out[i] = p->pixels[i];
    return XIAOTAI_OK;
}

static xiaotai_err_t present(void *handle, const xiaotai_rect_t *area, const void *pixels)
{
    (void)area;
    fake_panel_t *p = handle;
    for (uint8_t i = 0; i < p->count; ++i) {
        if (p->pixels[i] == pixels) {
            p->shown = i;
            return XIAOTAI_OK;
        }
    }
    memcpy(p->pixels[p->shown], pixels, p->bytes);
    return XIAOTAI_OK;
}

const rgb_io_t xiaotai_rgb_io = {
    .panel_open = panel_open, .panel_close = panel_close, .panel_enable = enable,
    .framebuffers = framebuffers, .present = present,
};
