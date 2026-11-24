// SPDX-License-Identifier: MIT
#include "panel_io.h"
#include "xiaotai/port/mem.h"

typedef struct {
    xiaotai_panel_ssd1306_config_t config;
    void *io;
    void *panel;
    bool ready;
} ssd1306_t;

static xiaotai_err_t stop(void *self)
{
    ssd1306_t *p = self;
    p->ready = false;
    if (p->panel) {
        const xiaotai_err_t err = xiaotai_ssd1306_del_panel(p->panel);
        if (err != XIAOTAI_OK) return err;
        p->panel = NULL;
    }
    if (p->io) {
        const xiaotai_err_t err = xiaotai_ssd1306_del_io(p->io);
        if (err != XIAOTAI_OK) return err;
        p->io = NULL;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t start(void *self, xiaotai_display_handles_t *handles)
{
    ssd1306_t *p = self;
    if (handles) *handles = (xiaotai_display_handles_t){0};
    /* Also catches handles retained after any failed rollback. */
    if (p->panel || p->io) return XIAOTAI_ERR_INVALID_STATE;

    const xiaotai_panel_ssd1306_config_t *c = &p->config;
    uint8_t blank[128 * 64 / 8] = {0};
    xiaotai_err_t err = xiaotai_ssd1306_probe(c, c->transfer_timeout_ms);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_ssd1306_new_io(c, c->transfer_timeout_ms, &p->io);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_ssd1306_new_panel(p->io, c->height, &p->panel);
    if (err != XIAOTAI_OK) goto failed;
    err = xiaotai_ssd1306_configure(p->panel, c->mirror_x, c->mirror_y, c->invert);
    if (err != XIAOTAI_OK) goto failed;
    err = xiaotai_ssd1306_draw(p->panel, c->width, c->height, blank);
    if (err != XIAOTAI_OK) goto failed;
    err = xiaotai_ssd1306_switch(p->panel, true);
    if (err != XIAOTAI_OK) goto failed;

    p->ready = true;
    if (handles) {
        handles->mirror_x = c->mirror_x;
        handles->mirror_y = c->mirror_y;
        xiaotai_ssd1306_native_handles(p->panel, p->io, handles);
    }
    return XIAOTAI_OK;

failed:
    /* Preserve the startup fault; stop logs cleanup failures in native IO
     * and retains the handle so no later start reuses uncertain hardware. */
    (void)stop(p);
    return err;
}

static xiaotai_err_t present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    ssd1306_t *p = self;
    if (!p->ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!pixels || !area || area->x || area->y ||
        area->width != p->config.width || area->height != p->config.height)
        return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_ssd1306_draw(p->panel, p->config.width, p->config.height, pixels);
}

static xiaotai_err_t set_on(void *self, bool on)
{
    ssd1306_t *p = self;
    return p->ready ? xiaotai_ssd1306_switch(p->panel, on) : XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t xiaotai_panel_ssd1306_create(const xiaotai_panel_ssd1306_config_t *config, xiaotai_panel_t *panel)
{
    if (!panel) return XIAOTAI_ERR_INVALID_ARG;
    *panel = (xiaotai_panel_t){0};
    if (!config || !config->bus || config->address > 0x7f || !config->hz ||
        config->width != 128 || (config->height != 32 && config->height != 64))
        return XIAOTAI_ERR_INVALID_ARG;
    ssd1306_t *p = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*p));
    if (!p) return XIAOTAI_ERR_NO_MEM;
    p->config = *config;
    if (!p->config.transfer_timeout_ms) p->config.transfer_timeout_ms = 100;
    *panel = (xiaotai_panel_t){.start = start, .stop = stop, .present = present, .set_on = set_on, .self = p};
    return XIAOTAI_OK;
}
