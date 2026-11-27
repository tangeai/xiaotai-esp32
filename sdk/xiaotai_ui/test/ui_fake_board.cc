// SPDX-License-Identifier: MIT
// Fake board for the UI system's host tests: display parts only, set by the
// test through ui_test.h. open() is counted and may be told to fail; present()
// and read_touch() succeed without effect.
#include <array>
#include <cstring>
#include "ui_test.h"
#include "xiaotai/board.hpp"

namespace {

struct FakeDisplay {
    xiaotai_display_t part{};
    uint32_t opens = 0;
    xiaotai_err_t open_result = XIAOTAI_OK;
};

std::array<FakeDisplay, UI_FAKE_DISPLAYS_MAX> g_displays;
size_t g_count = 0;

xiaotai_err_t Open(void* self, xiaotai_display_handles_t* handles)
{
    auto* display = static_cast<FakeDisplay*>(self);
    ++display->opens;
    if (handles) *handles = xiaotai_display_handles_t{};
    return display->open_result;
}

xiaotai_err_t SetBacklight(void*, uint8_t percent) { return percent > 100 ? XIAOTAI_ERR_INVALID_ARG : XIAOTAI_OK; }

xiaotai_err_t Present(void*, const xiaotai_rect_t*, const void*) { return XIAOTAI_OK; }

xiaotai_err_t ReadTouch(void*, xiaotai_touch_point_t*, size_t, size_t* count)
{
    *count = 0;
    return XIAOTAI_OK;
}

class FakeBoard final : public xiaotai::board::Board {
public:
    xiaotai_board_identity_t Identity() const override { return {"test-ui", "1", nullptr, nullptr}; }
    const xiaotai_display_t* Display(size_t index) override
    {
        return index < g_count ? &g_displays[index].part : nullptr;
    }
};

}  // namespace

namespace xiaotai::board {
Board& SelectedBoard()
{
    static FakeBoard board;
    return board;
}
}  // namespace xiaotai::board

extern "C" void ui_fake_board_set_displays(const xiaotai_display_desc_t* descs, size_t count)
{
    g_count = count < g_displays.size() ? count : g_displays.size();
    for (size_t i = 0; i < g_displays.size(); ++i) {
        FakeDisplay& display = g_displays[i];
        display = FakeDisplay{};
        if (i >= g_count) continue;
        display.part.desc = descs[i];
        display.part.open = Open;
        display.part.set_backlight = SetBacklight;
        display.part.present = Present;
        display.part.read_touch = descs[i].touch ? ReadTouch : nullptr;
        display.part.self = &display;
    }
}

extern "C" void ui_fake_display_fail_open(size_t index, xiaotai_err_t err)
{
    if (index < g_displays.size()) g_displays[index].open_result = err;
}

extern "C" uint32_t ui_fake_display_opens(size_t index)
{
    return index < g_displays.size() ? g_displays[index].opens : 0;
}

extern "C" const xiaotai_display_t* ui_fake_display(size_t index)
{
    return index < g_count ? &g_displays[index].part : nullptr;
}

extern "C" xiaotai_display_desc_t ui_fake_desc(uint16_t width, uint16_t height, xiaotai_pixel_t pixel)
{
    xiaotai_display_desc_t desc{};
    desc.width = width;
    desc.height = height;
    desc.pixel = pixel;
    desc.render = pixel == XIAOTAI_PIXEL_RGB565 ? XIAOTAI_RENDER_PARTIAL : XIAOTAI_RENDER_FULL;
    return desc;
}
