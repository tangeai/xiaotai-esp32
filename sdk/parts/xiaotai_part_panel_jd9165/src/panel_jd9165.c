// SPDX-License-Identifier: MIT
#include "panel_io.h"
#include "xiaotai/port/os.h"

typedef struct {
    xiaotai_panel_jd9165_config_t config;
    void *phy, *bus, *io, *panel;
    xiaotai_display_handles_t handles;
    bool ready, latched;
} jd9165_t;

static xiaotai_err_t release(void **handle, xiaotai_err_t (*close_handle)(void *))
{
    if (!*handle) return XIAOTAI_OK;
    const xiaotai_err_t err = close_handle(*handle);
    if (err == XIAOTAI_OK) *handle = NULL;
    return err;
}

static xiaotai_err_t stop(void *self)
{
    jd9165_t *p = self;
    if (p->latched) return XIAOTAI_ERR_INVALID_STATE;
    p->ready = false;
    p->handles = (xiaotai_display_handles_t){0};
    const jd9165_io_t *io = &xiaotai_jd9165_io;
    /* A child that did not release may still address its parent. */
    xiaotai_err_t err = release(&p->panel, io->panel_close);
    if (err == XIAOTAI_OK) err = release(&p->io, io->io_close);
    if (err == XIAOTAI_OK) err = release(&p->bus, io->bus_close);
    if (err == XIAOTAI_OK) err = release(&p->phy, io->phy_close);
    if (err != XIAOTAI_OK) p->latched = true;
    return err;
}

static xiaotai_err_t start(void *self, xiaotai_display_handles_t *out)
{
    jd9165_t *p = self;
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = (xiaotai_display_handles_t){0};
    if (p->latched) return XIAOTAI_ERR_INVALID_STATE;
    if (p->ready) {
        *out = p->handles;
        return XIAOTAI_OK;
    }
    const xiaotai_panel_jd9165_config_t *c = &p->config;
    const jd9165_io_t *io = &xiaotai_jd9165_io;
    xiaotai_err_t err = XIAOTAI_OK;
    if (c->phy_ldo_channel >= 0) err = io->phy_open(c, &p->phy);
    if (err == XIAOTAI_OK) err = io->bus_open(c, &p->bus);
    if (err == XIAOTAI_OK) err = io->io_open(p->bus, &p->io);
    if (err == XIAOTAI_OK) err = io->panel_open(c, p->bus, p->io, &p->panel);
    const bool dedicated_reset = c->reset.set != NULL;
    if (err == XIAOTAI_OK && dedicated_reset) {
        err = xiaotai_line_set(&c->reset, true);
        if (err == XIAOTAI_OK && c->reset_hold_ms) xiaotai_delay_ms(c->reset_hold_ms);
        /* Try deasserting even if assertion reported failure. */
        const xiaotai_err_t deassert = xiaotai_line_set(&c->reset, false);
        if (err == XIAOTAI_OK) err = deassert;
        if (err == XIAOTAI_OK && c->reset_settle_ms) xiaotai_delay_ms(c->reset_settle_ms);
        if (deassert != XIAOTAI_OK) {
            (void)stop(p);
            p->latched = true;
            return err;
        }
    }
    if (err == XIAOTAI_OK) err = io->panel_enable(p->panel, !dedicated_reset);
    xiaotai_display_handles_t handles = {0};
    if (err == XIAOTAI_OK) err = io->framebuffers(p->panel, c->buffers, handles.framebuffers);
    if (err == XIAOTAI_OK && (!handles.framebuffers[0] || (c->buffers == 2 && !handles.framebuffers[1])))
        err = XIAOTAI_ERR_INVALID_STATE;
    if (err != XIAOTAI_OK) {
        (void)stop(p);
        return err;
    }
    if (io->native_handles) io->native_handles(p->panel, p->io, &handles);
    handles.scanout_kind = XIAOTAI_SCANOUT_DPI;
    p->ready = true;
    *out = p->handles = handles;
    return XIAOTAI_OK;
}

static xiaotai_err_t present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    jd9165_t *p = self;
    if (!p->ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!area || !pixels || area->x || area->y || area->width != p->config.width ||
        area->height != p->config.height) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_jd9165_io.present(p->panel, area, pixels);
}

static bool cleanup_failed(void *self) { return ((const jd9165_t *)self)->latched; }

xiaotai_err_t xiaotai_panel_jd9165_create(const xiaotai_panel_jd9165_config_t *config, xiaotai_panel_t *panel)
{
    if (!panel) return XIAOTAI_ERR_INVALID_ARG;
    *panel = (xiaotai_panel_t){0};
    if (!config || !config->width || !config->height || !config->lanes || !config->lane_mbps ||
        !config->pixel_clock_hz || !config->buffers || config->buffers > 2 ||
        (config->phy_ldo_channel >= 0 && !config->phy_ldo_mv) ||
        (xiaotai_line_connected(&config->reset) && !config->reset.set))
        return XIAOTAI_ERR_INVALID_ARG;
    jd9165_t *p = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*p));
    if (!p) return XIAOTAI_ERR_NO_MEM;
    p->config = *config;
    *panel = (xiaotai_panel_t){.self = p, .start = start, .stop = stop, .present = present,
                              .cleanup_failed = cleanup_failed};
    return XIAOTAI_OK;
}

typedef struct {
    int channel;
    uint16_t millivolts;
    void *handle;
} phy_line_t;

static xiaotai_err_t set_phy(void *self, bool active)
{
    phy_line_t *phy = self;
    if (!active) return release(&phy->handle, xiaotai_jd9165_io.phy_close);
    if (phy->handle) return XIAOTAI_OK;
    const xiaotai_panel_jd9165_config_t config = {.phy_ldo_channel = phy->channel, .phy_ldo_mv = phy->millivolts};
    return xiaotai_jd9165_io.phy_open(&config, &phy->handle);
}

xiaotai_err_t xiaotai_panel_jd9165_phy_line(int channel, uint16_t millivolts, xiaotai_line_t *line)
{
    if (!line) return XIAOTAI_ERR_INVALID_ARG;
    *line = (xiaotai_line_t){0};
    if (channel < 0 || !millivolts) return XIAOTAI_ERR_INVALID_ARG;
    phy_line_t *phy = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*phy));
    if (!phy) return XIAOTAI_ERR_NO_MEM;
    phy->channel = channel;
    phy->millivolts = millivolts;
    *line = (xiaotai_line_t){.set = set_phy, .self = phy};
    return XIAOTAI_OK;
}
