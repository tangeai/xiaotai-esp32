// SPDX-License-Identifier: MIT
#include "xiaotai/parts/touch_ft5x06.h"
#include "xiaotai_bus_host.h"
#include "xiaotai_mem_host.h"

#include <assert.h>

static void raw_point(uint8_t address, unsigned i, uint16_t x, uint16_t y)
{
    const uint8_t reg = (uint8_t)(3 + 6 * i);
    xiaotai_bus_host_i2c_poke(0, address, reg, 0x80 | (uint8_t)(x >> 8));
    xiaotai_bus_host_i2c_poke(0, address, reg + 1, (uint8_t)x);
    xiaotai_bus_host_i2c_poke(0, address, reg + 2, 0x30 | (uint8_t)(y >> 8));
    xiaotai_bus_host_i2c_poke(0, address, reg + 3, (uint8_t)y);
}

int main(void)
{
    const uint8_t address = 0x38;
    const xiaotai_i2c_config_t bus_config = {.port = 0, .sda_pin = 1, .scl_pin = 2, .hz = 100000};
    xiaotai_i2c_bus_t *bus = NULL;
    assert(xiaotai_i2c_create(&bus_config, &bus) == XIAOTAI_OK);
    const xiaotai_touch_i2c_config_t c = {
        .bus = bus, .hz = 100000, .width = 240, .height = 320, .mirror_x = true, .swap_xy = true,
        .reset_pin = -1, .interrupt_pin = -1,
    };
    xiaotai_touch_t touch;
    assert(xiaotai_touch_ft5x06_create(&c, &touch) == XIAOTAI_OK && !xiaotai_bus_host_i2c_transfers());
    int sentinel = 1;
    void *native = &sentinel;
    assert(touch.start(touch.self, &native) == XIAOTAI_ERR_NOT_FOUND && !native);
    assert(xiaotai_bus_host_i2c_attach(0, address) == XIAOTAI_OK);
    assert(touch.start(touch.self, &native) == XIAOTAI_OK && !native);
    xiaotai_bus_host_i2c_log_clear();
    xiaotai_touch_point_t points[2];
    size_t count = 17;
    xiaotai_bus_host_i2c_poke(0, address, 2, 2);
    raw_point(address, 0, 18, 300);
    raw_point(address, 1, 30, 92);
    assert(touch.read(touch.self, points, 2, &count) == XIAOTAI_OK && count == 2);
    assert(points[0].x == 300 && points[0].y == 222 && points[1].x == 92 && points[1].y == 210);
    xiaotai_bus_host_transfer_t log[2];
    assert(xiaotai_bus_host_i2c_log(log, 2) == 1 && log[0].reg == 2 && log[0].size == 13);
    assert(touch.read(touch.self, points, 1, &count) == XIAOTAI_OK && count == 1);
    assert(xiaotai_bus_host_i2c_log(log, 2) == 2 && log[1].size == 7);
    assert(touch.read(touch.self, NULL, 0, &count) == XIAOTAI_OK && !count);
    assert(xiaotai_bus_host_i2c_transfers() == 2);
    xiaotai_bus_host_i2c_poke(0, address, 2, 0);
    assert(touch.read(touch.self, points, 2, &count) == XIAOTAI_OK && !count);
    xiaotai_bus_host_i2c_poke(0, address, 2, 6);
    assert(touch.read(touch.self, points, 2, &count) == XIAOTAI_OK && !count);
    xiaotai_bus_host_i2c_fail(0, address, XIAOTAI_BUS_HOST_READ, 1, XIAOTAI_ERR_TIMEOUT);
    count = 5;
    assert(touch.read(touch.self, points, 2, &count) == XIAOTAI_ERR_TIMEOUT && !count);
    assert(touch.stop(touch.self) == XIAOTAI_OK && touch.stop(touch.self) == XIAOTAI_OK);
    assert(touch.read(touch.self, points, 2, &count) == XIAOTAI_ERR_INVALID_STATE && !count);
    xiaotai_mem_host_clear();
    xiaotai_mem_host_fail_nth(1);
    assert(touch.start(touch.self, &native) == XIAOTAI_ERR_NO_MEM && !native);
    assert(touch.start(touch.self, &native) == XIAOTAI_OK && !native);
    assert(touch.stop(touch.self) == XIAOTAI_OK && !xiaotai_mem_host_outstanding());
    return 0;
}
