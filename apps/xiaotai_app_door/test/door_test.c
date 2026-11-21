// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_c61_ota_image_admission.py
// Replaces legacy: tests/regression/test_native_preferences.py
// Replaces legacy: tests/regression/test_p4_h264_feedback.py
// Replaces legacy: tests/lvgl/test_door_preferences.c::door_preferences
// Replaces legacy: tests/regression/test_wt99_h5_local_stop.py
// Replaces legacy: tests/lvgl/test_door_preferences.c
#include "door_internal.h"
#include "fakes.h"

#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/modules/video_call.h"
#include "xiaotai/modules/stream_video.h"
#include "xiaotai/port/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

typedef struct {
    char id[32], status[96], text[96], toast[96], title[96], subtitle[128];
    char toast_id[32];
    char overlay_id[32], overlay_text[96], overlay_title[96];
    xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX], overlay_actions[XIAOTAI_VIEW_ACTIONS_MAX];
    size_t action_count, overlay_action_count;
    xiaotai_view_field_t field;
    xiaotai_app_door_call_view_t call;
    uint8_t timer_mode;
    unsigned format_toasts;
    char rows[DOOR_ROWS_MAX][65];
    uint8_t item_state[DOOR_ROWS_MAX];
    int32_t item_value[DOOR_ROWS_MAX];
    unsigned flags, kind;
    size_t count;
    xiaotai_app_door_state_t state;
    bool menu, network;
} snapshot_t;

static snapshot_t view;
static xiaotai_signal_t ready;
static const char *foreground;
static unsigned companion_calls, companion_ai, stop_ai;
static unsigned companion_setup;
static xiaotai_app_entry_t companion_entry;
static int opens, open_result;

static void capture(const xiaotai_view_t *shown, void *ctx)
{
    (void)ctx;
    if (shown->layer == XIAOTAI_LAYER_TOAST) {
        if ((shown->flags & XIAOTAI_VIEW_DISMISS) && shown->id && strcmp(shown->id, view.toast_id)) return;
        (void)snprintf(view.toast, sizeof(view.toast), "%s", shown->text ? shown->text : "");
        (void)snprintf(view.toast_id, sizeof(view.toast_id), "%s",
                       shown->text && shown->id ? shown->id : "");
        if (strcmp(view.toast, DOOR_FORMAT_ERROR) == 0) ++view.format_toasts;
        return;
    }
    CHECK(shown->action_count <= XIAOTAI_VIEW_ACTIONS_MAX);
    if (shown->layer == XIAOTAI_LAYER_OVERLAY) {
        (void)snprintf(view.overlay_id, sizeof(view.overlay_id), "%s", shown->flags & XIAOTAI_VIEW_DISMISS ? "" : shown->id);
        (void)snprintf(view.overlay_title, sizeof(view.overlay_title), "%s", shown->title ? shown->title : "");
        (void)snprintf(view.overlay_text, sizeof(view.overlay_text), "%s", shown->text ? shown->text : "");
        view.overlay_action_count = shown->action_count;
        if (shown->action_count) memcpy(view.overlay_actions, shown->actions, shown->action_count * sizeof(*shown->actions));
        view.field = shown->field_count ? shown->fields[0] : (xiaotai_view_field_t){0};
        return;
    }
    CHECK(shown->item_count <= DOOR_ROWS_MAX);
    view.kind = shown->kind;
    view.flags = shown->flags;
    view.count = shown->item_count;
    view.timer_mode = shown->timer_mode;
    view.action_count = shown->action_count;
    if (shown->action_count) memcpy(view.actions, shown->actions, shown->action_count * sizeof(*shown->actions));
    view.menu = false;
    view.network = shown->indicator_count > 0 && shown->indicators[0].intent &&
                   strcmp(shown->indicators[0].intent, XIAOTAI_INTENT_SYSTEM_NETWORK) == 0;
    (void)snprintf(view.id, sizeof(view.id), "%s", shown->id ? shown->id : "");
    (void)snprintf(view.status, sizeof(view.status), "%s", shown->status ? shown->status : "");
    (void)snprintf(view.text, sizeof(view.text), "%s", shown->text ? shown->text : "");
    (void)snprintf(view.title, sizeof(view.title), "%s", shown->title ? shown->title : "");
    (void)snprintf(view.subtitle, sizeof(view.subtitle), "%s", shown->subtitle ? shown->subtitle : "");
    if (shown->custom_data && shown->custom_size == sizeof(xiaotai_app_door_home_view_t)) {
        view.state = ((const xiaotai_app_door_home_view_t *)shown->custom_data)->state;
    }
    if (shown->custom_data && shown->custom_size == sizeof(view.call) && !strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_CALL)) {
        view.call = *(const xiaotai_app_door_call_view_t *)shown->custom_data;
    }
    for (size_t i = 0; i < shown->action_count; i++) {
        if (shown->actions[i].intent && strcmp(shown->actions[i].intent, XIAOTAI_INTENT_SYSTEM_MENU) == 0) {
            CHECK(shown->actions[i].trigger == XIAOTAI_TRIGGER_LONG_PRESS);
            view.menu = true;
        }
    }
    for (size_t i = 0; i < shown->item_count; i++) {
        (void)snprintf(view.rows[i], sizeof(view.rows[i]), "%s", shown->items[i].label);
        view.item_state[i] = shown->items[i].state;
        view.item_value[i] = shown->items[i].value;
    }
}

static void opened(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    ++opens;
    open_result = event->args.value;
}

static void call_action(const xiaotai_args_t *args, void *ctx)
{
    const char *name = ctx;
    const uint32_t session = (uint32_t)args->value;
    CHECK(session == fake_call.session);
    if (!strcmp(name, XIAOTAI_INTENT_CALL_ACCEPT)) (void)xiaotai_call_accept(session);
    else if (!strcmp(name, XIAOTAI_INTENT_CALL_REJECT)) (void)xiaotai_call_reject(session);
    else if (!strcmp(name, XIAOTAI_INTENT_CALL_HANGUP)) (void)xiaotai_call_hangup(session);
    else CHECK(false);
}

static xiaotai_err_t init_fixture(void)
{
    xiaotai_view_set_sink(capture, NULL);
    CHECK(xiaotai_event_subscribe(XIAOTAI_EVENT_APP_DOOR_OPENED, opened, NULL) == XIAOTAI_OK);
    static const char *const controls[] = {XIAOTAI_INTENT_CALL_ACCEPT, XIAOTAI_INTENT_CALL_REJECT, XIAOTAI_INTENT_CALL_HANGUP};
    for (size_t i = 0; i < sizeof(controls) / sizeof(*controls); ++i)
        CHECK(xiaotai_intent_handle(controls[i], call_action, (void *)controls[i]) == XIAOTAI_OK);
    return fake_store_init();
}

#ifdef XIAOTAI_DOOR_WITH_WECHAT
static void wechat_action(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    CHECK(args->value && (uint32_t)args->value == fake_wechat.session);
    ++fake_wechat_calls;
}

static xiaotai_err_t wechat_init(void)
{
    CHECK(xiaotai_intent_handle(XIAOTAI_INTENT_WECHAT_ACCEPT, wechat_action, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_intent_handle(XIAOTAI_INTENT_WECHAT_REJECT, wechat_action, NULL) == XIAOTAI_OK);
    return xiaotai_intent_handle(XIAOTAI_INTENT_WECHAT_HANGUP, wechat_action, NULL);
}
#endif

static void check_sync(void *ctx)
{
    (void)ctx;
    foreground = xiaotai_app_foreground();
    xiaotai_signal_give(&ready);
}

static void sync(void)
{
    CHECK(xiaotai_run(check_sync, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&ready, 1000));
}

static void on_owner(void (*fn)(void *), void *ctx)
{
    CHECK(xiaotai_run(fn, ctx) == XIAOTAI_OK);
    sync();
}

static void emit(const char *name, int32_t value, const void *data, size_t size)
{
    const xiaotai_args_t args = {.value = value};
    CHECK(xiaotai_event_emit(name, &args, data, size) == XIAOTAI_OK);
}

static void intent_text(const char *name, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (text) snprintf(args.text, sizeof(args.text), "%s", text);
    CHECK(xiaotai_intent_post(name, &args) == XIAOTAI_OK);
    sync();
}

static void intent(const char *name, int32_t value) { intent_text(name, value, NULL); }

static const xiaotai_view_action_t *visible_action(const char *name)
{
    for (size_t i = 0; i < view.action_count; ++i) {
        if (!strcmp(name, view.actions[i].intent)) return &view.actions[i];
    }
    return NULL;
}

static void tap(const char *name)
{
    const xiaotai_view_action_t *action = visible_action(name);
    CHECK(action && !(action->state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY)));
    intent(action->intent, action->value); // Same args as UI's ui_input on TAP.
}

static void save_peer(void *peer) { CHECK(xiaotai_setting_set_text(XIAOTAI_SETTING_APP_DOOR_PEER, peer) == XIAOTAI_OK); }
static void record_call(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    companion_calls++;
}

static void record_ai(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    companion_ai++;
}

static void record_setup(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    ++companion_setup;
}

static void wrap_up_call(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    (void)xiaotai_app_return();
}

static void stop_talk(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    stop_ai++;
}

static void companion_page(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    CHECK((args->value > 0 ? xiaotai_app_next() : xiaotai_app_previous()) == XIAOTAI_OK);
}

static xiaotai_err_t companion_init(void)
{
    CHECK(xiaotai_event_subscribe(XIAOTAI_EVENT_CALL_INCOMING, record_call, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_event_subscribe(XIAOTAI_EVENT_CALL_ENDED, wrap_up_call, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_event_subscribe("ai_talk.state", record_ai, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_event_subscribe(XIAOTAI_EVENT_PROVISIONING_CHANGED, record_setup, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_event_subscribe(XIAOTAI_EVENT_BINDING_CODE, record_setup, NULL) == XIAOTAI_OK);
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    CHECK(xiaotai_event_subscribe(XIAOTAI_EVENT_WECHAT_INCOMING, record_call, NULL) == XIAOTAI_OK);
#endif
    CHECK(xiaotai_intent_handle(XIAOTAI_INTENT_SYSTEM_MENU, stop_talk, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_intent_handle(XIAOTAI_INTENT_SYSTEM_NETWORK, stop_talk, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_intent_handle("talk.stop", stop_talk, NULL) == XIAOTAI_OK);
    return xiaotai_intent_handle("fixture.page", companion_page, NULL);
}

static void companion_foreground(bool active)
{
    if (active) companion_entry = xiaotai_app_entry();
}

static const char *const door_handles[] = {
    XIAOTAI_EVENT_CALL_INCOMING, XIAOTAI_EVENT_CALL_STATE, XIAOTAI_EVENT_CALL_CONNECTED, XIAOTAI_EVENT_CALL_ENDED,
    XIAOTAI_EVENT_STREAM_STATE, XIAOTAI_EVENT_STREAM_STARTED, XIAOTAI_EVENT_STREAM_ENDED,
    XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED,
    XIAOTAI_EVENT_VIDEO_CALL_STATE, XIAOTAI_EVENT_VIDEO_CALL_REMOTE, XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, NULL,
};
static const char *const companion_handles[] = {
    XIAOTAI_EVENT_CALL_INCOMING, XIAOTAI_EVENT_CALL_STATE, XIAOTAI_EVENT_CALL_ENDED, "ai_talk.state",
    XIAOTAI_EVENT_PROVISIONING_CHANGED, XIAOTAI_EVENT_BINDING_CODE,
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    XIAOTAI_EVENT_WECHAT_INCOMING, XIAOTAI_EVENT_WECHAT_STATE, XIAOTAI_EVENT_WECHAT_CONNECTED,
    XIAOTAI_EVENT_WECHAT_ENDED,
#endif
    NULL,
};
static const xiaotai_application_t door_app = {
    .name = "door", .init = xiaotai_app_door_init, .start = xiaotai_app_door_start,
    .foreground = xiaotai_app_door_foreground, .handles = door_handles, .claim = xiaotai_app_door_claim,
};
static const xiaotai_application_t companion_app = {
    .name = "companion", .init = companion_init, .foreground = companion_foreground, .handles = companion_handles,
};
static const xiaotai_module_t fixture = {.name = "fixture", .init = init_fixture};

static void start(bool with_companion)
{
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    static const xiaotai_module_t wechat = {.name = "wechat", .init = wechat_init};
    static const xiaotai_module_t *const modules[] = {&fixture, &wechat};
#else
    static const xiaotai_module_t *const modules[] = {&fixture};
#endif
    static const xiaotai_application_t *const apps[] = {&door_app, &companion_app};
    const xiaotai_framework_config_t config = {
        .modules = modules, .module_count = sizeof(modules) / sizeof(*modules),
        .applications = apps, .application_count = with_companion ? 2U : 1U,
    };
    CHECK(xiaotai_signal_init(&ready) == XIAOTAI_OK);
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    sync();
}

static void stop(void)
{
    xiaotai_framework_reset();
    xiaotai_app_door_reset();
    xiaotai_signal_deinit(&ready);
}

static void change_home(void *context)
{
    const xiaotai_app_door_state_t target = *(const xiaotai_app_door_state_t *)context;
    fake_net.state = target == XIAOTAI_APP_DOOR_NO_NETWORK ? XIAOTAI_NET_WAITING : XIAOTAI_NET_UP;
    fake_platform.phase = target == XIAOTAI_APP_DOOR_NOT_READY ? XIAOTAI_PLATFORM_DISCOVERY : XIAOTAI_PLATFORM_ONLINE;
    fake_list = (xiaotai_contacts_status_t){
        .generation = 1, .page_count = target >= XIAOTAI_APP_DOOR_STATION_OFFLINE ? 2 : 0,
    };
    fake_contacts[0] = (xiaotai_contact_t){.kind = 0, .id = "wechat", .name = "WeChat", .online = true};
    fake_contacts[1] = (xiaotai_contact_t){.kind = 1, .id = "outside", .name = "Station", .online_known = true,
                                          .online = target == XIAOTAI_APP_DOOR_STATION_ONLINE};
    if (target == XIAOTAI_APP_DOOR_STREAMING) emit(XIAOTAI_EVENT_STREAM_STARTED, 101, NULL, 0);
    if (target == XIAOTAI_APP_DOOR_NO_NETWORK) {
        const xiaotai_stream_ended_t ended = {.generation = 101, .started = true};
        emit(XIAOTAI_EVENT_STREAM_ENDED, 0, &ended, sizeof(ended));
    }
    emit(XIAOTAI_EVENT_CONTACTS_CHANGED, 0, NULL, 0);
}

static void set_overlay(void *open) { door.overlay = *(const bool *)open ? DOOR_DIALOG_OPEN : DOOR_DIALOG_NONE; }

static void refresh_station(void *id)
{
    (void)snprintf(fake_contacts[1].id, sizeof(fake_contacts[1].id), "%s", (const char *)id);
    ++fake_list.generation;
    emit(XIAOTAI_EVENT_CONTACTS_CHANGED, 0, NULL, 0);
}

static void freeze(void *ticket)
{
    CHECK(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, ticket) == XIAOTAI_OK);
}

static void thaw(void *ticket) { xiaotai_quiesce_end(*(const uint32_t *)ticket); }

static void home_test(void)
{
    static const char *const texts[] = {
        "远程查看中", "网络未连接", "设备尚未就绪", "尚未选择门外机", "门外机已移除", "门外机离线", "门外机在线",
    };
    start(true);
    CHECK(strcmp(view.id, "door.home") == 0 && view.menu && view.network);
    CHECK(view.flags & XIAOTAI_VIEW_MAY_SLEEP);
    for (xiaotai_app_door_state_t state = XIAOTAI_APP_DOOR_STREAMING;
         state <= XIAOTAI_APP_DOOR_STATION_ONLINE; ++state) {
        if (state == XIAOTAI_APP_DOOR_STATION_REMOVED) on_owner(save_peer, "outside");
        on_owner(change_home, &state);
        sync();
        CHECK(view.state == state && strcmp(view.status, texts[state]) == 0);
        if (state == XIAOTAI_APP_DOOR_STREAMING) CHECK(!(view.flags & XIAOTAI_VIEW_MAY_SLEEP));
    }
    intent(XIAOTAI_INTENT_APP_DOOR_DEVICES, 0);
    CHECK(strcmp(view.id, "door.devices") == 0 && fake_refreshes == 1);
    CHECK(view.count == 1 && strcmp(view.rows[0], "Station") == 0);
    CHECK(view.item_state[0] & XIAOTAI_STATE_CHECKED);
    const int32_t same_station = view.item_value[0];
    on_owner(save_peer, "");
    CHECK(!(view.item_state[0] & XIAOTAI_STATE_CHECKED));
    CHECK(view.item_value[0] == same_station); /* A redraw alone does not invalidate an action. */
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, 8);
    CHECK(!fake_saved_peer[0]);
    uint32_t ticket = 0;
    on_owner(freeze, &ticket);
    sync();
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, 0);
    CHECK(!fake_saved_peer[0] && strcmp(view.id, "door.devices") == 0 && strstr(view.toast, "维护中"));
    on_owner(thaw, &ticket);
    sync();
    const int32_t stale_tap = view.item_value[0];
    on_owner(refresh_station, "other-station");
    sync();
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, stale_tap);
    CHECK(!fake_saved_peer[0] && strcmp(view.id, "door.devices") == 0);
    on_owner(refresh_station, "outside");
    sync();
    const int32_t previous_visit = view.item_value[0];
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_DEVICES, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, previous_visit);
    CHECK(!fake_saved_peer[0] && strcmp(view.id, "door.devices") == 0);
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, view.item_value[0]);
    CHECK(strcmp(view.id, "door.home") == 0 && strcmp(fake_saved_peer, "outside") == 0);
    CHECK(strstr(view.toast, "已选择"));
    intent(XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);
    CHECK(strcmp(view.id, "door.monitor") == 0 && strstr(view.text, "暂未接入"));
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(strcmp(foreground, "door") == 0);
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    const bool yes = true, no = false;
    on_owner(set_overlay, (void *)&yes);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(strcmp(foreground, "door") == 0);
    on_owner(set_overlay, (void *)&no);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(strcmp(foreground, "companion") == 0 && companion_entry == XIAOTAI_APP_ENTRY_NEXT);
    intent("fixture.page", 1);
    CHECK(strcmp(foreground, "door") == 0);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, -1);
    CHECK(strcmp(foreground, "door") == 0);
    fake_advance_ms(350);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, -1);
    CHECK(strcmp(foreground, "companion") == 0 && companion_entry == XIAOTAI_APP_ENTRY_PREVIOUS);
    stop();
    start(false);
    char peer[65];
    CHECK(xiaotai_setting_get_text(XIAOTAI_SETTING_APP_DOOR_PEER, peer, sizeof(peer)) == XIAOTAI_OK);
    CHECK(strcmp(peer, "outside") == 0);
    stop();
}

static void receive_call(void *id)
{
    fake_call = (xiaotai_call_status_t){.session = 40, .state = XIAOTAI_CALL_INCOMING};
    (void)snprintf(fake_call.peer_id, sizeof(fake_call.peer_id), "%s", (const char *)id);
    emit(XIAOTAI_EVENT_CALL_INCOMING, 0, &fake_call, sizeof(fake_call));
}

static void end_call(void *ctx)
{
    (void)ctx;
    fake_call.session = 0;
    fake_call.state = XIAOTAI_CALL_IDLE;
    emit(XIAOTAI_EVENT_CALL_ENDED, XIAOTAI_CALL_END_PEER_HANGUP, &fake_call, sizeof(fake_call));
}

static void provision(void *ctx)
{
    const xiaotai_provisioning_phase_t phase = *(const xiaotai_provisioning_phase_t *)ctx;
    fake_provisioning.phase = phase;
    emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, phase, &fake_provisioning, sizeof(fake_provisioning));
}

static void set_provision(xiaotai_provisioning_phase_t phase)
{
    on_owner(provision, &phase);
    sync();
}

static void presenter_test(void)
{
    strcpy(fake_saved_peer, "outside");
    start(true);
    const unsigned initial_stop = stop_ai;
    on_owner(receive_call, "elsewhere");
    sync();
    CHECK(strcmp(foreground, "companion") == 0 && companion_calls == 1);
    CHECK(strcmp(view.id, "door.home") == 0);
    emit("ai_talk.state", 1, NULL, 0);
    sync();
    CHECK(companion_ai == 1 && strcmp(view.id, "door.home") == 0);
    set_provision(XIAOTAI_PROVISIONING_WAITING);
    CHECK(companion_setup == 1 && !door.setup && !strcmp(view.id, "door.home"));
    set_provision(XIAOTAI_PROVISIONING_NONE);
    on_owner(end_call, NULL);
    sync();
    CHECK(strcmp(foreground, "door") == 0);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(strcmp(foreground, "companion") == 0 && companion_entry == XIAOTAI_APP_ENTRY_NEXT);
    const unsigned stop_on_home = stop_ai;
    on_owner(receive_call, "outside");
    sync();
    CHECK(strcmp(foreground, "door") == 0 && strcmp(view.id, "door.call") == 0 && companion_calls == 1);
    CHECK(stop_on_home > initial_stop && stop_ai == stop_on_home);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(strcmp(foreground, "door") == 0);
    on_owner(end_call, NULL);
    sync();
    CHECK(strcmp(foreground, "companion") == 0 && companion_entry == XIAOTAI_APP_ENTRY_RETURN);
    emit(XIAOTAI_EVENT_STREAM_STATE, XIAOTAI_STREAM_LISTENING, NULL, 0);
    const xiaotai_stream_ended_t refused = {.generation = 90, .started = false};
    emit(XIAOTAI_EVENT_STREAM_ENDED, XIAOTAI_STREAM_END_BUSY, &refused, sizeof(refused));
    sync();
    CHECK(strcmp(foreground, "companion") == 0 && companion_entry == XIAOTAI_APP_ENTRY_RETURN);
    emit(XIAOTAI_EVENT_STREAM_STARTED, 91, NULL, 0);
    sync();
    CHECK(strcmp(foreground, "door") == 0 && view.state == XIAOTAI_APP_DOOR_STREAMING);
    const xiaotai_stream_ended_t old = {.generation = 90, .started = true};
    emit(XIAOTAI_EVENT_STREAM_ENDED, 0, &old, sizeof(old));
    sync();
    CHECK(strcmp(foreground, "door") == 0);
    const xiaotai_stream_ended_t actual = {.generation = 91, .started = true};
    const uint32_t remote = 91;
    emit("stream_video.remote", 1, &remote, sizeof(remote));
    sync();
    CHECK(view.flags & XIAOTAI_VIEW_VIDEO);
    emit(XIAOTAI_EVENT_STREAM_ENDED, 0, &actual, sizeof(actual));
    sync();
    CHECK(strcmp(foreground, "companion") == 0 && companion_entry == XIAOTAI_APP_ENTRY_RETURN);
    stop();

    start(false);
    CHECK(strcmp(view.id, "door.home") == 0 && !view.menu && !view.network);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(strcmp(foreground, "door") == 0);
    on_owner(receive_call, "elsewhere");
    sync();
    CHECK(view.kind == XIAOTAI_VIEW_CALL && strcmp(view.id, "door.other_call") == 0);
    on_owner(end_call, NULL);
    sync();
    CHECK(strcmp(view.id, "door.other_result") == 0);
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);
    CHECK(strcmp(view.id, "door.home") == 0);
    set_provision(XIAOTAI_PROVISIONING_WAITING);
    CHECK(view.kind == XIAOTAI_VIEW_PROGRESS && strcmp(view.id, "door.network") == 0);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(strcmp(view.id, "door.network") == 0);
    set_provision(XIAOTAI_PROVISIONING_NONE);
    CHECK(strcmp(view.id, "door.home") == 0);
    stop();
}

static void call_at(void *arg)
{
    fake_call = *(const xiaotai_call_status_t *)arg;
    emit(XIAOTAI_EVENT_CALL_STATE, 0, &fake_call, sizeof(fake_call));
}

static void shown_call(xiaotai_call_status_t *call)
{
    on_owner(call_at, call);
    sync();
}

static void set_binding(void *ctx)
{
    fake_binding = *(const xiaotai_binding_status_t *)ctx;
    emit(XIAOTAI_EVENT_BINDING_CHANGED, fake_binding.phase, &fake_binding, sizeof(fake_binding));
    if (fake_binding.phase == XIAOTAI_BINDING_WAITING && fake_binding.verification[0]) {
        xiaotai_args_t args = {0};
        snprintf(args.text, sizeof(args.text), "%s", fake_binding.verification);
        CHECK(xiaotai_event_emit(XIAOTAI_EVENT_BINDING_CODE, &args, &fake_binding, sizeof(fake_binding)) == XIAOTAI_OK);
    }
}

static void apply_binding(xiaotai_binding_status_t *status)
{
    on_owner(set_binding, status);
    sync();
}

static void audio_pending(void *value)
{
    fake_feedback_pending = *(const bool *)value;
}

static void fallback_test(bool announce)
{
    CHECK(xiaotai_app_door_configure(&(xiaotai_app_door_config_t){.announce_on_code = announce}) == XIAOTAI_OK);
    start(false);
    emit("ai_talk.state", 1, NULL, 0);
    sync();
    CHECK(!strcmp(view.id, "door.assistant"));
    emit("ai_talk.state", 0, NULL, 0);
    sync();
    CHECK(!strcmp(view.id, "door.home"));

    xiaotai_call_status_t call = {.session = 31, .state = XIAOTAI_CALL_INCOMING,
        .peer_id = "other", .peer_name = "Fixture visitor"};
    shown_call(&call);
    CHECK(view.kind == XIAOTAI_VIEW_CALL && !strcmp(view.id, "door.other_call") && !strcmp(view.subtitle, "Fixture visitor"));
    const xiaotai_view_action_t *accept = visible_action(XIAOTAI_INTENT_CALL_ACCEPT);
    const xiaotai_view_action_t *reject = visible_action(XIAOTAI_INTENT_CALL_REJECT);
    CHECK(accept && accept->value == 31 && reject && reject->value == 31);
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN) && fake_ring_in && !fake_ring_out);
    tap(XIAOTAI_INTENT_CALL_ACCEPT);
    CHECK(fake_calls == 1 && fake_control_session == 31 && !strcmp(fake_verb, "accept"));
    call.state = XIAOTAI_CALL_ACTIVE;
    shown_call(&call);
    CHECK(!visible_action(XIAOTAI_INTENT_CALL_ACCEPT) && !visible_action(XIAOTAI_INTENT_CALL_REJECT));
    const xiaotai_view_action_t *hangup = visible_action(XIAOTAI_INTENT_CALL_HANGUP);
    CHECK(hangup && hangup->value == 31 && !fake_ring_in && !fake_ring_out);
    tap(XIAOTAI_INTENT_CALL_HANGUP);
    CHECK(fake_calls == 2 && fake_control_session == 31 && !strcmp(fake_verb, "hangup"));
    on_owner(end_call, NULL);
    sync();
    CHECK(!strcmp(view.id, "door.other_result") && visible_action(XIAOTAI_INTENT_APP_DOOR_BACK));
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);
    CHECK(!strcmp(view.id, "door.home"));

    xiaotai_net_prepare_t prep = {.stage = XIAOTAI_NET_PREPARE_CHECKING, .percent = 26};
    emit(XIAOTAI_EVENT_NET_PREPARE, 0, &prep, sizeof(prep));
    sync();
    CHECK(view.kind == XIAOTAI_VIEW_PROGRESS && !strcmp(view.id, "door.prepare"));
    prep.stage = XIAOTAI_NET_PREPARE_FAILED;
    emit(XIAOTAI_EVENT_NET_PREPARE, 0, &prep, sizeof(prep));
    sync();
    CHECK(view.kind == XIAOTAI_VIEW_NOTICE && !strcmp(view.id, "door.prepare"));
    const struct {xiaotai_net_fault_t fault; bool policy_blocked, retry;} faults[] = {
        {XIAOTAI_NET_FAULT_NO_LINK, false, true},
        {XIAOTAI_NET_FAULT_UNSUPPORTED, false, true},
        {XIAOTAI_NET_FAULT_UNSUPPORTED, true, false},
        {XIAOTAI_NET_FAULT_START, true, false},
    };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        fake_net.state = XIAOTAI_NET_BLOCKED;
        prep.fault = faults[i].fault;
        fake_net.prepare = prep;
        fake_net.prepare.retry_blocked = faults[i].policy_blocked;
        emit(XIAOTAI_EVENT_NET_PREPARE, 0, &prep, sizeof(prep));
        sync();
        const xiaotai_view_action_t *retry = visible_action(XIAOTAI_INTENT_PROVISIONING_RETRY);
        printf("prepare fault=%d policy_blocked=%u retry=%u\n", faults[i].fault,
               (unsigned)faults[i].policy_blocked, (unsigned)(retry != NULL));
        CHECK((retry != NULL) == faults[i].retry);
        if (retry) CHECK(retry->role == XIAOTAI_ROLE_PRIMARY && !strcmp(retry->label, "重新检查"));
    }
    prep.fault = XIAOTAI_NET_FAULT_POWER_CYCLE;
    prep.retry_blocked = true;
    fake_net.prepare = prep;
    emit(XIAOTAI_EVENT_NET_PREPARE, 0, &prep, sizeof(prep));
    sync();
    CHECK(view.kind == XIAOTAI_VIEW_NOTICE && !strcmp(view.id, "door.prepare"));
    CHECK(!strcmp(view.text, "请断电后重新启动") && !visible_action(XIAOTAI_INTENT_PROVISIONING_RETRY));
    puts("power-cycle notice displays the required power-off and offers no retry");
    fake_net = (xiaotai_net_status_t){0};
    prep.stage = XIAOTAI_NET_PREPARE_READY;
    emit(XIAOTAI_EVENT_NET_PREPARE, 0, &prep, sizeof(prep));
    sync();
    CHECK(!strcmp(view.id, "door.home"));

    const bool yes = true, no = false;
    on_owner(audio_pending, (void *)&yes);
    xiaotai_binding_status_t binding = {.phase = XIAOTAI_BINDING_WAITING, .revision = 1, .verification = "012345",
        .generation = 2, .seconds_left = 90};
    apply_binding(&binding);
    CHECK(view.kind == XIAOTAI_VIEW_CODE && !strcmp(view.id, "door.binding") && !strcmp(view.text, "012345"));
    CHECK(view.timer_mode == XIAOTAI_TIMER_REMAINING && !fake_open_announcements);
    static const char replay[] = "door.fallback.replay";
    const xiaotai_view_action_t *action = visible_action(replay);
    CHECK(action && action->value);
    const int32_t first_token = action->value;
    tap(replay);
    CHECK(!fake_open_announcements);
    on_owner(audio_pending, (void *)&no);
    fake_advance_ms(700);
    sync();
    apply_binding(&binding);
    CHECK(fake_open_announcements == (announce ? 1U : 0U));
    ++binding.revision; /* Announcement state and countdown may update the revision without a new code. */
    binding.seconds_left = 70;
    apply_binding(&binding);
    CHECK(fake_open_announcements == (announce ? 1U : 0U));
    tap(replay);
    CHECK(fake_open_announcements == (announce ? 2U : 1U));

    ++binding.revision;
    strcpy(binding.verification, "098765");
    apply_binding(&binding);
    CHECK(!strcmp(view.text, "098765") && visible_action(replay) && visible_action(replay)->value != first_token);
    CHECK(fake_open_announcements == (announce ? 3U : 1U));
    intent(replay, first_token);
    CHECK(fake_open_announcements == (announce ? 3U : 1U));
    binding = (xiaotai_binding_status_t){.phase = XIAOTAI_BINDING_EXPIRED, .revision = 4};
    apply_binding(&binding);
    CHECK(view.kind == XIAOTAI_VIEW_NOTICE && !view.text[0] && !visible_action(replay));
    binding = (xiaotai_binding_status_t){.phase = XIAOTAI_BINDING_BOUND, .revision = 5};
    apply_binding(&binding);
    CHECK(!strcmp(view.id, "door.home"));
    stop();
}

#ifdef XIAOTAI_DOOR_WITH_WECHAT
static void show_wechat(void *arg)
{
    fake_wechat = *(const xiaotai_wechat_status_t *)arg;
    const char *event = fake_wechat.state == XIAOTAI_WECHAT_IDLE ? XIAOTAI_EVENT_WECHAT_ENDED :
                        fake_wechat.state == XIAOTAI_WECHAT_INCOMING ? XIAOTAI_EVENT_WECHAT_INCOMING :
                        XIAOTAI_EVENT_WECHAT_STATE;
    emit(event, fake_wechat.state == XIAOTAI_WECHAT_IDLE ? (int32_t)fake_wechat.end : (int32_t)fake_wechat.session,
         &fake_wechat, sizeof(fake_wechat));
}

static void apply_wechat(xiaotai_wechat_status_t *status)
{
    on_owner(show_wechat, status);
    sync();
}

static void wechat_fallback_test(void)
{
    start(false);
    xiaotai_wechat_status_t wx = {.state = XIAOTAI_WECHAT_INCOMING, .session = 92, .peer_name = "Fixture"};
    apply_wechat(&wx);
    CHECK(view.kind == XIAOTAI_VIEW_CALL && !strcmp(view.id, "door.other_call"));
    CHECK(visible_action(XIAOTAI_INTENT_WECHAT_ACCEPT) && visible_action(XIAOTAI_INTENT_WECHAT_REJECT));
    CHECK(!visible_action(XIAOTAI_INTENT_CALL_ACCEPT) && !visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN));
    tap(XIAOTAI_INTENT_WECHAT_ACCEPT);
    CHECK(fake_wechat_calls == 1 && !fake_calls);
    wx.state = XIAOTAI_WECHAT_ACTIVE;
    apply_wechat(&wx);
    CHECK(!visible_action(XIAOTAI_INTENT_WECHAT_ACCEPT) && visible_action(XIAOTAI_INTENT_WECHAT_HANGUP));
    tap(XIAOTAI_INTENT_WECHAT_HANGUP);
    CHECK(fake_wechat_calls == 2 && !fake_calls);
    wx.state = XIAOTAI_WECHAT_IDLE;
    wx.session = 0;
    wx.end = XIAOTAI_WECHAT_END_PEER_HANGUP;
    apply_wechat(&wx);
    CHECK(!strcmp(view.id, "door.other_result"));
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);
    CHECK(!strcmp(view.id, "door.home"));
    stop();

    start(true);
    wx.state = XIAOTAI_WECHAT_INCOMING;
    wx.session = 93;
    apply_wechat(&wx);
    CHECK(companion_calls == 1 && !strcmp(foreground, "companion"));
    CHECK(!strcmp(view.id, "door.home") && !visible_action(XIAOTAI_INTENT_WECHAT_ACCEPT));
    stop();
}
#endif

static void refuse_maintenance_during_drag(void *unused)
{
    (void)unused;
    uint32_t ticket = 0;
    CHECK(xiaotai_quiesce_begin("test", &ticket) == XIAOTAI_ERR_BUSY && !xiaotai_quiesced());
}

static void queue_volume_change_release(void *ctx)
{
    const int32_t value = *(const int32_t *)ctx;
    const xiaotai_args_t live = {.value = value, .text = XIAOTAI_ARG_LIVE};
    const xiaotai_args_t released = {.value = value};
    CHECK(xiaotai_on_owner());
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, &live) == XIAOTAI_OK);
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, &released) == XIAOTAI_OK);
}

static void volume_change_release(int32_t value)
{
    /* Queue both before the owner can process either. The sample on release precedes redraw. */
    on_owner(queue_volume_change_release, &value);
    sync();
}

static void call_test(void)
{
    strcpy(fake_saved_peer, "outside");
    start(false);
    xiaotai_call_status_t call = {
        .session = UINT32_C(0xf1234567), .peer_id = "outside", .peer_name = "Station", .state = XIAOTAI_CALL_INCOMING,
    };
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.call") && strstr(view.subtitle, "Station") && fake_ring_in && !fake_ring_out);
    CHECK(view.action_count == 2 && view.call.phase == XIAOTAI_APP_DOOR_CALL_RINGING);
    CHECK((uint32_t)visible_action("door.call.reject")->value == call.session);
    intent_text(XIAOTAI_INTENT_APP_DOOR_CALL, (int32_t)(call.session - 1U), "accept");
    CHECK(!fake_calls && strstr(view.toast, "操作未受理"));
    fake_control_error = XIAOTAI_ERR_INVALID_STATE;
    tap("door.call.reject");
    CHECK(fake_calls == 1 && fake_control_session == call.session && !strcmp(fake_verb, "reject") &&
          strstr(view.toast, "操作未受理"));
    fake_control_error = XIAOTAI_OK;
    tap("door.call.accept");
    CHECK(fake_calls == 2 && !strcmp(fake_verb, "accept") && !strcmp(view.status, "正在接听"));
    fake_advance_ms(1499);
    sync();
    CHECK(visible_action("door.call.accept")->state & XIAOTAI_STATE_BUSY);
    fake_advance_ms(1);
    sync();
    sync();
    CHECK(!(visible_action("door.call.accept")->state & XIAOTAI_STATE_BUSY) && strstr(view.toast, "操作未受理"));
    call.peer_id[0] = '\0';
    call.state = XIAOTAI_CALL_CONNECTING;
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.call") && !fake_ring_in && !fake_ring_out && view.action_count == 1);
    CHECK(view.call.phase == XIAOTAI_APP_DOOR_CALL_CONNECTING && visible_action("door.call.hangup"));
    call.state = XIAOTAI_CALL_ACTIVE;
    call.flags = XIAOTAI_CALL_VIDEO;
    call.link = 74;
    call.active_since_us = 1000000;
    shown_call(&call);
    CHECK(view.action_count == 4 && view.call.video && view.timer_mode == XIAOTAI_TIMER_ELAPSED);
    CHECK(!(view.flags & (XIAOTAI_VIEW_VIDEO | XIAOTAI_VIEW_MAY_SLEEP)));
    intent(XIAOTAI_INTENT_APP_DOOR_MUTE, (int32_t)(call.session - 1U));
    CHECK(!fake_mics && !fake_mic_saves);
    tap(XIAOTAI_INTENT_APP_DOOR_MUTE);
    CHECK(fake_mics == 1 && fake_requested_mute && fake_mic_saves == 1 && fake_saved_mute && view.call.muted);

    uint32_t ticket = 0;
    int32_t saved_mute = -1;
    on_owner(freeze, &ticket);
    CHECK(ticket && xiaotai_quiesced() && !strcmp(view.id, "door.call"));
    CHECK(fake_call.session == call.session && fake_call.state == XIAOTAI_CALL_ACTIVE && !fake_call.outgoing);
    const xiaotai_view_action_t *mute = visible_action(XIAOTAI_INTENT_APP_DOOR_MUTE);
    CHECK(mute && (uint32_t)mute->value == call.session);
    view.toast[0] = '\0';
    tap(XIAOTAI_INTENT_APP_DOOR_MUTE);
    CHECK(xiaotai_quiesced() && strstr(view.toast, "维护中"));
    CHECK(fake_mics == 1 && fake_requested_mute && fake_mic_saves == 1 && fake_saved_mute);
    CHECK(fake_call.muted && view.call.muted);
    CHECK(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &saved_mute) == XIAOTAI_OK && saved_mute == 1);
    on_owner(thaw, &ticket);
    CHECK(!xiaotai_quiesced());

    fake_mic_error = XIAOTAI_FAIL;
    tap(XIAOTAI_INTENT_APP_DOOR_MUTE);
    CHECK(fake_mics == 2 && !fake_requested_mute && fake_mic_saves == 1 && fake_saved_mute && view.call.muted);
    CHECK(strstr(view.toast, "操作未受理"));
    fake_mic_error = XIAOTAI_OK;
    tap(XIAOTAI_INTENT_APP_DOOR_MUTE);
    CHECK(fake_mics == 3 && !fake_requested_mute && fake_mic_saves == 2 && !fake_saved_mute && !view.call.muted);

    tap(XIAOTAI_INTENT_APP_DOOR_VOLUME);
    CHECK(!strcmp(view.overlay_id, "door.volume") && view.field.value == 70 && view.field.step == 10);
    CHECK(view.field.kind == XIAOTAI_FIELD_SLIDER && view.field.flags == XIAOTAI_FIELD_LIVE);
    CHECK(!strcmp(view.field.text, "%"));
    fake_preview_error = XIAOTAI_FAIL;
    intent_text(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 90, XIAOTAI_ARG_LIVE);
    CHECK(!fake_previewing && fake_volume == 70 && !fake_volume_saves && view.field.value == 70);
    CHECK(strstr(view.toast, "音量设置未受理"));
    volume_change_release(90);
    CHECK(!fake_previewing && fake_volume == 70 && !fake_volume_saves && view.field.value == 70);
    CHECK(strstr(view.toast, "音量设置未受理"));
    fake_preview_error = XIAOTAI_OK;
    intent_text(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 70, XIAOTAI_ARG_LIVE);
    CHECK(fake_previewing && fake_preview == 70 && !fake_volume_saves);
    intent(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 70);
    CHECK(!fake_previewing && fake_volume == 70 && !fake_volume_saves && view.field.value == 70);
    intent_text(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 83, XIAOTAI_ARG_LIVE);
    CHECK(fake_previewing && fake_preview == 80 && !fake_volume_saves && view.field.value == 80);
    fake_preview_error = XIAOTAI_FAIL;
    intent_text(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 12, XIAOTAI_ARG_LIVE);
    CHECK(fake_preview == 80 && view.field.value == 80 && strstr(view.toast, "音量设置未受理"));
    volume_change_release(12);
    CHECK(!fake_previewing && fake_volume == 80 && fake_volume_saves == 1 && view.field.value == 80);
    fake_preview_error = XIAOTAI_OK;
    intent(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 12);
    CHECK(!fake_previewing && fake_volume == 80 && fake_volume_saves == 1 && view.field.value == 80);
    on_owner(freeze, &ticket);
    intent_text(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 12, XIAOTAI_ARG_LIVE);
    CHECK(!fake_previewing && fake_volume == 80 && fake_volume_saves == 1 && view.field.value == 80);
    CHECK(strstr(view.toast, "维护中"));
    view.toast[0] = '\0';
    intent(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 12);
    CHECK(!fake_previewing && fake_volume == 80 && fake_volume_saves == 1 && view.field.value == 80);
    CHECK(strstr(view.toast, "维护中"));
    on_owner(thaw, &ticket);
    intent_text(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 42, XIAOTAI_ARG_LIVE);
    on_owner(refuse_maintenance_during_drag, NULL);
    CHECK(fake_previewing && fake_volume_saves == 1);
    const unsigned touches = fake_activity;
    shown_call(&call);
    CHECK(fake_activity > touches);
    on_owner(end_call, NULL);
    sync();
    CHECK(!strcmp(view.id, "door.home") && !view.overlay_id[0] && !fake_previewing && fake_volume == 40 &&
          fake_volume_saves == 2 && !fake_ring_in && !fake_ring_out);
    on_owner(freeze, &ticket);
    on_owner(thaw, &ticket);

    call.session += 1U;
    (void)snprintf(call.peer_id, sizeof(call.peer_id), "%s", "outside");
    shown_call(&call);
    fake_volume_step = 7;
    tap(XIAOTAI_INTENT_APP_DOOR_VOLUME);
    CHECK(view.field.step == 7);
    intent_text(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 83, XIAOTAI_ARG_LIVE);
    CHECK(fake_previewing && fake_preview == 84 && fake_volume_saves == 2);
    fake_preview_error = XIAOTAI_FAIL;
    intent_text(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 100, XIAOTAI_ARG_LIVE);
    CHECK(fake_previewing && fake_preview == 84 && fake_volume_saves == 2 && view.field.value == 84);
    fake_preview_error = XIAOTAI_OK;
    volume_change_release(100);
    CHECK(!fake_previewing && fake_volume == 100 && fake_volume_saves == 3 && view.field.value == 100);
    intent(XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, 100);
    CHECK(!fake_previewing && fake_volume == 100 && fake_volume_saves == 3 && view.field.value == 100);
    on_owner(end_call, NULL);
    sync();

    call = (xiaotai_call_status_t){.session = 89, .peer_id = "outside", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.call") && fake_ring_out && !fake_ring_in);
    tap("door.call.hangup");
    CHECK(!strcmp(fake_verb, "hangup") && fake_control_session == 89);
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN));
    call = (xiaotai_call_status_t){.session = 90, .peer_id = "different", .state = XIAOTAI_CALL_INCOMING};
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.other_call") && !fake_ring_out && fake_ring_in);
    stop();
}

static int32_t open_prompt(void)
{
    tap(XIAOTAI_INTENT_APP_DOOR_OPEN);
    CHECK(!strcmp(view.overlay_id, "door.open") && view.overlay_action_count == 2);
    CHECK(!strcmp(view.overlay_actions[1].intent, XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM));
    return view.overlay_actions[1].value;
}

static void confirm_prompt(int32_t value)
{
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, value);
    sync();
    CHECK(!strcmp(view.overlay_id, "door.result"));
}

static void open_test(void)
{
    const xiaotai_app_door_config_t config = {.lock = 1};
    CHECK(xiaotai_app_door_configure(&config) == XIAOTAI_OK);
    start(false);
    confirm_prompt(open_prompt());
    CHECK(opens == 1 && open_result == XIAOTAI_ERR_NOT_FOUND && !fake_lock_calls &&
          strstr(view.overlay_text, "未发送开门指令"));
    fake_advance_ms(XIAOTAI_TOAST_DEFAULT_MS);
    sync();
    sync();
    CHECK(!view.overlay_id[0]);
    on_owner(save_peer, "outside");
    const int32_t changed_prompt = open_prompt();
    on_owner(save_peer, "another");
    on_owner(save_peer, "outside"); // Changing away and back still invalidates the original question.
    confirm_prompt(changed_prompt);
    CHECK(opens == 2 && open_result == XIAOTAI_ERR_INVALID_STATE && !fake_lock_calls &&
          strstr(view.overlay_text, "已变更"));
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    const int32_t dismissed = open_prompt();
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, 0);
    const int32_t newer = open_prompt();
    CHECK(dismissed != newer);
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, dismissed);
    CHECK(opens == 2 && !strcmp(view.overlay_id, "door.open"));
    fake_lock_count = 1;  // Lock index 1 is not available even in the selected build.
    confirm_prompt(newer);
    CHECK(opens == 3 && open_result == XIAOTAI_ERR_NOT_SUPPORTED && !fake_lock_calls &&
          strstr(view.overlay_text, "门锁尚未配置"));
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    fake_lock_count = 2;
    intent(XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);
#ifdef DOOR_TEST_OPEN_LOCK
    confirm_prompt(open_prompt());
    CHECK(opens == 4 && open_result == XIAOTAI_OK && fake_lock_calls == 1 && fake_lock_index == 1);
    CHECK(!strcmp(view.overlay_title, "门已打开"));
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    fake_lock_error = XIAOTAI_ERR_BUSY;
    confirm_prompt(open_prompt());
    CHECK(opens == 5 && open_result == XIAOTAI_ERR_BUSY && fake_lock_calls == 2 &&
          strstr(view.overlay_text, "未发送开门指令"));
#else
    confirm_prompt(open_prompt());
    CHECK(opens == 4 && open_result == XIAOTAI_ERR_NOT_SUPPORTED && !fake_lock_calls);
#endif
    stop();
}

static void unrelated_toast(void *unused)
{
    (void)unused;
    xiaotai_view_toast("unrelated notice", 0);
}

static void stream_test(void)
{
    strcpy(fake_saved_peer, "outside");
    start(false);
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_STREAM_STOP) && (view.flags & XIAOTAI_VIEW_MAY_SLEEP));
    const uint32_t first = 100, next = 200, older = 99;
    fake_stream_video = (xiaotai_stream_video_status_t){.generation = first, .remote_unsupported = true};
    emit(XIAOTAI_EVENT_STREAM_STARTED, (int32_t)first, NULL, 0);
    sync();
    const int32_t saved = visible_action(XIAOTAI_INTENT_APP_DOOR_STREAM_STOP)->value;
    CHECK((uint32_t)saved == first && !(view.flags & XIAOTAI_VIEW_MAY_SLEEP));
    emit(XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, 1, &older, sizeof(older));
    emit(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)older, NULL, 0);
    sync();
    CHECK(!(view.flags & XIAOTAI_VIEW_VIDEO) && !view.format_toasts);
    emit(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)first, NULL, 0);
    emit(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)first, NULL, 0);
    sync();
    CHECK(view.format_toasts == 1);
    emit(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, (int32_t)older, NULL, 0);
    emit(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, (int32_t)first, NULL, 0);
    emit(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)first, NULL, 0);
    sync();
    CHECK(view.toast[0] && view.format_toasts == 1);
    fake_stream_video.remote_unsupported = false;
    emit(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, (int32_t)first, NULL, 0);
    sync();
    CHECK(!view.toast[0] && !(view.flags & XIAOTAI_VIEW_VIDEO));
    fake_stream_video.remote_unsupported = true;
    emit(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)first, NULL, 0);
    sync();
    CHECK(view.format_toasts == 2 && view.toast[0]);
    on_owner(unrelated_toast, NULL);
    fake_stream_video.remote_unsupported = false;
    emit(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, (int32_t)first, NULL, 0);
    emit(XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, 1, &first, sizeof(first));
    sync();
    CHECK(view.flags & XIAOTAI_VIEW_VIDEO);
    CHECK(!strcmp(view.toast, "unrelated notice"));
    emit(XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, 0, &first, sizeof(first));
    sync();
    CHECK(!(view.flags & XIAOTAI_VIEW_VIDEO));
    (void)open_prompt();
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_STREAM_STOP));
    intent(XIAOTAI_INTENT_APP_DOOR_STREAM_STOP, saved);
    CHECK(!fake_stop_generation);
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, 0);
    fake_stream_generation = next;
    fake_stream_video = (xiaotai_stream_video_status_t){.generation = next, .remote_unsupported = true};
    emit(XIAOTAI_EVENT_STREAM_STARTED, (int32_t)next, NULL, 0);
    sync();
    intent(XIAOTAI_INTENT_APP_DOOR_STREAM_STOP, saved);
    CHECK(!fake_stop_generation && !fake_stream_stops);
    tap(XIAOTAI_INTENT_APP_DOOR_STREAM_STOP);
    CHECK(fake_stop_generation == next && fake_stream_stops == 1);
    emit(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)next, NULL, 0);
    sync();
    emit(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, (int32_t)first, NULL, 0);
    sync();
    CHECK(view.format_toasts == 3 && !strcmp(view.toast, DOOR_FORMAT_ERROR));
    const xiaotai_stream_ended_t ended = {.generation = next, .started = true};
    emit(XIAOTAI_EVENT_STREAM_ENDED, 0, &ended, sizeof(ended));
    sync();
    CHECK(view.format_toasts == 3 && !view.toast[0] && !(view.flags & XIAOTAI_VIEW_VIDEO) &&
          (view.flags & XIAOTAI_VIEW_MAY_SLEEP) && !visible_action(XIAOTAI_INTENT_APP_DOOR_STREAM_STOP));
    xiaotai_call_status_t call = {
        .session = 321, .peer_id = "outside", .state = XIAOTAI_CALL_ACTIVE, .link = 311, .flags = XIAOTAI_CALL_VIDEO,
    };
    shown_call(&call);
    xiaotai_video_call_status_t video = {.active = true, .link = 309};
    emit(XIAOTAI_EVENT_VIDEO_CALL_REMOTE, 1, &video, sizeof(video));
    sync();
    CHECK(!(view.flags & XIAOTAI_VIEW_VIDEO));
    video.link = 311;
    video.unsupported = true;
    fake_video_call = video;
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1, &video, sizeof(video));
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1, &video, sizeof(video));
    emit(XIAOTAI_EVENT_VIDEO_CALL_REMOTE, 1, &video, sizeof(video));
    sync();
    CHECK(view.format_toasts == 4 && view.toast[0] && (view.flags & XIAOTAI_VIEW_VIDEO));
    emit(XIAOTAI_EVENT_VIDEO_CALL_REMOTE, 0, &video, sizeof(video));
    sync();
    CHECK(!(view.flags & XIAOTAI_VIEW_VIDEO));
    xiaotai_video_call_status_t recovery = video;
    recovery.link = 309;
    recovery.unsupported = false;
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 0, &recovery, sizeof(recovery));
    recovery.link = video.link;
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 0, &recovery, sizeof(recovery));
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1, &video, sizeof(video));
    sync();
    CHECK(view.toast[0] && view.format_toasts == 4);
    fake_video_call.unsupported = false;
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 0, &recovery, sizeof(recovery));
    sync();
    CHECK(!view.toast[0]);
    fake_video_call.unsupported = true;
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1, &video, sizeof(video));
    sync();
    CHECK(view.format_toasts == 5 && view.toast[0]);
    on_owner(unrelated_toast, NULL);
    fake_video_call.unsupported = false;
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 0, &recovery, sizeof(recovery));
    sync();
    CHECK(!strcmp(view.toast, "unrelated notice"));
    ++call.link;
    shown_call(&call);
    video.link = call.link;
    fake_video_call = video;
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1, &video, sizeof(video));
    emit(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 0, &recovery, sizeof(recovery));
    sync();
    CHECK(view.format_toasts == 6 && !strcmp(view.toast, DOOR_FORMAT_ERROR));
    stop();
}

static void stream_stop_policy_test(void)
{
    const char *const action = XIAOTAI_INTENT_APP_DOOR_STREAM_STOP;
    const int32_t first = 100, next = 200;
    start(false);
    fake_stream_generation = (uint32_t)first;
    emit(XIAOTAI_EVENT_STREAM_STARTED, first, NULL, 0);
    sync();

    strcpy(fake_ui.overlay, "wechat.qr");
    intent(action, first);
    fake_ui.overlay[0] = '\0';
    intent(XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);
    CHECK(!visible_action(action));
    intent(action, first);
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    CHECK(!fake_stream_stop_attempts);

    set_provision(XIAOTAI_PROVISIONING_WAITING);
    CHECK(door.setup && !visible_action(action));
    intent(action, first);
    CHECK(!fake_stream_stop_attempts);
    set_provision(XIAOTAI_PROVISIONING_NONE);
    CHECK(!door.setup && visible_action(action)->value == first);

    fake_stream_stop_error = XIAOTAI_ERR_NO_MEM;
    tap(action);
    CHECK(fake_stream_stop_attempts == 1 && !fake_stream_stops && view.toast[0]);
    CHECK(!(visible_action(action)->state & XIAOTAI_STATE_BUSY));
    fake_stream_stop_error = XIAOTAI_OK;
    tap(action);
    CHECK(fake_stream_stop_attempts == 2 && fake_stream_stops == 1);
    intent(action, first); /* The second tap can already be in the app queue. */
    CHECK(fake_stream_stop_attempts == 2 && fake_stream_stops == 1);
    CHECK(visible_action(action)->state & XIAOTAI_STATE_BUSY);

    fake_advance_ms(1499);
    intent(action, first);
    CHECK(fake_stream_stop_attempts == 2 && (visible_action(action)->state & XIAOTAI_STATE_BUSY));
    fake_advance_ms(1);
    sync();
    sync();
    CHECK(!(visible_action(action)->state & XIAOTAI_STATE_BUSY));
    tap(action);
    CHECK(fake_stream_stop_attempts == 3 && fake_stream_stops == 2);

    fake_advance_ms(1500);
    sync();
    sync();
    CHECK(!(visible_action(action)->state & XIAOTAI_STATE_BUSY));
    fake_stream_generation = (uint32_t)next; /* Owner changes before the UI is notified. */
    intent(action, first);
    CHECK(fake_stream_stop_attempts == 3);

    fake_stream_generation = (uint32_t)first;
    tap(action);
    CHECK(fake_stream_stop_attempts == 4 && fake_stream_stops == 3);
    fake_advance_ms(100);
    fake_stream_generation = (uint32_t)next;
    emit(XIAOTAI_EVENT_STREAM_STARTED, next, NULL, 0);
    sync();
    intent(action, first);
    CHECK(fake_stream_stop_attempts == 4 && visible_action(action)->value == next);
    tap(action);
    CHECK(fake_stream_stop_attempts == 5 && fake_stream_stops == 4);
    CHECK(visible_action(action)->state & XIAOTAI_STATE_BUSY);
    fake_advance_ms(1400); /* Old timer's deadline must not unlock the replacement. */
    sync();
    sync();
    CHECK(visible_action(action)->state & XIAOTAI_STATE_BUSY);
    const xiaotai_stream_ended_t stale = {.generation = (uint32_t)first, .started = true};
    emit(XIAOTAI_EVENT_STREAM_ENDED, 0, &stale, sizeof(stale));
    sync();
    CHECK(visible_action(action)->state & XIAOTAI_STATE_BUSY);
    const xiaotai_stream_ended_t ended = {.generation = (uint32_t)next, .started = true};
    fake_stream_generation = 0;
    emit(XIAOTAI_EVENT_STREAM_ENDED, 0, &ended, sizeof(ended));
    sync();
    CHECK(!visible_action(action));
    stop();
}

typedef struct {
    uint32_t ids[256];
    size_t count;
} test_timer_table_t;

static void unexpected_expiration(void *unused)
{
    (void)unused;
    CHECK(false);
}

static void fill_timers(void *context)
{
    test_timer_table_t *table = context;
    for (; table->count < sizeof(table->ids) / sizeof(table->ids[0]); ++table->count) {
        const xiaotai_err_t err = xiaotai_timer_start(10000, unexpected_expiration, NULL, &table->ids[table->count]);
        if (err == XIAOTAI_ERR_NO_MEM) break;
        CHECK(err == XIAOTAI_OK);
    }
    CHECK(table->count && table->count < sizeof(table->ids) / sizeof(table->ids[0]));
}

static void cancel_test_timers(void *context)
{
    test_timer_table_t *table = context;
    while (table->count) xiaotai_timer_cancel(table->ids[--table->count]);
}

static void stream_stop_full_timers_test(void)
{
    const char *const action = XIAOTAI_INTENT_APP_DOOR_STREAM_STOP;
    test_timer_table_t table = {0};
    start(false);
    fake_stream_generation = 100;
    emit(XIAOTAI_EVENT_STREAM_STARTED, 100, NULL, 0);
    sync();
    on_owner(fill_timers, &table);
    tap(action);
    CHECK(fake_stream_stop_attempts == 1 && fake_stream_stops == 1);
    CHECK(!(visible_action(action)->state & XIAOTAI_STATE_BUSY));
    fake_advance_ms(1499);
    tap(action);
    CHECK(fake_stream_stop_attempts == 1 && fake_stream_stops == 1);
    fake_advance_ms(1);
    tap(action); /* No UI repaint needed to retry at the deadline. */
    CHECK(fake_stream_stop_attempts == 2 && fake_stream_stops == 2);
    on_owner(cancel_test_timers, &table);
    fake_advance_ms(1500);
    tap(action);
    CHECK(fake_stream_stop_attempts == 3 && fake_stream_stops == 3);
    CHECK(visible_action(action)->state & XIAOTAI_STATE_BUSY);
    stop();
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "presenter") == 0) presenter_test();
    else if (argc == 2 && strcmp(argv[1], "call") == 0) call_test();
    else if (argc == 2 && strcmp(argv[1], "stream") == 0) stream_test();
    else if (argc == 2 && strcmp(argv[1], "stream_stop_policy") == 0) stream_stop_policy_test();
    else if (argc == 2 && strcmp(argv[1], "stream_stop_full_timers") == 0) stream_stop_full_timers_test();
    else if (argc == 2 && strcmp(argv[1], "fallback") == 0) fallback_test(false);
    else if (argc == 2 && strcmp(argv[1], "fallback_announce") == 0) fallback_test(true);
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    else if (argc == 1 || (argc == 2 && strcmp(argv[1], "wechat") == 0)) wechat_fallback_test();
#endif
#ifdef DOOR_TEST_OPEN_LOCK
    else if (argc == 1 || strcmp(argv[1], "open") == 0) open_test();
#else
    else if (argc == 2 && strcmp(argv[1], "open") == 0) open_test();
#endif
    else home_test();
    return 0;
}
