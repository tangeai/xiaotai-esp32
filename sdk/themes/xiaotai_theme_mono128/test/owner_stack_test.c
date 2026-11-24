// SPDX-License-Identifier: MIT
#include "host_stack.h"
#include "mono_theme_private.h"
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/os.h"
#include "xiaotai/theme_mono128.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

_Static_assert(!XIAOTAI_UI_TASK, "test uses the owner's timer instead of a UI task");

static atomic_uint time_ms = 1000;
static xiaotai_timer_fn_t timer_fn;
static void *timer_ctx;
static uint32_t timer_due, timer_serial;
static xiaotai_signal_t synchronized;
static unsigned writes;
static size_t peak;
static bool reject_next;
static uint32_t fingerprint = UINT32_C(2166136261);

#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
static union {
    struct {
        xiaotai_view_item_t items[XIAOTAI_VIEW_ITEMS_MAX];
        xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX];
        xiaotai_view_field_t fields[XIAOTAI_VIEW_FIELDS_MAX];
    };
    uint8_t pixels[XIAOTAI_THEME_MONO128_FRAME_BYTES];
} shared;
static bool publishing;

static uint8_t *offer(size_t bytes)
{
    assert(xiaotai_on_owner());
    return !publishing && bytes == sizeof(shared.pixels) ? shared.pixels : NULL;
}
#endif

static xiaotai_err_t app_init(void)
{
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    assert(xiaotai_ui_owner_frame_offer(offer) == XIAOTAI_OK);
    assert(xiaotai_ui_owner_frame_offer(offer) == XIAOTAI_ERR_INVALID_STATE);
#endif
    return xiaotai_engine_theme_mono128_init();
}

int64_t __wrap_xiaotai_time_us(void) { return (int64_t)atomic_load(&time_ms) * 1000; }

xiaotai_err_t __wrap_xiaotai_timer_start(uint32_t delay, xiaotai_timer_fn_t fn, void *ctx, uint32_t *id)
{
    assert(xiaotai_on_owner() && !timer_fn);
    timer_due = atomic_load(&time_ms) + delay;
    timer_fn = fn;
    timer_ctx = ctx;
    *id = ++timer_serial;
    return XIAOTAI_OK;
}

void __wrap_xiaotai_timer_cancel(uint32_t id)
{
    assert(xiaotai_on_owner());
    if (id == timer_serial) timer_fn = NULL;
}

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *, xiaotai_task_t *);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *out)
{
    assert(strcmp(cfg->name, "xt_ui") && "owner-timer colourless display must not create a UI task");
    return __real_xiaotai_task_create(cfg, out);
}

static xiaotai_err_t present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    (void)self;
    assert(xiaotai_on_owner() && !area->x && !area->y && area->width == 128 && area->height == 64);
    const uint8_t *frame = pixels;
    assert(frame && frame[128] & (1U << 5U)); /* real page header */
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    assert(frame == shared.pixels && frame == xiaotai_ui_host_owner_frame(&ui_display_at(0)->host));
#endif
    ++writes;
    if (reject_next) {
        reject_next = false;
        return XIAOTAI_FAIL;
    }
    for (size_t i = 0; i < XIAOTAI_THEME_MONO128_FRAME_BYTES; ++i)
        fingerprint = (fingerprint ^ frame[i]) * UINT32_C(16777619);
    return XIAOTAI_OK;
}

typedef struct {
    void (*fn)(void *);
    void *ctx;
} job_t;

static void job_runner(void *opaque)
{
    const job_t job = *(const job_t *)opaque;
    job.fn(job.ctx);
    xiaotai_signal_give(&synchronized);
}

static void run(void (*fn)(void *), void *ctx)
{
    job_t job = {fn, ctx};
    assert(xiaotai_run(job_runner, &job) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&synchronized, 5000U));
}

static void setup(void)
{
    const xiaotai_display_desc_t desc = ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    ui_fake_board_set_displays(&desc, 1);
    ((xiaotai_display_t *)ui_fake_display(0))->present = present;
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    assert(xiaotai_signal_init(&synchronized) == XIAOTAI_OK);
    static const xiaotai_module_t engine = {.name = "ui", .init = xiaotai_engine_ui_init,
                                            .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "fixture", .init = app_init};
    static const xiaotai_module_t *const modules[] = {&engine};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1,
                                               .applications = apps, .application_count = 1};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
}

static void fire(void *ctx)
{
    (void)ctx;
    assert(timer_fn && (int32_t)(atomic_load(&time_ms) - timer_due) >= 0);
    xiaotai_timer_fn_t pending = timer_fn;
    timer_fn = NULL;
    pending(timer_ctx);
}

static void measure(void *arg)
{
    const char *name = arg;
    size_t used = 0;
    assert(xiaotai_host_stack_measure(fire, NULL, &used));
    printf("mono owner %s: %zu / %u bytes\n", name, used, XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES);
    if (used > peak) peak = used;
    assert(used <= XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES);
}

static void show(void *arg)
{
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    xiaotai_view_t view = *(const xiaotai_view_t *)arg;
    if (view.item_count) {
        memcpy(shared.items, view.items, view.item_count * sizeof(*view.items));
        view.items = shared.items;
    }
    if (view.action_count) {
        memcpy(shared.actions, view.actions, view.action_count * sizeof(*view.actions));
        view.actions = shared.actions;
    }
    if (view.field_count) {
        memcpy(shared.fields, view.fields, view.field_count * sizeof(*view.fields));
        view.fields = shared.fields;
    }
    publishing = true;
    assert(!xiaotai_ui_host_owner_frame(&ui_display_at(0)->host));
    xiaotai_view_show(&view);
    assert(!ui_render_scene(ui_display_at(0)));
    publishing = false;
#else
    xiaotai_view_show(arg);
#endif
}

static void verify_frames(void *unused)
{
    (void)unused;
    ui_display_t *d = ui_display_at(0);
    uint8_t before[XIAOTAI_THEME_MONO128_FRAME_BYTES];
    memcpy(before, d->frame.last, sizeof(before));
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    assert(sizeof(mono_state_t) < XIAOTAI_THEME_MONO128_FRAME_BYTES);
    assert(d->frame.owner_workspace_bytes == sizeof(before));
    publishing = true;
    assert(!ui_render_scene(d));
    xiaotai_ui_capture_t blocked = {0};
    assert(ui_frame_capture(d, &blocked) == XIAOTAI_ERR_INVALID_STATE && !blocked.pixels);
    publishing = false;
#endif
    xiaotai_ui_capture_t shot = {0};
    const unsigned sent = writes;
    assert(ui_frame_capture(d, &shot) == XIAOTAI_OK);
    assert(shot.size == sizeof(before) && shot.pixels != d->frame.last &&
           !memcmp(shot.pixels, before, sizeof(before)));
    assert(writes == sent);

    const xiaotai_view_t next = {.kind = XIAOTAI_VIEW_HOME, .id = "later", .title = "设备"};
    xiaotai_view_show(&next);
    reject_next = true;
    fire(NULL);
    assert(writes == sent + 1U && d->frame.fault);
    assert(!memcmp(d->frame.last, before, sizeof(before)) && !memcmp(shot.pixels, before, sizeof(before)));
    assert(xiaotai_ui_refresh(0) == XIAOTAI_OK);
    fire(NULL);
    assert(writes == sent + 2U && !d->frame.fault);
    assert(memcmp(d->frame.last, before, sizeof(before)) && !memcmp(shot.pixels, before, sizeof(before)));
    xiaotai_ui_capture_free(&shot);
}

int main(void)
{
    static const char *const servers[] = {"clock.fixture.invalid"};
    assert(xiaotai_clock_init(&(xiaotai_clock_config_t){.servers = servers, .server_count = 1}) == XIAOTAI_OK);
    assert(xiaotai_clock_set_unix_ms(1767270845000LL) == XIAOTAI_OK);
    setup();
    static const char long_cn[] = "这是一段很长的联系人备注文字要在单色屏幕上滚动显示并保持字形边界完整";
    static const xiaotai_view_item_t contacts[] = {
        {.label = long_cn, .intent = "contact.show"}, {.label = "菜单", .intent = "menu.show"},
        {.label = "绑定", .intent = "bind.show"}, {.label = "设置", .intent = "settings.show"},
    };
    static const xiaotai_view_action_t actions[] = {
        {.label = "双击继续", .role = XIAOTAI_ROLE_PRIMARY, .intent = "continue"},
        {.label = "单击下一项", .role = XIAOTAI_ROLE_AUX, .intent = "next"},
        {.label = "长按返回", .role = XIAOTAI_ROLE_BACK, .intent = "back"},
    };
    static const xiaotai_view_field_t digit = {.kind = XIAOTAI_FIELD_DIGITS, .flags = XIAOTAI_FIELD_MASKED,
        .label = "密码", .intent = "edit", .text = "1234567890123456", .max = 16};
    static const xiaotai_view_field_t fields[] = {
        {.kind = XIAOTAI_FIELD_STEPPER, .label = "增益", .intent = "set", .text = "分贝", .value = INT32_MIN},
        {.kind = XIAOTAI_FIELD_STEPPER, .label = "音量", .intent = "set", .text = "%", .value = 85},
        {.kind = XIAOTAI_FIELD_SWITCH, .label = "声音开关", .intent = "toggle", .value = 1},
    };
    xiaotai_view_t cases[] = {
        {.kind = XIAOTAI_VIEW_HOME, .id = "home", .title = "小钛", .items = contacts, .item_count = 4},
        {.kind = XIAOTAI_VIEW_LIST, .id = "list", .title = "联系人", .items = contacts, .item_count = 4},
        {.kind = XIAOTAI_VIEW_TALK, .id = "talk", .title = "AI对话", .phase = XIAOTAI_TALK_SPEAKING,
            .status = "正在回答", .text = long_cn, .timer_mode = XIAOTAI_TIMER_ELAPSED, .timer_s = 61},
        {.kind = XIAOTAI_VIEW_CALL, .id = "call", .title = "设备通话", .subtitle = long_cn, .text = "来电",
            .timer_mode = XIAOTAI_TIMER_ELAPSED, .timer_s = 6001, .actions = actions, .action_count = 3},
        {.kind = XIAOTAI_VIEW_CODE, .id = "code", .title = "设备绑定", .subtitle = "微信搜索小钛",
            .text = "123456", .timer_mode = XIAOTAI_TIMER_REMAINING, .timer_s = INT32_MAX},
        {.kind = XIAOTAI_VIEW_NOTICE, .id = "notice", .title = "无线网络", .text = long_cn,
            .subtitle = long_cn, .status = "192.168.6.1", .actions = actions, .action_count = 3},
        {.kind = XIAOTAI_VIEW_PROGRESS, .id = "progress", .title = "联网模块", .text = long_cn,
            .flags = XIAOTAI_VIEW_PERCENT, .progress = 99, .actions = actions, .action_count = 3},
        {.kind = XIAOTAI_VIEW_CLOCK, .id = "clock", .title = "时间"},
        {.kind = XIAOTAI_VIEW_FORM, .id = "digits", .title = "房间密码", .fields = &digit,
            .field_count = 1, .actions = actions, .action_count = 3},
        {.kind = XIAOTAI_VIEW_FORM, .id = "fields", .title = "设置", .fields = fields, .field_count = 3},
    };
    const xiaotai_view_t toast = {.kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_TOAST,
                                 .text = "操作未执行", .duration_ms = 1500};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        atomic_store(&time_ms, (uint32_t)(2000U + i * 1000U));
        const unsigned before = writes;
        run(show, &cases[i]);
        run(measure, (void *)cases[i].id);
        assert(writes > before);
        if (i == 1) {
            /* With the long focused row still visible, redraw it and the
             * toast in the same measured owner-timer step. */
            run(show, (void *)&toast);
            run(measure, "list+toast");
            assert(writes > before + 1U);
        }
    }
    xiaotai_ui_info_t info;
    assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.ready && info.owner_timer);
    run(verify_frames, NULL);
    printf("mono owner sent fingerprint: %08x\n", (unsigned)fingerprint);
    printf("mono owner peak: %zu bytes\n", peak);
    xiaotai_ui_reset();
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&synchronized);
    return 0;
}
