// SPDX-License-Identifier: MIT
#include <cinttypes>
#include <cstdio>
#include <iterator>

#include "xiaotai/board.hpp"
#include "xiaotai/parts/audio.h"
#include "xiaotai/parts/button.h"
#include "xiaotai/parts/camera_csi.h"
#include "xiaotai/parts/coprocessor.h"
#include "xiaotai/parts/panel_st7796.h"
#include "xiaotai/parts/touch_ft5x06.h"
#include "xiaotai_board_data.hpp"

extern "C" const uint8_t _binary_waveshare_c6_app_bin_start[];

namespace {

constexpr xiaotai_i2c_config_t kI2c{1, 7, 8, 400000, true, 0};
constexpr xiaotai_gpio_config_t kAmp{53, false, false, false, false, false};
constexpr xiaotai_pwm_config_t kBacklight{28, 1, 1, 5000, 10, false};
constexpr xiaotai_gpio_config_t kDisplayReset{27, true, false, false, false, false};
constexpr xiaotai_gpio_config_t kButton{35, true, true, false, false, false};
constexpr xiaotai_i2s_config_t kI2s{1, 13, 12, 10, 11, 9, 16000, 256, 2, 2, 6, 240,
                                     XIAOTAI_I2S_SLOTS, XIAOTAI_I2S_SLOTS};
constexpr uint16_t kNativeWidth = 320;
constexpr uint16_t kNativeHeight = 480;
// Two strips of 80 rows (UI_PRESENT_TASK: one is drawn while the other is
// sent): finer strips overlap more, and two take the memory one of 160 did.
constexpr uint32_t kStripPixels = 480 * 80;
constexpr xiaotai_sensor_reg_t kAntiflicker[]{
    {0x3a00, 0x20, 0x20}, {0x3c00, 0x04, 0x04}, {0x3c01, 0x80, 0x80},
};

constexpr char kC6Project[] = "eh_cp_wifi_sta";
constexpr const char* kC6Protocols[]{"3.0.7", nullptr};
constexpr xiaotai_coprocessor_image_t kC6Image{
    _binary_waveshare_c6_app_bin_start, XIAOTAI_C6_IMAGE_SIZE, XIAOTAI_C6_IMAGE_SHA,
    kC6Project, "3.0.7-xiaotai.5",
};
constexpr xiaotai_coprocessor_source_t kC6Sources[]{
    {kC6Project, "3.0.7-xiaotai.2", "15cc2697818686a6a7a4120af6453bc8bd663095e60e0c3ad160677e1abff243",
     0x10f760},
    {kC6Project, "3.0.7-xiaotai.3", "56f93ac0ba85a4316dcf67378cde14d8cf0ae370bb33c81270026817d3e85aaf",
     0x10f760},
    {kC6Project, "3.0.7-xiaotai.4", "1eca43db04c735cc505fe9d05ebb73cf621156c1df831c444ba28ae904c3ab03",
     0x10f760},
};
constexpr auto kC6 = [] {
    xiaotai_coprocessor_config_t config{};
    config.provisioning = XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL;
    config.chip_id = 0x0d;
    config.link_timeout_ms = 15000;
    config.protocols = kC6Protocols;
    config.image = &kC6Image;
    config.sources = kC6Sources;
    config.source_count = std::size(kC6Sources);
    config.legacy = {kC6Project, "1", nullptr, 0};
    config.legacy_protocol_capability = 0x0d;
    config.legacy_image_max = 0x180000;
    config.activate_wait_ms = 50;
    config.legacy_activate_wait_ms = 6000;
    config.write_block_bytes = 1024;
    config.attempt_space = "c6_update";
    config.attempt_key = "attempt";
    return config;
}();

class WaveshareP4 final : public xiaotai::board::Board {
public:
    xiaotai_err_t Init() override
    {
        xiaotai_i2c_bus_t* i2c = nullptr;
        xiaotai_line_t amp{}, reset{}, key{};
        xiaotai_level_t light{};
        auto err = xiaotai_i2c_create(&kI2c, &i2c);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_output(&kAmp, &amp);
        if (err == XIAOTAI_OK) err = xiaotai_level_pwm(&kBacklight, &light);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_output(&kDisplayReset, &reset);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_input(&kButton, &key);
        if (err == XIAOTAI_OK) err = CreateAudio(i2c, amp);
        if (err == XIAOTAI_OK) err = CreateDisplay(i2c, reset, light);
        const xiaotai_button_config_t button{key, "main", XIAOTAI_BUTTON_DEBOUNCE_MS, false, 0};
        if (err == XIAOTAI_OK) err = xiaotai_button_create(&button, &button_);
        if (err == XIAOTAI_OK) err = CreateCamera(i2c);
        if (err == XIAOTAI_OK) err = xiaotai_coprocessor_create(&kC6, &network_);
        return err;
    }

    xiaotai_board_identity_t Identity() const override
    {
        return {XIAOTAI_BOARD_MODEL, XIAOTAI_BOARD_REVISION,
                XIAOTAI_BOARD_PROFILE_MODEL, XIAOTAI_BOARD_PROFILE_CHIP};
    }

    const xiaotai_audio_input_t* AudioInput(size_t i) override { return i == 0 ? mic_ : nullptr; }
    const xiaotai_audio_output_t* AudioOutput(size_t i) override { return i == 0 ? speaker_ : nullptr; }
    const xiaotai_display_t* Display(size_t i) override { return i == 0 ? display_ : nullptr; }
    const xiaotai_input_t* Input(size_t i) override { return i == 0 ? button_ : nullptr; }
    const xiaotai_camera_t* Camera(size_t i) override { return i == 0 ? camera_ : nullptr; }
    const xiaotai_network_t* Network(size_t i) override { return i == 0 ? network_ : nullptr; }

    void Report(xiaotai_hal_write_fn_t write, void* ctx) override
    {
        xiaotai_audio_device_report(mic_, write, ctx);
        xiaotai_camera_csi_report(camera_, write, ctx);
        xiaotai_display_part_stats_t display{};
        xiaotai_display_part_stats(display_, &display);
        char line[192];
        std::snprintf(line, sizeof(line), "display opens=%" PRIu32 " open_failures=%" PRIu32
                      " present_errors=%" PRIu32 " touch_errors=%" PRIu32 " latched=%u",
                      display.opens, display.open_failures, display.present_errors, display.touch_errors,
                      static_cast<unsigned>(display.latched));
        write(ctx, line);
        xiaotai_coprocessor_state_t c6{};
        xiaotai_coprocessor_state(network_, &c6);
        std::snprintf(line, sizeof(line), "coprocessor stage=%u rule=%u error=%d attempt=%u transfer=%u",
                      static_cast<unsigned>(c6.stage), static_cast<unsigned>(c6.rule), c6.error,
                      static_cast<unsigned>(c6.attempt_recorded), static_cast<unsigned>(c6.transfer_started));
        write(ctx, line);
    }

private:
    xiaotai_err_t CreateAudio(xiaotai_i2c_bus_t* i2c, xiaotai_line_t amp)
    {
        xiaotai_audio_codec_t* codec = nullptr;
        xiaotai_es8311_config_t es8311{};
        es8311.bus = i2c;
        es8311.mclk_multiple = kI2s.mclk_multiple;
        es8311.capture = es8311.playback = es8311.reference = true;
        es8311.i2c_hz = 100000;
        auto err = xiaotai_es8311_create(&es8311, &codec);
        if (err != XIAOTAI_OK) return err;
        xiaotai_audio_device_config_t audio{};
        audio.i2s = kI2s;
        audio.group = 1;
        audio.capture_codec = codec;
        audio.capture_channels = 2;
        audio.capture_roles[0] = XIAOTAI_CHANNEL_MIC;
        audio.capture_roles[1] = XIAOTAI_CHANNEL_REFERENCE;
        audio.input_gain_db[0] = 30;
        audio.mic_inputs = 1;
        audio.echo_coupling_db = XIAOTAI_AUDIO_COUPLING_UNKNOWN;
        audio.playback_codec = codec;
        audio.playback_channels = 2;
        audio.max_write_frames = kI2s.dma_frames;
        audio.amplifier = amp;
        audio.amplifier_enable_ms = 120;
        return xiaotai_audio_device_create(&audio, &mic_, &speaker_);
    }

    xiaotai_err_t CreateDisplay(xiaotai_i2c_bus_t* i2c, xiaotai_line_t reset, xiaotai_level_t light)
    {
        xiaotai_display_config_t lcd{};
        // The controller turns the picture (swap_xy below); turning a frame in
        // memory took 140 ms of its 190. Only what changed goes over the bus.
        lcd.desc = {kNativeHeight, kNativeWidth, XIAOTAI_PIXEL_RGB565, false, false, XIAOTAI_RENDER_PARTIAL,
                    0, 0, 0, kStripPixels, XIAOTAI_DISPLAY_SWAP_BYTES | XIAOTAI_DISPLAY_DIMMABLE};
        auto err = xiaotai_panel_spi_external_dma_alignment(&lcd.desc.buffer_align);
        if (err != XIAOTAI_OK) return err;
        lcd.backlight = light;

        xiaotai_panel_spi_config_t spi{};
        spi.host = 1;  // ESP-IDF's SPI2_HOST index.
        spi.mosi_pin = 20;
        spi.sclk_pin = 21;
        spi.dc_pin = 26;
        spi.cs_pin = 23;
        spi.reset = reset;
        spi.reset_hold_ms = 10;
        spi.reset_settle_ms = 120;
        spi.hz = 80000000;
        spi.queue_depth = 20;
        spi.native_width = kNativeWidth;
        spi.native_height = kNativeHeight;
        spi.bgr = spi.invert = spi.swap_xy = spi.mirror_x = spi.mirror_y = true;
        spi.external_dma = spi.wait_done = true;
        err = xiaotai_panel_spi_create(xiaotai_panel_st7796(), &spi, &lcd.panel);
        if (err != XIAOTAI_OK) return err;

        const xiaotai_touch_i2c_config_t touch{
            i2c, 0, kI2c.hz, kNativeWidth, kNativeHeight, true, false, true, 29, false, 50,
        };
        err = xiaotai_touch_ft5x06_create(&touch, &lcd.touch);
        if (err != XIAOTAI_OK) return err;
        return xiaotai_display_create(&lcd, &display_);
    }

    xiaotai_err_t CreateCamera(xiaotai_i2c_bus_t* i2c)
    {
        xiaotai_camera_csi_config_t camera{};
        camera.sccb = i2c;
        camera.sccb_hz = 100000;
        camera.sensor_address = 0x36;
        camera.width = 1280;
        camera.height = 960;
        camera.fps = 45;
        camera.rotation = 3;
        camera.buffers = 3;
        camera.setup = kAntiflicker;
        camera.setup_count = std::size(kAntiflicker);
        return xiaotai_camera_csi_create(&camera, &camera_);
    }

    const xiaotai_audio_input_t* mic_ = nullptr;
    const xiaotai_audio_output_t* speaker_ = nullptr;
    const xiaotai_display_t* display_ = nullptr;
    const xiaotai_input_t* button_ = nullptr;
    const xiaotai_camera_t* camera_ = nullptr;
    const xiaotai_network_t* network_ = nullptr;
};

}  // namespace

xiaotai::board::Board& xiaotai::board::SelectedBoard()
{
    static WaveshareP4 board;
    return board;
}
