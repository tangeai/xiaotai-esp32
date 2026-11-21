// SPDX-License-Identifier: MIT
#include "fakes.h"
#include "feedback_fakes.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/modules/room.h"
#include "xiaotai/port/os.h"

static atomic_int_fast64_t now;
static xiaotai_signal_t fence;
static xiaotai_room_status_t snapshot;
static xiaotai_room_member_t people[4];
static unsigned attends, exits, creates, joins, leaves, presses, releases, connects, disconnects, refreshes;
static unsigned press_attempts;
static bool deny_press, local_hold_config, pointer_hold_config;
static uint32_t ai;
static uint32_t held_ticket;
static companion_local_room_hold_fn_t held_callback;
static char last_code[7], last_password[5];

extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_room;

static bool alias(const char *id, char *out, size_t capacity)
{
    if (strcmp(id, "device-b")) return false;
    snprintf(out, capacity, "%s", "客厅");
    return true;
}

static xiaotai_err_t attach_hold(const char *intent, companion_local_room_hold_fn_t fn,
                                  companion_local_room_refresh_fn_t refresh)
{
    assert((refresh != NULL) == pointer_hold_config);
    assert(!strcmp(intent, XIAOTAI_INTENT_APP_COMPANION_ROOM_TALK) && !held_callback && fn);
    held_callback = fn;
    return XIAOTAI_OK;
}

static const xiaotai_app_companion_part_t contacts = {.name = "contacts_fixture", .room_member_name = alias,
    .register_local_room_hold = attach_hold};
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {
    &xiaotai_app_companion_with_room, &contacts, NULL,
};

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&now); }

static void fence_signal(void *ctx) { (void)ctx; xiaotai_signal_give(&fence); }
static void flush(void)
{
    for (unsigned n = 0; n < 5U; ++n) {
        assert(xiaotai_run(fence_signal, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void at(int64_t ms) { atomic_store(&now, ms * INT64_C(1000)); flush(); }

static void send(const char *intent, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) snprintf(args.text, sizeof(args.text), "%s", text);
    assert(xiaotai_intent_post(intent, &args) == XIAOTAI_OK);
    flush();
}

static void emit(const char *event, const void *data, size_t size)
{
    assert(xiaotai_event_emit(event, NULL, data, size) == XIAOTAI_OK);
    flush();
}

static const xiaotai_view_action_t *find_action(unsigned role, const char *name)
{
    for (size_t i = 0; i < fixture.action_count; ++i) {
        const xiaotai_view_action_t *a = fixture.actions + i;
        if (role && a->role != role) continue;
        if (name && ((!a->label || strcmp(a->label, name)) && (!a->icon || strcmp(a->icon, name)))) continue;
        return a;
    }
    return NULL;
}

static void choose(const char *label)
{
    for (size_t i = 0; i < fixture.item_count; ++i) {
        const xiaotai_view_item_t *item = fixture.items + i;
        if (strcmp(item->label, label)) continue;
        assert(item->intent && !item->state);
        send(item->intent, NULL, item->value);
        return;
    }
    fprintf(stderr, "missing option %s on %s\n", label, fixture.id);
    assert(false);
}

static void action(unsigned role, const char *label, const char *text)
{
    const xiaotai_view_action_t *a = find_action(role, label);
    assert(a && a->intent && !a->state);
    send(a->intent, text, a->value);
}

static void field(unsigned i, const char *value)
{
    assert(i < fixture.field_count && fixture.fields[i].intent);
    send(fixture.fields[i].intent, value, 0);
}

void xiaotai_room_status(xiaotai_room_status_t *out) { *out = snapshot; }
xiaotai_err_t xiaotai_room_observe_attempts(xiaotai_room_attempt_fn_t observer)
{
    (void)observer;
    assert(false && "the UI-only fixture does not opt into room end feedback");
    return XIAOTAI_ERR_INVALID_STATE;
}
size_t xiaotai_room_members(uint16_t offset, xiaotai_room_member_t *out, size_t count, uint32_t *revision)
{
    assert(count == 1);
    if (revision) *revision = snapshot.members_revision;
    if (offset >= snapshot.listed_count) return 0;
    *out = people[offset];
    return 1;
}

xiaotai_err_t xiaotai_room_attend(bool attending)
{
    assert(xiaotai_on_owner());
    if (attending) ++attends;
    else ++exits;
    snapshot.attending = attending;
    (void)xiaotai_event_emit(XIAOTAI_EVENT_ROOM_STATE, NULL, NULL, 0);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_room_create(const char *password)
{
    ++creates;
    snprintf(last_password, sizeof(last_password), "%s", password ? password : "");
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_room_join(const char *code, const char *password)
{
    ++joins;
    snprintf(last_code, sizeof(last_code), "%s", code);
    snprintf(last_password, sizeof(last_password), "%s", password ? password : "");
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_room_refresh(void) { ++refreshes; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_room_connect(void) { ++connects; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_room_disconnect(void) { ++disconnects; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_room_talk(uint32_t generation, bool pressed)
{
    if (pressed) {
        assert(generation == snapshot.generation);
        ++press_attempts;
        if (deny_press) return XIAOTAI_ERR_INVALID_STATE;
        ++presses;
    } else ++releases;
    snapshot.transmitting = pressed;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_room_toggle_talk(uint32_t generation, bool *pressed)
{
    *pressed = !snapshot.transmitting;
    return xiaotai_room_talk(generation, *pressed);
}

xiaotai_err_t xiaotai_room_hold_local(uint32_t generation, uint32_t ticket, bool pressed)
{
    assert(!xiaotai_on_owner() && ticket);
    if (!generation || generation != snapshot.generation) return pressed ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
    if (!pressed && held_ticket != ticket) return XIAOTAI_OK;
    const xiaotai_err_t err = xiaotai_room_talk(generation, pressed);
    if (err == XIAOTAI_OK) held_ticket = pressed ? ticket : 0;
    return err;
}

xiaotai_err_t xiaotai_room_hold_local_refresh(uint32_t generation, uint32_t ticket)
{
    return generation && ticket == held_ticket ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

static void leave(const xiaotai_args_t *args, void *ctx) { (void)args; (void)ctx; ++leaves; }

static xiaotai_err_t fixture_init(void)
{
    xiaotai_err_t err = companion_fixture_init();
    return err == XIAOTAI_OK ? xiaotai_intent_handle(XIAOTAI_INTENT_ROOM_LEAVE, leave, NULL) : err;
}

static void boot(uint32_t needs, bool confirms, bool keeps)
{
    static const xiaotai_module_t mod = {.name = "fixture", .init = fixture_init};
    static const xiaotai_module_t *const modules[] = {&mod};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&app};
    fixture.needs = needs;
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    feedback_fixture = (companion_feedback_fixture_t){.enabled = true, .audio_ready = true};
    snapshot.phase = XIAOTAI_ROOM_EMPTY;
    snapshot.assignment_known = true;
    atomic_store(&now, 1000000);
    assert(xiaotai_app_companion_configure(&(xiaotai_app_companion_config_t){
        .room_leave_confirm = confirms, .room_keeps_talk = keeps, .room_local_hold = local_hold_config,
        .room_pointer_hold = pointer_hold_config}) == XIAOTAI_OK);
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1,
        .applications = apps, .application_count = 1, .home = "companion", .available_needs = fixture.needs};
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    flush();
}

static void assigned(bool count_only)
{
    snapshot.phase = XIAOTAI_ROOM_LISTENING;
    snapshot.assigned = snapshot.members_ready = true;
    snapshot.generation = 11;
    snapshot.assignment_version = snapshot.members_revision = 5;
    snapshot.member_count = 4;
    snapshot.listed_count = count_only ? 0 : 4;
    strcpy(snapshot.room_code, "012345");
    strcpy(snapshot.room_id, "room-a");
    people[0] = (xiaotai_room_member_t){.device_id = "device-a", .self = true};
    people[1] = (xiaotai_room_member_t){.device_id = "device-b", .mic = XIAOTAI_ROOM_MIC_SPEAKING};
    people[2] = (xiaotai_room_member_t){.device_id = "device-c", .mic = XIAOTAI_ROOM_MIC_ON};
    people[3] = (xiaotai_room_member_t){.device_id = "device-d"};
    ++snapshot.revision;
    emit(XIAOTAI_EVENT_ROOM_STATE, NULL, 0);
}

static void pointer(void)
{
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH, false, false);
    fixture.fail_talk_stop = true;
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "room", 0);
    assert(!strcmp(fixture.id, "home") && !attends && fixture.last_refusal.value == XIAOTAI_ERR_NO_MEM);
    fixture.fail_talk_stop = false;
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "room", 0);
    assert(!strcmp(fixture.id, "room") && attends == 1 && fixture.talk_stops == 1);
    choose("加入房间");
    assert(attends == 1 && !exits && fixture.talk_stops == 1 && fixture.kind == XIAOTAI_VIEW_FORM);
    assert(fixture.fields[0].max == 6 && fixture.fields[1].max == 4 &&
        (fixture.fields[1].flags & (XIAOTAI_FIELD_MASKED | XIAOTAI_FIELD_ALLOW_EMPTY)) ==
        (XIAOTAI_FIELD_MASKED | XIAOTAI_FIELD_ALLOW_EMPTY));
    field(0, "01234");
    action(XIAOTAI_ROLE_PRIMARY, NULL, NULL);
    assert(!joins && fixture.last_refusal.value == XIAOTAI_ERR_INVALID_ARG);
    field(0, "012345");
    field(1, "0000");
    action(XIAOTAI_ROLE_BACK, NULL, NULL);
    assert(!strcmp(fixture.id, "room") && attends == 1 && !exits);
    choose("加入房间");
    assert(!strcmp(fixture.fields[0].text, "012345") && !strcmp(fixture.fields[1].text, "0000"));
    action(XIAOTAI_ROLE_PRIMARY, NULL, NULL);
    assert(joins == 1 && !strcmp(last_code, "012345") && !strcmp(last_password, "0000"));
    assert(find_action(XIAOTAI_ROLE_PRIMARY, NULL)->state & XIAOTAI_STATE_BUSY);
    emit(XIAOTAI_EVENT_ROOM_RESULT, &(xiaotai_room_result_t){.action = XIAOTAI_ROOM_ACTION_JOIN,
        .error = XIAOTAI_FAIL, .code = XIAOTAI_ROOM_CODE_WRONG_PASSWORD}, sizeof(xiaotai_room_result_t));
    assert(!strcmp(fixture.toast, "密码错误"));
    at(2500);
    field(1, "");
    action(XIAOTAI_ROLE_PRIMARY, NULL, NULL);
    assert(joins == 2 && !last_password[0]);
    assigned(false);
    emit(XIAOTAI_EVENT_ROOM_RESULT, &(xiaotai_room_result_t){.action = XIAOTAI_ROOM_ACTION_JOIN}, sizeof(xiaotai_room_result_t));
    assert(!strcmp(fixture.title, "012345") && !strcmp(fixture.subtitle, "4 人在线 1/2"));
    assert(fixture.item_count == 3 && !strcmp(fixture.items[1].label, "客厅") &&
        !strcmp(fixture.items[1].detail, "讲话中") && strstr(fixture.items[0].detail, "本机"));
    action(0, XIAOTAI_ICON_NEXT, NULL);
    assert(!strcmp(fixture.subtitle, "4 人在线 2/2") && !strcmp(fixture.items[0].label, "device-d"));
    const xiaotai_view_action_t *a = find_action(XIAOTAI_ROLE_PRIMARY, NULL);
    assert(a && a->trigger == XIAOTAI_TRIGGER_HOLD && a->value == 11);
    deny_press = true;
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_PRESS);
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_PRESS);
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_RELEASE);
    assert(press_attempts == 1 && !presses && !releases && !strcmp(fixture.toast, "尚未就绪或麦克风已关闭"));
    deny_press = false;
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_PRESS);
    assert(find_action(XIAOTAI_ROLE_PRIMARY, "松开结束讲话"));
    at(2750);
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_PRESS);
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_RELEASE);
    assert(presses == 2 && releases == 1);
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_PRESS);
    send(XIAOTAI_INTENT_APP_COMPANION_ROOM_TALK, XIAOTAI_ARG_RELEASE, 10);
    assert(presses == 3 && releases == 1);
    at(3501);
    snapshot.transmitting = false; /* room engine's watchdog closed the gate */
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_PRESS);
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_PRESS);
    assert(presses == 3 && releases == 2);
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_RELEASE);
    action(XIAOTAI_ROLE_PRIMARY, NULL, XIAOTAI_ARG_PRESS);
    assert(presses == 4 && releases == 2);
    action(0, "退出房间", NULL);
    assert(leaves == 1 && !fixture.overlays);
    snapshot.generation = 0;
    fixture.net.state = XIAOTAI_NET_WAITING;
    emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, NULL, 0);
    assert(!strcmp(fixture.id, "room") && !strcmp(fixture.status, "网络未连接") && releases == 3);
    at(13500);
    assert(!strcmp(fixture.id, "room") && !exits);
    at(13501);
    assert(!strcmp(fixture.id, XIAOTAI_APP_COMPANION_PAGE_SETUP) && exits == 1);
}

static void start_ai(void *ctx)
{
    (void)ctx;
    assert(xiaotai_activity_begin(&(xiaotai_activity_t){.name = "ai"}, &ai) == XIAOTAI_OK);
}

static void end_ai(void *ctx) { (void)ctx; xiaotai_activity_end(ai); }

static void options(void)
{
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH, true, true);
    assert(!held_callback);
    assert(xiaotai_run(start_ai, NULL) == XIAOTAI_OK);
    flush();
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "room", 0);
    assert(attends == 1 && !fixture.talk_stops);
    choose("加入房间");
    assert(!strcmp(fixture.id, "room") && fixture.last_refusal.value == XIAOTAI_ERR_BUSY && !joins);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "settings", 0);
    assert(!strcmp(fixture.id, "settings") && fixture.talk_stops == 1 && exits == 1);
    assert(xiaotai_run(end_ai, NULL) == XIAOTAI_OK);
    flush();
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(!strcmp(fixture.id, "room") && attends == 2);
    assigned(false);
    action(0, "退出房间", NULL);
    assert(!leaves && !strcmp(fixture.overlay_title, "退出房间") && !strcmp(fixture.overlay_body, "退出后需要重新加入"));
    send(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, NULL, 0);
    assert(!leaves && !strcmp(fixture.id, "room"));
    action(0, "退出房间", NULL);
    ++snapshot.assignment_version;
    send(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, NULL, 1);
    assert(!leaves && fixture.last_refusal.value == XIAOTAI_ERR_INVALID_STATE);
    action(0, "退出房间", NULL);
    send(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, NULL, 1);
    assert(leaves == 1);

    at(1100);
    fixture.net.state = XIAOTAI_NET_WAITING;
    emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, NULL, 0);
    assert(!strcmp(fixture.id, "room"));
    at(11099);
    fixture.net.state = XIAOTAI_NET_UP;
    emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, NULL, 0);
    at(11101);
    assert(!strcmp(fixture.id, "room") && exits == 1);
    fixture.net.state = XIAOTAI_NET_WAITING;
    fixture.provision.phase = XIAOTAI_PROVISIONING_WAITING;
    emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, NULL, 0);
    assert(!strcmp(fixture.id, XIAOTAI_APP_COMPANION_PAGE_SETUP) && exits == 2);
}

static void keys(void)
{
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY, false, false);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "room", 0);
    choose("创建房间");
    assert(creates == 1 && !last_password[0]);
    choose("加入密码房间");
    assert(fixture.field_count == 2 && fixture.fields[1].flags == XIAOTAI_FIELD_MASKED);
    action(XIAOTAI_ROLE_BACK, NULL, NULL);
    assigned(true);
    choose("开始发言");
    /* The owner refused the asynchronous send without leaving the room. */
    snapshot.transmitting = false;
    snapshot.error = -5;
    emit(XIAOTAI_EVENT_ROOM_STATE, NULL, 0);
    choose("开始发言");
    assert(presses == 2 && releases == 0 && snapshot.transmitting);
    choose("停止发言");
    assert(presses == 2 && releases == 1 && !snapshot.transmitting);
    action(XIAOTAI_ROLE_BACK, NULL, NULL);
    assert(disconnects == 1 && exits == 1 && !strcmp(fixture.id, "home"));
}

static void physical_hold(void)
{
    local_hold_config = true;
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH, false, false);
    assert(held_callback);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "room", 0);
    assigned(false);
    assert(held_callback(1, 11, true) == XIAOTAI_OK && snapshot.transmitting);
    emit(XIAOTAI_EVENT_ROOM_STATE, NULL, 0);
    assert(find_action(XIAOTAI_ROLE_PRIMARY, "松开结束讲话"));
    assert(held_callback(1, 11, false) == XIAOTAI_OK && !snapshot.transmitting && releases == 1);
    emit(XIAOTAI_EVENT_ROOM_STATE, NULL, 0);
    assert(find_action(XIAOTAI_ROLE_PRIMARY, "按住讲话"));
    deny_press = true;
    assert(held_callback(2, 11, true) == XIAOTAI_ERR_INVALID_STATE);
    flush();
    assert(fixture.toasts == 1 && !strcmp(fixture.toast, "尚未就绪或麦克风已关闭"));
    assert(held_callback(2, 11, false) == XIAOTAI_OK && releases == 1);
    assert(held_callback(3, 10, true) == XIAOTAI_ERR_INVALID_STATE);
    flush();
    assert(fixture.toasts == 1);
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(held_callback(4, 11, true) == XIAOTAI_ERR_INVALID_STATE);
    flush();
    assert(fixture.toasts == 1 && exits == 1);
}

static void pointer_hold_feedback(void)
{
    pointer_hold_config = true;
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH, false, false);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "room", 0);
    assert(held_callback && !find_action(XIAOTAI_ROLE_PRIMARY, "按下讲话"));
    assigned(false);
    snapshot.generation = 0;
    emit(XIAOTAI_EVENT_ROOM_STATE, NULL, 0);
    const xiaotai_view_action_t *a = find_action(XIAOTAI_ROLE_PRIMARY, "按下讲话");
    assert(a && !a->value && a->state == XIAOTAI_STATE_HOLD_FROM_POINTER);
    assert(held_callback(1, 0, true) == XIAOTAI_ERR_INVALID_STATE);
    flush();
    assert(fixture.toasts == 1 && !strcmp(fixture.toast, "尚未就绪或麦克风已关闭") && !releases && !presses);

    snapshot.generation = 11;
    emit(XIAOTAI_EVENT_ROOM_STATE, NULL, 0);
    deny_press = true;
    assert(held_callback(2, 11, true) == XIAOTAI_ERR_INVALID_STATE);
    flush();
    assert(fixture.toasts == 2 && !releases && !presses);
    assert(held_callback(3, 10, true) == XIAOTAI_ERR_INVALID_STATE);
    flush();
    assert(fixture.toasts == 2 && !releases && !presses);
    deny_press = false;
    assert(held_callback(4, 11, true) == XIAOTAI_OK);
    emit(XIAOTAI_EVENT_ROOM_VOICE, NULL, 0);
    assert(find_action(XIAOTAI_ROLE_PRIMARY, "松开结束讲话"));
    assert(held_callback(3, 11, false) == XIAOTAI_OK && snapshot.transmitting);
    assert(held_callback(4, 11, false) == XIAOTAI_OK && !snapshot.transmitting);
    assert(held_callback(4, 11, false) == XIAOTAI_OK && releases == 1);
    emit(XIAOTAI_EVENT_ROOM_VOICE, NULL, 0);
    assert(find_action(XIAOTAI_ROLE_PRIMARY, "按下讲话") && fixture.toasts == 2);
    printf("pointer room notices=%u capture=%u exact_open_to_close=%u\n", fixture.toasts, presses, releases);
}

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    if (argc == 1) pointer();
    else if (!strcmp(argv[1], "room_options")) options();
    else if (!strcmp(argv[1], "room_keys")) keys();
    else if (!strcmp(argv[1], "room_local_hold")) physical_hold();
    else if (!strcmp(argv[1], "room_pointer_hold")) pointer_hold_feedback();
    else {
        assert(!strcmp(argv[1], "room_plain"));
        boot(XIAOTAI_NEED_KEY, false, false);
        assert(!xiaotai_intent_exists(XIAOTAI_INTENT_APP_COMPANION_ROOM) && !fixture.views && !attends);
    }
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&fence);
    return 0;
}
