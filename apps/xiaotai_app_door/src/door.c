// SPDX-License-Identifier: MIT
#include "door_internal.h"
#ifdef XIAOTAI_DOOR_WIDE
#include "templates_wide.h"
#endif

#include "xiaotai/modules/call.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/ui.h"

#include <stdio.h>
#include <string.h>

#define TALK_STOP "talk.stop"

xiaotai_app_door_config_t door_config;
door_state_t door;

static const door_feature_t *const features[] = {
#ifdef XIAOTAI_DOOR_WITH_VIDEO_CALL
    &door_with_video_call,
#endif
#ifdef XIAOTAI_DOOR_WITH_STREAM
    &door_with_stream,
#endif
#ifdef XIAOTAI_DOOR_WITH_STREAM_VIDEO
    &door_with_stream_video,
#endif
#ifdef XIAOTAI_DOOR_WITH_LOCK
    &door_with_lock,
#endif
    NULL,
};

static bool name_is(const xiaotai_event_t *event, const char *name)
{
    return strcmp(event->name, name) == 0;
}

static bool name_starts(const xiaotai_event_t *event, const char *prefix)
{
    return strncmp(event->name, prefix, strlen(prefix)) == 0;
}

xiaotai_err_t xiaotai_app_door_configure(const xiaotai_app_door_config_t *config)
{
    if (door.initialized) return XIAOTAI_ERR_INVALID_STATE;
    door_config = config ? *config : (xiaotai_app_door_config_t){0};
    return XIAOTAI_OK;
}

static const xiaotai_call_status_t *call_data(const xiaotai_event_t *event)
{
    return event->size == sizeof(xiaotai_call_status_t) ? event->data : NULL;
}

static bool feature_claim(const xiaotai_event_t *event)
{
    for (size_t i = 0; features[i]; ++i) {
        if (features[i]->claim && features[i]->claim(event)) return true;
    }
    return false;
}

bool door_lock_configured(void)
{
    for (size_t i = 0; features[i]; ++i) {
        if (features[i]->lock_configured) return features[i]->lock_configured();
    }
    return false;
}

xiaotai_err_t door_lock_request(void)
{
    for (size_t i = 0; features[i]; ++i) {
        if (features[i]->open_lock) return features[i]->open_lock();
    }
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

bool xiaotai_app_door_claim(const xiaotai_event_t *event)
{
    if (!event || !event->name) return false;
    if (!name_starts(event, "call.")) return feature_claim(event);
    if (name_is(event, XIAOTAI_EVENT_CALL_ENDED)) return door.session && door.classified_door;
    const xiaotai_call_status_t *call = call_data(event);
    return call && door_call_matches(call);
}

void door_register_view(const xiaotai_view_t *view)
{
    if (!door.published_page || strcmp(door.published_page, view->id) != 0) {
        xiaotai_args_t args = {0};
        (void)snprintf(args.text, sizeof(args.text), "%s", view->id);
        door.published_page = view->id;
        (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_DOOR_PAGE, &args, NULL, 0);
    }
    xiaotai_view_show(view);
}

void door_render(void)
{
    if (!door.foreground) return;
    switch (door.page) {
    case DOOR_CALL:
        door_call_render();
        break;
    case DOOR_OTHER_CALL:
    case DOOR_OTHER_RESULT:
    case DOOR_SETUP:
    case DOOR_ASSISTANT:
        door_fallback_render();
        break;
    default:
        door_render_home_page();
        break;
    }
}

void door_finish_presentation(void)
{
    door_overlay_close();
    door.page = DOOR_HOME;
    if (xiaotai_app_return() != XIAOTAI_OK) door_render();
}

void door_format_show(uint32_t generation, uint32_t *noted, const char *id)
{
    if (!door.foreground || !generation || *noted == generation) return;
    *noted = generation;
    const xiaotai_view_t toast = {
        .id = id, .layer = XIAOTAI_LAYER_TOAST, .kind = XIAOTAI_VIEW_NOTICE,
        .text = DOOR_FORMAT_ERROR, .duration_ms = XIAOTAI_TOAST_DEFAULT_MS,
    };
    xiaotai_view_show(&toast);
}

void door_format_clear(uint32_t generation, uint32_t *noted, const char *id)
{
    if (!generation || generation != *noted) return;
    *noted = 0;
    xiaotai_view_dismiss(XIAOTAI_LAYER_TOAST, id);
}

static void on_refresh(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!door.foreground) return;
    if (name_is(event, XIAOTAI_EVENT_SETTING_NOT_SAVED)) {
        const char *setting = event->args.text;
        if (strcmp(setting, XIAOTAI_SETTING_APP_DOOR_PEER) == 0 ||
            strcmp(setting, XIAOTAI_SETTING_APP_DOOR_SENDER) == 0 ||
            strcmp(setting, XIAOTAI_SETTING_AUDIO_MIC_MUTED) == 0 ||
            strcmp(setting, XIAOTAI_SETTING_AUDIO_VOLUME) == 0) {
            xiaotai_view_toast("设置保存失败，重启后可能恢复原设置", 0);
        }
    } else {
        if (door.overlay) xiaotai_ui_activity();
        door_render();
    }
}

static void read_settings(void)
{
    char peer[sizeof(door.peer)] = {0};
    (void)xiaotai_setting_get_text(XIAOTAI_SETTING_APP_DOOR_PEER, peer, sizeof(peer));
    if (strcmp(door.peer, peer)) {
        memcpy(door.peer, peer, sizeof(door.peer));
        ++door.peer_revision;
    }
    int32_t sender = 0;
    (void)xiaotai_setting_get_int(XIAOTAI_SETTING_APP_DOOR_SENDER, &sender);
    door.sender = sender != 0;
}

static void setting_changed(const char *name, void *ctx)
{
    (void)ctx;
    if (strcmp(name, XIAOTAI_SETTING_APP_DOOR_PEER) == 0) {
        xiaotai_call_status_t call;
        xiaotai_call_status(&call);
        if (call.session && call.session != door.classification_session) {
            door.classified_door = door_call_matches(&call);
            door.classification_session = call.session;
        }
    }
    read_settings();
    door_render();
}

void xiaotai_app_door_start(void) { read_settings(); }

void xiaotai_app_door_foreground(bool foreground)
{
    door.foreground = foreground;
    if (!foreground) {
        door_fallback_foreground(false);
        door_overlay_close();
        door_call_ring_update();
        door.published_page = NULL;
        (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_DOOR_PAGE, NULL, NULL, 0);
        return;
    }
    door.setup = false;
    if (xiaotai_app_entry() == XIAOTAI_APP_ENTRY_PRESENT) return;
    if (door.session) {
        door_page(door.classified_door ? DOOR_CALL : DOOR_OTHER_CALL);
        door_call_ring_update();
        return;
    }
    if (xiaotai_intent_exists(TALK_STOP)) (void)xiaotai_intent_post(TALK_STOP, NULL);
    door_page(DOOR_HOME);
    door_fallback_foreground(true);
}

xiaotai_err_t xiaotai_app_door_init(void)
{
    if (door.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (!door_config.title) door_config.title = "小钛可视门铃";
    if (!door_config.door_name) door_config.door_name = "入户门";
    const xiaotai_setting_t peer = {
        .name = XIAOTAI_SETTING_APP_DOOR_PEER, .key = XIAOTAI_SETTING_KEY_APP_DOOR_PEER,
        .kind = XIAOTAI_SETTING_TEXT, .default_text = "", .changed = setting_changed,
    };
    const xiaotai_setting_t sender = {
        .name = XIAOTAI_SETTING_APP_DOOR_SENDER, .key = XIAOTAI_SETTING_KEY_APP_DOOR_SENDER,
        .kind = XIAOTAI_SETTING_BOOL, .min = 0, .max = 1, .changed = setting_changed,
    };
    xiaotai_err_t result = xiaotai_setting_register(&peer);
    if (result == XIAOTAI_OK) result = xiaotai_setting_register(&sender);
#ifdef XIAOTAI_DOOR_WIDE
    if (result == XIAOTAI_OK) result = door_templates_register();
#endif
    if (result == XIAOTAI_OK) result = door_init_call();
    if (result == XIAOTAI_OK) result = door_init_open();
    if (result == XIAOTAI_OK) result = door_fallback_init();
    if (result == XIAOTAI_OK) result = door_init_at();
    for (size_t i = 0; result == XIAOTAI_OK && features[i]; ++i) {
        if (features[i]->init) result = features[i]->init();
    }
    static const struct {
        const char *name;
        xiaotai_intent_fn_t handler;
    } intents[] = {
        {XIAOTAI_INTENT_APP_DOOR_HOME, door_home_intent},
        {XIAOTAI_INTENT_APP_DOOR_DEVICES, door_devices_intent},
        {XIAOTAI_INTENT_APP_DOOR_SELECT, door_select_intent},
        {XIAOTAI_INTENT_APP_DOOR_BACK, door_back_intent},
        {XIAOTAI_INTENT_APP_DOOR_MONITOR, door_monitor_intent},
    };
    for (size_t i = 0; result == XIAOTAI_OK && i < sizeof(intents) / sizeof(*intents); ++i) {
        result = xiaotai_intent_handle(intents[i].name, intents[i].handler, NULL);
    }
    static const struct {
        const char *name;
        xiaotai_event_fn_t handler;
    } events[] = {
        {XIAOTAI_EVENT_CALL_INCOMING, door_on_call}, {XIAOTAI_EVENT_CALL_STATE, door_on_call},
        {XIAOTAI_EVENT_CALL_CONNECTED, door_on_call}, {XIAOTAI_EVENT_CALL_ENDED, door_on_call},
        {XIAOTAI_EVENT_SETTINGS_CHANGED, door_call_settings}, {XIAOTAI_EVENT_SETTINGS_REFUSED, door_call_settings},
        {XIAOTAI_EVENT_STARTED, on_refresh}, {XIAOTAI_EVENT_ACTIVITY, on_refresh},
        {XIAOTAI_EVENT_QUIESCE, on_refresh},
        {XIAOTAI_EVENT_CONTACTS_CHANGED, on_refresh}, {XIAOTAI_EVENT_NET_STATE, on_refresh},
        {XIAOTAI_EVENT_NET_UP, on_refresh}, {XIAOTAI_EVENT_NET_DOWN, on_refresh},
        {XIAOTAI_EVENT_NET_SIGNAL, on_refresh}, {XIAOTAI_EVENT_PLATFORM_ONLINE, on_refresh},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, on_refresh}, {XIAOTAI_EVENT_SETTING_NOT_SAVED, on_refresh},
    };
    for (size_t i = 0; result == XIAOTAI_OK && i < sizeof(events) / sizeof(*events); ++i) {
        result = xiaotai_event_subscribe(events[i].name, events[i].handler, NULL);
    }
    if (result != XIAOTAI_OK) return result;
    door.rows = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, DOOR_ROWS_MAX, sizeof(*door.rows));
    if (!door.rows) return XIAOTAI_ERR_NO_MEM;
    door.initialized = true;
    return XIAOTAI_OK;
}

void xiaotai_app_door_status(xiaotai_app_door_status_t *status)
{
    if (!status) return;
    xiaotai_settings_status_t settings;
    xiaotai_settings_status(&settings);
    *status = (xiaotai_app_door_status_t){
        .foreground = door.foreground, .presenting_call = door.foreground && door.page == DOOR_CALL,
        .overlay = door.foreground && door.overlay,
        .station_selected = door.peer[0] != '\0', .sender = door.sender, .save_pending = settings.save_pending,
        .state = door_home_state(), .door_calls = door.door_calls, .yielded = door.yielded,
        .lock_configured = door_lock_configured(), .opens = door.opens, .opens_done = door.opens_done,
    };
    if (door.published_page) (void)snprintf(status->page, sizeof(status->page), "%s", door.published_page);
}

void xiaotai_app_door_reset(void)
{
    xiaotai_timer_cancel(door.stream_stop_timer);
    xiaotai_mem_free(door.rows);
    door = (door_state_t){0};
    door_config = (xiaotai_app_door_config_t){0};
    door_call_reset();
    door_overlay_reset();
    door_fallback_reset();
}
