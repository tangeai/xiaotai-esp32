// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_board_camera_port.py
#include "xiaotai_fake_parts.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>

using namespace xiaotai::test;
static xiaotai_audio_desc_t audio(xiaotai_codec_t codec)
{
    xiaotai_audio_desc_t result{};
    result.encoding = codec;
    result.rate_hz = 16000;
    result.channels = 1;
    return result;
}

static void audio_io()
{
    AudioInput mic(audio(XIAOTAI_CODEC_PCM16));
    const auto* in = mic.get();
    size_t written = 42;
    uint8_t frame[4]{};
    assert(in->read(in->self, frame, 4, &written) == XIAOTAI_ERR_INVALID_STATE && !written);
    mic.open_result = XIAOTAI_ERR_NO_MEM;
    assert(in->open(in->self) == XIAOTAI_ERR_NO_MEM && !mic.opened);
    mic.open_result = XIAOTAI_OK;
    assert(in->open(in->self) == XIAOTAI_OK);
    assert(in->read(in->self, frame, 4, &written) == XIAOTAI_ERR_TIMEOUT);
    mic.queued.push_back({1, 2, 3, 4, 5, 6});
    assert(in->read(in->self, frame, 3, &written) == XIAOTAI_ERR_INVALID_SIZE);
    auto other_reader = std::thread([&] {
        assert(in->read(in->self, frame, 4, &written) == XIAOTAI_ERR_INVALID_STATE);
    });
    other_reader.join();
    assert(in->read(in->self, frame, 4, &written) == XIAOTAI_OK && written == 4 && frame[3] == 4);
    assert(in->read(in->self, frame, 2, &written) == XIAOTAI_OK && written == 2 && frame[0] == 5);
    mic.part.info.gain_max_db = 20;
    mic.part.info.gain_step_db = 2;
    assert(in->set_gain_db(in->self, 9) == XIAOTAI_OK && mic.gain == 8);
    assert(in->set_gain_db(in->self, 21) == XIAOTAI_ERR_INVALID_ARG && mic.gain == 8);
    mic.queued.push_back({1, 2});
    assert(in->close(in->self) == XIAOTAI_OK && mic.queued.empty());

    AudioInput encoded(audio(XIAOTAI_CODEC_OPUS));
    in = encoded.get();
    assert(in->open(in->self) == XIAOTAI_OK);
    encoded.queued.push_back({1, 2, 3, 4, 5});
    encoded.queued.push_back({21, 22, 23});
    assert(in->read(in->self, frame, 4, &written) == XIAOTAI_ERR_INVALID_SIZE && written == 0);
    assert(in->read(in->self, frame, 4, &written) == XIAOTAI_OK && written == 3 && frame[0] == 21);
    xiaotai_audio_stats_t st{};
    in->stats(in->self, &st);
    assert(st.lost_frames == 1);

    AudioOutput speaker(audio(XIAOTAI_CODEC_PCM16));
    const auto* out = speaker.get();
    assert(out->write(out->self, frame, sizeof(frame)) == XIAOTAI_ERR_INVALID_STATE);
    speaker.part.info.max_write_frames = 1;
    assert(out->open(out->self) == XIAOTAI_OK && !speaker.enabled);
    assert(out->set_enabled(out->self, true) == XIAOTAI_OK && speaker.enabled);
    assert(out->set_volume(out->self, 101) == XIAOTAI_ERR_INVALID_ARG);
    assert(out->set_volume(out->self, 45) == XIAOTAI_OK && speaker.volume == 45);
    assert(out->set_muted(out->self, true) == XIAOTAI_OK && speaker.muted);
    assert(out->write(out->self, frame, 4) == XIAOTAI_ERR_INVALID_SIZE);
    speaker.write_result = XIAOTAI_FAIL;
    assert(out->write(out->self, frame, 2) == XIAOTAI_FAIL && !speaker.bytes_written);
    speaker.write_result = XIAOTAI_OK;
    assert(out->write(out->self, frame, 2) == XIAOTAI_OK && speaker.bytes_written == 2);
    assert(out->close(out->self) == XIAOTAI_OK && !speaker.opened && !speaker.enabled);
    out->stats(out->self, &st);
    assert(st.errors == 1);
}

static void display_io()
{
    xiaotai_display_desc_t info{};
    info.width = 4;
    info.height = 2;
    info.pixel = XIAOTAI_PIXEL_RGB565;
    info.rotation = 1;
    info.touch = true;
    info.transfer_pixels = 1;
    Display screen(info);
    const auto* p = screen.get();
    xiaotai_rect_t area{.x = 0, .y = 1, .width = 2, .height = 1};
    uint8_t pixels[]{1, 2, 3, 4};
    assert(p->present(p->self, &area, pixels) == XIAOTAI_ERR_INVALID_STATE);
    screen.open_result = XIAOTAI_FAIL;
    assert(p->open(p->self, nullptr) == XIAOTAI_FAIL && !screen.opened);
    screen.open_result = XIAOTAI_OK;
    assert(p->open(p->self, nullptr) == XIAOTAI_OK && screen.backlight == 0);
    assert(p->present(p->self, &area, pixels) == XIAOTAI_OK && screen.presents == 1);
    assert(!std::memcmp(screen.showing() + 4, pixels, sizeof(pixels)));
    pixels[0] = 9;
    assert(screen.showing()[4] == 1);
    xiaotai_touch_point_t point{};
    size_t count = 33;
    assert(p->read_touch(p->self, &point, 1, &count) == XIAOTAI_OK && count == 0);
    screen.touch = xiaotai_touch_point_t{1, 3};
    assert(p->read_touch(p->self, &point, 1, &count) == XIAOTAI_OK && count == 1 && point.y == 3);

    info.render = XIAOTAI_RENDER_SCANOUT;
    info.buffers = 2;
    Display wide(info);
    p = wide.get();
    xiaotai_display_handles_t handles{}, reread{};
    assert(p->open(p->self, &handles) == XIAOTAI_OK);
    assert(p->open(p->self, &reread) == XIAOTAI_OK && handles.framebuffers[1] == reread.framebuffers[1]);
    assert(handles.framebuffers[0] && handles.framebuffers[1] && !handles.panel);
    assert(p->present(p->self, &area, handles.framebuffers[0]) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_rect_t whole{0, 0, 2, 4};
    assert(p->present(p->self, &whole, handles.framebuffers[1]) == XIAOTAI_OK);
    assert(wide.showing() == handles.framebuffers[1]);
}

static void controls()
{
    Input card({.kind = XIAOTAI_INPUT_CARD, .role = "card", .events = XIAOTAI_INPUT_DATA,
                .level_min = 0, .level_max = 0});
    const auto* in = card.get();
    char text[XIAOTAI_INPUT_TEXT_MAX];
    assert(in->read(in->self, text, sizeof(text)) == XIAOTAI_ERR_INVALID_STATE);
    assert(in->open(in->self) == XIAOTAI_OK);
    card.queued.push_back({XIAOTAI_INPUT_DATA, "card-12"});
    assert(in->read(in->self, text, sizeof(text)) == XIAOTAI_ERR_NOT_FOUND);
    assert(in->poll(in->self) == XIAOTAI_INPUT_DATA);
    assert(in->read(in->self, text, 2) == XIAOTAI_ERR_INVALID_SIZE);
    assert(in->read(in->self, text, sizeof(text)) == XIAOTAI_OK && !std::strcmp(text, "card-12"));
    assert(in->read(in->self, text, sizeof(text)) == XIAOTAI_ERR_NOT_FOUND);
    card.queued.push_back({XIAOTAI_INPUT_DATA, "discard"});
    assert(in->poll(in->self) == XIAOTAI_INPUT_DATA && in->poll(in->self) == 0);
    assert(in->read(in->self, text, sizeof(text)) == XIAOTAI_ERR_NOT_FOUND);

    Input slider({.kind = XIAOTAI_INPUT_SLIDER, .role = "slider", .events = XIAOTAI_INPUT_LEVEL,
                  .level_min = 0, .level_max = 100});
    in = slider.get();
    slider.initial_level = 33;
    int32_t value = 0;
    assert(in->open(in->self) == XIAOTAI_OK);
    assert(in->read_level(in->self, &value) == XIAOTAI_ERR_NOT_FOUND);
    assert(in->poll(in->self) == XIAOTAI_INPUT_LEVEL && in->read_level(in->self, &value) == XIAOTAI_OK && value == 33);
    slider.queued.push_back({XIAOTAI_INPUT_LEVEL, "", 14});
    assert(in->poll(in->self) == XIAOTAI_INPUT_LEVEL && in->read_level(in->self, &value) == XIAOTAI_OK && value == 14);

    Input key({.kind = XIAOTAI_INPUT_BUTTON, .role = "main", .events = XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED,
               .level_min = 0, .level_max = 0});
    key.initially_pressed = true;
    in = key.get();
    assert(in->open(in->self) == XIAOTAI_OK && in->poll(in->self) == 0);
    key.queued.push_back({XIAOTAI_INPUT_RELEASED});
    key.queued.push_back({XIAOTAI_INPUT_PRESSED});
    assert(in->poll(in->self) == 0 && in->poll(in->self) == XIAOTAI_INPUT_PRESSED);

    Output strip({XIAOTAI_OUTPUT_RGB_LIGHT, "status", 2});
    const auto* out = strip.get();
    uint8_t rgb[]{1, 2, 3, 4, 5, 6};
    assert(out->set(out->self, 7, 8, 9) == XIAOTAI_OK && strip.pixels[4] == 8);
    assert(out->write(out->self, rgb, 1) == XIAOTAI_ERR_INVALID_ARG);
    assert(out->write(out->self, rgb, 2) == XIAOTAI_OK && strip.pixels[5] == 6);
    assert(strip.writes == 2);
}

static void camera_io()
{
    CameraTrace trace;
    xiaotai_camera_desc_t desc{};
    desc.formats = XIAOTAI_CAMERA_YUV422;
    desc.min_width = desc.max_width = 2;
    desc.min_height = desc.max_height = 2;
    Camera camera(desc, trace);
    const auto* p = camera.get();
    camera.prepare_result = XIAOTAI_ERR_NO_MEM;
    const xiaotai_camera_mode_t mode{2, 2, XIAOTAI_CAMERA_YUV422, 120};
    xiaotai_camera_frame_t loan{}, second{};
    uint8_t frame[]{1, 2, 3, 4, 5, 6, 7, 8};
    assert(p->prepare(p->self) == XIAOTAI_ERR_NO_MEM);
    assert(p->set_power(p->self, true) == XIAOTAI_OK);
    assert(p->open(p->self, &mode) == XIAOTAI_OK);
    assert(p->borrow(p->self, &loan) == XIAOTAI_ERR_NOT_FOUND);
    camera.Publish({frame, sizeof(frame), 2, 2, 2, mode.format, 0, 1, 0});
    camera.Publish({frame, sizeof(frame), 2, 2, 2, mode.format, 0, 2, 0});
    assert(p->borrow(p->self, &loan) == XIAOTAI_OK && loan.sequence == 2 && loan.skipped == 1);
    assert(p->borrow(p->self, &second) == XIAOTAI_ERR_INVALID_STATE);
    assert(p->close(p->self) == XIAOTAI_ERR_INVALID_STATE);
    second = loan;
    assert(p->release(p->self, &second) == XIAOTAI_ERR_INVALID_STATE);
    assert(p->release(p->self, &loan) == XIAOTAI_OK && !loan.data);
    assert(p->close(p->self) == XIAOTAI_OK);
    assert(p->set_power(p->self, false) == XIAOTAI_OK);
    assert(trace.count == 12 && trace.calls[0].op == CameraOp::Prepare &&
           trace.calls[1].op == CameraOp::PowerOn && trace.calls[2].op == CameraOp::Reserve &&
           trace.calls[3].op == CameraOp::Open && trace.calls[11].op == CameraOp::PowerOff);
    for (size_t i = 0; i < trace.count; ++i)
        assert(trace.calls[i].sequence == i + 1 && trace.calls[i].task == std::this_thread::get_id());
    assert(p->prepare(p->self) == XIAOTAI_ERR_INVALID_STATE);
    assert(p->set_power(p->self, true) == XIAOTAI_OK);
    camera.open_result = XIAOTAI_ERR_NOT_SUPPORTED;
    assert(p->open(p->self, &mode) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(p->set_power(p->self, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(p->close(p->self) == XIAOTAI_OK && p->set_power(p->self, false) == XIAOTAI_OK);

    Camera bare(desc, trace, false);
    assert(!bare.get()->prepare && bare.part.set_power(bare.part.self, true) == XIAOTAI_OK);
    assert(trace.calls[trace.count - 1].op == CameraOp::Reserve);
}

static int64_t sample_time{};
static void network_sensor()
{
    Network wifi({XIAOTAI_NETWORK_WIFI_COPROCESSOR, XIAOTAI_PROVISION_SERIAL}, true);
    auto* n = wifi.get();
    wifi.wifi.tx_buffers_on_demand = true;
    assert(static_cast<const xiaotai_network_wifi_t*>(n->driver_config)->tx_buffers_on_demand);
    std::vector<xiaotai_network_stage_t> progress;
    auto collect = [](xiaotai_network_stage_t stage, uint8_t, void* ctx) {
        static_cast<std::vector<xiaotai_network_stage_t>*>(ctx)->push_back(stage);
    };
    assert(n->prepare(n->self, true, collect, &progress) == XIAOTAI_OK && wifi.last_retry);
    assert(progress == wifi.prepare_stages && progress.back() == XIAOTAI_NETWORK_READY);
    wifi.prepare_result = XIAOTAI_FAIL;
    progress.clear();
    assert(n->prepare(n->self, false, collect, &progress) == XIAOTAI_FAIL && !wifi.last_retry);
    assert(progress.size() == 1 && progress[0] == XIAOTAI_NETWORK_CHECKING);
    progress.clear();
    uint8_t fw[]{2, 3, 5};
    assert(n->coprocessor_update(n->self, fw, sizeof(fw), collect, &progress) == XIAOTAI_OK &&
           wifi.last_update_size == sizeof(fw) && progress.back() == XIAOTAI_NETWORK_RESTARTING);
    char reply[4];
    wifi.reply = "12345";
    assert(n->coprocessor_command(n->self, "status", reply, sizeof(reply)) == XIAOTAI_OK &&
           !std::strcmp(reply, "123") && wifi.last_command == "status");
    xiaotai_coprocessor_info_t version;
    assert(n->coprocessor_info(n->self, false, &version) == XIAOTAI_ERR_NOT_FOUND);
    std::strcpy(wifi.identity.version, "test-1");
    assert(n->coprocessor_info(n->self, true, &version) == XIAOTAI_OK &&
           wifi.live_queries == 1 && !std::strcmp(version.version, "test-1"));

    Measurement sensor({XIAOTAI_MEASURE_TEMPERATURE, "room", 10});
    sensor.clock_us = [] { return sample_time; };
    sensor.sample = 20002;
    const auto* m = sensor.get();
    int32_t read{};
    sensor.sample_result = XIAOTAI_ERR_NOT_FOUND;
    assert(m->read(m->self, &read) == XIAOTAI_ERR_NOT_FOUND);
    sensor.sample_result = XIAOTAI_OK;
    assert(m->read(m->self, &read) == XIAOTAI_OK && read == 20002);
    sensor.sample = 20712;
    sample_time = 9999;
    assert(m->read(m->self, &read) == XIAOTAI_OK && read == 20002 && sensor.samples == 2);
    sample_time = 10000;
    assert(m->read(m->self, &read) == XIAOTAI_OK && read == 20712 && sensor.samples == 3);
    FakeBoard board;
    board.networks.push_back(n);
    board.measurements.push_back(m);
    assert(board.Init() == XIAOTAI_OK && board.Network(0) == n && !board.Network(1));
    assert(board.Measurement(0) == m && !board.Display(0));
}

int main()
{
    audio_io();
    display_io();
    controls();
    camera_io();
    network_sensor();
}
