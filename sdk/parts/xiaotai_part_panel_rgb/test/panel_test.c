// SPDX-License-Identifier: MIT
#include "panel_io.h"
#include "xiaotai/port/mem.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <string.h>

enum { PANEL, FB0, FB1 };
static int handles[3];
static char steps[32];
static size_t used;
static char fail_at, fail_release;
static bool omit_buffer;
static int reset_level = -1;

static xiaotai_panel_rgb_config_t base(void)
{
    xiaotai_panel_rgb_config_t c = {
        .pixel_clock_hz = 34000000, .width = 1024, .height = 600,
        .hsync_pulse = 24, .hsync_back_porch = 160, .hsync_front_porch = 136,
        .vsync_pulse = 1, .vsync_back_porch = 23, .vsync_front_porch = 11,
        .pclk_active_falling = true,
        .hsync_pin = 44, .vsync_pin = 45, .de_pin = 43, .pclk_pin = 40, .disp_pin = -1,
        .data_lines = 16, .data_pins = {36, 37, 38, 39, 46, 7, 35, 48, 49, 50, 51, 4, 5, 0, 1, 6},
        .buffers = 2, .dma_burst_bytes = 64,
    };
    return c;
}

static xiaotai_err_t step(char command)
{
    assert(used + 1 < sizeof(steps));
    steps[used++] = command;
    steps[used] = '\0';
    if (command == fail_at) return XIAOTAI_ERR_TIMEOUT;
    if (command == fail_release) return XIAOTAI_FAIL;
    return XIAOTAI_OK;
}

static xiaotai_err_t panel_open(const xiaotai_panel_rgb_config_t *config, void **out)
{
    assert(config->width == 1024 && config->height == 600 && config->pixel_clock_hz == 34000000);
    assert(config->data_lines == 16 && config->data_pins[15] == 6 && config->pclk_active_falling);
    const xiaotai_err_t err = step('P');
    if (err == XIAOTAI_OK) *out = &handles[PANEL];
    return err;
}
static xiaotai_err_t panel_close(void *handle) { assert(handle == &handles[PANEL]); return step('p'); }
static xiaotai_err_t enable(void *handle) { assert(handle == &handles[PANEL]); return step('S'); }
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
    assert(handle == &handles[PANEL] && area->width == 1024 && area->height == 600);
    assert(pixels == &handles[FB1]);
    return step('D');
}
static void native(void *handle, xiaotai_display_handles_t *out) { out->panel = handle; }

const rgb_io_t xiaotai_rgb_io = {
    .panel_open = panel_open, .panel_close = panel_close, .panel_enable = enable,
    .framebuffers = buffers, .present = present, .native_handles = native,
};

static xiaotai_err_t set_reset(void *self, bool active)
{
    (void)self;
    reset_level = active;
    return step(active ? 'R' : 'r');
}

static void reset_steps(void)
{
    used = 0;
    steps[0] = '\0';
    fail_at = fail_release = 0;
    omit_buffer = false;
}

static void test_rejects_incomplete_configs(void)
{
    xiaotai_panel_t panel;
    xiaotai_panel_rgb_config_t c = base();
    assert(xiaotai_panel_rgb_create(NULL, &panel) == XIAOTAI_ERR_INVALID_ARG);
    c.buffers = 3;
    assert(xiaotai_panel_rgb_create(&c, &panel) == XIAOTAI_ERR_INVALID_ARG);
    c = base();
    c.data_pins[7] = -1;
    assert(xiaotai_panel_rgb_create(&c, &panel) == XIAOTAI_ERR_INVALID_ARG);
    c = base();
    c.data_lines = 8;
    assert(xiaotai_panel_rgb_create(&c, &panel) == XIAOTAI_ERR_INVALID_ARG);
    c = base();
    c.pclk_pin = -1;
    assert(xiaotai_panel_rgb_create(&c, &panel) == XIAOTAI_ERR_INVALID_ARG);
}

static void test_start_reports_rgb_scanout_and_stops_in_order(void)
{
    reset_steps();
    xiaotai_panel_rgb_config_t c = base();
    c.reset = (xiaotai_line_t){.set = set_reset, .self = handles};
    xiaotai_panel_t panel;
    assert(xiaotai_panel_rgb_create(&c, &panel) == XIAOTAI_OK);
    xiaotai_display_handles_t h;
    assert(panel.start(panel.self, &h) == XIAOTAI_OK);
    assert(strcmp(steps, "RrPSF") == 0 && reset_level == 0);
    assert(h.scanout_kind == XIAOTAI_SCANOUT_RGB && h.panel == &handles[PANEL]);
    assert(h.framebuffers[0] == &handles[FB0] && h.framebuffers[1] == &handles[FB1]);
    /* Started again: the same handles, nothing touched. */
    xiaotai_display_handles_t again;
    assert(panel.start(panel.self, &again) == XIAOTAI_OK && strcmp(steps, "RrPSF") == 0);
    assert(memcmp(&again, &h, sizeof(h)) == 0);
    const xiaotai_rect_t full = {0, 0, 1024, 600}, part = {0, 0, 512, 600};
    assert(panel.present(panel.self, &part, &handles[FB1]) == XIAOTAI_ERR_INVALID_ARG);
    assert(panel.present(panel.self, &full, &handles[FB1]) == XIAOTAI_OK);
    assert(panel.stop(panel.self) == XIAOTAI_OK && strcmp(steps, "RrPSFDp") == 0);
    assert(panel.present(panel.self, &full, &handles[FB1]) == XIAOTAI_ERR_INVALID_STATE);
}

static void test_failed_start_releases_the_panel(void)
{
    const char failures[] = {'P', 'S', 'F'};
    for (size_t i = 0; i < sizeof(failures); ++i) {
        reset_steps();
        fail_at = failures[i];
        xiaotai_panel_rgb_config_t c = base();
        xiaotai_panel_t panel;
        assert(xiaotai_panel_rgb_create(&c, &panel) == XIAOTAI_OK);
        xiaotai_display_handles_t h;
        assert(panel.start(panel.self, &h) == XIAOTAI_ERR_TIMEOUT);
        /* Everything taken was released; nothing was taken after the failure. */
        const char *expected = failures[i] == 'P' ? "P" : failures[i] == 'S' ? "PSp" : "PSFp";
        assert(strcmp(steps, expected) == 0);
        assert(!panel.cleanup_failed(panel.self));
        assert(h.panel == NULL && h.framebuffers[0] == NULL);
    }
    reset_steps();
    omit_buffer = true;
    xiaotai_panel_rgb_config_t c = base();
    xiaotai_panel_t panel;
    assert(xiaotai_panel_rgb_create(&c, &panel) == XIAOTAI_OK);
    xiaotai_display_handles_t h;
    assert(panel.start(panel.self, &h) == XIAOTAI_ERR_INVALID_STATE && strcmp(steps, "PSFp") == 0);
}

static void test_close_failure_latches(void)
{
    reset_steps();
    xiaotai_panel_rgb_config_t c = base();
    xiaotai_panel_t panel;
    assert(xiaotai_panel_rgb_create(&c, &panel) == XIAOTAI_OK);
    xiaotai_display_handles_t h;
    assert(panel.start(panel.self, &h) == XIAOTAI_OK);
    fail_release = 'p';
    assert(panel.stop(panel.self) == XIAOTAI_FAIL);
    assert(panel.cleanup_failed(panel.self));
    assert(panel.start(panel.self, &h) == XIAOTAI_ERR_INVALID_STATE);
    assert(panel.stop(panel.self) == XIAOTAI_ERR_INVALID_STATE);
}

int main(void)
{
    test_rejects_incomplete_configs();
    test_start_reports_rgb_scanout_and_stops_in_order();
    test_failed_start_releases_the_panel();
    test_close_failure_latches();
    return 0;
}
