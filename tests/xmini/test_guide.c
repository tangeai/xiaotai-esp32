// SPDX-License-Identifier: MIT
#include <assert.h>
#include <string.h>
#include "xmini_guide.h"
#include "xmini_canvas.h"

static bool rows_blank(const uint8_t *frame, unsigned begin, unsigned end)
{
    for (unsigned y = begin; y < end; ++y)
        for (unsigned x = 0; x < 128; ++x)
            if (frame[(y / 8) * 128 + x] & (1U << (y % 8))) return false;
    return true;
}

static void rows_equal(const uint8_t *actual, const uint8_t *expected, unsigned begin, unsigned end)
{
    for (unsigned y = begin; y < end; ++y)
        for (unsigned x = 0; x < 128; ++x)
            assert((actual[(y / 8) * 128 + x] & (1U << (y % 8))) ==
                   (expected[(y / 8) * 128 + x] & (1U << (y % 8))));
}

static void test_content_rows(void)
{
    uint8_t frame[1024], expected[1024] = {0};
    xmini_guide_t guide = {.phase = XMINI_GUIDE_WIFI_SETUP, .ssid = "XiaoTai-CA88"};
    xmini_guide_render(&guide, 0, frame);
    xmini_canvas_utf8_center(expected, 22, "手机连接热点");
    xmini_canvas_utf8_center(expected, 36, guide.ssid);
    xmini_canvas_utf8_center(expected, 50, "192.168.6.1");
    rows_equal(frame, expected, 22, 64);
    assert(rows_blank(frame, 34, 36) && rows_blank(frame, 48, 50) && rows_blank(frame, 62, 64));

    static const char *const codes[] = {"000000", "111111", "888888", "999999", "012345", "987654"};
    guide = (xmini_guide_t){.phase = XMINI_GUIDE_BINDING, .expires_at_ms = 99000};
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        strcpy(guide.code, codes[i]);
        memset(expected, 0, sizeof(expected));
        xmini_canvas_center(expected, 22, codes[i], 2);
        xmini_canvas_utf8_center(expected, 36, "微信搜索小钛");
        xmini_canvas_utf8_center(expected, 50, "99秒后过期");
        xmini_guide_render(&guide, 0, frame);
        // Six doubled 5x7 digits occupy y22..35; the 12px hint starts at y36.
        rows_equal(frame, expected, 22, 36);
        rows_equal(frame, expected, 36, 48);
        rows_equal(frame, expected, 50, 64);
        assert(rows_blank(frame, 48, 50) && rows_blank(frame, 62, 64));
    }

    guide = (xmini_guide_t){.phase = XMINI_GUIDE_OFFLINE, .retryable = true};
    memset(expected, 0, sizeof(expected));
    xmini_canvas_utf8_center(expected, 22, "网络已断开");
    xmini_canvas_utf8_center(expected, 36, "双击重试");
    xmini_canvas_utf8_center(expected, 50, "双击重试");
    xmini_guide_render(&guide, 0, frame);
    rows_equal(frame, expected, 22, 64);
    assert(rows_blank(frame, 34, 36) && rows_blank(frame, 48, 50) && rows_blank(frame, 62, 64));

    guide = (xmini_guide_t){.phase = XMINI_GUIDE_BINDING, .code = "012345", .retryable = true, .expires_at_ms = 50};
    memset(expected, 0, sizeof(expected));
    xmini_canvas_utf8_center(expected, 22, "验证码已过期");
    xmini_canvas_utf8_center(expected, 36, "双击重试");
    xmini_canvas_utf8_center(expected, 50, "双击重试");
    xmini_guide_render(&guide, 50, frame);
    rows_equal(frame, expected, 22, 64);
}

int main(void)
{
    assert(xmini_guide_valid_code("012345"));
    assert(xmini_guide_valid_code("000000"));
    assert(!xmini_guide_valid_code("12345") && !xmini_guide_valid_code("1234567"));
    assert(!xmini_guide_valid_code("12a456") && !xmini_guide_valid_code(NULL));
    xmini_guide_t guide = {.phase = XMINI_GUIDE_BINDING, .code = "000001", .expires_at_ms = 50};
    assert(xmini_guide_seconds_left(&guide, UINT32_MAX - 949) == 1);
    assert(xmini_guide_seconds_left(&guide, 49) == 1);
    assert(xmini_guide_seconds_left(&guide, 50) == 0);
    assert(xmini_guide_seconds_left(&guide, 51) == 0);
    assert(xmini_canvas_utf8_width("微信搜索小钛") == 72);
    assert(xmini_canvas_utf8_width("XiaoTai-CA88") <= 128);
    assert(xmini_canvas_utf8_width("正在连接无线网络") <= 128);
    unsigned char data[1056];
    static const char *const titles[] = {"小钛", "无线网络", "小钛", "小钛", "设备绑定", "小钛", "小钛", "小钛", "操作未完成"};
    for (unsigned mode = 0; mode < 4; ++mode) {
      for (unsigned phase = XMINI_GUIDE_STARTUP; phase <= XMINI_GUIDE_ERROR; ++phase) {
        memset(data, 0xa5, sizeof(data));
        guide.phase = phase; strcpy(guide.ssid, "XiaoTai-CA88");
        guide.retryable = (mode & 1) != 0;
        guide.startup_failed = (mode & 2) != 0;
        xmini_guide_render(&guide, 0, data + 16);
        bool nonblank = false;
        for (unsigned i = 16; i < 1040; ++i) nonblank |= data[i] != 0;
        assert(nonblank);
        assert(rows_blank(data + 16, 14, 22));
        assert(rows_blank(data + 16, 62, 64));
        uint8_t title[1024] = {0};
        xmini_canvas_utf8_center(title, 2, titles[phase]);
        rows_equal(data + 16, title, 0, 14);
        xmini_canvas_utf8(data + 16, -6, -6, "绑定成功");
        xmini_canvas_utf8(data + 16, 127, 63, "设备");
        xmini_canvas_utf8(data + 16, 0, 0, "\xe4\xb8");
        for (unsigned i = 0; i < 16; ++i) assert(data[i] == 0xa5 && data[i + 1040] == 0xa5);
      }
    }
    test_content_rows();
    return 0;
}
