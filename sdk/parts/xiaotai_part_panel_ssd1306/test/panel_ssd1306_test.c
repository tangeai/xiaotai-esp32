// SPDX-License-Identifier: MIT
#include "panel_fake.h"
#include "xiaotai_bus_host.h"

#include <assert.h>
#include <string.h>

static xiaotai_i2c_bus_t *bus;
static const uint8_t address = 0x3c;
static const uint8_t blank[1024];
static const xt_oled_operation_t start_order[] = {
    XT_OLED_PROBE, XT_OLED_NEW_IO, XT_OLED_NEW_PANEL, XT_OLED_CONFIGURE, XT_OLED_DRAW, XT_OLED_SWITCH,
};

static xiaotai_panel_ssd1306_config_t config(void)
{
    return (xiaotai_panel_ssd1306_config_t){
        .bus = bus, .address = address, .hz = 400000, .width = 128, .height = 64,
        .mirror_x = true, .mirror_y = true,
    };
}

static xiaotai_panel_t new_panel(uint8_t height, uint16_t timeout)
{
    xiaotai_panel_ssd1306_config_t c = config();
    c.height = height;
    c.transfer_timeout_ms = timeout;
    c.invert = height == 32;
    xiaotai_panel_t panel;
    assert(xiaotai_panel_ssd1306_create(&c, &panel) == XIAOTAI_OK);
    return panel;
}

static void expect_start(uint8_t height, uint16_t timeout)
{
    xt_oled_fake_reset();
    xiaotai_panel_t panel = new_panel(height, timeout);
    const xt_oled_fake_t *s = xt_oled_fake_state();
    assert(s->count == 0);
    assert(panel.set_on(panel.self, true) == XIAOTAI_ERR_INVALID_STATE);
    assert(panel.present(panel.self, NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_display_handles_t handles;
    assert(panel.start(panel.self, &handles) == XIAOTAI_OK);
    assert(!handles.panel && !handles.panel_io && !handles.swap_xy && handles.mirror_x && handles.mirror_y);
    assert(s->count == sizeof(start_order) / sizeof(start_order[0]));
    assert(!memcmp(s->log, start_order, sizeof(start_order)));
    assert(s->probe_timeout_ms == (timeout ? timeout : 100) && s->io_timeout_ms == s->probe_timeout_ms);
    assert(s->config.bus == bus && s->config.address == address && s->config.hz == 400000);
    assert(s->mirror_x && s->mirror_y && s->invert == (height == 32) && s->on);
    assert(s->frame_bytes == (size_t)128 * height / 8 && !memcmp(s->frame, blank, s->frame_bytes));
    assert(panel.start(panel.self, NULL) == XIAOTAI_ERR_INVALID_STATE);

    uint8_t pixels[1024];
    for (size_t i = 0; i < sizeof(pixels); ++i) pixels[i] = (uint8_t)(i ^ 0x5a);
    const xiaotai_rect_t bad[] = {
        {.width = 64, .height = height}, {.width = 128, .height = height - 1},
        {.x = 1, .width = 128, .height = height}, {.y = 1, .width = 128, .height = height},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(panel.present(panel.self, &bad[i], pixels) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_rect_t full = {.width = 128, .height = height};
    assert(panel.present(panel.self, NULL, pixels) == XIAOTAI_ERR_INVALID_ARG);
    assert(panel.present(panel.self, &full, NULL) == XIAOTAI_ERR_INVALID_ARG);
    assert(s->count == sizeof(start_order) / sizeof(start_order[0]));
    xt_oled_fake_fail(XT_OLED_DRAW, XIAOTAI_ERR_TIMEOUT);
    assert(panel.present(panel.self, &full, pixels) == XIAOTAI_ERR_TIMEOUT);
    assert(!memcmp(s->frame, blank, s->frame_bytes));
    assert(panel.present(panel.self, &full, pixels) == XIAOTAI_OK);
    assert(!memcmp(s->frame, pixels, s->frame_bytes));
    assert(panel.set_on(panel.self, false) == XIAOTAI_OK && !s->on);
    xt_oled_fake_fail(XT_OLED_SWITCH, XIAOTAI_ERR_TIMEOUT);
    assert(panel.set_on(panel.self, true) == XIAOTAI_ERR_TIMEOUT && !s->on);
    assert(panel.set_on(panel.self, true) == XIAOTAI_OK && s->on);
    assert(panel.stop(panel.self) == XIAOTAI_OK);
    assert(s->log[s->count - 2] == XT_OLED_DEL_PANEL && s->log[s->count - 1] == XT_OLED_DEL_IO);
    assert(panel.stop(panel.self) == XIAOTAI_OK);
    assert(panel.present(panel.self, &full, pixels) == XIAOTAI_ERR_INVALID_STATE);
}

static void expect_cleanup(void)
{
    xiaotai_panel_t panel = new_panel(64, 0);
    const xt_oled_fake_t *s = xt_oled_fake_state();
    size_t ops = sizeof(start_order) / sizeof(start_order[0]);
    for (size_t fail = 0; fail < ops; ++fail) {
        xt_oled_fake_reset();
        xt_oled_fake_fail(start_order[fail], XIAOTAI_ERR_TIMEOUT);
        xiaotai_display_handles_t handles = {.panel = &panel, .mirror_x = true};
        assert(panel.start(panel.self, &handles) == XIAOTAI_ERR_TIMEOUT);
        assert(!handles.panel && !handles.mirror_x);
        assert(!memcmp(s->log, start_order, (fail + 1) * sizeof(start_order[0])));
        assert(s->count == fail + 1 + (fail >= 2) + (fail >= 3));
        if (fail >= 3) assert(s->log[s->count - 2] == XT_OLED_DEL_PANEL);
        if (fail >= 2) assert(s->log[s->count - 1] == XT_OLED_DEL_IO);
        assert(panel.start(panel.self, NULL) == XIAOTAI_OK);
        assert(panel.stop(panel.self) == XIAOTAI_OK);
    }

    const xt_oled_operation_t release_failures[] = {XT_OLED_DEL_PANEL, XT_OLED_DEL_IO};
    for (size_t i = 0; i < sizeof(release_failures) / sizeof(release_failures[0]); ++i) {
        xt_oled_fake_reset();
        xt_oled_fake_fail(XT_OLED_DRAW, XIAOTAI_ERR_TIMEOUT);
        xt_oled_fake_fail(release_failures[i], XIAOTAI_FAIL);
        assert(panel.start(panel.self, NULL) == XIAOTAI_ERR_TIMEOUT);
        const size_t count = s->count;
        assert(s->log[count - 1] == release_failures[i]);
        assert(panel.start(panel.self, NULL) == XIAOTAI_ERR_INVALID_STATE && s->count == count);
        assert(panel.set_on(panel.self, true) == XIAOTAI_ERR_INVALID_STATE);
        assert(panel.stop(panel.self) == XIAOTAI_OK);
        assert(s->log[count] == release_failures[i]); /* retries the retained handle, not a freed one */
    }
}

static void expect_display(void)
{
    xt_oled_fake_reset();
    const xiaotai_display_config_t c = {
        .desc = {.width = 128, .height = 64, .pixel = XIAOTAI_PIXEL_MONO1_VLSB, .render = XIAOTAI_RENDER_FULL},
        .panel = new_panel(64, 0),
    };
    const xiaotai_display_t *display = NULL;
    const xt_oled_fake_t *s = xt_oled_fake_state();
    assert(xiaotai_display_create(&c, &display) == XIAOTAI_OK && display);
    assert(display->open(display->self, NULL) == XIAOTAI_OK && s->on);
    assert(display->open(display->self, NULL) == XIAOTAI_OK && s->count == 6);
    assert(display->set_backlight(display->self, 0) == XIAOTAI_OK && !s->on);
    assert(display->set_backlight(display->self, 1) == XIAOTAI_OK && s->on);
    assert(display->set_backlight(display->self, 101) == XIAOTAI_ERR_INVALID_ARG && s->on);
    const xiaotai_rect_t part = {.width = 64, .height = 64};
    assert(display->present(display->self, &part, blank) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_rect_t full = {.width = 128, .height = 64};
    assert(display->present(display->self, &full, blank) == XIAOTAI_OK);
    assert(c.panel.stop(c.panel.self) == XIAOTAI_OK); /* test teardown: old display has no close API */
}

int main(void)
{
    const xiaotai_i2c_config_t i2c = {.port = 0, .sda_pin = 0, .scl_pin = 1, .hz = 400000, .pullup = true};
    assert(xiaotai_i2c_create(&i2c, &bus) == XIAOTAI_OK);
    xiaotai_panel_t invalid;
    xiaotai_panel_ssd1306_config_t bad = config();
    bad.width = 64;
    assert(xiaotai_panel_ssd1306_create(&bad, &invalid) == XIAOTAI_ERR_INVALID_ARG && !invalid.self);
    bad = config();
    bad.height = 40;
    assert(xiaotai_panel_ssd1306_create(&bad, &invalid) == XIAOTAI_ERR_INVALID_ARG && !invalid.self);
    bad = config();
    bad.address = 0x80;
    assert(xiaotai_panel_ssd1306_create(&bad, &invalid) == XIAOTAI_ERR_INVALID_ARG && !invalid.self);
    const xiaotai_panel_t absent = new_panel(64, 0);
    assert(absent.start(absent.self, NULL) == XIAOTAI_ERR_NOT_FOUND && xt_oled_fake_state()->count == 1);
    assert(xiaotai_bus_host_i2c_attach(0, address) == XIAOTAI_OK);
    expect_start(64, 0);
    expect_start(32, 37);
    expect_cleanup();
    expect_display();
    return 0;
}
