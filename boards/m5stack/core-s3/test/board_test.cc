// SPDX-License-Identifier: MIT
// Replaces legacy: tests/native/cores3/test_cores3_pcm_port.py
// Replaces legacy: tests/test_cores3_board_display.py
// Replaces legacy: tests/regression/test_s3_user_button.py
// Replaces legacy: tests/test_board_camera_port.py
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "xiaotai/board.hpp"
#include "xiaotai/parts/audio.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai_board_data.hpp"
#include "xiaotai_bus_host.h"
extern "C" {
#include "audio_io_fake.h"
#include "camera_dvp_fake.h"
#include "spi_fake.h"
#include "spi_io.h"
#include "touch_io.h"
}
extern "C" xiaotai_err_t __real_xt_spi_bus_open(const xiaotai_panel_spi_config_t*, size_t);

namespace {
constexpr uint8_t kAw = 0x58, kAxp = 0x34;
struct Write { uint8_t addr, reg, value; };
constexpr Write kSetup[] = {
    {kAw, 0x02, 0x05}, {kAw, 0x03, 0x82}, {kAw, 0x04, 0x18}, {kAw, 0x05, 0x0c},
    {kAw, 0x11, 0x10}, {kAw, 0x12, 0xff}, {kAw, 0x13, 0xff},
    {kAxp, 0x92, 13}, {kAxp, 0x93, 28}, {kAxp, 0x95, 28}, {kAxp, 0x99, 23}, {kAxp, 0x90, 0xab},
    {kAxp, 0x27, 0x00}, {kAxp, 0x69, 0x11}, {kAxp, 0x10, 0x30}, {kAxp, 0x30, 0x0f},
    {kAw, 0x03, 0x80}, {kAw, 0x03, 0x82},
    {kAw, 0x03, 0x82}, {kAxp, 0x90, 0xab},
};

struct Delay { uint32_t ms; uint8_t aw, axp; };
std::vector<Delay> delays;
xiaotai_i2c_bus_t* audio_bus;
bool fail_touch_io;
bool fail_touch_io_close;
unsigned touch_io_closes;

uint8_t reg(uint8_t address, uint8_t address_reg)
{
    return xiaotai_bus_host_i2c_peek(0, address, address_reg);
}

void expect_writes(const Write* expected, size_t count, xiaotai_err_t error = XIAOTAI_OK, size_t failed = 0)
{
    xiaotai_bus_host_transfer_t log[128];
    const size_t n = xiaotai_bus_host_i2c_log(log, std::size(log));
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        if (log[i].op != XIAOTAI_BUS_HOST_WRITE) continue;
        assert(j < count && log[i].port == 0 && log[i].timeout_ms == 100 && log[i].size == 1);
        assert(log[i].address == expected[j].addr && log[i].reg == expected[j].reg);
        assert(log[i].data[0] == expected[j].value && log[i].result == (j + 1 == failed ? error : XIAOTAI_OK));
        ++j;
    }
    assert(j == count);
}

void expect_writes(std::initializer_list<Write> expected)
{
    expect_writes(expected.begin(), expected.size());
}

void init_fixture()
{
    for (uint8_t addr : {kAw, kAxp, static_cast<uint8_t>(XT_FT_ADDRESS)})
        assert(xiaotai_bus_host_i2c_attach(0, addr) == XIAOTAI_OK);
}

void expect_empty()
{
    assert(!xiaotai_hal_audio_input(0) && !xiaotai_hal_audio_output(0) && !xiaotai_hal_input(0));
    assert(!xiaotai_hal_camera(0) && !xiaotai_hal_display(0) && !xiaotai_hal_network(0));
    assert(xiaotai_hal_needs() == XIAOTAI_NEED_EXTERNAL_RAM);
}

void initialization_failure(int step)
{
    xiaotai_i2c_bus_t* other{};
    xiaotai_err_t expected = XIAOTAI_ERR_TIMEOUT;
    if (step == 0) {
        const xiaotai_i2c_config_t config{0, 12, 11, 400000, true, 100};
        assert(xiaotai_i2c_create(&config, &other) == XIAOTAI_OK);
        expected = XIAOTAI_ERR_INVALID_STATE;
    } else {
        const uint8_t addr = kSetup[step - 1].addr;
        unsigned nth = 0;
        for (int i = 0; i < step; ++i) if (kSetup[i].addr == addr) ++nth;
        xiaotai_bus_host_i2c_fail(0, addr, XIAOTAI_BUS_HOST_WRITE, nth, expected);
    }
    assert(xiaotai_hal_init() == expected);
    assert(xiaotai_hal_init() == expected);
    expect_empty();
    std::vector<Write> writes(kSetup, kSetup + step);
    if (step == 17 || step == 18) writes.push_back({kAw, 0x03, 0x82}); // Release a failed LCD reset.
    if (step == 19) writes.push_back({kAxp, 0x90, 0xab}); // DVP create still attempts a safe rail-off.
    expect_writes(writes.data(), writes.size(), expected, static_cast<size_t>(step));
    assert(delays.size() == (step <= 17 ? 0U : step == 18 ? 1U : 2U));
    assert(xt_spi_fake_state()->calls == 0);
}

void check_board()
{
    xiaotai_board_identity_t id{};
    xiaotai_hal_identity(&id);
    assert(!strcmp(id.model, "m5stack-cores3-lite") && !strcmp(id.revision, "Lite"));
    assert(!strcmp(id.profile_model, "M5Stack-CoreS3-Lite") && !strcmp(id.profile_chip, "ESP32-S3"));
    expect_writes(kSetup, std::size(kSetup));
    assert(!xiaotai_hal_audio_input(1) && !xiaotai_hal_audio_output(1) && !xiaotai_hal_input(1));
    assert(!xiaotai_hal_camera(1) && !xiaotai_hal_display(1) && !xiaotai_hal_network(1));
    assert(!xiaotai_hal_output(0) && !xiaotai_hal_measurement(0));
    assert(xiaotai_hal_needs() == XIAOTAI_BOARD_MANIFEST_NEEDS);
    assert(xiaotai_hal_network(0)->desc.kind == XIAOTAI_NETWORK_WIFI);
    assert(xiaotai_hal_network(0)->desc.provisioning == (XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL));
    xiaotai_i2c_config_t bus{};
    assert(xiaotai_bus_host_i2c_port(0, &bus));
    assert(bus.sda_pin == 12 && bus.scl_pin == 11 && bus.hz == 400000 && bus.pullup && bus.timeout_ms == 100);
    assert(!xiaotai_bus_host_i2c_port(1, &bus) && !xiaotai_bus_host_pin(0).configured);
    assert(delays.size() == 2 && delays[0].ms == 10 && delays[0].aw == 0x80 && delays[0].axp == 0xab);
    assert(delays[1].ms == 120 && delays[1].aw == 0x82 && xt_spi_fake_state()->calls == 0);
}

void check_display()
{
    const auto* display = xiaotai_hal_display(0);
    const auto& d = display->desc;
    assert(d.width == 320 && d.height == 240 && d.pixel == XIAOTAI_PIXEL_RGB565 && d.touch);
    assert(d.render == XIAOTAI_RENDER_PARTIAL && d.rotation == 0 && d.transfer_pixels == 3200 && d.buffer_align == 4);
    assert(d.flags == (XIAOTAI_DISPLAY_SWAP_BYTES | XIAOTAI_DISPLAY_DMA_BUFFER | XIAOTAI_DISPLAY_EVEN_AREAS));
    xiaotai_bus_host_i2c_log_clear();
    xiaotai_display_handles_t handles{};
    assert(display->open(display->self, &handles) == XIAOTAI_OK);
    assert(!handles.swap_xy && !handles.mirror_x && !handles.mirror_y);
    const auto* spi = xt_spi_fake_state();
    assert(spi->count[XT_SPI_RESET] == 1 && spi->count[XT_SPI_ON] == 1);
    assert(spi->pixels_painted == 320 * 240 && !spi->nonblack && !spi->queued);
    assert(spi->config.bgr && spi->config.invert && !spi->config.external_dma && spi->config.wait_done);
    assert(spi->config.init_count == 11);
    const auto* cmd = spi->config.init;
    constexpr uint8_t ids[]{0xc8, 0xc0, 0xc1, 0xc5, 0xb0, 0xf6, 0xe0, 0xe1, 0xb6, 0x38, 0x11};
    for (size_t i = 0; i < std::size(ids); ++i) assert(cmd[i].command == ids[i]);
    assert(cmd[0].size == 3 && cmd[0].data[0] == 0xff && cmd[0].data[1] == 0x93 && cmd[0].data[2] == 0x42);
    assert(cmd[6].size == 15 && cmd[7].size == 15 && cmd[10].delay_ms == 120);
    assert(delays.size() == 2);  // No duplicate external pulse at display open.
    expect_writes({{kAxp, 0x90, 0x2b}});
    const size_t calls = spi->calls;
    assert(display->open(display->self, nullptr) == XIAOTAI_OK && spi->calls == calls);
    assert(display->set_backlight(display->self, 17) == XIAOTAI_OK && reg(kAxp, 0x90) == 0xab);
    assert(display->set_backlight(display->self, 0) == XIAOTAI_OK && reg(kAxp, 0x90) == 0x2b);

    xiaotai_bus_host_i2c_poke(0, XT_FT_ADDRESS, XT_FT_POINT_REGISTER, 1);
    xiaotai_bus_host_i2c_poke(0, XT_FT_ADDRESS, XT_FT_POINT_REGISTER + 2, 117);
    xiaotai_bus_host_i2c_poke(0, XT_FT_ADDRESS, XT_FT_POINT_REGISTER + 4, 68);
    xiaotai_touch_point_t point{};
    size_t n = 0;
    assert(display->read_touch(display->self, &point, 1, &n) == XIAOTAI_OK && n == 1);
    assert(point.x == 117 && point.y == 68);
}

void display_failure(int step, bool failed_cleanup)
{
    const auto* d = xiaotai_hal_display(0);
    constexpr auto timeout = XIAOTAI_ERR_TIMEOUT;
    const xt_spi_fake_op_t spi_steps[]{XT_SPI_BUS_OPEN, XT_SPI_IO_OPEN, XT_SPI_PANEL_OPEN, XT_SPI_RESET,
                                      XT_SPI_SETUP, XT_SPI_DRAW, XT_SPI_DRAIN, XT_SPI_ON};
    if (step == 0) xiaotai_bus_host_i2c_fail(0, kAxp, XIAOTAI_BUS_HOST_WRITE, 1, timeout);
    else if (step <= 8) xt_spi_fake_fail(spi_steps[step - 1], 1, timeout);
    else if (step == 9) fail_touch_io = true;
    else xiaotai_bus_host_i2c_fail(0, XT_FT_ADDRESS, XIAOTAI_BUS_HOST_PROBE, 1, timeout);
    if (failed_cleanup) xt_spi_fake_fail(XT_SPI_PANEL_CLOSE, 1, XIAOTAI_FAIL);
    xiaotai_display_handles_t h{};
    h.mirror_x = true;
    assert(d->open(d->self, &h) == timeout && !h.panel && !h.panel_io && !h.touch && !h.mirror_x);
    assert(reg(kAxp, 0x90) == 0x2b && reg(kAw, 0x03) == 0x82);
    const auto* spi = xt_spi_fake_state();
    if (step == 0) assert(spi->calls == 0);
    if (step >= 2 && !failed_cleanup) assert(spi->count[XT_SPI_BUS_CLOSE] == 1);
    if (step >= 4 && !failed_cleanup) {
        assert(spi->log[spi->calls - 3] == XT_SPI_PANEL_CLOSE);
        assert(spi->log[spi->calls - 2] == XT_SPI_IO_CLOSE);
        assert(spi->log[spi->calls - 1] == XT_SPI_BUS_CLOSE);
    }
    const size_t calls = spi->calls;
    if (failed_cleanup) {
        xiaotai_display_part_stats_t state{};
        xiaotai_display_part_stats(d, &state);
        assert(state.latched && !state.open && state.last_error == timeout);
        assert(d->open(d->self, &h) == XIAOTAI_ERR_INVALID_STATE && spi->calls == calls);
    } else {
        assert(d->open(d->self, &h) == XIAOTAI_OK && spi->calls > calls);
    }
}

void display_other_owner()
{
    xiaotai_panel_spi_config_t other{};
    other.host = 2;
    assert(__real_xt_spi_bus_open(&other, 6400) == XIAOTAI_OK);
    const auto* d = xiaotai_hal_display(0);
    const auto* spi = xt_spi_fake_state();
    assert(d->open(d->self, nullptr) == XIAOTAI_ERR_INVALID_STATE);
    assert(spi->count[XT_SPI_BUS_CLOSE] == 0 && spi->count[XT_SPI_IO_OPEN] == 0);
    assert(xt_spi_bus_close(2) == XIAOTAI_OK);
    assert(d->open(d->self, nullptr) == XIAOTAI_OK);
}

void display_touch_cleanup_failure()
{
    const auto* d = xiaotai_hal_display(0);
    xiaotai_bus_host_i2c_fail(0, XT_FT_ADDRESS, XIAOTAI_BUS_HOST_PROBE, 1, XIAOTAI_ERR_TIMEOUT);
    fail_touch_io_close = true;
    assert(d->open(d->self, nullptr) == XIAOTAI_ERR_TIMEOUT && touch_io_closes == 1);
    const auto* spi = xt_spi_fake_state();
    assert(spi->count[XT_SPI_PANEL_CLOSE] == 1 && spi->count[XT_SPI_IO_CLOSE] == 1);
    assert(spi->count[XT_SPI_BUS_OPEN] == 1 && spi->count[XT_SPI_BUS_CLOSE] == 1);
    xiaotai_display_part_stats_t state{};
    xiaotai_display_part_stats(d, &state);
    assert(state.latched && !state.open && state.last_error == XIAOTAI_ERR_TIMEOUT);
    const size_t spi_calls = spi->calls;
    xiaotai_bus_host_i2c_log_clear();
    assert(d->open(d->self, nullptr) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_bus_host_transfer_t operation{};
    assert(spi->calls == spi_calls && touch_io_closes == 1 && !xiaotai_bus_host_i2c_log(&operation, 1));
}

void check_camera()
{
    const auto* cam = xiaotai_hal_camera(0);
    assert(cam->desc.formats == XIAOTAI_CAMERA_YUV422 && cam->desc.max_width == 240 && cam->desc.max_height == 176);
    assert(cam->desc.min_width == 160 && cam->desc.min_height == 120 && !cam->prepare);
    delays.clear();
    xiaotai_bus_host_i2c_log_clear();
    assert(cam->set_power(cam->self, true) == XIAOTAI_OK);
    expect_writes({{kAw, 0x03, 0x82}, {kAxp, 0x94, 28}, {kAxp, 0x90, 0x3f}, {kAw, 0x03, 0x83}});
    assert(delays.size() == 2 && delays[0].ms == 10 && delays[0].aw == 0x82 && delays[0].axp == 0x3f);
    assert(delays[1].ms == 10 && delays[1].aw == 0x83);
    xiaotai_camera_mode_t mode{};
    mode.format = XIAOTAI_CAMERA_YUV422;
    mode.width = 240;
    mode.height = 176;
    assert(cam->open(cam->self, &mode) == XIAOTAI_OK);
    const auto state = xt_dvp_fake_status();
    const auto& c = state.config;
    assert(c.sccb == audio_bus && c.sensor_id == 0x9b && state.size == XT_DVP_240_176);
    assert(c.xclk_pin == -1 && c.xclk_hz == 20000000 && c.xclk_timer == 1 && c.xclk_channel == 1);
    const int pins[]{39, 40, 41, 42, 15, 16, 48, 47};
    assert(!memcmp(pins, c.data_pins, sizeof(pins)) && c.vsync_pin == 46 && c.href_pin == 38 && c.pclk_pin == 45);
    assert(cam->set_power(cam->self, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(cam->close(cam->self) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_fail(0, kAw, XIAOTAI_BUS_HOST_WRITE, 1, XIAOTAI_ERR_TIMEOUT);
    assert(cam->set_power(cam->self, false) == XIAOTAI_ERR_TIMEOUT && reg(kAxp, 0x90) == 0x3f);
    assert(cam->set_power(cam->self, true) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_bus_host_i2c_log_clear();
    assert(cam->set_power(cam->self, false) == XIAOTAI_OK);
    expect_writes({{kAw, 0x03, 0x82}, {kAxp, 0x90, 0x2b}});
}

void check_camera_power_failure(unsigned step)
{
    const auto* cam = xiaotai_hal_camera(0);
    xiaotai_bus_host_i2c_poke(0, kAw, 0x03, 0xff);
    xiaotai_bus_host_i2c_poke(0, kAxp, 0x90, 0x82);
    const Write startup[]{{kAw, 0x03, 0xfe}, {kAxp, 0x94, 28}, {kAxp, 0x90, 0x96}, {kAw, 0x03, 0xff}};
    constexpr auto error = XIAOTAI_FAIL;
    if (step == std::size(startup)) {
        assert(cam->set_power(cam->self, true) == XIAOTAI_OK);
        delays.clear();
        xiaotai_bus_host_i2c_log_clear();
        xiaotai_bus_host_i2c_fail(0, kAxp, XIAOTAI_BUS_HOST_WRITE, 1, error);
        assert(cam->set_power(cam->self, false) == error && delays.empty());
        const Write shutdown[]{{kAw, 0x03, 0xfe}, {kAxp, 0x90, 0x82}};
        expect_writes(shutdown, std::size(shutdown), error, 2);
        assert(reg(kAw, 0x03) == 0xfe && reg(kAxp, 0x90) == 0x96);
        return;
    }

    assert(step < std::size(startup));
    delays.clear();
    xiaotai_bus_host_i2c_log_clear();
    unsigned nth = 0;
    for (unsigned i = 0; i <= step; ++i) if (startup[i].addr == startup[step].addr) ++nth;
    xiaotai_bus_host_i2c_fail(0, startup[step].addr, XIAOTAI_BUS_HOST_WRITE, nth, error);
    assert(cam->set_power(cam->self, true) == error);
    std::vector<Write> expected(startup, startup + step + 1);
    // New DVP protects a partially enabled camera, without continuing startup.
    if (step != 0) {
        expected.push_back({kAw, 0x03, 0xfe});
        expected.push_back({kAxp, 0x90, 0x82});
    }
    expect_writes(expected.data(), expected.size(), error, step + 1);
    assert(delays.size() == (step == 3 ? 1U : 0U));
    if (step == 3) assert(delays[0].ms == 10 && delays[0].aw == 0xfe && delays[0].axp == 0x96);
    assert(reg(kAxp, 0x90) == 0x82 && reg(kAw, 0x03) == (step == 0 ? 0xff : 0xfe));
}

void expect_audio(std::initializer_list<xt_audio_fake_call_t> expected)
{
    xt_audio_fake_record_t log[16];
    assert(xt_audio_fake_log(log, std::size(log)) == expected.size());
    size_t i = 0;
    for (auto call : expected) assert(log[i++].call == call);
}

void check_audio_latched(xt_audio_fake_call_t fault, xiaotai_err_t error,
                         std::initializer_list<xt_audio_fake_call_t> sequence)
{
    const auto* in = xiaotai_hal_audio_input(0);
    const auto* out = xiaotai_hal_audio_output(0);
    xt_audio_fake_clear();
    xt_audio_fake_fail(fault, error, false);
    assert(out->open(out->self) == error);
    expect_audio(sequence);
    xt_audio_fake_clear();
    assert(in->open(in->self) == XIAOTAI_ERR_INVALID_STATE);
    assert(out->open(out->self) == XIAOTAI_ERR_INVALID_STATE && out->flush_frames(out->self) == 0);
    expect_audio({});
    xiaotai_audio_device_stats_t diag{};
    xiaotai_audio_device_stats(in, &diag);
    assert(diag.latched && !diag.running && !diag.opens && diag.open_failures == 1 && diag.last_error == error);
}

void check_audio_mute(const xiaotai_audio_output_t* out)
{
    xt_audio_fake_clear();
    xt_audio_fake_fail(FAKE_MUTE, XIAOTAI_FAIL, false);
    assert(out->set_enabled(out->self, true) == XIAOTAI_FAIL);
    assert(out->set_muted(out->self, false) == XIAOTAI_OK);
    xt_audio_fake_fail(FAKE_MUTE, XIAOTAI_FAIL, false);
    assert(out->set_muted(out->self, true) == XIAOTAI_FAIL);
    assert(out->set_enabled(out->self, true) == XIAOTAI_OK);
    assert(out->set_muted(out->self, true) == XIAOTAI_OK);
    assert(out->set_enabled(out->self, true) == XIAOTAI_OK);
    assert(out->set_muted(out->self, false) == XIAOTAI_OK);
    assert(out->set_enabled(out->self, false) == XIAOTAI_OK);
    // A failed gate change cannot expose sound, and a failed user mute cannot stick.
    const int values[]{0, 1, 1, 0, 1, 1, 0, 1};
    xt_audio_fake_record_t log[std::size(values) + 1];
    assert(xt_audio_fake_log(log, std::size(log)) == std::size(values));
    for (size_t i = 0; i < std::size(values); ++i) assert(log[i].call == FAKE_MUTE && log[i].value == values[i]);
}

void check_audio()
{
    const auto* in = xiaotai_hal_audio_input(0);
    const auto* out = xiaotai_hal_audio_output(0);
    assert(in->desc.encoding == XIAOTAI_CODEC_PCM16 && in->desc.rate_hz == 16000 && in->desc.channels == 4);
    const xiaotai_channel_role_t roles[]{XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE,
                                        XIAOTAI_CHANNEL_MONITOR, XIAOTAI_CHANNEL_UNUSED};
    for (size_t i = 0; i < std::size(roles); ++i) assert(in->desc.roles[i] == roles[i]);
    assert(in->info.group == 1 && in->info.gain_db == 24 && in->info.block_frames == 256);
    assert(in->info.read_timeout_ms == 100 && !in->set_gain_db);
    assert(out->desc.encoding == XIAOTAI_CODEC_PCM16 && out->desc.rate_hz == 16000 && out->desc.channels == 2);
    assert(out->desc.roles[0] == XIAOTAI_CHANNEL_LEFT && out->desc.roles[1] == XIAOTAI_CHANNEL_RIGHT);
    assert(out->info.group == 1 && out->info.latency_ms == 96 && out->info.enable_ms == 0);
    assert(out->info.max_write_frames == 0 && out->info.flags & XIAOTAI_AUDIO_OUTPUT_PACED);
    int16_t pcm[7 * 4]{}, playback[7 * 2]{};
    size_t read = 1;
    assert(out->flush_frames(out->self) == 0);
    assert(in->read(in->self, pcm, sizeof(pcm), &read) == XIAOTAI_ERR_INVALID_STATE && read == 0);
    assert(out->write(out->self, playback, sizeof(playback)) == XIAOTAI_ERR_INVALID_STATE);
    assert(out->set_enabled(out->self, true) == XIAOTAI_ERR_INVALID_STATE);
    xt_audio_fake_clear();
    assert(out->open(out->self) == XIAOTAI_OK);
    expect_audio({FAKE_START, FAKE_OPEN_OUTPUT, FAKE_OPEN_INPUT, FAKE_CHECK_CLOCK, FAKE_CHECK_CLOCK,
                  FAKE_GAIN, FAKE_GAIN, FAKE_GAIN, FAKE_VOLUME});
    assert(out->flush_frames(out->self) == 1792);
    xt_audio_fake_clear();
    assert(in->open(in->self) == XIAOTAI_OK && in->open(in->self) == XIAOTAI_OK);
    assert(out->open(out->self) == XIAOTAI_OK);
    expect_audio({});
    assert(in->read(in->self, nullptr, sizeof(pcm), &read) == XIAOTAI_ERR_INVALID_ARG && read == 0);
    assert(in->read(in->self, pcm, SIZE_MAX, &read) == XIAOTAI_ERR_INVALID_ARG && read == 0);
    assert(out->write(out->self, playback, 0) == XIAOTAI_ERR_INVALID_ARG);
    assert(out->write(out->self, playback, SIZE_MAX) == XIAOTAI_ERR_INVALID_ARG);
    assert(out->set_volume(out->self, 101) == XIAOTAI_ERR_INVALID_ARG);
    expect_audio({});
    assert(in->read(in->self, pcm, sizeof(pcm), &read) == XIAOTAI_ERR_TIMEOUT && read == 0);
    assert(out->write(out->self, playback, sizeof(playback)) == XIAOTAI_OK);
    int16_t sink[7 * 2]{};
    assert(xt_audio_fake_last_write(sink, sizeof(sink)) == sizeof(playback));
    xt_audio_fake_fail(FAKE_WRITE, XIAOTAI_FAIL, false);
    assert(out->write(out->self, playback, sizeof(playback)) == XIAOTAI_FAIL);
    assert(out->set_volume(out->self, 70) == XIAOTAI_OK);
    xt_audio_fake_record_t log[4];
    assert(xt_audio_fake_log(log, std::size(log)) == 3);
    assert(log[0].call == FAKE_WRITE && log[0].value == sizeof(playback));
    // The board plays at most 70 % of the codec range: 70 % asked is 49 % played.
    assert(log[2].call == FAKE_VOLUME && log[2].value == 49);
    assert(out->info.volume_span_cdb == 4950 * 70 / 100);
    check_audio_mute(out);

    int16_t dma[256 * 4]{};
    for (size_t i = 0; i < std::size(pcm); ++i) dma[i] = (int16_t)(i + 12);
    static_assert(sizeof(pcm) == 56 && sizeof(playback) == 28);
    for (int i = 0; i < 16; ++i) xt_audio_fake_receive(0, dma, sizeof(dma));
    int16_t dropped[256 * 4]{};
    dropped[0] = -42;
    xt_audio_fake_receive(0, dropped, sizeof(dropped));
    xiaotai_audio_stats_t stats{};
    in->stats(in->self, &stats);
    assert(stats.overruns == 1 && stats.lost_frames == 256);
    assert(out->close(out->self) == XIAOTAI_OK);
    assert(in->read(in->self, pcm, sizeof(pcm), &read) == XIAOTAI_OK && read == sizeof(pcm));
    assert(!memcmp(pcm, dma, sizeof(pcm))); // Playback is idle; capture keeps all four TDM slots.
    xiaotai_audio_device_stats_t diag{};
    xiaotai_audio_device_stats(in, &diag);
    assert(diag.read_timeouts == 1 && diag.ring_overflows == 1 && diag.write_errors == 1 && diag.running);
    assert(in->close(in->self) == XIAOTAI_OK);
}

void check_key()
{
    const auto* key = xiaotai_hal_input_by_role("main");
    assert(key == xiaotai_hal_input(0) && key->desc.kind == XIAOTAI_INPUT_BUTTON);
    assert(key->desc.events == XIAOTAI_INPUT_CLICK);
    assert(key->open(key->self) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_poke(0, kAxp, 0x49, 0x08);
    for (int i = 0; i < 4; ++i) assert(key->poll(key->self) == 0);
    assert(key->poll(key->self) == XIAOTAI_INPUT_CLICK);
}
}  // namespace

extern "C" bool __wrap_xiaotai_mem_has_external() { return true; }

extern "C" void __wrap_xiaotai_delay_ms(uint32_t ms)
{
    delays.push_back({ms, reg(kAw, 0x03), reg(kAxp, 0x90)});
}

extern "C" xiaotai_err_t __wrap_xt_spi_bus_open(const xiaotai_panel_spi_config_t* c, size_t bytes)
{
    assert(reg(kAxp, 0x90) == 0x2b);
    assert(c->host == 2 && c->mosi_pin == 37 && c->sclk_pin == 36 && c->dc_pin == 35 && c->cs_pin == 3);
    assert(c->hz == 40000000 && c->spi_mode == 0 && c->queue_depth == 4 && bytes == 6400);
    return __real_xt_spi_bus_open(c, bytes);
}

extern "C" xiaotai_err_t __real_xt_ft_io_open(const xiaotai_touch_i2c_config_t*, void**);
extern "C" xiaotai_err_t __wrap_xt_ft_io_open(const xiaotai_touch_i2c_config_t* c, void** io)
{
    assert(c->bus == audio_bus && c->address == XT_FT_ADDRESS && c->hz == 400000);
    assert(c->width == 320 && c->height == 240 && c->reset_pin == -1 && c->interrupt_pin == -1);
    assert(!c->swap_xy && !c->mirror_x && !c->mirror_y);
    if (fail_touch_io) {
        fail_touch_io = false;
        return XIAOTAI_ERR_TIMEOUT;
    }
    return __real_xt_ft_io_open(c, io);
}

extern "C" xiaotai_err_t __real_xt_ft_io_close(void*);
extern "C" xiaotai_err_t __wrap_xt_ft_io_close(void* io)
{
    ++touch_io_closes;
    return fail_touch_io_close ? XIAOTAI_FAIL : __real_xt_ft_io_close(io);
}

extern "C" xiaotai_err_t __real_xiaotai_es7210_create(const xiaotai_es7210_config_t*, xiaotai_audio_codec_t**);
extern "C" xiaotai_err_t __wrap_xiaotai_es7210_create(const xiaotai_es7210_config_t* c, xiaotai_audio_codec_t** out)
{
    assert(c->bus && c->address == 0 && c->mclk_multiple == 256 && c->inputs == 0x07);
    audio_bus = c->bus;
    return __real_xiaotai_es7210_create(c, out);
}

extern "C" xiaotai_err_t __real_xiaotai_aw88298_create(const xiaotai_aw88298_config_t*, xiaotai_audio_codec_t**);
extern "C" xiaotai_err_t __wrap_xiaotai_aw88298_create(const xiaotai_aw88298_config_t* c, xiaotai_audio_codec_t** out)
{
    assert(c->bus == audio_bus && c->address == 0 && c->bit_clocks_per_frame == 64 && !c->reset.set);
    return __real_xiaotai_aw88298_create(c, out);
}

extern "C" xiaotai_err_t __real_xiaotai_audio_device_create(const xiaotai_audio_device_config_t*,
                                                          const xiaotai_audio_input_t**, const xiaotai_audio_output_t**);
extern "C" xiaotai_err_t __wrap_xiaotai_audio_device_create(const xiaotai_audio_device_config_t* config,
                                                          const xiaotai_audio_input_t** in, const xiaotai_audio_output_t** out)
{
    const auto& i = config->i2s;
    assert(i.port == 0 && i.mclk_pin == 0 && i.bclk_pin == 34 && i.ws_pin == 33 && i.din_pin == 14 && i.dout_pin == 13);
    assert(i.rx_slots == 4 && i.tx_slots == 2 && i.dma_descriptors == 6 && i.dma_frames == 256 && i.mclk_multiple == 256);
    assert(config->capture_mode == XIAOTAI_AUDIO_CAPTURE_RING && config->ring_blocks == 16 && config->mic_inputs == 3);
    assert(config->input_gain_db[0] == 24 && config->input_gain_db[1] == 24 && config->input_gain_db[2] == 0);
    return __real_xiaotai_audio_device_create(config, in, out);
}

int main(int argc, char** argv)
{
    expect_empty();
    init_fixture();
    if (argc == 3 && !strcmp(argv[1], "init")) {
        initialization_failure(atoi(argv[2]));
        return 0;
    }
    assert(xiaotai_hal_init() == XIAOTAI_OK && xiaotai_hal_init() == XIAOTAI_OK);
    check_board();
    if (argc == 3 && !strcmp(argv[1], "display")) display_failure(atoi(argv[2]), false);
    else if (argc == 3 && !strcmp(argv[1], "camera-power")) check_camera_power_failure(atoi(argv[2]));
    else if (argc == 2 && !strcmp(argv[1], "display-latched")) display_failure(10, true);
    else if (argc == 2 && !strcmp(argv[1], "display-owned")) display_other_owner();
    else if (argc == 2 && !strcmp(argv[1], "display-touch-cleanup")) display_touch_cleanup_failure();
    else if (argc == 2 && !strcmp(argv[1], "audio-latched"))
        check_audio_latched(FAKE_CHECK_CLOCK, XIAOTAI_FAIL,
            {FAKE_START, FAKE_OPEN_OUTPUT, FAKE_OPEN_INPUT, FAKE_CHECK_CLOCK,
             FAKE_CLOSE_INPUT, FAKE_CLOSE_OUTPUT, FAKE_STOP});
    else if (argc == 2 && !strcmp(argv[1], "audio-dma-failure"))
        check_audio_latched(FAKE_START, XIAOTAI_ERR_TIMEOUT, {FAKE_START});
    else if (argc == 2 && !strcmp(argv[1], "audio-codec-failure"))
        check_audio_latched(FAKE_OPEN_INPUT, XIAOTAI_ERR_NO_MEM,
            {FAKE_START, FAKE_OPEN_OUTPUT, FAKE_OPEN_INPUT, FAKE_CLOSE_OUTPUT, FAKE_STOP});
    else {
        assert(argc == 1);
        check_display();  // Starts before either audio direction on the shared I2C bus.
        check_camera();
        check_audio();
        check_key();
    }
}
