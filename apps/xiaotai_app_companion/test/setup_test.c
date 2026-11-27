// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_runtime_wifi.c::runtime_wifi
// Replaces legacy: tests/xmini/test_guide.c::guide
// Replaces legacy: tests/regression/test_binding_lifecycle.py
// Replaces legacy: tests/regression/test_wifi_manual_disconnect.py
#include "fakes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/port/os.h"

static xiaotai_signal_t synchronized;
static uint32_t activity, quiet;

static void barrier(void *ctx) { xiaotai_signal_give(ctx); }

static void flush(void)
{
    for (unsigned n = 0; n < 3U; ++n) {
        assert(xiaotai_run(barrier, &synchronized) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&synchronized, 3000U));
    }
}

static void post(const char *intent, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_intent_post(intent, &args) == XIAOTAI_OK);
    flush();
}

static void notify(const char *name)
{
    assert(xiaotai_event_emit(name, NULL, NULL, 0) == XIAOTAI_OK);
    flush();
}

static void call(void (*run)(void *), void *ctx)
{
    assert(xiaotai_run(run, ctx) == XIAOTAI_OK);
    flush();
}

static void begin_quiet(void *ctx) { assert(xiaotai_quiesce_begin(ctx, &quiet) == XIAOTAI_OK); }
static void end_quiet(void *ctx) { (void)ctx; xiaotai_quiesce_end(quiet); }

static void begin_session(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t session = {.name = "session_fixture", .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_LINK};
    assert(xiaotai_activity_begin(&session, &activity) == XIAOTAI_OK);
}

static void end_session(void *ctx) { (void)ctx; xiaotai_activity_end(activity); }

static void boot_config(uint32_t needs, bool online, const xiaotai_app_companion_config_t *config)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = companion_fixture_init};
    static const xiaotai_module_t *const modules[] = {&probe};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&app};
    fixture = (companion_test_fixture_t){.needs = needs, .navigation_pages = true,
        .net.state = online ? XIAOTAI_NET_UP : XIAOTAI_NET_UNCONFIGURED,
        .binding.phase = online ? XIAOTAI_BINDING_BOUND : XIAOTAI_BINDING_UNKNOWN,
        .platform.phase = online ? XIAOTAI_PLATFORM_ONLINE : XIAOTAI_PLATFORM_STOPPED};
    const xiaotai_framework_config_t fw = {.modules = modules, .module_count = 1U,
        .applications = apps, .application_count = 1U, .available_needs = needs};
    assert(xiaotai_app_companion_configure(config) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&fw) == XIAOTAI_OK);
    flush();
}

static void boot(uint32_t needs, const char *portal, bool online)
{
    boot_config(needs, online, &(xiaotai_app_companion_config_t){.binding_url = portal});
}

static void stop(void)
{
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
}

static void page(const char *id, uint8_t kind, const char *text)
{
    assert(!strcmp(fixture.id, id) && !strcmp(companion.status.page, id) && fixture.kind == kind);
    if (text) assert(!strcmp(fixture.text, text));
}

static const xiaotai_view_action_t *find_action(uint8_t role)
{
    for (size_t n = 0; n < fixture.action_count; ++n) {
        if (fixture.actions[n].role == role) return &fixture.actions[n];
    }
    return NULL;
}

static void press(uint8_t role, const char *intent, const char *label, int32_t value)
{
    const xiaotai_view_action_t *action = find_action(role);
    assert(action && !strcmp(action->intent, intent) && action->value == value);
    if (label) assert(!strcmp(action->label, label));
    post(action->intent, NULL, action->value);
}

static bool legacy_manual_disconnect(void)
{
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH | XIAOTAI_NEED_KEY, "http://portal.invalid/bind", false);
    fixture.saved_credentials = true;
    fixture.net.state = XIAOTAI_NET_HELD;
    fixture.provision.link = fixture.net;
    fixture.provision.provision.reason = XIAOTAI_NET_PROVISION_REASON_DISCONNECTED;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    page("setup", XIAOTAI_VIEW_NOTICE, NULL);
    bool offer_begin = false, offers_reconnect = false;
    for (size_t n = 0; n < fixture.action_count; ++n) {
        const xiaotai_view_action_t *action = &fixture.actions[n];
        offer_begin |= !strcmp(action->intent, XIAOTAI_INTENT_PROVISIONING_BEGIN) &&
                       action->value == XIAOTAI_PROVISIONING_PREEMPT;
        offers_reconnect |= !strcmp(action->intent, XIAOTAI_INTENT_PROVISIONING_RETRY);
    }
    printf("manual Wi-Fi held before portal: begin=%u reconnect=%u message=%s\n",
           (unsigned)offer_begin, (unsigned)offers_reconnect, fixture.text);
    fixture.provision.phase = XIAOTAI_PROVISIONING_WAITING;
    strcpy(fixture.provision.provision.ap_ssid, "Workshop-AP");
    strcpy(fixture.provision.provision.url, "http://192.168.6.1");
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    page("setup", XIAOTAI_VIEW_NOTICE, "手机连接设备热点");
    assert(!fixture.provisioning_begins && !fixture.provisioning_retries);
    stop();
    return offer_begin && !offers_reconnect;
}

static void pointer(void)
{
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH | XIAOTAI_NEED_KEY;
    boot(needs, "http://portal.invalid/bind", false);
    page("setup", XIAOTAI_VIEW_NOTICE, "正在连接 Wi-Fi");
    assert(companion.status.setup && !strcmp(fixture.context, "setup") && !(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP));
    assert(fixture.activity_notes == (SETUP_TEST_OPTIONAL ? 1U : 0U));
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_PROVISIONING_BEGIN, "开启热点配网", XIAOTAI_PROVISIONING_PREEMPT);
    assert(fixture.provisioning_begins == 1U && fixture.provisioning_begin_flags == XIAOTAI_PROVISIONING_PREEMPT);
    fixture.net.control_error = XIAOTAI_FAIL;
    notify(XIAOTAI_EVENT_NET_STATE);
    page("setup", XIAOTAI_VIEW_NOTICE, "热点启动失败，请点击热点配网重试");
    fixture.net.control_error = XIAOTAI_OK;
    fixture.provision.phase = XIAOTAI_PROVISIONING_WAITING;
    strcpy(fixture.provision.provision.ap_ssid, "Workshop-AP");
    strcpy(fixture.provision.provision.ap_password, "fixture-private");
    strcpy(fixture.provision.provision.url, "http://192.168.9.1");
    fixture.provision.provision.reason = XIAOTAI_NET_PROVISION_REASON_UNCONFIGURED;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    page("setup", XIAOTAI_VIEW_NOTICE, "手机连接设备热点");
    assert(fixture.item_count == 3U && !strcmp(fixture.items[0].detail, "Workshop-AP") &&
           !strcmp(fixture.items[1].detail, "http://192.168.9.1") && !find_action(XIAOTAI_ROLE_BACK));
    for (size_t n = 0; n < fixture.item_count; ++n) assert(!strstr(fixture.items[n].detail, "fixture-private"));
    unsigned before = fixture.views;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    assert(fixture.views == before);

    fixture.provision.provision.reason = XIAOTAI_NET_PROVISION_REASON_DISCONNECTED;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    assert(!find_action(XIAOTAI_ROLE_BACK));
    strcpy(fixture.net.ssid, "uncommitted-candidate");
    notify(XIAOTAI_EVENT_NET_STATE);
    assert(!find_action(XIAOTAI_ROLE_BACK));
    fixture.net.ssid[0] = '\0';
    fixture.provision.provision.reason = XIAOTAI_NET_PROVISION_REASON_FAILING;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    assert(!find_action(XIAOTAI_ROLE_BACK));
    fixture.saved_credentials = true;
    notify(XIAOTAI_EVENT_NET_CREDENTIALS_CHANGED);
    assert(find_action(XIAOTAI_ROLE_BACK));
    fixture.net.control_pending = true;
    notify(XIAOTAI_EVENT_NET_STATE);
    assert(!find_action(XIAOTAI_ROLE_BACK));
    fixture.net.control_pending = false;
    fixture.provision.exclusive = true;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    assert(!find_action(XIAOTAI_ROLE_BACK));
    fixture.provision.exclusive = false;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    press(XIAOTAI_ROLE_BACK, XIAOTAI_INTENT_PROVISIONING_CANCEL, "取消", 0);
    assert(fixture.provisioning_cancels == 1U);
    fixture.saved_credentials = false;
    notify(XIAOTAI_EVENT_NET_CREDENTIALS_CHANGED);
    assert(!find_action(XIAOTAI_ROLE_BACK));
    const struct {xiaotai_provisioning_phase_t phase; const char *text;} wifi_phases[] = {
        {XIAOTAI_PROVISIONING_STARTING, "正在开启热点配网"},
        {XIAOTAI_PROVISIONING_VERIFYING, "正在验证 Wi-Fi 连接"},
        {XIAOTAI_PROVISIONING_REJECTED, "连接失败，请重试"},
        {XIAOTAI_PROVISIONING_STORING, "正在保存 Wi-Fi 设置"},
        {XIAOTAI_PROVISIONING_DONE, "Wi-Fi 已保存，等待重启"},
        {XIAOTAI_PROVISIONING_RESTARTING, "设备即将重启"},
    };
    for (size_t n = 0; n < sizeof(wifi_phases) / sizeof(wifi_phases[0]); ++n) {
        fixture.provision.phase = wifi_phases[n].phase;
        fixture.provision.restart_pending = wifi_phases[n].phase >= XIAOTAI_PROVISIONING_DONE;
        notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
        page("setup", XIAOTAI_VIEW_NOTICE, wifi_phases[n].text);
        /* No saved station for REJECTED here: a second submission uses
         * the portal, not a retry button the engine would always refuse. */
        assert(!find_action(XIAOTAI_ROLE_PRIMARY));
    }
    fixture.provision.restart_pending = false;
    fixture.provision.phase = XIAOTAI_PROVISIONING_STORE_FAILED;
    fixture.provision.provision.error = XIAOTAI_ERR_NO_MEM;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    assert(!strcmp(fixture.status, "ERR 0101") && !find_action(XIAOTAI_ROLE_BACK));
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_PROVISIONING_RETRY, "重试", 0);
    assert(fixture.provisioning_retries == 1U);

    fixture.net.state = XIAOTAI_NET_PREPARING;
    const struct {xiaotai_net_prepare_stage_t stage; const char *text;} stages[] = {
        {XIAOTAI_NET_PREPARE_CHECKING, "正在检查联网模块"},
        {XIAOTAI_NET_PREPARE_VERIFYING, "正在校验联网模块镜像"},
        {XIAOTAI_NET_PREPARE_WRITING, "正在更新联网模块"},
        {XIAOTAI_NET_PREPARE_ACTIVATING, "正在激活联网模块"},
        {XIAOTAI_NET_PREPARE_RESTARTING, "设备即将重启"},
    };
    for (size_t n = 0; n < sizeof(stages) / sizeof(stages[0]); ++n) {
        fixture.net.prepare.stage = stages[n].stage;
        fixture.net.prepare.percent = (uint8_t)(11U + n);
        notify(XIAOTAI_EVENT_NET_PREPARE);
        page("preparing", XIAOTAI_VIEW_PROGRESS, stages[n].text);
        assert(fixture.flags & XIAOTAI_VIEW_PERCENT && fixture.progress == 11U + n);
    }
    call(begin_quiet, XIAOTAI_QUIESCE_COPROCESSOR);
    page("maintenance", XIAOTAI_VIEW_PROGRESS, "正在维护联网模块");
    assert(!fixture.action_count);
    call(end_quiet, NULL);
    page("preparing", XIAOTAI_VIEW_PROGRESS, "设备即将重启");
    fixture.net.state = XIAOTAI_NET_BLOCKED;
    const struct {xiaotai_net_fault_t fault; const char *text;} faults[] = {
        {XIAOTAI_NET_FAULT_INTERRUPTED, "上次更新未完成"}, {XIAOTAI_NET_FAULT_NO_LINK, "联网模块未就绪"},
        {XIAOTAI_NET_FAULT_BAD_IMAGE, "联网模块镜像无效"}, {XIAOTAI_NET_FAULT_UNSUPPORTED, "不支持自动升级"},
        {XIAOTAI_NET_FAULT_POWER_CYCLE, "请断电后重新启动"},
    };
    for (size_t n = 0; n < sizeof(faults) / sizeof(faults[0]); ++n) {
        fixture.net.prepare.fault = faults[n].fault;
        notify(XIAOTAI_EVENT_NET_STATE);
        page("preparing", XIAOTAI_VIEW_PROGRESS, faults[n].text);
    }
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_PROVISIONING_RETRY, "重试", 0);
    assert(fixture.provisioning_retries == 2U);

    fixture.net.state = XIAOTAI_NET_UP;
    notify(XIAOTAI_EVENT_NET_UP);
    page("setup", XIAOTAI_VIEW_NOTICE, "Wi-Fi 设置未保存，请重试");
    fixture.provision.phase = XIAOTAI_PROVISIONING_NONE;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    page("binding", XIAOTAI_VIEW_CODE, "------"); /* UNKNOWN without an identity */
    assert(!strcmp(fixture.items[0].label, "正在获取验证码"));
    const struct { xiaotai_binding_phase_t phase; const char *text; } binding_phases[] = {
        {XIAOTAI_BINDING_NEEDED, "正在获取验证码"}, {XIAOTAI_BINDING_PREPARING, "正在获取验证码"},
        {XIAOTAI_BINDING_REPORTING, "正在获取验证码"}, {XIAOTAI_BINDING_CONNECTING, "正在获取验证码"},
        {XIAOTAI_BINDING_STORING, "正在验证授权"}, {XIAOTAI_BINDING_CONFIRMING, "正在验证授权"},
        {XIAOTAI_BINDING_FAILED, "绑定未完成"},
    };
    for (size_t n = 0; n < sizeof(binding_phases) / sizeof(binding_phases[0]); ++n) {
        fixture.binding.phase = binding_phases[n].phase;
        notify(XIAOTAI_EVENT_BINDING_CHANGED);
        page("binding", XIAOTAI_VIEW_CODE, "------");
        assert(!strcmp(fixture.items[0].label, binding_phases[n].text) && fixture.timer_mode == XIAOTAI_TIMER_NONE);
    }
    assert(find_action(XIAOTAI_ROLE_PRIMARY) &&
           !strcmp(find_action(XIAOTAI_ROLE_PRIMARY)->intent, XIAOTAI_INTENT_BINDING_RETRY));
    fixture.binding.phase = XIAOTAI_BINDING_WAITING;
    strcpy(fixture.binding.verification, "123456");
    fixture.binding.seconds_left = 31U;
    notify(XIAOTAI_EVENT_BINDING_CODE);
    page("binding", XIAOTAI_VIEW_CODE, "123456");
    assert(!strcmp(fixture.subtitle, "http://portal.invalid/bind") && !strcmp(fixture.status, "网络已连接"));
    assert(fixture.timer_mode == XIAOTAI_TIMER_REMAINING && fixture.timer_s == 31);
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_BINDING_ANNOUNCE, "重播验证码", 0);
    assert(fixture.binding_announcements == 1U);
    fixture.binding.phase = XIAOTAI_BINDING_EXPIRED;
    notify(XIAOTAI_EVENT_BINDING_CHANGED);
    assert(!strcmp(fixture.items[0].label, "验证码已过期"));
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_BINDING_RETRY, "重试", 0);
    assert(fixture.binding_retries == 1U);

    fixture.binding.phase = XIAOTAI_BINDING_UNKNOWN;
    strcpy(fixture.binding.device_id, "stored-id");
    fixture.platform.phase = XIAOTAI_PLATFORM_DISCOVERY;
    notify(XIAOTAI_EVENT_BINDING_CHANGED);
    page("preparing", XIAOTAI_VIEW_PROGRESS, "正在准备设备服务");
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_CONNECTING;
    notify(XIAOTAI_EVENT_PLATFORM_DISCOVERED);
    page("preparing", XIAOTAI_VIEW_PROGRESS, "正在连接设备服务");
    fixture.platform.phase = XIAOTAI_PLATFORM_PAUSED;
    fixture.platform.last_error = XIAOTAI_ERR_TIMEOUT;
    notify(XIAOTAI_EVENT_PLATFORM_PAUSED);
    page("preparing", XIAOTAI_VIEW_PROGRESS, "初始化失败");
    assert(!strcmp(fixture.status, "ERR 0107"));
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_APP_COMPANION_SESSION, "重试", 0);
    assert(fixture.binding_retries == 2U);
    post(XIAOTAI_INTENT_APP_COMPANION_SESSION, "retry", 13);
    assert(fixture.binding_retries == 2U && fixture.last_refusal.value == XIAOTAI_ERR_INVALID_STATE);

    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    fixture.rtc_preparing = true;
    notify(XIAOTAI_EVENT_PLATFORM_ONLINE);
    if (SETUP_TEST_OPTIONAL) page("preparing", XIAOTAI_VIEW_PROGRESS, "正在初始化通话服务");
    else page("home", XIAOTAI_VIEW_HOME, NULL);
    fixture.rtc_preparing = false;
    notify(XIAOTAI_EVENT_PLATFORM_ONLINE);
    page("home", XIAOTAI_VIEW_HOME, NULL);
    assert(!companion.status.setup && !strcmp(fixture.context, "idle"));
    fixture.rtc_preparing = true;
    notify(XIAOTAI_EVENT_PLATFORM_ONLINE);
    page("home", XIAOTAI_VIEW_HOME, NULL); /* a later service recovery is not first-boot setup */
    fixture.rtc_preparing = false;

    post(XIAOTAI_INTENT_APP_COMPANION_SHOW, "status", 0);
    page("status", XIAOTAI_VIEW_NOTICE, NULL);
    call(begin_session, NULL);
    fixture.net.state = XIAOTAI_NET_UNCONFIGURED;
    notify(XIAOTAI_EVENT_NET_DOWN);
    page("status", XIAOTAI_VIEW_NOTICE, NULL); /* offline but activity still owns its session */
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.provision.phase = XIAOTAI_PROVISIONING_STARTING;
    fixture.platform.phase = XIAOTAI_PLATFORM_PAUSED;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    page("status", XIAOTAI_VIEW_NOTICE, NULL); /* attached + bound + active also blocks 3–5 */
    call(end_session, NULL);
    page("setup", XIAOTAI_VIEW_NOTICE, "正在开启热点配网");
    post(XIAOTAI_INTENT_APP_COMPANION_SHOW, "settings", 0);
    assert(!strcmp(fixture.id, "setup") && fixture.last_refusal.value == XIAOTAI_ERR_BUSY && !fixture.talk_stops);
    post(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(!strcmp(fixture.id, "setup"));
    fixture.provision.phase = XIAOTAI_PROVISIONING_NONE;
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    page("home", XIAOTAI_VIEW_HOME, NULL);
    assert(companion.depth == 1U && !companion.status.setup); /* no stale boot panel after a prior online */
    stop();
}

static void keys(void)
{
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY, NULL, false);
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_WAITING;
    strcpy(fixture.binding.verification, "123456");
    fixture.binding.seconds_left = 12U;
    notify(XIAOTAI_EVENT_NET_UP);
    page("binding", XIAOTAI_VIEW_CODE, "123456");
    assert(!fixture.item_count && !strcmp(fixture.title, "设备绑定") && !strcmp(fixture.subtitle, "微信搜索小钛"));
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_BINDING_ANNOUNCE, "重播验证码", 0);
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_PAUSED;
    fixture.platform.last_error = XIAOTAI_FAIL;
    notify(XIAOTAI_EVENT_PLATFORM_PAUSED);
    page("preparing", XIAOTAI_VIEW_PROGRESS, "初始化失败");
    assert(!strcmp(fixture.title, "操作未完成") && !strcmp(fixture.status, "ERR FFFF"));
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_APP_COMPANION_SESSION, "双击重试", 0);
    assert(fixture.binding_retries == 1U && fixture.binding_announcements == 1U);
    stop();
}

static void claim_code(void *out)
{
    xiaotai_binding_status_t status;
    xiaotai_binding_status(&status);
    xiaotai_event_t event = {.name = XIAOTAI_EVENT_BINDING_CODE, .args = {.value = (int32_t)status.seconds_left}};
    strcpy(event.args.text, status.verification);
    *(bool *)out = companion_setup_claim(&event);
}

static void expect_binding_input(const char *code, uint32_t left, bool visible)
{
    const char *value = code ? code : "";
    strcpy(fixture.binding.verification, value);
    fixture.binding.seconds_left = left;
    notify(XIAOTAI_EVENT_BINDING_CHANGED);
    bool claimed;
    call(claim_code, &claimed);
    const xiaotai_view_action_t *const action = find_action(XIAOTAI_ROLE_PRIMARY);
    const bool replay = action && !strcmp(action->intent, XIAOTAI_INTENT_BINDING_ANNOUNCE);
    const char *expected = visible ? value : "------";
    if (strcmp(fixture.text, expected) || claimed != visible || replay != visible) {
        fprintf(stderr, "Binding guide input='%s' left=%u text='%s' claim=%u replay=%u\n",
                value, left, fixture.text, (unsigned)claimed, (unsigned)replay);
    }
    page("binding", XIAOTAI_VIEW_CODE, expected);
    assert(claimed == visible && replay == visible);
    assert(fixture.timer_mode == (visible ? XIAOTAI_TIMER_REMAINING : XIAOTAI_TIMER_NONE));
    if (visible) assert(fixture.timer_s == (int32_t)left);
    xiaotai_binding_status_t status;
    xiaotai_binding_status(&status);
    assert(status.phase == XIAOTAI_BINDING_WAITING && status.seconds_left == left);
    assert(!strcmp(status.verification, value));
}

static void binding_code_guide(void)
{
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY;
    boot(needs, NULL, false);
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_WAITING;
    /* Products without the guide constraint still accept the full provider field. */
    expect_binding_input("AbC1234567890123", 9, true);
    stop();

    boot_config(needs, false, &(xiaotai_app_companion_config_t){.binding_guide_six_digits = true});
    fixture.provision.phase = XIAOTAI_PROVISIONING_WAITING;
    strcpy(fixture.provision.provision.ap_ssid, "XiaoTai-CA88");
    strcpy(fixture.provision.provision.url, "192.168.6.1");
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    page("setup", XIAOTAI_VIEW_NOTICE, "手机连接设备热点");
    assert(!strcmp(fixture.subtitle, "XiaoTai-CA88") && !strcmp(fixture.status, "192.168.6.1"));

    fixture.provision.phase = XIAOTAI_PROVISIONING_NONE;
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_WAITING;
    static const char *const valid[] = {"000000", "111111", "888888", "999999", "012345", "987654"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) expect_binding_input(valid[i], 99, true);
    static const char *const invalid[] = {"12345", "1234567", "12a456", NULL};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) expect_binding_input(invalid[i], 99, false);
    expect_binding_input("012345", 1, true);
    expect_binding_input("012345", 0, false);
    fixture.binding.phase = XIAOTAI_BINDING_EXPIRED;
    notify(XIAOTAI_EVENT_BINDING_CHANGED);
    page("binding", XIAOTAI_VIEW_CODE, "------");
    assert(!strcmp(fixture.status, "验证码已过期") && fixture.timer_mode == XIAOTAI_TIMER_NONE);
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_BINDING_RETRY, "双击重试", 0);
    assert(fixture.binding_retries == 1 && !fixture.binding_announcements);

    memset(&fixture.provision, 0, sizeof(fixture.provision));
    fixture.net.state = XIAOTAI_NET_WAITING;
    fixture.net.failing = true;
    notify(XIAOTAI_EVENT_NET_STATE);
    page("setup", XIAOTAI_VIEW_NOTICE, "正在重连已保存的 Wi-Fi");
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_PROVISIONING_RETRY, "双击重试", 0);
    printf("binding_guide valid=6 invalid=4 default_16=1 expiry_retry=1 offline_retry=1\n");
    stop();
}

static void begin_key_setup(xiaotai_err_t error)
{
    post(XIAOTAI_INTENT_APP_COMPANION_REPROVISION, NULL, 0);
    assert(companion.confirm_resolved);
    post(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, NULL, 1);
    fixture.provision.phase = XIAOTAI_PROVISIONING_STARTING;
    fixture.provision.error = error;
    fixture.provisioning_begin_cancellable = true;
    call(begin_quiet, XIAOTAI_QUIESCE_PROVISIONING);
    fixture.provisioning_quiesce = quiet;
}

static void keys_cancel_setup(void)
{
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY;
    boot_config(needs, true, &(xiaotai_app_companion_config_t){.provisioning_cancel_from_keys = true});
    post(XIAOTAI_INTENT_APP_COMPANION_SHOW, XIAOTAI_APP_COMPANION_PAGE_SETTINGS, 0);
    begin_key_setup(XIAOTAI_OK);
    page("maintenance", XIAOTAI_VIEW_PROGRESS, "正在准备重新配网");
    assert(!fixture.status[0] && fixture.provisioning_begins == 1U);
    const xiaotai_args_t back = {.value = COMPANION_SETUP_BACK};
    assert(xiaotai_intent_post_external(XIAOTAI_INTENT_APP_COMPANION_BACK, &back) == XIAOTAI_OK);
    flush();
    assert(fixture.provisioning_cancels == 0U && xiaotai_quiesced());
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_APP_COMPANION_BACK, "长按或双击取消", COMPANION_SETUP_BACK);
    assert(!strcmp(fixture.id, XIAOTAI_APP_COMPANION_PAGE_SETTINGS) && !xiaotai_quiesced());
    /* A second key from the previous view must not navigate out of Settings. */
    post(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, COMPANION_SETUP_BACK);
    assert(!strcmp(fixture.id, XIAOTAI_APP_COMPANION_PAGE_SETTINGS) && fixture.provisioning_cancels == 1U);

    begin_key_setup(XIAOTAI_ERR_NO_MEM);
    page("maintenance", XIAOTAI_VIEW_PROGRESS, "配网准备失败");
    assert(!strcmp(fixture.status, "ERR 0101") && fixture.provisioning_begins == 2U);
    const xiaotai_interaction_rule_t *long_press = NULL;
    for (size_t n = 0; n < fixture.profile->rule_count; ++n) {
        const xiaotai_interaction_rule_t *rule = &fixture.profile->rules[n];
        if (rule->gesture == XIAOTAI_GESTURE_LONG_PRESS && rule->context &&
            !strcmp(rule->context, XIAOTAI_APP_COMPANION_CONTEXT_SETUP)) long_press = rule;
    }
    assert(long_press && !strcmp(long_press->intent, XIAOTAI_INTENT_APP_COMPANION_BACK));
    post(long_press->intent, NULL, long_press->value);
    assert(!strcmp(fixture.id, XIAOTAI_APP_COMPANION_PAGE_SETTINGS) && !xiaotai_quiesced());
    assert(fixture.provisioning_cancels == 2U);

    begin_key_setup(XIAOTAI_OK);
    /* The engine took ownership before the owner processes this key. */
    fixture.provisioning_begin_cancellable = false;
    fixture.provision.restart_pending = true;
    post(long_press->intent, NULL, long_press->value);
    assert(fixture.provisioning_cancels == 2U && xiaotai_quiesced());
    notify(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    assert(!find_action(XIAOTAI_ROLE_PRIMARY));
    fixture.provision.phase = XIAOTAI_PROVISIONING_NONE;
    fixture.provision.restart_pending = false;
    call(end_quiet, NULL);
    call(begin_quiet, XIAOTAI_QUIESCE_ENVIRONMENT);
    assert(!find_action(XIAOTAI_ROLE_PRIMARY));
    post(long_press->intent, NULL, long_press->value);
    assert(fixture.provisioning_cancels == 2U && xiaotai_quiesced());
    call(end_quiet, NULL);
    stop();

    boot(needs, NULL, true);
    post(XIAOTAI_INTENT_APP_COMPANION_SHOW, XIAOTAI_APP_COMPANION_PAGE_SETTINGS, 0);
    begin_key_setup(XIAOTAI_OK);
    assert(!find_action(XIAOTAI_ROLE_PRIMARY));
    post(long_press->intent, NULL, long_press->value);
    assert(fixture.provisioning_cancels == 0U && xiaotai_quiesced());
    call(end_quiet, NULL);
    stop();
}

static void headless(void)
{
    boot(XIAOTAI_NEED_KEY, NULL, false);
    assert(companion.status.setup && !strcmp(fixture.context, "setup") && !companion.buffer);
    post(XIAOTAI_INTENT_APP_COMPANION_MAIN, NULL, 0);
    assert(!fixture.main_requests && fixture.last_refusal.value == XIAOTAI_ERR_BUSY);
    assert(!fixture.activity_notes && !fixture.views && !fixture.toasts && !fixture.page_events);
    stop();
}

static void binding_failure_hint(bool header)
{
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH | XIAOTAI_NEED_KEY;
    boot_config(needs, false, &(const xiaotai_app_companion_config_t){
        .binding_retry_control = header ? XIAOTAI_COMPANION_BINDING_RETRY_HEADER : XIAOTAI_COMPANION_BINDING_RETRY_REFRESH,
    });
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_FAILED;
    notify(XIAOTAI_EVENT_NET_UP);
    page("binding", XIAOTAI_VIEW_CODE, "------");
    assert(!fixture.binding_retries);
    const char *hint = header ? "绑定未完成，请点击右上角重试" : "绑定未完成，请点击刷新";
    if (strcmp(fixture.items[0].label, hint))
        fprintf(stderr, "binding hint: expected '%s', got '%s'\n", hint, fixture.items[0].label);
    assert(!strcmp(fixture.items[0].label, hint));
    const xiaotai_view_action_t *action = find_action(XIAOTAI_ROLE_PRIMARY);
    assert(action && (header ? action->icon && !strcmp(action->icon, XIAOTAI_ICON_REFRESH) : !action->icon));
    press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_BINDING_RETRY, header ? "重试" : "刷新验证码", 0);
    assert(fixture.binding_retries == 1U);
    if (!header) {
        fixture.binding.phase = XIAOTAI_BINDING_EXPIRED;
        notify(XIAOTAI_EVENT_BINDING_CHANGED);
        assert(!strcmp(fixture.items[0].label, "验证码已过期，请点击刷新"));
        assert(fixture.binding_retries == 1U);
        press(XIAOTAI_ROLE_PRIMARY, XIAOTAI_INTENT_BINDING_RETRY, "刷新验证码", 0);
        assert(fixture.binding_retries == 2U);
    }
    stop();
}

int main(int argc, char **argv)
{
    bool matches_old = true;
    if (argc > 2) return 1;
    assert(xiaotai_signal_init(&synchronized) == XIAOTAI_OK);
    if (argc == 1) pointer();
    else if (!strcmp(argv[1], "legacy-manual-disconnect")) matches_old = legacy_manual_disconnect();
    else if (!strcmp(argv[1], "keys")) { keys(); keys_cancel_setup(); }
    else if (!strcmp(argv[1], "binding_code")) binding_code_guide();
    else if (!strcmp(argv[1], "headless")) headless();
    else if (!strcmp(argv[1], "binding_header_retry")) binding_failure_hint(true);
    else if (!strcmp(argv[1], "binding_refresh_retry")) binding_failure_hint(false);
    else return 1;
    xiaotai_signal_deinit(&synchronized);
    return matches_old ? 0 : 1;
}
