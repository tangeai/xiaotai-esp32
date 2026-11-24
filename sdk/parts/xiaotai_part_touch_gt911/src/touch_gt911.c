// SPDX-License-Identifier: MIT
#include "touch_io.h"
#include "xiaotai/port/mem.h"

typedef struct {
    xiaotai_touch_i2c_config_t config;
    void *io, *controller;
    bool latched;
} gt911_t;

static xiaotai_err_t stop(void *self)
{
    gt911_t *touch = self;
    if (touch->latched) return XIAOTAI_ERR_INVALID_STATE;
    const gt911_io_t *io = &xiaotai_gt911_io;
    if (touch->controller) {
        const xiaotai_err_t err = io->controller_close(touch->controller);
        if (err != XIAOTAI_OK) {
            touch->latched = true;
            return err;
        }
        touch->controller = NULL;
    }
    if (touch->io) {
        const xiaotai_err_t err = io->io_close(touch->io);
        if (err != XIAOTAI_OK) {
            touch->latched = true;
            return err;
        }
        touch->io = NULL;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t start(void *self, void **native)
{
    gt911_t *touch = self;
    if (!native) return XIAOTAI_ERR_INVALID_ARG;
    *native = NULL;
    if (touch->latched) return XIAOTAI_ERR_INVALID_STATE;
    const gt911_io_t *io = &xiaotai_gt911_io;
    if (touch->controller) {
        if (io->native_handle) *native = io->native_handle(touch->controller);
        return XIAOTAI_OK;
    }
    xiaotai_err_t err = io->io_open(&touch->config, &touch->io);
    if (err == XIAOTAI_OK) err = io->controller_open(&touch->config, touch->io, &touch->controller);
    if (err != XIAOTAI_OK) {
        (void)stop(touch);
        return err;
    }
    if (io->native_handle) *native = io->native_handle(touch->controller);
    return XIAOTAI_OK;
}

static xiaotai_err_t read_touch(void *self, xiaotai_touch_point_t *points, size_t capacity, size_t *count)
{
    gt911_t *touch = self;
    if (!count) return XIAOTAI_ERR_INVALID_ARG;
    *count = 0;
    if (capacity && !points) return XIAOTAI_ERR_INVALID_ARG;
    if (!touch->controller || touch->latched) return XIAOTAI_ERR_INVALID_STATE;
    if (!capacity) return XIAOTAI_OK;
    const xiaotai_err_t err = xiaotai_gt911_io.read(touch->controller, points, capacity, count);
    if (err != XIAOTAI_OK) *count = 0;
    return err;
}

static bool cleanup_failed(void *self) { return ((const gt911_t *)self)->latched; }

xiaotai_err_t xiaotai_touch_gt911_create(const xiaotai_touch_i2c_config_t *config, xiaotai_touch_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = (xiaotai_touch_t){0};
    if (!config || !config->bus || !config->width || !config->height || config->reset_pin < -1 ||
        config->interrupt_pin < -1 || (config->address && config->address != GT911_PRIMARY_ADDRESS &&
        config->address != GT911_BACKUP_ADDRESS)) return XIAOTAI_ERR_INVALID_ARG;
    gt911_t *touch = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*touch));
    if (!touch) return XIAOTAI_ERR_NO_MEM;
    touch->config = *config;
    if (!touch->config.address) touch->config.address = GT911_PRIMARY_ADDRESS;
    *out = (xiaotai_touch_t){.self = touch, .start = start, .stop = stop, .read = read_touch,
                            .cleanup_failed = cleanup_failed};
    return XIAOTAI_OK;
}
