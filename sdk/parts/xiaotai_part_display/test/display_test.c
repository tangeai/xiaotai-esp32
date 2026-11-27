// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_wt_display_cleanup.py
// Replaces legacy: tests/test_cores3_board_display.py
// Replaces legacy: tests/regression/test_p4_display_port.py
// Replaces legacy: tests/test_native_helpers.py
#include "xiaotai/parts/display.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <string.h>

enum { POWER_UP = 'A', POWER_DOWN = 'a', RESET_UP = 'B', RESET_DOWN = 'b',
       SETTLE = '.', PANEL_START = 'P', PANEL_STOP = 'p', TOUCH_START = 'T',
       TOUCH_STOP = 't', DARK = 'D', LIGHT = 'L', PRESENT = 'F', READ = 'R',
       PANEL_ON = '+', PANEL_OFF = '-' };

typedef struct fixture fixture_t;
typedef struct { fixture_t *f; bool second; } line_t;
struct fixture {
    char trace[80];
    size_t next;
    int fail, fail_cleanup;
    bool power, reset, panel, touch, framebuffers, panel_on;
    uint8_t level;
    line_t lines[2];
    const void *expected_pixels;
    xiaotai_rect_t expected_area;
    xiaotai_touch_point_t samples[2];
    size_t sample_count;
};

static fixture_t *clock_fixture;
static int64_t now_us;

static xiaotai_err_t event(fixture_t *f, int id)
{
    assert(f->next + 1 < sizeof(f->trace));
    f->trace[f->next++] = (char)id;
    f->trace[f->next] = '\0';
    if (f->fail == id) {
        f->fail = 0;
        return XIAOTAI_ERR_TIMEOUT;
    }
    return f->fail_cleanup == id ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
}

static void trace(fixture_t *f, const char *expected)
{
    assert(strcmp(f->trace, expected) == 0);
    f->next = 0;
    f->trace[0] = '\0';
}

int64_t __wrap_xiaotai_time_us(void) { return now_us; }
void __wrap_xiaotai_delay_ms(uint32_t ms)
{
    assert(ms == 7);
    assert(event(clock_fixture, SETTLE) == XIAOTAI_OK);
}

static xiaotai_err_t line_set(void *self, bool active)
{
    line_t *line = self;
    fixture_t *f = line->f;
    /* A failed bus write can already have changed the output. */
    if (line->second) {
        f->reset = !active;
        return event(f, active ? RESET_DOWN : RESET_UP);
    }
    f->power = active;
    return event(f, active ? POWER_UP : POWER_DOWN);
}

static xiaotai_err_t panel_start(void *self, xiaotai_display_handles_t *out)
{
    fixture_t *f = self;
    const xiaotai_err_t err = event(f, PANEL_START);
    if (err != XIAOTAI_OK) return err;
    assert(!f->panel);
    f->panel = true;
    f->panel_on = true;
    *out = (xiaotai_display_handles_t){.panel = &f->panel, .panel_io = &f->power,
        .touch = &f->level, .swap_xy = true, .mirror_y = true,
        .framebuffers = {f->framebuffers ? &f->panel : NULL, f->framebuffers ? &f->reset : NULL}};
    return XIAOTAI_OK;
}

static xiaotai_err_t panel_stop(void *self)
{
    fixture_t *f = self;
    assert(f->panel);
    const xiaotai_err_t err = event(f, PANEL_STOP);
    if (err == XIAOTAI_OK) f->panel = false;
    return err;
}

static xiaotai_err_t panel_present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    fixture_t *f = self;
    assert(f->panel && pixels == f->expected_pixels);
    assert(area->x == f->expected_area.x && area->y == f->expected_area.y);
    assert(area->width == f->expected_area.width && area->height == f->expected_area.height);
    return event(f, PRESENT);
}

static xiaotai_err_t panel_switch(void *self, bool on)
{
    fixture_t *f = self;
    assert(f->panel);
    f->panel_on = on;
    return event(f, on ? PANEL_ON : PANEL_OFF);
}

static xiaotai_err_t touch_start(void *self, void **out)
{
    fixture_t *f = self;
    const xiaotai_err_t err = event(f, TOUCH_START);
    if (err != XIAOTAI_OK) return err;
    assert(!f->touch);
    f->touch = true;
    *out = &f->touch;
    return XIAOTAI_OK;
}

static xiaotai_err_t touch_stop(void *self)
{
    fixture_t *f = self;
    assert(f->touch);
    f->touch = false;
    return event(f, TOUCH_STOP);
}

static xiaotai_err_t touch_read(void *self, xiaotai_touch_point_t *points, size_t capacity, size_t *count)
{
    fixture_t *f = self;
    assert(f->touch);
    for (size_t i = 0; i < f->sample_count && i < capacity; ++i) points[i] = f->samples[i];
    *count = f->sample_count;  /* also simulates a driver that violates the count contract */
    return event(f, READ);
}

static xiaotai_err_t light(void *self, uint8_t percent)
{
    fixture_t *f = self;
    f->level = percent;
    return event(f, percent ? LIGHT : DARK);
}

static xiaotai_display_config_t config_for(fixture_t *f)
{
    f->lines[0] = (line_t){f, false};
    f->lines[1] = (line_t){f, true};
    clock_fixture = f;
    return (xiaotai_display_config_t){
        .desc = {.width = 480, .height = 320, .rotation = 3, .pixel = XIAOTAI_PIXEL_RGB565,
                 .render = XIAOTAI_RENDER_PARTIAL, .transfer_pixels = 320 * 24},
        .panel = {panel_start, panel_stop, panel_present, NULL, f},
        .touch = {touch_start, touch_stop, touch_read, f},
        .backlight = {.set = light, .self = f},
        .power = {{.line = {.set = line_set, .self = &f->lines[0]}, .active = true, .settle_ms = 7},
                  {.line = {.set = line_set, .self = &f->lines[1]}, .active = false}},
        .power_steps = 2, .touch_filter = {30, 3, 24},
    };
}

static const xiaotai_display_t *make(xiaotai_display_config_t *cfg)
{
    const xiaotai_display_t *display = NULL;
    assert(xiaotai_display_create(cfg, &display) == XIAOTAI_OK && display);
    return display;
}

static xiaotai_display_part_stats_t stats(const xiaotai_display_t *display)
{
    xiaotai_display_part_stats_t result;
    xiaotai_display_part_stats(display, &result);
    return result;
}

/* These are fake controller handles with no lifetime beyond one fixture. */
static void done(const xiaotai_display_t *display) { xiaotai_mem_free(display->self); }

static void open_rollback(void)
{
    static const struct { bool touch_first; int fail; const char *expected; } cases[] = {
        {false, POWER_UP, "Aa"}, {false, RESET_UP, "A.Bba"},
        {false, PANEL_START, "A.BPba"}, {false, TOUCH_START, "A.BPTpba"},
        {false, DARK, "A.BPTDDtpba"}, {true, TOUCH_START, "A.BTba"},
        {true, PANEL_START, "A.BTPtba"}, {true, DARK, "A.BTPDDptba"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fixture_t f = {.fail = cases[i].fail};
        xiaotai_display_config_t cfg = config_for(&f);
        cfg.touch_first = cases[i].touch_first;
        const xiaotai_display_t *display = make(&cfg);
        trace(&f, "");
        xiaotai_display_handles_t out = {.panel = &f, .panel_io = &f, .touch = &f, .mirror_x = true};
        assert(display->open(display->self, &out) == XIAOTAI_ERR_TIMEOUT);
        trace(&f, cases[i].expected);
        assert(!out.panel && !out.panel_io && !out.touch && !out.mirror_x);
        assert(!f.power && !f.reset && !f.panel && !f.touch && !f.level);
        const xiaotai_display_part_stats_t before = stats(display);
        assert(!before.open && !before.latched && !before.opens);
        assert(before.open_failures == 1 && before.last_error == XIAOTAI_ERR_TIMEOUT);
        assert(display->open(display->self, &out) == XIAOTAI_OK);
        trace(&f, cases[i].touch_first ? "A.BTPD" : "A.BPTD");
        assert(out.panel == &f.panel && out.panel_io == &f.power && out.touch == &f.touch);
        assert(stats(display).open && stats(display).opens == 1);
        done(display);
    }

    fixture_t f = {.fail = DARK, .fail_cleanup = PANEL_STOP};
    xiaotai_display_config_t cfg = config_for(&f);
    cfg.touch_first = true;
    const xiaotai_display_t *display = make(&cfg);
    assert(display->open(display->self, NULL) == XIAOTAI_ERR_TIMEOUT);
    trace(&f, "A.BTPDDptba");
    assert(f.panel && !f.touch && !f.power && !f.reset);
    assert(stats(display).latched && !stats(display).open);
    assert(display->open(display->self, NULL) == XIAOTAI_ERR_INVALID_STATE);
    trace(&f, "");
    assert(stats(display).open_failures == 2 && stats(display).last_error == XIAOTAI_ERR_TIMEOUT);
    done(display);
}

static void backlight_and_cached_handles(void)
{
    fixture_t f = {.level = 73};
    xiaotai_display_config_t cfg = config_for(&f);
    const xiaotai_display_t *display = make(&cfg);
    assert(display->desc.touch && display->desc.width == 480 && display->desc.rotation == 3);
    assert(display->set_backlight(display->self, 11) == XIAOTAI_ERR_INVALID_STATE);
    cfg.touch_first = true;
    cfg.power_steps = 0;
    cfg.backlight.dimmable = true;
    trace(&f, "");
    assert(display->open(display->self, NULL) == XIAOTAI_OK && f.level == 0);
    trace(&f, "A.BPTD");
    xiaotai_display_handles_t out;
    assert(display->open(display->self, &out) == XIAOTAI_OK);
    assert(out.panel == &f.panel && out.panel_io == &f.power && out.touch == &f.touch);
    assert(out.swap_xy && !out.mirror_x && out.mirror_y && stats(display).opens == 1);
    trace(&f, "");
    assert(display->set_backlight(display->self, 255) == XIAOTAI_ERR_INVALID_ARG);
    assert(display->set_backlight(display->self, 11) == XIAOTAI_OK && f.level == 100);
    f.fail = DARK;
    assert(display->set_backlight(display->self, 0) == XIAOTAI_ERR_TIMEOUT && f.level == 0);
    assert(display->set_backlight(display->self, 0) == XIAOTAI_OK && f.level == 0);
    trace(&f, "LDD");
    done(display);

    f = (fixture_t){0};
    cfg = config_for(&f);
    cfg.backlight.dimmable = true;
    cfg.desc.flags = XIAOTAI_DISPLAY_DIMMABLE | XIAOTAI_DISPLAY_SWAP_BYTES;
    display = make(&cfg);
    assert(display->desc.flags == cfg.desc.flags);
    assert(display->open(display->self, NULL) == XIAOTAI_OK);
    assert(display->set_backlight(display->self, 11) == XIAOTAI_OK && f.level == 11);
    trace(&f, "A.BPTDL");
    done(display);
}

static void render_contracts(void)
{
    fixture_t f = {0};
    xiaotai_display_config_t cfg = config_for(&f);
    const xiaotai_display_t *display = make(&cfg);
    const uint16_t dummy = 0;
    f.expected_pixels = &dummy;
    f.expected_area = (xiaotai_rect_t){.x = 319, .y = 478, .width = 1, .height = 2};
    assert(display->present(display->self, &f.expected_area, &dummy) == XIAOTAI_ERR_INVALID_STATE);
    assert(display->open(display->self, NULL) == XIAOTAI_OK);
    assert(display->present(display->self, &f.expected_area, &dummy) == XIAOTAI_OK);
    f.fail = PRESENT;
    assert(display->present(display->self, &f.expected_area, &dummy) == XIAOTAI_ERR_TIMEOUT);
    assert(stats(display).present_errors == 1 && stats(display).last_error == XIAOTAI_ERR_TIMEOUT);
    const xiaotai_rect_t invalid = {.x = UINT16_MAX, .width = 2, .height = 1};
    assert(display->present(display->self, &invalid, &dummy) == XIAOTAI_ERR_INVALID_ARG);
    assert(display->present(display->self, NULL, &dummy) == XIAOTAI_ERR_INVALID_ARG);
    assert(display->present(display->self, &f.expected_area, NULL) == XIAOTAI_ERR_INVALID_ARG);
    trace(&f, "A.BPTDFF");
    done(display);

    f = (fixture_t){0};
    cfg = config_for(&f);
    cfg.power_steps = 0;
    cfg.desc = (xiaotai_display_desc_t){.width = 128, .height = 32, .pixel = XIAOTAI_PIXEL_MONO1_VLSB,
        .render = XIAOTAI_RENDER_FULL, .touch = true};
    cfg.touch = (xiaotai_touch_t){0};
    cfg.backlight = (xiaotai_level_t){0};
    cfg.panel.set_on = panel_switch;
    display = make(&cfg);
    assert(!display->desc.touch && !display->read_touch);
    assert(display->open(display->self, NULL) == XIAOTAI_OK && f.panel_on);
    assert(display->set_backlight(display->self, 0) == XIAOTAI_OK && !f.panel_on);
    assert(display->set_backlight(display->self, 2) == XIAOTAI_OK && f.panel_on);
    f.expected_pixels = &dummy;
    f.expected_area = (xiaotai_rect_t){.width = 128, .height = 32};
    const xiaotai_rect_t partial = {.width = 128, .height = 31};
    assert(display->present(display->self, &partial, &dummy) == XIAOTAI_ERR_INVALID_ARG);
    assert(display->present(display->self, &f.expected_area, &dummy) == XIAOTAI_OK);
    trace(&f, "P-+F");
    done(display);

    f = (fixture_t){0};
    cfg = config_for(&f);
    cfg.power_steps = 0;
    cfg.desc.render = XIAOTAI_RENDER_SCANOUT;
    cfg.desc.buffers = 2;
    cfg.desc.transfer_pixels = 0;
    cfg.backlight = (xiaotai_level_t){0};
    display = make(&cfg);
    assert(!display->set_backlight);
    assert(display->open(display->self, NULL) == XIAOTAI_ERR_INVALID_STATE);
    trace(&f, "Pp");
    f.framebuffers = true;
    xiaotai_display_handles_t out;
    assert(display->open(display->self, &out) == XIAOTAI_OK);
    assert(out.framebuffers[0] == &f.panel && out.framebuffers[1] == &f.reset);
    f.expected_pixels = out.framebuffers[0];
    f.expected_area = (xiaotai_rect_t){.width = 320, .height = 480};
    assert(display->present(display->self, &partial, f.expected_pixels) == XIAOTAI_ERR_INVALID_ARG);
    assert(display->present(display->self, &f.expected_area, f.expected_pixels) == XIAOTAI_OK);
    trace(&f, "PTF");
    done(display);
}

static void touch_pipeline(void)
{
    fixture_t f = {0};
    xiaotai_display_config_t cfg = config_for(&f);
    const xiaotai_display_t *display = make(&cfg);
    xiaotai_touch_point_t point[2];
    size_t count = 7;
    assert(display->read_touch(display->self, point, 2, &count) == XIAOTAI_ERR_INVALID_STATE && !count);
    assert(display->open(display->self, NULL) == XIAOTAI_OK);
    assert(display->read_touch(display->self, NULL, 0, &count) == XIAOTAI_OK && !count);
    assert(display->read_touch(display->self, NULL, 2, &count) == XIAOTAI_ERR_INVALID_ARG);
    f.sample_count = 2;
    f.samples[0] = (xiaotai_touch_point_t){.x = 100, .y = 200};
    f.samples[1] = (xiaotai_touch_point_t){.x = 450, .y = 590};
    now_us = 100000;
    assert(display->read_touch(display->self, point, 2, &count) == XIAOTAI_OK && count == 2);
    assert(point[0].x == 100 && point[0].y == 200 && point[1].x == 319 && point[1].y == 479);
    f.samples[0] = (xiaotai_touch_point_t){.x = 103, .y = 177};
    f.samples[1] = (xiaotai_touch_point_t){.x = 81, .y = 77};
    now_us += 30000;
    assert(display->read_touch(display->self, point, 2, &count) == XIAOTAI_OK && count == 2);
    assert(point[0].x == 100 && point[0].y == 189 && point[1].x == 81 && point[1].y == 77);
    f.samples[0] = (xiaotai_touch_point_t){.x = 124, .y = 192};
    now_us += 1000;
    assert(display->read_touch(display->self, point, 2, &count) == XIAOTAI_OK);
    assert(point[0].x == 124 && point[0].y == 189);
    f.samples[0] = (xiaotai_touch_point_t){.x = 125, .y = 190};
    now_us += 31000;
    assert(display->read_touch(display->self, point, 2, &count) == XIAOTAI_OK);
    assert(point[0].x == 125 && point[0].y == 190);
    f.sample_count = 0;
    assert(display->read_touch(display->self, point, 2, &count) == XIAOTAI_OK && count == 0);
    f.sample_count = 2;
    assert(display->read_touch(display->self, point, 1, &count) == XIAOTAI_ERR_INVALID_SIZE && !count);
    f.fail = READ;
    assert(display->read_touch(display->self, point, 2, &count) == XIAOTAI_ERR_TIMEOUT && !count);
    f.samples[0] = (xiaotai_touch_point_t){.x = 127, .y = 192};
    assert(display->read_touch(display->self, point, 2, &count) == XIAOTAI_OK);
    assert(point[0].x == 127 && point[0].y == 192);
    assert(stats(display).touch_errors == 2 && stats(display).last_error == XIAOTAI_ERR_TIMEOUT);
    trace(&f, "A.BPTDRRRRRRRR");
    done(display);
}

static void invalid_configs_and_pure_filter(void)
{
    fixture_t f = {0};
    const xiaotai_display_config_t good = config_for(&f);
    const uint32_t allocations_before = xiaotai_mem_host_allocations();
    assert(xiaotai_display_create(&good, NULL) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_mem_host_allocations() == allocations_before);
    const xiaotai_display_t other = {0};
    const xiaotai_display_t *display = &other;
    assert(xiaotai_display_create(NULL, &display) == XIAOTAI_ERR_INVALID_ARG && !display);
    for (int i = 0; i < 8; ++i) {
        xiaotai_display_config_t cfg = good;
        if (i == 0) cfg.desc.width = 0;
        if (i == 1) cfg.desc.render = XIAOTAI_RENDER_SCANOUT;
        if (i == 2) cfg.desc.pixel = XIAOTAI_PIXEL_MONO1_VLSB;
        if (i == 3) cfg.desc.rotation = 4;
        if (i == 4) cfg.desc.transfer_pixels = 0;
        if (i == 5) cfg.touch.read = NULL;
        if (i == 6) cfg.panel.stop = NULL;
        if (i == 7) cfg.power_steps = XIAOTAI_DISPLAY_POWER_STEPS_MAX + 1;
        assert(xiaotai_display_create(&cfg, &display) == XIAOTAI_ERR_INVALID_ARG && !display);
    }
    assert(!stats(&other).open && !stats(NULL).opens);
    xiaotai_mem_host_fail_nth(1);
    assert(xiaotai_display_create(&good, &display) == XIAOTAI_ERR_NO_MEM && !display);
    xiaotai_mem_host_fail_nth(0);
    trace(&f, "");

    xiaotai_touch_filter_state_t state = {0};
    const xiaotai_touch_filter_t disabled = {0};
    xiaotai_touch_point_t point = {.x = 500, .y = 300};
    xiaotai_touch_filter_step(&disabled, &state, INT64_MIN, &point);
    point = (xiaotai_touch_point_t){.x = 501, .y = 299};
    xiaotai_touch_filter_step(&disabled, &state, INT64_MIN, &point);
    assert(point.x == 501 && point.y == 299);
    point = (xiaotai_touch_point_t){.x = 502, .y = 300};
    xiaotai_touch_filter_step(&good.touch_filter, &state, INT64_MAX, &point);
    assert(point.x == 502 && point.y == 300);
    point = (xiaotai_touch_point_t){.x = UINT16_MAX};
    xiaotai_touch_filter_step(&good.touch_filter, &state, INT64_MAX, &point);
    assert(point.x == UINT16_MAX && point.y == 0);
}

int main(void)
{
    open_rollback();
    backlight_and_cached_handles();
    render_contracts();
    touch_pipeline();
    invalid_configs_and_pure_filter();
    return 0;
}
