// SPDX-License-Identifier: MIT
#include "xiaotai/board.hpp"
#include "xiaotai/parts/audio.h"
#include "xiaotai/parts/camera_dvp.h"
#include "xiaotai/parts/panel_ili9341.h"
#include "xiaotai/parts/power.h"
#include "xiaotai/parts/touch_ft5x06.h"
#include "xiaotai_board_data.hpp"

namespace {

constexpr xiaotai_i2c_config_t kI2c{0, 12, 11, 400000, true, 100};
constexpr uint8_t kExpanderAddress = 0x58;
constexpr uint8_t kPowerAddress = 0x34;
constexpr uint8_t kSpi3Host = 2;  // Platform host index, not the controller number.
constexpr uint16_t kGc0308Pid = 0x9b;
constexpr uint16_t kWidth = 320;
constexpr uint16_t kHeight = 240;
constexpr uint32_t kTransferPixels = kWidth * 10;

// M5Unified / M5GFX (MIT) setup; keep USB in device mode and hold camera reset.
constexpr xiaotai_reg_value_t kExpanderSetup[] = {
    {0x02, 0x05}, {0x03, 0x82}, {0x04, 0x18}, {0x05, 0x0c},
    {0x11, 0x10}, {0x12, 0xff}, {0x13, 0xff},
};
constexpr xiaotai_reg_value_t kPowerSetup[] = {
    {0x92, 13}, {0x93, 28}, {0x95, 28}, {0x99, 23}, {0x90, 0xab},
    {0x27, 0x00}, {0x69, 0x11}, {0x10, 0x30}, {0x30, 0x0f},
};
constexpr xiaotai_axp2101_rail_t kBacklightRail{0x90, 0x80, 0, 0};
constexpr xiaotai_axp2101_rail_t kCameraRail{0x90, 0x14, 0x94, 28};

// ILI9342C power/gamma sequence from M5GFX (MIT).
constexpr uint8_t kPanelId[] = {0xff, 0x93, 0x42};
constexpr uint8_t kPanelPower[] = {0x12, 0x12};
constexpr uint8_t kPanelPower2[] = {0x03};
constexpr uint8_t kPanelVcom[] = {0xf2};
constexpr uint8_t kPanelInterface[] = {0xe0};
constexpr uint8_t kPanelInterface2[] = {0x01, 0x00, 0x00};
constexpr uint8_t kPanelGammaP[] = {0x00, 0x0c, 0x11, 0x04, 0x11, 0x08, 0x37, 0x89,
                                  0x4c, 0x06, 0x0c, 0x0a, 0x2e, 0x34, 0x0f};
constexpr uint8_t kPanelGammaN[] = {0x00, 0x0b, 0x11, 0x05, 0x13, 0x09, 0x33, 0x67,
                                  0x48, 0x07, 0x0e, 0x0b, 0x2e, 0x33, 0x0f};
constexpr uint8_t kPanelDisplayFn[] = {0x08, 0x82, 0x1d, 0x04};
constexpr xiaotai_panel_command_t kPanelSetup[] = {
    {0xc8, kPanelId, sizeof(kPanelId), 0}, {0xc0, kPanelPower, sizeof(kPanelPower), 0},
    {0xc1, kPanelPower2, sizeof(kPanelPower2), 0}, {0xc5, kPanelVcom, sizeof(kPanelVcom), 0},
    {0xb0, kPanelInterface, sizeof(kPanelInterface), 0}, {0xf6, kPanelInterface2, sizeof(kPanelInterface2), 0},
    {0xe0, kPanelGammaP, sizeof(kPanelGammaP), 0}, {0xe1, kPanelGammaN, sizeof(kPanelGammaN), 0},
    {0xb6, kPanelDisplayFn, sizeof(kPanelDisplayFn), 0}, {0x38, nullptr, 0, 0}, {0x11, nullptr, 0, 120},
};
constexpr xiaotai_network_t kWifi{
    {XIAOTAI_NETWORK_WIFI, XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL},
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

class CoreS3 final : public xiaotai::board::Board {
public:
    xiaotai_err_t Init() override
    {
        auto err = xiaotai_i2c_create(&kI2c, &bus_);
        if (err != XIAOTAI_OK) return err;
        const xiaotai_aw9523_config_t expander{bus_, kExpanderAddress, kExpanderSetup, sizeof(kExpanderSetup) / sizeof(*kExpanderSetup)};
        err = xiaotai_aw9523_create(&expander, &expander_);
        if (err != XIAOTAI_OK) return err;
        const xiaotai_axp2101_config_t power{bus_, kPowerAddress, kPowerSetup, sizeof(kPowerSetup) / sizeof(*kPowerSetup)};
        err = xiaotai_axp2101_create(&power, &power_);
        if (err != XIAOTAI_OK) return err;
        xiaotai_line_t lcd_reset{}, camera_reset{}, camera_power{};
        err = xiaotai_aw9523_line(expander_, 1, 1, true, &lcd_reset);
        if (err != XIAOTAI_OK) return err;
        err = xiaotai_aw9523_line(expander_, 1, 0, true, &camera_reset);
        if (err != XIAOTAI_OK) return err;
        err = xiaotai_axp2101_line(power_, &kBacklightRail, &backlight_);
        if (err != XIAOTAI_OK) return err;
        err = xiaotai_axp2101_line(power_, &kCameraRail, &camera_power);
        if (err != XIAOTAI_OK) return err;
        err = CreateDisplay(lcd_reset);
        if (err != XIAOTAI_OK) return err;
        err = CreateAudio();
        if (err != XIAOTAI_OK) return err;
        err = xiaotai_axp2101_power_key(power_, "main", &key_);
        if (err != XIAOTAI_OK) return err;
        return CreateCamera(camera_reset, camera_power);
    }

    xiaotai_board_identity_t Identity() const override
    {
        return {XIAOTAI_BOARD_MODEL, XIAOTAI_BOARD_REVISION,
                XIAOTAI_BOARD_PROFILE_MODEL, XIAOTAI_BOARD_PROFILE_CHIP};
    }

    const xiaotai_audio_input_t* AudioInput(size_t i) override { return i == 0 ? mic_ : nullptr; }
    const xiaotai_audio_output_t* AudioOutput(size_t i) override { return i == 0 ? speaker_ : nullptr; }
    const xiaotai_display_t* Display(size_t i) override { return i == 0 ? display_ : nullptr; }
    const xiaotai_input_t* Input(size_t i) override { return i == 0 ? key_ : nullptr; }
    const xiaotai_camera_t* Camera(size_t i) override { return i == 0 ? camera_ : nullptr; }
    const xiaotai_network_t* Network(size_t i) override { return i == 0 ? &kWifi : nullptr; }

    void Report(xiaotai_hal_write_fn_t write, void* ctx) override
    {
        xiaotai_aw9523_report(expander_, write, ctx);
        xiaotai_axp2101_report(power_, write, ctx);
        xiaotai_audio_device_report(mic_, write, ctx);
    }

private:
    xiaotai_err_t CreateAudio()
    {
        constexpr uint16_t kMclkMultiple = 256;
        xiaotai_audio_codec_t *adc = nullptr, *amp = nullptr;
        const xiaotai_es7210_config_t es7210{bus_, 0, kMclkMultiple, 0x07};
        auto err = xiaotai_es7210_create(&es7210, &adc);
        if (err != XIAOTAI_OK) return err;
        const xiaotai_aw88298_config_t aw88298{bus_, 0, {}, 64};
        err = xiaotai_aw88298_create(&aw88298, &amp);
        if (err != XIAOTAI_OK) return err;

        xiaotai_audio_device_config_t audio{};
        audio.i2s = {0, 0, 34, 33, 14, 13, 16000, kMclkMultiple,
                     4, 2, 6, 256, XIAOTAI_I2S_SLOTS, XIAOTAI_I2S_SLOTS};
        audio.group = 1;
        audio.capture_codec = adc;
        audio.capture_channels = 4;
        audio.capture_roles[0] = XIAOTAI_CHANNEL_MIC;
        audio.capture_roles[1] = XIAOTAI_CHANNEL_REFERENCE;
        audio.capture_roles[2] = XIAOTAI_CHANNEL_MONITOR;  // Secondary mic is diagnostic, not an AFE input.
        audio.capture_roles[3] = XIAOTAI_CHANNEL_UNUSED;
        audio.input_gain_db[0] = audio.input_gain_db[1] = 24;
        audio.mic_inputs = 0x03;
        audio.gain_min_db = audio.gain_max_db = 24;
        audio.echo_coupling_db = XIAOTAI_AUDIO_COUPLING_UNKNOWN;
        audio.capture_mode = XIAOTAI_AUDIO_CAPTURE_RING;
        audio.ring_blocks = 16;
        audio.read_timeout_ms = 100;
        audio.playback_codec = amp;
        audio.playback_channels = 2;
        return xiaotai_audio_device_create(&audio, &mic_, &speaker_);
    }

    xiaotai_err_t CreateDisplay(xiaotai_line_t reset)
    {
        xiaotai_panel_spi_config_t spi{};
        spi.host = kSpi3Host;
        spi.mosi_pin = 37;
        spi.sclk_pin = 36;
        spi.dc_pin = 35;
        spi.cs_pin = 3;
        spi.reset = reset;
        spi.reset_hold_ms = 10;
        spi.reset_settle_ms = 120;
        spi.software_reset_after_line = true;
        spi.reset_line_at_create = true;
        spi.hz = 40000000;
        spi.queue_depth = 4;
        spi.max_transfer_bytes = kTransferPixels * sizeof(uint16_t);
        spi.native_width = kWidth;
        spi.native_height = kHeight;
        spi.bgr = spi.invert = true;
        spi.wait_done = true;
        spi.init = kPanelSetup;
        spi.init_count = sizeof(kPanelSetup) / sizeof(*kPanelSetup);

        xiaotai_display_config_t display{};
        auto err = xiaotai_panel_spi_create(xiaotai_panel_ili9341(), &spi, &display.panel);
        if (err != XIAOTAI_OK) return err;
        const xiaotai_touch_i2c_config_t ft6336{bus_, 0, 400000, kWidth, kHeight,
                                              false, false, false, -1, false, -1};
        err = xiaotai_touch_ft5x06_create(&ft6336, &display.touch);
        if (err != XIAOTAI_OK) return err;
        xiaotai_level_from_line(&backlight_, &display.backlight);
        display.backlight_off_before_start = true;
        display.latch_on_start_cleanup_error = true;
        display.desc.width = kWidth;
        display.desc.height = kHeight;
        display.desc.pixel = XIAOTAI_PIXEL_RGB565;
        display.desc.render = XIAOTAI_RENDER_PARTIAL;
        display.desc.buffer_align = 4;
        display.desc.transfer_pixels = kTransferPixels;
        display.desc.flags = XIAOTAI_DISPLAY_SWAP_BYTES | XIAOTAI_DISPLAY_DMA_BUFFER | XIAOTAI_DISPLAY_EVEN_AREAS;
        return xiaotai_display_create(&display, &display_);
    }

    xiaotai_err_t CreateCamera(xiaotai_line_t reset, xiaotai_line_t power)
    {
        const xiaotai_camera_dvp_config_t camera{
            bus_, -1, 20000000, 1, 1, {39, 40, 41, 42, 15, 16, 48, 47}, 46, 38, 45,
            kGc0308Pid, 240, 176, 0, 0, power, reset, 10, 10, true,
        };
        return xiaotai_camera_dvp_create(&camera, &camera_);
    }

    xiaotai_i2c_bus_t* bus_ = nullptr;
    xiaotai_aw9523_t* expander_ = nullptr;
    xiaotai_axp2101_t* power_ = nullptr;
    xiaotai_line_t backlight_{};  // The level adapter borrows the line for the life of the display.
    const xiaotai_audio_input_t* mic_ = nullptr;
    const xiaotai_audio_output_t* speaker_ = nullptr;
    const xiaotai_display_t* display_ = nullptr;
    const xiaotai_input_t* key_ = nullptr;
    const xiaotai_camera_t* camera_ = nullptr;
};

}  // namespace

xiaotai::board::Board& xiaotai::board::SelectedBoard()
{
    static CoreS3 board;
    return board;
}
