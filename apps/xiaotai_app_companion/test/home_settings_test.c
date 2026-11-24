// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_product_volume_application
// Replaces legacy: tests/regression/test_preferences_feedback.py
#include "fakes.h"
#include "companion_preferences_fixture.h"
#include "feedback_fakes.h"
#include "kv_backend.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/media.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_mem_host.h"

static xiaotai_signal_t fence;
static atomic_int_fast64_t now;
static bool fail_volume;
static uint32_t views_refused;
static uint32_t maintenance_ticket;
static xiaotai_err_t set_result;
static int32_t setting_value;
static bool companion_fixture_use_preferences;
static unsigned not_saved;
static xiaotai_err_t last_error;
enum { LEGACY_COMMIT_ERROR = -2 };
static bool commit_fault_case;
static atomic_bool fail_next_commit;
static atomic_uint commit_attempts;
static uint32_t failure_revision;
static bool expect_unversioned;
static bool settings_current_revision;
static bool settings_save_on_reselect;
static bool keep_saved_wifi_on_reprovision;

static void observe_not_saved(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    assert(!strcmp(event->args.text, XIAOTAI_SETTING_AUDIO_VOLUME));
    last_error = (xiaotai_err_t)event->args.value;
    if (commit_fault_case) assert(last_error == LEGACY_COMMIT_ERROR);
    else assert(last_error == XIAOTAI_ERR_TIMEOUT || last_error == XIAOTAI_ERR_NO_MEM);
    if (expect_unversioned) {
        assert(!event->data && !event->size);
    } else {
        assert(event->data && event->size == sizeof(failure_revision));
        memcpy(&failure_revision, event->data, sizeof(failure_revision));
        assert(failure_revision);
    }
    ++not_saved;
}

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&now); }

void xiaotai_ui_activity(void) { ++fixture.activity_notes; }
bool xiaotai_ui_awake(uint8_t display) { assert(!display); return !fixture.screen_asleep; }
xiaotai_err_t xiaotai_ui_sleep(uint8_t display)
{
    assert(!display);
    fixture.screen_asleep = true;
    ++fixture.sleep_requests;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_ui_set_sleep_timeout(uint32_t timeout) { fixture.sleep_ms = timeout; return XIAOTAI_OK; }
static uint8_t screen_brightness;
xiaotai_err_t xiaotai_ui_set_brightness(uint8_t display, uint8_t percent)
{
    assert(!display && percent >= 1 && percent <= 100);
    screen_brightness = percent;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_ui_local_hold_register(const char *intent, xiaotai_ui_local_hold_fn_t fn, void *ctx)
{
    (void)intent;
    (void)fn;
    (void)ctx;
    assert(false); /* home/settings never opts into physical HOLD */
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static bool sample(companion_home_session_t *out)
{
    if (fixture.home_session.mode == COMPANION_HOME_IDLE) return false;
    *out = fixture.home_session;
    return true;
}

extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_ui;
static const xiaotai_app_companion_part_t session = {.name = "fixture", .home_session = sample};
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {
    &xiaotai_app_companion_with_ui, &session, NULL,
};

static void barrier(void *unused) { (void)unused; xiaotai_signal_give(&fence); }
static void flush(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        assert(xiaotai_run(barrier, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void at_ms(int64_t ms) { atomic_store(&now, ms * 1000); flush(); }

static void emit(const char *name, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_event_emit(name, &args, NULL, 0) == XIAOTAI_OK);
    flush();
}

static void send(const char *intent, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_intent_post(intent, &args) == XIAOTAI_OK);
    flush();
}

static void page(const char *target, int32_t arg)
{
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, target, arg);
    assert(!strcmp(fixture.id, target));
}

static void set_int(void *ctx)
{
    set_result = xiaotai_setting_set_int(ctx, setting_value);
}

static void setting(const char *name, int32_t value)
{
    setting_value = value;
    assert(xiaotai_run(set_int, (void *)name) == XIAOTAI_OK);
    flush();
    assert(set_result == XIAOTAI_OK);
}

static void freeze(void *unused)
{
    (void)unused;
    assert(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &maintenance_ticket) == XIAOTAI_OK);
}

static void thaw(void *unused)
{
    (void)unused;
    xiaotai_quiesce_end(maintenance_ticket);
}

static void volume_apply(const char *name, void *unused)
{
    (void)unused;
    if (!fail_volume) return;
    fail_volume = false;
    assert(xiaotai_setting_set_int(name, 40) == XIAOTAI_OK);
    xiaotai_args_t args = {.value = XIAOTAI_FAIL};
    strcpy(args.text, name);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_SETTINGS_REFUSED, &args, NULL, 0) == XIAOTAI_OK);
}

static xiaotai_err_t default_int(void *ctx, const char *key, int32_t default_value, int32_t *out)
{ (void)ctx; (void)key; *out = default_value; return XIAOTAI_OK; }
static xiaotai_err_t default_text(void *ctx, const char *key, char *out, size_t size)
{ (void)ctx; (void)key; (void)out; (void)size; return XIAOTAI_ERR_NOT_FOUND; }
static xiaotai_err_t save_int(void *ctx, const char *key, int32_t value)
{ (void)ctx; (void)key; (void)value; ++fixture.setting_int_writes; return XIAOTAI_OK; }
static xiaotai_err_t save_text(void *ctx, const char *key, const char *value)
{ (void)ctx; (void)key; (void)value; return XIAOTAI_OK; }

static bool speaker_guard_fixture, speaker_refuses;

static bool admit_speaker(int32_t value, void *ctx)
{
    (void)value;
    (void)ctx;
    if (!speaker_refuses) return true;
    xiaotai_args_t args = {.value = XIAOTAI_ERR_NOT_SUPPORTED};
    strcpy(args.text, XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_SETTINGS_REFUSED, &args, NULL, 0) == XIAOTAI_OK);
    return false;
}

static xiaotai_err_t install_fixture(void)
{
    static const xiaotai_setting_store_t store = {
        .load_int = default_int, .load_text = default_text, .store_int = save_int, .store_text = save_text,
    };
    xiaotai_err_t err = companion_fixture_use_preferences ? companion_preferences_install() :
        xiaotai_setting_store_set(&store);
    if (err != XIAOTAI_OK) return err;
    if (!fixture.board_settings) {
        static const xiaotai_setting_t volume = {.name = XIAOTAI_SETTING_AUDIO_VOLUME, .key = "volume",
            .max = 100, .default_value = 40, .changed = volume_apply};
        err = xiaotai_setting_register(&volume);
        if (err != XIAOTAI_OK) return err;
    }
    if (companion_fixture_use_preferences || speaker_guard_fixture) {
        static const xiaotai_setting_t speaker_muted = {.name = XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED,
            .key = "spk_mute", .kind = XIAOTAI_SETTING_BOOL};
        err = speaker_guard_fixture ? xiaotai_setting_register_int_admission(&speaker_muted, admit_speaker)
                                    : xiaotai_setting_register(&speaker_muted);
        if (err != XIAOTAI_OK) return err;
    }
    if (companion_fixture_use_preferences) {
        err = xiaotai_event_subscribe(XIAOTAI_EVENT_SETTING_NOT_SAVED, observe_not_saved, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    return companion_fixture_init();
}

static void boot(bool keys, bool board_settings, bool home_notice, bool toast_notice)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = install_fixture};
    static const xiaotai_module_t *const modules[] = {&probe};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t other = {.name = "other"};
    static const xiaotai_application_t *const apps[] = {&app, &other};
    atomic_store(&now, 1000000);
    feedback_fixture = (companion_feedback_fixture_t){.audio_ready = true};
    fixture = (companion_test_fixture_t){.needs = XIAOTAI_NEED_DISPLAY | (keys ? XIAOTAI_NEED_KEY : XIAOTAI_NEED_TOUCH),
        .navigation_pages = true, .board_settings = board_settings,
        .read_save_pending = companion_fixture_use_preferences ? companion_preferences_pending : NULL};
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    const xiaotai_app_companion_config_t cfg = {.save_failure_on_home = home_notice, .save_failure_toast = toast_notice,
        .save_only_current_revision = settings_current_revision, .save_choice_on_reselect = settings_save_on_reselect,
        .keep_saved_wifi_on_reprovision = keep_saved_wifi_on_reprovision};
    assert(xiaotai_app_companion_configure(&cfg) == XIAOTAI_OK);
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1,
        .applications = apps, .application_count = 2, .home = "companion", .available_needs = fixture.needs};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    flush();
    assert(!strcmp(fixture.id, "home"));
}

static void stop(void) { xiaotai_framework_reset(); xiaotai_app_companion_reset(); }

static void set_audio_readiness(void *ctx)
{
    feedback_fixture.audio_ready = *(const bool *)ctx;
}

static void check_reprovision_confirmation(int32_t expected_flags)
{
    const unsigned overlays = fixture.overlays, requests = fixture.provisioning_begins;
    send(XIAOTAI_INTENT_APP_COMPANION_REPROVISION, NULL, 0);
    assert(fixture.overlays == overlays + 1 && !strcmp(fixture.overlay_body, "绑定会保留"));
    assert(fixture.provisioning_begins == requests);
    send(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, NULL, 0);
    assert(fixture.provisioning_begins == requests);
    send(XIAOTAI_INTENT_APP_COMPANION_REPROVISION, NULL, 0);
    send(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, NULL, 1);
    assert(fixture.provisioning_begins == requests + 1 && fixture.provisioning_begin_flags == expected_flags);
}

static void home_case(void)
{
    boot(false, true, false, false);
    assert(!strcmp(fixture.status, "待唤醒") && !strcmp(fixture.emotion, "neutral"));
    assert(strstr(fixture.text, "你好小钛") && (fixture.flags & XIAOTAI_VIEW_MAY_SLEEP));
    at_ms(45999);
    assert(!strcmp(fixture.emotion, "neutral"));
    at_ms(46000);
    assert(!strcmp(fixture.emotion, "ambient"));
    at_ms(180999);
    assert(!strcmp(fixture.emotion, "ambient"));
    at_ms(181000);
    assert(!strcmp(fixture.emotion, "sleepy"));
    emit(XIAOTAI_EVENT_UI_INTERACTION, NULL, 0);
    assert(!strcmp(fixture.emotion, "neutral"));

    emit(XIAOTAI_EVENT_MEDIA_VOICE, NULL, 2);
    assert(!strcmp(fixture.emotion, "listening"));
    emit(XIAOTAI_EVENT_MEDIA_VOICE, NULL, 1);
    assert(!strcmp(fixture.emotion, "ambient"));
    at_ms(185499);
    assert(!strcmp(fixture.emotion, "ambient"));
    at_ms(185500);
    assert(!strcmp(fixture.emotion, "neutral"));

    send(XIAOTAI_INTENT_APP_COMPANION_REST, NULL, 0);
    assert(fixture.sleep_requests == 1 && fixture.screen_asleep && fixture.talk_stops == 1);
    const unsigned before = fixture.activity_notes;
    at_ms(187999);
    emit(XIAOTAI_EVENT_MEDIA_VOICE, NULL, 2);
    assert(fixture.activity_notes == before);
    emit(XIAOTAI_EVENT_MEDIA_VOICE, NULL, 1);
    at_ms(188000);
    emit(XIAOTAI_EVENT_MEDIA_VOICE, NULL, 2);
    assert(fixture.activity_notes == before + 1);

    emit(XIAOTAI_EVENT_NET_UP, NULL, 19);
    assert(!strcmp(fixture.status, "Wi-Fi 连接成功"));
    at_ms(191199);
    assert(!strcmp(fixture.status, "Wi-Fi 连接成功"));
    at_ms(191200);
    assert(!strcmp(fixture.status, "待唤醒"));
    setting(XIAOTAI_SETTING_AUDIO_MIC_MUTED, 1);
    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, XIAOTAI_SETTING_AUDIO_MIC_MUTED, 1);
    assert(!strcmp(fixture.status, "麦克风关闭") && strstr(fixture.text, "麦克风已关闭"));

    static const char *const states[] = {NULL, "正在连接 AI", "结束对话中", "小钛在这儿", "远程对讲中",
        "远程查看中", "摄像头启动中", "视频查看中", "摄像头异常", "语音来电", "正在建立通话", "通话中",
        "正在加入房间", "房间对讲中"};
    for (size_t i = 1; i < sizeof(states) / sizeof(states[0]); ++i) {
        fixture.home_session = (companion_home_session_t){.mode = (companion_home_mode_t)i, .caption = "字幕"};
        page("home", 0);
        /* State updates are delivered without changing the page id. */
        emit(XIAOTAI_EVENT_ACTIVITY, NULL, 1);
        assert(!strcmp(fixture.status, states[i]) && !(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP));
        const bool streaming = i >= COMPANION_HOME_STREAM_AUDIO && i <= COMPANION_HOME_STREAM_CAMERA_ERROR;
        assert(!strcmp(fixture.text, streaming ? "" : "字幕"));
    }
    fixture.home_session = (companion_home_session_t){.mode = COMPANION_HOME_AI_ACTIVE, .phase = XIAOTAI_TALK_THINKING,
        .speaker = XIAOTAI_SPEAKER_ASSISTANT, .emotion = "兴奋", .caption = "字幕"};
    emit(XIAOTAI_EVENT_ACTIVITY, NULL, 1);
    assert(!strcmp(fixture.status, "AI 正在思考") && !strcmp(fixture.emotion, "excited") &&
           fixture.phase == XIAOTAI_TALK_THINKING && fixture.speaker == XIAOTAI_SPEAKER_ASSISTANT);
    fixture.home_session.mode = COMPANION_HOME_IDLE;
    emit(XIAOTAI_EVENT_ACTIVITY, NULL, 0);
    assert(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP);
    /* A page left open may sleep; waking starts from the home screen. A
     * session keeps the page lit and where it is. */
    page("emotions", 0);
    assert(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP);
    emit(XIAOTAI_EVENT_UI_SLEEP, "0", 1);
    assert(!strcmp(fixture.id, "home"));
    emit(XIAOTAI_EVENT_UI_SLEEP, "0", 0);
    fixture.home_session.mode = COMPANION_HOME_ROOM;
    page("emotions", 0);
    assert(!(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP));
    emit(XIAOTAI_EVENT_UI_SLEEP, "0", 1);
    assert(!strcmp(fixture.id, "emotions"));
    emit(XIAOTAI_EVENT_UI_SLEEP, "0", 0);
    fixture.home_session.mode = COMPANION_HOME_IDLE;
    page("home", 0);
    page("emotions", 0);
    assert(fixture.item_count == XIAOTAI_APP_COMPANION_EMOTIONS && (fixture.flags & XIAOTAI_VIEW_GRID));
    assert(!strcmp(fixture.items[0].label, "平静"));
    send(XIAOTAI_INTENT_APP_COMPANION_EMOTION, NULL, 7);
    assert(!strcmp(fixture.id, "emotion") && !strcmp(fixture.emotion, "loving"));
    assert(fixture.kind == XIAOTAI_VIEW_TALK && (fixture.flags & XIAOTAI_VIEW_EMOTION_PREVIEW));
    send(XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, NULL, 7);
    assert(!strcmp(fixture.emotion, "loving"));
    const unsigned default_emotion_writes = fixture.setting_int_writes;
    page("emotions", 0);
    assert(fixture.items[7].state & XIAOTAI_STATE_CHECKED);
    send(fixture.items[7].intent, NULL, fixture.items[7].value);
    assert(!strcmp(fixture.id, "emotion") && !strcmp(fixture.actions[0].intent, XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY));
    send(fixture.actions[0].intent, NULL, fixture.actions[0].value);
    assert(!strcmp(fixture.id, "home") && !strcmp(fixture.emotion, "loving"));
    assert(fixture.setting_int_writes == default_emotion_writes && !fixture.choice_confirms);
    strcpy(fixture.net.ssid, "station");
    memcpy(&fixture.net.ipv4, (const uint8_t[]){192, 0, 2, 10}, 4);
    fixture.net.signal_valid = true;
    fixture.net.signal_dbm = -68;
    strcpy(fixture.binding.device_id, "fixture-device");
    page("network", 0);
    assert(fixture.item_count == 6 && !strcmp(fixture.items[0].detail, "station"));
    assert(!strcmp(fixture.items[1].detail, "192.0.2.10") && !strcmp(fixture.items[2].detail, "-68 dBm"));
    assert(!strcmp(fixture.items[3].detail, "fixture-device") && !strcmp(fixture.items[5].detail, "mqtt://broker.invalid"));
    const unsigned views_before_tick = fixture.views;
    at_ms(192199);
    assert(fixture.views == views_before_tick);
    at_ms(192200);
    assert(fixture.views == views_before_tick + 1);
    page("status", 0);
    assert(strstr(fixture.text, "view_refused=") && fixture.action_count == 7);
    const unsigned depth = companion.depth;
    send(fixture.actions[2].intent, NULL, fixture.actions[2].value);
    assert(companion.depth == depth && !strcmp(fixture.status, "平台"));
    page("home", 0);
    const unsigned home_views = fixture.views;
    at_ms(193200);
    assert(fixture.views == home_views);
    stop();

    boot(true, true, false, false);
    assert(!strcmp(fixture.status, "在线"));
    fixture.platform.phase = XIAOTAI_PLATFORM_CONNECTING;
    emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, NULL, 0);
    assert(!strcmp(fixture.status, "连接中"));
    /* Running either of the ongoing session activities is independent of the
     * network-only status text of a keys-style homepage. */
    fixture.home_session.mode = COMPANION_HOME_AI_ACTIVE;
    emit(XIAOTAI_EVENT_ACTIVITY, NULL, 1);
    assert(!strcmp(fixture.status, "连接中") && !(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP));
    stop();
}

static void settings_case(void)
{
    boot(false, false, false, false);
    page("settings", 0);
    assert(fixture.kind == XIAOTAI_VIEW_FORM && fixture.field_count == 1);
    assert(fixture.fields[0].value == 40 && fixture.fields[0].step == 10);
    fail_volume = true;
    send(fixture.fields[0].intent, NULL, 90);
    assert(fixture.fields[0].value == 40 && !strcmp(fixture.toast, "音量调整失败，请重试"));
    assert(!fixture.status[0]);
    fixture.save_pending = true;
    send(fixture.fields[0].intent, NULL, 70);
    assert(fixture.fields[0].value == 70 && !strcmp(fixture.status, "保存中"));
    fixture.save_pending = false;
    at_ms(1100);
    assert(!strcmp(fixture.status, "已保存"));
    stop();

    speaker_guard_fixture = true;
    boot(false, false, false, false);
    page("settings", 0);
    assert(fixture.field_count == 2 && fixture.fields[1].value == 1 && !fixture.status[0]);
    speaker_refuses = true;
    const unsigned toasts = fixture.toasts;
    send(fixture.fields[1].intent, NULL, 0);
    assert(fixture.fields[1].value == 1 && !fixture.status[0] && fixture.toasts == toasts + 1);
    assert(!strcmp(fixture.toast, "扬声器切换失败，请重试"));

    speaker_refuses = false;
    fixture.save_pending = true;
    send(fixture.fields[1].intent, NULL, 0);
    assert(fixture.fields[1].value == 0 && !strcmp(fixture.status, "保存中"));
    speaker_refuses = true;
    send(fixture.fields[1].intent, NULL, 1);
    assert(fixture.fields[1].value == 0 && !strcmp(fixture.status, "保存中") && fixture.toasts == toasts + 2);
    fixture.save_pending = false;
    at_ms(1100);
    assert(!strcmp(fixture.status, "已保存"));
    stop();
    speaker_refuses = speaker_guard_fixture = false;

    boot(false, true, false, false);
    page("settings", 0);
    assert(fixture.field_count == 8 && fixture.fields[3].step == 3 && fixture.fields[6].option_count == 5);
    assert(fixture.fields[6].value == 1 && fixture.fields[7].value == 0);
    send(fixture.fields[6].intent, NULL, 2);
    int32_t value;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_SCREEN_SLEEP, &value) == XIAOTAI_OK && value == 600);
    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, XIAOTAI_SETTING_SCREEN_SLEEP, 600);
    assert(fixture.sleep_ms == 600000);
    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, XIAOTAI_SETTING_SCREEN_SLEEP, -1);
    assert(fixture.sleep_ms == 0);
    /* The brightness setting reaches the backlight; a value no screen takes does not. */
    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, XIAOTAI_SETTING_SCREEN_BRIGHTNESS, 30);
    assert(screen_brightness == 30);
    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, XIAOTAI_SETTING_SCREEN_BRIGHTNESS, 0);
    assert(screen_brightness == 30);
    const char *const sleep_intent = fixture.fields[6].intent;
    const char *const voice_intent = fixture.fields[7].intent;
    assert(xiaotai_run(freeze, NULL) == XIAOTAI_OK);
    flush();
    send(sleep_intent, NULL, 3);
    send(voice_intent, NULL, 1);
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_SCREEN_SLEEP, &value) == XIAOTAI_OK && value == 600);
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE, &value) == XIAOTAI_OK && value == 0);
    assert(!strcmp(fixture.toast, "维护中，设置暂不可修改"));
    assert(xiaotai_run(thaw, NULL) == XIAOTAI_OK);
    flush();
    page("settings", 0);
    check_reprovision_confirmation(XIAOTAI_PROVISIONING_FORGET);
    stop();

    boot(true, true, false, false);
    page("settings", 0);
    assert(fixture.kind == XIAOTAI_VIEW_LIST && fixture.field_count == 0);
    assert(fixture.items[0].value == 50 && fixture.items[1].value == 30);
    assert(fixture.items[2].value == 27 && fixture.items[3].value == 21);
    bool ready = false;
    assert(xiaotai_run(set_audio_readiness, &ready) == XIAOTAI_OK);
    flush();
    const unsigned writes = fixture.setting_int_writes;
    send(fixture.items[2].intent, NULL, fixture.items[2].value);
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_GAIN, &value) == XIAOTAI_OK && value == 24);
    assert(fixture.setting_int_writes == writes);
    for (size_t i = 0; i < 4; ++i) assert(fixture.items[i].state & XIAOTAI_STATE_DISABLED);
    ready = true;
    assert(xiaotai_run(set_audio_readiness, &ready) == XIAOTAI_OK);
    flush();
    page("home", 0);
    page("settings", 0);
    send(fixture.items[2].intent, NULL, fixture.items[2].value);
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_GAIN, &value) == XIAOTAI_OK && value == 27);
    assert(fixture.items[2].value == 30 && fixture.items[3].value == 24);
    setting(XIAOTAI_SETTING_AUDIO_VOLUME, 96);
    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, XIAOTAI_SETTING_AUDIO_VOLUME, 96);
    assert(fixture.items[0].value == 100);
    send(fixture.items[0].intent, NULL, fixture.items[0].value);
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &value) == XIAOTAI_OK && value == 100);
    assert(fixture.items[0].state & XIAOTAI_STATE_DISABLED);
    stop();

    keep_saved_wifi_on_reprovision = true;
    boot(true, true, false, false);
    page("settings", 0);
    check_reprovision_confirmation(0);
    stop();
    keep_saved_wifi_on_reprovision = false;
}

static void assert_setting_value(const char *setting_name, int32_t expected)
{
    int32_t value;
    assert(xiaotai_setting_get_int(setting_name, &value) == XIAOTAI_OK && value == expected);
}

static void choice_reconfirm_case(void)
{
    const char *const tag = XIAOTAI_ARG_CHOICE_RECONFIRM;
    const char *const sleep = XIAOTAI_SETTING_SCREEN_SLEEP;
    const char *const ack = XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE;
    boot(false, true, false, false);
    page("settings", 0);
    assert(fixture.field_count == 8);
    const char *const sleep_intent = fixture.fields[6].intent;
    const char *const ack_intent = fixture.fields[7].intent;
    assert(!(fixture.fields[6].flags & XIAOTAI_FIELD_CONFIRM_CURRENT));
    assert(!(fixture.fields[7].flags & XIAOTAI_FIELD_CONFIRM_CURRENT));
    unsigned writes = fixture.setting_int_writes;
    send(sleep_intent, tag, 1);
    send(ack_intent, tag, 0);
    assert(!fixture.choice_confirms && fixture.setting_int_writes == writes);
    const unsigned invalid_toasts = fixture.toasts;
    send(sleep_intent, NULL, 99);
    send(ack_intent, NULL, 99);
    assert(fixture.toasts == invalid_toasts + 2U && fixture.setting_int_writes == writes);
    stop();

    settings_save_on_reselect = true;
    boot(false, true, false, false);
    page("settings", 0);
    assert(fixture.field_count == 8 && fixture.fields[6].value == 1 && fixture.fields[7].value == 0);
    assert(fixture.fields[6].flags & XIAOTAI_FIELD_CONFIRM_CURRENT);
    assert(fixture.fields[7].flags & XIAOTAI_FIELD_CONFIRM_CURRENT);
    assert(!(fixture.fields[0].flags & XIAOTAI_FIELD_CONFIRM_CURRENT));
    writes = fixture.setting_int_writes;

    send(sleep_intent, NULL, 1);
    send(ack_intent, NULL, 0);
    setting(sleep, 300);
    setting(ack, 0);
    assert(!fixture.choice_confirms && fixture.setting_int_writes == writes);

    send(sleep_intent, tag, 1);
    assert(fixture.choice_confirms == 1 && !strcmp(fixture.confirmed_setting, sleep) && fixture.confirmed_value == 300);
    send(ack_intent, tag, 0);
    assert(fixture.choice_confirms == 2 && !strcmp(fixture.confirmed_setting, ack) && fixture.confirmed_value == 0);
    assert(fixture.setting_int_writes == writes);

    send(sleep_intent, NULL, 2);
    send(ack_intent, NULL, 1);
    assert(fixture.choice_confirms == 2 && fixture.setting_int_writes == writes + 2);
    writes = fixture.setting_int_writes;
    send(sleep_intent, tag, 1);
    send(ack_intent, tag, 0);
    send(fixture.fields[0].intent, tag, 70);
    assert(fixture.choice_confirms == 2 && fixture.setting_int_writes == writes);
    assert_setting_value(sleep, 600);
    assert_setting_value(ack, 1);

    assert(xiaotai_run(freeze, NULL) == XIAOTAI_OK);
    flush();
    send(sleep_intent, tag, 2);
    send(ack_intent, tag, 1);
    assert(fixture.choice_confirms == 2 && fixture.setting_int_writes == writes);
    assert(xiaotai_run(thaw, NULL) == XIAOTAI_OK);
    flush();
    page("home", 0);
    send(ack_intent, tag, 1);
    assert(fixture.choice_confirms == 2 && fixture.setting_int_writes == writes);

    setting(sleep, 17);
    page("settings", 0);
    assert(fixture.fields[6].value == 5 && !(fixture.fields[6].flags & XIAOTAI_FIELD_CONFIRM_CURRENT));
    writes = fixture.setting_int_writes;
    send(sleep_intent, tag, 5);
    assert(fixture.choice_confirms == 2 && fixture.setting_int_writes == writes);
    assert_setting_value(sleep, 17);

    const unsigned refusals = fixture.refused;
    fixture.confirm_result = XIAOTAI_ERR_TIMEOUT;
    send(ack_intent, tag, 1);
    assert(fixture.choice_confirms == 3 && fixture.setting_int_writes == writes);
    assert(fixture.refused == refusals + 1 && fixture.last_refusal.value == XIAOTAI_ERR_TIMEOUT);
    assert(!strcmp(fixture.last_refusal.text, ack));
    assert_setting_value(ack, 1);
    stop();
}

static void diag(void *unused);
static void capture_diag(void *ctx, const char *text)
{
    (void)ctx;
    const char *key = strstr(text, "view_refused=");
    if (key) assert(sscanf(key, "view_refused=%u", &views_refused) == 1);
}
static void diag(void *unused)
{
    (void)unused;
    const xiaotai_reply_t out = {.write = capture_diag};
    xiaotai_diag_report("framework", &out);
}

static void fail_save(void) { emit(XIAOTAI_EVENT_SETTING_NOT_SAVED, XIAOTAI_SETTING_AUDIO_VOLUME, XIAOTAI_FAIL); }
static void switch_out(void *unused) { (void)unused; assert(xiaotai_app_switch("other") == XIAOTAI_OK); }
static void code_show(void *unused) { (void)unused; companion_settings_code_visible(true); }
static void code_hide(void *unused) { (void)unused; companion_settings_code_visible(false); }

static void only_save_toast(bool ignored_toast)
{
    boot(false, false, false, ignored_toast);
    page("settings", 0);
    fixture.save_pending = true;
    fail_save();
    assert(!strcmp(fixture.status, "未保存") && !fixture.toasts);
    fixture.save_pending = false;
    page("home", 0);
    assert(!fixture.toasts && !strcmp(fixture.status, "待唤醒"));
    fail_save();
    assert(fixture.toasts == 1 && fixture.toast_ms == 2400 && !strcmp(fixture.toast, "设置未保存"));
    page("call", 0);
    fail_save();
    assert(fixture.toasts == 1);
    page("wechat.code", 0);
    assert(fixture.toasts == 1);
    page("home", 0);
    assert(fixture.toasts == 2);
    fixture.screen_asleep = true;
    fail_save();
    assert(fixture.toasts == 2);
    fixture.screen_asleep = false;
    emit(XIAOTAI_EVENT_UI_SLEEP, "0", 0);
    assert(fixture.toasts == 3);
    emit(XIAOTAI_EVENT_UI_SLEEP, "0", 0);
    assert(fixture.toasts == 3 && !strcmp(fixture.status, "待唤醒"));
    assert(xiaotai_run(code_show, NULL) == XIAOTAI_OK);
    flush();
    fail_save();
    assert(fixture.toasts == 3);
    assert(xiaotai_run(code_hide, NULL) == XIAOTAI_OK);
    flush();
    assert(fixture.toasts == 4);
    stop();
}

static void save_failure_case(void)
{
    only_save_toast(false);
    only_save_toast(true);
    for (unsigned toast = 0; toast < 2; ++toast) {
        boot(false, false, true, toast != 0);
        page("settings", 0);
        fail_save();
        assert(!fixture.status[0] && fixture.toasts == toast);
        page("home", 0);
        at_ms(5999);
        assert(!strcmp(fixture.status, "设置保存失败"));
        at_ms(6000);
        assert(!strcmp(fixture.status, "待唤醒"));
        page("call", 0);
        fixture.screen_asleep = true;
        fail_save();
        assert(fixture.toasts == toast * 2);
        if (toast) assert(!strcmp(fixture.toast, "设置保存失败，重启后可能恢复原设置") && fixture.toast_ms == 2400);
        assert(xiaotai_run(switch_out, NULL) == XIAOTAI_OK);
        flush();
        assert(xiaotai_run(diag, NULL) == XIAOTAI_OK);
        flush();
        const uint32_t prior = views_refused;
        fail_save();
        assert(xiaotai_run(diag, NULL) == XIAOTAI_OK);
        flush();
        assert(views_refused == prior && fixture.toasts == toast * 2);
        stop();
    }
    boot(true, false, true, true);
    page("settings", 0);
    fail_save();
    assert(!strcmp(fixture.status, "保存失败") && !fixture.toasts);
    stop();
}

/* Use the deployed S3/CoreS3 scalar binding and exercise both ends of the
 * production asynchronous preference-to-UI chain with a gated host KV. */
static void preference_boot_policy(bool seed, bool keys, bool home_failure, bool current_revision, uint8_t raw_volume)
{
    companion_preferences_prepare(seed, raw_volume);
    companion_fixture_use_preferences = true;
    settings_current_revision = current_revision;
    boot(keys, false, home_failure, false);
    page("settings", 0);
    if (keys) assert(fixture.item_count >= 2 && fixture.items[0].value == (raw_volume + 1) * 10);
    else assert(fixture.field_count == 2 && fixture.fields[0].value == raw_volume * 10);
}

static void preference_boot(bool seed) { preference_boot_policy(seed, false, false, true, 4); }

static void preference_stop(void)
{
    stop();
    companion_preferences_reset();
}

static void preference_open_failure_visibility(void)
{
    preference_boot_policy(true, false, false, true, 7);
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    xiaotai_kv_host_gate(false);
    send(fixture.fields[0].intent, NULL, 80);
    at_ms(1030);
    assert(xiaotai_kv_host_wait_held(3000) && companion_preferences_pending());
    page("call", 0);
    xiaotai_kv_host_fail(XIAOTAI_ERR_TIMEOUT);
    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    xiaotai_kv_host_fail(XIAOTAI_OK);
    flush();
    xiaotai_kv_host_counts(&after);
    assert(not_saved == 1 && last_error == XIAOTAI_ERR_TIMEOUT && !companion_preferences_pending());
    assert(after.writes == before.writes && after.commits == before.commits && fixture.toasts == 0);

    uint8_t raw;
    assert(xiaotai_kv_get_u8("product", "volume", &raw, 3000) == XIAOTAI_OK && raw == 7U);
    fixture.screen_asleep = true;
    page("home", 0);
    assert(fixture.toasts == 0);
    fixture.screen_asleep = false;
    emit(XIAOTAI_EVENT_UI_SLEEP, "0", 0);
    assert(fixture.toasts == 1 && !strcmp(fixture.toast, "设置未保存"));
    emit(XIAOTAI_EVENT_UI_SLEEP, "0", 0);
    assert(fixture.toasts == 1 && companion_preferences_flush() == XIAOTAI_ERR_TIMEOUT);
    puts("write-open failure: no write, no commit, no toast in call or sleeping home, one awake toast");
    preference_stop();
}

xiaotai_err_t __real_xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle);
xiaotai_err_t __wrap_xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle)
{
    ++commit_attempts;
    if (atomic_exchange(&fail_next_commit, false)) return LEGACY_COMMIT_ERROR;
    return __real_xiaotai_kv_backend_commit(handle);
}

static void preference_commit_failure(bool superseded)
{
    commit_fault_case = true;
    preference_boot_policy(true, false, false, true, 7);
    const char *const change_volume = fixture.fields[0].intent;
    xiaotai_kv_host_counts_t before, failed;
    xiaotai_kv_host_counts(&before);
    const unsigned attempts = atomic_load(&commit_attempts);
    xiaotai_kv_host_gate(false);
    atomic_store(&fail_next_commit, true);
    send(change_volume, NULL, 80);
    at_ms(1030);
    assert(xiaotai_kv_host_wait_held(3000) && !not_saved);
    if (superseded) send(change_volume, NULL, 90);
    assert(!strcmp(fixture.status, "保存中") && companion_preferences_pending());
    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    flush();

    xiaotai_kv_host_counts(&failed);
    assert(failed.writes == before.writes + 1U && failed.commits == before.commits);
    assert(failed.closes == before.closes + 1U && atomic_load(&commit_attempts) == attempts + 1U);
    assert(!atomic_load(&fail_next_commit) && not_saved == 1U && last_error == LEGACY_COMMIT_ERROR);
    assert(companion_preferences_pending() == superseded);
    const char *const expected = superseded ? "保存中" : "未保存";
    assert(fixture.fields[0].value == (superseded ? 90 : 80) && !strcmp(fixture.status, expected));
    printf("commit failed: error=%d writes=1 commit_attempts=1 closes=1 live=%d status=%s\n",
           last_error, fixture.fields[0].value, fixture.status);

    if (superseded) {
        at_ms(1060);
        assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
        at_ms(1200);
        xiaotai_kv_host_counts_t saved;
        xiaotai_kv_host_counts(&saved);
        assert(saved.writes == failed.writes + 1U && saved.commits == failed.commits + 1U);
        assert(saved.closes == failed.closes + 1U && atomic_load(&commit_attempts) == attempts + 2U);
        assert(not_saved == 1U && !companion_preferences_pending());
        assert(fixture.fields[0].value == 90 && !strcmp(fixture.status, "已保存"));
        uint8_t raw;
        assert(xiaotai_kv_get_u8("product", "volume", &raw, 3000) == XIAOTAI_OK && raw == 9);
        puts("later commit succeeded: writes=1 commit_attempts=1 closes=1 live=90 status=已保存");
    }
    assert(companion_preferences_flush() == LEGACY_COMMIT_ERROR);
    preference_stop();
}

static int preference_order_case(bool supersede, bool unrelated, bool newer_failed, bool no_op)
{
    /* The shared legacy S3 fixture starts at raw 7 and edits to 8 then 9. */
    const uint8_t initial = supersede ? 7 : 4;
    const int32_t older = (initial + 1) * 10, newer = (initial + 2) * 10;
    preference_boot_policy(true, false, false, true, initial);
    const char *const change_volume = fixture.fields[0].intent;
    const char *const change_speaker = fixture.fields[1].intent;
    xiaotai_kv_host_counts_t before, failed;
    xiaotai_kv_host_counts(&before);
    xiaotai_kv_host_gate(false);
    xiaotai_kv_host_fail_key("volume", XIAOTAI_ERR_TIMEOUT);
    send(change_volume, NULL, older);
    at_ms(1030);
    assert(xiaotai_kv_host_wait_held(3000) && !not_saved);
    if (supersede) send(change_volume, NULL, newer);
    if (unrelated) send(change_speaker, NULL, 0);
    if (no_op) send(change_volume, NULL, older);
    assert(!strcmp(fixture.status, "保存中") && companion_preferences_pending());

    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    flush();
    const bool later_pending = supersede || unrelated;
    assert(companion_preferences_pending() == later_pending && not_saved == 1 && last_error == XIAOTAI_ERR_TIMEOUT);
    const companion_preferences_stats_t stats_after_a = companion_preferences_stats();
    assert(stats_after_a.write_failures == 1);
    assert(stats_after_a.stores == (later_pending ? 2U : 1U));
    xiaotai_kv_host_counts(&failed);
    assert(failed.writes == before.writes + 1U && failed.commits == before.commits);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    uint8_t raw = 0;
    assert(xiaotai_kv_get_u8("product", "volume", &raw, 3000) == XIAOTAI_OK && raw == initial);
    if (supersede) assert(fixture.fields[0].value == newer);
    const char *const expected_a = supersede ? "保存中" : "未保存";
    int mismatches = strcmp(fixture.status, expected_a) != 0;
    printf("after_A_failure expected=%s observed=%s pending=%u not_saved=%u write_failures=%u raw_volume=%u "
           "set_attempts=%u commits=%u\n", expected_a, fixture.status, (unsigned)later_pending, not_saved,
           (unsigned)stats_after_a.write_failures, raw, failed.writes - before.writes, failed.commits - before.commits);

    if (later_pending) {
        if (newer_failed) xiaotai_kv_host_fail_key("volume", XIAOTAI_ERR_TIMEOUT);
        at_ms(1060);
        assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
        at_ms(1200);
        assert(!companion_preferences_pending() && not_saved == (newer_failed ? 2U : 1U));
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        const uint8_t durable = supersede && !newer_failed ? initial + 2U : initial;
        assert(xiaotai_kv_get_u8("product", "volume", &raw, 3000) == XIAOTAI_OK && raw == durable);
        xiaotai_kv_host_counts_t final;
        xiaotai_kv_host_counts(&final);
        assert(final.writes == failed.writes + 1U && final.commits == failed.commits + !newer_failed);
        if (supersede) assert(fixture.fields[0].value == newer);
        if (unrelated) {
            uint8_t speaker = 0;
            assert(xiaotai_kv_get_u8("product", "spk_mute", &speaker, 3000) == XIAOTAI_OK && speaker == 1);
        }
        const char *const expected_b = supersede && !newer_failed ? "已保存" : "未保存";
        mismatches += strcmp(fixture.status, expected_b) != 0;
        printf("after_B_%s expected=%s observed=%s pending=0 not_saved=%u raw_volume=%u\n",
               newer_failed ? "failure" : "success", expected_b, fixture.status, not_saved, raw);
    }
    /* Reporting the real error to flush remains correct even if a later
     * operation replaced the setting's intended value. */
    assert(companion_preferences_flush() == XIAOTAI_ERR_TIMEOUT);
    preference_stop();
    return mismatches != 0;
}

static void occupy_preference_slots(void *ctx)
{
    (void)ctx;
    companion_preferences_fill(15);
}

static void preference_rejected_case(void)
{
    preference_boot(true);
    const char *const change_volume = fixture.fields[0].intent;
    xiaotai_kv_host_gate(false);
    send(change_volume, NULL, 50);
    at_ms(1030);
    assert(xiaotai_kv_host_wait_held(3000) && !not_saved);
    /* Fill the remaining 15 slots with unrelated writes; the active write
     * keeps its reserved slot. A later change fails storage admission. */
    assert(xiaotai_run(occupy_preference_slots, NULL) == XIAOTAI_OK);
    flush();
    send(change_volume, NULL, 60);
    assert(not_saved == 1 && last_error == XIAOTAI_ERR_NO_MEM);
    assert(!strcmp(fixture.status, "未保存"));
    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    assert(companion_preferences_flush() == XIAOTAI_OK);
    at_ms(1200);
    uint8_t raw = 0;
    assert(xiaotai_kv_get_u8("product", "volume", &raw, 3000) == XIAOTAI_OK && raw == 5);
    assert(!companion_preferences_pending() && not_saved == 1);
    assert(!strcmp(fixture.status, "未保存"));
    printf("later_admission_failed earlier_write_success=1 not_saved=%u raw_volume=%u observed=%s\n",
           not_saved, raw, fixture.status);
    preference_stop();
}

static void preference_restart_case(void)
{
    assert(preference_order_case(false, false, false, false) == 0);
    const uint32_t retired = failure_revision;
    preference_boot(false);
    send(fixture.fields[0].intent, NULL, 60);
    at_ms(1030);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    at_ms(1200);
    assert(!strcmp(fixture.status, "已保存"));
    /* Only host can replay metadata from a retired boot. The real reset
     * contract drains active KV callbacks before starting a new owner. */
    xiaotai_args_t args = {.value = XIAOTAI_ERR_TIMEOUT};
    strcpy(args.text, XIAOTAI_SETTING_AUDIO_VOLUME);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_SETTING_NOT_SAVED, &args, &retired, sizeof(retired)) == XIAOTAI_OK);
    flush();
    assert(not_saved == 2 && !strcmp(fixture.status, "已保存"));
    printf("retired_result_observed=%u current_after_reset=%s\n", not_saved, fixture.status);
    preference_stop();
}

static void preference_metadata_oom(void)
{
    preference_boot(true);
    xiaotai_kv_host_gate(false);
    xiaotai_kv_host_fail_key("volume", XIAOTAI_ERR_TIMEOUT);
    send(fixture.fields[0].intent, NULL, 50);
    at_ms(1030);
    assert(xiaotai_kv_host_wait_held(3000) && !not_saved);
    expect_unversioned = true;
    xiaotai_mem_host_fail_nth(1);
    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    flush();
    assert(!xiaotai_mem_host_fail_pending());
    assert(not_saved == 1 && !strcmp(fixture.status, "未保存"));
    assert(companion_preferences_flush() == XIAOTAI_ERR_TIMEOUT);
    printf("optional_metadata_oom original_failure_events=%u observed=%s\n", not_saved, fixture.status);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    preference_stop();
}

static void preference_notify_older(bool keys)
{
    preference_boot_policy(true, keys, !keys, false, 4);
    xiaotai_kv_host_gate(false);
    xiaotai_kv_host_fail_key("volume", XIAOTAI_ERR_TIMEOUT);
    const char *const intent = keys ? fixture.items[0].intent : fixture.fields[0].intent;
    send(intent, NULL, 50);
    at_ms(1030);
    assert(xiaotai_kv_host_wait_held(3000) && !not_saved);
    send(intent, NULL, 60);
    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    flush();
    assert(not_saved == 1 && companion_preferences_pending());
    if (!keys) page("home", 0);
    assert(!strcmp(fixture.status, keys ? "保存失败" : "设置保存失败"));
    printf("notify_every_failure keys=%u newer_pending=1 observed=%s\n", (unsigned)keys, fixture.status);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    at_ms(1060);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    assert(companion_preferences_flush() == XIAOTAI_ERR_TIMEOUT);
    preference_stop();
}

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    int result = 0;
    if (argc > 1 && !strcmp(argv[1], "settings")) settings_case();
    else if (argc > 1 && !strcmp(argv[1], "choice_reconfirm")) choice_reconfirm_case();
    else if (argc > 1 && !strcmp(argv[1], "save_failure")) {
        save_failure_case();
        preference_open_failure_visibility();
    }
    else if (argc > 1 && !strcmp(argv[1], "commit_failure")) preference_commit_failure(false);
    else if (argc > 1 && !strcmp(argv[1], "commit_failure_superseded")) preference_commit_failure(true);
    else if (argc > 1 && !strcmp(argv[1], "stale_same_key")) result = preference_order_case(true, false, false, false);
    else if (argc > 1 && !strcmp(argv[1], "current_failure")) result = preference_order_case(false, false, false, false);
    else if (argc > 1 && !strcmp(argv[1], "other_key")) result = preference_order_case(false, true, false, false);
    else if (argc > 1 && !strcmp(argv[1], "newer_failed")) result = preference_order_case(true, false, true, false);
    else if (argc > 1 && !strcmp(argv[1], "no_op")) result = preference_order_case(false, false, false, true);
    else if (argc > 1 && !strcmp(argv[1], "rejected")) preference_rejected_case();
    else if (argc > 1 && !strcmp(argv[1], "retired")) preference_restart_case();
    else if (argc > 1 && !strcmp(argv[1], "metadata_oom")) preference_metadata_oom();
    else if (argc > 1 && !strcmp(argv[1], "notify_home")) preference_notify_older(false);
    else if (argc > 1 && !strcmp(argv[1], "notify_keys")) preference_notify_older(true);
    else home_case();
    xiaotai_signal_deinit(&fence);
    return result;
}
