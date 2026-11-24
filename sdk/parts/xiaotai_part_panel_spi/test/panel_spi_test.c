// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_board_rollback.py
// Replaces legacy: tests/test_s3_panel_mapping.py
#include "spi_fake.h"
#include "xiaotai/parts/panel_ili9341.h"
#include "xiaotai/parts/panel_st7796.h"
#include "xiaotai_mem_host.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool set, fail_high, fail_low;
    unsigned changes;
    size_t last_call;
} line_t;

static const xt_spi_fake_t *fake;
static uint32_t delays[16];
static size_t delay_count;

void __wrap_xiaotai_delay_ms(uint32_t ms)
{
    assert(delay_count < sizeof(delays) / sizeof(delays[0]));
    delays[delay_count++] = ms;
}

static xiaotai_err_t set_line(void *self, bool active)
{
    line_t *l = self;
    l->set = active;
    ++l->changes;
    l->last_call = fake->calls;
    bool *fail = active ? &l->fail_high : &l->fail_low;
    if (*fail) {
        *fail = false;
        return XIAOTAI_ERR_TIMEOUT; /* wrote the line before failing */
    }
    return XIAOTAI_OK;
}

static xiaotai_panel_spi_config_t config(void)
{
    return (xiaotai_panel_spi_config_t){
        .host = 2, .mosi_pin = 37, .sclk_pin = 36, .dc_pin = 35, .cs_pin = 3,
        .hz = 40000000, .queue_depth = 4, .native_width = 6, .native_height = 4,
        .max_transfer_bytes = 18, .wait_done = true,
    };
}

static xiaotai_panel_t make(const xiaotai_panel_spi_controller_t *controller, const xiaotai_panel_spi_config_t *c)
{
    xiaotai_panel_t p;
    assert(xiaotai_panel_spi_create(controller, c, &p) == XIAOTAI_OK);
    assert(p.self && p.start && p.stop && p.present && !p.set_on);
    return p;
}

static void check_lck_orientation(void)
{
    xt_spi_fake_reset();
    line_t cs = {0};
    xiaotai_panel_spi_config_t c = config();
    c.native_width = 240; c.native_height = 320;
    c.cs_pin = -1;
    c.cs_line = (xiaotai_line_t){.self = &cs, .set = set_line};
    c.max_transfer_bytes = 6400; c.hz = 80000000; c.spi_mode = 2;
    c.swap_xy = c.mirror_x = c.invert = true;
    c.wait_done = false;
    xiaotai_panel_t p = make(xiaotai_panel_st7789(), &c);
    assert(fake->calls == 0 && !cs.changes);
    xiaotai_display_handles_t handles;
    assert(p.start(p.self, &handles) == XIAOTAI_OK);
    assert(handles.swap_xy && handles.mirror_x && !handles.mirror_y && !handles.panel && !handles.panel_io);
    assert(fake->limit == 6400 && fake->config.hz == 80000000 && fake->config.spi_mode == 2 &&
           fake->config.cs_pin == -1 && fake->config.invert && !fake->config.bgr);
    assert(cs.set && cs.last_call == 4 && fake->log[3] == XT_SPI_RESET && fake->log[4] == XT_SPI_SETUP);
    assert(fake->pixels_painted == 320U * 240U && !fake->nonblack && !fake->queued);
    assert(fake->log[fake->calls - 1] == XT_SPI_ON && fake->log[fake->calls - 2] == XT_SPI_DRAIN);
    const xiaotai_rect_t end = {.x = 319, .y = 239, .width = 1, .height = 1};
    const uint16_t pixel = 0xabcd;
    assert(p.present(p.self, &end, &pixel) == XIAOTAI_OK);
    assert(p.stop(p.self) == XIAOTAI_OK && !cs.set && cs.changes == 2);
}

static void check_s3_panel_mapping(void)
{
    for (unsigned partial = 0; partial < 2; ++partial) {
        for (unsigned alternate = 0; alternate < 2; ++alternate) {
            xt_spi_fake_reset();
            xiaotai_panel_spi_config_t c = config();
            c.swap_xy = c.mirror_x = partial == alternate;
            c.mirror_y = alternate != 0;
            c.native_width = c.swap_xy ? 240U : 320U;
            c.native_height = c.swap_xy ? 320U : 240U;
            c.max_transfer_bytes = (alternate ? 1600U : 3200U) * sizeof(uint16_t);
            c.wait_done = partial != 0;
            xiaotai_panel_t p = make(xiaotai_panel_st7789(), &c);
            xiaotai_display_handles_t handles;
            assert(p.start(p.self, &handles) == XIAOTAI_OK);
            assert(handles.swap_xy == c.swap_xy && handles.mirror_x == c.mirror_x && handles.mirror_y == c.mirror_y);
            assert(fake->config.swap_xy == c.swap_xy && fake->config.mirror_x == c.mirror_x &&
                   fake->config.mirror_y == c.mirror_y && fake->limit == c.max_transfer_bytes);
            assert(fake->pixels_painted == 320U * 240U && p.stop(p.self) == XIAOTAI_OK);
        }
    }
}

static void check_waveshare_direct(void)
{
    xt_spi_fake_reset();
    const uint8_t expected[] = {1, 2, 3, 4, 5, 6, 7, 8};
    line_t rst = {0};
    xiaotai_panel_spi_config_t c = config();
    c.native_width = 320; c.native_height = 480;
    c.mirror_x = c.bgr = c.invert = c.external_dma = true;
    c.max_transfer_bytes = 0; c.hz = 80000000; c.queue_depth = 20;
    c.reset = (xiaotai_line_t){.self = &rst, .set = set_line};
    c.reset_hold_ms = 10; c.reset_settle_ms = 120;
    xiaotai_panel_t p = make(xiaotai_panel_st7796(), &c);
    xiaotai_display_handles_t handles;
    delay_count = 0;
    assert(p.start(p.self, &handles) == XIAOTAI_OK);
    assert(delay_count == 2 && delays[0] == 10 && delays[1] == 120);
    assert(rst.changes == 2 && !rst.set && !fake->count[XT_SPI_RESET]);
    assert(!handles.swap_xy && handles.mirror_x && !handles.mirror_y);
    assert(fake->limit == 307200 && fake->config.external_dma && fake->config.wait_done &&
           fake->config.hz == 80000000 && fake->config.queue_depth == 20);
    assert(fake->pixels_painted == 320U * 480U && !fake->nonblack);
    xt_spi_fake_reset();
    xiaotai_mem_host_clear();
    const xiaotai_rect_t frame = {.width = 320, .height = 480};
    uint8_t *full = calloc(1, 307200);
    assert(full);
    full[0] = 0x53;
    assert(p.present(p.self, &frame, full) == XIAOTAI_OK);
    assert(fake->draws == 1 && fake->pixels_painted == 153600 && !fake->queued && fake->last[0] == 0x53);
    assert(fake->count[XT_SPI_DRAIN] == 1 && xiaotai_mem_host_allocations() == 0);
    free(full);
    xt_spi_fake_reset();
    const xiaotai_rect_t region = {.x = 2, .y = 3, .width = 2, .height = 2};
    uint8_t pixels[8];
    memcpy(pixels, expected, sizeof(pixels));
    assert(p.present(p.self, &region, pixels) == XIAOTAI_OK);
    assert(!fake->queued && fake->draws == 1 && xiaotai_mem_host_allocations() == 0);
    assert(fake->count[XT_SPI_DRAIN] == 1 && !memcmp(fake->last, expected, sizeof(expected)));
    xt_spi_fake_fail(XT_SPI_DRAW, 1, XIAOTAI_ERR_TIMEOUT);
    assert(p.present(p.self, &region, pixels) == XIAOTAI_ERR_TIMEOUT);
    assert(!fake->queued && fake->count[XT_SPI_DRAIN] == 2);
    assert(p.stop(p.self) == XIAOTAI_OK);
}

static void check_staged_and_tiling(void)
{
    xt_spi_fake_reset();
    xiaotai_panel_spi_config_t c = config();
    c.wait_done = false;
    c.max_transfer_bytes = 6;
    xiaotai_panel_t p = make(xiaotai_panel_st7789(), &c);
    assert(p.start(p.self, NULL) == XIAOTAI_OK);
    xt_spi_fake_reset();
    xiaotai_mem_host_clear();
    const xiaotai_rect_t area = {.x = 1, .y = 1, .width = 5, .height = 2};
    uint8_t pixels[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    xiaotai_mem_host_fail_nth(1);
    assert(p.present(p.self, &area, pixels) == XIAOTAI_ERR_NO_MEM && !fake->draws);
    assert(p.present(p.self, &area, pixels) == XIAOTAI_OK);
    assert(fake->draws == 4 && fake->count[XT_SPI_DRAIN] == 3 && fake->queued != pixels);
    const xiaotai_rect_t expect[4] = {{1, 1, 3, 1}, {4, 1, 2, 1}, {1, 2, 3, 1}, {4, 2, 2, 1}};
    assert(!memcmp(expect, fake->drawn, sizeof(expect)));
    memset(pixels, 0xdd, sizeof(pixels)); /* caller storage is already free */
    xt_spi_fake_fail(XT_SPI_DRAIN, 1, XIAOTAI_ERR_TIMEOUT);
    assert(p.stop(p.self) == XIAOTAI_ERR_TIMEOUT && xiaotai_mem_host_outstanding() == 1);
    assert(!fake->count[XT_SPI_PANEL_CLOSE] && p.start(p.self, NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(p.stop(p.self) == XIAOTAI_OK && xiaotai_mem_host_outstanding() == 0);
    assert(!memcmp(fake->last, (uint8_t[]){17, 18, 19, 20}, 4));
    assert(fake->log[fake->calls - 1] == XT_SPI_BUS_CLOSE);
    assert(p.start(p.self, NULL) == XIAOTAI_OK);
    xt_spi_fake_reset();
    xt_spi_fake_fail(XT_SPI_DRAW, 2, XIAOTAI_ERR_TIMEOUT);
    pixels[6] = 0x87;
    assert(p.present(p.self, &area, pixels) == XIAOTAI_ERR_TIMEOUT && fake->draws == 2);
    assert(fake->queued != pixels);
    pixels[6] = 0x27;
    assert(p.stop(p.self) == XIAOTAI_OK && fake->last[0] == 0x87);

    c.max_transfer_bytes = 12;
    c.wait_done = true;
    p = make(xiaotai_panel_st7789(), &c);
    assert(p.start(p.self, NULL) == XIAOTAI_OK);
    xt_spi_fake_reset();
    const xiaotai_rect_t narrow = {.width = 2, .height = 4};
    assert(p.present(p.self, &narrow, pixels) == XIAOTAI_OK);
    assert(fake->draws == 2 && fake->drawn[0].height == 3 && fake->drawn[1].height == 1);
    assert(p.stop(p.self) == XIAOTAI_OK);
}

static void check_rollback(void)
{
    const xt_spi_fake_op_t failures[] = {XT_SPI_BUS_OPEN, XT_SPI_IO_OPEN, XT_SPI_PANEL_OPEN,
        XT_SPI_RESET, XT_SPI_SETUP, XT_SPI_DRAW, XT_SPI_DRAIN, XT_SPI_ON};
    const xiaotai_panel_spi_config_t c = config();
    xiaotai_panel_t p = make(xiaotai_panel_st7789(), &c);
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        xt_spi_fake_reset();
        xiaotai_mem_host_clear();
        xt_spi_fake_fail(failures[i], 1, XIAOTAI_ERR_TIMEOUT);
        xiaotai_display_handles_t out = {.panel = &p, .mirror_x = true};
        assert(p.start(p.self, &out) == XIAOTAI_ERR_TIMEOUT && !out.panel && !out.mirror_x);
        assert(!fake->queued && xiaotai_mem_host_outstanding() == 0);
        assert(fake->count[XT_SPI_BUS_CLOSE] == (i > 0));
        if (i > 2) {
            assert(fake->log[fake->calls - 3] == XT_SPI_PANEL_CLOSE);
            assert(fake->log[fake->calls - 2] == XT_SPI_IO_CLOSE);
        }
        assert(p.start(p.self, NULL) == XIAOTAI_OK && p.stop(p.self) == XIAOTAI_OK);
    }

    line_t cs = {.fail_high = true}, rst = {0};
    xiaotai_panel_spi_config_t cfg = config();
    cfg.cs_pin = -1; cfg.cs_line = (xiaotai_line_t){.self = &cs, .set = set_line};
    cfg.reset = (xiaotai_line_t){.self = &rst, .set = set_line};
    p = make(xiaotai_panel_st7789(), &cfg);
    assert(p.start(p.self, NULL) == XIAOTAI_ERR_TIMEOUT && !cs.set && cs.changes == 2);
    rst.fail_low = true;
    assert(p.start(p.self, NULL) == XIAOTAI_ERR_TIMEOUT && !rst.set && rst.changes == 5);
    assert(p.start(p.self, NULL) == XIAOTAI_OK && cs.set);
    xt_spi_fake_fail(XT_SPI_IO_CLOSE, 1, XIAOTAI_ERR_TIMEOUT);
    assert(p.stop(p.self) == XIAOTAI_ERR_TIMEOUT && cs.set);
    assert(p.start(p.self, NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(p.stop(p.self) == XIAOTAI_OK && !cs.set);
}

static void check_ili_profile(void)
{
    xt_spi_fake_reset();
    static const uint8_t cmd_data[] = {0xff, 0x93, 0x42};
    static const xiaotai_panel_command_t commands[] = {{0xc8, cmd_data, 3, 0}, {0x11, NULL, 0, 120}};
    xiaotai_panel_spi_config_t c = config();
    c.init = commands; c.init_count = sizeof(commands) / sizeof(commands[0]); c.bgr = c.invert = true;
    line_t rst = {0};
    c.reset = (xiaotai_line_t){.self = &rst, .set = set_line};
    c.reset_hold_ms = 10; c.reset_settle_ms = 120; c.software_reset_after_line = true;
    xiaotai_panel_t p = make(xiaotai_panel_ili9341(), &c);
    for (uint32_t i = 1; i <= 2; ++i) {
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(i);
        assert(p.start(p.self, NULL) == XIAOTAI_ERR_NO_MEM && !xiaotai_mem_host_outstanding());
    }
    assert(p.start(p.self, NULL) == XIAOTAI_OK);
    assert(fake->config.init == commands && fake->config.bgr && fake->config.invert && !fake->config.swap_xy);
    assert(fake->config.init[1].delay_ms == 120 && fake->count[XT_SPI_RESET] >= 1);
    assert(p.stop(p.self) == XIAOTAI_OK);
    xiaotai_panel_t unsupported;
    assert(xiaotai_panel_spi_create(xiaotai_panel_st7789(), &c, &unsupported) == XIAOTAI_ERR_NOT_SUPPORTED);
    c.external_dma = true; c.wait_done = false;
    assert(xiaotai_panel_spi_create(xiaotai_panel_ili9341(), &c, &unsupported) == XIAOTAI_ERR_INVALID_ARG);
}

int main(void)
{
    fake = xt_spi_fake_state();
    check_lck_orientation();
    check_s3_panel_mapping();
    check_waveshare_direct();
    check_staged_and_tiling();
    check_rollback();
    check_ili_profile();
    return 0;
}
