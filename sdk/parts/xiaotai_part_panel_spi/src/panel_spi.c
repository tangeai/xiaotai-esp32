// SPDX-License-Identifier: MIT
#include "spi_io.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#include <limits.h>
#include <string.h>

typedef struct {
    const xiaotai_panel_spi_controller_t *controller;
    xiaotai_panel_spi_config_t config;
    size_t transfer_bytes;
    uint16_t width, height;
    void *io, *panel, *vendor, *staging;
    bool bus, cs_touched, reset_touched, ready, in_flight;
} spi_panel_t;

static xiaotai_err_t drain(spi_panel_t *p)
{
    if (!p->in_flight) return XIAOTAI_OK;
    const xiaotai_err_t err = xt_spi_drain(p->io);
    if (err == XIAOTAI_OK) p->in_flight = false;
    return err;
}

static xiaotai_err_t stop(void *self)
{
    spi_panel_t *p = self;
    p->ready = false;
    xiaotai_err_t err = drain(p);
    if (err != XIAOTAI_OK) return err;
    xiaotai_mem_free(p->staging);
    p->staging = NULL;
    if (p->panel) {
        err = xt_spi_panel_close(p->panel);
        if (err != XIAOTAI_OK) return err;
        p->panel = NULL;
    }
    xiaotai_mem_free(p->vendor);
    p->vendor = NULL;
    if (p->io) {
        err = xt_spi_io_close(p->io);
        if (err != XIAOTAI_OK) return err;
        p->io = NULL;
    }
    if (p->bus) {
        err = xt_spi_bus_close(p->config.host);
        if (err != XIAOTAI_OK) return err;
        p->bus = false;
    }
    if (p->cs_touched) {
        err = xiaotai_line_set(&p->config.cs_line, false);
        if (err != XIAOTAI_OK) return err;
        p->cs_touched = false;
    }
    if (p->reset_touched) {
        err = xiaotai_line_set(&p->config.reset, false);
        if (err != XIAOTAI_OK) return err;
        p->reset_touched = false;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t pulse_reset(spi_panel_t *p)
{
    const xiaotai_panel_spi_config_t *c = &p->config;
    p->reset_touched = true;
    xiaotai_err_t err = xiaotai_line_set(&c->reset, true);
    if (err != XIAOTAI_OK) return err;
    if (c->reset_hold_ms) xiaotai_delay_ms(c->reset_hold_ms);
    err = xiaotai_line_set(&c->reset, false);
    if (err != XIAOTAI_OK) return err;
    p->reset_touched = false;
    if (c->reset_settle_ms) xiaotai_delay_ms(c->reset_settle_ms);
    return XIAOTAI_OK;
}

static xiaotai_err_t reset(spi_panel_t *p)
{
    const xiaotai_panel_spi_config_t *c = &p->config;
    if (c->reset.set) {
        if (!c->reset_line_at_create) {
            const xiaotai_err_t err = pulse_reset(p);
            if (err != XIAOTAI_OK) return err;
        }
        if (!c->software_reset_after_line) return XIAOTAI_OK;
    }
    return xt_spi_panel_reset(p->panel);
}

/* Tiles are packed: transfer rows in groups, or split one row when even
 * that exceeds the bus limit. Padding is never read from the caller. */
static xiaotai_err_t draw_tiles(spi_panel_t *p, const xiaotai_rect_t *area, const uint8_t *src,
                               size_t tile_bytes, bool clear)
{
    const size_t max_pixels = tile_bytes / sizeof(uint16_t);
    const bool staged = !clear && !p->config.wait_done;
    const size_t rows = area->width <= max_pixels ? max_pixels / area->width : 1;
    for (uint16_t y = 0; y < area->height;) {
        const uint16_t h = (size_t)(area->height - y) < rows ? (size_t)(area->height - y) : rows;
        for (uint16_t x = 0; x < area->width;) {
            const size_t remaining = (size_t)area->width - x;
            const uint16_t w = remaining <= max_pixels ? remaining : max_pixels;
            const xiaotai_rect_t tile = {.x = area->x + x, .y = area->y + y, .width = w, .height = h};
            const uint8_t *pixels = clear ? p->staging : src + ((size_t)y * area->width + x) * sizeof(uint16_t);
            if (staged) {
                const xiaotai_err_t err = drain(p);
                if (err != XIAOTAI_OK) return err;
                memcpy(p->staging, pixels, (size_t)w * h * sizeof(uint16_t));
                pixels = p->staging;
            }
            /* A failed driver call can have queued part of the request. */
            p->in_flight = true;
            const xiaotai_err_t err = xt_spi_draw(p->panel, &tile, pixels);
            if (err != XIAOTAI_OK) return err;
            x += w;
        }
        y += h;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t clear_panel(spi_panel_t *p)
{
    const size_t bytes = p->transfer_bytes < 2048 ? p->transfer_bytes : 2048;
    p->staging = xiaotai_mem_calloc(XIAOTAI_MEM_DMA, 1, bytes);
    if (!p->staging) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_rect_t full = {.width = p->width, .height = p->height};
    const xiaotai_err_t draw_err = draw_tiles(p, &full, NULL, bytes, true);
    const xiaotai_err_t fence_err = drain(p);
    if (fence_err == XIAOTAI_OK) {
        xiaotai_mem_free(p->staging);
        p->staging = NULL;
    }
    return draw_err != XIAOTAI_OK ? draw_err : fence_err;
}

static xiaotai_err_t start(void *self, xiaotai_display_handles_t *out)
{
    spi_panel_t *p = self;
    const xiaotai_panel_spi_config_t *c = &p->config;
    if (out) *out = (xiaotai_display_handles_t){0};
    if (p->bus || p->cs_touched || p->reset_touched) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = xt_spi_bus_open(c, p->transfer_bytes);
    if (err != XIAOTAI_OK) return err;
    p->bus = true;
    if ((err = xt_spi_io_open(c, &p->io)) != XIAOTAI_OK) goto failed;
    if (c->init && (err = p->controller->prepare_vendor(c, &p->vendor)) != XIAOTAI_OK) goto failed;
    if ((err = xt_spi_panel_open(p->controller, c, p->io, p->vendor, &p->panel)) != XIAOTAI_OK) goto failed;
    if ((err = reset(p)) != XIAOTAI_OK) goto failed;
    if (c->cs_line.set) {
        p->cs_touched = true;
        if ((err = xiaotai_line_set(&c->cs_line, true)) != XIAOTAI_OK) goto failed;
    }
    if ((err = xt_spi_panel_setup(p->panel, c)) != XIAOTAI_OK) goto failed;
    if ((err = clear_panel(p)) != XIAOTAI_OK) goto failed;
    if ((err = xt_spi_panel_on(p->panel)) != XIAOTAI_OK) goto failed;
    p->ready = true;
    if (out) {
        out->swap_xy = c->swap_xy;
        out->mirror_x = c->mirror_x;
        out->mirror_y = c->mirror_y;
        xt_spi_handles(p->panel, p->io, out);
    }
    return XIAOTAI_OK;
failed:
    (void)stop(p); /* Retained resources prevent restart if rollback fails. */
    return err;
}

static xiaotai_err_t present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    spi_panel_t *p = self;
    if (!p->ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!area || !pixels || !area->width || !area->height ||
        (uint32_t)area->x + area->width > p->width || (uint32_t)area->y + area->height > p->height)
        return XIAOTAI_ERR_INVALID_ARG;
    if (!p->config.wait_done && !p->staging) {
        p->staging = xiaotai_mem_alloc(XIAOTAI_MEM_DMA, p->transfer_bytes);
        if (!p->staging) return XIAOTAI_ERR_NO_MEM;
    }
    const xiaotai_err_t err = draw_tiles(p, area, pixels, p->transfer_bytes, false);
    if (!p->config.wait_done) return err;
    const xiaotai_err_t fence = drain(p);
    if (fence != XIAOTAI_OK) p->ready = false;
    return err != XIAOTAI_OK ? err : fence;
}

xiaotai_err_t xiaotai_panel_spi_create(const xiaotai_panel_spi_controller_t *controller,
                                      const xiaotai_panel_spi_config_t *config, xiaotai_panel_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = (xiaotai_panel_t){0};
    if (!controller || !controller->create || !config || config->host < 0 || config->mosi_pin < 0 ||
        config->sclk_pin < 0 || config->dc_pin < 0 || config->cs_pin < -1 ||
        ((config->cs_pin == -1) != (config->cs_line.set != NULL)) ||
        config->spi_mode > 3 || !config->hz || !config->queue_depth ||
        !config->native_width || !config->native_height ||
        (config->external_dma && !config->wait_done) ||
        (config->reset_line_at_create && !config->reset.set) ||
        ((config->init != NULL) != (config->init_count != 0)) || config->init_count > UINT16_MAX)
        return XIAOTAI_ERR_INVALID_ARG;
    if (config->init && !controller->prepare_vendor) return XIAOTAI_ERR_NOT_SUPPORTED;
    for (size_t i = 0; i < config->init_count; ++i)
        if (config->init[i].size && !config->init[i].data) return XIAOTAI_ERR_INVALID_ARG;

    const uint64_t frame_bytes = (uint64_t)config->native_width * config->native_height * sizeof(uint16_t);
    const uint64_t cap = config->max_transfer_bytes ? config->max_transfer_bytes : frame_bytes;
    const uint64_t bytes = cap < frame_bytes ? cap : frame_bytes;
    const uint16_t width = config->swap_xy ? config->native_height : config->native_width;
    const uint16_t height = config->swap_xy ? config->native_width : config->native_height;
    if (bytes < sizeof(uint16_t) || bytes > INT_MAX ||
        (uint32_t)config->gap_x + width - 1U > UINT16_MAX ||
        (uint32_t)config->gap_y + height - 1U > UINT16_MAX)
        return XIAOTAI_ERR_INVALID_SIZE;
    spi_panel_t *p = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*p));
    if (!p) return XIAOTAI_ERR_NO_MEM;
    p->config = *config;
    p->controller = controller;
    p->transfer_bytes = (size_t)bytes;
    p->width = width;
    p->height = height;
    if (config->reset_line_at_create) {
        const xiaotai_err_t err = pulse_reset(p);
        if (err != XIAOTAI_OK) {
            (void)stop(p);
            xiaotai_mem_free(p);
            return err;
        }
    }
    *out = (xiaotai_panel_t){.self = p, .start = start, .stop = stop, .present = present};
    return XIAOTAI_OK;
}
