// SPDX-License-Identifier: MIT
#include "touch_io.h"
#include "xiaotai/port/mem.h"

/* One fake device on the shared host bus. No native handle or synthetic
 * presses are minted here; fault and gesture tests can inject their own IO. */
static xiaotai_err_t io_open(const xiaotai_touch_i2c_config_t *config, void **out)
{
    xiaotai_touch_i2c_config_t *io = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*io));
    if (!io) return XIAOTAI_ERR_NO_MEM;
    *io = *config;
    *out = io;
    return XIAOTAI_OK;
}

static xiaotai_err_t io_close(void *io)
{
    xiaotai_mem_free(io);
    return XIAOTAI_OK;
}

static xiaotai_err_t controller_open(const xiaotai_touch_i2c_config_t *config, void *io, void **out)
{
    xiaotai_err_t err = XIAOTAI_OK;
    if (config->reset_pin != -1) {
        xiaotai_line_t reset;
        err = xiaotai_line_gpio_output(&(xiaotai_gpio_config_t){
            .pin = config->reset_pin, .active_low = !config->reset_active_high, .initially_active = true,
        }, &reset);
        if (err == XIAOTAI_OK) err = xiaotai_line_set(&reset, false);
    }
    if (err == XIAOTAI_OK) err = xiaotai_i2c_probe(config->bus, config->address);
    if (err == XIAOTAI_OK) *out = io;
    return err;
}

static xiaotai_err_t controller_close(void *controller)
{
    (void)controller;
    return XIAOTAI_OK;
}

static xiaotai_err_t read_touch(void *controller, xiaotai_touch_point_t *points, size_t capacity, size_t *count)
{
    (void)points;
    (void)capacity;
    const xiaotai_touch_i2c_config_t *config = controller;
    *count = 0;
    return xiaotai_i2c_probe(config->bus, config->address);
}

const gt911_io_t xiaotai_gt911_io = {
    .io_open = io_open, .io_close = io_close, .controller_open = controller_open,
    .controller_close = controller_close, .read = read_touch,
};
