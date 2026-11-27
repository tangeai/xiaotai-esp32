// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_c6_ota_recovery.py
// Replaces legacy: tests/regression/test_p4_display_port.py
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "xiaotai/hal.h"
#include "xiaotai/needs.h"
#include "xiaotai/parts/audio.h"
#include "xiaotai/parts/button.h"
#include "xiaotai/parts/camera_csi.h"
#include "xiaotai/parts/coprocessor.h"
#include "xiaotai/parts/panel_st7796.h"
#include "xiaotai/parts/touch_ft5x06.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_board_data.hpp"
#include "xiaotai_bus_host.h"
#include "xiaotai_coprocessor_host.h"

extern "C" {
#include "audio_io_fake.h"
#include "camera_csi_fake.h"
#include "spi_fake.h"
}

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #expr); std::abort(); } \
} while (0)

namespace {
int step = 0;
int fail_at = 0;
constexpr auto kFailure = XIAOTAI_ERR_TIMEOUT;
xiaotai_i2c_bus_t* control_bus = nullptr;
const xiaotai_coprocessor_config_t* maintenance = nullptr;

bool inject(int n)
{
    CHECK(++step == n);
    CHECK(!xiaotai_hal_audio_input(0) && !xiaotai_hal_display(0) && !xiaotai_hal_network(0));
    return step == fail_at;
}
}

extern "C" {
bool __wrap_xiaotai_mem_has_external() { return true; }
bool __wrap_xiaotai_mem_is_external(const void* ptr) { return ptr != nullptr; }

xiaotai_err_t __real_xiaotai_i2c_create(const xiaotai_i2c_config_t*, xiaotai_i2c_bus_t**);
xiaotai_err_t __wrap_xiaotai_i2c_create(const xiaotai_i2c_config_t* cfg, xiaotai_i2c_bus_t** bus)
{
    CHECK(cfg->port == 1 && cfg->sda_pin == 7 && cfg->scl_pin == 8 && cfg->hz == 400000);
    if (inject(1)) return kFailure;
    const auto err = __real_xiaotai_i2c_create(cfg, bus);
    if (err == XIAOTAI_OK) control_bus = *bus;
    return err;
}

xiaotai_err_t __real_xiaotai_line_gpio_output(const xiaotai_gpio_config_t*, xiaotai_line_t*);
xiaotai_err_t __wrap_xiaotai_line_gpio_output(const xiaotai_gpio_config_t* cfg, xiaotai_line_t* line)
{
    CHECK((cfg->pin == 53 || cfg->pin == 27) && !cfg->initially_active);
    CHECK(cfg->active_low == (cfg->pin == 27));
    return inject(cfg->pin == 53 ? 2 : 4) ? kFailure : __real_xiaotai_line_gpio_output(cfg, line);
}

xiaotai_err_t __real_xiaotai_level_pwm(const xiaotai_pwm_config_t*, xiaotai_level_t*);
xiaotai_err_t __wrap_xiaotai_level_pwm(const xiaotai_pwm_config_t* cfg, xiaotai_level_t* level)
{
    CHECK(cfg->pin == 28 && cfg->timer == 1 && cfg->channel == 1 && cfg->hz == 5000);
    CHECK(cfg->resolution_bits == 10 && !cfg->active_low);
    return inject(3) ? kFailure : __real_xiaotai_level_pwm(cfg, level);
}

xiaotai_err_t __real_xiaotai_line_gpio_input(const xiaotai_gpio_config_t*, xiaotai_line_t*);
xiaotai_err_t __wrap_xiaotai_line_gpio_input(const xiaotai_gpio_config_t* cfg, xiaotai_line_t* line)
{
    CHECK(cfg->pin == 35 && cfg->active_low && cfg->pull_up);
    return inject(5) ? kFailure : __real_xiaotai_line_gpio_input(cfg, line);
}

xiaotai_err_t __real_xiaotai_es8311_create(const xiaotai_es8311_config_t*, xiaotai_audio_codec_t**);
xiaotai_err_t __wrap_xiaotai_es8311_create(const xiaotai_es8311_config_t* cfg, xiaotai_audio_codec_t** codec)
{
    CHECK(cfg->bus == control_bus && cfg->address == 0 && cfg->mclk_multiple == 256);
    CHECK(cfg->capture && cfg->playback && cfg->reference && cfg->i2c_hz == 100000);
    return inject(6) ? kFailure : __real_xiaotai_es8311_create(cfg, codec);
}

xiaotai_err_t __real_xiaotai_audio_device_create(const xiaotai_audio_device_config_t*,
                                               const xiaotai_audio_input_t**, const xiaotai_audio_output_t**);
xiaotai_err_t __wrap_xiaotai_audio_device_create(const xiaotai_audio_device_config_t* cfg,
                                               const xiaotai_audio_input_t** in, const xiaotai_audio_output_t** out)
{
    CHECK(cfg->i2s.port == 1 && cfg->i2s.mclk_pin == 13 && cfg->i2s.bclk_pin == 12 && cfg->i2s.ws_pin == 10);
    CHECK(cfg->i2s.din_pin == 11 && cfg->i2s.dout_pin == 9 && cfg->i2s.rx_slots == 2 && cfg->i2s.tx_slots == 2);
    CHECK(cfg->i2s.dma_descriptors == 6 && cfg->i2s.dma_frames == 240);
    CHECK(cfg->capture_codec && cfg->capture_codec == cfg->playback_codec && cfg->mic_inputs == 1);
    CHECK(xiaotai_line_connected(&cfg->amplifier) && cfg->capture_mode == XIAOTAI_AUDIO_CAPTURE_BLOCKING);
    return inject(7) ? kFailure : __real_xiaotai_audio_device_create(cfg, in, out);
}

xiaotai_err_t __real_xiaotai_panel_spi_create(const xiaotai_panel_spi_controller_t*,
                                            const xiaotai_panel_spi_config_t*, xiaotai_panel_t*);
xiaotai_err_t __wrap_xiaotai_panel_spi_create(const xiaotai_panel_spi_controller_t* chip,
                                            const xiaotai_panel_spi_config_t* cfg, xiaotai_panel_t* panel)
{
    CHECK(chip == xiaotai_panel_st7796());
    CHECK(cfg->host == 1 && cfg->mosi_pin == 20 && cfg->sclk_pin == 21 && cfg->cs_pin == 23 && cfg->dc_pin == 26);
    CHECK(cfg->hz == 80000000 && cfg->spi_mode == 0 && cfg->queue_depth == 20);
    CHECK(cfg->external_dma && cfg->wait_done && cfg->bgr && cfg->invert && cfg->mirror_x && cfg->mirror_y);
    CHECK(cfg->native_width == 320 && cfg->native_height == 480 && cfg->swap_xy && !cfg->max_transfer_bytes);
    CHECK(xiaotai_line_connected(&cfg->reset) && cfg->reset_hold_ms == 10 && cfg->reset_settle_ms == 120);
    return inject(8) ? kFailure : __real_xiaotai_panel_spi_create(chip, cfg, panel);
}

xiaotai_err_t __real_xiaotai_touch_ft5x06_create(const xiaotai_touch_i2c_config_t*, xiaotai_touch_t*);
xiaotai_err_t __wrap_xiaotai_touch_ft5x06_create(const xiaotai_touch_i2c_config_t* cfg, xiaotai_touch_t* touch)
{
    CHECK(cfg->bus == control_bus && cfg->hz == 400000 && cfg->width == 320 && cfg->height == 480);
    CHECK(cfg->reset_pin == 29 && !cfg->reset_active_high && cfg->interrupt_pin == 50);
    CHECK(cfg->swap_xy && !cfg->mirror_x && cfg->mirror_y);
    return inject(9) ? kFailure : __real_xiaotai_touch_ft5x06_create(cfg, touch);
}

xiaotai_err_t __real_xiaotai_display_create(const xiaotai_display_config_t*, const xiaotai_display_t**);
xiaotai_err_t __wrap_xiaotai_display_create(const xiaotai_display_config_t* cfg, const xiaotai_display_t** lcd)
{
    CHECK(!cfg->touch_first && !cfg->touch_filter.press_gap_ms && !cfg->touch_filter.hold_px);
    return inject(10) ? kFailure : __real_xiaotai_display_create(cfg, lcd);
}

xiaotai_err_t __real_xiaotai_button_create(const xiaotai_button_config_t*, const xiaotai_input_t**);
xiaotai_err_t __wrap_xiaotai_button_create(const xiaotai_button_config_t* cfg, const xiaotai_input_t** btn)
{
    CHECK(xiaotai_line_connected(&cfg->line) && cfg->debounce_ms == 50 && !cfg->contact);
    return inject(11) ? kFailure : __real_xiaotai_button_create(cfg, btn);
}

xiaotai_err_t __real_xiaotai_camera_csi_create(const xiaotai_camera_csi_config_t*, const xiaotai_camera_t**);
xiaotai_err_t __wrap_xiaotai_camera_csi_create(const xiaotai_camera_csi_config_t* cfg, const xiaotai_camera_t** cam)
{
    CHECK(cfg->sccb == control_bus && cfg->sccb_hz == 100000 && cfg->sensor_address == 0x36);
    CHECK(cfg->buffers == 3 && !cfg->sensor_id && !cfg->fixed_fps && !cfg->full_range);
    CHECK(!xiaotai_line_connected(&cfg->power) && cfg->setup_count == 3);
    return inject(12) ? kFailure : __real_xiaotai_camera_csi_create(cfg, cam);
}

xiaotai_err_t __real_xiaotai_coprocessor_create(const xiaotai_coprocessor_config_t*, const xiaotai_network_t**);
xiaotai_err_t __wrap_xiaotai_coprocessor_create(const xiaotai_coprocessor_config_t* cfg, const xiaotai_network_t** net)
{
    CHECK(!cfg->wifi && cfg->chip_id == 0x0d && !cfg->manual_link && cfg->link_timeout_ms == 15000);
    CHECK(std::strcmp(cfg->protocols[0], "3.0.7") == 0 && !cfg->protocols[1]);
    CHECK(cfg->image->size == 1111904 && cfg->source_count == 3 && !cfg->commands && !cfg->update_projects);
    CHECK(cfg->write_block_bytes == 1024 && cfg->activate_wait_ms == 50 && cfg->legacy_activate_wait_ms == 6000);
    CHECK(cfg->legacy_protocol_capability == 0x0d && cfg->legacy_image_max == 0x180000);
    CHECK(std::strcmp(cfg->legacy.project, "eh_cp_wifi_sta") == 0 && std::strcmp(cfg->legacy.version, "1") == 0);
    if (inject(13)) return kFailure;
    maintenance = cfg;
    return __real_xiaotai_coprocessor_create(cfg, net);
}
}

namespace {

void check_display(const xiaotai_display_t& lcd)
{
    CHECK(lcd.desc.render == XIAOTAI_RENDER_PARTIAL && !lcd.desc.rotation);
    CHECK(lcd.desc.buffer_align == 128 && !lcd.desc.buffers && lcd.desc.transfer_pixels == 480 * 80);
    CHECK(lcd.desc.flags == (XIAOTAI_DISPLAY_SWAP_BYTES | XIAOTAI_DISPLAY_DIMMABLE));
    CHECK(lcd.desc.width == 480 && lcd.desc.height == 320 && lcd.desc.touch);
    xt_spi_fake_reset();
    xiaotai_display_handles_t handles{};
    // Missing FT5x06 at the final stage must release all three SPI resources.
    CHECK(lcd.open(lcd.self, &handles) != XIAOTAI_OK);
    CHECK(!handles.panel && !handles.panel_io);
    const auto* state = xt_spi_fake_state();
    CHECK(state->calls >= 3 && state->log[state->calls - 3] == XT_SPI_PANEL_CLOSE);
    CHECK(state->log[state->calls - 2] == XT_SPI_IO_CLOSE && state->log[state->calls - 1] == XT_SPI_BUS_CLOSE);
    CHECK(xiaotai_bus_host_pwm(1).duty == 0 && xiaotai_bus_host_pin(27).level);
    CHECK(xiaotai_bus_host_i2c_attach(1, 0x38) == XIAOTAI_OK);
    CHECK(lcd.open(lcd.self, &handles) == XIAOTAI_OK);
    CHECK(handles.swap_xy && handles.mirror_x && handles.mirror_y);
    const xiaotai_rect_t full{0, 0, 480, 320};
    const size_t frame_bytes = 320U * 480U * 2U;
    void* pixels = std::aligned_alloc(lcd.desc.buffer_align, frame_bytes);
    CHECK(pixels);
    std::memset(pixels, 0x7e, frame_bytes);
    const auto draws = state->count[XT_SPI_DRAW];
    CHECK(lcd.present(lcd.self, &full, pixels) == XIAOTAI_OK);
    CHECK(state->limit == frame_bytes && !state->queued && state->last[0] == 0x7e);
    CHECK(state->count[XT_SPI_DRAW] == draws + 1 && state->log[state->calls - 1] == XT_SPI_DRAIN);
    const xiaotai_rect_t update{10, 20, 4, 2};
    const auto drains = state->count[XT_SPI_DRAIN];
    xt_spi_fake_fail(XT_SPI_DRAW, 1, kFailure);
    CHECK(lcd.present(lcd.self, &update, pixels) == kFailure);
    CHECK(state->count[XT_SPI_DRAIN] == drains + 1 && !state->queued);
    CHECK(state->draws);
    const auto& drawn = state->drawn[state->draws - 1];
    CHECK(drawn.x == update.x && drawn.y == update.y && drawn.width == update.width && drawn.height == update.height);
    std::free(pixels);
    CHECK(lcd.set_backlight(lcd.self, 50) == XIAOTAI_OK && xiaotai_bus_host_pwm(1).duty == 511);
    CHECK(lcd.set_backlight(lcd.self, 0) == XIAOTAI_OK && xiaotai_bus_host_pwm(1).duty == 0);
}

void check_camera(const xiaotai_camera_t& cam)
{
    csi_fake.powered = true;  // The Waveshare sensor has no switchable rail.
    CHECK(cam.desc.max_fps == 45 && cam.desc.rotation == 3 && cam.desc.row_align == 0);
    CHECK(cam.desc.min_width == cam.desc.max_width && cam.desc.min_height == cam.desc.max_height);
    CHECK(cam.prepare(cam.self) == XIAOTAI_OK && csi_fake.guard && csi_fake.hz == 100000);
    xiaotai_camera_csi_stats_t stats{};
    xiaotai_camera_csi_stats(&cam, &stats);
    CHECK(stats.pool_bytes == 3U * 1280U * 960U * 3U / 2U && csi_fake.start_calls == 1);
    CHECK(cam.set_power(cam.self, true) == XIAOTAI_OK);
    const xiaotai_camera_mode_t mode{1280, 960, XIAOTAI_CAMERA_YUV420_PAIRED, 45};
    csi_fake.reg[0x3a00] = 1;
    csi_fake.reg[0x3c00] = 2;
    csi_fake.reg[0x3c01] = 3;
    csi_fake.reject_reg_write = true;
    CHECK(cam.open(cam.self, &mode) == XIAOTAI_FAIL && !csi_fake.fd && !csi_fake.streaming);
    csi_fake.reject_reg_write = false;
    CHECK(cam.open(cam.self, &mode) == XIAOTAI_OK && csi_fake.streaming && csi_fake.buffer_count == 3);
    CHECK(csi_fake.reg[0x3a00] == 0x21 && csi_fake.reg[0x3c00] == 6 && csi_fake.reg[0x3c01] == 0x83);
    CHECK(cam.close(cam.self) == XIAOTAI_OK && !csi_fake.fd);
    CHECK(csi_fake.calls[csi_fake.call_count - 3] == CSI_STREAM_OFF);
    CHECK(csi_fake.calls[csi_fake.call_count - 2] == CSI_FREE && csi_fake.calls[csi_fake.call_count - 1] == CSI_CLOSE);
    CHECK(cam.set_power(cam.self, false) == XIAOTAI_OK);
}

void check_audio(const xiaotai_audio_input_t& in, const xiaotai_audio_output_t& out)
{
    CHECK(in.info.gain_db == 30 && in.info.gain_min_db == 30 && in.info.gain_max_db == 30);
    CHECK(!in.set_gain_db && in.info.block_frames == 240 && !in.info.read_timeout_ms);
    CHECK(in.info.group == 1 && out.info.group == 1 && out.info.flags == XIAOTAI_AUDIO_OUTPUT_PACED);
    CHECK(out.info.latency_ms == 90 && out.info.enable_ms == 120 && out.info.max_write_frames == 240);
    CHECK(out.desc.roles[0] == XIAOTAI_CHANNEL_LEFT && out.desc.roles[1] == XIAOTAI_CHANNEL_RIGHT);
    CHECK(in.open(in.self) == XIAOTAI_OK && out.open(out.self) == XIAOTAI_OK);
    CHECK(out.flush_frames(out.self) == 1680);
    xt_audio_fake_overflow(1);
    xiaotai_audio_stats_t stats{};
    in.stats(in.self, &stats);
    CHECK(stats.overruns == 1 && stats.lost_frames == 240);
    CHECK(out.set_enabled(out.self, true) == XIAOTAI_OK && xiaotai_bus_host_pin(53).level);
    CHECK(out.close(out.self) == XIAOTAI_OK && !xiaotai_bus_host_pin(53).level);
    CHECK(in.close(in.self) == XIAOTAI_OK);
}

void check_upgrade(const xiaotai_network_t& net, bool approved)
{
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    xiaotai_coprocessor_host_reset();
    auto* fake = xiaotai_coprocessor_host();
    fake->chip = 0x0d;
    fake->major = 3;
    fake->patch = 7;
    fake->app_magic = 0xabcd5432;
    fake->image = maintenance->image->data;
    std::strcpy(fake->project, "eh_cp_wifi_sta");
    std::strcpy(fake->version, "3.0.7-xiaotai.2");
    constexpr uint8_t approved_elf[] = {
        0x15, 0xcc, 0x26, 0x97, 0x81, 0x86, 0x86, 0xa6, 0xa7, 0xa4, 0x12, 0x0a, 0xf6, 0x45, 0x3b, 0xc8,
        0xbd, 0x66, 0x30, 0x95, 0xe6, 0x0e, 0x0c, 0x3a, 0xd1, 0x60, 0x67, 0x7e, 0x1a, 0xbf, 0xf2, 0x43,
    };
    std::memcpy(fake->elf_sha256, approved_elf, sizeof(approved_elf));
    if (!approved) fake->elf_sha256[0] ^= 1;
    CHECK(net.prepare(net.self, false, nullptr, nullptr) == XIAOTAI_OK);
    CHECK(fake->waits == 1 && !fake->starts && fake->last_timeout_ms == 15000);
    uint8_t marker = 0;
    const auto marker_err = xiaotai_kv_get_u8("c6_update", "attempt", &marker, 1000);
    xiaotai_coprocessor_state_t progress{};
    xiaotai_coprocessor_state(&net, &progress);
    if (approved) {
        CHECK(progress.stage == XIAOTAI_NETWORK_RESTARTING && progress.rule == 8);
        CHECK(fake->begins == 1 && fake->writes == 1086 && fake->written == 1111904);
        CHECK(fake->largest_write == 1024 && fake->contiguous && fake->ends == 1 && fake->activations == 1);
        CHECK(marker_err == XIAOTAI_OK && marker == 1);
    } else {
        CHECK(progress.stage == XIAOTAI_NETWORK_READY && progress.rule == 10);
        CHECK(!fake->begins && !fake->writes && marker_err == XIAOTAI_ERR_NOT_FOUND);
    }
}

void check_pins_safe()
{
    CHECK(!xiaotai_bus_host_pin(53).level && xiaotai_bus_host_pwm(1).duty == 0);
    const auto reset = xiaotai_bus_host_pin(27);
    CHECK(!reset.configured || reset.level);
}

}  // namespace

int main(int argc, char** argv)
{
    CHECK(argc == 1 || argc == 2);
    const char* mode = argc == 2 ? argv[1] : "";
    fail_at = std::atoi(mode);
    xiaotai_board_identity_t id{};
    xiaotai_hal_identity(&id);
    CHECK(std::strcmp(id.model, "waveshare-p4-touch-lcd-3.5") == 0 && std::strcmp(id.revision, "rev3.2") == 0);
    CHECK(std::strcmp(id.profile_model, "Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5") == 0);
    CHECK(std::strcmp(id.profile_chip, "ESP32-P4") == 0);
    const auto result = fail_at ? kFailure : XIAOTAI_OK;
    CHECK(xiaotai_hal_init() == result && xiaotai_hal_init() == result);
    CHECK(step == (fail_at ? fail_at : 13));
    CHECK(xt_spi_fake_state()->calls == 0 && !csi_fake.ready && !xiaotai_coprocessor_host()->waits);
    check_pins_safe();
    if (fail_at) {
        CHECK(!xiaotai_hal_audio_input(0) && !xiaotai_hal_audio_output(0) && !xiaotai_hal_display(0));
        CHECK(!xiaotai_hal_input(0) && !xiaotai_hal_camera(0) && !xiaotai_hal_network(0));
        CHECK(xiaotai_hal_needs() == XIAOTAI_NEED_EXTERNAL_RAM);
        return 0;
    }

    CHECK(xiaotai_hal_needs() == XIAOTAI_BOARD_MANIFEST_NEEDS);
    const auto* mic = xiaotai_hal_audio_input(0);
    const auto* spk = xiaotai_hal_audio_output(0);
    const auto* key = xiaotai_hal_input_by_role("main");
    const auto* lcd = xiaotai_hal_display(0);
    const auto* cam = xiaotai_hal_camera(0);
    const auto* net = xiaotai_hal_network(0);
    CHECK(mic && spk && key == xiaotai_hal_input(0) && key && lcd && cam && net);
    CHECK(!xiaotai_hal_audio_input(1) && !xiaotai_hal_audio_output(1) && !xiaotai_hal_input(1));
    CHECK(!xiaotai_hal_display(1) && !xiaotai_hal_camera(1) && !xiaotai_hal_network(1));
    CHECK(!xiaotai_hal_output(0) && !xiaotai_hal_measurement(0));
    CHECK(mic->desc.encoding == XIAOTAI_CODEC_PCM16 && mic->desc.channels == 2);
    CHECK(mic->desc.roles[0] == XIAOTAI_CHANNEL_MIC && mic->desc.roles[1] == XIAOTAI_CHANNEL_REFERENCE);
    CHECK(spk->desc.encoding == XIAOTAI_CODEC_PCM16 && lcd->desc.pixel == XIAOTAI_PIXEL_RGB565);
    CHECK(cam->desc.formats == XIAOTAI_CAMERA_YUV420_PAIRED && key->desc.kind == XIAOTAI_INPUT_BUTTON);
    CHECK(key->desc.events == (XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED));
    CHECK(net->desc.kind == XIAOTAI_NETWORK_WIFI_COPROCESSOR && !net->driver_config);
    CHECK(net->desc.provisioning == (XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL));
    CHECK(net->prepare && net->coprocessor_info && !net->coprocessor_command && !net->coprocessor_update);
    if (std::strcmp(mode, "display") == 0) check_display(*lcd);
    else if (std::strcmp(mode, "camera") == 0) check_camera(*cam);
    else if (std::strcmp(mode, "audio") == 0) check_audio(*mic, *spk);
    else if (std::strcmp(mode, "update") == 0) check_upgrade(*net, true);
    else if (std::strcmp(mode, "unknown") == 0) check_upgrade(*net, false);
    else CHECK(*mode == '\0');

    std::printf("{\"audio_input\":[{\"encoding\":\"pcm16\",\"rate_hz\":%u,\"roles\":[\"mic\",\"reference\"]}],"
                "\"audio_output\":[{\"encoding\":\"pcm16\",\"rate_hz\":%u,\"channels\":%u}],"
                "\"display\":[{\"pixel\":\"rgb565\",\"width\":%u,\"height\":%u,\"touch\":%s}],"
                "\"input\":[{\"kind\":\"button\",\"role\":\"%s\"}],"
                "\"camera\":[{\"format\":\"yuv420_paired\",\"width\":%u,\"height\":%u}],"
                "\"network\":[{\"kind\":\"wifi_coprocessor\",\"provisioning\":[\"portal\",\"serial\"]}]}\n",
                mic->desc.rate_hz, spk->desc.rate_hz, unsigned(spk->desc.channels), unsigned(lcd->desc.width),
                unsigned(lcd->desc.height), lcd->desc.touch ? "true" : "false", key->desc.role,
                unsigned(cam->desc.max_width), unsigned(cam->desc.max_height));
}
