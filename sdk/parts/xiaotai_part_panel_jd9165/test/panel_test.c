// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_wt_display_cleanup.py
#include "panel_io.h"
#include "xiaotai/port/mem.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <string.h>

enum { PHY, BUS, DBI, PANEL, FB0, FB1 };
static int handles[6];
static char steps[32];
static size_t used;
static char fail_at, fail_release;
static bool expect_software_reset, omit_buffer;
static const xiaotai_panel_jd9165_config_t base = {
    .lanes = 2, .lane_mbps = 600, .phy_ldo_channel = 3, .phy_ldo_mv = 2500,
    .pixel_clock_hz = 40000000, .width = 1024, .height = 600, .buffers = 2,
    .hsync_back_porch = 136, .hsync_pulse = 24, .hsync_front_porch = 160,
    .vsync_back_porch = 21, .vsync_pulse = 2, .vsync_front_porch = 12,
};

static xiaotai_err_t step(char command)
{
    assert(used + 1 < sizeof(steps));
    steps[used++] = command;
    steps[used] = '\0';
    if (command == fail_at) return XIAOTAI_ERR_TIMEOUT;
    if (command == fail_release) return XIAOTAI_FAIL;
    return XIAOTAI_OK;
}

static xiaotai_err_t acquire(char command, int id, void **out)
{
    const xiaotai_err_t err = step(command);
    if (err == XIAOTAI_OK) *out = &handles[id];
    return err;
}

static xiaotai_err_t phy_open(const xiaotai_panel_jd9165_config_t *config, void **out)
{
    assert(config->phy_ldo_channel == 3 && config->phy_ldo_mv == 2500);
    return acquire('L', PHY, out);
}
static xiaotai_err_t phy_close(void *handle) { assert(handle == &handles[PHY]); return step('l'); }
static xiaotai_err_t bus_open(const xiaotai_panel_jd9165_config_t *config, void **out)
{
    assert(config->lanes == 2 && config->lane_mbps == 600);
    return acquire('B', BUS, out);
}
static xiaotai_err_t bus_close(void *handle) { assert(handle == &handles[BUS]); return step('b'); }
static xiaotai_err_t io_open(void *bus, void **out) { assert(bus == &handles[BUS]); return acquire('I', DBI, out); }
static xiaotai_err_t io_close(void *handle) { assert(handle == &handles[DBI]); return step('i'); }
static xiaotai_err_t panel_open(const xiaotai_panel_jd9165_config_t *config, void *bus, void *io, void **out)
{
    assert(config->pixel_clock_hz == 40000000 && config->height == 600 && config->width == 1024);
    assert(bus == &handles[BUS] && io == &handles[DBI]);
    return acquire('P', PANEL, out);
}
static xiaotai_err_t panel_close(void *handle) { assert(handle == &handles[PANEL]); return step('p'); }
static xiaotai_err_t enable(void *handle, bool soft_reset)
{
    assert(handle == &handles[PANEL] && soft_reset == expect_software_reset);
    return step('S');
}
static xiaotai_err_t buffers(void *handle, uint8_t count, void *out[2])
{
    assert(handle == &handles[PANEL] && (count == 1 || count == 2));
    const xiaotai_err_t err = step('F');
    if (err == XIAOTAI_OK) {
        out[0] = &handles[FB0];
        if (count == 2 && !omit_buffer) out[1] = &handles[FB1];
    }
    return err;
}
static xiaotai_err_t present(void *handle, const xiaotai_rect_t *area, const void *pixels)
{
    assert(handle == &handles[PANEL] && area->width == base.width && area->height == base.height);
    assert(pixels == &handles[FB1] || pixels == &handles[PHY]);
    return step('D');
}

static void native_handles(void *panel, void *io, xiaotai_display_handles_t *out)
{
    out->panel = panel;
    out->panel_io = io;
}

const jd9165_io_t xiaotai_jd9165_io = {
    .phy_open = phy_open, .phy_close = phy_close, .bus_open = bus_open, .bus_close = bus_close,
    .io_open = io_open, .io_close = io_close, .panel_open = panel_open, .panel_close = panel_close,
    .panel_enable = enable, .framebuffers = buffers, .present = present,
    .native_handles = native_handles,
};

static void trace(void) { used = 0; steps[0] = '\0'; }

static void test_acquisition_rollback(void)
{
    const char *const expected[] = {"L", "LBl", "LBIbl", "LBIPibl", "LBIPSpibl", "LBIPSFpibl"};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        xiaotai_panel_t p;
        trace();
        expect_software_reset = true;
        assert(xiaotai_panel_jd9165_create(&base, &p) == XIAOTAI_OK && used == 0);
        fail_at = "LBIPSF"[i];
        xiaotai_display_handles_t out = {.panel = &handles[PANEL]};
        assert(p.start(p.self, &out) == XIAOTAI_ERR_TIMEOUT);
        assert(!out.panel && !out.panel_io && !out.framebuffers[0] && !out.framebuffers[1] &&
               out.scanout_kind == XIAOTAI_SCANOUT_UNSPECIFIED);
        assert(!strcmp(expected[i], steps));
        assert(p.present(p.self, NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
        fail_at = 0;
        trace();
        assert(p.start(p.self, &out) == XIAOTAI_OK);
        assert(out.panel == &handles[PANEL] && out.panel_io == &handles[DBI]);
        assert(out.framebuffers[0] == &handles[FB0] && out.framebuffers[1] == &handles[FB1]);
        assert(!out.mirror_x && !out.mirror_y && !out.swap_xy);
        assert(out.scanout_kind == XIAOTAI_SCANOUT_DPI);
        assert(p.start(p.self, &out) == XIAOTAI_OK && !strcmp(steps, "LBIPSF"));
        const xiaotai_rect_t whole = {.width = 1024, .height = 600};
        assert(p.present(p.self, &whole, &handles[FB1]) == XIAOTAI_OK);
        assert(p.present(p.self, &whole, &handles[PHY]) == XIAOTAI_OK);
        assert(p.present(p.self, &(xiaotai_rect_t){.width = 1023, .height = 600}, &handles[PHY]) == XIAOTAI_ERR_INVALID_ARG);
        assert(p.stop(p.self) == XIAOTAI_OK && !strcmp(steps, "LBIPSFDDpibl"));
        assert(p.stop(p.self) == XIAOTAI_OK && !strcmp(steps, "LBIPSFDDpibl"));
        xiaotai_mem_free(p.self);
    }
}

static void test_release_failure_latches(void)
{
    for (size_t i = 0; i < 4; ++i) {
        xiaotai_panel_t p;
        xiaotai_display_handles_t out;
        expect_software_reset = true;
        trace();
        assert(xiaotai_panel_jd9165_create(&base, &p) == XIAOTAI_OK);
        fail_at = 'S';
        fail_release = "pibl"[i];
        assert(p.start(p.self, &out) == XIAOTAI_ERR_TIMEOUT);
        assert(!strncmp(steps, "LBIPSpibl", 6 + i) && used == 6 + i && !out.panel);
        fail_at = fail_release = 0;
        assert(p.start(p.self, &out) == XIAOTAI_ERR_INVALID_STATE && used == 6 + i);
        assert(p.stop(p.self) == XIAOTAI_ERR_INVALID_STATE && used == 6 + i);
        xiaotai_mem_free(p.self);
    }
}

static xiaotai_err_t reset(void *self, bool active) { assert(!self); return step(active ? 'R' : 'r'); }

static void test_reset_and_buffer_contract(void)
{
    xiaotai_panel_t p;
    xiaotai_panel_jd9165_config_t config = base;
    config.phy_ldo_channel = -1;
    config.phy_ldo_mv = 0;
    config.buffers = 1;
    config.reset = (xiaotai_line_t){.set = reset};
    expect_software_reset = false;
    assert(xiaotai_panel_jd9165_create(&config, &p) == XIAOTAI_OK);
    config.lanes = 1;
    config.width = 1;
    xiaotai_display_handles_t out;
    trace();
    fail_at = 'R';
    assert(p.start(p.self, &out) == XIAOTAI_ERR_TIMEOUT);
    assert(!strcmp(steps, "BIPRrpib") && !out.panel);
    trace();
    fail_at = 0;
    assert(p.start(p.self, &out) == XIAOTAI_OK && !strcmp(steps, "BIPRrSF"));
    assert(out.framebuffers[0] == &handles[FB0] && !out.framebuffers[1]);
    assert(p.stop(p.self) == XIAOTAI_OK && !strcmp(steps, "BIPRrSFpib"));
    trace();
    fail_at = 'r';
    assert(p.start(p.self, &out) == XIAOTAI_ERR_TIMEOUT && !strcmp(steps, "BIPRrpib"));
    fail_at = 0;
    assert(p.start(p.self, &out) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_mem_free(p.self);

    expect_software_reset = true;
    omit_buffer = true;
    trace();
    assert(xiaotai_panel_jd9165_create(&base, &p) == XIAOTAI_OK);
    assert(p.start(p.self, &out) == XIAOTAI_ERR_INVALID_STATE && !strcmp(steps, "LBIPSFpibl"));
    assert(!out.panel && !out.framebuffers[0]);
    xiaotai_mem_free(p.self);
}

int main(void)
{
    assert(xiaotai_panel_jd9165_create(&base, NULL) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_panel_t p;
    assert(xiaotai_panel_jd9165_create(NULL, &p) == XIAOTAI_ERR_INVALID_ARG && !p.self);
    xiaotai_mem_host_fail_nth(1);
    assert(xiaotai_panel_jd9165_create(&base, &p) == XIAOTAI_ERR_NO_MEM && !p.self);
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        test_acquisition_rollback();
        assert(xiaotai_mem_host_outstanding() == 0);
    }
    test_release_failure_latches();
    test_reset_and_buffer_contract();
    assert(xiaotai_mem_host_outstanding() == 0);
    return 0;
}
