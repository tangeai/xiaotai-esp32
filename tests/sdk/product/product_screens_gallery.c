// SPDX-License-Identifier: MIT
// A gallery of views for design review: states the real flows of
// product_screens.c cannot reach with the host fixtures (the set-up steps,
// failures, empty and very long lists, both ends of a slider, dialogs, long and
// mixed-script names). Each view is shown straight through the UI system, so
// the product's real theme draws it on the product's real display size. The
// texts copy what the applications show in those states.
#include <stdio.h>
#include <string.h>

#include "product_screens.h"
#include "product_test.h"
#include "xiaotai/port/os.h"
#include "xiaotai/view.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define BACK {.role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK, .intent = "gallery.none"}

static void show_on_owner(void *ctx) { xiaotai_view_show(ctx); }

static void show(const xiaotai_view_t *view, const char *name)
{
    xiaotai_view_t copy = *view;
    product_on_owner(show_on_owner, &copy);
    if (name) product_screens_shoot(name);
}

static void dismiss(xiaotai_view_layer_t layer)
{
    xiaotai_view_t view = {.kind = XIAOTAI_VIEW_NOTICE, .layer = (uint8_t)layer, .flags = XIAOTAI_VIEW_DISMISS};
    product_on_owner(show_on_owner, &view);
}

static void toast(const char *text, const char *name)
{
    xiaotai_view_t view = {.kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_TOAST, .text = text,
                           .duration_ms = 6000};
    show(&view, name);
    dismiss(XIAOTAI_LAYER_TOAST);
}

static void setup_pages(void)
{
    static const xiaotai_view_item_t steps[] = {
        {.label = "1. 连接热点", .detail = "XiaoTai-7F3A"},
        {.label = "2. 打开网址", .detail = "http://192.168.4.1"},
        {.label = "3. 选择网络", .detail = "家里的 Wi-Fi 和密码"},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .id = "g.setup", .title = "连接 Wi-Fi",
        .text = "手机连接设备热点", .subtitle = "用手机按下面三步配网", .items = steps, .item_count = COUNT(steps)},
        "setup-hotspot");
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .id = "g.setup", .title = "连接 Wi-Fi",
        .text = "正在验证 Wi-Fi 连接"}, "setup-verifying");
    static const xiaotai_view_action_t failed[] = {
        {.label = "开启热点配网", .intent = "gallery.none"},
        {.label = "重试", .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY},
        {.label = "取消", .intent = "gallery.none", .role = XIAOTAI_ROLE_BACK},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .id = "g.setup", .title = "连接 Wi-Fi",
        .text = "连接失败，请重试", .status = "ERR 0107", .actions = failed, .action_count = COUNT(failed)},
        "setup-failed");

    static const xiaotai_view_item_t waiting[] = {
        {.label = "等待绑定"}, {.label = "用手机打开网址，输入验证码"},
    };
    static const xiaotai_view_action_t code_actions[] = {
        {.label = "重播验证码", .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY},
        {.label = "更换网络", .intent = "gallery.none"},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CODE, .id = "g.binding", .title = "绑定设备", .text = "482913",
        .subtitle = "https://xiaotai.chat/", .status = "网络已连接", .timer_mode = XIAOTAI_TIMER_REMAINING,
        .timer_s = 287, .items = waiting, .item_count = COUNT(waiting), .actions = code_actions,
        .action_count = COUNT(code_actions)}, "binding-code");
    static const xiaotai_view_item_t fetching[] = {
        {.label = "正在获取验证码"}, {.label = "用手机打开网址，输入验证码"},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CODE, .id = "g.binding", .title = "绑定设备", .text = "------",
        .subtitle = "https://xiaotai.chat/", .status = "网络已连接", .items = fetching, .item_count = COUNT(fetching),
        .actions = code_actions + 1, .action_count = 1}, "binding-fetching");
    static const xiaotai_view_item_t expired[] = {
        {.label = "验证码已过期"}, {.label = "用手机打开网址，输入验证码"},
    };
    static const xiaotai_view_action_t retry[] = {
        {.label = "重试", .icon = XIAOTAI_ICON_REFRESH, .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY},
        {.label = "更换网络", .intent = "gallery.none"},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CODE, .id = "g.binding", .title = "绑定设备", .text = "------",
        .subtitle = "https://xiaotai.chat/", .status = "网络已连接", .items = expired, .item_count = COUNT(expired),
        .actions = retry, .action_count = COUNT(retry)}, "binding-expired");
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CODE, .id = "g.qr", .title = "微信扫码", .flags = XIAOTAI_VIEW_QR,
        .text = "https://xiaotai.chat/bind?code=482913&from=device", .subtitle = "用微信扫一扫添加设备",
        .actions = (const xiaotai_view_action_t[]){BACK}, .action_count = 1}, "code-qr");

    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_PROGRESS, .id = "g.progress", .title = "设备初始化",
        .text = "正在连接设备服务"}, "progress-waiting");
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_PROGRESS, .id = "g.progress", .title = "联网模块",
        .text = "正在更新联网模块", .flags = XIAOTAI_VIEW_PERCENT, .progress = 42}, "progress-percent");
    static const xiaotai_view_action_t again[] = {
        {.label = "重试", .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_PROGRESS, .id = "g.progress", .title = "设备初始化",
        .text = "初始化失败", .status = "ERR 0203", .actions = again, .action_count = 1}, "progress-failed");
}

static void list_pages(void)
{
    static const xiaotai_view_action_t header[] = {
        BACK, {.label = "刷新", .icon = XIAOTAI_ICON_REFRESH, .intent = "gallery.none"},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_LIST, .id = "g.list.empty", .title = "通讯录", .status = "暂无联系人",
        .actions = header, .action_count = COUNT(header)}, "list-empty");
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_LIST, .id = "g.list.empty", .title = "通讯录", .status = "加载失败",
        .actions = header, .action_count = COUNT(header)}, "list-error");

    static xiaotai_view_item_t people[18];
    static const char *const names[] = {
        "前台", "王小明的办公室设备（三楼东侧会议区）", "Alexander von Humboldt 的 XiaoTai", "奶奶", "会议室 B-204",
        "张经理", "Li Lei & Han Meimei", "客厅", "研发中心一号实验台 Lab-01 测试机", "妈妈",
        "二楼书房", "欧阳娜娜", "仓库", "司马相如", "门口", "儿童房", "诸葛孔明", "厨房",
    };
    for (size_t i = 0; i < COUNT(people); ++i) {
        people[i] = (xiaotai_view_item_t){.label = names[i], .intent = "gallery.none", .value = (int32_t)i,
            .detail = i % 3 == 2 ? "微信联系人" : i % 2 ? "设备 · 离线" : "设备 · 在线"};
    }
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_LIST, .id = "g.list.long", .title = "通讯录", .items = people,
        .item_count = COUNT(people), .actions = header, .action_count = COUNT(header)}, "list-long-names");

    static const xiaotai_view_item_t menu[] = {
        {.label = "通讯录", .icon = XIAOTAI_ICON_CONTACTS, .intent = "gallery.none"},
        {.label = "多人对讲", .icon = XIAOTAI_ICON_GROUP, .intent = "gallery.none", .state = XIAOTAI_STATE_DISABLED},
        {.label = "设置", .icon = XIAOTAI_ICON_SETTINGS, .intent = "gallery.none", .state = XIAOTAI_STATE_CHECKED},
        {.label = "只读的一行", .detail = "没有操作"},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_LIST, .id = "g.list.states", .title = "菜单", .status = "已保存",
        .items = menu, .item_count = COUNT(menu), .actions = (const xiaotai_view_action_t[]){BACK},
        .action_count = 1}, "list-states");
}

static void form_pages(void)
{
    static const char *const sleeps[] = {"1 分钟", "5 分钟", "10 分钟", "30 分钟", "永不"};
    static const xiaotai_view_field_t fields[] = {
        {.kind = XIAOTAI_FIELD_SLIDER, .label = "亮度", .intent = "gallery.none", .value = 0, .max = 100, .text = "%"},
        {.kind = XIAOTAI_FIELD_SLIDER, .label = "音量", .intent = "gallery.none", .value = 100, .max = 100, .text = "%"},
        {.kind = XIAOTAI_FIELD_STEPPER, .label = "麦克风增益", .intent = "gallery.none", .value = 0, .max = 30,
         .step = 3, .text = "dB"},
        {.kind = XIAOTAI_FIELD_SWITCH, .label = "语音唤醒", .intent = "gallery.none", .value = 1},
        {.kind = XIAOTAI_FIELD_SWITCH, .label = "语音引导", .intent = "gallery.none", .value = 0},
        {.kind = XIAOTAI_FIELD_SWITCH, .label = "维护中不可修改", .intent = "gallery.none", .value = 1,
         .state = XIAOTAI_STATE_DISABLED},
        {.kind = XIAOTAI_FIELD_CHOICE, .label = "自动息屏", .intent = "gallery.none", .value = 1, .options = sleeps,
         .option_count = COUNT(sleeps)},
        {.kind = XIAOTAI_FIELD_TEXT, .label = "固件版本", .text = "0.3.2"},
    };
    static const xiaotai_view_action_t actions[] = {
        BACK, {.label = "保存", .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_FORM, .id = "g.form", .title = "设置", .status = "已保存",
        .fields = fields, .field_count = COUNT(fields), .actions = actions, .action_count = COUNT(actions)},
        "form-fields");

    static const xiaotai_view_field_t digits[] = {
        {.kind = XIAOTAI_FIELD_DIGITS, .label = "房间号", .intent = "gallery.none", .max = 6, .text = "2048"},
        {.kind = XIAOTAI_FIELD_DIGITS, .label = "密码", .intent = "gallery.none", .max = 4, .flags = XIAOTAI_FIELD_MASKED},
    };
    static const xiaotai_view_action_t join[] = {
        BACK, {.label = "加入", .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_FORM, .id = "g.digits", .title = "加入房间", .fields = digits,
        .field_count = COUNT(digits), .actions = join, .action_count = COUNT(join)}, "form-digits");
}

static void call_pages(void)
{
    static const xiaotai_view_action_t ringing[] = {
        {.label = "接听", .icon = XIAOTAI_ICON_ACCEPT, .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY,
         .style = XIAOTAI_STYLE_POSITIVE},
        {.label = "拒接", .icon = XIAOTAI_ICON_HANGUP, .intent = "gallery.none", .role = XIAOTAI_ROLE_BACK,
         .style = XIAOTAI_STYLE_DESTRUCTIVE},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CALL, .id = "g.call", .title = "微信视频",
        .subtitle = "Alexander 王小明的办公室设备-前台 02", .text = "语音来电", .actions = ringing,
        .action_count = COUNT(ringing)}, "call-long-name");
    static const xiaotai_view_action_t active[] = {
        {.label = "打开麦克风", .icon = XIAOTAI_ICON_MIC_OFF, .intent = "gallery.none", .style = XIAOTAI_STYLE_QUIET},
        {.label = "挂断", .icon = XIAOTAI_ICON_HANGUP, .intent = "gallery.none", .role = XIAOTAI_ROLE_BACK,
         .style = XIAOTAI_STYLE_DESTRUCTIVE},
        {.label = "关闭摄像头", .icon = XIAOTAI_ICON_CAMERA, .intent = "gallery.none", .style = XIAOTAI_STYLE_QUIET},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CALL, .id = "g.call", .title = "设备视频", .subtitle = "奶奶",
        .text = "麦克风已关闭", .timer_mode = XIAOTAI_TIMER_ELAPSED, .timer_s = 3725, .actions = active,
        .action_count = COUNT(active)}, "call-video-muted");
    static const xiaotai_view_action_t busy[] = {
        {.label = "挂断", .icon = XIAOTAI_ICON_HANGUP, .intent = "gallery.none", .role = XIAOTAI_ROLE_BACK,
         .style = XIAOTAI_STYLE_DESTRUCTIVE, .state = XIAOTAI_STATE_BUSY},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CALL, .id = "g.call", .title = "设备语音", .subtitle = "前台",
        .text = "正在处理…", .timer_mode = XIAOTAI_TIMER_ELAPSED, .timer_s = 59, .actions = busy,
        .action_count = COUNT(busy)}, "call-busy");
}

static void notice_pages(void)
{
    static const xiaotai_view_action_t ok[] = {
        BACK, {.label = "确认", .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .id = "g.result", .title = "提示", .text = "对方无人接听",
        .actions = ok, .action_count = COUNT(ok)}, "result-failed");
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .id = "g.long", .title = "使用说明",
        .subtitle = "第一次使用请先看这里",
        .text = "说“你好小钛”或者点一下表情就能开始对话，再点一下结束。右上角的菜单里有通讯录、多人对讲和设置。"
                "通话时可以随时关闭麦克风；对方挂断后会回到首页。屏幕会在设定的时间后自动息屏，点一下屏幕即可唤醒。"
                "如果 Wi-Fi 换了密码，请在设置里选择“重新配网”，绑定关系会保留。English words such as Wi-Fi, "
                "XiaoTai and ESP32 wrap together with 中文。",
        .actions = (const xiaotai_view_action_t[]){BACK}, .action_count = 1}, "notice-long-text");

    static const xiaotai_view_action_t tabs[] = {
        BACK,
        {.label = "框架", .intent = "gallery.none", .state = XIAOTAI_STATE_CHECKED},
        {.label = "网络", .intent = "gallery.none"}, {.label = "平台", .intent = "gallery.none"},
        {.label = "媒体", .intent = "gallery.none"},
    };
    static const xiaotai_view_item_t facts[] = {
        {.label = "SSID", .detail = "TP-LINK_5G_办公室"}, {.label = "IP", .detail = "192.168.31.207"},
        {.label = "信号", .detail = "-58 dBm"}, {.label = "设备号", .detail = "host-device"},
        {.label = "平台服务", .detail = "https://device.example.invalid/services/v1"},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .id = "g.tabs", .title = "运行状态", .status = "框架",
        .items = facts, .item_count = COUNT(facts), .actions = tabs, .action_count = COUNT(tabs)}, "notice-tabs");

    /* Dialogs and notices above a page. */
    static const xiaotai_view_action_t confirm[] = {
        {.label = "确认", .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY},
        {.label = "取消", .intent = "gallery.none", .role = XIAOTAI_ROLE_BACK},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_OVERLAY, .id = "g.confirm",
        .title = "重新配网", .text = "绑定会保留", .actions = confirm, .action_count = COUNT(confirm)}, "dialog-confirm");
    static const xiaotai_view_action_t destructive[] = {
        {.label = "解除绑定", .intent = "gallery.none", .role = XIAOTAI_ROLE_PRIMARY, .style = XIAOTAI_STYLE_DESTRUCTIVE},
        {.label = "取消", .intent = "gallery.none", .role = XIAOTAI_ROLE_BACK},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_OVERLAY, .id = "g.confirm2",
        .title = "解除绑定", .text = "解除后需要重新扫码添加，通讯录和房间记录会被清空，确定继续吗？",
        .actions = destructive, .action_count = COUNT(destructive)}, "dialog-destructive");
    static const xiaotai_view_item_t choices[] = {
        {.label = "取消", .intent = "gallery.none"}, {.label = "确认配网", .intent = "gallery.none", .value = 1},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_LIST, .layer = XIAOTAI_LAYER_OVERLAY, .id = "g.choice",
        .title = "重新配网", .status = "绑定会保留", .items = choices, .item_count = COUNT(choices)}, "dialog-choice");
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CODE, .layer = XIAOTAI_LAYER_OVERLAY, .id = "g.scan",
        .image = "companion.wechat_code", .title = "微信扫码添加联系人",
        .actions = (const xiaotai_view_action_t[]){BACK}, .action_count = 1}, "dialog-scan");
    dismiss(XIAOTAI_LAYER_OVERLAY);
    toast("已保存", "toast-short");
    toast("设置保存失败，重启后可能恢复原设置，请稍后再试一次", "toast-long");
}

static void home_pages(void)
{
    static const xiaotai_view_indicator_t offline[] = {{.icon = XIAOTAI_ICON_WIFI, .level = -1}};
    static const xiaotai_view_action_t home[] = {
        {.role = XIAOTAI_ROLE_PRIMARY, .intent = "gallery.none", .state = XIAOTAI_STATE_DISABLED},
        {.icon = XIAOTAI_ICON_MENU, .intent = "gallery.none"},
        {.icon = XIAOTAI_ICON_PHONE, .intent = "gallery.none", .state = XIAOTAI_STATE_DISABLED},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_HOME, .id = "g.home", .title = "小钛", .status = "Wi-Fi 连接失败",
        .emotion = XIAOTAI_EMOTION_NEUTRAL, .text = "麦克风已关闭\n点击表情唤醒对话", .indicators = offline,
        .indicator_count = 1, .actions = home, .action_count = COUNT(home)}, "home-offline");
    static const xiaotai_view_indicator_t weak[] = {
        {.icon = XIAOTAI_ICON_WIFI, .level = 1}, {.icon = XIAOTAI_ICON_MIC_OFF},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_HOME, .id = "g.home", .title = "小钛", .status = "AI 回复中",
        .emotion = "happy", .phase = XIAOTAI_TALK_SPEAKING, .speaker = XIAOTAI_SPEAKER_ASSISTANT,
        .text = "当然可以！The quick brown fox 跳过了那只懒狗，然后我们继续聊聊今天的安排吧。", .indicators = weak,
        .indicator_count = COUNT(weak), .actions = home + 1, .action_count = 1}, "home-caption-mixed");
    static const xiaotai_view_action_t clock[] = {
        {.label = "闹钟", .intent = "gallery.none"}, {.label = "倒计时", .intent = "gallery.none"},
        {.label = "设置", .intent = "gallery.none"},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CLOCK, .id = "g.clock", .title = "小钛", .status = "闹钟",
        .text = "下一个闹钟 明天 07:30", .actions = clock, .action_count = COUNT(clock)}, "clock-actions");
    static const xiaotai_view_action_t desk[] = {
        {.icon = XIAOTAI_ICON_PREVIOUS, .intent = "gallery.none"}, {.icon = XIAOTAI_ICON_NEXT, .intent = "gallery.none"},
        {.label = "闹钟", .icon = XIAOTAI_ICON_ALARM, .intent = "gallery.none"},
        {.label = "倒计时", .icon = XIAOTAI_ICON_TIMER, .intent = "gallery.none"},
        {.label = "设置", .icon = XIAOTAI_ICON_SETTINGS, .intent = "gallery.none"},
    };
    static const xiaotai_view_item_t facts[] = {
        {.label = "倒计时", .detail = "还剩 04:59", .icon = XIAOTAI_ICON_TIMER},
        {.label = "下一个闹钟", .detail = "明天 07:30", .icon = XIAOTAI_ICON_ALARM},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CLOCK, .id = "g.desk", .title = "小钛", .status = "闹钟",
        .text = "下一个闹钟 明天 07:30", .items = facts + 1, .item_count = 1, .actions = desk,
        .action_count = COUNT(desk), .flags = XIAOTAI_VIEW_CALENDAR}, "desk-alarm");
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CLOCK, .id = "g.desk2", .title = "小钛", .status = "闹钟",
        .text = "倒计时 还剩 04:59", .items = facts, .item_count = COUNT(facts), .actions = desk,
        .action_count = COUNT(desk), .flags = XIAOTAI_VIEW_CALENDAR}, "desk-countdown");
    static const xiaotai_view_action_t shortcut[] = {
        {.icon = XIAOTAI_ICON_MENU, .intent = "gallery.none"}, {.icon = XIAOTAI_ICON_PHONE, .intent = "gallery.none"},
        {.icon = XIAOTAI_ICON_PREVIOUS, .intent = "gallery.none"}, {.icon = XIAOTAI_ICON_NEXT, .intent = "gallery.none"},
    };
    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_CLOCK, .id = "g.desk3", .title = "小钛", .status = "待唤醒",
        .indicators = weak, .indicator_count = 1, .actions = shortcut, .action_count = COUNT(shortcut),
        .flags = XIAOTAI_VIEW_CALENDAR}, "desk-plain");
}

void product_screens_gallery(void)
{
    home_pages();
    setup_pages();
    list_pages();
    form_pages();
    call_pages();
    notice_pages();
}
