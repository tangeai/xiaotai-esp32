// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "xiaotai/hal.h"
#include "xiaotai/needs.h"
#include "xiaotai/parts/audio.h"
#include "xiaotai/parts/display.h"
#include "xiaotai/parts/panel_rgb.h"
#include "xiaotai/parts/touch_gt911.h"
#include "xiaotai_board_data.hpp"

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #expr); std::abort(); } \
} while (0)

static unsigned created;

extern "C" {
// The Linux port has no external RAM. Model the module's PSRAM.
bool __wrap_xiaotai_mem_has_external(void) { return true; }

xiaotai_err_t __real_xiaotai_i2c_create(const xiaotai_i2c_config_t*, xiaotai_i2c_bus_t**);
xiaotai_err_t __wrap_xiaotai_i2c_create(const xiaotai_i2c_config_t* c, xiaotai_i2c_bus_t** out)
{
    CHECK(c->port == 0 && c->sda_pin == 2 && c->scl_pin == 3 && c->hz == 400000 && c->pullup);
    ++created;
    return __real_xiaotai_i2c_create(c, out);
}

xiaotai_err_t __real_xiaotai_line_gpio_output(const xiaotai_gpio_config_t*, xiaotai_line_t*);
xiaotai_err_t __wrap_xiaotai_line_gpio_output(const xiaotai_gpio_config_t* c, xiaotai_line_t* out)
{
    // The NS4150 enable, active high, off until playback.
    CHECK(c->pin == 42 && !c->active_low && !c->initially_active);
    ++created;
    return __real_xiaotai_line_gpio_output(c, out);
}

xiaotai_err_t __real_xiaotai_level_pwm(const xiaotai_pwm_config_t*, xiaotai_level_t*);
xiaotai_err_t __wrap_xiaotai_level_pwm(const xiaotai_pwm_config_t* c, xiaotai_level_t* out)
{
    CHECK(c->pin == 52 && c->hz == 20000 && !c->active_low);
    ++created;
    return __real_xiaotai_level_pwm(c, out);
}

xiaotai_err_t __real_xiaotai_es8389_create(const xiaotai_es8389_config_t*, xiaotai_audio_codec_t**);
xiaotai_err_t __wrap_xiaotai_es8389_create(const xiaotai_es8389_config_t* c, xiaotai_audio_codec_t** out)
{
    CHECK(c->bus && c->capture && c->playback && c->reference && c->inputs == 0x03);
    CHECK(c->address == 0 && c->mclk_multiple == 0);
    ++created;
    return __real_xiaotai_es8389_create(c, out);
}

xiaotai_err_t __real_xiaotai_audio_device_create(const xiaotai_audio_device_config_t*,
                                                 const xiaotai_audio_input_t**, const xiaotai_audio_output_t**);
xiaotai_err_t __wrap_xiaotai_audio_device_create(const xiaotai_audio_device_config_t* c,
                                                 const xiaotai_audio_input_t** in, const xiaotai_audio_output_t** out)
{
    const auto& i2s = c->i2s;
    CHECK(i2s.port == 0 && i2s.mclk_pin == -1 && i2s.bclk_pin == 54 && i2s.ws_pin == 47);
    CHECK(i2s.din_pin == 16 && i2s.dout_pin == 56 && i2s.rate_hz == 16000);
    CHECK(i2s.rx_slots == 4 && i2s.tx_slots == 2);
    CHECK(c->capture_codec && c->capture_codec == c->playback_codec && c->amplifier.set);
    CHECK(c->mic_inputs == 0x03 && c->capture_channels == 4 && c->playback_channels == 2);
    ++created;
    return __real_xiaotai_audio_device_create(c, in, out);
}

xiaotai_err_t __real_xiaotai_panel_rgb_create(const xiaotai_panel_rgb_config_t*, xiaotai_panel_t*);
xiaotai_err_t __wrap_xiaotai_panel_rgb_create(const xiaotai_panel_rgb_config_t* c, xiaotai_panel_t* out)
{
    static constexpr int data[16] = {36, 37, 38, 39, 46, 7, 35, 48, 49, 50, 51, 4, 5, 0, 1, 6};
    CHECK(c->width == 1024 && c->height == 600 && c->pixel_clock_hz == 34000000 && c->buffers == 2);
    CHECK(c->hsync_pin == 44 && c->vsync_pin == 45 && c->de_pin == 43 && c->pclk_pin == 40 && c->disp_pin == -1);
    CHECK(c->hsync_pulse == 24 && c->hsync_back_porch == 160 && c->hsync_front_porch == 136);
    CHECK(c->vsync_pulse == 1 && c->vsync_back_porch == 23 && c->vsync_front_porch == 11);
    CHECK(c->pclk_active_falling && c->data_lines == 16 && std::memcmp(c->data_pins, data, sizeof(data)) == 0);
    CHECK(!c->reset.set);
    ++created;
    return __real_xiaotai_panel_rgb_create(c, out);
}

xiaotai_err_t __real_xiaotai_touch_gt911_create(const xiaotai_touch_i2c_config_t*, xiaotai_touch_t*);
xiaotai_err_t __wrap_xiaotai_touch_gt911_create(const xiaotai_touch_i2c_config_t* c, xiaotai_touch_t* out)
{
    CHECK(c->bus && c->address == 0x5d && c->width == 1024 && c->height == 600);
    CHECK(c->reset_pin == -1 && c->interrupt_pin == -1 && !c->swap_xy && !c->mirror_x && !c->mirror_y);
    ++created;
    return __real_xiaotai_touch_gt911_create(c, out);
}

xiaotai_err_t __real_xiaotai_display_create(const xiaotai_display_config_t*, const xiaotai_display_t**);
xiaotai_err_t __wrap_xiaotai_display_create(const xiaotai_display_config_t* c, const xiaotai_display_t** out)
{
    CHECK(c->desc.render == XIAOTAI_RENDER_SCANOUT && c->desc.buffers == 2 && c->backlight.set);
    CHECK(c->desc.flags == XIAOTAI_DISPLAY_DIMMABLE && !c->power_steps);
    ++created;
    return __real_xiaotai_display_create(c, out);
}
}

static void print_manifest(const xiaotai_audio_input_t* mic, const xiaotai_audio_output_t* speaker,
                           const xiaotai_display_t* display)
{
    std::printf("{\"audio_input\":[{\"encoding\":\"pcm16\",\"rate_hz\":%u,\"roles\":[\"mic\",\"reference\",\"mic\",\"unused\"]}],"
                "\"audio_output\":[{\"encoding\":\"pcm16\",\"rate_hz\":%u,\"channels\":%u}],"
                "\"display\":[{\"pixel\":\"rgb565\",\"width\":%u,\"height\":%u,\"touch\":%s}],"
                "\"network\":[{\"kind\":\"wifi\",\"provisioning\":[\"portal\",\"serial\"]}]}\n",
                mic->desc.rate_hz, speaker->desc.rate_hz, unsigned(speaker->desc.channels),
                unsigned(display->desc.width), unsigned(display->desc.height), display->desc.touch ? "true" : "false");
}

int main(int argc, char** argv)
{
    CHECK(argc == 1 || argc == 2);
    const char* mode = argc == 2 ? argv[1] : "parts";
    xiaotai_board_identity_t id{};
    xiaotai_hal_identity(&id);
    CHECK(std::strcmp(id.model, "guition-jc1060f570c") == 0 && std::strcmp(id.revision, "1.0") == 0);
    CHECK(std::strcmp(id.profile_model, "GUITION-JC1060F570C-V1.0") == 0 && std::strcmp(id.profile_chip, "ESP32-S31") == 0);
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    CHECK(created == 8);
    const auto* mic = xiaotai_hal_audio_input(0);
    const auto* speaker = xiaotai_hal_audio_output(0);
    const auto* display = xiaotai_hal_display(0);
    const auto* network = xiaotai_hal_network(0);
    CHECK(mic && speaker && display && network && xiaotai_hal_needs() == XIAOTAI_BOARD_MANIFEST_NEEDS);
    CHECK(!xiaotai_hal_input(0) && !xiaotai_hal_camera(0) && !xiaotai_hal_audio_input(1) && !xiaotai_hal_display(1));
    CHECK(mic->desc.encoding == XIAOTAI_CODEC_PCM16 && mic->desc.channels == 4 && mic->desc.rate_hz == 16000);
    CHECK(mic->desc.roles[0] == XIAOTAI_CHANNEL_MIC && mic->desc.roles[1] == XIAOTAI_CHANNEL_REFERENCE &&
          mic->desc.roles[2] == XIAOTAI_CHANNEL_MIC && mic->desc.roles[3] == XIAOTAI_CHANNEL_UNUSED);
    CHECK(mic->info.group == 1 && mic->info.gain_db == 24 && mic->info.echo_coupling_db == XIAOTAI_AUDIO_COUPLING_UNKNOWN);
    CHECK(speaker->desc.channels == 2 && speaker->desc.roles[0] == XIAOTAI_CHANNEL_LEFT &&
          speaker->desc.roles[1] == XIAOTAI_CHANNEL_RIGHT && speaker->info.group == 1 && speaker->info.enable_ms == 120);
    CHECK(display->desc.pixel == XIAOTAI_PIXEL_RGB565 && display->desc.render == XIAOTAI_RENDER_SCANOUT);
    CHECK(display->desc.width == 1024 && display->desc.height == 600 && display->desc.buffers == 2 && display->desc.touch);
    CHECK(network->desc.kind == XIAOTAI_NETWORK_WIFI && !network->coprocessor_info);
    CHECK(network->desc.provisioning == (XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL));
    if (std::strcmp(mode, "wiring") == 0) return 0;
    CHECK(std::strcmp(mode, "parts") == 0);
    print_manifest(mic, speaker, display);
    return 0;
}
