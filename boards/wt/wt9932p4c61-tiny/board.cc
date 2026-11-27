// SPDX-License-Identifier: MIT
/* 启明云端 WT9932P4C61-TINY: an ESP32-P4 with an ESP32-C61 Wi-Fi coprocessor on
 * 4-bit SDIO, an SC2336 camera on MIPI CSI, the BOOT key and an RGB light. The
 * board has no codec, so audio is the silent device (xiaotai/parts/audio.h):
 * calls and the media engine run, nothing is heard. Its optional screen is
 * not used; the board is a door station without a display. */
#include <cinttypes>
#include <cstdio>

#include "xiaotai/board.hpp"
#include "xiaotai/parts/audio.h"
#include "xiaotai/parts/button.h"
#include "xiaotai/parts/camera_csi.h"
#include "xiaotai/parts/coprocessor.h"
#include "xiaotai/port/os.h"
#include "xiaotai_board_data.hpp"

namespace {

constexpr uint16_t kWidth = 1024, kHeight = 600;
constexpr xiaotai_i2c_config_t kI2c{0, 7, 8, 400000, true, 100};
// The camera rail. A reset of the P4 can leave it on with a half-done SCCB
// transfer, so it is switched off first and settles after it is switched on.
constexpr xiaotai_gpio_config_t kCameraPower{0, false, false, false, false, false};
constexpr uint32_t kCameraPowerOffMs = 50, kCameraPowerSettleMs = 150;
constexpr xiaotai_gpio_config_t kKey{35, true, true, false, false, false};
constexpr xiaotai_network_wifi_t kWifi{true};
constexpr const char* kProtocols[]{"2.12.12", "2.12.13", "3.0.7", nullptr};
// EH_PRIV_FIRMWARE_CHIP_ESP32C61 / ESP_CHIP_ID_ESP32C61 in the locked dependencies.
constexpr uint16_t kC61ChipId = 0x14;
constexpr auto kC61 = [] {
    xiaotai_coprocessor_config_t config{};
    config.provisioning = XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL;
    config.wifi = &kWifi;
    config.chip_id = kC61ChipId;
    config.protocols = kProtocols;
    config.link_timeout_ms = 15000;
    return config;
}();

class QimingTiny final : public xiaotai::board::Board {
public:
    xiaotai_err_t Init() override
    {
        xiaotai_i2c_bus_t* i2c = nullptr;
        xiaotai_line_t camera_power{}, key{};
        auto err = xiaotai_line_gpio_output(&kCameraPower, &camera_power);
        if (err == XIAOTAI_OK) err = xiaotai_line_set(&camera_power, false);
        if (err != XIAOTAI_OK) return err;
        xiaotai_delay_ms(kCameraPowerOffMs);
        err = xiaotai_line_set(&camera_power, true);
        if (err != XIAOTAI_OK) return err;
        xiaotai_delay_ms(kCameraPowerSettleMs);

        err = xiaotai_i2c_create(&kI2c, &i2c);
        if (err == XIAOTAI_OK) err = xiaotai_line_gpio_input(&kKey, &key);
        if (err == XIAOTAI_OK) err = CreateAudio();
        const xiaotai_button_config_t button{key, "main", XIAOTAI_BUTTON_DEBOUNCE_MS, false, 0};
        if (err == XIAOTAI_OK) err = xiaotai_button_create(&button, &key_);
        if (err == XIAOTAI_OK) err = CreateCamera(i2c);
        if (err == XIAOTAI_OK) err = xiaotai_coprocessor_create(&kC61, &network_);
        if (err != XIAOTAI_OK) (void)xiaotai_line_set(&camera_power, false);
        return err;
    }

    xiaotai_board_identity_t Identity() const override
    {
        return {XIAOTAI_BOARD_MODEL, XIAOTAI_BOARD_REVISION,
                XIAOTAI_BOARD_PROFILE_MODEL, XIAOTAI_BOARD_PROFILE_CHIP};
    }

    const xiaotai_audio_input_t* AudioInput(size_t i) override { return i == 0 ? mic_ : nullptr; }
    const xiaotai_audio_output_t* AudioOutput(size_t i) override { return i == 0 ? speaker_ : nullptr; }
    const xiaotai_input_t* Input(size_t i) override { return i == 0 ? key_ : nullptr; }
    const xiaotai_camera_t* Camera(size_t i) override { return i == 0 ? camera_ : nullptr; }
    const xiaotai_network_t* Network(size_t i) override { return i == 0 ? network_ : nullptr; }

    void Report(xiaotai_hal_write_fn_t write, void* ctx) override
    {
        xiaotai_camera_csi_report(camera_, write, ctx);
        xiaotai_coprocessor_state_t network{};
        xiaotai_coprocessor_state(network_, &network);
        char line[96];
        std::snprintf(line, sizeof(line), "audio silent; coprocessor stage=%d rule=%u error=%d", network.stage,
                      unsigned(network.rule), network.error);
        write(ctx, line);
    }

private:
    xiaotai_err_t CreateAudio()
    {
        xiaotai_audio_silent_config_t audio{};
        audio.rate_hz = 16000;
        audio.capture_channels = 1;
        audio.capture_roles[0] = XIAOTAI_CHANNEL_MIC;
        audio.playback_channels = 1;
        audio.group = 1;
        return xiaotai_audio_silent_create(&audio, &mic_, &speaker_);
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
    const xiaotai_input_t* key_ = nullptr;
    const xiaotai_camera_t* camera_ = nullptr;
    const xiaotai_network_t* network_ = nullptr;
};

}  // namespace

xiaotai::board::Board& xiaotai::board::SelectedBoard()
{
    static QimingTiny board;
    return board;
}
