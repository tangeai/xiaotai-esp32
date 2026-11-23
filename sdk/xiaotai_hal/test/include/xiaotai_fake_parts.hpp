// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/board.hpp"
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace xiaotai::test {

struct Pinned {
    Pinned() = default;
    Pinned(const Pinned&) = delete;
    Pinned& operator=(const Pinned&) = delete;
};

enum class CameraOp { Prepare, Reserve, PowerOn, Open, Borrow, Release, Close, PowerOff };
struct CameraCall { CameraOp op; std::thread::id task; size_t sequence; };

struct CameraTrace : Pinned {
    void Add(CameraOp op);
    std::array<CameraCall, 96> calls{};
    size_t count{};
    size_t dropped{};
private:
    std::mutex mutex;
};

struct AudioInput : Pinned {
    explicit AudioInput(xiaotai_audio_desc_t desc);
    const xiaotai_audio_input_t* get() const { return &part; }
    xiaotai_audio_input_t part{};
    xiaotai_err_t open_result{}, read_result{};
    std::deque<std::vector<uint8_t>> queued;
    int8_t gain{};
    std::atomic_uint32_t overruns{}, lost{}, errors{};
    bool opened{};
    std::thread::id reader{};
    std::mutex mutex;
};

struct AudioOutput : Pinned {
    explicit AudioOutput(xiaotai_audio_desc_t desc);
    const xiaotai_audio_output_t* get() const { return &part; }
    xiaotai_audio_output_t part{};
    xiaotai_err_t open_result{}, write_result{};
    size_t flush{}, bytes_written{};
    uint8_t volume{};
    bool opened{}, enabled{}, muted{};
    std::atomic_uint32_t errors{};
    std::thread::id writer{};
    std::mutex mutex;
};

struct Display : Pinned {
    explicit Display(xiaotai_display_desc_t desc);
    const xiaotai_display_t* get() const { return &part; }
    xiaotai_display_t part{};
    xiaotai_err_t open_result{}, present_result{};
    std::array<std::vector<uint8_t>, 2> scanout;
    std::vector<uint8_t> visible;
    std::optional<xiaotai_touch_point_t> touch;
    const uint8_t* showing() const;
    xiaotai_rect_t last_area{};
    unsigned presents{};
    uint8_t backlight{};
    bool opened{};
private:
    int visible_buffer{-1};
};

struct InputSample { uint32_t events; std::string data{}; int32_t level{}; };
struct Input : Pinned {
    explicit Input(xiaotai_input_desc_t desc);
    const xiaotai_input_t* get() const { return &part; }
    xiaotai_input_t part{};
    xiaotai_err_t open_result{};
    std::deque<InputSample> queued;
    bool initially_pressed{};
    int32_t initial_level{};
private:
    bool opened{}, first{}, held{};
    std::string data;
    bool has_data{};
    std::optional<int32_t> level;
    std::thread::id reader{};
};

struct Output : Pinned {
    explicit Output(xiaotai_output_desc_t desc);
    const xiaotai_output_t* get() const { return &part; }
    xiaotai_output_t part{};
    xiaotai_err_t result{};
    unsigned writes{};
    std::vector<uint8_t> pixels;
};

struct Camera : Pinned {
    explicit Camera(xiaotai_camera_desc_t desc, CameraTrace& trace, bool has_prepare = true);
    const xiaotai_camera_t* get() const { return &part; }
    /* A test must keep the frame's bytes valid until they are replaced or released. */
    void Publish(xiaotai_camera_frame_t frame);
    xiaotai_camera_t part{};
    CameraTrace& trace;
    xiaotai_err_t prepare_result{}, reserve_result{}, power_result{}, open_result{};
private:
    bool prepared{}, ready{}, power_called{}, powered{}, attempted_open{}, opened{};
    std::thread::id worker{};
    std::mutex frame_mutex;
    std::optional<xiaotai_camera_frame_t> newest;
    xiaotai_camera_frame_t* loan{};
};

struct Network : Pinned {
    explicit Network(xiaotai_network_desc_t desc, bool with_maintenance = false);
    const xiaotai_network_t* get() const { return &part; }
    xiaotai_network_t part{};
    xiaotai_err_t prepare_result{}, update_result{}, command_result{};
    xiaotai_coprocessor_info_t identity{};
    xiaotai_network_wifi_t wifi{};
    std::vector<xiaotai_network_stage_t> prepare_stages{XIAOTAI_NETWORK_CHECKING, XIAOTAI_NETWORK_READY};
    std::vector<xiaotai_network_stage_t> update_stages{XIAOTAI_NETWORK_WRITING, XIAOTAI_NETWORK_RESTARTING};
    std::string reply, last_command;
    size_t last_update_size{};
    unsigned prepares{}, updates{}, live_queries{};
    bool last_retry{};
};

struct Measurement : Pinned {
    explicit Measurement(xiaotai_measurement_desc_t desc);
    const xiaotai_measurement_t* get() const { return &part; }
    xiaotai_measurement_t part{};
    xiaotai_err_t sample_result{};
    int32_t sample{};
    unsigned samples{};
    /* Injectable time base for testing exact sampling boundaries. No RT callers. */
    int64_t (*clock_us)(){};
private:
    std::mutex mutex;
    std::optional<int64_t> sampled_us;
    int32_t cached{};
};

class FakeBoard final : public board::Board {
public:
    xiaotai_board_identity_t identity{"fixture-board", "test", nullptr, nullptr};
    xiaotai_err_t init_result{};
    std::vector<const xiaotai_audio_input_t*> audio_inputs;
    std::vector<const xiaotai_audio_output_t*> audio_outputs;
    std::vector<const xiaotai_display_t*> displays;
    std::vector<const xiaotai_input_t*> inputs;
    std::vector<const xiaotai_output_t*> outputs;
    std::vector<const xiaotai_camera_t*> cameras;
    std::vector<const xiaotai_network_t*> networks;
    std::vector<const xiaotai_measurement_t*> measurements;
    xiaotai_err_t Init() override { return init_result; }
    xiaotai_board_identity_t Identity() const override { return identity; }
    const xiaotai_audio_input_t* AudioInput(size_t i) override { return at(audio_inputs, i); }
    const xiaotai_audio_output_t* AudioOutput(size_t i) override { return at(audio_outputs, i); }
    const xiaotai_display_t* Display(size_t i) override { return at(displays, i); }
    const xiaotai_input_t* Input(size_t i) override { return at(inputs, i); }
    const xiaotai_output_t* Output(size_t i) override { return at(outputs, i); }
    const xiaotai_camera_t* Camera(size_t i) override { return at(cameras, i); }
    const xiaotai_network_t* Network(size_t i) override { return at(networks, i); }
    const xiaotai_measurement_t* Measurement(size_t i) override { return at(measurements, i); }
private:
    template<class T> static const T* at(const std::vector<const T*>& parts, size_t i)
    { return i < parts.size() ? parts[i] : nullptr; }
};

}  // namespace xiaotai::test
