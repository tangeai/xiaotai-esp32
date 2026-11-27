// SPDX-License-Identifier: MIT
#include <cinttypes>
#include <cstdio>

#include "xiaotai/board.hpp"
#include "xiaotai/parts/audio.h"
#include "xiaotai/parts/button.h"
#include "xiaotai/parts/camera_csi.h"
#include "xiaotai/parts/coprocessor.h"
#include "xiaotai/parts/display.h"
#include "xiaotai/parts/panel_jd9165.h"
#include "xiaotai/parts/touch_gt911.h"
#include "xiaotai/port/os.h"
#include "xiaotai_board_data.hpp"

namespace {

constexpr uint16_t kWidth = 1024, kHeight = 600;
constexpr xiaotai_i2c_config_t kI2c{1, 48, 47, 400000, true, 100};
constexpr xiaotai_gpio_config_t kAmp{53, false, false, false, false, false};
constexpr xiaotai_gpio_config_t kScreenPower{22, false, false, false, false, false};
constexpr xiaotai_gpio_config_t kUpDown{26, false, false, false, false, false};
constexpr xiaotai_gpio_config_t kLeftRight{27, false, false, false, false, false};
constexpr xiaotai_gpio_config_t kSharedReset{23, false, false, false, false, false};
constexpr xiaotai_pwm_config_t kBacklight{20, 1, 1, 5000, 10, false};
constexpr xiaotai_gpio_config_t kControlBusPower{0, false, false, false, false, false};
constexpr uint32_t kControlBusPowerSettleMs = 100;
constexpr xiaotai_gpio_config_t kKey{35, true, true, false, false, false};
constexpr xiaotai_network_wifi_t kWifi{true};
constexpr const char* kProtocols[]{"2.12.13", "3.0.7", nullptr};
constexpr const char* kProjects[]{"xiaotai_wt99p461_c61", "network_adapter", nullptr};
constexpr const char* kCommands[]{"INFO", "RXRESET", "SINK0", "SINK1", "ROLLBACK", nullptr};
constexpr const char* kRestartCommands[]{"ROLLBACK", nullptr};
// EH_PRIV_FIRMWARE_CHIP_ESP32C61 / ESP_CHIP_ID_ESP32C61 in the locked dependencies.
constexpr uint16_t kC61ChipId = 0x14;
constexpr auto kC61 = [] {
    xiaotai_coprocessor_config_t config{};
    config.provisioning = XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL;
    config.wifi = &kWifi;
    config.chip_id = kC61ChipId;
    config.chip_revision = 100;
    config.manual_link = true;
    config.protocols = kProtocols;
    // Do not attach an image: boot is a read-only identity check.
    config.update_projects = kProjects;
    config.update_image_max = 0x180000;
    config.update_block_bytes = 512;
    config.update_restart_wait_ms = 3000;
    config.command_channel = 0x57545331;
    config.commands = kCommands;
    config.restart_commands = kRestartCommands;
    config.restart_command_wait_ms = 4000;
    return config;
}();

class Wt99 final : public xiaotai::board::Board {
public:
    xiaotai_err_t Init() override
    {
        xiaotai_i2c_bus_t* i2c = nullptr;
        xiaotai_line_t amp{}, rail{}, updown{}, shlr{}, reset{}, control_bus_power{}, key{};
        xiaotai_line_t pwm_enable{};
        xiaotai_level_t backlight{};
        auto err = xiaotai_line_gpio_output(&kControlBusPower, &control_bus_power);
        if (err == XIAOTAI_OK) err = xiaotai_line_set(&control_bus_power, true);
        if (err != XIAOTAI_OK) return err;

        // The legacy board settles this shared rail before any control-bus traffic.
        xiaotai_delay_ms(kControlBusPowerSettleMs);
        err = xiaotai_i2c_create(&kI2c, &i2c);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_output(&kAmp, &amp);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_output(&kScreenPower, &rail);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_output(&kUpDown, &updown);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_output(&kLeftRight, &shlr);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_output(&kSharedReset, &reset);
        if (err == XIAOTAI_OK) err = xiaotai_level_pwm_managed(&kBacklight, &backlight, &pwm_enable);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_input(&kKey, &key);

        if (err == XIAOTAI_OK) err = CreateAudio(i2c, amp);
        if (err == XIAOTAI_OK) err = CreateDisplay(i2c, rail, updown, shlr, reset, backlight, pwm_enable);
        const xiaotai_button_config_t button{key, "main", XIAOTAI_BUTTON_DEBOUNCE_MS, false, 0};
        if (err == XIAOTAI_OK) err = xiaotai_button_create(&button, &key_);
        if (err == XIAOTAI_OK) err = CreateCamera(i2c);
        if (err == XIAOTAI_OK) err = xiaotai_coprocessor_create(&kC61, &network_);
        if (err != XIAOTAI_OK) (void)xiaotai_line_set(&control_bus_power, false);
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
    const xiaotai_input_t* Input(size_t i) override { return i == 0 ? key_ : nullptr; }
    const xiaotai_camera_t* Camera(size_t i) override { return i == 0 ? camera_ : nullptr; }
    const xiaotai_network_t* Network(size_t i) override { return i == 0 ? network_ : nullptr; }

    void Report(xiaotai_hal_write_fn_t write, void* ctx) override
    {
        xiaotai_audio_device_report(mic_, write, ctx);
        xiaotai_camera_csi_report(camera_, write, ctx);
        xiaotai_display_part_stats_t display{};
        xiaotai_display_part_stats(display_, &display);
        xiaotai_coprocessor_state_t network{};
        xiaotai_coprocessor_state(network_, &network);
        char line[128];
        std::snprintf(line, sizeof(line), "display opens=%" PRIu32 " failures=%" PRIu32
                      " touch_errors=%" PRIu32 " latched=%u error=%d", display.opens,
                      display.open_failures, display.touch_errors, unsigned(display.latched), display.last_error);
        write(ctx, line);
        std::snprintf(line, sizeof(line), "coprocessor stage=%d rule=%u error=%d", network.stage,
                      unsigned(network.rule), network.error);
        write(ctx, line);
    }

private:
    xiaotai_err_t CreateAudio(xiaotai_i2c_bus_t* i2c, const xiaotai_line_t& amp)
    {
        xiaotai_es8311_config_t es8311{};
        es8311.bus = i2c;
        es8311.mclk_multiple = 256;
        es8311.capture = es8311.playback = es8311.reference = true;
        es8311.i2c_hz = 100000;
        xiaotai_audio_codec_t* codec = nullptr;
        auto err = xiaotai_es8311_create(&es8311, &codec);
        if (err != XIAOTAI_OK) return err;
        xiaotai_audio_device_config_t audio{};
        audio.i2s = {1, 8, 7, 10, 11, 9, 16000, 256, 2, 2, 6, 240, XIAOTAI_I2S_SLOTS, XIAOTAI_I2S_SLOTS};
        audio.group = 1;
        audio.capture_codec = codec;
        audio.capture_channels = 2;
        audio.capture_roles[0] = XIAOTAI_CHANNEL_MIC;
        audio.capture_roles[1] = XIAOTAI_CHANNEL_REFERENCE;
        audio.input_gain_db[0] = audio.gain_min_db = audio.gain_max_db = 30;
        audio.mic_inputs = 1;
        audio.echo_coupling_db = XIAOTAI_AUDIO_COUPLING_UNKNOWN;
        audio.capture_mode = XIAOTAI_AUDIO_CAPTURE_BLOCKING;
        audio.playback_codec = codec;
        audio.playback_channels = 2;
        audio.max_write_frames = 240;
        audio.amplifier = amp;
        audio.amplifier_enable_ms = 120;
        return xiaotai_audio_device_create(&audio, &mic_, &speaker_);
    }

    xiaotai_err_t CreateDisplay(xiaotai_i2c_bus_t* i2c, const xiaotai_line_t& rail,
                                const xiaotai_line_t& updown, const xiaotai_line_t& shlr,
                                const xiaotai_line_t& reset, const xiaotai_level_t& backlight,
                                const xiaotai_line_t& pwm_enable)
    {
        xiaotai_line_t phy{};
        auto err = xiaotai_panel_jd9165_phy_line(3, 2500, &phy);
        if (err != XIAOTAI_OK) return err;
        xiaotai_display_config_t display{};
        const xiaotai_panel_jd9165_config_t panel{2, 600, -1, 0, 40000000, kWidth, kHeight,
                                                  136, 24, 160, 21, 2, 12, 2, {}, 0, 0};
        err = xiaotai_panel_jd9165_create(&panel, &display.panel);
        if (err != XIAOTAI_OK) return err;
        const xiaotai_touch_i2c_config_t touch{i2c, 0x5d, 400000, kWidth, kHeight,
                                               false, false, false, kSharedReset.pin, true, 21};
        err = xiaotai_touch_gt911_create(&touch, &display.touch);
        if (err != XIAOTAI_OK) return err;
        display.desc.width = kWidth;
        display.desc.height = kHeight;
        display.desc.pixel = XIAOTAI_PIXEL_RGB565;
        display.desc.render = XIAOTAI_RENDER_SCANOUT;
        display.desc.buffers = 2;
        display.desc.flags = XIAOTAI_DISPLAY_DIMMABLE;
        display.backlight = backlight;
        display.touch_first = true;
        display.power[0] = {rail, true, 0};
        display.power[1] = {updown, true, 0};
        display.power[2] = {shlr, true, 0};
        display.power[3] = {reset, true, 20};
        display.power[4] = {phy, true, 0};
        display.power[5] = {pwm_enable, true, 0};
        display.power_steps = 6;
        display.backlight_managed_in_power_steps = true;
        display.stop_on_rollback_error = true;
        // Scan straps and the reset level are not released by the old BSP.
        display.power_rollback[0] = 4;
        display.power_rollback[1] = 5;
        display.power_rollback[2] = 0;
        display.power_rollback_steps = 3;
        display.bind_before_commit = true;
        return xiaotai_display_create(&display, &display_);
    }

    xiaotai_err_t CreateCamera(xiaotai_i2c_bus_t* i2c)
    {
        xiaotai_camera_csi_config_t camera{};
        camera.sccb = i2c;
        camera.sensor_address = 0x30;
        camera.sensor_id = 0xcb3a;
        camera.sensor_id_reg = 0x3107;
        camera.width = kWidth;
        camera.height = kHeight;
        camera.fps = 30;
        camera.fixed_fps = true;
        camera.row_align = 16;
        camera.full_range = true;
        camera.buffers = 3;
        camera.keep_powered = true;
        camera.sccb_hz = kI2c.hz;
        return xiaotai_camera_csi_create(&camera, &camera_);
    }

    const xiaotai_audio_input_t* mic_ = nullptr;
    const xiaotai_audio_output_t* speaker_ = nullptr;
    const xiaotai_display_t* display_ = nullptr;
    const xiaotai_input_t* key_ = nullptr;
    const xiaotai_camera_t* camera_ = nullptr;
    const xiaotai_network_t* network_ = nullptr;
};

}  // namespace

xiaotai::board::Board& xiaotai::board::SelectedBoard()
{
    static Wt99 board;
    return board;
}
