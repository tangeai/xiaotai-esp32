// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_wt_display_cleanup.py
// Replaces legacy: tests/regression/test_p4_display_port.py
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "xiaotai/hal.h"
#include "xiaotai/parts/coprocessor.h"
#include "xiaotai_bus_host.h"
#include "xiaotai_coprocessor_host.h"
#include "xiaotai_board_data.hpp"
extern "C" {
#include "camera_csi_fake.h"
#include "panel_io.h"
#include "touch_io.h"
}

#ifdef WT99_UI_BIND_PROBE
#include <cstdio>
#include "xiaotai/parts/display.h"
#include "wt99_display_bind_test.h"
static unsigned shared_power_writes;
#endif

namespace {

enum Event {
    Rail, UpDown, LeftRight, Reset, Phy, Pwm, TouchIo, Touch,
    Dsi, Dbi, Panel, Enable, Framebuffers, Delay20,
};
struct Op { Event event; bool active; };
bool operator==(Op a, Op b) { return a.event == b.event && a.active == b.active; }

std::vector<Op> journal;
unsigned start_index{}, stop_index{}, fail_start{}, fail_stop{};
bool recording{}, fake_psram = true;
bool watch_control_power{}, control_power_settled{}, control_bus_opened{};

xiaotai_err_t mark(Event event, bool active)
{
    if (!recording) return XIAOTAI_OK;
    journal.push_back({event, active});
    if (event == Delay20) return XIAOTAI_OK;
    if (active) return ++start_index == fail_start ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
    return ++stop_index == fail_stop ? XIAOTAI_FAIL : XIAOTAI_OK;
}

constexpr std::array<Op, 14> start_order{{
    {Rail, true}, {UpDown, true}, {LeftRight, true}, {Reset, true}, {Delay20, true},
    {Phy, true}, {Pwm, true}, {TouchIo, true}, {Touch, true}, {Dsi, true},
    {Dbi, true}, {Panel, true}, {Enable, true}, {Framebuffers, true},
}};
constexpr std::array<Event, 8> release_order{Panel, Dbi, Dsi, Touch, TouchIo, Phy, Pwm, Rail};
alignas(64) std::array<std::array<uint16_t, 1024 * 600>, 2> frame_memory{};
unsigned showing{};
char phy, dsi, dbi, panel, touch_io, touch;

template <Event event> xiaotai_err_t opened(void* object, void** handle)
{
    const auto result = mark(event, true);
    if (result == XIAOTAI_OK) *handle = object;
    return result;
}

template <Event event, char* object> xiaotai_err_t release(void* handle)
{
    assert(handle == object);
    return mark(event, false);
}

xiaotai_err_t phy_open(const xiaotai_panel_jd9165_config_t* c, void** out)
{
    assert(c->phy_ldo_channel == 3 && c->phy_ldo_mv == 2500);
    return opened<Phy>(&phy, out);
}

xiaotai_err_t dsi_open(const xiaotai_panel_jd9165_config_t* c, void** out)
{
    assert(c->phy_ldo_channel == -1 && c->lanes == 2 && c->lane_mbps == 600 && !c->reset.set);
    assert(!xiaotai_bus_host_pin(23).level);
    return opened<Dsi>(&dsi, out);
}

xiaotai_err_t dbi_open(void* bus, void** out)
{
    assert(bus == &dsi);
    return opened<Dbi>(&dbi, out);
}

xiaotai_err_t panel_open(const xiaotai_panel_jd9165_config_t* c, void* bus, void* io, void** out)
{
    assert(bus == &dsi && io == &dbi && c->buffers == 2 && c->pixel_clock_hz == 40000000);
    assert(c->width == 1024 && c->height == 600);
    assert(c->hsync_back_porch == 136 && c->hsync_pulse == 24 && c->hsync_front_porch == 160);
    assert(c->vsync_back_porch == 21 && c->vsync_pulse == 2 && c->vsync_front_porch == 12);
    return opened<Panel>(&panel, out);
}

xiaotai_err_t panel_enable(void* handle, bool soft_reset)
{
    assert(handle == &panel && soft_reset && !xiaotai_bus_host_pin(23).level);
    return mark(Enable, true);
}

xiaotai_err_t panel_frames(void* handle, uint8_t count, void* out[2])
{
    assert(handle == &panel && count == 2);
    const auto result = mark(Framebuffers, true);
    if (result == XIAOTAI_OK) {
        out[0] = frame_memory[0].data();
        out[1] = frame_memory[1].data();
    }
    return result;
}

xiaotai_err_t panel_present(void* handle, const xiaotai_rect_t* area, const void* pixels)
{
    assert(handle == &panel && area->width == 1024 && area->height == 600);
    for (unsigned i = 0; i < frame_memory.size(); ++i) {
        if (pixels == frame_memory[i].data()) { showing = i; return XIAOTAI_OK; }
    }
    std::memcpy(frame_memory[showing].data(), pixels, sizeof(frame_memory[showing]));
    return XIAOTAI_OK;
}

xiaotai_err_t touch_io_open(const xiaotai_touch_i2c_config_t* c, void** out)
{
    xiaotai_i2c_config_t bus{};
    assert(xiaotai_bus_host_i2c_port(1, &bus) && bus.hz == 400000);
    assert(c->bus && c->address == 0x5d && c->hz == 400000 && c->width == 1024 && c->height == 600);
    assert(c->reset_pin == 23 && c->reset_active_high && c->interrupt_pin == 21);
    assert(!c->swap_xy && !c->mirror_x && !c->mirror_y);
    assert(xiaotai_bus_host_pin(23).level);
    auto pwm = xiaotai_bus_host_pwm(1);
    assert(pwm.configured && pwm.pin == 20 && pwm.timer == 1 && pwm.hz == 5000 &&
           pwm.resolution_bits == 10 && pwm.duty == 0);
    return opened<TouchIo>(&touch_io, out);
}

xiaotai_err_t touch_open(const xiaotai_touch_i2c_config_t* c, void* io, void** out)
{
    assert(io == &touch_io && xiaotai_bus_host_pin(c->reset_pin).level);
    const auto result = opened<Touch>(&touch, out);
    if (result != XIAOTAI_OK) return result;
    // The GT911 controller, never the panel, releases the shared reset.
    const xiaotai_gpio_config_t pin{c->reset_pin, false, false, false, false, false};
    xiaotai_line_t line{};
    return xiaotai_line_gpio_output(&pin, &line);
}

xiaotai_err_t touch_read(void* handle, xiaotai_touch_point_t* points, size_t capacity, size_t* count)
{
    assert(handle == &touch && capacity);
    points[0] = {21, 33};
    *count = 1;
    return XIAOTAI_OK;
}

void descriptors()
{
    xiaotai_board_identity_t identity{};
    xiaotai_hal_identity(&identity);
    assert(std::string_view(identity.model) == "wt99p461-s1" && std::string_view(identity.revision) == "1V1");
    assert(std::string_view(identity.profile_model) == "WT99P461-S1-1V1" &&
           std::string_view(identity.profile_chip) == "ESP32-P4");
    assert(xiaotai_hal_needs() == XIAOTAI_BOARD_MANIFEST_NEEDS);
    assert(!xiaotai_hal_output(0) && !xiaotai_hal_input(1) && !xiaotai_hal_camera(1) &&
           !xiaotai_hal_display(1) && !xiaotai_hal_audio_input(1) && !xiaotai_hal_audio_output(1) &&
           !xiaotai_hal_network(1) && !xiaotai_hal_measurement(0));
    const auto* mic = xiaotai_hal_audio_input(0);
    const auto* speaker = xiaotai_hal_audio_output(0);
    assert(mic->desc.encoding == XIAOTAI_CODEC_PCM16 && mic->desc.rate_hz == 16000 && mic->desc.channels == 2);
    assert(mic->desc.roles[0] == XIAOTAI_CHANNEL_MIC && mic->desc.roles[1] == XIAOTAI_CHANNEL_REFERENCE);
    assert(mic->info.gain_db == 30 && !mic->set_gain_db && mic->info.block_frames == 240);
    assert(speaker->desc.encoding == XIAOTAI_CODEC_PCM16 && speaker->desc.rate_hz == 16000 &&
           speaker->desc.channels == 2 && speaker->desc.roles[0] == XIAOTAI_CHANNEL_LEFT &&
           speaker->desc.roles[1] == XIAOTAI_CHANNEL_RIGHT);
    assert(mic->info.group == 1 && speaker->info.group == 1 && speaker->info.latency_ms == 90 &&
           speaker->info.enable_ms == 120 && speaker->info.max_write_frames == 240 &&
           speaker->info.flags == XIAOTAI_AUDIO_OUTPUT_PACED);
    assert(speaker->flush_frames(speaker->self) == 0 && speaker->open(speaker->self) == XIAOTAI_OK);
    assert(speaker->flush_frames(speaker->self) == 1680);
    assert(speaker->set_enabled(speaker->self, true) == XIAOTAI_OK && xiaotai_bus_host_pin(53).level);
    assert(speaker->close(speaker->self) == XIAOTAI_OK && !xiaotai_bus_host_pin(53).level);
    const auto* key = xiaotai_hal_input(0);
    assert(key->desc.kind == XIAOTAI_INPUT_BUTTON && std::string_view(key->desc.role) == "main" &&
           key->desc.events == (XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED));
    assert(xiaotai_bus_host_pin(35).pull_up && !xiaotai_bus_host_pin(35).output);
}

void display_success()
{
    descriptors();
    const auto* display = xiaotai_hal_display(0);
    assert(display->desc.width == 1024 && display->desc.height == 600 && display->desc.touch &&
           display->desc.render == XIAOTAI_RENDER_SCANOUT && display->desc.buffers == 2 &&
           display->desc.pixel == XIAOTAI_PIXEL_RGB565 && display->desc.rotation == 0 &&
           display->desc.flags == XIAOTAI_DISPLAY_DIMMABLE);
    assert(!xiaotai_bus_host_pin(22).level && !xiaotai_bus_host_pwm(1).configured);
    assert(display->set_backlight(display->self, 50) == XIAOTAI_ERR_INVALID_STATE);
    recording = true;
    xiaotai_display_handles_t handles{};
    assert(display->open(display->self, &handles) == XIAOTAI_OK);
    assert(journal == std::vector<Op>(start_order.begin(), start_order.end()));
    assert(handles.framebuffers[0] == frame_memory[0].data() && handles.framebuffers[1] == frame_memory[1].data());
    assert(display->open(display->self, nullptr) == XIAOTAI_OK && journal.size() == start_order.size());
    xiaotai_touch_point_t point{};
    size_t count{};
    assert(display->read_touch(display->self, &point, 1, &count) == XIAOTAI_OK &&
           count == 1 && point.x == 21 && point.y == 33);
    assert(display->set_backlight(display->self, 101) == XIAOTAI_ERR_INVALID_ARG);
    assert(display->set_backlight(display->self, 50) == XIAOTAI_OK && xiaotai_bus_host_pwm(1).duty == 511);
    const xiaotai_rect_t area{0, 0, 1024, 600};
    assert(display->present(display->self, &area, handles.framebuffers[1]) == XIAOTAI_OK && showing == 1);
    const xiaotai_rect_t partial{10, 20, 4, 2};
    assert(display->present(display->self, &partial, handles.framebuffers[0]) == XIAOTAI_ERR_INVALID_ARG);
    assert(showing == 1);
    std::vector<uint16_t> independent(1024 * 600, 0x1234);
    assert(display->present(display->self, &area, independent.data()) == XIAOTAI_OK && frame_memory[1][0] == 0x1234);
    independent[0] = 0xf123;
    assert(frame_memory[1][0] == 0x1234);
}

#ifdef WT99_UI_BIND_PROBE
void display_bind(wt99_bind_mode_t mode, bool borrowed = false, bool stop_fails = false)
{
    descriptors();
    const auto* display = xiaotai_hal_display(0);
    assert(display->open_bound);
    assert(!xiaotai_bus_host_pin(22).level && !xiaotai_bus_host_pwm(1).configured);
    const bool shared_power = xiaotai_bus_host_pin(0).level;
    assert(shared_power);
    shared_power_writes = 0;
    recording = true;
    if (borrowed) assert(display->open(display->self, nullptr) == XIAOTAI_OK);
    if (stop_fails) fail_stop = 1;
    wt99_ui_bind_probe(mode, borrowed);
    const bool open = borrowed || mode == HEALTHY;
    const bool latched = mode == DETACH_FAIL || stop_fails;
    std::vector<Op> expected(start_order.begin(), start_order.end());
    if (stop_fails) expected.push_back({Panel, false});
    else if (!open && !latched)
        for (Event event : release_order) expected.push_back({event, false});
    assert(journal == expected);
    assert(xiaotai_bus_host_pin(0).level == shared_power);
    assert(xiaotai_bus_host_pin(22).level == (open || latched));
    assert(xiaotai_bus_host_pwm(1).configured == (open || latched));
    xiaotai_display_part_stats_t stats{};
    xiaotai_display_part_stats(display, &stats);
    assert(stats.opens == (open ? 1U : 0U) && stats.open_failures == (open ? 0U : 1U));
    assert(stats.open == open && stats.latched == latched);
    if (!open) {
        const bool oom = mode == CREATE_FAIL || mode == POINTER_FAIL;
        assert(stats.last_error == (oom ? XIAOTAI_ERR_NO_MEM : XIAOTAI_ERR_TIMEOUT));
    }
    xiaotai_touch_point_t point{};
    size_t count{};
    assert(display->read_touch(display->self, &point, 1, &count) == (open ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE));
    if (open) assert(count == 1 && point.x == 21 && point.y == 33);
    xiaotai_display_handles_t reopened{};
    if (latched) {
        assert(display->open(display->self, &reopened) == XIAOTAI_ERR_INVALID_STATE);
        assert(!reopened.panel && journal == expected);
    } else {
        assert(display->open(display->self, &reopened) == XIAOTAI_OK);
        assert(reopened.panel == &panel && reopened.touch == &touch);
        assert(reopened.framebuffers[0] == frame_memory[0].data());
        if (!open) expected.insert(expected.end(), start_order.begin(), start_order.end());
        assert(journal == expected);
    }
    std::fprintf(stderr, "WT board releases=%u opened=%d latched=%d shared_power_writes=%u\n",
                 stop_index, open, latched, shared_power_writes);
    assert(!shared_power_writes);
}
#endif

void display_failure(unsigned start, unsigned stop)
{
    const auto* display = xiaotai_hal_display(0);
    fail_start = start;
    fail_stop = stop;
    recording = true;
    xiaotai_display_handles_t handles{};
    handles.framebuffers[0] = handles.framebuffers[1] = &panel;
    assert(display->open(display->self, &handles) == XIAOTAI_ERR_TIMEOUT);
    assert(!handles.panel && !handles.panel_io && !handles.touch && !handles.framebuffers[0] && !handles.framebuffers[1]);
    std::vector<Op> expected;
    unsigned successful = 0;
    for (auto op : start_order) {
        expected.push_back(op);
        if (op.event != Delay20 && ++successful == start) break;
    }
    const size_t start_size = expected.size();
    for (auto event : release_order) {
        const auto acquired = [&] {
            for (size_t i = 0; i < start_size; ++i)
                if (expected[i] == Op{event, true}) return event == Rail || i + 1 < start_size;
            return false;
        }();
        if (acquired) expected.push_back({event, false});
        if (stop && expected.size() == start_size + stop) break;
    }
    assert(journal == expected);
    if (stop) {
        const auto retained = journal.size();
        assert(display->open(display->self, &handles) == XIAOTAI_ERR_INVALID_STATE && journal.size() == retained);
        assert(xiaotai_bus_host_pin(22).level);
    } else {
        assert(!xiaotai_bus_host_pin(22).level && !xiaotai_bus_host_pwm(1).configured);
        fail_start = 0;
        assert(display->open(display->self, &handles) == XIAOTAI_OK);
    }
}

void camera_setup()
{
    const auto* camera = xiaotai_hal_camera(0);
    assert(camera->desc.max_width == 1024 && camera->desc.max_height == 600 && camera->desc.min_width == 1024 &&
           camera->desc.min_height == 600 && camera->desc.max_fps == 30 && camera->desc.row_align == 16 &&
           camera->desc.rotation == 0 && camera->desc.formats == XIAOTAI_CAMERA_YUV420_PAIRED &&
           camera->desc.flags == XIAOTAI_CAMERA_FULL_RANGE);
    assert(xiaotai_bus_host_pin(0).level);
    csi_fake.reg[0x3107] = 0xcb;
    csi_fake.reg[0x3108] = 0x3a;
    assert(camera->prepare(camera->self) == XIAOTAI_OK && xiaotai_bus_host_pin(0).level);
    assert(csi_fake.hz == 400000 && csi_fake.start_calls == 1);
    const xiaotai_camera_mode_t mode{1024, 600, XIAOTAI_CAMERA_YUV420_PAIRED, 30};
    assert(camera->open(camera->self, &mode) == XIAOTAI_OK && csi_fake.buffer_count == 3 &&
           csi_fake.capacities[0] == 1024 * 608 * 3 / 2 && csi_fake.full_range && !csi_fake.rate_calls);
    assert(camera->close(camera->self) == XIAOTAI_OK && camera->set_power(camera->self, false) == XIAOTAI_OK);
    assert(xiaotai_bus_host_pin(0).level && csi_fake.powered);
}

void c61_maintenance()
{
    xiaotai_coprocessor_host_reset();
    auto* host = xiaotai_coprocessor_host();
    const auto* cp = xiaotai_hal_network(0);
    assert(cp->desc.kind == XIAOTAI_NETWORK_WIFI_COPROCESSOR &&
           cp->desc.provisioning == (XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL) &&
           static_cast<const xiaotai_network_wifi_t*>(cp->driver_config)->tx_buffers_on_demand);
    host->chip = 0x14;
    host->major = 2; host->minor = 12; host->patch = 13;
    assert(cp->prepare(cp->self, false, nullptr, nullptr) == XIAOTAI_OK);
    assert(host->starts == 1 && host->stops == 0 && host->writes == 0 && host->begins == 0);
    host->major = 3; host->minor = 0; host->patch = 7;
    assert(cp->prepare(cp->self, true, nullptr, nullptr) == XIAOTAI_OK);
    assert(host->starts == 2 && host->stops == 1 && host->writes == 0);
    host->patch = 8;
    const auto rejected = cp->prepare(cp->self, true, nullptr, nullptr);
    xiaotai_coprocessor_state_t state{};
    xiaotai_coprocessor_state(cp, &state);
    assert(rejected != XIAOTAI_OK && state.stage == XIAOTAI_NETWORK_UNSUPPORTED && state.error == rejected);
    assert(host->writes == 0 && host->begins == 0);
    xiaotai_coprocessor_host_journal_clear();
    char reply[32];
    assert(cp->coprocessor_command(cp->self, "unknown", reply, sizeof(reply)) == XIAOTAI_ERR_INVALID_ARG);
    for (const char* word : {"INFO", "RXRESET", "SINK0", "SINK1", "ROLLBACK"})
        assert(cp->coprocessor_command(cp->self, word, reply, sizeof(reply)) == XIAOTAI_OK);
    assert(std::string_view(xiaotai_coprocessor_host_journal()) ==
           "command 57545331 INFO 1\ncommand 57545331 RXRESET 1\ncommand 57545331 SINK0 1\n"
           "command 57545331 SINK1 1\ncommand 57545331 ROLLBACK 0\ndelay 4000\n");
    std::array<uint8_t, 1303> image{};
    image[0] = 0xe9; image[1] = 1; image[12] = 0x14; image[15] = 99; image[17] = 101; image[29] = 1;
    image[32] = 0x32; image[33] = 0x54; image[34] = 0xcd; image[35] = 0xab;
    constexpr char project[] = "xiaotai_wt99p461_c61";
    std::memcpy(image.data() + 80, project, sizeof(project));
    host->image = image.data();
    xiaotai_coprocessor_host_journal_clear();
    assert(cp->coprocessor_update(cp->self, image.data(), image.size(), nullptr, nullptr) == XIAOTAI_OK);
    assert(host->writes == 3 && host->largest_write == 512 && host->contiguous);
    assert(std::string_view(xiaotai_coprocessor_host_journal()).find("activate\ndelay 3000\n") != std::string_view::npos);
}

}  // namespace

#ifdef WT99_UI_BIND_PROBE
static void native_panel_handles(void *native_panel, void *native_io, xiaotai_display_handles_t *handles)
{
    assert(native_panel == &panel && native_io == &dbi);
    handles->panel = native_panel;
    handles->panel_io = native_io;
}

static void *native_touch_handle(void *native_touch)
{
    assert(native_touch == &touch);
    return native_touch;
}
#endif

extern "C" const jd9165_io_t xiaotai_jd9165_io{
    phy_open, release<Phy, &phy>, dsi_open, release<Dsi, &dsi>, dbi_open, release<Dbi, &dbi>,
    panel_open, release<Panel, &panel>, panel_enable, panel_frames, panel_present,
#ifdef WT99_UI_BIND_PROBE
    native_panel_handles,
#else
    nullptr,
#endif
};
extern "C" const gt911_io_t xiaotai_gt911_io{
    touch_io_open, release<TouchIo, &touch_io>, touch_open, release<Touch, &touch>, touch_read,
#ifdef WT99_UI_BIND_PROBE
    native_touch_handle,
#else
    nullptr,
#endif
};

extern "C" xiaotai_err_t __real_xiaotai_bus_io_pin_set(int pin, bool level);
extern "C" xiaotai_err_t __wrap_xiaotai_bus_io_pin_set(int pin, bool level)
{
#ifdef WT99_UI_BIND_PROBE
    if (recording && pin == 0) ++shared_power_writes;
#endif
    Event event;
    switch (pin) {
    case 0: csi_fake.powered = level; return __real_xiaotai_bus_io_pin_set(pin, level);
    case 22: event = Rail; break;
    case 26: event = UpDown; break;
    case 27: event = LeftRight; break;
    case 23: event = Reset; break;
    default: return __real_xiaotai_bus_io_pin_set(pin, level);
    }
    auto err = mark(event, level);
    return err == XIAOTAI_OK ? __real_xiaotai_bus_io_pin_set(pin, level) : err;
}

extern "C" xiaotai_err_t __real_xiaotai_bus_io_pwm_setup(const xiaotai_pwm_config_t*, uint32_t);
extern "C" xiaotai_err_t __wrap_xiaotai_bus_io_pwm_setup(const xiaotai_pwm_config_t* c, uint32_t duty)
{
    auto err = mark(Pwm, true);
    return err == XIAOTAI_OK ? __real_xiaotai_bus_io_pwm_setup(c, duty) : err;
}

extern "C" xiaotai_err_t __real_xiaotai_bus_io_pwm_stop(uint8_t, bool);
extern "C" xiaotai_err_t __wrap_xiaotai_bus_io_pwm_stop(uint8_t channel, bool idle_high)
{
    auto err = mark(Pwm, false);
    return err == XIAOTAI_OK ? __real_xiaotai_bus_io_pwm_stop(channel, idle_high) : err;
}

extern "C" bool __wrap_xiaotai_mem_has_external() { return fake_psram; }
extern "C" bool __wrap_xiaotai_mem_is_external(const void* data) { return fake_psram && data; }

struct xiaotai_bus_io_i2c;
extern "C" xiaotai_err_t __real_xiaotai_bus_io_i2c_open(const xiaotai_i2c_config_t*, xiaotai_bus_io_i2c**);
extern "C" xiaotai_err_t __wrap_xiaotai_bus_io_i2c_open(const xiaotai_i2c_config_t* config, xiaotai_bus_io_i2c** io)
{
    if (watch_control_power && config->port == 1) {
        assert(xiaotai_bus_host_pin(0).level && control_power_settled);
        control_bus_opened = true;
    }
    return __real_xiaotai_bus_io_i2c_open(config, io);
}

extern "C" void __wrap_xiaotai_delay_ms(uint32_t ms)
{
    if (ms == 20) (void)mark(Delay20, true);
    if (watch_control_power && !control_bus_opened && ms == 100) {
        assert(xiaotai_bus_host_pin(0).level);
        control_power_settled = true;
    }
    xiaotai_coprocessor_host_note("delay %u", ms);
}

int main(int argc, char* argv[])
{
    assert(argc >= 1);
#ifdef WT99_UI_BIND_PROBE
    const std::string_view test = argc > 1 ? argv[1] : "ui-bind-fail";
#else
    const std::string_view test = argc > 1 ? argv[1] : "display-success";
#endif
    if (test == "init-pin") {
        assert(argc == 3);
        const int pin = std::atoi(argv[2]);
        xiaotai_bus_host_pin_fail(pin, XIAOTAI_ERR_TIMEOUT);
        assert(xiaotai_hal_init() == XIAOTAI_ERR_TIMEOUT && xiaotai_hal_init() == XIAOTAI_ERR_TIMEOUT);
        assert(!xiaotai_hal_display(0) && !xiaotai_hal_network(0) && !xiaotai_hal_camera(0));
        assert(!xiaotai_bus_host_pin(22).level && !xiaotai_bus_host_pin(53).level && !xiaotai_bus_host_pin(0).level);
        return 0;
    }
    watch_control_power = test == "control-bus-power";
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    if (watch_control_power) {
        assert(control_bus_opened && control_power_settled);
        camera_setup();
        return 0;
    }
    if (test == "display-success") display_success();
#ifdef WT99_UI_BIND_PROBE
    else if (test == "ui-bind-fail") display_bind(CREATE_FAIL);
    else if (test == "ui-bind-pointer-fail") display_bind(POINTER_FAIL);
    else if (test == "ui-bind-borrow") display_bind(CREATE_FAIL, true);
    else if (test == "ui-bind-timeout") display_bind(SCANOUT_TIMEOUT);
    else if (test == "ui-bind-detach-fail") display_bind(DETACH_FAIL);
    else if (test == "ui-bind-success") display_bind(HEALTHY);
    else if (test == "ui-bind-release-fail") display_bind(CREATE_FAIL, false, true);
#endif
    else if (test == "display-fail") {
        assert(argc == 4);
        display_failure(unsigned(std::atoi(argv[2])), unsigned(std::atoi(argv[3])));
    } else if (test == "camera") camera_setup();
    else if (test == "c61") c61_maintenance();
    else std::abort();
}
