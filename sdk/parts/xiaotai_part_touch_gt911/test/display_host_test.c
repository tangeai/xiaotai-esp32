// SPDX-License-Identifier: MIT
#include "xiaotai/parts/panel_jd9165.h"
#include "xiaotai/parts/touch_gt911.h"
#include "xiaotai/port/mem.h"
#include "xiaotai_bus_host.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    xiaotai_i2c_bus_t *bus;
    assert(xiaotai_i2c_create(&(xiaotai_i2c_config_t){.port = 1, .sda_pin = 48, .scl_pin = 47, .hz = 400000},
                             &bus) == XIAOTAI_OK);
    xiaotai_panel_t panel;
    xiaotai_touch_t touch;
    assert(xiaotai_panel_jd9165_create(&(xiaotai_panel_jd9165_config_t){
        .width = 2, .height = 2, .buffers = 2, .lanes = 2, .lane_mbps = 600, .pixel_clock_hz = 40000000,
        .phy_ldo_channel = 3, .phy_ldo_mv = 2500,
    }, &panel) == XIAOTAI_OK);
    assert(xiaotai_touch_gt911_create(&(xiaotai_touch_i2c_config_t){.bus = bus, .width = 2, .height = 2,
        .hz = 400000, .reset_pin = 23, .reset_active_high = true, .interrupt_pin = 21,
    }, &touch) == XIAOTAI_OK);
    xiaotai_line_t shared_reset;
    assert(xiaotai_line_gpio_output(&(xiaotai_gpio_config_t){.pin = 23}, &shared_reset) == XIAOTAI_OK);
    const xiaotai_display_t *display;
    assert(xiaotai_display_create(&(xiaotai_display_config_t){
        .desc = {.width = 2, .height = 2, .pixel = XIAOTAI_PIXEL_RGB565, .render = XIAOTAI_RENDER_SCANOUT, .buffers = 2},
        .panel = panel, .touch = touch, .touch_first = true,
        .power = {{.line = shared_reset, .active = true}},
        .power_steps = 1,
    }, &display) == XIAOTAI_OK);
    const int64_t before_open = xiaotai_mem_host_outstanding();
    xiaotai_display_handles_t handles;
    assert(display->open(display->self, &handles) == XIAOTAI_ERR_NOT_FOUND && !xiaotai_bus_host_pin(23).level);
    assert(xiaotai_bus_host_i2c_attach(1, 0x5d) == XIAOTAI_OK);
    /* Touch's IO, DPI state and the first frame allocate; fail the second frame. */
    xiaotai_mem_host_fail_nth(4);
    assert(display->open(display->self, &handles) == XIAOTAI_ERR_NO_MEM && !xiaotai_bus_host_pin(23).level);
    assert(xiaotai_mem_host_outstanding() == before_open);
    assert(display->open(display->self, &handles) == XIAOTAI_OK);
    assert(!xiaotai_bus_host_pin(23).level);
    assert(!handles.panel && !handles.panel_io && !handles.touch);
    assert(handles.framebuffers[0] && handles.framebuffers[1] && handles.framebuffers[0] != handles.framebuffers[1]);
    const uint16_t drawn[] = {1, 2, 3, 4};
    const uint16_t black[] = {0, 0, 0, 0};
    const xiaotai_rect_t entire = {.width = 2, .height = 2};
    assert(memcmp(handles.framebuffers[0], black, sizeof(black)) == 0);
    assert(display->present(display->self, &entire, handles.framebuffers[1]) == XIAOTAI_OK);
    assert(display->present(display->self, &entire, drawn) == XIAOTAI_OK);
    assert(memcmp(handles.framebuffers[0], black, sizeof(black)) == 0);
    assert(memcmp(handles.framebuffers[1], drawn, sizeof(drawn)) == 0);
    xiaotai_touch_point_t point;
    size_t count;
    assert(display->read_touch(display->self, &point, 1, &count) == XIAOTAI_OK && count == 0);
    xiaotai_bus_host_i2c_detach(1, 0x5d);
    assert(display->read_touch(display->self, &point, 1, &count) == XIAOTAI_ERR_NOT_FOUND && count == 0);
    assert(panel.stop(panel.self) == XIAOTAI_OK && touch.stop(touch.self) == XIAOTAI_OK);
    assert(xiaotai_mem_host_outstanding() == before_open);
    xiaotai_mem_free(panel.self);
    xiaotai_mem_free(touch.self);
    xiaotai_mem_free(display->self);
    xiaotai_bus_host_reset();
    xiaotai_mem_free(bus);
    return 0;
}
