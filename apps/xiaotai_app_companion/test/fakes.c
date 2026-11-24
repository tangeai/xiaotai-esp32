// SPDX-License-Identifier: MIT
#include "fakes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/at.h"
#include "xiaotai/hal.h"
#include "xiaotai/ui.h"

companion_test_fixture_t fixture;

#ifdef COMPANION_TEST_ROOM_LCD
void ui_view_accept(const xiaotai_view_t *view);
#endif

#ifndef COMPANION_TEST_REAL_UI
uint32_t xiaotai_hal_needs(void) { return fixture.needs; }
bool xiaotai_ui_rejection_current(const xiaotai_args_t *args) { return args && args->value == 0; }
#endif
#if !defined(COMPANION_TEST_REAL_UI) && !defined(COMPANION_TEST_REAL_AT)
xiaotai_err_t xiaotai_at_verbs_register(const char *command, const xiaotai_at_verb_t *verbs, size_t count)
{
    assert(!strcmp(command, "AT+CALL") && count == 1 && !strcmp(verbs[0].word, "DISMISS"));
    return XIAOTAI_OK;
}
#endif

void xiaotai_provisioning_status(xiaotai_provisioning_status_t *status) { *status = fixture.provision; }
void xiaotai_net_status(xiaotai_net_status_t *status) { *status = fixture.net; }
bool xiaotai_net_has_saved_credentials(void) { return fixture.saved_credentials; }
void xiaotai_binding_status(xiaotai_binding_status_t *status) { *status = fixture.binding; }
void xiaotai_platform_status(xiaotai_platform_status_t *status) { *status = fixture.platform; }
xiaotai_err_t xiaotai_platform_endpoint(const char *service, char *out, size_t capacity)
{
    const char *base = !strcmp(service, XIAOTAI_PLATFORM_SERVICE_DEVICE) ? "http://device.invalid" : "mqtt://broker.invalid";
    if (strlen(base) >= capacity) return XIAOTAI_ERR_INVALID_SIZE;
    strcpy(out, base);
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_provisioning_begin(uint32_t flags)
{
    assert(xiaotai_on_owner());
    ++fixture.provisioning_begins;
    fixture.provisioning_begin_flags = (int32_t)flags;
    return XIAOTAI_OK;
}
bool xiaotai_provisioning_begin_cancellable(void) { return fixture.provisioning_begin_cancellable; }
xiaotai_err_t xiaotai_provisioning_cancel(void)
{
    assert(xiaotai_on_owner() && fixture.provisioning_begin_cancellable && fixture.provisioning_quiesce);
    fixture.provisioning_begin_cancellable = false;
    fixture.provision.phase = XIAOTAI_PROVISIONING_NONE;
    fixture.provision.error = XIAOTAI_OK;
    ++fixture.provisioning_cancels;
    xiaotai_quiesce_end(fixture.provisioning_quiesce);
    fixture.provisioning_quiesce = 0;
    return XIAOTAI_OK;
}
void xiaotai_settings_status(xiaotai_settings_status_t *status)
{
    *status = (xiaotai_settings_status_t){
        .save_pending = fixture.read_save_pending ? fixture.read_save_pending() : fixture.save_pending,
    };
}
uint8_t xiaotai_settings_volume_step_percent(void) { return 10; }
xiaotai_err_t xiaotai_settings_confirm_int_selection(const char *name, int32_t value)
{
    assert(xiaotai_on_owner() && strlen(name) < sizeof(fixture.confirmed_setting));
    strcpy(fixture.confirmed_setting, name);
    fixture.confirmed_value = value;
    ++fixture.choice_confirms;
    return fixture.confirm_result;
}

xiaotai_err_t xiaotai_binding_retry(void)
{
    assert(xiaotai_on_owner());
    ++fixture.binding_retries;
    return XIAOTAI_OK;
}

#ifndef COMPANION_TEST_REAL_INTERACTION
xiaotai_err_t xiaotai_interaction_register(const xiaotai_interaction_profile_t *profile)
{
    assert(!fixture.profile && strlen(profile->name) <= 15U);
    fixture.profile = profile;
    return XIAOTAI_OK;
}

void xiaotai_interaction_set_context(const char *context) { strcpy(fixture.context, context); }
void xiaotai_interaction_invalidate(void) { ++fixture.invalidations; }
#endif

xiaotai_err_t xiaotai_feedback_register(const xiaotai_feedback_table_t *feedback)
{
    assert(!fixture.feedback && feedback->effect_count);
    fixture.feedback = feedback;
    return XIAOTAI_OK;
}

static void show(const xiaotai_view_t *view, void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner());
#ifdef COMPANION_TEST_ROOM_LCD
    ui_view_accept(view);
#endif
    if (view->layer == XIAOTAI_LAYER_TOAST) {
        if (!view->text) {
            if (view->id && strcmp(view->id, fixture.toast_id)) return;
            fixture.toast[0] = '\0';
            fixture.toast_id[0] = '\0';
            fixture.toast_ms = 0;
            return;
        }
        ++fixture.toasts;
        assert(strlen(view->text) < sizeof(fixture.toast));
        strcpy(fixture.toast, view->text);
        snprintf(fixture.toast_id, sizeof(fixture.toast_id), "%s", view->id ? view->id : "");
        fixture.toast_ms = view->duration_ms;
        return;
    }
    if (view->layer == XIAOTAI_LAYER_OVERLAY) {
        ++fixture.overlays;
        snprintf(fixture.overlay_title, sizeof(fixture.overlay_title), "%s", view->title ? view->title : "");
        const char *body = view->kind == XIAOTAI_VIEW_LIST ? view->status : view->text;
        snprintf(fixture.overlay_body, sizeof(fixture.overlay_body), "%s", body ? body : "");
        return;
    }
    assert(view->id && strlen(view->id) < sizeof(fixture.id));
    strcpy(fixture.id, view->id);
    strcpy(fixture.title, view->title ? view->title : "");
    snprintf(fixture.text, sizeof(fixture.text), "%s", view->text ? view->text : "");
    strcpy(fixture.subtitle, view->subtitle ? view->subtitle : "");
    strcpy(fixture.status, view->status ? view->status : "");
    fixture.kind = view->kind;
    strcpy(fixture.emotion, view->emotion ? view->emotion : "");
    fixture.phase = view->phase;
    fixture.level = view->level;
    fixture.speaker = view->speaker;
    fixture.flags = view->flags;
    fixture.progress = view->progress;
    fixture.timer_mode = view->timer_mode;
    fixture.timer_s = view->timer_s;
    fixture.focus = view->focus;
    fixture.item_count = view->item_count;
    if (view->item_count) memcpy(fixture.items, view->items, view->item_count * sizeof(*view->items));
    fixture.action_count = view->action_count;
    if (view->action_count) memcpy(fixture.actions, view->actions, view->action_count * sizeof(*view->actions));
    fixture.field_count = view->field_count;
    if (view->field_count) memcpy(fixture.fields, view->fields, view->field_count * sizeof(*view->fields));
    ++fixture.views;
}

static void event(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!strcmp(event->name, XIAOTAI_EVENT_APP_COMPANION_REFUSED)) {
        ++fixture.refused;
        fixture.last_refusal = event->args;
    } else {
        assert(!strcmp(event->name, XIAOTAI_EVENT_APP_COMPANION_PAGE));
        ++fixture.page_events;
    }
}

static void stop_talk(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    assert(args->value == 0);
    ++fixture.talk_stops;
}

static void setup_intent(const xiaotai_args_t *args, void *ctx)
{
    const char *name = ctx;
    if (!strcmp(name, XIAOTAI_INTENT_PROVISIONING_BEGIN)) {
        ++fixture.provisioning_begins;
        fixture.provisioning_begin_flags = args->value;
    } else if (!strcmp(name, XIAOTAI_INTENT_PROVISIONING_RETRY)) ++fixture.provisioning_retries;
    else if (!strcmp(name, XIAOTAI_INTENT_PROVISIONING_CANCEL)) ++fixture.provisioning_cancels;
    else if (!strcmp(name, XIAOTAI_INTENT_BINDING_ANNOUNCE)) ++fixture.binding_announcements;
    else if (!strcmp(name, XIAOTAI_INTENT_BINDING_RETRY)) (void)xiaotai_binding_retry();
    else assert(false);
}

static void navigation_page(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    view->kind = XIAOTAI_VIEW_NOTICE;
}

static void wake_enable(const xiaotai_args_t *args, void *unused) { (void)args; (void)unused; }

#ifdef COMPANION_TEST_PAGES
xiaotai_err_t __real_xiaotai_intent_post(const char *name, const xiaotai_args_t *args);
xiaotai_err_t __wrap_xiaotai_intent_post(const char *name, const xiaotai_args_t *args)
{
    if (fixture.fail_talk_stop && !strcmp(name, "talk.stop")) return XIAOTAI_ERR_NO_MEM;
    return __real_xiaotai_intent_post(name, args);
}
#endif

xiaotai_err_t companion_fixture_init(void)
{
    xiaotai_view_set_sink(show, NULL);
    xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_APP_COMPANION_REFUSED, event, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_APP_COMPANION_PAGE, event, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle("talk.stop", stop_talk, NULL);
    static const char *const setup_intents[] = {XIAOTAI_INTENT_PROVISIONING_BEGIN, XIAOTAI_INTENT_PROVISIONING_RETRY,
        XIAOTAI_INTENT_PROVISIONING_CANCEL, XIAOTAI_INTENT_BINDING_ANNOUNCE, XIAOTAI_INTENT_BINDING_RETRY};
    for (size_t n = 0; err == XIAOTAI_OK && n < sizeof(setup_intents) / sizeof(setup_intents[0]); ++n) {
        err = xiaotai_intent_handle(setup_intents[n], setup_intent, (void *)setup_intents[n]);
    }
    if (fixture.navigation_pages) {
        static const companion_page_t pages[] = {
            {.id = "call", .title = "Call", .build = navigation_page},
            {.id = "result", .title = "Result", .build = navigation_page},
            {.id = "wechat.code", .title = "Code", .build = navigation_page},
            {.id = "room.form", .title = "Room code", .build = navigation_page},
        };
        for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]) && err == XIAOTAI_OK; ++i) {
            err = companion_page_register(&pages[i]);
        }
    }
    if (fixture.board_settings) {
        static const xiaotai_setting_t board[] = {
            {.name = XIAOTAI_SETTING_AUDIO_VOLUME, .key = "volume", .max = 100, .default_value = 40},
            {.name = XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, .key = "spk_mute", .kind = XIAOTAI_SETTING_BOOL, .max = 1},
            {.name = XIAOTAI_SETTING_AUDIO_MIC_MUTED, .key = "mic_mute", .kind = XIAOTAI_SETTING_BOOL, .max = 1},
            {.name = XIAOTAI_SETTING_AUDIO_MIC_GAIN, .key = "mic_gain", .max = 36, .default_value = 24},
            {.name = XIAOTAI_SETTING_GUIDANCE_ENABLED, .key = "guidance", .kind = XIAOTAI_SETTING_BOOL, .max = 1, .default_value = 1},
            {.name = XIAOTAI_SETTING_SCREEN_SLEEP, .key = "sleep", .max = 86400, .default_value = 300},
            {.name = "wake.enabled", .key = "wake_on", .kind = XIAOTAI_SETTING_BOOL, .max = 1, .default_value = 1},
        };
        for (size_t i = 0; i < sizeof(board) / sizeof(board[0]) && err == XIAOTAI_OK; ++i)
            err = xiaotai_setting_register(&board[i]);
        if (err == XIAOTAI_OK) err = xiaotai_intent_handle("wake.enable", wake_enable, NULL);
    }
    return err;
}
