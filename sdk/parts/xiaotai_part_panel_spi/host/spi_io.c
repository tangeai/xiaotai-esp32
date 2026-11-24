// SPDX-License-Identifier: MIT
#include "spi_io.h"
#include "spi_fake.h"

#include <assert.h>
#include <string.h>

typedef struct {
    bool bus, io, panel;
    const uint8_t *queued;
    size_t bytes;
} slot_t;

typedef struct {
    unsigned nth;
    xiaotai_err_t err;
} failure_t;

static slot_t slots[8];
static failure_t fail[XT_SPI_OP_COUNT];
static xt_spi_fake_t state;

xiaotai_err_t xiaotai_panel_spi_external_dma_alignment(uint8_t *alignment)
{
    if (!alignment) return XIAOTAI_ERR_INVALID_ARG;
    *alignment = 128;  /* Model the stricter supported external DMA alignment. */
    return XIAOTAI_OK;
}

const xt_spi_fake_t *xt_spi_fake_state(void)
{
    return &state;
}

void xt_spi_fake_reset(void)
{
    state = (xt_spi_fake_t){0};
    memset(fail, 0, sizeof(fail));
}

void xt_spi_fake_fail(xt_spi_fake_op_t op, unsigned nth, xiaotai_err_t err)
{
    fail[op] = (failure_t){.nth = nth, .err = err};
}

static xiaotai_err_t call(xt_spi_fake_op_t op)
{
    if (state.calls < sizeof(state.log) / sizeof(state.log[0])) state.log[state.calls++] = op;
    ++state.count[op];
    return fail[op].nth && !--fail[op].nth ? fail[op].err : XIAOTAI_OK;
}

static void complete(slot_t *slot)
{
    if (!slot->queued) return;
    const size_t n = slot->bytes < sizeof(state.last) ? slot->bytes : sizeof(state.last);
    memset(state.last, 0, sizeof(state.last));
    memcpy(state.last, slot->queued, n);
    for (size_t i = 0; i < slot->bytes; ++i) state.nonblack |= slot->queued[i] != 0;
    state.pixels_painted += slot->bytes / sizeof(uint16_t);
    state.queued = slot->queued = NULL;
}

xiaotai_err_t xt_spi_bus_open(const xiaotai_panel_spi_config_t *config, size_t transfer_bytes)
{
    const xiaotai_err_t err = call(XT_SPI_BUS_OPEN);
    if (err != XIAOTAI_OK) return err;
    if ((unsigned)config->host >= sizeof(slots) / sizeof(slots[0])) return XIAOTAI_ERR_INVALID_ARG;
    slot_t *s = &slots[config->host];
    if (s->bus) return XIAOTAI_ERR_INVALID_STATE;
    state.config = *config;
    state.limit = transfer_bytes;
    s->bus = true;
    return XIAOTAI_OK;
}

xiaotai_err_t xt_spi_bus_close(int host)
{
    slot_t *s = &slots[host];
    assert(s->bus && !s->io);
    const xiaotai_err_t err = call(XT_SPI_BUS_CLOSE);
    if (err == XIAOTAI_OK) s->bus = false;
    return err;
}

xiaotai_err_t xt_spi_io_open(const xiaotai_panel_spi_config_t *config, void **out)
{
    slot_t *s = &slots[config->host];
    assert(s->bus && !s->io);
    const xiaotai_err_t err = call(XT_SPI_IO_OPEN);
    if (err == XIAOTAI_OK) {
        s->io = true;
        *out = s;
    }
    return err;
}

xiaotai_err_t xt_spi_io_close(void *io)
{
    slot_t *s = io;
    assert(s->io && !s->panel && !s->queued);
    const xiaotai_err_t err = call(XT_SPI_IO_CLOSE);
    if (err == XIAOTAI_OK) s->io = false;
    return err;
}

xiaotai_err_t xt_spi_panel_open(const xiaotai_panel_spi_controller_t *controller,
                               const xiaotai_panel_spi_config_t *config, void *io, void *vendor, void **out)
{
    slot_t *s = io;
    assert(s->io && !s->panel);
    xiaotai_err_t err = call(XT_SPI_PANEL_OPEN);
    if (err != XIAOTAI_OK) return err;
    err = controller->create(io, vendor, config, out);
    if (err == XIAOTAI_OK) s->panel = true;
    return err;
}

xiaotai_err_t xt_spi_panel_close(void *panel)
{
    slot_t *s = panel;
    assert(s->panel && !s->queued);
    const xiaotai_err_t err = call(XT_SPI_PANEL_CLOSE);
    if (err == XIAOTAI_OK) s->panel = false;
    return err;
}

xiaotai_err_t xt_spi_panel_reset(void *panel)
{
    assert(((slot_t *)panel)->panel);
    return call(XT_SPI_RESET);
}

xiaotai_err_t xt_spi_panel_setup(void *panel, const xiaotai_panel_spi_config_t *config)
{
    assert(((slot_t *)panel)->panel);
    state.config = *config;
    return call(XT_SPI_SETUP);
}

xiaotai_err_t xt_spi_panel_on(void *panel)
{
    slot_t *s = panel;
    assert(s->panel && !s->queued);
    return call(XT_SPI_ON);
}

xiaotai_err_t xt_spi_draw(void *panel, const xiaotai_rect_t *area, const void *pixels)
{
    slot_t *s = panel;
    assert(s->panel && area->width && area->height);
    complete(s); /* A new IDF draw starts by writing its column/page params. */
    s->bytes = (size_t)area->width * area->height * sizeof(uint16_t);
    s->queued = state.queued = pixels;
    if (state.draws < sizeof(state.drawn) / sizeof(state.drawn[0])) state.drawn[state.draws++] = *area;
    return call(XT_SPI_DRAW); /* Can fail after queueing. */
}

xiaotai_err_t xt_spi_drain(void *io)
{
    const xiaotai_err_t err = call(XT_SPI_DRAIN);
    if (err == XIAOTAI_OK) complete(io);
    return err;
}

void xt_spi_handles(void *panel, void *io, xiaotai_display_handles_t *out)
{
    (void)panel;
    (void)io;
    (void)out;
}

static xiaotai_err_t create_st7789(void *io, const void *dev,
                                  const xiaotai_panel_spi_config_t *config, void **out)
{
    (void)dev;
    (void)config;
    *out = io;
    return XIAOTAI_OK;
}

const xiaotai_panel_spi_controller_t *xiaotai_panel_st7789(void)
{
    static const xiaotai_panel_spi_controller_t controller = {.name = "st7789", .create = create_st7789};
    return &controller;
}
