// SPDX-License-Identifier: MIT
#include "xiaotai/board.hpp"
#include "xiaotai/parts/audio.h"
#include "xiaotai/parts/button.h"
#include "xiaotai/parts/camera_dvp.h"
#include "xiaotai/parts/panel_spi.h"
#include "xiaotai/parts/power.h"
#include "xiaotai/parts/touch_ft5x06.h"
#include "xiaotai_board_data.hpp"

namespace {

constexpr xiaotai_i2c_config_t kI2c{0, 1, 2, 100000, true, 0};
// Timer and channel 1 make the camera's clock.
constexpr xiaotai_pwm_config_t kBacklight{42, 0, 0, 5000, 10, true};
constexpr xiaotai_gpio_config_t kButton{0, true, true, false, false, false};
// The deployed debounce accepted the sample after the 50 ms boundary.
constexpr uint16_t kButtonDebounceMs = 50 + XIAOTAI_INPUT_POLL_MS;
constexpr uint32_t kRate = 16000;
constexpr uint16_t kMclkMultiple = 256;
constexpr uint16_t kDisplayWidth = 320;
constexpr uint16_t kDisplayHeight = 240;
constexpr uint32_t kTransferPixels = 3200;
constexpr uint32_t kStripPixels = 320 * 120;
constexpr xiaotai_network_t kWifi{
    {XIAOTAI_NETWORK_WIFI, XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL},
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

class Szpi final : public xiaotai::board::Board {
public:
    xiaotai_err_t Init() override
    {
        auto err = xiaotai_i2c_create(&kI2c, &i2c_);
        if (err != XIAOTAI_OK) return err;
        // Keep the panel deselected, amplifier muted and camera unpowered.
        const xiaotai_pca9557_config_t expander{i2c_, 0x19, 0x07, 0x05, 500};
        err = xiaotai_pca9557_create(&expander, &expander_);
        if (err != XIAOTAI_OK) return err;
        xiaotai_line_t panel_cs{}, amplifier{}, camera_power{}, key{};
        if ((err = xiaotai_pca9557_line(expander_, 0, true, &panel_cs)) != XIAOTAI_OK ||
            (err = xiaotai_pca9557_line(expander_, 1, false, &amplifier)) != XIAOTAI_OK ||
            (err = xiaotai_pca9557_line(expander_, 2, true, &camera_power)) != XIAOTAI_OK ||
            (err = xiaotai_level_pwm(&kBacklight, &backlight_)) != XIAOTAI_OK ||
            (err = xiaotai_line_gpio_input(&kButton, &key)) != XIAOTAI_OK) return err;

        if ((err = CreateAudio(amplifier)) != XIAOTAI_OK ||
            (err = CreateDisplay(panel_cs)) != XIAOTAI_OK) return err;
        const xiaotai_button_config_t button{key, "main", kButtonDebounceMs, false, 0};
        err = xiaotai_button_create(&button, &key_);
        if (err != XIAOTAI_OK) return err;
        xiaotai_camera_dvp_config_t camera{};
        camera.sccb = i2c_;
        camera.xclk_pin = 5;
        camera.xclk_hz = 20000000;
        camera.xclk_timer = camera.xclk_channel = 1;
        constexpr int data_pins[] = {16, 18, 8, 17, 15, 6, 4, 9};
        for (size_t i = 0; i < 8; ++i) camera.data_pins[i] = data_pins[i];
        camera.vsync_pin = 3;
        camera.href_pin = 46;
        camera.pclk_pin = 7;
        camera.sensor_id = 0x2145;  // GC2145
        camera.max_width = 320;
        camera.max_height = 240;
        camera.power = camera_power;
        camera.power_settle_ms = 10;
        return xiaotai_camera_dvp_create(&camera, &camera_);
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
        xiaotai_pca9557_report(expander_, write, ctx);
        xiaotai_audio_device_report(mic_, write, ctx);
    }

private:
    xiaotai_err_t CreateAudio(const xiaotai_line_t& amplifier)
    {
        const xiaotai_es7210_config_t adc{i2c_, 0x41, kMclkMultiple, 0x0f};
        xiaotai_es8311_config_t dac{};
        dac.bus = i2c_;
        dac.mclk_multiple = kMclkMultiple;
        dac.playback = true;
        dac.i2c_hz = kI2c.hz;
        xiaotai_audio_device_config_t audio{};
        auto err = xiaotai_es7210_create(&adc, &audio.capture_codec);
        if (err != XIAOTAI_OK) return err;
        err = xiaotai_es8311_create(&dac, &audio.playback_codec);
        if (err != XIAOTAI_OK) return err;
        audio.i2s = {0, 38, 14, 13, 12, 45, kRate, kMclkMultiple, 4, 2, 0, 0,
                     XIAOTAI_I2S_SLOTS, XIAOTAI_I2S_SLOTS};
        audio.group = 1;
        audio.capture_channels = 4;
        // ES7210 slot order is MIC1/MIC3/MIC2/MIC4; gain is by physical input.
        audio.capture_roles[0] = XIAOTAI_CHANNEL_MIC;
        audio.capture_roles[1] = XIAOTAI_CHANNEL_REFERENCE;
        audio.capture_roles[2] = XIAOTAI_CHANNEL_MIC;
        audio.capture_roles[3] = XIAOTAI_CHANNEL_UNUSED;
        audio.input_gain_db[0] = audio.input_gain_db[1] = 24;
        audio.mic_inputs = 0x03;
        audio.gain_min_db = audio.gain_max_db = 24;
        audio.echo_coupling_db = XIAOTAI_AUDIO_COUPLING_UNKNOWN;
        audio.capture_mode = XIAOTAI_AUDIO_CAPTURE_BLOCKING;
        audio.playback_channels = 2;
        audio.amplifier = amplifier;
        audio.amplifier_enable_ms = 120;
        return xiaotai_audio_device_create(&audio, &mic_, &speaker_);
    }

    xiaotai_err_t CreateDisplay(const xiaotai_line_t& chip_select)
    {
        xiaotai_panel_spi_config_t panel{};
        panel.host = 2;  // ESP-IDF SPI3_HOST (zero-based controller index).
        panel.mosi_pin = 40;
        panel.sclk_pin = 41;
        panel.dc_pin = 39;
        panel.cs_pin = -1;
        panel.cs_line = chip_select;
        panel.hz = 80000000;
        panel.spi_mode = 2;
        panel.queue_depth = 4;
        panel.max_transfer_bytes = kTransferPixels * sizeof(uint16_t);
        panel.native_width = kDisplayHeight;
        panel.native_height = kDisplayWidth;
        panel.invert = panel.swap_xy = panel.mirror_x = true;

        const xiaotai_touch_i2c_config_t touch{i2c_, 0, kI2c.hz, kDisplayHeight, kDisplayWidth,
                                               true, true, false, -1, false, -1};
        xiaotai_display_config_t display{};
        auto err = xiaotai_panel_spi_create(xiaotai_panel_st7789(), &panel, &display.panel);
        if (err != XIAOTAI_OK) return err;
        err = xiaotai_touch_ft5x06_create(&touch, &display.touch);
        if (err != XIAOTAI_OK) return err;
        display.desc.width = kDisplayWidth;
        display.desc.height = kDisplayHeight;
        display.desc.pixel = XIAOTAI_PIXEL_RGB565;
        // Only what changed goes over the bus: a whole frame takes 26 ms of it.
        display.desc.render = XIAOTAI_RENDER_PARTIAL;
        display.desc.transfer_pixels = kStripPixels;
        display.desc.flags = XIAOTAI_DISPLAY_SWAP_BYTES | XIAOTAI_DISPLAY_EVEN_AREAS | XIAOTAI_DISPLAY_DIMMABLE;
        display.backlight = backlight_;
        return xiaotai_display_create(&display, &display_);
    }

    xiaotai_i2c_bus_t* i2c_ = nullptr;
    xiaotai_pca9557_t* expander_ = nullptr;
    xiaotai_level_t backlight_{};
    const xiaotai_audio_input_t* mic_ = nullptr;
    const xiaotai_audio_output_t* speaker_ = nullptr;
    const xiaotai_display_t* display_ = nullptr;
    const xiaotai_input_t* key_ = nullptr;
    const xiaotai_camera_t* camera_ = nullptr;
};

}  // namespace

xiaotai::board::Board& xiaotai::board::SelectedBoard()
{
    static Szpi board;
    return board;
}
