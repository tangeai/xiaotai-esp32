// SPDX-License-Identifier: MIT
/* The volume dialog: what the settings page offers for the speaker, over a
 * running call or conversation, without leaving it. It goes away by itself. */
#include "companion_internal.h"

#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"

#define OVERLAY "companion.volume"
#define LEVEL_INTENT "companion.volume.level"
#define SPEAKER_INTENT "companion.volume.speaker"
#define CLOSE_INTENT "companion.volume.close"
#define SHOWN_MS 6000U

static struct {
    int64_t until_us;
    bool registered;
} s;

static bool showing(void) { return s.until_us && xiaotai_time_us() < s.until_us; }

bool companion_volume_available(void) { return s.registered; }

static void show(void)
{
    int32_t level = 0, muted = 0;
    (void)xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &level);
    (void)xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, &muted);
    const xiaotai_view_field_t fields[] = {
        {.kind = XIAOTAI_FIELD_STEPPER, .label = "音量", .intent = LEVEL_INTENT, .value = level, .max = 100,
         .step = xiaotai_settings_volume_step_percent(), .text = "%"},
        {.kind = XIAOTAI_FIELD_SWITCH, .label = "扬声器", .intent = SPEAKER_INTENT, .value = !muted, .max = 1},
    };
    const xiaotai_view_action_t close = {.role = XIAOTAI_ROLE_BACK, .label = "完成", .intent = CLOSE_INTENT};
    const xiaotai_view_t view = {.kind = XIAOTAI_VIEW_FORM, .layer = XIAOTAI_LAYER_OVERLAY, .id = OVERLAY,
        .title = "音量", .fields = fields, .field_count = 2, .actions = &close, .action_count = 1,
        .duration_ms = SHOWN_MS};
    s.until_us = xiaotai_time_us() + (int64_t)SHOWN_MS * 1000;
    xiaotai_view_show(&view);
}

static bool may_act(void)
{
    if (companion.status.foreground && !companion.status.setup && !companion.code_visible && !companion.confirm_resolved)
        return true;
    ++companion.status.stale;
    return false;
}

static void on_open(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    if (!may_act()) return;
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL) && !companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) {
        ++companion.status.stale;
        return;
    }
    show();
}

static void change(const char *setting, int32_t value)
{
    if (!may_act() || !showing()) return;
    const xiaotai_err_t err = xiaotai_setting_set_int(setting, value);
    if (err != XIAOTAI_OK) companion_refuse_text(setting, err, "音量调整失败，请重试");
    show();
}

static void on_level(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    change(XIAOTAI_SETTING_AUDIO_VOLUME, args->value);
}

static void on_speaker(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    change(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, !args->value);
}

static void on_close(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    s.until_us = 0;
    xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, OVERLAY);
}

xiaotai_err_t companion_volume_init(void)
{
    int32_t level;
    if (!companion.has_display || !(companion.style->needs_all & XIAOTAI_NEED_TOUCH) ||
        xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &level) != XIAOTAI_OK) return XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_intent_handle_ex(COMPANION_VOLUME_INTENT, on_open, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(LEVEL_INTENT, on_level, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(SPEAKER_INTENT, on_speaker, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(CLOSE_INTENT, on_close, NULL, XIAOTAI_INTENT_LOCAL);
    s.registered = err == XIAOTAI_OK;
    return err;
}

void companion_volume_reset(void)
{
    s.until_us = 0;
    s.registered = false;
}
