// SPDX-License-Identifier: MIT
#include "touch_io.h"
#include "xiaotai/port/mem.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <string.h>

static int bus, hardware_io, controller;
static char trace[24], fail_at, cleanup_error;
static size_t used, points_to_return;

static void clear_trace(void) { trace[0] = '\0'; used = 0; }

static xiaotai_err_t record(char step)
{
    assert(used + 1 < sizeof(trace));
    trace[used++] = step;
    trace[used] = '\0';
    if (step == fail_at) return XIAOTAI_ERR_TIMEOUT;
    return step == cleanup_error ? XIAOTAI_FAIL : XIAOTAI_OK;
}

static xiaotai_err_t acquire(char step, void *resource, void **out)
{
    const xiaotai_err_t err = record(step);
    if (err == XIAOTAI_OK) *out = resource;
    return err;
}

static xiaotai_err_t io_open(const xiaotai_touch_i2c_config_t *c, void **out)
{
    assert(c->bus == (void *)&bus && c->address == GT911_PRIMARY_ADDRESS && c->hz == 400000);
    return acquire('I', &hardware_io, out);
}

static xiaotai_err_t touch_open(const xiaotai_touch_i2c_config_t *c, void *io, void **out)
{
    assert(io == &hardware_io);
    assert(c->width == 1024 && c->height == 600 && c->reset_pin == 23 && c->reset_active_high);
    assert(c->interrupt_pin == 21 && c->swap_xy && c->mirror_x && !c->mirror_y);
    return acquire('T', &controller, out);
}
static xiaotai_err_t io_close(void *io) { assert(io == &hardware_io); return record('i'); }
static xiaotai_err_t touch_close(void *touch) { assert(touch == &controller); return record('t'); }
static void *native_handle(void *touch) { assert(touch == &controller); return touch; }

static xiaotai_err_t read_touch(void *touch, xiaotai_touch_point_t *points, size_t capacity, size_t *count)
{
    assert(touch == &controller && capacity > 0);
    *count = points_to_return < capacity ? points_to_return : capacity;
    for (size_t i = 0; i < *count; ++i) points[i] = (xiaotai_touch_point_t){42, (uint16_t)i};
    return record('R');
}

const gt911_io_t xiaotai_gt911_io = {
    .io_open = io_open, .io_close = io_close, .controller_open = touch_open,
    .controller_close = touch_close, .read = read_touch, .native_handle = native_handle,
};

static xiaotai_touch_i2c_config_t make_config(void)
{
    return (xiaotai_touch_i2c_config_t){
        .bus = (void *)&bus, .width = 1024, .height = 600, .hz = 400000,
        .reset_pin = 23, .reset_active_high = true, .interrupt_pin = 21,
        .swap_xy = true, .mirror_x = true,
    };
}

static void test_latched(bool fail_initialization)
{
    const xiaotai_touch_i2c_config_t config = make_config();
    xiaotai_touch_t t;
    void *native;
    clear_trace();
    assert(xiaotai_touch_gt911_create(&config, &t) == XIAOTAI_OK);
    fail_at = fail_initialization ? 'T' : 0;
    cleanup_error = fail_initialization ? 'i' : 't';
    if (fail_initialization) {
        assert(t.start(t.self, &native) == XIAOTAI_ERR_TIMEOUT && !native && !strcmp(trace, "ITi"));
    } else {
        assert(t.start(t.self, &native) == XIAOTAI_OK);
        assert(t.stop(t.self) == XIAOTAI_FAIL && !strcmp(trace, "ITt"));
    }
    clear_trace();
    fail_at = cleanup_error = 0;
    assert(t.stop(t.self) == XIAOTAI_ERR_INVALID_STATE);
    assert(t.start(t.self, &native) == XIAOTAI_ERR_INVALID_STATE && !native && used == 0);
    xiaotai_mem_free(t.self);
}

int main(void)
{
    xiaotai_touch_i2c_config_t cfg = make_config();
    xiaotai_touch_t t;
    assert(xiaotai_touch_gt911_create(NULL, &t) == XIAOTAI_ERR_INVALID_ARG && !t.self);
    cfg.address = 0x08;
    assert(xiaotai_touch_gt911_create(&cfg, &t) == XIAOTAI_ERR_INVALID_ARG && !t.self);
    cfg.address = 0;
    xiaotai_mem_host_fail_nth(1);
    assert(xiaotai_touch_gt911_create(&cfg, &t) == XIAOTAI_ERR_NO_MEM && !t.self);
    assert(xiaotai_touch_gt911_create(&cfg, &t) == XIAOTAI_OK && used == 0);
    cfg.hz = 10000;
    void *native = &controller;
    xiaotai_touch_point_t points[2];
    size_t count = 9;
    assert(t.read(t.self, points, 2, &count) == XIAOTAI_ERR_INVALID_STATE && count == 0);
    fail_at = 'I';
    assert(t.start(t.self, &native) == XIAOTAI_ERR_TIMEOUT && !native && !strcmp(trace, "I"));
    clear_trace();
    fail_at = 'T';
    assert(t.start(t.self, &native) == XIAOTAI_ERR_TIMEOUT && !native && !strcmp(trace, "ITi"));
    clear_trace();
    fail_at = 0;
    assert(t.start(t.self, &native) == XIAOTAI_OK && native == &controller);
    assert(t.start(t.self, &native) == XIAOTAI_OK && !strcmp(trace, "IT"));
    assert(t.read(t.self, NULL, 0, &count) == XIAOTAI_OK && count == 0 && !strcmp(trace, "IT"));
    points_to_return = 3;
    assert(t.read(t.self, points, 2, &count) == XIAOTAI_OK && count == 2);
    assert(points[0].x == 42 && points[1].y == 1);
    fail_at = 'R';
    assert(t.read(t.self, points, 2, &count) == XIAOTAI_ERR_TIMEOUT && count == 0);
    fail_at = 0;
    points_to_return = 0;
    assert(t.read(t.self, points, 2, &count) == XIAOTAI_OK && count == 0);
    assert(t.stop(t.self) == XIAOTAI_OK && !strcmp(trace, "ITRRRti"));
    assert(t.stop(t.self) == XIAOTAI_OK && !strcmp(trace, "ITRRRti"));
    xiaotai_mem_free(t.self);
    test_latched(true);
    test_latched(false);
    assert(xiaotai_mem_host_outstanding() == 0);
    return 0;
}
