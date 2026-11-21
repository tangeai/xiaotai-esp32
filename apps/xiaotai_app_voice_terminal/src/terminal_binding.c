// SPDX-License-Identifier: MIT
#include "terminal_internal.h"

#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/feedback.h"
#include "xiaotai/modules/binding.h"
#ifdef XIAOTAI_TERMINAL_WITH_SETTINGS
#include "xiaotai/modules/settings.h"
#endif

#define WAIT_MS XIAOTAI_APP_VOICE_TERMINAL_ANNOUNCE_WAIT_MS

static struct {
    bool guidance, suspended, visible, owned, attempted;
    uint32_t timer;
} voice;

static void tick(void *ctx);

static uint32_t interval(void) { return (uint32_t)terminal_config.announce_repeat_s * 1000U; }

static void cancel(void)
{
    xiaotai_timer_cancel(voice.timer);
    voice.timer = 0;
}

static void schedule(uint32_t ms)
{
    if (!voice.timer && xiaotai_timer_start(ms, tick, NULL, &voice.timer) != XIAOTAI_OK)
        xiaotai_eventlog_warn("terminal", "binding.announce.timer", 0, XIAOTAI_ERR_NO_MEM);
}

static bool visible(const xiaotai_binding_status_t *status)
{
    return status->phase == XIAOTAI_BINDING_WAITING && status->seconds_left && status->verification[0];
}

static bool allowed(void)
{
    return !voice.suspended && (!terminal_config.announce_follows_guidance || voice.guidance) &&
           !xiaotai_quiesced() && !xiaotai_restart_pending();
}

static void hide(void)
{
    cancel();
    voice.visible = voice.owned = voice.attempted = false;
}

static void finished(const xiaotai_binding_status_t *status)
{
    if (!voice.owned || status->announcing) return;
    voice.owned = false;
    if (terminal_config.announce_repeat_from_end) {
        cancel();
        if (interval() && visible(status) && allowed()) schedule(interval());
    }
}

static void tick(void *ctx)
{
    (void)ctx;
    voice.timer = 0;
    xiaotai_binding_status_t status;
    xiaotai_binding_status(&status);
    if (!visible(&status)) {
        hide();
        return;
    }
    if (!allowed()) return;
    if (voice.owned && terminal_config.announce_repeat_from_end) {
        // Observe completion even if a full owner queue lost the event.
        if (status.announcing) schedule(WAIT_MS);
        else finished(&status);
        return;
    }
    finished(&status);
    if (xiaotai_feedback_sound_pending()) {
        schedule(WAIT_MS);
        return;
    }
    const xiaotai_err_t err = xiaotai_binding_announce();
    voice.attempted = true;
    if (err == XIAOTAI_OK) {
        voice.owned = true;
        if (interval()) schedule(terminal_config.announce_repeat_from_end ? WAIT_MS : interval());
    } else if (err == XIAOTAI_ERR_BUSY) {
        if (interval()) schedule(interval());
    } else {
        xiaotai_eventlog_warn("terminal", "binding.announce.failed", 0, err);
    }
}

static void changed(const xiaotai_binding_status_t *status)
{
    if (!visible(status)) {
        hide();
        return;
    }
    if (!voice.visible) {
        voice.visible = true;
        if (allowed()) {
            if (terminal_config.announce_on_code) tick(NULL);
            else if (interval()) schedule(interval());
        }
    } else {
        finished(status);
    }
}

static void on_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    xiaotai_binding_status_t status;
    xiaotai_binding_status(&status);
    if (strcmp(event->name, XIAOTAI_EVENT_BINDING_CHANGED) == 0 && event->data &&
        event->size == sizeof(status) && !visible(event->data)) {
        // The prior code may have disappeared before the owner read this snapshot.
        hide();
    }
#ifdef XIAOTAI_TERMINAL_WITH_SETTINGS
    if (strcmp(event->name, XIAOTAI_EVENT_SETTINGS_CHANGED) == 0 &&
        strcmp(event->args.text, XIAOTAI_SETTING_GUIDANCE_ENABLED) != 0) return;
    if (strcmp(event->name, XIAOTAI_EVENT_STARTED) == 0 ||
        strcmp(event->name, XIAOTAI_EVENT_SETTINGS_CHANGED) == 0) {
        int32_t value = 1;
        (void)xiaotai_setting_get_int(XIAOTAI_SETTING_GUIDANCE_ENABLED, &value);
        const bool before = allowed();
        voice.guidance = value != 0;
        if (!allowed()) {
            cancel();
            if (voice.owned) xiaotai_binding_announce_stop();
            voice.owned = false;
        } else if (!before && voice.visible && visible(&status)) {
            if (terminal_config.announce_on_code) tick(NULL);
            else if (interval()) schedule(interval());
        }
    }
#else
    (void)event;
#endif
    changed(&status);
}

void terminal_binding_suspend(void)
{
    voice.suspended = true;
    cancel();
    xiaotai_binding_announce_stop();
    voice.owned = false;
}

void terminal_binding_resume(void)
{
    if (!voice.suspended) return;
    voice.suspended = false;
    xiaotai_binding_status_t status;
    xiaotai_binding_status(&status);
    if (!voice.visible || !visible(&status)) changed(&status);
    else if (allowed()) {
        if (terminal_config.announce_on_code && !voice.attempted) tick(NULL);
        else if (interval()) schedule(interval());
    }
}

xiaotai_err_t terminal_binding_init(void)
{
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_STARTED, on_event, NULL},
        {XIAOTAI_EVENT_BINDING_CODE, on_event, NULL},
        {XIAOTAI_EVENT_BINDING_CHANGED, on_event, NULL},
        {XIAOTAI_EVENT_BINDING_ANNOUNCED, on_event, NULL},
#ifdef XIAOTAI_TERMINAL_WITH_SETTINGS
        {XIAOTAI_EVENT_SETTINGS_CHANGED, on_event, NULL},
#endif
    };
    voice.guidance = true;
    for (size_t i = 0; i < sizeof(events) / sizeof(*events); ++i) {
        const xiaotai_err_t err = xiaotai_event_subscribe_static(&events[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

void terminal_binding_teardown(void) { memset(&voice, 0, sizeof(voice)); }
