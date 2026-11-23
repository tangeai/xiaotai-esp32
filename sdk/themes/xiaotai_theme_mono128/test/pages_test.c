// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_ui.c::ui
// Replaces legacy: tests/xmini/test_guide.c::guide
#include "mono_canvas.h"
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/port/clock.h"
#include "xiaotai_clock_host.h"

#include <stdio.h>
#include <string.h>

static uint8_t frame[1024], expected[1024];
static mono_canvas_t reference = {expected, XIAOTAI_PIXEL_MONO1_VLSB};
static uint32_t now;
static unsigned present_count;

static uint32_t fake_now(void) { return now; }

static xiaotai_err_t present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    (void)self;
    if (!pixels || area->x || area->y || area->width != 128 || area->height != 64)
        return XIAOTAI_ERR_INVALID_ARG;
    memcpy(frame, pixels, sizeof(frame));
    ++present_count;
    return XIAOTAI_OK;
}

static void at(uint32_t ms)
{
    now = ms;
    ui_driver_step(ui_display_at(0), now);
}

static void show(const xiaotai_view_t *view)
{
    ui_view_accept(view);
    at(now);
}

static bool dot(const uint8_t *pixels, unsigned x, unsigned y)
{
    return (pixels[y / 8U * 128U + x] & (1U << (y % 8U))) != 0;
}

static bool same_region(unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    for (unsigned y = y0; y < y1; ++y)
        for (unsigned x = x0; x < x1; ++x)
            if (dot(frame, x, y) != dot(expected, x, y)) return false;
    return true;
}

static void clean_expected(void) { memset(expected, 0, sizeof(expected)); }

static int home_and_notice(void)
{
    static const xiaotai_view_item_t home_menu[] = {
        {.label = "AI对话", .intent = "go.talk"}, {.label = "微信通话", .intent = "go.wechat"},
    };
    const xiaotai_view_t home = {.kind = XIAOTAI_VIEW_HOME, .title = "小钛", .status = "在线",
                                .items = home_menu, .item_count = 2};
    show(&home);
    CHECK(dot(frame, 0, 15) && dot(frame, 127, 29) && !dot(frame, 0, 30));
    xiaotai_view_t notice = {.kind = XIAOTAI_VIEW_NOTICE, .title = "无线网络", .status = "192.168.6.1",
                            .subtitle = "XiaoTai-CA88", .text = "手机连接设备热点"};
    show(&notice);
    clean_expected();
    (void)mono_window(&reference, 0, 0, notice.title, 128, 0);
    mono_center(&reference, 15, notice.text);
    mono_center(&reference, 27, notice.subtitle);
    mono_center(&reference, 39, notice.status);
    CHECK(same_region(0, 0, 128, 12) && same_region(0, 14, 128, 51));

    notice.id = "multiline";
    notice.title = "正在验证";
    notice.status = notice.subtitle = NULL;
    notice.text = "第一行\n第二行\n第三行";
    show(&notice);
    clean_expected();
    mono_center(&reference, 15, "第一行");
    mono_center(&reference, 27, "第二行");
    mono_center(&reference, 39, "第三行");
    CHECK(same_region(0, 14, 128, 51));

    notice.id = "stream";
    notice.title = "实时查看";
    notice.text = "通话中";
    notice.timer_mode = XIAOTAI_TIMER_ELAPSED;
    notice.timer_s = 59;
    show(&notice);
    clean_expected();
    (void)mono_window(&reference, 88, 0, "00:59", 40, 0);
    mono_center(&reference, 32, notice.text);
    CHECK(same_region(88, 0, 128, 12) && same_region(0, 15, 128, 51));
    at(now + 1000U);
    clean_expected();
    (void)mono_window(&reference, 88, 0, "01:00", 40, 0);
    CHECK(same_region(88, 0, 128, 12));
    notice.status = "状态也要保留";
    show(&notice);
    clean_expected();
    mono_center(&reference, 39, notice.status);
    CHECK(same_region(0, 39, 128, 51));
    return 0;
}

static int talk_and_call(void)
{
    at(0);
    /* Production member layout in companion with_ai_talk.c:237-246:
     * status is a state; text and speaker carry the independent caption. */
    xiaotai_view_t talk = {.kind = XIAOTAI_VIEW_TALK, .id = "talk", .title = "AI对话",
                          .status = "请说话", .text = "我是实时字幕", .phase = XIAOTAI_TALK_LISTENING,
                          .speaker = XIAOTAI_SPEAKER_ASSISTANT};
    show(&talk);
    clean_expected();
    (void)mono_window(&reference, 0, 0, talk.title, 128, 0);
    mono_center(&reference, 26, talk.status);
    CHECK(same_region(0, 0, 128, 12) && same_region(0, 14, 128, 51));
    CHECK(dot(frame, 50, 52) && !dot(frame, 62, 52));
    const unsigned before = present_count;
    talk.text = "这句也不能占用单色屏";
    show(&talk);
    CHECK(present_count == before);
    at(100);
    at(200);
    at(300);
    at(301);
    CHECK(!dot(frame, 50, 52) && dot(frame, 62, 52) && present_count == before + 1);

    talk.phase = XIAOTAI_TALK_SPEAKING;
    talk.status = "正在回答";
    talk.timer_mode = XIAOTAI_TIMER_ELAPSED;
    talk.timer_s = 61;
    show(&talk);
    clean_expected();
    (void)mono_window(&reference, 88, 0, "01:01", 40, 0);
    mono_center(&reference, 26, talk.status);
    CHECK(same_region(88, 0, 128, 12) && same_region(0, 14, 128, 45));
    for (unsigned i = 0; i < 9; ++i) CHECK(dot(frame, 30 + 8 * i, 59));
    const unsigned bars = present_count;
    at(401); at(501); at(601); at(602);
    CHECK(present_count == bars + 1);
    talk.phase = XIAOTAI_TALK_IDLE;
    talk.status = "正在结束";
    talk.timer_mode = XIAOTAI_TIMER_NONE;
    show(&talk);
    clean_expected();
    CHECK(same_region(0, 40, 128, 64) && !ui_display_at(0)->driver.tick);

    static const xiaotai_view_action_t keys[] = {
        {.label = "双击接听", .intent = "companion.call.accept", .role = XIAOTAI_ROLE_PRIMARY,
         .icon = XIAOTAI_ICON_ACCEPT},
        {.label = "长按拒绝", .intent = "companion.call.reject", .role = XIAOTAI_ROLE_BACK,
         .icon = XIAOTAI_ICON_HANGUP},
    };
    at(800);
    xiaotai_view_t call = {.kind = XIAOTAI_VIEW_CALL, .id = "call", .title = "设备通话",
                           .subtitle = "长的联系人名称用于验证窗口滚动", .text = "来电",
                           .timer_mode = XIAOTAI_TIMER_ELAPSED, .timer_s = 6001, .actions = keys, .action_count = 2};
    show(&call);
    clean_expected();
    (void)mono_window(&reference, 88, 0, "00:01", 40, 0);
    (void)mono_window(&reference, 0, 18, call.subtitle, 128, 8);
    mono_center(&reference, 34, "来电");
    mono_center(&reference, 51, "双击接听 长按拒绝");
    CHECK(same_region(88, 0, 128, 12) && same_region(0, 14, 128, 64));
    ui_display_t *const display = ui_display_at(0);
    xiaotai_ui_control_t controls[2];
    CHECK(display->theme->controls(display->theme_state, controls, 2) == 2U);
    CHECK(controls[0].element == XIAOTAI_UI_ELEMENT_ACTION && controls[0].index == 0 && controls[0].y == 51U);
    CHECK(controls[1].element == XIAOTAI_UI_ELEMENT_ACTION && controls[1].index == 1 && controls[1].y == 51U);
    static const xiaotai_view_action_t active[] = {
        {.label = "关闭麦克风", .intent = "companion.call.mute", .role = XIAOTAI_ROLE_NONE, .icon = XIAOTAI_ICON_MIC},
        {.intent = "companion.call.hangup", .role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_HANGUP},
    };
    call.text = "通话中";
    call.actions = active;
    show(&call);
    clean_expected();
    CHECK(same_region(0, 50, 128, 64) && display->theme->controls(display->theme_state, controls, 2) == 0U);
    return 0;
}

static int codes_progress_and_clock(void)
{
    at(1000);
    xiaotai_view_t code = {.kind = XIAOTAI_VIEW_CODE, .title = "设备绑定", .text = "123456",
                           .subtitle = "微信搜索小钛", .timer_mode = XIAOTAI_TIMER_REMAINING, .timer_s = 2};
    show(&code);
    clean_expected();
    mono_center5(&reference, 20, code.text, 2);
    mono_center(&reference, 36, code.subtitle);
    mono_center(&reference, 50, "2秒后过期");
    CHECK(same_region(0, 14, 128, 64));
    const unsigned first = present_count;
    at(1999);
    CHECK(present_count == first); /* remaining fraction still rounds up */
    at(2000);
    clean_expected();
    mono_center(&reference, 50, "1秒后过期");
    CHECK(same_region(0, 50, 128, 64));
    at(3000);
    clean_expected();
    CHECK(same_region(0, 15, 128, 34) && same_region(0, 50, 128, 64));
    code.image = "mono.demo";
    code.flags = XIAOTAI_VIEW_QR;
    code.text = "https://example.invalid/a";
    code.subtitle = NULL;
    code.timer_mode = XIAOTAI_TIMER_NONE;
    show(&code);
    CHECK(dot(frame, 59, 16) && dot(frame, 68, 16) && !dot(frame, 58, 16));
    code.image = "color.demo";
    show(&code);
    CHECK(same_region(0, 14, 128, 64));

    xiaotai_view_t progress = {.kind = XIAOTAI_VIEW_PROGRESS, .title = "联网模块", .text = "正在连接服务",
                               .flags = XIAOTAI_VIEW_PERCENT, .progress = 25};
    show(&progress);
    CHECK(dot(frame, 8, 36) && dot(frame, 119, 43) && dot(frame, 35, 37) && !dot(frame, 36, 37));
    progress.flags = 0;
    show(&progress);
    CHECK(!dot(frame, 8, 36) && !dot(frame, 119, 43));

    const xiaotai_view_t clock = {.kind = XIAOTAI_VIEW_CLOCK, .title = "时间"};
    show(&clock);
    clean_expected();
    mono_center5(&reference, 20, "--:--", 2);
    CHECK(same_region(0, 14, 128, 64));
    CHECK(xiaotai_clock_set_unix_ms(1767270845000LL) == XIAOTAI_OK);
    at(4000);
    xiaotai_clock_fields_t local;
    CHECK(xiaotai_clock_local(1767270845000LL, &local) == XIAOTAI_OK);
    CHECK(local.year == 2026 && local.month == 1 && local.day == 1);
    /* Fixture UTC date is deterministic; the component uses core clock fields. */
    CHECK(local.hour == 12 && local.minute == 34);
    clean_expected();
    mono_center5(&reference, 20, "12:34", 2);
    mono_center(&reference, 36, "2026-01-01");
    CHECK(same_region(0, 14, 128, 64));
    return 0;
}

static int forms(void)
{
    static const xiaotai_view_action_t keys[] = {
        {.label = "单击改数 双击下一位", .intent = "key.confirm", .role = XIAOTAI_ROLE_PRIMARY},
        {.label = "不进提示", .intent = "key.back", .role = XIAOTAI_ROLE_BACK},
    };
    xiaotai_view_field_t field = {.label = "密码", .kind = XIAOTAI_FIELD_DIGITS, .intent = "key.edit",
                                 .flags = XIAOTAI_FIELD_MASKED, .max = 4, .text = "1234"};
    xiaotai_view_t view = {.kind = XIAOTAI_VIEW_FORM, .title = "房间密码", .fields = &field, .field_count = 1,
                           .actions = keys, .action_count = 2};
    show(&view);
    clean_expected();
    mono_text5(&reference, 40, 20, "1***", 2);
    mono_rect(&reference, 40, 36, 10, 2, MONO_SET);
    mono_center(&reference, 48, keys[0].label);
    CHECK(same_region(0, 14, 128, 64));
    const unsigned shown = present_count;
    field.text = "1567";
    show(&view);
    CHECK(present_count == shown); /* hidden digits never affect the frame */
    /* The core owns the cursor; exercise the theme with its next selection. */
    ui_display_t *display = ui_display_at(0);
    display->scene.page.cursor = 2;
    display->scene_pending[XIAOTAI_LAYER_PAGE] = XIAOTAI_CHANGED_FIELDS;
    at(now);
    clean_expected();
    mono_text5(&reference, 40, 20, "**6*", 2);
    mono_rect(&reference, 64, 36, 10, 2, MONO_SET);
    CHECK(same_region(0, 14, 128, 46));
    view.id = "plain-digits";
    field.flags = 0;
    field.max = 6;
    field.text = "123456";
    show(&view);
    clean_expected();
    mono_text5(&reference, 28, 20, "123456", 2);
    mono_rect(&reference, 28, 36, 10, 2, MONO_SET);
    CHECK(same_region(0, 14, 128, 46));
    field.max = 16;
    field.text = "1234567890123456";
    show(&view);
    clean_expected();
    mono_text5(&reference, 16, 20, field.text, 1);
    mono_rect(&reference, 16, 36, 5, 2, MONO_SET);
    CHECK(same_region(0, 14, 128, 46));

    static const char *const options[] = {"甲", "乙"};
    const xiaotai_view_field_t fields[] = {
        {.label = "隐藏", .kind = XIAOTAI_FIELD_TEXT, .state = XIAOTAI_STATE_HIDDEN},
        {.label = "音量", .kind = XIAOTAI_FIELD_STEPPER, .value = -7, .text = "dB"},
        {.label = "开关", .kind = XIAOTAI_FIELD_SWITCH, .value = 1},
        {.label = "选择", .kind = XIAOTAI_FIELD_CHOICE, .value = 1, .options = options, .option_count = 2},
    };
    view.fields = fields;
    view.field_count = 4;
    view.focus = 4;
    show(&view);
    const xiaotai_theme_t *theme = ui_display_at(0)->theme;
    xiaotai_ui_control_t controls[3];
    CHECK(theme->controls(ui_display_at(0)->theme_state, controls, 3) == 3);
    CHECK(controls[0].index == 1 && controls[1].index == 2 && controls[2].index == 3);
    CHECK(controls[0].element == XIAOTAI_UI_ELEMENT_FIELD && controls[2].y == 47);
    clean_expected();
    (void)mono_window(&reference, 82, 17, "-7 dB", 40, 0);
    (void)mono_window(&reference, 106, 33, "ON", 16, 0);
    (void)mono_window(&reference, 110, 49, "乙", 12, 0);
    mono_rect(&reference, 0, 47, 128, 15, MONO_INVERT);
    CHECK(same_region(80, 14, 128, 64));
    return 0;
}

static int bounded_pages(void)
{
    const uint8_t blank[sizeof(frame)] = {0};
    char label[65];
    memset(label, 'C', sizeof(label) - 1U);
    label[sizeof(label) - 1U] = 0;
    xiaotai_view_item_t rows[5];
    for (size_t i = 0; i < sizeof(rows) / sizeof(*rows); ++i)
        rows[i] = (xiaotai_view_item_t){.label = label, .intent = "select", .value = (int32_t)i};
    static const xiaotai_view_field_t digits = {
        .label = "Code", .kind = XIAOTAI_FIELD_DIGITS, .intent = "edit", .max = 6, .text = "012345",
    };
    xiaotai_view_t views[] = {
        {.kind = XIAOTAI_VIEW_HOME, .items = rows, .item_count = 5},
        {.kind = XIAOTAI_VIEW_LIST, .items = rows, .item_count = 5, .status = "2/3"},
        {.kind = XIAOTAI_VIEW_TALK, .text = label},
        {.kind = XIAOTAI_VIEW_CALL, .text = label, .subtitle = label},
        {.kind = XIAOTAI_VIEW_NOTICE, .text = label},
        {.kind = XIAOTAI_VIEW_FORM, .fields = &digits, .field_count = 1},
    };
    for (size_t page = 0; page < sizeof(views) / sizeof(*views); ++page) {
        xiaotai_view_t *const view = &views[page];
        view->title = label;
        for (unsigned focus = 0; focus < (view->item_count ? view->item_count : 1U); ++focus) {
            char name[28];
            snprintf(name, sizeof(name), "bounded.%u.%u", (unsigned)page, focus);
            view->id = name;
            view->focus = (uint8_t)(focus + 1U);
            const unsigned before = present_count;
            show(view);
            CHECK(present_count == before + 1U && memcmp(frame, blank, sizeof(frame)));
        }
    }
    return 0;
}

static int binding_guide(void)
{
    static const char *const digits[] = {"000000", "111111", "888888", "999999", "012345", "987654"};
    xiaotai_view_t code = {.kind = XIAOTAI_VIEW_CODE, .id = "guide.code", .title = "设备绑定",
        .subtitle = "微信搜索小钛", .timer_mode = XIAOTAI_TIMER_REMAINING, .timer_s = 99};
    for (size_t i = 0; i < sizeof(digits) / sizeof(*digits); ++i) {
        code.text = digits[i];
        show(&code);
        clean_expected();
        mono_center5(&reference, 20, digits[i], 2);
        mono_center(&reference, 36, "微信搜索小钛");
        mono_center(&reference, 50, "99秒后过期");
        CHECK(same_region(0, 14, 128, 64));
    }

    show(&(xiaotai_view_t){.kind = XIAOTAI_VIEW_PROGRESS, .title = "设备初始化", .text = "正在准备设备服务"});
    code.id = "guide.wrap";
    code.text = "000001";
    code.timer_s = 1;
    /* Start 1000 ms before 50 across uint32_t wrap, as in the old guide oracle. */
    at(UINT32_MAX - 949U);
    show(&code);
    clean_expected();
    mono_center5(&reference, 20, "000001", 2);
    mono_center(&reference, 50, "1秒后过期");
    CHECK(same_region(0, 15, 128, 34) && same_region(0, 50, 128, 64));
    at(49U);
    CHECK(same_region(0, 15, 128, 34) && same_region(0, 50, 128, 64));
    clean_expected();
    at(50U);
    CHECK(same_region(0, 15, 128, 34) && same_region(0, 50, 128, 64));
    at(51U);
    CHECK(same_region(0, 15, 128, 34) && same_region(0, 50, 128, 64));

    static const xiaotai_view_action_t retry = {.label = "双击重试", .intent = "fixture.retry",
        .role = XIAOTAI_ROLE_PRIMARY};
    code.id = "guide.expired";
    code.text = "------";
    code.status = "验证码已过期";
    code.timer_mode = XIAOTAI_TIMER_NONE;
    code.actions = &retry;
    code.action_count = 1;
    show(&code);
    clean_expected();
    mono_center5(&reference, 20, "------", 2);
    mono_center(&reference, 51, retry.label);
    CHECK(same_region(0, 15, 128, 34) && same_region(0, 51, 128, 64));

    static const xiaotai_view_t stages[] = {
        {.kind = XIAOTAI_VIEW_PROGRESS, .title = "联网模块", .text = "正在启动"},
        {.kind = XIAOTAI_VIEW_NOTICE, .title = "无线网络", .text = "手机连接设备热点",
         .subtitle = "XiaoTai-CA88", .status = "192.168.6.1"},
        {.kind = XIAOTAI_VIEW_NOTICE, .title = "连接 Wi-Fi", .text = "正在验证 Wi-Fi 连接"},
        {.kind = XIAOTAI_VIEW_PROGRESS, .title = "设备初始化", .text = "正在准备设备服务"},
        {.kind = XIAOTAI_VIEW_CODE, .title = "设备绑定", .text = "012345", .subtitle = "微信搜索小钛"},
        {.kind = XIAOTAI_VIEW_CODE, .title = "设备绑定", .text = "------", .status = "正在验证授权"},
        {.kind = XIAOTAI_VIEW_HOME, .title = "小钛", .status = "在线"},
        {.kind = XIAOTAI_VIEW_NOTICE, .title = "连接 Wi-Fi", .text = "正在重连已保存的 Wi-Fi",
         .actions = &retry, .action_count = 1},
        {.kind = XIAOTAI_VIEW_PROGRESS, .title = "操作未完成", .text = "初始化失败", .status = "ERR FFFF",
         .actions = &retry, .action_count = 1},
    };
    clean_expected();
    for (size_t i = 0; i < sizeof(stages) / sizeof(*stages); ++i) {
        const unsigned before = present_count;
        show(&stages[i]);
        CHECK(present_count == before + 1U && !same_region(0, 0, 128, 64));
    }
    return 0;
}

int main(void)
{
    static const uint8_t dots[] = {0x80, 0x40};
    static const uint8_t rgb[] = {0xff, 0xff};
    static const xiaotai_ui_image_t mono = {"mono.demo", 10, 1, XIAOTAI_IMAGE_MONO1, dots, sizeof(dots)};
    static const xiaotai_ui_image_t color = {"color.demo", 1, 1, XIAOTAI_IMAGE_RGB565, rgb, sizeof(rgb)};
    const xiaotai_display_desc_t desc = ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    ui_fake_board_set_displays(&desc, 1);
    ((xiaotai_display_t *)ui_fake_display(0))->present = present;
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    CHECK(xiaotai_engine_ui_init() == XIAOTAI_OK);
    CHECK(xiaotai_engine_theme_mono128_init() == XIAOTAI_OK);
    CHECK(xiaotai_ui_image_register(&mono) == XIAOTAI_OK);
    CHECK(xiaotai_ui_image_register(&color) == XIAOTAI_OK);
    xiaotai_clock_host_reset();
    static const char *const servers[] = {"clock.fixture.invalid"};
    CHECK(xiaotai_clock_init(&(xiaotai_clock_config_t){.servers = servers, .server_count = 1}) == XIAOTAI_OK);
    ui_core_set_clock(fake_now);
    ui_driver_manual(true);
    xiaotai_engine_ui_start();
    RUN(home_and_notice);
    RUN(talk_and_call);
    RUN(codes_progress_and_clock);
    RUN(forms);
    RUN(binding_guide);
    RUN(bounded_pages);
    xiaotai_ui_reset();
    return 0;
}
