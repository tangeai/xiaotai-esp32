// SPDX-License-Identifier: MIT
#include "xmini_guide.h"
#include "xmini_canvas.h"
#include <stdio.h>
#include <string.h>

bool xmini_guide_valid_code(const char *code)
{
    if (!code || strlen(code) != 6) return false;
    for (unsigned i = 0; i < 6; ++i)
        if (code[i] < '0' || code[i] > '9') return false;
    return true;
}

unsigned xmini_guide_seconds_left(const xmini_guide_t *guide, uint32_t now_ms)
{
    if (!guide || guide->phase != XMINI_GUIDE_BINDING || !xmini_guide_valid_code(guide->code)) return 0;
    int32_t remaining = (int32_t)(guide->expires_at_ms - now_ms);
    return remaining > 0 ? ((unsigned)remaining + 999) / 1000 : 0;
}

void xmini_guide_render(const xmini_guide_t *guide, uint32_t now_ms, uint8_t frame[1024])
{
    memset(frame, 0, 1024);
    const char *title = "小钛", *message = "正在启动", *detail = "";
    char text[40];
    switch (guide->phase) {
    case XMINI_GUIDE_WIFI_SETUP:
        title = "无线网络";
        message = "手机连接热点";
        xmini_canvas_utf8_center(frame, 2, title);
        xmini_canvas_utf8_center(frame, 22, message);
        xmini_canvas_utf8_center(frame, 36, guide->ssid);
        xmini_canvas_utf8_center(frame, 50, "192.168.6.1");
        return;
    case XMINI_GUIDE_CONNECTING: message = "正在连接网络"; break;
    case XMINI_GUIDE_SERVICES: message = "正在连接服务"; break;
    case XMINI_GUIDE_BINDING: {
        unsigned seconds = xmini_guide_seconds_left(guide, now_ms);
        title = "设备绑定";
        if (!seconds) { message = "验证码已过期"; detail = "双击重试"; break; }
        xmini_canvas_utf8_center(frame, 2, title);
        xmini_canvas_center(frame, 22, guide->code, 2);
        xmini_canvas_utf8_center(frame, 36, "微信搜索小钛");
        snprintf(text, sizeof(text), "%u秒后过期", seconds);
        xmini_canvas_utf8_center(frame, 50, text);
        return;
    }
    case XMINI_GUIDE_VERIFYING: message = "正在验证授权"; break;
    case XMINI_GUIDE_ONLINE: message = "平台已在线"; detail = "RTC NOT READY"; break;
    case XMINI_GUIDE_OFFLINE: message = "网络已断开"; detail = "双击重试"; break;
    case XMINI_GUIDE_ERROR:
        title = "操作未完成"; message = guide->startup_failed ? "启动失败" : guide->ssid[0] ? "192.168.6.1" : "";
        snprintf(text, sizeof(text), "ERR %04X", guide->error); detail = text; break;
    default: break;
    }
    xmini_canvas_utf8_center(frame, 2, title);
    xmini_canvas_utf8_center(frame, guide->retryable ? 22 : 25, message);
    if (guide->retryable) xmini_canvas_utf8_center(frame, 36, "双击重试");
    xmini_canvas_utf8_center(frame, 50, detail);
}
