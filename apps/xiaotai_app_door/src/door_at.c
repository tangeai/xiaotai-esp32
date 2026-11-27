// SPDX-License-Identifier: MIT
#include "door_internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/net.h"
#include "xiaotai/ui.h"

#include <inttypes.h>

typedef enum {
    STATUS = 1, HOME, NEXT, PREVIOUS, VIEWER, SENDER, DEVICES, BACK, ACCESS, DIAL, DOORBELL,
} control_t;

static void display_status(xiaotai_ui_info_t *out)
{
    *out = (xiaotai_ui_info_t){0};
    (void)xiaotai_ui_inspect(0, out);
}

static bool setup_active(void)
{
    if (door.setup || xiaotai_provisioning_active()) return true;
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    if (net.prepare.stage != XIAOTAI_NET_PREPARE_NONE && net.prepare.stage != XIAOTAI_NET_PREPARE_READY) return true;
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    return binding.phase >= XIAOTAI_BINDING_NEEDED;
}

static bool idle_control(const xiaotai_ui_info_t *display)
{
    if (setup_active() || door.overlay || display->overlay[0] || door.session || door_hunt_active() || door_look_running() ||
        door.stream_generation ||
        (door.foreground && door.page == DOOR_ASSISTANT) ||
        door_fallback_busy() || xiaotai_activity_current(NULL)) return false;
    xiaotai_call_status_t call;
    xiaotai_call_status(&call);
    return !call.session;
}

static const char *page_name(const xiaotai_ui_info_t *display)
{
    if (door.foreground) {
        switch (door.page) {
        case DOOR_HOME: return door.access ? "keypad" : "door";
        case DOOR_DEVICES: return "devices";
        case DOOR_MONITOR: return "monitor";
        case DOOR_DIRECTORY: return "directory";
        case DOOR_MODE: return "mode";
        case DOOR_SETTINGS: return "settings";
        case DOOR_LOG: return "log";
        case DOOR_CALL:
        case DOOR_OTHER_CALL: return "call";
        default: return "other";
        }
    }
    if (display->page[0]) {
        switch (display->page_kind) {
        case XIAOTAI_VIEW_HOME:
        case XIAOTAI_VIEW_TALK: return "face";
        case XIAOTAI_VIEW_CLOCK: return "clock";
        case XIAOTAI_VIEW_CALL: return "call";
        default: break;
        }
    }
    return "other";
}

static bool set_role(xiaotai_app_door_mode_t mode)
{
    return door.foreground && door.page == DOOR_HOME && !xiaotai_quiesced() &&
           xiaotai_setting_set_int(XIAOTAI_SETTING_APP_DOOR_SENDER, mode) == XIAOTAI_OK &&
           xiaotai_setting_set_int(XIAOTAI_SETTING_APP_DOOR_CHOSEN, 1) == XIAOTAI_OK;
}

static void door_role_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (args->value < XIAOTAI_APP_DOOR_INDOOR || args->value > XIAOTAI_APP_DOOR_BELL_INDOOR) return;
    xiaotai_ui_info_t display;
    display_status(&display);
    if (idle_control(&display) && set_role((xiaotai_app_door_mode_t)args->value)) xiaotai_ui_activity();
}

static bool apply_control(control_t control, const char *text, const xiaotai_ui_info_t *display)
{
    if (control == STATUS) return true;
    if (!idle_control(display)) return false;
    xiaotai_ui_activity();
    switch (control) {
    case HOME:
        if (!door.foreground) return xiaotai_app_switch("door") == XIAOTAI_OK;
        door_page(DOOR_HOME);
        return true;
    case NEXT:
    case PREVIOUS:
        return xiaotai_app_page_step(control == NEXT ? 1 : -1) == XIAOTAI_OK;
    case VIEWER:
    case SENDER:
    case ACCESS:
    case DOORBELL:
        return set_role(control == VIEWER ? XIAOTAI_APP_DOOR_INDOOR : control == SENDER ? XIAOTAI_APP_DOOR_BELL :
                        control == ACCESS ? XIAOTAI_APP_DOOR_ACCESS : XIAOTAI_APP_DOOR_BELL_INDOOR);
    case DIAL:
        return door_access_call(text);
    case DEVICES:
        if (!door.foreground || door.page != DOOR_HOME) return false;
        door_devices_intent(NULL, NULL);
        return true;
    case BACK:
        if (!door.foreground || (door.page != DOOR_DEVICES && door.page != DOOR_MONITOR && door.page != DOOR_DIRECTORY &&
            door.page != DOOR_MODE && door.page != DOOR_SETTINGS && door.page != DOOR_LOG)) return false;
        door_back_intent(NULL, NULL);
        return true;
    default:
        return false;
    }
}

static const char *role_name(const xiaotai_app_door_status_t *status)
{
    return status->mode == XIAOTAI_APP_DOOR_ACCESS ? "access" : status->sender ? "sender" :
           status->mode == XIAOTAI_APP_DOOR_BELL_INDOOR ? "doorbell" : "viewer";
}

static xiaotai_err_t run_control(control_t control, const char *text, const xiaotai_reply_t *reply)
{
    xiaotai_ui_info_t display;
    display_status(&display);
    const bool accepted = apply_control(control, text, &display);
    if (control != STATUS) {
        display_status(&display);
        if (accepted) xiaotai_eventlog_note("door", "control.accepted", 0, control);
        else xiaotai_eventlog_warn("door", "control.refused", 0, control);
    }
    xiaotai_app_door_status_t status;
    xiaotai_app_door_status(&status);
    xiaotai_at_printf(reply, "+DOOR: control=%u accepted=%u page=%s role=%s peer_selected=%u save_pending=%u "
                     "awake=%u setup=%u lock_configured=%u", (unsigned)control, (unsigned)accepted,
                     page_name(&display), role_name(&status), (unsigned)status.station_selected,
                     (unsigned)status.save_pending, (unsigned)display.awake, (unsigned)setup_active(),
                     (unsigned)status.lock_configured);
    /* The request parsed successfully; accepted records the result of the action. */
    return XIAOTAI_OK;
}

#define COMMAND(symbol, control) \
    static xiaotai_err_t symbol(int argc, const char *const *argv, const xiaotai_reply_t *reply) \
    { (void)argc; (void)argv; return run_control(control, NULL, reply); }
COMMAND(at_status, STATUS)
COMMAND(at_home, HOME)
COMMAND(at_next, NEXT)
COMMAND(at_previous, PREVIOUS)
COMMAND(at_viewer, VIEWER)
COMMAND(at_sender, SENDER)
COMMAND(at_devices, DEVICES)
COMMAND(at_back, BACK)
COMMAND(at_access, ACCESS)
COMMAND(at_doorbell, DOORBELL)
#undef COMMAND

static xiaotai_err_t at_dial(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    return argc == 1 ? run_control(DIAL, argv[0], reply) : XIAOTAI_ERR_INVALID_ARG;
}

static void diagnostics(const xiaotai_reply_t *reply)
{
    xiaotai_app_door_status_t status;
    xiaotai_app_door_status(&status);
    xiaotai_at_printf(reply, "foreground=%u page=%s presenting_call=%u overlay=%u role=%s peer_selected=%u "
                     "save_pending=%u lock_configured=%u state=%u setup=%u",
                     (unsigned)status.foreground, status.page[0] ? status.page : "-",
                     (unsigned)status.presenting_call, (unsigned)status.overlay, role_name(&status),
                     (unsigned)status.station_selected, (unsigned)status.save_pending, (unsigned)status.lock_configured,
                     (unsigned)status.state, (unsigned)setup_active());
    xiaotai_at_printf(reply, "+APP-DOOR-COUNTS: door_calls=%" PRIu32 " yielded=%" PRIu32 " opens=%" PRIu32
                     " opens_done=%" PRIu32 " opens_received=%" PRIu32 " looks=%" PRIu32 " missed=%" PRIu32 " peers=%u",
                     status.door_calls, status.yielded, status.opens, status.opens_done, status.opens_received,
                     status.looks, status.missed, (unsigned)status.peers);
}

xiaotai_err_t door_init_at(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = at_status}, {.word = "HOME", .run = at_home}, {.word = "NEXT", .run = at_next},
        {.word = "PREVIOUS", .run = at_previous}, {.word = "VIEWER", .run = at_viewer}, {.word = "SENDER", .run = at_sender},
        {.word = "DEVICES", .run = at_devices}, {.word = "BACK", .run = at_back}, {.word = "ACCESS", .run = at_access},
        {.word = "DOORBELL", .run = at_doorbell},
        {.word = "DIAL", .run = at_dial, .min_args = 1, .max_args = 1, .usage = "<room>"},
    };
    xiaotai_err_t error = xiaotai_at_verbs_register("AT+DOOR", verbs, sizeof(verbs) / sizeof(*verbs));
    if (error == XIAOTAI_OK) error = xiaotai_diag_register("app.door", diagnostics);
    if (error == XIAOTAI_OK)
        error = xiaotai_intent_handle_ex(XIAOTAI_INTENT_APP_DOOR_ROLE, door_role_intent, NULL, XIAOTAI_INTENT_LOCAL);
    return error;
}
