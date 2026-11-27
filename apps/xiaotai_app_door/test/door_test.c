// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_c61_ota_image_admission.py
// Replaces legacy: tests/regression/test_native_preferences.py
// Replaces legacy: tests/regression/test_p4_h264_feedback.py
// Replaces legacy: tests/lvgl/test_door_preferences.c::door_preferences
// Replaces legacy: tests/regression/test_wt99_h5_local_stop.py
// Replaces legacy: tests/lvgl/test_door_preferences.c
#include "door_internal.h"
#include "fakes.h"
#include "xiaotai/interaction.h"

#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/modules/video_call.h"
#include "xiaotai/modules/stream_video.h"
#include "xiaotai/port/kv.h"
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
    sync();     /* what it emitted is behind the first one */
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
    if (!action || (action->state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY)))
        fprintf(stderr, "no control %s on %s\n", name, view.id);
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
    XIAOTAI_EVENT_VIDEO_CALL_STATE, XIAOTAI_EVENT_VIDEO_CALL_REMOTE, XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED,
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    XIAOTAI_EVENT_WECHAT_STATE, XIAOTAI_EVENT_WECHAT_CONNECTED, XIAOTAI_EVENT_WECHAT_ENDED,
#endif
    NULL,
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
        "远程查看中", "网络未连接", "设备尚未就绪", "尚未添加室外机", "室外机已移除", "室外机离线", "室外机在线",
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
    fake_dial_error = XIAOTAI_ERR_PLATFORM_OFFLINE;
    intent(XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);
    CHECK(strcmp(view.id, "door.monitor") == 0 && strstr(view.text, "网络未连接"));
    fake_dial_error = XIAOTAI_OK;
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

static uint32_t receive_session = 40;      /* most tests do not care that the number repeats */

static void receive_call(void *id)
{
    fake_call = (xiaotai_call_status_t){.session = receive_session, .state = XIAOTAI_CALL_INCOMING};
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

/* The door station calls a WeChat user from its directory and presents that
 * call itself, on the door call page; other WeChat calls stay with whoever
 * presented them before. */
static void station_wechat_test(void)
{
    strcpy(fake_saved_peer, "outside");
    fake_saved_sender = true;
    start(true);
    CHECK(!strcmp(foreground, "door"));
    xiaotai_app_door_state_t online = XIAOTAI_APP_DOOR_STATION_ONLINE;
    on_owner(change_home, &online);
    sync();
    intent(XIAOTAI_INTENT_APP_DOOR_DIRECTORY, 0);    /* no longer on a doorbell's screen */
    CHECK(!strcmp(view.rows[0], "WeChat"));
    intent(XIAOTAI_INTENT_APP_DOOR_DIAL, view.item_value[0]);
    CHECK(fake_wechat_dials == 1 && !strcmp(fake_wechat_dial_id, "wechat") && !fake_dials);
    xiaotai_wechat_status_t wx = {.state = XIAOTAI_WECHAT_OUTGOING, .session = 77, .outgoing = true,
                                  .flags = XIAOTAI_WECHAT_VIDEO, .peer_name = "Fixture"};
    apply_wechat(&wx);
    CHECK(!strcmp(foreground, "door") && !companion_calls);
    CHECK(!strcmp(view.id, "door.call") && view.kind == XIAOTAI_VIEW_CUSTOM && !strcmp(view.title, "正在呼叫"));
    CHECK(strstr(view.subtitle, "微信视频通话") && visible_action(XIAOTAI_INTENT_APP_DOOR_WECHAT_HANGUP) && fake_ring_out);
    wx.state = XIAOTAI_WECHAT_ACTIVE;
    wx.active_since_us = 1000000;
    apply_wechat(&wx);
    CHECK(!strcmp(view.title, "正在通话") && view.timer_mode == XIAOTAI_TIMER_ELAPSED && !fake_ring_out);
    intent(XIAOTAI_INTENT_APP_DOOR_WECHAT_HANGUP, 76);   /* another session: ignored */
    CHECK(!fake_wechat_hangups);
    tap(XIAOTAI_INTENT_APP_DOOR_WECHAT_HANGUP);
    CHECK(fake_wechat_hangups == 1);
    wx.state = XIAOTAI_WECHAT_IDLE;
    wx.session = 0;
    wx.end = XIAOTAI_WECHAT_END_LOCAL;
    apply_wechat(&wx);
    CHECK(!strcmp(view.id, "door.home") && !strcmp(view.toast, "通话已结束"));

    wx = (xiaotai_wechat_status_t){.state = XIAOTAI_WECHAT_INCOMING, .session = 78, .peer_name = "Other"};
    apply_wechat(&wx);   /* not the station's call: the companion presents it, as before */
    CHECK(companion_calls == 1 && !strcmp(foreground, "companion"));
    stop();
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
    /* Without a lock of its own the home screen offers no "open". */
    if (visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN)) tap(XIAOTAI_INTENT_APP_DOOR_OPEN);
    else intent(XIAOTAI_INTENT_APP_DOOR_OPEN, 0);
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

static void message_at(void *arg)
{
    const xiaotai_call_message_t *m = arg;
    emit(XIAOTAI_EVENT_CALL_MESSAGE, (int32_t)m->session, m, sizeof(*m));
}

static void deliver(const xiaotai_call_message_t *m)
{
    on_owner(message_at, (void *)m);
    sync();
}

/* The door station: the visitor calls, the resident's "open" is shown on the screen. */
static void station_test(void)
{
    strcpy(fake_saved_peer, "outside");
    fake_saved_sender = true;
    const xiaotai_app_door_config_t config = {.open_on_screen = true};
    CHECK(xiaotai_app_door_configure(&config) == XIAOTAI_OK);
    start(false);
    xiaotai_app_door_state_t online = XIAOTAI_APP_DOOR_STATION_ONLINE;
    on_owner(change_home, &online);
    sync();
    CHECK(!strcmp(view.id, "door.home") && !strcmp(view.status, "可以呼叫"));
    /* Nothing a visitor should change stands on the screen. */
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_RING) && !visible_action(XIAOTAI_INTENT_APP_DOOR_DEVICES));
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_DIRECTORY));   /* a doorbell rings its own household */
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_SETTINGS)->trigger == XIAOTAI_TRIGGER_LONG_PRESS);
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_LOG));
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN) && !visible_action(XIAOTAI_INTENT_APP_DOOR_MONITOR));
    intent(XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);   /* the station has no monitor page */
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN, 0);      /* nor opens from its own screen */
    CHECK(!strcmp(view.id, "door.home") && !view.overlay_id[0]);
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    CHECK(fake_dials == 1 && !strcmp(fake_dial_peer, "outside"));

    xiaotai_call_status_t call = {.session = 90, .peer_id = "outside", .peer_name = "Station",
                                  .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.call") && !strcmp(view.title, "正在呼叫") && fake_ring_out);
    CHECK(view.action_count == 1 && visible_action("door.call.hangup"));
    call.state = XIAOTAI_CALL_ACTIVE;
    call.link = 91;
    call.active_since_us = 1000000;
    shown_call(&call);
    CHECK(!fake_ring_out && visible_action(XIAOTAI_INTENT_APP_DOOR_VOLUME) && visible_action("door.call.hangup"));
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN) && !visible_action(XIAOTAI_INTENT_APP_DOOR_MUTE));

    const xiaotai_call_message_t stale = {.session = 89, .type = XIAOTAI_APP_DOOR_MESSAGE_OPEN};
    deliver(&stale);
    CHECK(!fake_messages && !view.overlay_id[0]);
    const xiaotai_call_message_t open = {.session = 90, .type = XIAOTAI_APP_DOOR_MESSAGE_OPEN};
    deliver(&open);
    CHECK(fake_messages == 1 && fake_message_session == 90 && fake_message_type == XIAOTAI_APP_DOOR_MESSAGE_OPEN_RESULT);
    CHECK(fake_message_size == 4 && !fake_message_data[0] && !fake_message_data[3]);   /* XIAOTAI_OK */
    CHECK(!strcmp(view.overlay_id, "door.result") && !strcmp(view.overlay_title, "门已打开，请进"));
    CHECK(strstr(view.overlay_text, "没有接门锁") && opens == 1 && open_result == XIAOTAI_OK);
    CHECK(fake_sounds == 1 && fake_sounds_over == 1);
    fake_advance_ms(5000);
    sync();
    sync();
    CHECK(!view.overlay_id[0] && !strcmp(view.id, "door.call"));

    on_owner(end_call, NULL);
    sync();
    CHECK(!strcmp(view.id, "door.home") && !strcmp(view.toast, "通话已结束"));
    call = (xiaotai_call_status_t){.session = 92, .peer_id = "outside", .state = XIAOTAI_CALL_OUTGOING,
                                   .outgoing = true};
    shown_call(&call);
    on_owner(end_call, NULL);
    sync();
    CHECK(!strcmp(view.id, "door.home") && !strcmp(view.toast, "对方暂未接听"));
    stop();
}

/* The door station's directory: devices (and WeChat users where the product
 * has them); a device it dials is presented as its own call even when it is
 * not the front desk. */
static void directory_test(void)
{
    strcpy(fake_saved_peer, "outside");
    fake_saved_sender = true;
    start(false);
    xiaotai_app_door_state_t online = XIAOTAI_APP_DOOR_STATION_ONLINE;
    on_owner(change_home, &online);
    sync();
    intent(XIAOTAI_INTENT_APP_DOOR_DIRECTORY, 0);    /* no longer on a doorbell's screen */
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_DIRECTORY) && !strcmp(view.title, "通讯录"));
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    CHECK(view.count == 2 && !strcmp(view.rows[0], "WeChat") && !strcmp(view.rows[1], "Station"));
    const int32_t device_row = view.item_value[1];
#else
    CHECK(view.count == 1 && !strcmp(view.rows[0], "Station"));
    const int32_t device_row = view.item_value[0];
#endif
    on_owner(refresh_station, "another");   /* a device that is not the front desk */
    sync();
    intent(XIAOTAI_INTENT_APP_DOOR_DIAL, device_row);   /* the list changed: the old row is stale */
    CHECK(!fake_dials);
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    intent(XIAOTAI_INTENT_APP_DOOR_DIAL, view.item_value[1]);
#else
    intent(XIAOTAI_INTENT_APP_DOOR_DIAL, view.item_value[0]);
#endif
    CHECK(fake_dials == 1 && !strcmp(fake_dial_peer, "another"));
    xiaotai_call_status_t call = {.session = 90, .peer_id = "another", .peer_name = "Meeting room",
                                  .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.call") && !strcmp(view.title, "正在呼叫") && strstr(view.subtitle, "Meeting room"));
    on_owner(end_call, NULL);
    sync();
    CHECK(!strcmp(view.id, "door.home") && !strcmp(view.toast, "对方暂未接听"));
    intent(XIAOTAI_INTENT_APP_DOOR_DIRECTORY, 0);    /* no longer on a doorbell's screen */
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);
    CHECK(!strcmp(view.id, "door.home"));
    stop();
}

/* The indoor station without a lock asks the door station of the door call. */
static void remote_open_test(void)
{
    strcpy(fake_saved_peer, "outside");
    start(false);
    xiaotai_call_status_t call = {.session = 70, .peer_id = "outside", .peer_name = "Station",
                                  .state = XIAOTAI_CALL_ACTIVE, .link = 71, .active_since_us = 1000000};
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.call"));
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, open_prompt());
    CHECK(fake_messages == 1 && fake_message_session == 70 && fake_message_type == XIAOTAI_APP_DOOR_MESSAGE_OPEN);
    CHECK(!fake_message_size && !strcmp(view.overlay_id, "door.open") && !strcmp(view.overlay_title, "正在开门…"));
    CHECK(!opens);
    xiaotai_call_message_t answer = {.session = 69, .type = XIAOTAI_APP_DOOR_MESSAGE_OPEN_RESULT, .size = 4};
    deliver(&answer);   /* another call's answer */
    CHECK(!opens);
    answer.session = 70;
    deliver(&answer);
    CHECK(opens == 1 && open_result == XIAOTAI_OK && !strcmp(view.overlay_title, "门已打开"));
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);

    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, open_prompt());
    fake_advance_ms(4000);
    for (unsigned i = 0; i < 4U; ++i) sync();
    CHECK(opens == 2 && open_result == XIAOTAI_ERR_TIMEOUT && strstr(view.overlay_text, "没有回应"));
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);

    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, open_prompt());
    const uint32_t unsupported = (uint32_t)XIAOTAI_ERR_NOT_SUPPORTED;
    for (unsigned i = 0; i < 4; ++i) answer.data[i] = (uint8_t)(unsupported >> (8U * i));
    deliver(&answer);
    CHECK(opens == 3 && open_result == XIAOTAI_ERR_NOT_SUPPORTED && strstr(view.overlay_text, "没有接门锁"));
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);

    fake_message_error = XIAOTAI_ERR_INVALID_STATE;
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, open_prompt());
    CHECK(opens == 4 && open_result == XIAOTAI_ERR_INVALID_STATE && strstr(view.overlay_text, "没有发出"));
    fake_message_error = XIAOTAI_OK;
    stop();
}

/* ---------------------------------------------------------------- two modes */

static void answer_ask(bool yes)
{
    CHECK(!strcmp(view.overlay_id, XIAOTAI_APP_DOOR_OVERLAY_ASK) && view.overlay_action_count == 2);
    CHECK(view.overlay_actions[0].value == 0 && view.overlay_actions[1].value > 1);
    intent(XIAOTAI_INTENT_APP_DOOR_ASK, yes ? view.overlay_actions[1].value : 0);
}

/* A fresh device asks; one that was used before does not; a change asks again and waits for calls to end. */
/* Sound is enough to stand at the door. */
static void mode_sound_test(void)
{
    fake_saved_chosen = false;
    fake_camera = false;
    start(true);
    CHECK(!fake_saved_chosen && !strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_MODE));
    intent(XIAOTAI_INTENT_APP_DOOR_MODE, 2);
    CHECK(fake_saved_chosen && fake_saved_sender && !strcmp(view.id, "door.home"));
}

static void mode_test(void)
{
    fake_saved_chosen = false;
    start(true);
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_MODE));
    intent(XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_RING, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN, 0);
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_MODE) && !view.overlay_id[0] && !fake_dials);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);   /* it can be paged past */
    CHECK(!strcmp(foreground, "companion"));
    intent("fixture.page", 1);
    intent(XIAOTAI_INTENT_APP_DOOR_MODE, 1);
    CHECK(fake_saved_chosen && !fake_saved_sender && !strcmp(view.id, "door.home"));
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_SETTINGS) && !visible_action(XIAOTAI_INTENT_APP_DOOR_RING));

    tap(XIAOTAI_INTENT_APP_DOOR_SETTINGS);
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_SETTINGS) && view.count == 3 && !strcmp(view.rows[0], "工作模式"));
    intent(XIAOTAI_INTENT_APP_DOOR_MODE, 2);
    answer_ask(false);
    CHECK(!fake_saved_sender && !strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_SETTINGS));
    intent(XIAOTAI_INTENT_APP_DOOR_MODE, 2);
    const int32_t stale = view.overlay_actions[1].value;
    intent(XIAOTAI_INTENT_APP_DOOR_ASK, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_MODE, 2);
    intent(XIAOTAI_INTENT_APP_DOOR_ASK, stale);
    CHECK(!fake_saved_sender && view.overlay_id[0]);
    answer_ask(true);
    CHECK(fake_saved_sender && !strcmp(view.id, "door.home") && visible_action(XIAOTAI_INTENT_APP_DOOR_DEVICES));
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_RING));   /* nobody to call yet */
    stop();

    fake_saved_chosen = false;   /* configured with an earlier firmware: a role, or a counterpart */
    start(true);
    CHECK(!strcmp(view.id, "door.home") && door.sender);
    stop();
    fake_saved_sender = false;
    strcpy(fake_saved_peer, "outside");
    start(true);
    CHECK(!strcmp(view.id, "door.home") && !door.sender);
    tap(XIAOTAI_INTENT_APP_DOOR_SETTINGS);
    on_owner(receive_call, "outside");
    sync();
    CHECK(!strcmp(view.id, "door.call"));
    intent(XIAOTAI_INTENT_APP_DOOR_MODE, 2);
    CHECK(!fake_saved_sender && !view.overlay_id[0] && strstr(view.toast, "不能切换"));
    on_owner(end_call, NULL);
    sync();
    stop();
}

static void background(void *again) { xiaotai_app_door_foreground(again != NULL); }

static void online_contacts(const char *first, bool first_online, const char *second, bool second_online)
{
    fake_net.state = XIAOTAI_NET_UP;
    fake_platform.phase = XIAOTAI_PLATFORM_ONLINE;
    fake_list = (xiaotai_contacts_status_t){.generation = 1, .page_count = second ? 2 : 1};
    fake_contacts[0] = (xiaotai_contact_t){.kind = 1, .online_known = true, .online = first_online};
    (void)snprintf(fake_contacts[0].id, sizeof(fake_contacts[0].id), "%s", first);
    (void)snprintf(fake_contacts[0].name, sizeof(fake_contacts[0].name), "Device %s", first);
    if (!second) return;
    fake_contacts[1] = (xiaotai_contact_t){.kind = 1, .online_known = true, .online = second_online};
    (void)snprintf(fake_contacts[1].id, sizeof(fake_contacts[1].id), "%s", second);
    (void)snprintf(fake_contacts[1].name, sizeof(fake_contacts[1].name), "Device %s", second);
}

/* Counterparts: added in front, switched, removed after a question; no gaps and no repetitions. */
static void peers_test(void)
{
    strcpy(fake_saved_peers[1], "b");   /* a gap, and the same device twice */
    strcpy(fake_saved_peers[2], "b");
    online_contacts("a", true, "b", false);
    start(true);
    CHECK(door.peer_count == 1 && !strcmp(door.peers[0], "b") && door_peer_slot("a") < 0 && door_peer_slot("") < 0);
    intent(XIAOTAI_INTENT_APP_DOOR_DEVICES, 0);
    CHECK(view.count == 2 && !(view.item_state[0] & XIAOTAI_STATE_CHECKED) && (view.item_state[1] & XIAOTAI_STATE_CHECKED));
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, view.item_value[0]);
    CHECK(door.peer_count == 2 && !strcmp(door.peers[0], "a") && !strcmp(door.peers[1], "b"));
    CHECK(!strcmp(fake_saved_peer, "a") && !strcmp(fake_saved_peers[0], "b") && !fake_saved_peers[1][0]);
    CHECK(!strcmp(view.id, "door.home") && visible_action(XIAOTAI_INTENT_APP_DOOR_DEVICES));
    intent(XIAOTAI_INTENT_APP_DOOR_DEVICES, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, view.item_value[1]);   /* switch: the first one changes, nothing is lost */
    CHECK(!strcmp(fake_saved_peer, "b") && !strcmp(fake_saved_peers[0], "a") && door.peer_count == 2);
    tap(XIAOTAI_INTENT_APP_DOOR_SETTINGS);
    CHECK(view.count == 5);
    intent(XIAOTAI_INTENT_APP_DOOR_REMOVE, 0);
    answer_ask(false);
    CHECK(door.peer_count == 2);
    intent(XIAOTAI_INTENT_APP_DOOR_REMOVE, 0);
    answer_ask(true);
    CHECK(door.peer_count == 1 && !strcmp(fake_saved_peer, "a") && !fake_saved_peers[0][0] && view.count == 4);
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    on_owner(save_peer, "x");
    for (unsigned i = 0; i < 3; ++i) (void)snprintf(fake_saved_peers[i], 65, "y%u", i);
    stop();
    start(true);   /* four already */
    CHECK(door.peer_count == 4);
    intent(XIAOTAI_INTENT_APP_DOOR_DEVICES, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, view.item_value[0]);
    CHECK(door.peer_count == 4 && !strcmp(fake_saved_peer, "x") && strstr(view.toast, "已经有 4 台"));
    stop();
}

static void end_with(void *end)
{
    fake_call.session = 0;
    fake_call.state = XIAOTAI_CALL_IDLE;
    fake_call.active_since_us = 0;
    fake_call.end = *(const xiaotai_call_end_t *)end;
    emit(XIAOTAI_EVENT_CALL_ENDED, fake_call.end, &fake_call, sizeof(fake_call));
}

static void call_ends(xiaotai_call_end_t end)
{
    on_owner(end_with, &end);
    sync();
}

static void wait_ms(unsigned ms)
{
    fake_advance_ms(ms);
    for (unsigned i = 0; i < 4U; ++i) sync();
}

static void look_at(uint32_t session, const char *peer)
{
    xiaotai_call_status_t call = {.session = session, .state = XIAOTAI_CALL_OUTGOING, .outgoing = true, .flags = XIAOTAI_CALL_VIDEO};
    (void)snprintf(call.peer_id, sizeof(call.peer_id), "%s", peer);
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.monitor") && visible_action(XIAOTAI_INTENT_APP_DOOR_LOOK_END) && !fake_ring_out);
    call.state = XIAOTAI_CALL_ACTIVE;
    call.link = session + 1;
    call.active_since_us = 1000000;
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.monitor") && view.timer_mode == XIAOTAI_TIMER_ELAPSED);
}

/* The indoor station looks, talks, is rung, switches, and says why a look ended. */
static void look_test(void)
{
    strcpy(fake_saved_peer, "a");
    strcpy(fake_saved_peers[0], "b");
    online_contacts("a", true, "b", true);
    start(true);
    tap(XIAOTAI_INTENT_APP_DOOR_MONITOR);
    CHECK(fake_dials == 1 && !strcmp(fake_dial_peer, "a") && fake_mutes == 1 && fake_call_muted);
    intent(XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);   /* already on its way */
    CHECK(fake_dials == 1);
    look_at(90, "a");
    CHECK(fake_call_muted && fake_messages == 1 && fake_message_type == XIAOTAI_APP_DOOR_MESSAGE_LOOK);
    CHECK(fake_message_size == 1 && !fake_message_data[0]);
    CHECK(strstr(view.text, "正在等待门口画面"));
    wait_ms(8000);      /* none came; it may still */
    CHECK(strstr(view.text, "没有画面"));
    xiaotai_video_call_status_t video = {.active = true, .link = 91, .camera = true, .camera_enabled = true};
    emit(XIAOTAI_EVENT_VIDEO_CALL_STATE, 1, &video, sizeof(video));
    sync();
    CHECK(fake_camera_switches == 1);   /* the door is shown, not the room */
    emit(XIAOTAI_EVENT_VIDEO_CALL_REMOTE, 1, &video, sizeof(video));
    sync();
    CHECK(view.flags & XIAOTAI_VIEW_VIDEO);

    tap(XIAOTAI_INTENT_APP_DOOR_LOOK_TALK);
    CHECK(!fake_call_muted && fake_messages == 2 && fake_message_data[0] == 1);
    wait_ms(XIAOTAI_APP_DOOR_LOOK_MS + 1000);   /* talking is not cut off */
    CHECK(!fake_calls);
    tap(XIAOTAI_INTENT_APP_DOOR_LOOK_TALK);
    CHECK(fake_call_muted && fake_messages == 3 && !fake_message_data[0]);

    /* The visitor rings meanwhile; a message of another call counts for nothing. */
    xiaotai_call_message_t ring = {.session = 89, .type = XIAOTAI_APP_DOOR_MESSAGE_RING, .size = 1, .data = {1}};
    deliver(&ring);
    CHECK(!fake_ring_in && !strcmp(view.title, "查看门口"));
    ring.session = 90;
    deliver(&ring);
    CHECK(fake_ring_in && !strcmp(view.title, "门口有人按铃") && visible_action(XIAOTAI_INTENT_APP_DOOR_LOOK_IGNORE));
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_LOOK_NEXT));
    tap(XIAOTAI_INTENT_APP_DOOR_LOOK_IGNORE);
    CHECK(!fake_ring_in && fake_messages == 4 && !fake_message_data[0]);
    deliver(&ring);
    ring.data[0] = 0;                            /* gave up */
    deliver(&ring);
    CHECK(!fake_ring_in && door.missed == 1);
    ring.data[0] = 1;
    deliver(&ring);
    tap(XIAOTAI_INTENT_APP_DOOR_LOOK_TALK);      /* answered */
    CHECK(!fake_ring_in && !fake_call_muted && fake_message_data[0] == 1);

    /* Opening goes to the station that is looked at. */
    const unsigned before = fake_messages;
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, open_prompt());
    CHECK(fake_messages == before + 1 && fake_message_type == XIAOTAI_APP_DOOR_MESSAGE_OPEN);
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, 0);

    /* The next station: hung up here, dialled after the release. */
    tap(XIAOTAI_INTENT_APP_DOOR_LOOK_NEXT);
    CHECK(!strcmp(fake_verb, "hangup") && fake_dials == 1);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    CHECK(!strcmp(view.id, "door.monitor") && fake_dials == 1);
    fake_dial_error = XIAOTAI_ERR_BUSY;          /* the old link is still being closed */
    wait_ms(400);
    CHECK(fake_dials == 2 && !strcmp(view.id, "door.monitor"));
    fake_dial_error = XIAOTAI_OK;
    fake_dial_session = 95;
    wait_ms(400);
    CHECK(fake_dials == 3 && !strcmp(fake_dial_peer, "b"));
    look_at(95, "b");

    /* A long look ends by itself and can be begun again. */
    wait_ms(XIAOTAI_APP_DOOR_LOOK_MS);
    CHECK(!strcmp(fake_verb, "hangup") && fake_control_session == 95);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    CHECK(!strcmp(view.id, "door.monitor") && strstr(view.text, "自动结束"));
    fake_dial_session = 100;
    tap(XIAOTAI_INTENT_APP_DOOR_MONITOR);
    CHECK(!strcmp(fake_dial_peer, "b"));
    static const struct { xiaotai_call_end_t end; const char *text; } ends[] = {
        {XIAOTAI_CALL_END_BUSY, "正忙"}, {XIAOTAI_CALL_END_PEER_REJECT, "正忙"}, {XIAOTAI_CALL_END_NO_ANSWER, "没有应答"},
        {XIAOTAI_CALL_END_NETWORK, "网络中断"}, {XIAOTAI_CALL_END_FAILED, "连不上"},
        {XIAOTAI_CALL_END_PEER_HANGUP, "结束了这次查看"}, {XIAOTAI_CALL_END_PREEMPTED, "打断"},
    };
    for (unsigned i = 0; i < sizeof(ends) / sizeof(*ends); ++i) {
        look_at(100 + i, "b");
        wait_ms(2000);                            /* long enough for somebody to have declined */
        call_ends(ends[i].end);
        CHECK(!strcmp(view.id, "door.monitor") && strstr(view.text, ends[i].text) && !strcmp(foreground, "door"));
        fake_dial_session = 101 + i;
        tap(XIAOTAI_INTENT_APP_DOOR_MONITOR);
    }
    look_at(107, "b");
    call_ends(XIAOTAI_CALL_END_PEER_REJECT);      /* at once, by somebody or because it is busy: not called again */
    const unsigned declined = fake_dials;
    wait_ms(1000);
    CHECK(fake_dials == declined && strstr(view.text, "正忙"));
    fake_dial_session = 107;
    tap(XIAOTAI_INTENT_APP_DOOR_MONITOR);
    look_at(107, "b");
    fake_call.error = XIAOTAI_ERR_NOT_SUPPORTED;
    call_ends(XIAOTAI_CALL_END_PEER_REJECT);      /* a device without video */
    fake_call.error = XIAOTAI_OK;
    const unsigned refused = fake_dials;
    fake_dial_session = 108;
    wait_ms(400);
    CHECK(fake_dials == refused + 1 && !strcmp(view.id, "door.monitor"));
    look_at(108, "b");
    tap(XIAOTAI_INTENT_APP_DOOR_LOOK_END);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    /* Looked again right after one that was given up: waits until the answer can be let in. */
    fake_answer_hold = 5000;
    fake_dial_session = 109;
    const unsigned held = fake_dials;
    tap(XIAOTAI_INTENT_APP_DOOR_MONITOR);
    CHECK(fake_dials == held && !strcmp(view.id, "door.monitor") && visible_action(XIAOTAI_INTENT_APP_DOOR_LOOK_END));
    fake_answer_hold = 0;
    wait_ms(5000);
    CHECK(fake_dials == held + 1);
    look_at(109, "b");
    tap(XIAOTAI_INTENT_APP_DOOR_LOOK_END);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    CHECK(!strcmp(view.id, "door.home"));

    /* A door call that nobody answers is remembered until the list is opened. */
    door.missed = 0;
    on_owner(receive_call, "a");
    sync();
    CHECK(!strcmp(view.id, "door.call") && fake_ring_in);
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    CHECK(!strcmp(view.id, "door.home") && door.missed == 1);
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_LOG)->state & XIAOTAI_STATE_CHECKED);
    tap(XIAOTAI_INTENT_APP_DOOR_LOG);
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_LOG) && view.count >= 3 && !strcmp(view.rows[0], "Device a") && !door.missed);
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);
    CHECK(!strcmp(view.id, "door.home"));
    stop();
}

static void looked_show_companion(void *unused) { (void)unused; CHECK(xiaotai_app_switch("companion") == XIAOTAI_OK); }
static void looked_show_door(void *unused) { (void)unused; CHECK(xiaotai_app_switch("door") == XIAOTAI_OK); }
/* What the framework announces when an activity begins ("" when none runs). */
static void looked_activity(void *name)
{
    xiaotai_args_t args = {0};
    (void)snprintf(args.text, sizeof(args.text), "%s", (const char *)name);
    CHECK(xiaotai_event_emit(XIAOTAI_EVENT_ACTIVITY, &args, NULL, 0) == XIAOTAI_OK);
}

/* The door station is looked at by its own indoor stations only. */
static void looked_test(void)
{
    fake_saved_sender = true;
    strcpy(fake_saved_peer, "a");
    online_contacts("a", true, "stranger", true);
    start(true);
    const unsigned companion = companion_calls;
    /* On its home page the door station lets any device of the contacts look, chosen or not. */
    on_owner(receive_call, "stranger");
    sync();
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "accept") && !fake_ring_in && !strcmp(foreground, "door"));
    CHECK(companion_calls == companion);
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    fake_calls = 0;
    /* Off it only the chosen one does: the stranger rings, on the door's own page (a door station shows
     * no face for a call); nobody sees the door. */
    intent(XIAOTAI_INTENT_APP_DOOR_SETTINGS, 0);
    CHECK(!strcmp(view.id, "door.settings"));
    on_owner(receive_call, "stranger");
    sync();
    CHECK(!fake_calls && companion_calls == companion && !strcmp(foreground, "door"));
    CHECK(!strcmp(view.id, "door.call") && fake_ring_in && visible_action("door.call.accept"));
    /* The caller by a readable name, never by its raw id (a call carries the id where there is no remark). */
    CHECK(strstr(view.subtitle, "设备 nger") && !strstr(view.subtitle, "stranger"));
    /* How a call that came in ended: nobody at the door placed it. */
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    CHECK(!strcmp(view.toast, "对方已取消呼叫"));
    intent(XIAOTAI_INTENT_APP_DOOR_SETTINGS, 0);
    on_owner(receive_call, "stranger");
    sync();
    CHECK(!fake_calls && !strcmp(view.id, "door.call"));
    call_ends(XIAOTAI_CALL_END_LOCAL);
    CHECK(!strcmp(view.toast, "已拒接"));
    intent(XIAOTAI_INTENT_APP_DOOR_SETTINGS, 0);
    on_owner(receive_call, "stranger");
    sync();
    CHECK(!fake_calls && !strcmp(view.id, "door.call"));
    call_ends(XIAOTAI_CALL_END_MISSED);
    CHECK(!strcmp(view.toast, "来电未接听"));
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 0);
    /* Beside another application resting on its home page (a companion's face) it is there to be
     * looked at as on its own: the door comes forward, answered, and gives the screen back after,
     * with the word about the look said before it does. */
    on_owner(looked_show_companion, NULL);
    sync();
    fake_ui.ready = true;
    fake_ui.page_kind = XIAOTAI_VIEW_HOME;
    memset(view.toast, 0, sizeof(view.toast));
    on_owner(receive_call, "stranger");
    sync();
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "accept") && !fake_ring_in && !strcmp(foreground, "door"));
    CHECK(companion_calls == companion);
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    CHECK(!strcmp(foreground, "companion") && !strcmp(view.toast, "查看结束"));
    fake_calls = 0;
    /* A conversation with the assistant is drawn on that same home page and is over when the call is
     * offered (the call's activity ended it): the activities tell. The stranger rings on the door's page. */
    on_owner(looked_activity, "ai_talk");
    on_owner(looked_activity, "call");
    on_owner(receive_call, "stranger");
    sync();
    CHECK(!fake_calls && companion_calls == companion && !strcmp(foreground, "door") && !strcmp(view.id, "door.call"));
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    on_owner(looked_activity, "");
    /* A conversation that ended by itself left the device at rest. */
    on_owner(looked_show_companion, NULL);
    sync();
    on_owner(looked_activity, "ai_talk");
    on_owner(looked_activity, "");
    on_owner(looked_activity, "call");
    on_owner(receive_call, "stranger");
    sync();
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "accept") && !strcmp(foreground, "door"));
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    on_owner(looked_activity, "");
    fake_calls = 0;
    /* That application on another page of its own: the stranger rings on the door's page. */
    on_owner(looked_show_companion, NULL);
    sync();
    fake_ui.page_kind = XIAOTAI_VIEW_TALK;
    on_owner(receive_call, "stranger");
    sync();
    CHECK(!fake_calls && companion_calls == companion && !strcmp(foreground, "door") && !strcmp(view.id, "door.call"));
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    fake_ui = (xiaotai_ui_info_t){.awake = true};
    on_owner(looked_show_door, NULL);
    sync();
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 0);
    fake_list.page_count = 0;                     /* chosen, but no longer a contact */
    on_owner(receive_call, "a");
    sync();
    CHECK(!fake_calls && !strcmp(view.id, "door.call") && fake_ring_in && visible_action("door.call.accept"));
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    fake_list.page_count = 2;
    fake_camera = false;                          /* without a camera it is the sound that is asked for */
    on_owner(receive_call, "a");
    sync();
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "accept") && !fake_ring_in && !strcmp(foreground, "door"));
    fake_camera = true;
    xiaotai_call_status_t call = {.session = 40, .state = XIAOTAI_CALL_CONNECTING, .peer_id = "a", .peer_name = "Desk"};
    shown_call(&call);
    CHECK(fake_calls == 1 && !view.action_count);
    call.state = XIAOTAI_CALL_ACTIVE;
    call.link = 41;
    call.active_since_us = 1000000;
    shown_call(&call);
    /* The screen says so, and who; a visitor cannot end it and cannot hear the room. */
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_LOOKED) && !strcmp(view.title, "正在被查看") && strstr(view.subtitle, "Desk"));
    /* A device without a remark calls with its id as the name: the screen shows a short label instead. */
    (void)snprintf(call.peer_name, sizeof(call.peer_name), "%s", call.peer_id);
    shown_call(&call);
    CHECK(strstr(view.subtitle, "设备 a") && !strstr(view.subtitle, "Desk"));
    (void)snprintf(call.peer_name, sizeof(call.peer_name), "Desk");
    shown_call(&call);
    CHECK(strstr(view.subtitle, "Desk"));
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_RING) && !visible_action("door.call.hangup"));
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN));

    tap(XIAOTAI_INTENT_APP_DOOR_RING);            /* the visitor calls: over the call that is there */
    CHECK(!fake_dials && fake_messages == 1 && fake_message_type == XIAOTAI_APP_DOOR_MESSAGE_RING && fake_message_data[0] == 1);
    CHECK(fake_ring_out && !fake_ring_in);
    wait_ms(XIAOTAI_APP_DOOR_RING_MS);
    CHECK(fake_messages == 2 && !fake_message_data[0] && !fake_ring_out && strstr(view.toast, "无人应答"));
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    xiaotai_call_message_t look = {.session = 40, .type = XIAOTAI_APP_DOOR_MESSAGE_LOOK, .size = 1};
    deliver(&look);
    CHECK(!fake_ring_out && strstr(view.toast, "不方便"));
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    look.data[0] = 1;
    deliver(&look);
    CHECK(!fake_ring_out && !strcmp(view.title, "正在通话") && visible_action("door.call.hangup"));
    look.data[0] = 0;
    deliver(&look);
    CHECK(!strcmp(view.title, "正在被查看") && !visible_action("door.call.hangup"));
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    CHECK(!strcmp(view.toast, "查看结束"));
    CHECK(!strcmp(foreground, "door") && !strcmp(view.id, "door.home"));

    /* The link breaks while the visitor waits: the call goes on as an ordinary one. */
    on_owner(receive_call, "a");
    sync();
    call.session = 40;
    shown_call(&call);
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 0);
    call_ends(XIAOTAI_CALL_END_NETWORK);
    stop();

    /* Choosing none: on the home page a device of the contacts still looks; off it nobody does. */
    fake_saved_peer[0] = '\0';
    fake_calls = 0;
    start(true);
    on_owner(receive_call, "a");
    sync();
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "accept"));
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    fake_calls = 0;
    intent(XIAOTAI_INTENT_APP_DOOR_SETTINGS, 0);
    on_owner(receive_call, "a");
    sync();
    CHECK(!fake_calls);
    stop();
}

/* The doorbell's indoor station (可视门铃): chosen from the four places; its home looks at the door and
 * nothing more; a look and a call have no opening and no switching; no visits are kept. */
static void bell_indoor_test(void)
{
    fake_saved_chosen = false;
    start(true);
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_MODE) && view.count == 4 && !strcmp(view.rows[0], "可视门铃"));
    intent(XIAOTAI_INTENT_APP_DOOR_MODE, XIAOTAI_APP_DOOR_BELL_INDOOR + 1);
    CHECK(fake_saved_chosen && fake_saved_sender == XIAOTAI_APP_DOOR_BELL_INDOOR && !strcmp(view.id, "door.home"));
    CHECK(!strcmp(view.toast, "接下来请添加门铃") && visible_action(XIAOTAI_INTENT_APP_DOOR_DEVICES));
    stop();

    strcpy(fake_saved_peer, "a");
    strcpy(fake_saved_peers[0], "b");
    fake_saved_sender = XIAOTAI_APP_DOOR_BELL_INDOOR;
    online_contacts("a", true, "b", true);
    start(true);
    CHECK(!strcmp(view.id, "door.home") && !strcmp(view.status, "门铃在线"));
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_MONITOR) && visible_action(XIAOTAI_INTENT_APP_DOOR_SETTINGS));
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_LOG) && !visible_action(XIAOTAI_INTENT_APP_DOOR_DEVICES));
    tap(XIAOTAI_INTENT_APP_DOOR_SETTINGS);
    CHECK(!strcmp(view.rows[0], "工作模式") && !strcmp(view.rows[1], "门铃"));
    for (unsigned i = 0; i < view.count; ++i) CHECK(strcmp(view.rows[i], "来访记录"));
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);
    tap(XIAOTAI_INTENT_APP_DOOR_MONITOR);
    look_at(90, "a");
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_LOOK_TALK) && !visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN));
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_LOOK_NEXT));      /* two door stations, no switching */
    call_ends(XIAOTAI_CALL_END_LOCAL);
    /* A ring from another page: answered as any call, without opening. */
    tap(XIAOTAI_INTENT_APP_DOOR_SETTINGS);
    xiaotai_call_status_t call = {.session = 60, .state = XIAOTAI_CALL_INCOMING, .peer_id = "a"};
    shown_call(&call);
    CHECK(!strcmp(view.id, "door.call") && fake_ring_in);
    call.state = XIAOTAI_CALL_ACTIVE;
    call.active_since_us = 1000000;
    shown_call(&call);
    CHECK(visible_action("door.call.hangup") && !visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN));
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    CHECK(door.missed == 0);
    /* One doorbell: choosing another takes its place, whatever was stored before. */
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_DEVICES, 0);
    CHECK(!strcmp(view.id, "door.devices") && view.count == 2 && door.peer_count == 2);
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, view.item_value[1]);
    CHECK(door.peer_count == 1 && !strcmp(door.peers[0], "b") && !strcmp(fake_saved_peer, "b") && !fake_saved_peers[0][0]);
    CHECK(!strcmp(view.id, "door.home") && !strcmp(view.toast, "已换成这台门铃"));
    intent(XIAOTAI_INTENT_APP_DOOR_DEVICES, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, view.item_value[1]);     /* the same one again: nothing changes */
    CHECK(door.peer_count == 1 && !strcmp(door.peers[0], "b") && !strcmp(view.toast, "已选择这台门铃"));
    stop();
}

/* Every mode a page (mode_pages). From the face page back: the doorbell's indoor station, the doorbell, the
 * access system's indoor station, the access machine, and out again. Each page has its own counterparts; the
 * page that stays, or is used, is the one the device answers in from elsewhere; paging through changes nothing;
 * a choice from outside moves the page; all of it is there after a restart. */
static bool titled(const char *what) { return strstr(view.title, what) != NULL; }
static void choose_access(void *unused)
{
    (void)unused;
    CHECK(xiaotai_setting_set_int(XIAOTAI_SETTING_APP_DOOR_SENDER, XIAOTAI_APP_DOOR_ACCESS) == XIAOTAI_OK);
}

static void pages_test(void)
{
    const xiaotai_app_door_config_t config = {.mode_pages = true};
    strcpy(fake_saved_peer, "a");                       /* from before: the indoor station's door station */
    fake_saved_sender = XIAOTAI_APP_DOOR_INDOOR;
    fake_saved_chosen = false;                          /* never asked: the pages are the choice */
    online_contacts("a", true, "b", true);
    CHECK(xiaotai_app_door_configure(&config) == XIAOTAI_OK);
    start(true);
    xiaotai_app_door_status_t status;
    xiaotai_app_door_status(&status);
    CHECK(!strcmp(view.id, "door.home") && titled("门禁室内机") && status.mode == XIAOTAI_APP_DOOR_INDOOR &&
          status.peers == 1);
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_HOME));

    /* Back: the access machine with a list of its own, then out to the face page. Nothing settled. */
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, -1);
    xiaotai_app_door_status(&status);
    CHECK(!strcmp(foreground, "door") && titled("门禁机") && status.mode == XIAOTAI_APP_DOOR_ACCESS && !status.peers);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, -1);           /* the same gesture */
    CHECK(!strcmp(foreground, "door") && titled("门禁机"));
    fake_advance_ms(XIAOTAI_APP_DOOR_PAGER_GUARD_MS);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, -1);
    xiaotai_app_door_status(&status);
    CHECK(!strcmp(foreground, "companion") && status.mode == XIAOTAI_APP_DOOR_INDOOR &&
          fake_saved_sender == XIAOTAI_APP_DOOR_INDOOR);

    /* From the face page back again: the doorbell's indoor station. It stays, and is the mode. */
    intent("fixture.page", -1);
    xiaotai_app_door_status(&status);
    CHECK(!strcmp(foreground, "door") && titled("可视门铃") && status.mode == XIAOTAI_APP_DOOR_BELL_INDOOR);
    CHECK(!status.peers && visible_action(XIAOTAI_INTENT_APP_DOOR_DEVICES));
    fake_advance_ms(XIAOTAI_APP_DOOR_MODE_SETTLE_MS - 1);
    sync();
    CHECK(fake_saved_sender == XIAOTAI_APP_DOOR_INDOOR);
    fake_advance_ms(1);
    sync();
    sync();
    CHECK(fake_saved_sender == XIAOTAI_APP_DOOR_BELL_INDOOR && fake_saved_chosen);
    /* Its doorbell "b"; the settings of before stay as they were. No mode row. */
    tap(XIAOTAI_INTENT_APP_DOOR_DEVICES);
    CHECK(!strcmp(view.id, "door.devices"));
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, view.item_value[1]);
    xiaotai_app_door_status(&status);
    CHECK(!strcmp(view.id, "door.home") && status.peers == 1 && !strcmp(fake_saved_peer, "a"));
    tap(XIAOTAI_INTENT_APP_DOOR_SETTINGS);
    CHECK(!strcmp(view.id, "door.settings") && !strcmp(view.rows[0], "门铃"));
    for (unsigned i = 0; i < view.count; ++i) CHECK(strcmp(view.rows[i], "工作模式"));
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);

    /* On to the doorbell: used at once, it is the mode at once. Its indoor station is "a". */
    fake_advance_ms(XIAOTAI_APP_DOOR_PAGER_GUARD_MS);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, -1);
    CHECK(titled("门铃室外机") && fake_saved_sender == XIAOTAI_APP_DOOR_BELL_INDOOR);
    CHECK(!visible_action(XIAOTAI_INTENT_APP_DOOR_DIRECTORY));
    tap(XIAOTAI_INTENT_APP_DOOR_DEVICES);
    CHECK(!strcmp(view.id, "door.devices") && fake_saved_sender == XIAOTAI_APP_DOOR_BELL);
    intent(XIAOTAI_INTENT_APP_DOOR_SELECT, view.item_value[0]);
    xiaotai_app_door_status(&status);
    CHECK(!strcmp(view.id, "door.home") && status.peers == 1 && status.mode == XIAOTAI_APP_DOOR_BELL);

    /* Forward to the doorbell's indoor station, it settles, out to the face page: "b" rings there, "a" does not. */
    fake_advance_ms(XIAOTAI_APP_DOOR_PAGER_GUARD_MS);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(titled("可视门铃"));
    fake_advance_ms(XIAOTAI_APP_DOOR_MODE_SETTLE_MS);
    sync();
    sync();
    CHECK(fake_saved_sender == XIAOTAI_APP_DOOR_BELL_INDOOR);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(!strcmp(foreground, "companion"));
    on_owner(receive_call, "b");
    sync();
    CHECK(!strcmp(foreground, "door") && !strcmp(view.id, "door.call") && companion_calls == 0);
    on_owner(end_call, NULL);
    sync();
    CHECK(!strcmp(foreground, "companion"));
    on_owner(receive_call, "a");
    sync();
    CHECK(!strcmp(foreground, "companion") && companion_calls == 1);
    on_owner(end_call, NULL);
    sync();

    /* Paged through on the other side (the access machine, then out): still the doorbell's indoor station. */
    intent("fixture.page", 1);
    CHECK(!strcmp(foreground, "door") && titled("门禁机"));
    fake_advance_ms(XIAOTAI_APP_DOOR_PAGER_GUARD_MS);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, -1);
    xiaotai_app_door_status(&status);
    CHECK(!strcmp(foreground, "companion") && status.mode == XIAOTAI_APP_DOOR_BELL_INDOOR &&
          fake_saved_sender == XIAOTAI_APP_DOOR_BELL_INDOOR);
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    stop();

    /* After a restart: the stored mode's page; every page with its own. */
    CHECK(xiaotai_app_door_configure(&config) == XIAOTAI_OK);
    start(true);
    xiaotai_app_door_status(&status);
    CHECK(titled("可视门铃") && status.mode == XIAOTAI_APP_DOOR_BELL_INDOOR && status.peers == 1);
    const struct { const char *title; uint8_t peers; } expected[] = {{"门铃室外机", 1}, {"门禁室内机", 1}, {"门禁机", 0}};
    for (size_t i = 0; i < 3; ++i) {
        fake_advance_ms(XIAOTAI_APP_DOOR_PAGER_GUARD_MS);
        intent(XIAOTAI_INTENT_APP_DOOR_HOME, -1);
        xiaotai_app_door_status(&status);
        CHECK(titled(expected[i].title) && status.peers == expected[i].peers);
    }
    /* A choice from outside (AT+DOOR ACCESS) moves the page. */
    fake_advance_ms(XIAOTAI_APP_DOOR_PAGER_GUARD_MS);
    intent(XIAOTAI_INTENT_APP_DOOR_HOME, 1);
    CHECK(titled("门禁室内机"));
    on_owner(choose_access, NULL);
    xiaotai_app_door_status(&status);
    CHECK(titled("门禁机") && status.mode == XIAOTAI_APP_DOOR_ACCESS && fake_saved_sender == XIAOTAI_APP_DOOR_ACCESS);
    fake_advance_ms(XIAOTAI_APP_DOOR_MODE_SETTLE_MS);
    sync();
    CHECK(fake_saved_sender == XIAOTAI_APP_DOOR_ACCESS);
    stop();
}

static void show_companion(void *unused) { (void)unused; CHECK(xiaotai_app_switch("companion") == XIAOTAI_OK); }
static void show_door(void *unused) { (void)unused; CHECK(xiaotai_app_switch("door") == XIAOTAI_OK); }

/* On the door's home page a door station's call shows the door at once, muted, and is answered with
 * "接听"; "忽略" hangs up. Elsewhere, and without a picture, it rings as any call does. */
static void take_test(void)
{
    strcpy(fake_saved_peer, "a");
    online_contacts("a", true, NULL, false);
    start(true);
    CHECK(!strcmp(view.id, "door.home"));
    xiaotai_call_status_t call = {.session = 50, .state = XIAOTAI_CALL_INCOMING, .peer_id = "a", .peer_name = "Front",
                                  .flags = XIAOTAI_CALL_VIDEO};
    shown_call(&call);
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "accept") && fake_call_muted);
    CHECK(!strcmp(view.id, "door.monitor") && !strcmp(view.title, "门口有人按铃") && fake_ring_in);
    call.state = XIAOTAI_CALL_ACTIVE;
    call.link = 51;
    call.active_since_us = 1000000;
    shown_call(&call);
    /* The door station learns that it is looked at, not answered. */
    CHECK(fake_messages == 1 && fake_message_type == XIAOTAI_APP_DOOR_MESSAGE_LOOK && !fake_message_data[0]);
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_LOOK_TALK) && visible_action(XIAOTAI_INTENT_APP_DOOR_LOOK_IGNORE) &&
          visible_action(XIAOTAI_INTENT_APP_DOOR_OPEN) && fake_ring_in);
    wait_ms(XIAOTAI_APP_DOOR_LOOK_MS + 1000);    /* nobody is cut off while the visitor waits */
    CHECK(fake_calls == 1);
    tap(XIAOTAI_INTENT_APP_DOOR_LOOK_TALK);
    CHECK(!fake_call_muted && fake_messages == 2 && fake_message_data[0] == 1 && !fake_ring_in);
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    CHECK(!strcmp(view.id, "door.home") && !strcmp(view.toast, "通话已结束"));

    call = (xiaotai_call_status_t){.session = 52, .state = XIAOTAI_CALL_INCOMING, .peer_id = "a", .flags = XIAOTAI_CALL_VIDEO};
    shown_call(&call);
    CHECK(fake_calls == 2 && !strcmp(fake_verb, "accept"));
    call.state = XIAOTAI_CALL_ACTIVE;
    call.active_since_us = 3000000;
    shown_call(&call);
    tap(XIAOTAI_INTENT_APP_DOOR_LOOK_IGNORE);
    CHECK(fake_calls == 3 && !strcmp(fake_verb, "hangup") && !fake_ring_in);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    CHECK(!strcmp(view.id, "door.home"));

    /* Without a picture it rings. */
    call = (xiaotai_call_status_t){.session = 53, .state = XIAOTAI_CALL_INCOMING, .peer_id = "a"};
    shown_call(&call);
    CHECK(fake_calls == 3 && !strcmp(view.id, "door.call") && fake_ring_in && visible_action("door.call.accept"));
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    /* Another application on the screen: the door comes forward to ring. */
    on_owner(show_companion, NULL);
    sync();
    CHECK(!strcmp(foreground, "companion"));
    call = (xiaotai_call_status_t){.session = 55, .state = XIAOTAI_CALL_INCOMING, .peer_id = "a", .flags = XIAOTAI_CALL_VIDEO};
    shown_call(&call);
    CHECK(fake_calls == 3 && !strcmp(foreground, "door") && !strcmp(view.id, "door.call") && fake_ring_in);
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    on_owner(show_door, NULL);
    sync();
    /* Another page of the door is not its home page. */
    tap(XIAOTAI_INTENT_APP_DOOR_SETTINGS);
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_SETTINGS));
    call = (xiaotai_call_status_t){.session = 54, .state = XIAOTAI_CALL_INCOMING, .peer_id = "a", .flags = XIAOTAI_CALL_VIDEO};
    shown_call(&call);
    CHECK(fake_calls == 3 && !strcmp(view.id, "door.call") && fake_ring_in);
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    stop();
}

/* The door station whose call is looked at before it is answered: it still rings for the visitor, and
 * calls the next indoor station when nobody answers. */
static void viewed_test(void)
{
    fake_saved_sender = true;
    strcpy(fake_saved_peer, "a");
    strcpy(fake_saved_peers[0], "b");
    online_contacts("a", true, "b", true);
    start(false);
    fake_dial_session = 90;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    CHECK(fake_dials == 1 && !strcmp(fake_dial_peer, "a"));
    xiaotai_call_status_t call = {.session = 90, .peer_id = "a", .peer_name = "Hall", .state = XIAOTAI_CALL_ACTIVE,
                                  .outgoing = true, .active_since_us = 1000000, .link = 91};
    shown_call(&call);
    xiaotai_call_message_t look = {.session = 90, .type = XIAOTAI_APP_DOOR_MESSAGE_LOOK, .size = 1};
    deliver(&look);
    CHECK(!strcmp(view.id, "door.call") && !strcmp(view.title, "正在呼叫") && strstr(view.text, "看门口") && fake_ring_out);
    wait_ms(XIAOTAI_APP_DOOR_RING_MS);
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "hangup") && fake_control_session == 90);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    fake_dial_session = 92;
    wait_ms(400);
    CHECK(fake_dials == 2 && !strcmp(fake_dial_peer, "b"));      /* not answered: the next one */
    call = (xiaotai_call_status_t){.session = 92, .peer_id = "b", .state = XIAOTAI_CALL_ACTIVE, .outgoing = true,
                                   .active_since_us = 2000000, .link = 93};
    shown_call(&call);
    look.session = 92;
    deliver(&look);
    CHECK(fake_ring_out);
    look.data[0] = 1;                                           /* answered */
    deliver(&look);
    CHECK(!fake_ring_out && !strcmp(view.title, "正在通话"));
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    wait_ms(2000);
    CHECK(fake_dials == 2 && !strcmp(view.id, "door.home") && !strcmp(view.toast, "通话已结束"));
    stop();
}

/* The visitor's bell: the indoor stations one after the other. */
/* The main key: the bell at the door, and a telephone's key on a call. */
static void key_test(void)
{
    extern const xiaotai_interaction_profile_t *fake_profile;
    fake_saved_sender = true;
    strcpy(fake_saved_peer, "a");
    online_contacts("a", true, "b", true);
    start(false);
    CHECK(fake_profile && !strcmp(fake_profile->application, "door") && fake_profile->rule_count == 2);
    intent(XIAOTAI_INTENT_APP_DOOR_KEY, 1);       /* a long press rings nobody */
    sync();
    CHECK(!fake_dials);
    intent(XIAOTAI_INTENT_APP_DOOR_KEY, 0);
    sync();
    CHECK(fake_dials == 1 && !strcmp(fake_dial_peer, "a"));
    xiaotai_call_status_t call = {.session = 90, .peer_id = "a", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    intent(XIAOTAI_INTENT_APP_DOOR_KEY, 0);
    sync();
    CHECK(!strcmp(fake_verb, "hangup") && fake_control_session == 90);
    call_ends(XIAOTAI_CALL_END_LOCAL);
}

static void hunt_test(void)
{
    fake_saved_sender = true;
    strcpy(fake_saved_peer, "a");
    strcpy(fake_saved_peers[0], "b");
    online_contacts("a", true, "b", true);
    start(false);
    CHECK(!strcmp(view.status, "可以呼叫"));
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    CHECK(fake_dials == 1 && !strcmp(fake_dial_peer, "a"));
    xiaotai_call_status_t call = {.session = 90, .peer_id = "a", .peer_name = "Desk", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    CHECK(strstr(view.text, "（1/2）") && fake_ring_out);
    call_ends(XIAOTAI_CALL_END_NO_ANSWER);
    CHECK(!strcmp(view.id, "door.call") && fake_dials == 1 && !fake_ring_out);   /* not home yet */
    intent(XIAOTAI_INTENT_APP_DOOR_RING, 0);
    CHECK(fake_dials == 1);
    fake_dial_error = XIAOTAI_ERR_BUSY;           /* the first call is still being released */
    wait_ms(400);
    CHECK(fake_dials == 2);
    fake_dial_error = XIAOTAI_OK;
    fake_dial_session = 92;
    wait_ms(400);
    CHECK(fake_dials == 3 && !strcmp(fake_dial_peer, "b"));
    call = (xiaotai_call_status_t){.session = 92, .peer_id = "b", .peer_name = "Kitchen", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    CHECK(strstr(view.subtitle, "Kitchen") && strstr(view.text, "（2/2）"));
    call.state = XIAOTAI_CALL_ACTIVE;
    call.active_since_us = 1000000;
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);      /* answered: nobody else is called */
    wait_ms(2000);
    CHECK(fake_dials == 3 && !strcmp(view.id, "door.home") && !strcmp(view.toast, "通话已结束"));

    /* One that nobody answers is given up for the next one, which is not dialled
     * before its answer could be let in. Meanwhile the screen says so. */
    fake_dial_session = 81;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    call = (xiaotai_call_status_t){.session = 81, .peer_id = "a", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    wait_ms(19000);
    CHECK(!fake_calls);
    wait_ms(1500);
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "hangup") && fake_control_session == 81);
    fake_answer_hold = 10000;
    call_ends(XIAOTAI_CALL_END_LOCAL);
    CHECK(!strcmp(view.id, "door.call") && strstr(view.text, "下一台") && view.action_count == 1);
    wait_ms(9000);
    CHECK(fake_dials == 4);
    fake_answer_hold = 0;
    fake_dial_session = 82;
    wait_ms(1500);
    CHECK(fake_dials == 5 && !strcmp(fake_dial_peer, "b"));
    call.session = 82;
    (void)snprintf(call.peer_id, sizeof(call.peer_id), "b");
    shown_call(&call);
    wait_ms(25000);                               /* the last one rings as long as a call does */
    CHECK(fake_calls == 1);
    call_ends(XIAOTAI_CALL_END_NO_ANSWER);
    CHECK(!strcmp(view.id, "door.home") && strstr(view.toast, "暂未接听"));
    /* Rung again right after a cancelled call: waits, and can be cancelled meanwhile. */
    fake_answer_hold = 8000;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    CHECK(fake_dials == 5 && !strcmp(view.id, "door.call") && strstr(view.text, "请稍候"));
    tap("door.call.hangup");
    fake_answer_hold = 0;
    wait_ms(9000);
    CHECK(fake_dials == 5 && !strcmp(view.id, "door.home") && strstr(view.toast, "已取消"));
    fake_calls = 0;
    fake_dials = 3;

    /* A device without video refuses a video call at once: it is called without. */
    fake_dial_session = 93;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    call = (xiaotai_call_status_t){.session = 93, .peer_id = "a", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true,
                                   .flags = XIAOTAI_CALL_VIDEO};
    shown_call(&call);
    fake_call.error = XIAOTAI_ERR_NOT_SUPPORTED;
    call_ends(XIAOTAI_CALL_END_PEER_REJECT);
    fake_call.error = XIAOTAI_OK;
    wait_ms(400);
    CHECK(fake_dials == 5 && !strcmp(fake_dial_peer, "a") && !fake_dial_flags);
    call.flags = 0;
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_PEER_REJECT);      /* declined: the next one */
    wait_ms(400);
    CHECK(fake_dials == 6 && !strcmp(fake_dial_peer, "b"));
    (void)snprintf(call.peer_id, sizeof(call.peer_id), "b");
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_LOCAL);

    /* One that is in another call says so at once: the next one is called, not it again without video. */
    fake_dials = 3;
    fake_dial_session = 97;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    call = (xiaotai_call_status_t){.session = 97, .peer_id = "a", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true,
                                   .flags = XIAOTAI_CALL_VIDEO};
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_BUSY);
    fake_dial_session = 98;
    wait_ms(400);
    CHECK(fake_dials == 5 && !strcmp(fake_dial_peer, "b"));
    call = (xiaotai_call_status_t){.session = 98, .peer_id = "b", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    fake_dials = 3;

    /* Cancelled between two calls. */
    fake_dial_session = 94;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    call = (xiaotai_call_status_t){.session = 94, .peer_id = "a", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_PEER_REJECT);
    intent("door.call.hangup", 94);
    CHECK(!strcmp(view.id, "door.home") && strstr(view.toast, "已取消"));
    wait_ms(2000);
    CHECK(fake_dials == 4);
    /* Cancelled while it rings. */
    fake_dial_session = 96;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    call.session = 96;
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    wait_ms(2000);
    CHECK(fake_dials == 5 && !strcmp(view.id, "door.home") && strstr(view.toast, "已取消"));

    /* One is offline: only the other is called. Both called each other: tried once more. */
    online_contacts("a", false, "b", true);
    fake_dial_session = 98;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    CHECK(fake_dials == 6 && !strcmp(fake_dial_peer, "b"));
    call = (xiaotai_call_status_t){.session = 98, .peer_id = "b", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    CHECK(!strstr(view.text, "/"));
    call_ends(XIAOTAI_CALL_END_BUSY);
    fake_dial_session = 99;
    wait_ms(1500);
    CHECK(fake_dials == 7);
    call.session = 99;
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_BUSY);
    wait_ms(3000);
    CHECK(fake_dials == 7 && !strcmp(view.id, "door.home") && strstr(view.toast, "正忙"));

    /* Somebody looks between two calls: the visitor's call goes on over that one. */
    online_contacts("a", true, "b", true);
    fake_dial_session = 101;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    call = (xiaotai_call_status_t){.session = 101, .peer_id = "a", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_NO_ANSWER);
    on_owner(receive_call, "b");
    sync();
    CHECK(!strcmp(fake_verb, "accept"));
    wait_ms(2000);
    CHECK(fake_dials == 8);
    fake_messages = 0;
    call = (xiaotai_call_status_t){.session = fake_call.session, .peer_id = "b", .state = XIAOTAI_CALL_ACTIVE, .link = 41,
                                   .active_since_us = 1000000};
    shown_call(&call);
    CHECK(fake_messages == 1 && fake_message_type == XIAOTAI_APP_DOOR_MESSAGE_RING && fake_message_data[0] == 1 && fake_ring_out);
    /* Nobody there answers: the other one is called. */
    fake_calls = 0;
    wait_ms(XIAOTAI_APP_DOOR_RING_MS);
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "hangup"));
    fake_dial_session = 102;
    call_ends(XIAOTAI_CALL_END_LOCAL);
    CHECK(!strcmp(view.id, "door.call") && fake_dials == 8);
    wait_ms(400);
    CHECK(fake_dials == 9 && !strcmp(fake_dial_peer, "a"));
    call = (xiaotai_call_status_t){.session = 102, .peer_id = "a", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    CHECK(!strstr(view.text, "/"));
    call_ends(XIAOTAI_CALL_END_NO_ANSWER);
    CHECK(!strcmp(view.id, "door.home"));

    /* Something else took the screen between two calls: the next one is dialled all the same. */
    fake_dial_session = 103;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    call = (xiaotai_call_status_t){.session = 103, .peer_id = "a", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    call_ends(XIAOTAI_CALL_END_PEER_REJECT);
    on_owner(background, NULL);
    const unsigned dials = fake_dials;
    fake_dial_session = 104;
    wait_ms(400);
    CHECK(fake_dials == dials + 1 && !strcmp(fake_dial_peer, "b"));
    on_owner(background, &fake_dials);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    stop();
}

/* Both ends keep to the same story: boxes, results, limits. Indoor station. */
static void agree_test(void)
{
    strcpy(fake_saved_peer, "a");
    strcpy(fake_saved_peers[0], "b");
    online_contacts("a", true, "b", true);
    start(false);
    fake_shows = false;
    tap(XIAOTAI_INTENT_APP_DOOR_MONITOR);
    look_at(90, "a");
    CHECK(strstr(view.text, "没有画面"));         /* nothing to wait for on this device */
    fake_shows = true;

    /* The visitor's call is not cut off, and nothing covers the answer. */
    tap(XIAOTAI_INTENT_APP_DOOR_VOLUME);
    CHECK(!strcmp(view.overlay_id, XIAOTAI_APP_DOOR_OVERLAY_VOLUME));
    wait_ms(XIAOTAI_APP_DOOR_LOOK_MS - 5000U);
    xiaotai_call_message_t ring = {.session = 90, .type = XIAOTAI_APP_DOOR_MESSAGE_RING, .size = 1, .data = {1}};
    deliver(&ring);
    CHECK(!view.overlay_id[0] && fake_ring_in);
    wait_ms(20000);
    CHECK(!fake_calls);
    ring.data[0] = 0;
    deliver(&ring);
    wait_ms(XIAOTAI_APP_DOOR_LOOK_MS - 1000U);
    CHECK(!fake_calls);
    wait_ms(2000);
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "hangup"));
    call_ends(XIAOTAI_CALL_END_LOCAL);

    /* An answer after the wait still counts, once. */
    fake_dial_session = 91;
    tap(XIAOTAI_INTENT_APP_DOOR_MONITOR);
    look_at(91, "a");
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, open_prompt());
    CHECK(!strcmp(view.overlay_actions[0].label, "不等了"));
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, 0);
    CHECK(!view.overlay_id[0] && !opens);
    const xiaotai_call_message_t done = {.session = 91, .type = XIAOTAI_APP_DOOR_MESSAGE_OPEN_RESULT, .size = 4};
    deliver(&done);
    CHECK(opens == 1 && open_result == XIAOTAI_OK && door.opens == 1 && door.opens_done == 1 && !strcmp(view.toast, "门已打开"));
    deliver(&done);
    CHECK(opens == 1);
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, open_prompt());
    wait_ms(4200);
    CHECK(opens == 2 && open_result == XIAOTAI_ERR_TIMEOUT && strstr(view.overlay_text, "没有回应"));
    deliver(&done);
    CHECK(opens == 3 && door.opens == 2 && door.opens_done == 2 && !strcmp(view.overlay_title, "门已打开"));
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);
    xiaotai_call_message_t refused = done;
    for (unsigned i = 0; i < 4; ++i) refused.data[i] = (uint8_t)((uint32_t)XIAOTAI_ERR_NOT_ALLOWED >> (8U * i));
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, open_prompt());
    deliver(&refused);
    CHECK(strstr(view.overlay_text, "没有把本机选为"));
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);

    /* The call breaks: no box stays, and an unanswered request is said to be. */
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN, 0);
    CHECK(!strcmp(view.overlay_title, "确认开门？"));
    call_ends(XIAOTAI_CALL_END_NETWORK);
    CHECK(!view.overlay_id[0] && strstr(view.text, "网络中断"));
    fake_dial_session = 92;
    tap(XIAOTAI_INTENT_APP_DOOR_MONITOR);
    look_at(92, "a");
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, open_prompt());
    const int before = opens;
    call_ends(XIAOTAI_CALL_END_NETWORK);
    CHECK(!view.overlay_id[0] && strstr(view.toast, "没有收到开门结果") && opens == before + 1 && open_result == XIAOTAI_ERR_TIMEOUT);
    const xiaotai_call_message_t late = {.session = 92, .type = XIAOTAI_APP_DOOR_MESSAGE_OPEN_RESULT, .size = 4};
    deliver(&late);
    CHECK(opens == before + 1);
    intent(XIAOTAI_INTENT_APP_DOOR_BACK, 0);

    /* Answering shows the visitor no room. A call from the contacts says that we talk. */
    fake_camera_switches = 0;
    xiaotai_call_status_t call = {.session = 93, .peer_id = "a", .state = XIAOTAI_CALL_ACTIVE, .link = 94, .active_since_us = 1000000};
    shown_call(&call);
    const xiaotai_video_call_status_t video = {.active = true, .link = 94, .camera = true, .camera_enabled = true};
    emit(XIAOTAI_EVENT_VIDEO_CALL_STATE, 1, &video, sizeof(video));
    sync();
    CHECK(fake_camera_switches == 1);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    call = (xiaotai_call_status_t){.session = 95, .peer_id = "a", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    fake_messages = 0;
    call.state = XIAOTAI_CALL_ACTIVE;
    call.active_since_us = 1000000;
    shown_call(&call);
    CHECK(fake_messages == 1 && fake_message_type == XIAOTAI_APP_DOOR_MESSAGE_LOOK && fake_message_data[0] == 1);
    call_ends(XIAOTAI_CALL_END_LOCAL);

    /* Left open on the way, a page goes back by itself; a touch begins the wait again. */
    tap(XIAOTAI_INTENT_APP_DOOR_SETTINGS);
    wait_ms(59000);
    emit(XIAOTAI_EVENT_UI_INTERACTION, 0, NULL, 0);
    sync();
    wait_ms(59000);
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_SETTINGS));
    wait_ms(1500);
    CHECK(!strcmp(view.id, "door.home"));
    stop();
}

/* The same for the door station. */
static void agree_station_test(void)
{
    fake_saved_sender = true;
    strcpy(fake_saved_peer, "a");
    const xiaotai_app_door_config_t config = {.open_on_screen = true};
    CHECK(xiaotai_app_door_configure(&config) == XIAOTAI_OK);
    online_contacts("a", true, "other", true);
    start(false);
    intent(XIAOTAI_INTENT_APP_DOOR_DIRECTORY, 0);    /* no longer on a doorbell's screen */
    wait_ms(61000);
    CHECK(!strcmp(view.id, "door.home"));
    intent(XIAOTAI_INTENT_APP_DOOR_DIRECTORY, 0);    /* no longer on a doorbell's screen */
    fake_dial_session = 90;
    intent(XIAOTAI_INTENT_APP_DOOR_DIAL, view.item_value[view.count - 1]);
    CHECK(fake_dials == 1 && !strcmp(fake_dial_peer, "other"));
    xiaotai_call_status_t call = {.session = 90, .peer_id = "other", .state = XIAOTAI_CALL_OUTGOING, .outgoing = true};
    shown_call(&call);
    call.state = XIAOTAI_CALL_ACTIVE;
    call.active_since_us = 1000000;
    shown_call(&call);
    const xiaotai_call_message_t open = {.session = 90, .type = XIAOTAI_APP_DOOR_MESSAGE_OPEN};
    deliver(&open);   /* called from the directory, but not chosen */
    CHECK(fake_message_type == XIAOTAI_APP_DOOR_MESSAGE_OPEN_RESULT && open_result == XIAOTAI_ERR_NOT_ALLOWED);
    CHECK(!fake_sounds && !strcmp(view.overlay_title, "门没有打开") && strstr(view.overlay_text, "权限"));
    call_ends(XIAOTAI_CALL_END_LOCAL);
    stop();
}

static void household(unsigned index, int kind, const char *id, const char *name, bool online)
{
    fake_contacts[index] = (xiaotai_contact_t){.kind = (int8_t)kind, .online_known = kind == 1, .online = online};
    (void)snprintf(fake_contacts[index].id, sizeof(fake_contacts[0].id), "%s", id);
    (void)snprintf(fake_contacts[index].name, sizeof(fake_contacts[0].name), "%s", name);
    if (fake_list.page_count <= index) fake_list.page_count = (uint8_t)(index + 1U);
}

static void type(const char *digits)
{
    for (; *digits; ++digits) intent(XIAOTAI_INTENT_APP_DOOR_KEY_DIGIT, *digits - '0');
}

static void dialled(uint32_t session, const char *peer, bool active)
{
    xiaotai_call_status_t call = {.session = session, .state = active ? XIAOTAI_CALL_ACTIVE : XIAOTAI_CALL_OUTGOING, .outgoing = true,
                                  .link = active ? session + 1U : 0, .active_since_us = active ? 1000000 : 0};
    (void)snprintf(call.peer_id, sizeof(call.peer_id), "%s", peer);
    shown_call(&call);
}

/* The access machine: rooms, households, what a visitor is told, who opens. */
static void access_test(void)
{
    fake_saved_sender = XIAOTAI_APP_DOOR_ACCESS;
    const xiaotai_app_door_config_t config = {.open_on_screen = true};
    CHECK(xiaotai_app_door_configure(&config) == XIAOTAI_OK);
    fake_net.state = XIAOTAI_NET_UP;
    fake_platform.phase = XIAOTAI_PLATFORM_ONLINE;
    fake_list = (xiaotai_contacts_status_t){.generation = 1};
    household(0, 1, "off", "3栋2单元301 王先生", false);
    household(1, 1, "on", "3栋2单元0301", true);
    household(2, 0, "wx", "3-2-301 王先生", false);
    household(3, 1, "other", "4栋301", true);
    household(4, 1, "nobody", "", true);
    household(5, 1, "desk", "前台", true);
    start(false);
    CHECK(door.sender && door.access && !strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_KEYPAD) && view.kind == XIAOTAI_VIEW_FORM);
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_DIRECTORY) && !visible_action(XIAOTAI_INTENT_APP_DOOR_RING));   /* no desk chosen */
    CHECK(visible_action(XIAOTAI_INTENT_APP_DOOR_SETTINGS)->trigger == XIAOTAI_TRIGGER_LONG_PRESS);
    intent(XIAOTAI_INTENT_APP_DOOR_KEY, 0);
    CHECK(strstr(view.toast, "还没有选管理处") && !fake_dials);

    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    CHECK(strstr(view.toast, "请输入房号"));
    type("999");
    CHECK(!strcmp(view.subtitle, "999") && fake_keys >= 3U);
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    CHECK(strstr(view.toast, "没有这个房号") && !fake_dials);
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CLEAR, 0);
    CHECK(!view.subtitle[0]);
    type("30");
    wait_ms(14000);
    type("1");
    wait_ms(14000);
    CHECK(!strcmp(view.subtitle, "301"));
    wait_ms(1500);                               /* nobody is there any more */
    CHECK(!view.subtitle[0]);
    type("12345678901234");
    CHECK(strlen(view.subtitle) == DOOR_ROOM_TYPED_MAX);
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CLEAR, 0);

    /* Two buildings have a 301: the visitor says which. No device number anywhere. */
    type("301");
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_DIRECTORY) && !strcmp(view.title, "请选择") && view.count == 2);
    CHECK(!strcmp(view.rows[0], "3栋2单元301") && !strcmp(view.rows[1], "4栋301"));    /* the door, not the name */
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);
    /* The door numbers, and below them the directory: everybody by name. */
    tap(XIAOTAI_INTENT_APP_DOOR_DIRECTORY);
    CHECK(!strcmp(view.title, "门牌号") && view.count == 3 && !strcmp(view.rows[0], "通讯录") &&
          !strcmp(view.rows[1], "3栋2单元301") && !strcmp(view.rows[2], "4栋301"));
    intent(XIAOTAI_INTENT_APP_DOOR_DIRECTORY, 1);
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    CHECK(view.count == 5 && !strcmp(view.rows[4], "3-2-301 王先生"));     /* WeChat after the devices */
#else
    CHECK(view.count == 4);
#endif
    CHECK(!strcmp(view.title, "通讯录") && !strcmp(view.rows[0], "3栋2单元301 王先生") && !strcmp(view.rows[3], "前台"));
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);
    CHECK(!strcmp(view.title, "门牌号"));
    tap(XIAOTAI_INTENT_APP_DOOR_BACK);
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_KEYPAD));

    /* One household: the device that is online, then WeChat, not the one known to be offline. */
    type("32301");
    fake_dial_session = 90;
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    CHECK(fake_dials == 1 && !strcmp(fake_dial_peer, "on") && !view.subtitle[0]);
    dialled(90, "on", false);
    CHECK(!strcmp(view.id, "door.call") && strstr(view.subtitle, "3栋2单元301") && !strstr(view.subtitle, "王先生"));
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    CHECK(strstr(view.text, "（1/2）"));
    call_ends(XIAOTAI_CALL_END_NO_ANSWER);
    fake_wechat_session = 70;
    wait_ms(400);
    CHECK(fake_dials == 1 && fake_wechat_dials == 1 && !strcmp(fake_wechat_dial_id, "wx"));
    xiaotai_wechat_status_t wechat = {.state = XIAOTAI_WECHAT_OUTGOING, .session = 70, .outgoing = true};
    apply_wechat(&wechat);
    CHECK(strstr(view.subtitle, "3栋2单元301") && strstr(view.subtitle, "（2/2）"));
    wait_ms(30500);                              /* nobody takes it up */
    CHECK(fake_wechat_hangups == 1);
    wechat = (xiaotai_wechat_status_t){.end = XIAOTAI_WECHAT_END_LOCAL};
    apply_wechat(&wechat);
#else
    call_ends(XIAOTAI_CALL_END_NO_ANSWER);
#endif
    CHECK(!strcmp(view.id, XIAOTAI_APP_DOOR_PAGE_KEYPAD) && !strcmp(view.toast, "3栋2单元301 暂时无人应答"));

    /* Cancelled by the visitor: said as such. Answered: the device called may open, chosen or not. */
    type("4301");
    fake_dial_session = 91;
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    dialled(91, "other", false);
    call_ends(XIAOTAI_CALL_END_LOCAL);
    CHECK(strstr(view.toast, "已取消"));
    type("4301");
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    dialled(91, "other", false);
    call_ends(XIAOTAI_CALL_END_PEER_REJECT);
    CHECK(!strcmp(view.toast, "4栋301 现在不方便接听"));
    type("4301");
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    dialled(91, "other", false);
    call_ends(XIAOTAI_CALL_END_BUSY);            /* tried once more */
    wait_ms(1500);
    dialled(91, "other", false);
    call_ends(XIAOTAI_CALL_END_BUSY);
    CHECK(!strcmp(view.toast, "4栋301 正忙，请稍后再试"));
    type("4301");
    fake_dial_session = 92;
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    dialled(92, "other", false);
    dialled(92, "other", true);
    CHECK(strstr(view.subtitle, "4栋301"));
    const xiaotai_call_message_t open = {.session = 92, .type = XIAOTAI_APP_DOOR_MESSAGE_OPEN};
    deliver(&open);
    CHECK(opens == 1 && open_result == XIAOTAI_OK && !strcmp(view.overlay_title, "门已打开，请进"));
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    CHECK(!strcmp(view.toast, "通话已结束") && !door.called[0]);
    /* The same device calling while the keypad is up looks at the door, as an indoor station of
     * the building does; it is nobody in particular and opens nothing. */
    fake_calls = 0;
    on_owner(receive_call, "other");
    sync();
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "accept"));
    const int before = opens;
    deliver(&open);
    CHECK(opens == before);
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);

    /* The desk: whoever was chosen; it is called with 管理处 and looks from anywhere. */
    on_owner(save_peer, "desk");
    sync();
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    CHECK(!strcmp(fake_dial_peer, "desk"));
    dialled(92, "desk", false);
    CHECK(strstr(view.subtitle, "管理处"));
    call_ends(XIAOTAI_CALL_END_NO_ANSWER);
    CHECK(!strcmp(view.toast, "管理处 暂时无人应答"));
    fake_calls = 0;
    on_owner(receive_call, "desk");
    sync();
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "accept"));
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);

    /* Without the network, and before the list is there, it says so. */
    fake_list.generation = 0;
    type("301");
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    CHECK(strstr(view.toast, "通讯录"));
    fake_net.state = XIAOTAI_NET_WAITING;
    intent(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, 0);
    CHECK(strstr(view.toast, "网络未连接"));
    stop();
}

/* The first contact pairs the side that was not set up. */
static void pairing_test(void)
{
    xiaotai_app_door_status_t status;
    /* A doorbell nobody set up: the first device that looks at it is its indoor station, and no other. */
    fake_saved_sender = true;
    fake_saved_peer[0] = '\0';
    online_contacts("a", true, "b", true);
    start(true);
    xiaotai_app_door_status(&status);
    CHECK(!status.peers && !visible_action(XIAOTAI_INTENT_APP_DOOR_RING));
    on_owner(receive_call, "a");
    sync();
    CHECK(fake_calls == 1 && !strcmp(fake_verb, "accept"));
    xiaotai_call_status_t call = {.session = 40, .state = XIAOTAI_CALL_ACTIVE, .peer_id = "a", .link = 41,
                                  .active_since_us = 1000000};
    shown_call(&call);
    xiaotai_app_door_status(&status);
    CHECK(!status.peers && !fake_hellos);          /* a call is not a look until it says so; nothing it placed */
    xiaotai_call_message_t look = {.session = 40, .type = XIAOTAI_APP_DOOR_MESSAGE_LOOK, .size = 1};
    deliver(&look);
    xiaotai_app_door_status(&status);
    CHECK(status.peers == 1 && !strcmp(fake_saved_peer, "a"));
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    CHECK(!strcmp(view.id, "door.home") && visible_action(XIAOTAI_INTENT_APP_DOOR_RING));
    fake_calls = 0;
    on_owner(receive_call, "b");
    sync();
    call = (xiaotai_call_status_t){.session = 40, .state = XIAOTAI_CALL_ACTIVE, .peer_id = "b", .link = 42,
                                   .active_since_us = 2000000};
    shown_call(&call);
    deliver(&look);
    xiaotai_app_door_status(&status);
    CHECK(status.peers == 1 && !strcmp(fake_saved_peer, "a"));
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    /* Its own call says who calls, once it is answered. */
    fake_dial_session = 90;
    tap(XIAOTAI_INTENT_APP_DOOR_RING);
    CHECK(fake_dials == 1 && !fake_hellos);
    call = (xiaotai_call_status_t){.session = 90, .state = XIAOTAI_CALL_OUTGOING, .outgoing = true, .peer_id = "a"};
    shown_call(&call);
    CHECK(!fake_hellos);
    call.state = XIAOTAI_CALL_ACTIVE;
    call.link = 91;
    call.active_since_us = 3000000;
    shown_call(&call);
    CHECK(fake_hellos == 1 && fake_hello_mode == XIAOTAI_APP_DOOR_BELL);
    shown_call(&call);                              /* the same call again: said once */
    CHECK(fake_hellos == 1);
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    stop();

    /* An indoor station that did not choose the door station learns it from the call it answered. That
     * call stays where it rang; the next one is a door call. */
    fake_saved_sender = false;
    fake_saved_peer[0] = '\0';
    fake_calls = fake_hellos = 0;
    online_contacts("a", true, "b", true);
    start(true);
    const unsigned companion = companion_calls;
    on_owner(receive_call, "a");
    sync();
    CHECK(companion_calls == companion + 1 && !strcmp(foreground, "companion"));
    call = (xiaotai_call_status_t){.session = 40, .state = XIAOTAI_CALL_ACTIVE, .peer_id = "a", .link = 41,
                                   .active_since_us = 1000000};
    shown_call(&call);
    CHECK(!strcmp(foreground, "companion") && strcmp(view.id, "door.call"));
    xiaotai_call_message_t hello = {.session = 40, .type = XIAOTAI_APP_DOOR_MESSAGE_HELLO, .size = 1,
                                    .data = {XIAOTAI_APP_DOOR_INDOOR}};
    deliver(&hello);                                /* an indoor station is no door station */
    xiaotai_app_door_status(&status);
    CHECK(!status.peers);
    hello.session = 39;                             /* nor is a call that is not the running one */
    hello.data[0] = XIAOTAI_APP_DOOR_BELL;
    deliver(&hello);
    xiaotai_app_door_status(&status);
    CHECK(!status.peers);
    hello.session = 40;
    deliver(&hello);
    xiaotai_app_door_status(&status);
    CHECK(status.peers == 1 && !strcmp(fake_saved_peer, "a"));
    CHECK(!strstr(view.toast, "已添加"));           /* another application is in front: said with the next door page */
    shown_call(&call);
    CHECK(companion_calls == companion + 1 && !strcmp(foreground, "companion") && strcmp(view.id, "door.call"));
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    receive_session = 44;                           /* the call module never gives a number twice */
    on_owner(receive_call, "a");
    sync();
    CHECK(companion_calls == companion + 1 && !strcmp(foreground, "door") && !strcmp(view.id, "door.call"));
    CHECK(visible_action("door.call.accept") && strstr(view.toast, "已添加") && strstr(view.toast, "Device a"));
    call_ends(XIAOTAI_CALL_END_PEER_CANCEL);
    /* Chosen already, or said again: nothing changes. */
    receive_session = 46;
    on_owner(receive_call, "a");
    sync();
    receive_session = 40;
    call = (xiaotai_call_status_t){.session = 46, .state = XIAOTAI_CALL_ACTIVE, .peer_id = "a", .link = 47,
                                   .active_since_us = 4000000};
    shown_call(&call);
    memset(view.toast, 0, sizeof(view.toast));
    hello.session = 46;
    deliver(&hello);
    xiaotai_app_door_status(&status);
    CHECK(status.peers == 1 && !view.toast[0] && !strcmp(view.id, "door.call"));
    call_ends(XIAOTAI_CALL_END_PEER_HANGUP);
    stop();
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "pairing") == 0) { pairing_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "access") == 0) { access_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "mode") == 0) { mode_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "mode_sound") == 0) { mode_sound_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "peers") == 0) { peers_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "look") == 0) { look_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "looked") == 0) { looked_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "take") == 0) { take_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "bell_indoor") == 0) { bell_indoor_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "pages") == 0) { pages_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "viewed") == 0) { viewed_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "hunt") == 0) { hunt_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "agree") == 0) { agree_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "agree_station") == 0) { agree_station_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "key") == 0) { key_test(); return 0; }
    if (argc == 2 && strcmp(argv[1], "station") == 0) station_test();
    else if (argc == 2 && strcmp(argv[1], "directory") == 0) directory_test();
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    else if (argc == 2 && strcmp(argv[1], "station_wechat") == 0) station_wechat_test();
#endif
    else if (argc == 2 && strcmp(argv[1], "remote_open") == 0) remote_open_test();
    else if (argc == 2 && strcmp(argv[1], "presenter") == 0) presenter_test();
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
