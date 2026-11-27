// SPDX-License-Identifier: MIT
#include "panel_io.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

typedef struct {
    xiaotai_panel_rgb_config_t config;
    void *panel;
    xiaotai_display_handles_t handles;
    bool ready, latched;
} rgb_t;

static xiaotai_err_t stop(void *self)
{
    rgb_t *p = self;
    if (p->latched) return XIAOTAI_ERR_INVALID_STATE;
    p->ready = false;
    p->handles = (xiaotai_display_handles_t){0};
    if (!p->panel) return XIAOTAI_OK;
    const xiaotai_err_t err = xiaotai_rgb_io.panel_close(p->panel);
    if (err == XIAOTAI_OK) p->panel = NULL;
    else p->latched = true;
    return err;
}

static xiaotai_err_t reset_panel(const xiaotai_panel_rgb_config_t *c)
{
    if (!c->reset.set) return XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_line_set(&c->reset, true);
    if (err == XIAOTAI_OK && c->reset_hold_ms) xiaotai_delay_ms(c->reset_hold_ms);
    /* Try deasserting even if assertion reported failure. */
    const xiaotai_err_t deassert = xiaotai_line_set(&c->reset, false);
    if (err == XIAOTAI_OK) err = deassert;
    if (err == XIAOTAI_OK && c->reset_settle_ms) xiaotai_delay_ms(c->reset_settle_ms);
    return err;
}

static xiaotai_err_t start(void *self, xiaotai_display_handles_t *out)
{
    rgb_t *p = self;
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = (xiaotai_display_handles_t){0};
    if (p->latched) return XIAOTAI_ERR_INVALID_STATE;
    if (p->ready) {
        *out = p->handles;
        return XIAOTAI_OK;
    }
    const xiaotai_panel_rgb_config_t *c = &p->config;
    const rgb_io_t *io = &xiaotai_rgb_io;
    xiaotai_err_t err = reset_panel(c);
    if (err != XIAOTAI_OK) {
        p->latched = true;
        return err;
    }
    err = io->panel_open(c, &p->panel);
    if (err == XIAOTAI_OK) err = io->panel_enable(p->panel);
    xiaotai_display_handles_t handles = {0};
    if (err == XIAOTAI_OK) err = io->framebuffers(p->panel, c->buffers, handles.framebuffers);
    if (err == XIAOTAI_OK && (!handles.framebuffers[0] || (c->buffers == 2 && !handles.framebuffers[1])))
        err = XIAOTAI_ERR_INVALID_STATE;
    if (err != XIAOTAI_OK) {
        (void)stop(p);
        return err;
    }
    if (io->native_handles) io->native_handles(p->panel, &handles);
    handles.scanout_kind = XIAOTAI_SCANOUT_RGB;
    p->ready = true;
    *out = p->handles = handles;
    return XIAOTAI_OK;
}

static xiaotai_err_t present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    rgb_t *p = self;
    if (!p->ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!area || !pixels || area->x || area->y || area->width != p->config.width ||
        area->height != p->config.height) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_rgb_io.present(p->panel, area, pixels);
}

static bool cleanup_failed(void *self) { return ((const rgb_t *)self)->latched; }

static bool pins_valid(const xiaotai_panel_rgb_config_t *c)
{
    if (c->hsync_pin < 0 || c->vsync_pin < 0 || c->pclk_pin < 0) return false;
    if (c->data_lines != 16) return false;
    for (size_t i = 0; i < c->data_lines; ++i)
        if (c->data_pins[i] < 0) return false;
    return true;
}

xiaotai_err_t xiaotai_panel_rgb_create(const xiaotai_panel_rgb_config_t *config, xiaotai_panel_t *panel)
{
    if (!panel) return XIAOTAI_ERR_INVALID_ARG;
    *panel = (xiaotai_panel_t){0};
    if (!config || !config->width || !config->height || !config->pixel_clock_hz ||
        !config->buffers || config->buffers > 2 || !pins_valid(config) ||
        (xiaotai_line_connected(&config->reset) && !config->reset.set))
        return XIAOTAI_ERR_INVALID_ARG;
    rgb_t *p = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*p));
    if (!p) return XIAOTAI_ERR_NO_MEM;
    p->config = *config;
    *panel = (xiaotai_panel_t){.self = p, .start = start, .stop = stop, .present = present,
                              .cleanup_failed = cleanup_failed};
    return XIAOTAI_OK;
}
