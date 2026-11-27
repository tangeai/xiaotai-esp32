// SPDX-License-Identifier: MIT
/* The board's main key while the door shows: at the door it is the bell, and
 * on both sides it answers and ends a call, as on a telephone. A board without
 * such a key has nothing to press and nothing changes. */
#include "door_internal.h"

#include "xiaotai/interaction.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/ui.h"

#define KEY_INTENT XIAOTAI_INTENT_APP_DOOR_KEY
#define DECLINE 1

static void post(const char *intent, uint32_t session)
{
    const xiaotai_args_t args = {.value = (int32_t)session};
    (void)xiaotai_intent_post(intent, &args);
}

static void on_key(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!door.foreground || door.setup || (door.overlay && !door.session)) return;
    xiaotai_ui_activity();
    xiaotai_call_status_t call;
    xiaotai_call_status(&call);
    const bool mine = door.session && call.session == door.session;
    if (mine && call.state == XIAOTAI_CALL_INCOMING) {
        post(args->value == DECLINE ? "door.call.reject" : "door.call.accept", call.session);
    } else if (mine && door_look_is(call.session) && door_look_rung()) {
        /* The visitor rings while the door shows: answer, or let it go. */
        post(args->value == DECLINE ? XIAOTAI_INTENT_APP_DOOR_LOOK_IGNORE : XIAOTAI_INTENT_APP_DOOR_LOOK_TALK, call.session);
    } else if (args->value == DECLINE) {
        return;
    } else if (mine && door_look_seen(call.session) && !door_look_seen_talking()) {
        post(XIAOTAI_INTENT_APP_DOOR_RING, 0);          /* somebody looks: the visitor asks for an answer */
    } else if (mine && door_look_is(call.session)) {
        post(XIAOTAI_INTENT_APP_DOOR_LOOK_END, call.session);
    } else if (door.session || door_hunt_active()) {
        post("door.call.hangup", door.session);
    } else if (door.page == DOOR_HOME && door.access && !door.peer_count) {
        xiaotai_view_toast("还没有选管理处", 0);
    } else if (door.page == DOOR_HOME && door.peer_count) {
        post(door.sender ? XIAOTAI_INTENT_APP_DOOR_RING : XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);
    }
}

xiaotai_err_t door_init_key(void)
{
    static const xiaotai_interaction_rule_t rules[] = {
        {.role = "main", .gesture = XIAOTAI_GESTURE_CLICK, .count = 1, .intent = KEY_INTENT},
        {.role = "main", .gesture = XIAOTAI_GESTURE_LONG_PRESS, .intent = KEY_INTENT, .value = DECLINE},
    };
    static const xiaotai_interaction_profile_t profile = {.name = "door-key", .rules = rules,
        .rule_count = sizeof(rules) / sizeof(rules[0]), .application = "door"};
    const xiaotai_err_t err = xiaotai_intent_handle(KEY_INTENT, on_key, NULL);
    return err == XIAOTAI_OK ? xiaotai_interaction_register(&profile) : err;
}
