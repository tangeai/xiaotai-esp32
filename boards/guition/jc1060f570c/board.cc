// SPDX-License-Identifier: MIT
#include "xiaotai/board.hpp"
#include "xiaotai/parts/audio.h"
#include "xiaotai/parts/display.h"
#include "xiaotai/parts/panel_rgb.h"
#include "xiaotai/parts/touch_gt911.h"
#include "xiaotai_board_data.hpp"

namespace {

constexpr uint16_t kWidth = 1024, kHeight = 600;
// GT911 touch and the ES8389 codec share I2C0.
constexpr xiaotai_i2c_config_t kI2c{0, 2, 3, 400000, true, 100};
constexpr xiaotai_gpio_config_t kAmplifier{42, false, false, false, false, false};
constexpr xiaotai_pwm_config_t kBacklight{52, 0, 0, 20000, 10, false};
constexpr uint32_t kRate = 16000;
constexpr xiaotai_network_t kWifi{
    {XIAOTAI_NETWORK_WIFI, XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL},
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

class Jc1060f570c final : public xiaotai::board::Board {
public:
    xiaotai_err_t Init() override
    {
        xiaotai_i2c_bus_t* i2c = nullptr;
        xiaotai_line_t amplifier{};
        xiaotai_level_t backlight{};
        auto err = xiaotai_i2c_create(&kI2c, &i2c);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_output(&kAmplifier, &amplifier);
        if (err == XIAOTAI_OK) err = xiaotai_level_pwm(&kBacklight, &backlight);
        if (err == XIAOTAI_OK) err = CreateAudio(i2c, amplifier);
        if (err == XIAOTAI_OK) err = CreateDisplay(i2c, backlight);
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
    const xiaotai_network_t* Network(size_t i) override { return i == 0 ? &kWifi : nullptr; }

    void Report(xiaotai_hal_write_fn_t write, void* ctx) override
    {
        xiaotai_audio_device_report(mic_, write, ctx);
    }

private:
    xiaotai_err_t CreateAudio(xiaotai_i2c_bus_t* i2c, const xiaotai_line_t& amplifier)
    {
        // ES8389 runs from the bit clock (no MCLK line) and loops the DAC back
        // into capture: four TDM slots MIC1, reference, MIC2, unused (the
        // vendor's AFE input format "MRMN"). The board has one microphone:
        // the third slot carries the same microphone in opposite phase
        // (correlation -0.97 in quiet, measured 2026-11-26), so it is not a
        // second microphone and the echo canceller takes one.
        xiaotai_es8389_config_t es8389{};
        es8389.bus = i2c;
        es8389.capture = es8389.playback = es8389.reference = true;
        es8389.inputs = 0x03;
        es8389.i2c_hz = kI2c.hz;
        xiaotai_audio_codec_t* codec = nullptr;
        auto err = xiaotai_es8389_create(&es8389, &codec);
        if (err != XIAOTAI_OK) return err;
        xiaotai_audio_device_config_t audio{};
        audio.i2s = {0, -1, 54, 47, 16, 56, kRate, 256, 4, 2, 0, 0, XIAOTAI_I2S_SLOTS, XIAOTAI_I2S_SLOTS};
        audio.group = 1;
        audio.capture_codec = codec;
        audio.capture_channels = 4;
        audio.capture_roles[0] = XIAOTAI_CHANNEL_MIC;
        audio.capture_roles[1] = XIAOTAI_CHANNEL_REFERENCE;
        audio.capture_roles[2] = XIAOTAI_CHANNEL_UNUSED;
        audio.capture_roles[3] = XIAOTAI_CHANNEL_UNUSED;
        audio.input_gain_db[0] = audio.input_gain_db[1] = 24;
        audio.mic_inputs = 0x03;
        audio.gain_min_db = audio.gain_max_db = 24;
        audio.echo_coupling_db = XIAOTAI_AUDIO_COUPLING_UNKNOWN;
        audio.capture_mode = XIAOTAI_AUDIO_CAPTURE_BLOCKING;
        audio.playback_codec = codec;
        audio.playback_channels = 2;
        audio.amplifier = amplifier;
        audio.amplifier_enable_ms = 120;
        return xiaotai_audio_device_create(&audio, &mic_, &speaker_);
    }

    xiaotai_err_t CreateDisplay(xiaotai_i2c_bus_t* i2c, const xiaotai_level_t& backlight)
    {
        // 7" 1024x600 IPS on a 16-bit RGB bus (JD9165 in RGB mode, no commands).
        xiaotai_panel_rgb_config_t panel{};
        panel.pixel_clock_hz = 34000000;
        panel.width = kWidth;
        panel.height = kHeight;
        panel.hsync_pulse = 24;
        panel.hsync_back_porch = 160;
        panel.hsync_front_porch = 136;
        panel.vsync_pulse = 1;
        panel.vsync_back_porch = 23;
        panel.vsync_front_porch = 11;
        panel.pclk_active_falling = true;
        panel.hsync_pin = 44;
        panel.vsync_pin = 45;
        panel.de_pin = 43;
        panel.pclk_pin = 40;
        panel.disp_pin = -1;
        panel.data_lines = 16;
        constexpr int data_pins[XIAOTAI_PANEL_RGB_DATA_LINES] = {36, 37, 38, 39, 46, 7, 35, 48,
                                                                 49, 50, 51, 4, 5, 0, 1, 6};
        for (size_t i = 0; i < XIAOTAI_PANEL_RGB_DATA_LINES; ++i) panel.data_pins[i] = data_pins[i];
        panel.buffers = 2;
        panel.dma_burst_bytes = 64;

        // INT and RST of the GT911 are not wired; it answers on its primary address.
        const xiaotai_touch_i2c_config_t touch{i2c, 0x5d, kI2c.hz, kWidth, kHeight,
                                               false, false, false, -1, false, -1};
        xiaotai_display_config_t display{};
        auto err = xiaotai_panel_rgb_create(&panel, &display.panel);
        if (err != XIAOTAI_OK) return err;
        err = xiaotai_touch_gt911_create(&touch, &display.touch);
        if (err != XIAOTAI_OK) return err;
        display.desc.width = kWidth;
        display.desc.height = kHeight;
        display.desc.pixel = XIAOTAI_PIXEL_RGB565;
        display.desc.render = XIAOTAI_RENDER_SCANOUT;
        display.desc.buffers = 2;
        display.desc.flags = XIAOTAI_DISPLAY_DIMMABLE;
        display.backlight = backlight;
        return xiaotai_display_create(&display, &display_);
    }

    const xiaotai_audio_input_t* mic_ = nullptr;
    const xiaotai_audio_output_t* speaker_ = nullptr;
    const xiaotai_display_t* display_ = nullptr;
};

}  // namespace

xiaotai::board::Board& xiaotai::board::SelectedBoard()
{
    static Jc1060f570c board;
    return board;
}
