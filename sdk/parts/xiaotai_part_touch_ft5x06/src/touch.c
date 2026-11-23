// SPDX-License-Identifier: MIT
#include "touch_io.h"
#include "xiaotai/port/mem.h"

typedef struct {
    xiaotai_touch_i2c_config_t config;
    void *io, *controller;
    bool ready;
} touch_t;

static xiaotai_err_t stop(void *self)
{
    touch_t *t = self;
    t->ready = false;
    if (t->controller) {
        const xiaotai_err_t err = xt_ft_controller_close(t->controller);
        if (err != XIAOTAI_OK) return err;
        t->controller = NULL;
    }
    if (t->io) {
        const xiaotai_err_t err = xt_ft_io_close(t->io);
        if (err != XIAOTAI_OK) return err;
        t->io = NULL;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t start(void *self, void **native)
{
    touch_t *t = self;
    if (native) *native = NULL;
    if (t->io || t->controller) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = xt_ft_io_open(&t->config, &t->io);
    if (err != XIAOTAI_OK) return err;
    err = xt_ft_controller_open(&t->config, t->io, &t->controller);
    if (err != XIAOTAI_OK) {
        (void)stop(t);
        return err;
    }
    t->ready = true;
    if (native) *native = xt_ft_controller_native(t->controller);
    return XIAOTAI_OK;
}

static bool cleanup_failed(void *self)
{
    const touch_t *t = self;
    return !t->ready && (t->controller || t->io);
}

static xiaotai_err_t read(void *self, xiaotai_touch_point_t *points, size_t cap, size_t *count)
{
    touch_t *t = self;
    if (!count) return XIAOTAI_ERR_INVALID_ARG;
    *count = 0;
    if (cap && !points) return XIAOTAI_ERR_INVALID_ARG;
    if (!t->ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!cap) return XIAOTAI_OK;
    size_t limit = xt_ft_max_points();
    if (limit > XT_FT_MAX_POINTS) limit = XT_FT_MAX_POINTS;
    if (limit > cap) limit = cap;
    uint8_t raw[XT_FT_FRAME_BYTES];
    xiaotai_err_t err = xt_ft_io_read(t->io, raw, 1 + limit * XT_FT_POINT_BYTES);
    if (err != XIAOTAI_OK) return err;
    if (raw[0] > XT_FT_MAX_POINTS) return XIAOTAI_OK;
    const size_t n = raw[0] < limit ? raw[0] : limit;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *r = &raw[1 + i * XT_FT_POINT_BYTES];
        uint16_t x = (uint16_t)((r[0] & 0xfU) << 8) | r[1];
        uint16_t y = (uint16_t)((r[2] & 0xfU) << 8) | r[3];
        /* Same mirror-before-swap order and limits as esp_lcd_touch;
         * the display owns final clamping and optional jitter filtering. */
        if (t->config.mirror_x) x = (uint16_t)(t->config.width - x);
        if (t->config.mirror_y) y = (uint16_t)(t->config.height - y);
        points[i] = t->config.swap_xy ? (xiaotai_touch_point_t){.x = y, .y = x}
                                      : (xiaotai_touch_point_t){.x = x, .y = y};
    }
    *count = n;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_touch_ft5x06_create(const xiaotai_touch_i2c_config_t *c, xiaotai_touch_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = (xiaotai_touch_t){0};
    if (!c || !c->bus || c->address > 0x7f || !c->hz || !c->width || !c->height ||
        c->reset_pin < -1 || c->interrupt_pin < -1)
        return XIAOTAI_ERR_INVALID_ARG;
    touch_t *t = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*t));
    if (!t) return XIAOTAI_ERR_NO_MEM;
    t->config = *c;
    if (!t->config.address) t->config.address = XT_FT_ADDRESS;
    *out = (xiaotai_touch_t){.self = t, .start = start, .stop = stop, .read = read,
                            .cleanup_failed = cleanup_failed};
    return XIAOTAI_OK;
}
