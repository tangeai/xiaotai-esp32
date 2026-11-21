// SPDX-License-Identifier: MIT
#include "terminal_internal.h"

#include "xiaotai/feedback.h"
#include "xiaotai/interaction.h"

#define STATE(n) XIAOTAI_APP_VOICE_TERMINAL_STATE_##n
#define EFFECT(n) XIAOTAI_APP_VOICE_TERMINAL_EFFECT_##n
#define INTENT(n) XIAOTAI_INTENT_APP_VOICE_TERMINAL_##n
#define N(a) (sizeof(a) / sizeof((a)[0]))

#define MAIN_RULE {.role = "main", .gesture = XIAOTAI_GESTURE_CLICK, .count = 1, .intent = INTENT(MAIN), \
                   .value = TERMINAL_GESTURE_VALUE}
#define END_RULE {.role = "main", .gesture = XIAOTAI_GESTURE_CLICK, .count = 2, .intent = INTENT(END), \
                  .value = TERMINAL_GESTURE_VALUE}
#define BIND_RULE {.role = "main", .gesture = XIAOTAI_GESTURE_LONG_PRESS, .intent = INTENT(RESET_BINDING)}
static const xiaotai_interaction_rule_t gestures[] = {
    MAIN_RULE, END_RULE,
    {.role = "main", .gesture = XIAOTAI_GESTURE_CLICK, .count = 4, .intent = INTENT(RESET_NETWORK)},
    BIND_RULE,
};
/* Keep the four-click window even when the reset action is absent. */
static const xiaotai_interaction_rule_t no_network[] = {MAIN_RULE, END_RULE,
    {.role = "main", .gesture = XIAOTAI_GESTURE_CLICK, .count = 4}, BIND_RULE};
#define PROFILE(rows,n) {.name = "terminal-key", .application = "voice_terminal", .rules = rows, .rule_count = n, \
    .multi_click_ms = XIAOTAI_APP_VOICE_TERMINAL_MULTI_CLICK_MS, .long_press_ms = XIAOTAI_APP_VOICE_TERMINAL_LONG_PRESS_MS}
static const xiaotai_interaction_profile_t profiles[] = {
    PROFILE(gestures, N(gestures)), PROFILE(no_network, N(no_network)), PROFILE(gestures, 3), PROFILE(no_network, 3),
};

#define BREATHE(r,g,b,lo,hi,ms) .kind = XIAOTAI_LIGHT_BREATHE, .color = {r,g,b}, \
    .level_min = lo, .level_max = hi, .period_ms = ms
#define CHASE(r,g,b,ar,ag,ab,ms) .kind = XIAOTAI_LIGHT_CHASE, .color = {r,g,b}, \
    .accent = {ar,ag,ab}, .period_ms = ms
#define BLINK(r,g,b,lo,hi,ms) .kind = XIAOTAI_LIGHT_BLINK, .color = {r,g,b}, \
    .level_min = lo, .level_max = hi, .period_ms = ms
#define ALTERNATE(r,g,b,ms) .kind = XIAOTAI_LIGHT_ALTERNATE, .color = {r,g,b}, .level_max = 255, .period_ms = ms
#define SOLID(r,g,b,l) .kind = XIAOTAI_LIGHT_SOLID, .color = {r,g,b}, .level_max = l
#define WARNING BLINK(255,138,0,55,255,80)

static const xiaotai_feedback_state_t states[] = {
    {.name = STATE(SETUP), .priority = 11, .pattern = {BREATHE(25,115,255,45,220,1200),
        .accent = {230,250,255}, .accent_mode = XIAOTAI_LIGHT_ACCENT_DOT, .accent_level = 255, .accent_ms = 180}},
    {.name = STATE(CLOSING), .priority = 10, .pattern = {BLINK(255,48,22,28,170,110)}},
    {.name = STATE(ACTIVE), .priority = 9, .pattern = {BREATHE(28,255,74,70,175,1400),
        .accent = {180,255,215}, .accent_mode = XIAOTAI_LIGHT_ACCENT_CENTER, .accent_level = 210}},
    {.name = STATE(LINKING), .priority = 8, .pattern = {CHASE(255,215,35,0,170,255,95)}},
    {.name = STATE(INCOMING), .priority = 7, .pattern = {ALTERNATE(20,255,92,220)}},
    {.name = STATE(OUTGOING), .priority = 6, .pattern = {CHASE(0,235,255,20,80,255,75)}},
    {.name = STATE(ERROR), .priority = 5, .pattern = {.kind = XIAOTAI_LIGHT_FLASHES, .color = {255,18,12},
        .count = 2, .level_max = 255, .accent_ms = 120, .period_ms = 1300}},
    {.name = STATE(BOOT), .priority = 4, .pattern = {CHASE(80,210,255,0,50,180,90)}},
    {.name = STATE(BINDING), .priority = 3, .pattern = {BREATHE(255,118,8,35,210,1800)}},
    {.name = STATE(READY), .priority = 2, .pattern = {BREATHE(0,210,155,28,90,2600)}},
    {.name = STATE(CONNECTING), .priority = 1, .pattern = {CHASE(50,125,255,20,30,170,140)}},
    {.name = STATE(RING), .sound = {.name = "ring.incoming", .gap_ms = 560}},
    {.name = STATE(RINGBACK), .sound = {.name = "ring.outgoing", .gap_ms = 760}},
};
enum { CONFIRM_EFFECT = 8, CONFIRM_AUDIO_EFFECT };
static xiaotai_feedback_effect_t effects[] = {
    {.name = EFFECT(TAP), .pattern = {SOLID(110,245,255,210)}, .duration_ms = 140},
    {.name = EFFECT(DOUBLE_TAP), .pattern = {ALTERNATE(255,45,20,70)}, .duration_ms = 320},
    {.name = EFFECT(BLOCKED), .event = XIAOTAI_EVENT_APP_VOICE_TERMINAL_BLOCKED,
        .pattern = {WARNING}, .duration_ms = 360},
    {.name = EFFECT(REFUSED), .event = XIAOTAI_EVENT_APP_VOICE_TERMINAL_REFUSED,
        .pattern = {WARNING}, .duration_ms = 360},
    {.name = EFFECT(CONNECTED), .pattern = {SOLID(210,255,225,255)}, .duration_ms = 420},
    {.name = EFFECT(ENDED), .sound = {.name = "cue.ended"}},
    {.name = EFFECT(FAILED), .pattern = {WARNING}, .duration_ms = 360, .sound = {.name = "cue.warning"}},
    {.name = EFFECT(SETUP), .sound = {.name = "guide.wifi_setup"}},
    [CONFIRM_EFFECT] = {.name = EFFECT(CONFIRM), .pattern = {SOLID(210,255,225,255)}, .duration_ms = 420,
        .sound = {.name = "cue.success"}},
    [CONFIRM_AUDIO_EFFECT] = {.name = TERMINAL_CONFIRM_AUDIO, .sound = {.name = "cue.success"}},
};
static const xiaotai_feedback_table_t feedback = {
    .states = states, .state_count = N(states), .effects = effects, .effect_count = N(effects),
};

xiaotai_err_t terminal_tables_init(void)
{
    const uint8_t flags = terminal_config.skip_busy_confirmation ? 0U : XIAOTAI_FEEDBACK_WAIT;
    effects[CONFIRM_EFFECT].sound.flags = effects[CONFIRM_AUDIO_EFFECT].sound.flags = flags;
    const unsigned selection = (terminal_config.no_network_reset ? 1U : 0U) +
        (terminal_config.no_binding_reset ? 2U : 0U);
    const xiaotai_err_t err = xiaotai_interaction_register(&profiles[selection]);
    return err == XIAOTAI_OK ? xiaotai_feedback_register(&feedback) : err;
}
