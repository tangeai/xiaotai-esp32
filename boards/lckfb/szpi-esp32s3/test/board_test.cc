// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_s3_user_button.py
// Replaces legacy: tests/test_board_camera_port.py
// Replaces legacy: tests/test_board_rollback.py
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "xiaotai/hal.h"
#include "xiaotai/needs.h"
#include "xiaotai/parts/audio.h"
#include "xiaotai/parts/button.h"
#include "xiaotai/parts/camera_dvp.h"
#include "xiaotai/parts/panel_spi.h"
#include "xiaotai/parts/power.h"
#include "xiaotai/parts/touch_ft5x06.h"
#include "xiaotai/port/os.h"
#include "xiaotai_board_data.hpp"
#include "xiaotai_bus_host.h"

extern "C" {
#include "audio_io_fake.h"
#include "camera_dvp_fake.h"
#include "spi_fake.h"
}

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #expr); std::abort(); } \
} while (0)

static int step, fail;
static constexpr auto kFailure = XIAOTAI_ERR_TIMEOUT;
static xiaotai_i2c_bus_t* board_bus;

static bool inject(int expected)
{
    CHECK(++step == expected);
    CHECK(!xiaotai_hal_audio_input(0) && !xiaotai_hal_display(0) && !xiaotai_hal_network(0));
    return step == fail;
}

static uint8_t pca_output() { return xiaotai_bus_host_i2c_peek(0, 0x19, 1); }

extern "C" {
// The Linux port has no external RAM. Model this board's detected PSRAM.
bool __wrap_xiaotai_mem_has_external(void) { return true; }

xiaotai_err_t __real_xiaotai_i2c_create(const xiaotai_i2c_config_t*, xiaotai_i2c_bus_t**);
xiaotai_err_t __wrap_xiaotai_i2c_create(const xiaotai_i2c_config_t* c, xiaotai_i2c_bus_t** out)
{
    CHECK(c->port == 0 && c->sda_pin == 1 && c->scl_pin == 2 && c->hz == 100000 && c->pullup);
    if (inject(1)) return kFailure;
    auto result = __real_xiaotai_i2c_create(c, out);
    board_bus = *out;
    return result;
}

xiaotai_err_t __real_xiaotai_pca9557_create(const xiaotai_pca9557_config_t*, xiaotai_pca9557_t**);
xiaotai_err_t __wrap_xiaotai_pca9557_create(const xiaotai_pca9557_config_t* c, xiaotai_pca9557_t** out)
{
    CHECK(c->bus == board_bus && c->address == 0x19 && c->outputs == 7 && c->initial == 5 && c->lock_timeout_ms == 500);
    if (inject(2)) return kFailure;
    return __real_xiaotai_pca9557_create(c, out);
}

xiaotai_err_t __real_xiaotai_pca9557_line(xiaotai_pca9557_t*, uint8_t, bool, xiaotai_line_t*);
xiaotai_err_t __wrap_xiaotai_pca9557_line(xiaotai_pca9557_t* chip, uint8_t pin, bool low, xiaotai_line_t* out)
{
    CHECK(pin < 3 && low == (pin != 1));
    return inject(3 + pin) ? kFailure : __real_xiaotai_pca9557_line(chip, pin, low, out);
}

xiaotai_err_t __real_xiaotai_level_pwm(const xiaotai_pwm_config_t*, xiaotai_level_t*);
xiaotai_err_t __wrap_xiaotai_level_pwm(const xiaotai_pwm_config_t* c, xiaotai_level_t* out)
{
    // The camera's clock owns timer and channel 1.
    CHECK(c->pin == 42 && c->active_low && c->timer == 0 && c->channel == 0 && c->hz == 5000);
    return inject(6) ? kFailure : __real_xiaotai_level_pwm(c, out);
}

xiaotai_err_t __real_xiaotai_line_gpio_input(const xiaotai_gpio_config_t*, xiaotai_line_t*);
xiaotai_err_t __wrap_xiaotai_line_gpio_input(const xiaotai_gpio_config_t* c, xiaotai_line_t* out)
{
    CHECK(c->pin == 0 && c->active_low && c->pull_up && !c->pull_down);
    return inject(7) ? kFailure : __real_xiaotai_line_gpio_input(c, out);
}

xiaotai_err_t __real_xiaotai_es7210_create(const xiaotai_es7210_config_t*, xiaotai_audio_codec_t**);
xiaotai_err_t __wrap_xiaotai_es7210_create(const xiaotai_es7210_config_t* c, xiaotai_audio_codec_t** out)
{
    CHECK(c->bus == board_bus && c->address == 0x41 && c->mclk_multiple == 256 && c->inputs == 0x0f);
    return inject(8) ? kFailure : __real_xiaotai_es7210_create(c, out);
}

xiaotai_err_t __real_xiaotai_es8311_create(const xiaotai_es8311_config_t*, xiaotai_audio_codec_t**);
xiaotai_err_t __wrap_xiaotai_es8311_create(const xiaotai_es8311_config_t* c, xiaotai_audio_codec_t** out)
{
    CHECK(c->bus == board_bus && c->address == 0 && c->mclk_multiple == 256 && c->i2c_hz == 100000);
    CHECK(!c->capture && c->playback && !c->reference);
    return inject(9) ? kFailure : __real_xiaotai_es8311_create(c, out);
}

xiaotai_err_t __real_xiaotai_audio_device_create(const xiaotai_audio_device_config_t*,
                                               const xiaotai_audio_input_t**, const xiaotai_audio_output_t**);
xiaotai_err_t __wrap_xiaotai_audio_device_create(const xiaotai_audio_device_config_t* c,
                                               const xiaotai_audio_input_t** mic, const xiaotai_audio_output_t** speaker)
{
    const auto& i = c->i2s;
    CHECK(i.port == 0 && i.mclk_pin == 38 && i.bclk_pin == 14 && i.ws_pin == 13 && i.din_pin == 12 && i.dout_pin == 45);
    CHECK(i.rate_hz == 16000 && i.mclk_multiple == 256 && i.rx_slots == 4 && i.tx_slots == 2);
    CHECK(!i.dma_descriptors && !i.dma_frames && i.rx_mode == XIAOTAI_I2S_SLOTS && i.tx_mode == XIAOTAI_I2S_SLOTS);
    CHECK(c->capture_codec && c->playback_codec && c->capture_codec != c->playback_codec);
    CHECK(c->input_gain_db[0] == 24 && c->input_gain_db[1] == 24 && c->input_gain_db[2] == 0 && c->input_gain_db[3] == 0);
    CHECK(c->mic_inputs == 3 && c->capture_mode == XIAOTAI_AUDIO_CAPTURE_BLOCKING && !c->verify_at_create);
    return inject(10) ? kFailure : __real_xiaotai_audio_device_create(c, mic, speaker);
}

xiaotai_err_t __real_xiaotai_panel_spi_create(const xiaotai_panel_spi_controller_t*, const xiaotai_panel_spi_config_t*,
                                             xiaotai_panel_t*);
xiaotai_err_t __wrap_xiaotai_panel_spi_create(const xiaotai_panel_spi_controller_t* controller,
                                             const xiaotai_panel_spi_config_t* c, xiaotai_panel_t* out)
{
    CHECK(controller == xiaotai_panel_st7789());
    CHECK(c->host == 2 && c->mosi_pin == 40 && c->sclk_pin == 41 && c->dc_pin == 39 && c->cs_pin == -1);
    CHECK(c->hz == 80000000 && c->spi_mode == 2 && c->queue_depth == 4 && c->max_transfer_bytes == 6400);
    CHECK(c->native_width == 240 && c->native_height == 320 && c->invert && c->swap_xy && c->mirror_x);
    CHECK(!c->mirror_y && !c->bgr && !c->external_dma && !xiaotai_line_connected(&c->reset));
    return inject(11) ? kFailure : __real_xiaotai_panel_spi_create(controller, c, out);
}

xiaotai_err_t __real_xiaotai_touch_ft5x06_create(const xiaotai_touch_i2c_config_t*, xiaotai_touch_t*);
xiaotai_err_t __wrap_xiaotai_touch_ft5x06_create(const xiaotai_touch_i2c_config_t* c, xiaotai_touch_t* out)
{
    CHECK(c->bus == board_bus && c->address == 0 && c->hz == 100000 && c->width == 240 && c->height == 320);
    CHECK(c->swap_xy && c->mirror_x && !c->mirror_y && c->reset_pin == -1 && c->interrupt_pin == -1);
    return inject(12) ? kFailure : __real_xiaotai_touch_ft5x06_create(c, out);
}

xiaotai_err_t __real_xiaotai_display_create(const xiaotai_display_config_t*, const xiaotai_display_t**);
xiaotai_err_t __wrap_xiaotai_display_create(const xiaotai_display_config_t* c, const xiaotai_display_t** out)
{
    CHECK(!c->touch_first && !c->power_steps && c->backlight.set && c->backlight.dimmable);
    return inject(13) ? kFailure : __real_xiaotai_display_create(c, out);
}

xiaotai_err_t __real_xiaotai_button_create(const xiaotai_button_config_t*, const xiaotai_input_t**);
xiaotai_err_t __wrap_xiaotai_button_create(const xiaotai_button_config_t* c, const xiaotai_input_t** out)
{
    CHECK(c->line.get && !c->contact && c->debounce_ms == 60 && std::strcmp(c->role, "main") == 0);
    return inject(14) ? kFailure : __real_xiaotai_button_create(c, out);
}

xiaotai_err_t __real_xiaotai_camera_dvp_create(const xiaotai_camera_dvp_config_t*, const xiaotai_camera_t**);
xiaotai_err_t __wrap_xiaotai_camera_dvp_create(const xiaotai_camera_dvp_config_t* c, const xiaotai_camera_t** out)
{
    const int pins[] = {16, 18, 8, 17, 15, 6, 4, 9};
    CHECK(c->sccb == board_bus && c->xclk_pin == 5 && c->xclk_hz == 20000000 && c->xclk_timer == 1 && c->xclk_channel == 1);
    CHECK(std::memcmp(c->data_pins, pins, sizeof(pins)) == 0 && c->vsync_pin == 3 && c->href_pin == 46 && c->pclk_pin == 7);
    CHECK(c->sensor_id == 0x2145 && c->power_settle_ms == 10 && c->power.set && !xiaotai_line_connected(&c->reset));
    return inject(15) ? kFailure : __real_xiaotai_camera_dvp_create(c, out);
}
}

static void verify_pcm_transfer(const xiaotai_audio_input_t* mic, const xiaotai_audio_output_t* speaker)
{
    int16_t samples[4 * 7]{};
    constexpr size_t playback_bytes = 2 * 7 * sizeof(samples[0]);
    size_t size = 0;
    xt_audio_fake_record_t calls[4]{};
    xt_audio_fake_clear();
    CHECK(mic->read(mic->self, nullptr, sizeof(samples), &size) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(mic->read(mic->self, samples, SIZE_MAX, &size) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(speaker->write(speaker->self, samples, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(speaker->write(speaker->self, samples, SIZE_MAX) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(speaker->set_volume(speaker->self, 101) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xt_audio_fake_log(calls, 4) == 0);

    CHECK(mic->read(mic->self, samples, sizeof(samples), &size) == XIAOTAI_OK && size == sizeof(samples));
    CHECK(speaker->write(speaker->self, samples, playback_bytes) == XIAOTAI_OK);
    xt_audio_fake_fail(FAKE_READ, XIAOTAI_FAIL, false);
    xt_audio_fake_fail(FAKE_WRITE, XIAOTAI_FAIL, false);
    CHECK(mic->read(mic->self, samples, sizeof(samples), &size) == XIAOTAI_FAIL && size == 0);
    CHECK(speaker->write(speaker->self, samples, playback_bytes) == XIAOTAI_FAIL);
    CHECK(xt_audio_fake_log(calls, 4) == 4);
    for (size_t i = 0; i < 4; i += 2) {
        CHECK(calls[i].call == FAKE_READ && calls[i].value == sizeof(samples));
        CHECK(calls[i + 1].call == FAKE_WRITE && calls[i + 1].value == playback_bytes);
    }
}

static void verify_audio(const xiaotai_audio_input_t* mic, const xiaotai_audio_output_t* speaker)
{
    xt_audio_fake_clear();
    int16_t unopened[4 * 7]{};
    size_t unopened_size = 0;
    CHECK(mic->read(mic->self, unopened, sizeof(unopened), &unopened_size) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(speaker->write(speaker->self, unopened, 28) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(speaker->set_enabled(speaker->self, true) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(speaker->flush_frames(speaker->self) == 0);
    xt_audio_fake_fail(FAKE_OPEN_INPUT, kFailure, false);
    CHECK(mic->open(mic->self) == kFailure && pca_output() == 5);
    xt_audio_fake_record_t calls[40]{};
    const size_t n = xt_audio_fake_log(calls, 40);
    CHECK(n >= 5 && calls[0].call == FAKE_START && calls[1].call == FAKE_OPEN_OUTPUT && calls[2].call == FAKE_OPEN_INPUT);
    CHECK(calls[n - 2].call == FAKE_CLOSE_OUTPUT && calls[n - 1].call == FAKE_STOP);
    CHECK(mic->open(mic->self) == XIAOTAI_OK && speaker->open(speaker->self) == XIAOTAI_OK);
    CHECK(speaker->flush_frames(speaker->self) == 1680);
    verify_pcm_transfer(mic, speaker);
    xiaotai_bus_host_i2c_fail(0, 0x19, XIAOTAI_BUS_HOST_WRITE, 1, kFailure);
    CHECK(speaker->set_enabled(speaker->self, true) == kFailure && pca_output() == 5);
    const auto before = xiaotai_time_us();
    CHECK(speaker->set_enabled(speaker->self, true) == XIAOTAI_OK && pca_output() == 7);
    CHECK(xiaotai_time_us() - before >= 119000);
    CHECK(speaker->close(speaker->self) == XIAOTAI_OK && pca_output() == 5);
    int16_t samples[4 * 240]{};
    size_t size = 0;
    CHECK(mic->read(mic->self, samples, sizeof(samples), &size) == XIAOTAI_OK && size == sizeof(samples));
    CHECK(mic->close(mic->self) == XIAOTAI_OK && speaker->flush_frames(speaker->self) == 0);
}

static void verify_display(const xiaotai_display_t* display)
{
    // A touch probe failure after panel start must release the whole SPI graph.
    xt_spi_fake_reset();
    xiaotai_bus_host_i2c_fail(0, 0x38, XIAOTAI_BUS_HOST_PROBE, 1, kFailure);
    xiaotai_display_handles_t handles{};
    CHECK(display->open(display->self, &handles) == kFailure && pca_output() == 5);
    CHECK(xiaotai_bus_host_pwm(0).duty == 0 && !handles.panel && !handles.touch);
    const auto* spi = xt_spi_fake_state();
    CHECK(spi->calls >= 3 && spi->log[spi->calls - 3] == XT_SPI_PANEL_CLOSE);
    CHECK(spi->log[spi->calls - 2] == XT_SPI_IO_CLOSE && spi->log[spi->calls - 1] == XT_SPI_BUS_CLOSE);
    CHECK(display->open(display->self, &handles) == XIAOTAI_OK && pca_output() == 4);
    CHECK(handles.swap_xy && handles.mirror_x && !handles.mirror_y && spi->limit == 6400);
    CHECK(display->set_backlight(display->self, 50) == XIAOTAI_OK && xiaotai_bus_host_pwm(0).duty == 511);
    CHECK(display->set_backlight(display->self, 100) == XIAOTAI_OK && xiaotai_bus_host_pwm(0).duty == 1023);
    CHECK(display->set_backlight(display->self, 0) == XIAOTAI_OK && xiaotai_bus_host_pwm(0).duty == 0);
    xiaotai_bus_host_i2c_poke(0, 0x38, 2, 1);
    xiaotai_bus_host_i2c_poke(0, 0x38, 3, 0x80);
    xiaotai_bus_host_i2c_poke(0, 0x38, 4, 18);
    xiaotai_bus_host_i2c_poke(0, 0x38, 5, 0x31);
    xiaotai_bus_host_i2c_poke(0, 0x38, 6, 44);
    size_t count = 0;
    xiaotai_touch_point_t point{};
    CHECK(display->read_touch(display->self, &point, 1, &count) == XIAOTAI_OK && count == 1);
    CHECK(point.x == 300 && point.y == 222);
}

static void verify_cleanup_failure(const xiaotai_display_t* display)
{
    xt_spi_fake_reset();
    xiaotai_bus_host_i2c_fail(0, 0x38, XIAOTAI_BUS_HOST_PROBE, 1, kFailure);
    xt_spi_fake_fail(XT_SPI_PANEL_CLOSE, 1, XIAOTAI_FAIL);
    CHECK(display->open(display->self, nullptr) == kFailure);
    CHECK(xiaotai_bus_host_pwm(0).duty == 0 && (pca_output() & 4) != 0);
    const auto* spi = xt_spi_fake_state();
    CHECK(spi->count[XT_SPI_PANEL_CLOSE] == 1 && spi->count[XT_SPI_IO_CLOSE] == 0);
    CHECK(display->open(display->self, nullptr) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(spi->count[XT_SPI_BUS_OPEN] == 1);
}

static void verify_camera(const xiaotai_camera_t* camera)
{
    xiaotai_bus_host_i2c_fail(0, 0x19, XIAOTAI_BUS_HOST_WRITE, 1, kFailure);
    CHECK(camera->set_power(camera->self, true) == kFailure && pca_output() == 5);
    const auto before = xiaotai_time_us();
    CHECK(camera->set_power(camera->self, true) == XIAOTAI_OK && pca_output() == 1);
    CHECK(xiaotai_time_us() - before >= 9000);
    const xiaotai_camera_mode_t mode{320, 240, XIAOTAI_CAMERA_YUV422, 0};
    xt_dvp_fake_script_t failure{};
    failure.open_error = kFailure;
    failure.partial_open = true;
    xt_dvp_fake_script(&failure);
    CHECK(camera->open(camera->self, &mode) == kFailure);
    CHECK(camera->set_power(camera->self, false) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(camera->close(camera->self) == XIAOTAI_OK);
    CHECK(camera->set_power(camera->self, false) == XIAOTAI_OK && pca_output() == 5);
    CHECK(camera->set_power(camera->self, true) == XIAOTAI_OK);
    xt_dvp_fake_script_t probe{};
    const uint16_t supported[] = {0x2145, 0x009b};
    for (uint16_t pid : supported) {
        probe.sensor_id = pid;
        xt_dvp_fake_script(&probe);
        CHECK(camera->open(camera->self, &mode) == XIAOTAI_OK);
        xt_dvp_fake_frame(0x55, 320U * 240U * 2U, 320, 240, XIAOTAI_CAMERA_YUV422, 1000);
        xiaotai_camera_frame_t frame{};
        CHECK(camera->borrow(camera->self, &frame) == XIAOTAI_OK && frame.data[0] == 0x55);
        CHECK(camera->release(camera->self, &frame) == XIAOTAI_OK && camera->close(camera->self) == XIAOTAI_OK);
    }
    probe.sensor_id = 0x1234;
    xt_dvp_fake_script(&probe);
    CHECK(camera->open(camera->self, &mode) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(camera->close(camera->self) == XIAOTAI_OK);
    xt_dvp_fake_script(nullptr);
    xiaotai_bus_host_i2c_fail(0, 0x19, XIAOTAI_BUS_HOST_WRITE, 1, kFailure);
    CHECK(camera->set_power(camera->self, false) == kFailure && pca_output() == 1);
    CHECK(camera->set_power(camera->self, false) == XIAOTAI_OK && pca_output() == 5);
}

static void verify_button(const xiaotai_input_t* key)
{
    xiaotai_bus_host_pin_drive(0, false);
    CHECK(key->open(key->self) == XIAOTAI_OK);
    // Same physical waveform as test_s3_user_button.py; extend the last hold.
    for (unsigned tick = 1; tick < 260; ++tick) {
        const bool pressed = tick < 12 || tick == 22 || (tick >= 26 && tick < 47) || tick == 48 ||
                             (tick >= 60 && tick < 72) || (tick >= 84 && tick < 240);
        xiaotai_bus_host_pin_drive(0, !pressed);
        const uint32_t events = key->poll(key->self);
        if (tick == 32 || tick == 66 || tick == 90) CHECK(events == XIAOTAI_INPUT_PRESSED);
        else if (tick == 55 || tick == 78 || tick == 246) CHECK(events == XIAOTAI_INPUT_RELEASED);
        else CHECK(events == 0);
    }
}

static void verify_shared_lines(const xiaotai_display_t* display, const xiaotai_audio_output_t* speaker,
                                const xiaotai_camera_t* camera)
{
    // Camera and speaker can run while the panel keeps its active-low CS.
    CHECK(display->open(display->self, nullptr) == XIAOTAI_OK && pca_output() == 4);
    CHECK(speaker->open(speaker->self) == XIAOTAI_OK && pca_output() == 4);
    CHECK(speaker->set_enabled(speaker->self, true) == XIAOTAI_OK && pca_output() == 6);
    CHECK(camera->set_power(camera->self, true) == XIAOTAI_OK && pca_output() == 2);
    const xiaotai_camera_mode_t mode{160, 120, XIAOTAI_CAMERA_YUV422, 0};
    CHECK(camera->open(camera->self, &mode) == XIAOTAI_OK);
    CHECK(speaker->close(speaker->self) == XIAOTAI_OK && pca_output() == 0);
    CHECK(camera->close(camera->self) == XIAOTAI_OK);
    CHECK(camera->set_power(camera->self, false) == XIAOTAI_OK && pca_output() == 4);
    CHECK(!xiaotai_bus_host_i2c_overlaps());
}

static void print_manifest(const xiaotai_audio_input_t* mic, const xiaotai_audio_output_t* speaker,
                           const xiaotai_display_t* display, const xiaotai_input_t* key, const xiaotai_camera_t* camera)
{
    std::printf("{\"audio_input\":[{\"encoding\":\"pcm16\",\"rate_hz\":%u,\"roles\":[\"mic\",\"reference\",\"mic\",\"unused\"]}],"
                "\"audio_output\":[{\"encoding\":\"pcm16\",\"rate_hz\":%u,\"channels\":%u}],"
                "\"display\":[{\"pixel\":\"rgb565\",\"width\":%u,\"height\":%u,\"touch\":%s}],"
                "\"input\":[{\"kind\":\"button\",\"role\":\"%s\"}],"
                "\"camera\":[{\"formats\":[\"yuv422\"],\"max_width\":%u,\"max_height\":%u}],"
                "\"network\":[{\"kind\":\"wifi\",\"provisioning\":[\"portal\",\"serial\"]}]}\n",
                mic->desc.rate_hz, speaker->desc.rate_hz, unsigned(speaker->desc.channels),
                unsigned(display->desc.width), unsigned(display->desc.height), display->desc.touch ? "true" : "false",
                key->desc.role, unsigned(camera->desc.max_width), unsigned(camera->desc.max_height));
}

int main(int argc, char** argv)
{
    CHECK(argc == 1 || argc == 2);
    const char* mode = argc == 2 ? argv[1] : "parts";
    const int injected = std::atoi(mode);
    CHECK(injected >= -3 && injected <= 15);
    fail = injected > 0 ? injected : 0;
    CHECK(xiaotai_bus_host_i2c_attach(0, 0x19) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_attach(0, 0x38) == XIAOTAI_OK);
    if (injected < 0) xiaotai_bus_host_i2c_fail(0, 0x19, XIAOTAI_BUS_HOST_WRITE, unsigned(-injected), kFailure);
    CHECK(!xiaotai_hal_input(0) && xiaotai_hal_needs() == XIAOTAI_NEED_EXTERNAL_RAM);
    xiaotai_board_identity_t id{};
    xiaotai_hal_identity(&id);
    CHECK(std::strcmp(id.model, "lckfb-szpi-esp32s3") == 0 && std::strcmp(id.revision, "1.0.1") == 0);
    CHECK(std::strcmp(id.profile_model, "LCKFB-SZPI-ESP32S3-V1.0.1") == 0 && std::strcmp(id.profile_chip, "ESP32-S3") == 0);
    CHECK(xiaotai_hal_init() == (injected ? kFailure : XIAOTAI_OK));
    CHECK(xiaotai_hal_init() == (injected ? kFailure : XIAOTAI_OK));
    const int expected_step = injected == -1 || injected == -2 ? 2 : 15;
    CHECK(step == (fail ? fail : expected_step));
    if (injected >= 0 && (!fail || fail > 2)) {
        xiaotai_bus_host_transfer_t log[4]{};
        CHECK(xiaotai_bus_host_i2c_log(log, 4) >= 2);
        CHECK(log[0].op == XIAOTAI_BUS_HOST_WRITE && log[0].address == 0x19 && log[0].reg == 1 && log[0].data[0] == 5);
        CHECK(log[1].op == XIAOTAI_BUS_HOST_WRITE && log[1].address == 0x19 && log[1].reg == 3 && log[1].data[0] == 0xf8);
        CHECK(pca_output() == 5);
    }
    if (step > 6) {
        const auto light = xiaotai_bus_host_pwm(0);
        CHECK(light.configured && light.pin == 42 && light.active_low && light.duty == 0);
    }
    const auto* mic = xiaotai_hal_audio_input(0);
    const auto* speaker = xiaotai_hal_audio_output(0);
    const auto* display = xiaotai_hal_display(0);
    const auto* key = xiaotai_hal_input(0);
    const auto* camera = xiaotai_hal_camera(0);
    const auto* network = xiaotai_hal_network(0);
    if (injected) {
        CHECK(!mic && !speaker && !display && !key && !camera && !network);
        CHECK(xiaotai_hal_needs() == XIAOTAI_NEED_EXTERNAL_RAM);
        if (injected < 0) {
            xiaotai_bus_host_transfer_t log[4]{};
            const auto count = xiaotai_bus_host_i2c_log(log, 4);
            CHECK(count == unsigned(-injected) && log[count - 1].result == kFailure);
            CHECK(log[count - 1].reg == (injected == -2 ? 3 : 1));
            CHECK(pca_output() == (injected == -1 ? 0 : 5));
        }
        return 0;
    }
    CHECK(mic && speaker && display && key && camera && network && xiaotai_hal_needs() == XIAOTAI_BOARD_MANIFEST_NEEDS);
    CHECK(!xiaotai_hal_audio_input(1) && !xiaotai_hal_audio_output(1) && !xiaotai_hal_display(1) && !xiaotai_hal_camera(1));
    CHECK(!xiaotai_hal_input(1) && !xiaotai_hal_network(1) && !xiaotai_hal_output(0) && !xiaotai_hal_measurement(0));
    CHECK(key == xiaotai_hal_input_by_role("main") && key->desc.kind == XIAOTAI_INPUT_BUTTON);
    CHECK(key->desc.events == (XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED));
    CHECK(mic->desc.encoding == XIAOTAI_CODEC_PCM16 && mic->desc.channels == 4 && mic->desc.rate_hz == 16000);
    CHECK(mic->desc.roles[0] == XIAOTAI_CHANNEL_MIC && mic->desc.roles[1] == XIAOTAI_CHANNEL_REFERENCE &&
          mic->desc.roles[2] == XIAOTAI_CHANNEL_MIC && mic->desc.roles[3] == XIAOTAI_CHANNEL_UNUSED);
    CHECK(mic->info.group == 1 && mic->info.gain_db == 24 && mic->info.gain_min_db == 24 && mic->info.gain_max_db == 24);
    CHECK(mic->info.block_frames == 240 && !mic->info.read_timeout_ms && mic->info.echo_coupling_db == XIAOTAI_AUDIO_COUPLING_UNKNOWN);
    CHECK(speaker->desc.encoding == XIAOTAI_CODEC_PCM16 && speaker->desc.channels == 2 && speaker->desc.rate_hz == 16000);
    CHECK(speaker->desc.roles[0] == XIAOTAI_CHANNEL_LEFT && speaker->desc.roles[1] == XIAOTAI_CHANNEL_RIGHT);
    CHECK(speaker->info.group == 1 && speaker->info.enable_ms == 120 && speaker->info.latency_ms == 90);
    CHECK(speaker->info.max_write_frames == 0 && speaker->info.flags == XIAOTAI_AUDIO_OUTPUT_PACED);
    CHECK(display->desc.pixel == XIAOTAI_PIXEL_RGB565 && display->desc.render == XIAOTAI_RENDER_PARTIAL && !display->desc.rotation);
    CHECK(display->desc.transfer_pixels == 38400 && display->desc.flags ==
          (XIAOTAI_DISPLAY_SWAP_BYTES | XIAOTAI_DISPLAY_EVEN_AREAS | XIAOTAI_DISPLAY_DIMMABLE));
    CHECK(camera->desc.formats == XIAOTAI_CAMERA_YUV422 && !camera->prepare && !camera->desc.rotation);
    CHECK(camera->desc.min_width == 160 && camera->desc.min_height == 120);
    CHECK(network->desc.kind == XIAOTAI_NETWORK_WIFI && !network->prepare && !network->coprocessor_info);
    CHECK(network->desc.provisioning == (XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL));
    if (std::strcmp(mode, "audio") == 0) verify_audio(mic, speaker);
    else if (std::strcmp(mode, "display") == 0) verify_display(display);
    else if (std::strcmp(mode, "cleanup-failure") == 0) verify_cleanup_failure(display);
    else if (std::strcmp(mode, "camera") == 0) {
        verify_camera(camera);
        return 0;
    } else if (std::strcmp(mode, "shared") == 0) {
        verify_shared_lines(display, speaker, camera);
        return 0;
    } else if (std::strcmp(mode, "button") == 0) verify_button(key);
    else {
        CHECK(std::strcmp(mode, "parts") == 0);
        print_manifest(mic, speaker, display, key, camera);
    }
    CHECK(xt_dvp_fake_status().opens == 0 && (pca_output() & 4) != 0);
}
