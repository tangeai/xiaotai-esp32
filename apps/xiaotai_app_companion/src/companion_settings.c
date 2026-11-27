// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>
#include "xiaotai/media.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SETTINGS_ENTRY "companion.settings.entry"
#define SCREEN_WAKE_SETTING "wake.enabled"
#define VISION_SETTING "ai_vision.enabled"
#define WECHAT_CODE_PAGE "wechat.code"
#define POLL_PENDING_MS 100U

typedef struct {
    const char *name, *intent, *label;
    uint8_t kind;
    bool inverted;
} field_t;

#define FIELD(name_, setting_, label_, kind_, inverted_) \
    [name_] = {setting_, XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX setting_, label_, kind_, inverted_}
static const field_t fields[] = {
    FIELD(0, XIAOTAI_SETTING_AUDIO_VOLUME, "音量", XIAOTAI_FIELD_STEPPER, false),
    FIELD(1, XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, "扬声器", XIAOTAI_FIELD_SWITCH, true),
    FIELD(2, XIAOTAI_SETTING_AUDIO_MIC_MUTED, "麦克风", XIAOTAI_FIELD_SWITCH, true),
    FIELD(3, XIAOTAI_SETTING_AUDIO_MIC_GAIN, "麦克风增益", XIAOTAI_FIELD_STEPPER, false),
    FIELD(4, XIAOTAI_SETTING_GUIDANCE_ENABLED, "语音引导", XIAOTAI_FIELD_SWITCH, false),
    FIELD(5, SCREEN_WAKE_SETTING, "语音唤醒", XIAOTAI_FIELD_SWITCH, false),
    FIELD(6, XIAOTAI_SETTING_SCREEN_BRIGHTNESS, "屏幕亮度", XIAOTAI_FIELD_STEPPER, false),
    FIELD(7, XIAOTAI_SETTING_SCREEN_SLEEP, "自动息屏", XIAOTAI_FIELD_CHOICE, false),
    FIELD(8, XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE, "回应声", XIAOTAI_FIELD_CHOICE, false),
    FIELD(9, VISION_SETTING, "AI 看图", XIAOTAI_FIELD_SWITCH, false),
};
_Static_assert(COUNT(fields) <= XIAOTAI_VIEW_FIELDS_MAX, "every setting has a row");
#undef FIELD
enum { VOLUME = 0, SPEAKER, MICROPHONE, GAIN, GUIDANCE, WAKE, BRIGHTNESS, SLEEP, ACK };
#define BRIGHTNESS_STEP 10
static const char *const sleep_labels[] = {"1 分钟", "5 分钟", "10 分钟", "30 分钟", "永不"};
static const char *const custom_sleep_labels[] = {"1 分钟", "5 分钟", "10 分钟", "30 分钟", "永不", "自定义"};
static const int32_t sleep_values[] = {60, 300, 600, 1800, 0};
static const char *const voice_labels[] = {"女声", "男声"};

static struct {
    uint32_t timer;
    int64_t failed_until;
    bool failed, toast_due, changed, reprovisioning;
    uint16_t audio_entry_focus;
} settings;

static bool keys(void)
{
    return (companion.style->policy & XIAOTAI_APP_COMPANION_HOME_NETWORK_STATUS) != 0;
}

static bool audio_ready(void)
{
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    return media.ready;
}

static bool fails_at_home(void)
{
    return !keys() && companion.config.save_failure_on_home;
}

const char *companion_save_failure_home(void)
{
    return fails_at_home() && xiaotai_time_us() < settings.failed_until ? "设置保存失败" : NULL;
}

static bool showing_settings(void) { return companion_on_page(XIAOTAI_APP_COMPANION_PAGE_SETTINGS); }

static const xiaotai_setting_t *find_setting(const char *name)
{
    for (size_t i = 0; i < xiaotai_setting_count(); ++i) {
        const xiaotai_setting_t *setting = xiaotai_setting_at(i);
        if (!strcmp(setting->name, name)) return setting;
    }
    return NULL;
}

static int32_t step_for(size_t idx)
{
    if (idx == GAIN) return XIAOTAI_APP_COMPANION_GAIN_STEP_DB;
    if (idx == BRIGHTNESS) return BRIGHTNESS_STEP;
    return idx == VOLUME ? xiaotai_settings_volume_step_percent() : 1;
}

static int32_t clipped(int32_t value, int32_t min, int32_t max)
{
    return value < min ? min : value > max ? max : value;
}

static size_t choice(const int32_t *values, size_t count, int32_t value)
{
    for (size_t i = 0; i < count; ++i) if (value == values[i]) return i;
    return count;
}

static void add_form(xiaotai_view_t *view, size_t idx, int32_t value)
{
    const field_t *field = &fields[idx];
    const xiaotai_setting_t *entry = find_setting(field->name);
    if (!entry) return;
    xiaotai_view_field_t form = {.kind = field->kind, .label = field->label, .intent = field->intent,
        .value = field->inverted ? !value : value, .min = entry->min, .max = entry->max, .step = step_for(idx)};
    if (idx == SLEEP) {
        const size_t select = value < 0 ? COUNT(sleep_values) - 1U : choice(sleep_values, COUNT(sleep_values), value);
        form.options = select == COUNT(sleep_values) ? custom_sleep_labels : sleep_labels;
        form.option_count = select == COUNT(sleep_values) ? COUNT(custom_sleep_labels) : COUNT(sleep_labels);
        form.value = (int32_t)select;
        if (companion.config.save_choice_on_reselect && select < COUNT(sleep_values) &&
            value == sleep_values[select]) form.flags |= XIAOTAI_FIELD_CONFIRM_CURRENT;
    } else if (idx == ACK) {
        form.options = voice_labels;
        form.option_count = COUNT(voice_labels);
        if (companion.config.save_choice_on_reselect) form.flags |= XIAOTAI_FIELD_CONFIRM_CURRENT;
    } else if (idx == GAIN) form.text = "dB";
    else if (idx == VOLUME || idx == BRIGHTNESS) form.text = "%";
    if (xiaotai_quiesced()) form.state = XIAOTAI_STATE_DISABLED;
    companion.buffer->fields[view->field_count++] = form;
}

static void add_item(xiaotai_view_t *view, const char *label, const char *intent, int32_t value, bool disabled)
{
    companion.buffer->items[view->item_count++] = (xiaotai_view_item_t){.label = label, .intent = intent,
        .value = value, .state = disabled ? XIAOTAI_STATE_DISABLED : 0};
}

static void add_key_step(xiaotai_view_t *view, size_t idx, int32_t value, const char *up, const char *down, bool ready)
{
    const xiaotai_setting_t *setting = find_setting(fields[idx].name);
    if (!setting) return;
    const int32_t step = step_for(idx);
    add_item(view, up, fields[idx].intent, clipped(value + step, setting->min, setting->max), !ready || value >= setting->max);
    add_item(view, down, fields[idx].intent, clipped(value - step, setting->min, setting->max), !ready || value <= setting->min);
}

enum { ENTRY_REQUESTS, ENTRY_AUDIO, ENTRY_NETWORK };
static const struct { const char *page, *label; } entries[] = {
    [ENTRY_REQUESTS] = {XIAOTAI_APP_COMPANION_PAGE_REQUESTS, "好友申请"},
    [ENTRY_AUDIO] = {XIAOTAI_APP_COMPANION_PAGE_AUDIO, "音频诊断"},
    [ENTRY_NETWORK] = {XIAOTAI_APP_COMPANION_PAGE_NETWORK, "网络信息"},
};

static void build_settings(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    const bool by_key = keys();
    view->kind = by_key ? XIAOTAI_VIEW_LIST : XIAOTAI_VIEW_FORM;
    view->items = companion.buffer->items;
    view->fields = companion.buffer->fields;
    settings.audio_entry_focus = 0;
    int32_t value;
    if (by_key) {
        const bool ready = audio_ready();
        if (xiaotai_setting_get_int(fields[VOLUME].name, &value) == XIAOTAI_OK)
            add_key_step(view, VOLUME, value, "音量 +", "音量 −", ready);
        if (xiaotai_setting_get_int(fields[GAIN].name, &value) == XIAOTAI_OK)
            add_key_step(view, GAIN, value, "增益 +", "增益 −", ready);
        if (xiaotai_setting_get_int(fields[GUIDANCE].name, &value) == XIAOTAI_OK)
            add_item(view, value ? "语音引导：开" : "语音引导：关", fields[GUIDANCE].intent, !value, false);
    } else {
        for (size_t i = 0; i < COUNT(fields); ++i)
            if (xiaotai_setting_get_int(fields[i].name, &value) == XIAOTAI_OK) add_form(view, i, value);
    }
    for (size_t i = 0; i < COUNT(entries); ++i) {
        if (!companion_page_exists(entries[i].page) || (by_key ? i == ENTRY_NETWORK : i == ENTRY_REQUESTS))
            continue;
        if (by_key && i == ENTRY_AUDIO) settings.audio_entry_focus = view->item_count + 1U;
        add_item(view, entries[i].label, SETTINGS_ENTRY, (int32_t)i, false);
    }
    add_item(view, "重新配网", XIAOTAI_INTENT_APP_COMPANION_REPROVISION, 0, settings.reprovisioning);

    if (!fails_at_home()) {
        xiaotai_settings_status_t snapshot;
        xiaotai_settings_status(&snapshot);
        if (settings.failed || snapshot.packed_load_error != XIAOTAI_OK)
            view->status = keys() ? "保存失败" : "未保存";
        else if (snapshot.save_pending) view->status = "保存中";
        else if (settings.changed) view->status = "已保存";
    }
    if (settings.reprovisioning) view->status = "正在准备";
    companion.buffer->actions[0] = (xiaotai_view_action_t){.role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK,
        .intent = XIAOTAI_INTENT_APP_COMPANION_BACK};
    view->actions = companion.buffer->actions;
    view->action_count = 1;
}

static const companion_page_t page = {
    .id = XIAOTAI_APP_COMPANION_PAGE_SETTINGS, .title = "设置", .icon = XIAOTAI_ICON_SETTINGS, .build = build_settings,
};

static void next_check(void *unused);

static void schedule(void)
{
    if (settings.timer) {
        xiaotai_timer_cancel(settings.timer);
        settings.timer = 0;
    }
    const int64_t now = xiaotai_time_us();
    uint32_t delay = 0;
    if (companion.status.foreground && settings.failed_until > now)
        delay = (uint32_t)((settings.failed_until - now + 999) / 1000);
    if (showing_settings() && !fails_at_home()) {
        xiaotai_settings_status_t snapshot;
        xiaotai_settings_status(&snapshot);
        if (snapshot.save_pending && (!delay || delay > POLL_PENDING_MS)) delay = POLL_PENDING_MS;
    }
    if (delay) (void)xiaotai_timer_start(delay, next_check, NULL, &settings.timer);
}

static void next_check(void *unused)
{
    (void)unused;
    settings.timer = 0;
    if (showing_settings() || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) companion_render();
    schedule();
}

void companion_settings_visible(void)
{
    schedule();
    if (!settings.toast_due || !companion.status.foreground || !companion.has_display ||
        !companion_screen_awake() || companion.code_visible || companion_on_page(WECHAT_CODE_PAGE) ||
        companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL)) return;
    settings.toast_due = false;
    if (!showing_settings()) xiaotai_view_toast("设置未保存", companion_toast_ms());
}

void companion_settings_screen_changed(bool awake)
{
    if (awake) companion_settings_visible();
}

void companion_settings_code_visible(bool visible)
{
    companion.code_visible = visible;
    if (visible) companion_user_activity();
    else companion_settings_visible();
}

static void refused(const char *setting, xiaotai_err_t err)
{
    const char *text = xiaotai_quiesced() ? "维护中，设置暂不可修改" :
        !strcmp(setting, XIAOTAI_SETTING_AUDIO_VOLUME) ? "音量调整失败，请重试" :
        !strcmp(setting, XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED) ? "扬声器切换失败，请重试" : NULL;
    companion_refuse_text(setting, err, text);
    if (showing_settings()) companion_render();
}

static void refuse_invalid_choice(const char *setting)
{
    if (!companion.config.ignore_invalid_choice_values) refused(setting, XIAOTAI_ERR_INVALID_ARG);
}

static void change_value(const xiaotai_args_t *args, void *ctx)
{
    if (!companion.status.foreground) { ++companion.status.stale; return; }
    const field_t *field = ctx;
    const size_t idx = (size_t)(field - fields);
    if (xiaotai_quiesced()) { refused(field->name, XIAOTAI_ERR_BUSY); return; }
    if (!showing_settings() || companion.status.setup) { ++companion.status.stale; return; }
    /* A key selected in an already rendered view may arrive after audio failed. */
    if (keys() && (idx == VOLUME || idx == GAIN) && !audio_ready()) {
        companion_render();
        return;
    }
    const bool reselect = memchr(args->text, '\0', sizeof(args->text)) &&
        !strcmp(args->text, XIAOTAI_ARG_CHOICE_RECONFIRM);
    if (reselect && (!companion.config.save_choice_on_reselect || keys() || (idx != ACK && idx != SLEEP))) return;
    int32_t value = args->value;
    if (idx == ACK && companion.config.ignore_invalid_choice_values &&
        (value < 0 || (size_t)value >= COUNT(voice_labels))) return;
    if (idx == SLEEP) {
        if (value == (int32_t)COUNT(sleep_values)) {
            /* A custom value loaded from storage is only informational. */
            int32_t stored;
            if (xiaotai_setting_get_int(field->name, &stored) != XIAOTAI_OK) {
                refused(field->name, XIAOTAI_ERR_INVALID_ARG);
                return;
            }
            if (stored < 0 || choice(sleep_values, COUNT(sleep_values), stored) != COUNT(sleep_values)) {
                refuse_invalid_choice(field->name);
                return;
            }
            companion_render();
            return;
        }
        if (value < 0 || (size_t)value >= COUNT(sleep_values)) { refuse_invalid_choice(field->name); return; }
        value = sleep_values[value];
    }
    if (field->inverted) {
        if (value < 0 || value > 1) { refused(field->name, XIAOTAI_ERR_INVALID_ARG); return; }
        value = !value;
    }
    int32_t previous;
    xiaotai_err_t err = xiaotai_setting_get_int(field->name, &previous);
    if (reselect && err == XIAOTAI_OK) {
        if (previous == value) err = xiaotai_settings_confirm_int_selection(field->name, value);
        else ++companion.status.stale;
    } else if (err == XIAOTAI_OK && previous != value) {
        err = xiaotai_setting_set_int(field->name, value);
        int32_t applied;
        const bool confirm = idx == SPEAKER || idx == VOLUME;
        if (err == XIAOTAI_OK && (!confirm ||
                (xiaotai_setting_get_int(field->name, &applied) == XIAOTAI_OK && applied == value))) {
            settings.failed = false;
            settings.changed = true;
        }
    }
    if (err != XIAOTAI_OK) { refused(field->name, err); return; }
    companion_render();
    schedule();
}

static void set_owned(const char *name, void *unused)
{
    (void)name;
    (void)unused;
    if (companion.status.foreground) companion_render();
}

static void changed(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!strcmp(event->name, XIAOTAI_EVENT_SETTING_NOT_SAVED)) {
        if (companion.config.save_only_current_revision && !xiaotai_setting_failure_current(event)) return;
        settings.failed = true;
        if (fails_at_home()) {
            settings.failed_until = xiaotai_time_us() + XIAOTAI_APP_COMPANION_SAVE_FAILURE_MS * INT64_C(1000);
            if (companion.config.save_failure_toast && companion.status.foreground && companion.has_display)
                xiaotai_view_toast("设置保存失败，重启后可能恢复原设置", companion_toast_ms());
        } else if (!keys()) {
            settings.toast_due = true;
        }
    } else if (!strcmp(event->name, XIAOTAI_EVENT_SETTINGS_REFUSED)) {
        if (showing_settings()) refused(event->args.text, event->args.value);
        return;
    } else if (!strcmp(event->name, XIAOTAI_EVENT_SETTINGS_CHANGED)) {
        if (!strcmp(event->args.text, XIAOTAI_SETTING_SCREEN_SLEEP)) {
            const uint32_t seconds = event->args.value <= 0 ? 0 : (uint32_t)event->args.value;
            const uint32_t ms = seconds <= UINT32_MAX / 1000U ? seconds * 1000U : UINT32_MAX;
            for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
                const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
                if (part->sleep_timeout) part->sleep_timeout(ms);
            }
        } else if (!strcmp(event->args.text, XIAOTAI_SETTING_SCREEN_BRIGHTNESS) &&
                   event->args.value >= 1 && event->args.value <= 100) {
            for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
                const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
                if (part->brightness) part->brightness((uint8_t)event->args.value);
            }
        }
    } else {
        const bool failed = !strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_FAILED);
        if (!settings.reprovisioning) return;
        settings.reprovisioning = false;
        if (failed && showing_settings()) companion_refuse_text(XIAOTAI_INTENT_APP_COMPANION_REPROVISION,
            event->args.value, "准备失败，请重试");
    }
    if (showing_settings() || (fails_at_home() && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME))) companion_render();
    companion_settings_visible();
}

static void entry(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!showing_settings()) { ++companion.status.stale; return; }
    if (args->value < 0 || (size_t)args->value >= COUNT(entries)) {
        companion_refuse(SETTINGS_ENTRY, XIAOTAI_ERR_INVALID_ARG);
        return;
    }
    const uint16_t focus = args->value == ENTRY_AUDIO ? settings.audio_entry_focus : 0;
    const xiaotai_err_t err = companion_show_page_from_item(entries[args->value].page, 0, focus);
    if (err != XIAOTAI_OK) companion_refuse(SETTINGS_ENTRY, err);
}

static void reprovision_confirmed(bool accepted)
{
    if (!accepted || !showing_settings()) return;
    const uint32_t flags = companion.config.keep_saved_wifi_on_reprovision ? 0U : XIAOTAI_PROVISIONING_FORGET;
    const xiaotai_err_t err = xiaotai_provisioning_begin(flags);
    if (err != XIAOTAI_OK) {
        companion_refuse_text(XIAOTAI_INTENT_APP_COMPANION_REPROVISION, err, "准备失败，请重试");
    } else {
        settings.reprovisioning = true;
        companion_render();
    }
}

static void reprovision(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    if (!showing_settings()) { ++companion.status.stale; return; }
    if (xiaotai_quiesced()) { refused(XIAOTAI_INTENT_APP_COMPANION_REPROVISION, XIAOTAI_ERR_BUSY); return; }
    if (!settings.reprovisioning) {
        const char *const title = "重新配网", *const detail = "绑定会保留";
        const bool key_style = !strcmp(companion.style->name, XIAOTAI_APP_COMPANION_STYLE_KEYS);
        if (key_style ? companion_confirm_open_choice(title, detail, "确认配网", reprovision_confirmed) :
                        companion_confirm_open(title, detail, reprovision_confirmed)) return;
    }
    companion_refuse(XIAOTAI_INTENT_APP_COMPANION_REPROVISION, XIAOTAI_ERR_BUSY);
}

xiaotai_err_t companion_settings_init(void)
{
    static const xiaotai_setting_t emotion = {.name = XIAOTAI_SETTING_APP_COMPANION_EMOTION,
        .key = XIAOTAI_SETTING_KEY_APP_COMPANION_EMOTION, .max = XIAOTAI_APP_COMPANION_EMOTIONS - 1U, .changed = set_owned};
    static const xiaotai_setting_t ack = {.name = XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE,
        .key = XIAOTAI_SETTING_KEY_APP_COMPANION_ACK_VOICE, .max = 1, .changed = set_owned};
    xiaotai_err_t err = xiaotai_setting_register(&emotion);
    if (err == XIAOTAI_OK && xiaotai_intent_exists("wake.enable")) err = xiaotai_setting_register(&ack);
    if (err != XIAOTAI_OK || !companion.has_display) return err;
    err = companion_page_register(&page);
    for (size_t i = 0; err == XIAOTAI_OK && i < COUNT(fields); ++i)
        if (find_setting(fields[i].name)) err = xiaotai_intent_handle_ex(fields[i].intent, change_value,
            (void *)&fields[i], XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(SETTINGS_ENTRY, entry, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(XIAOTAI_INTENT_APP_COMPANION_REPROVISION, reprovision, NULL, XIAOTAI_INTENT_LOCAL);
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_SETTING_NOT_SAVED, changed, NULL},
        {XIAOTAI_EVENT_SETTINGS_REFUSED, changed, NULL},
        {XIAOTAI_EVENT_SETTINGS_CHANGED, changed, NULL},
        {XIAOTAI_EVENT_PROVISIONING_OPENED, changed, NULL},
        {XIAOTAI_EVENT_PROVISIONING_FAILED, changed, NULL},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < COUNT(events); ++i)
        err = xiaotai_event_subscribe_static(&events[i]);
    return err;
}

void companion_settings_reprovision_cancelled(void) { settings.reprovisioning = false; }

void companion_settings_reset(void) { memset(&settings, 0, sizeof(settings)); }
