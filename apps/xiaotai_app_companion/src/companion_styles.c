// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <stdio.h>
#include <string.h>

#include "xiaotai/modules/settings.h"

#define COUNT(a) (sizeof(a) / sizeof(*(a)))
#define CLICK(n, context_, intent_) {.role = "main", .gesture = XIAOTAI_GESTURE_CLICK, .count = n, .context = context_, .intent = intent_}
#define LONG(context_, intent_) {.role = "main", .gesture = XIAOTAI_GESTURE_LONG_PRESS, .context = context_, .intent = intent_}

#define PRESS(context_, intent_) {.role = "main", .gesture = XIAOTAI_GESTURE_PRESS, .context = context_, .intent = intent_}
#define SINGLE_CLICK(context_, intent_) CLICK(1, context_, intent_)
/* Ringing: a press answers, a long one declines; a running call: a press hangs
 * up. Both on the release, so a long press can be told apart. */
#define POINTER_RULES(MAIN) \
    MAIN(XIAOTAI_APP_COMPANION_CONTEXT_SETUP, NULL), MAIN(XIAOTAI_APP_COMPANION_CONTEXT_SESSION, NULL), \
    SINGLE_CLICK(XIAOTAI_APP_COMPANION_CONTEXT_RINGING, XIAOTAI_INTENT_APP_COMPANION_MAIN), \
    {.role = "main", .gesture = XIAOTAI_GESTURE_LONG_PRESS, .context = XIAOTAI_APP_COMPANION_CONTEXT_RINGING, \
     .intent = XIAOTAI_INTENT_APP_COMPANION_MAIN, .value = XIAOTAI_APP_COMPANION_MAIN_DECLINE}, \
    SINGLE_CLICK(XIAOTAI_APP_COMPANION_CONTEXT_CALL, XIAOTAI_INTENT_APP_COMPANION_MAIN), \
    MAIN(NULL, XIAOTAI_INTENT_APP_COMPANION_MAIN), \
    {.role = "volume_up", .gesture = XIAOTAI_GESTURE_CLICK, .count = 1, .intent = XIAOTAI_INTENT_AUDIO_VOLUME_UP}, \
    {.role = "volume_down", .gesture = XIAOTAI_GESTURE_CLICK, .count = 1, .intent = XIAOTAI_INTENT_AUDIO_VOLUME_DOWN}

static const xiaotai_interaction_rule_t pointer_rules[] = { POINTER_RULES(SINGLE_CLICK) };
/* ... and where a press counts at once, it does not count there too. */
static const xiaotai_interaction_rule_t pointer_press_rules[] = {
    PRESS(XIAOTAI_APP_COMPANION_CONTEXT_RINGING, NULL), PRESS(XIAOTAI_APP_COMPANION_CONTEXT_CALL, NULL),
    POINTER_RULES(PRESS),
};
static const xiaotai_interaction_rule_t key_rules[] = {
    CLICK(1, NULL, "ui.next"), CLICK(2, NULL, "ui.confirm"),
    {.role = "main", .gesture = XIAOTAI_GESTURE_LONG_PRESS, .context = XIAOTAI_APP_COMPANION_CONTEXT_SETUP,
     .intent = XIAOTAI_INTENT_APP_COMPANION_BACK, .value = COMPANION_SETUP_BACK},
    LONG(NULL, "ui.back"),
};
static const xiaotai_interaction_rule_t plain_rules[] = {
    CLICK(1, NULL, XIAOTAI_INTENT_APP_COMPANION_MAIN), CLICK(2, NULL, XIAOTAI_INTENT_APP_COMPANION_END),
};

static const xiaotai_interaction_profile_t pointer_profile = {
    .name = "comp-pointer", .rules = pointer_rules, .rule_count = COUNT(pointer_rules), .application = "companion",
};
static const xiaotai_interaction_profile_t pointer_press_profile = {
    .name = "comp-pointer", .rules = pointer_press_rules, .rule_count = COUNT(pointer_press_rules), .application = "companion",
};
static const xiaotai_interaction_profile_t keys_profile = {
    .name = "companion-keys", .rules = key_rules, .rule_count = COUNT(key_rules), .application = "companion",
    .multi_click_ms = 350, .long_press_ms = 1200, .flags = XIAOTAI_INTERACTION_VOID_ON_VIEW | XIAOTAI_INTERACTION_VOID_ON_CONTEXT,
};
static const xiaotai_interaction_profile_t plain_profile = {
    .name = "companion-plain", .rules = plain_rules, .rule_count = COUNT(plain_rules), .application = "companion",
};

static const xiaotai_feedback_state_t ring_states[] = {
    {.name = XIAOTAI_APP_COMPANION_STATE_INCOMING, .priority = 3, .sound = {.name = "ring.incoming"}},
    {.name = XIAOTAI_APP_COMPANION_STATE_OUTGOING, .priority = 2, .sound = {.name = "ring.outgoing"}},
};
static const xiaotai_feedback_effect_t pointer_effects[] = {
    {.name = XIAOTAI_APP_COMPANION_EFFECT_ACK_FEMALE, .sound = {.name = "ack.female"}},
    {.name = XIAOTAI_APP_COMPANION_EFFECT_ACK_MALE, .sound = {.name = "ack.male"}},
    {.name = XIAOTAI_APP_COMPANION_EFFECT_REFUSED, .event = XIAOTAI_EVENT_APP_COMPANION_REFUSED},
};
static const xiaotai_feedback_table_t pointer_feedback = {
    .states = ring_states, .state_count = COUNT(ring_states), .effects = pointer_effects, .effect_count = COUNT(pointer_effects),
};

#define CUE(effect, sound_, group_) {.name = effect, .sound = {.name = sound_, .flags = XIAOTAI_FEEDBACK_WAIT, .group = group_}}
#define GUIDANCE(effect, sound_) CUE(effect, sound_, XIAOTAI_APP_COMPANION_GROUP_GUIDANCE)
static const xiaotai_feedback_effect_t guide_effects[] = {
    GUIDANCE(XIAOTAI_APP_COMPANION_EFFECT_STARTUP, "guide.startup"),
    GUIDANCE(XIAOTAI_APP_COMPANION_EFFECT_SETUP, "guide.wifi_setup"),
    GUIDANCE(XIAOTAI_APP_COMPANION_EFFECT_CONNECTING, "guide.wifi_connecting"),
    GUIDANCE(XIAOTAI_APP_COMPANION_EFFECT_CONNECTED, "guide.wifi_connected"),
    CUE(XIAOTAI_APP_COMPANION_EFFECT_SETUP_FAILED, "guide.wifi_failed", 0),
    GUIDANCE(XIAOTAI_APP_COMPANION_EFFECT_BOUND, "guide.bound"),
    GUIDANCE(XIAOTAI_APP_COMPANION_EFFECT_ONLINE, "guide.online"),
    CUE(XIAOTAI_APP_COMPANION_EFFECT_OFFLINE, "guide.offline", 0),
    CUE(XIAOTAI_APP_COMPANION_EFFECT_INCOMING, "guide.incoming", 0),
    CUE(XIAOTAI_APP_COMPANION_EFFECT_DIALING, "guide.dialing", 0),
    CUE(XIAOTAI_APP_COMPANION_EFFECT_ENDED, "guide.ended", 0),
    CUE(XIAOTAI_APP_COMPANION_EFFECT_FAILED, "guide.failed", 0),
};
static const xiaotai_feedback_table_t keys_feedback = {.effects = guide_effects, .effect_count = COUNT(guide_effects)};
/* Color supplements the rhythm; a single-color output still distinguishes
 * the common waiting, idle and fault states. No board brightness lives here. */
static const xiaotai_feedback_state_t plain_states[] = {
    {.name = XIAOTAI_APP_COMPANION_STATE_SETUP, .priority = 90,
     .pattern = {.kind = XIAOTAI_LIGHT_BLINK, .period_ms = 360, .level_max = 215, .color = {255, 120, 0}}},
    {.name = XIAOTAI_APP_COMPANION_STATE_BINDING, .priority = 80,
     .pattern = {.kind = XIAOTAI_LIGHT_BREATHE, .period_ms = 1800, .level_min = 25, .level_max = 180,
                 .color = {255, 185, 18}}},
    {.name = XIAOTAI_APP_COMPANION_STATE_OFFLINE, .priority = 70,
     .pattern = {.kind = XIAOTAI_LIGHT_BLINK, .period_ms = 900, .level_max = 140, .color = {255, 30, 20}}},
    {.name = XIAOTAI_APP_COMPANION_STATE_ERROR, .priority = 60,
     .pattern = {.kind = XIAOTAI_LIGHT_FLASHES, .period_ms = 1400, .accent_ms = 120, .count = 2,
                 .level_max = 250, .color = {255, 12, 12}}},
    {.name = XIAOTAI_APP_COMPANION_STATE_CALL, .priority = 50,
     .pattern = {.kind = XIAOTAI_LIGHT_SOLID, .level_max = 220, .color = {20, 235, 160}}},
    {.name = XIAOTAI_APP_COMPANION_STATE_INCOMING, .priority = 45,
     .pattern = {.kind = XIAOTAI_LIGHT_ALTERNATE, .period_ms = 240, .level_max = 230, .color = {170, 55, 255}},
     .sound = {.name = "ring.incoming"}},
    {.name = XIAOTAI_APP_COMPANION_STATE_OUTGOING, .priority = 40,
     .pattern = {.kind = XIAOTAI_LIGHT_BREATHE, .period_ms = 840, .level_min = 30, .level_max = 245,
                 .color = {20, 125, 255}}, .sound = {.name = "ring.outgoing"}},
    {.name = XIAOTAI_APP_COMPANION_STATE_TALK, .priority = 30,
     .pattern = {.kind = XIAOTAI_LIGHT_BREATHE, .period_ms = 1250, .level_min = 55, .level_max = 210,
                 .color = {20, 200, 255}}},
    {.name = XIAOTAI_APP_COMPANION_STATE_ONLINE, .priority = 10,
     .pattern = {.kind = XIAOTAI_LIGHT_SOLID, .level_max = 60, .color = {15, 220, 90}}},
};
static const xiaotai_feedback_table_t plain_feedback = {
    .states = plain_states, .state_count = COUNT(plain_states),
    .effects = guide_effects, .effect_count = COUNT(guide_effects),
};

static const char *const pointer_menu[] = {
    "contacts", "contacts:2", "recents", "recognition", "room", "settings", "emotions", "network", "status", NULL,
};
static const char *const keys_home[] = {"talk", "contacts:2", "contacts:1", "room", "doorbell", "settings", NULL};

static const xiaotai_app_companion_style_t pointer = {
    .name = XIAOTAI_APP_COMPANION_STYLE_POINTER, .needs_all = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH,
    .interaction = &pointer_profile, .feedback = &pointer_feedback, .menu_entries = pointer_menu,
    .policy = XIAOTAI_APP_COMPANION_TALK_ON_HOME | XIAOTAI_APP_COMPANION_PAGES_END_TALK |
        XIAOTAI_APP_COMPANION_RESULT_ALWAYS | XIAOTAI_APP_COMPANION_OFFLINE_GATE | XIAOTAI_APP_COMPANION_CLOCK_HOME,
};
static const xiaotai_app_companion_style_t keys = {
    .name = XIAOTAI_APP_COMPANION_STYLE_KEYS, .needs_all = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY,
    .needs_none = XIAOTAI_NEED_TOUCH, .interaction = &keys_profile,
    .feedback = &keys_feedback, .home_entries = keys_home, .refused_text = "操作未执行", .toast_ms = 1500,
    .policy = XIAOTAI_APP_COMPANION_FOLLOW_SESSIONS | XIAOTAI_APP_COMPANION_HOME_NETWORK_STATUS |
              XIAOTAI_APP_COMPANION_KEY_CALL_HINTS,
};
static const xiaotai_app_companion_style_t plain = {
    .name = XIAOTAI_APP_COMPANION_STYLE_PLAIN, .needs_all = XIAOTAI_NEED_KEY, .needs_none = XIAOTAI_NEED_DISPLAY,
    .interaction = &plain_profile, .feedback = &plain_feedback, .policy = XIAOTAI_APP_COMPANION_FOLLOW_SESSIONS,
};
const xiaotai_app_companion_style_t *const companion_builtins[] = {&pointer, &keys, &plain, NULL};

const xiaotai_interaction_profile_t *companion_interaction_profile(const xiaotai_app_companion_style_t *style,
                                                                 bool main_button_on_press)
{
    return style == &pointer && main_button_on_press ? &pointer_press_profile : style->interaction;
}

/* Stored idle-expression indices match the deployed list. Keep their order. */
static const char *const emotions[XIAOTAI_APP_COMPANION_EMOTIONS] = {
    "neutral", "happy", "laughing", "funny", "sad", "angry", "crying", "loving", "embarrassed", "surprised", "shocked",
    "thinking", "winking", "cool", "relaxed", "delicious", "kissy", "confident", "sleepy", "silly", "confused",
    "listening", "ambient",
};

const char *xiaotai_app_companion_emotion(size_t index)
{
    return index < XIAOTAI_APP_COMPANION_EMOTIONS ? emotions[index] : NULL;
}

/* What the expression list calls each of them, in the same order. */
static const char *const emotion_labels[] = {
    "平静", "开心", "大笑", "搞怪", "难过", "生气", "大哭", "喜爱", "害羞", "惊讶", "震惊",
    "思考", "眨眼", "酷", "放松", "美味", "亲亲", "自信", "困倦", "调皮", "困惑", "聆听", "待机",
};
_Static_assert(COUNT(emotion_labels) == XIAOTAI_APP_COMPANION_EMOTIONS, "every expression has a label");

const char *companion_emotion_label(size_t index)
{
    return index < XIAOTAI_APP_COMPANION_EMOTIONS ? emotion_labels[index] : NULL;
}

const char *xiaotai_app_companion_emotion_face(const char *word)
{
    static const struct { const char *word, *theme; } words[] = {
        {"中性", "neutral"}, {"开心", "happy"}, {"兴奋", "excited"}, {"温和", "gentle"}, {"安慰", "comfort"},
        {"思考", "thinking"}, {"惊讶", "surprised"}, {"严肃", "serious"}, {"困倦", "sleepy"}, {"困惑", "confused"},
    };
    static const char *const aliases[] = {"calm", "excited", "curious", "proud", "moved", "speech",
        "gentle", "comfort", "serious"};
    if (word && word[0]) {
        for (size_t i = 0; i < COUNT(words); ++i) if (!strcmp(word, words[i].word)) return words[i].theme;
        /* The expression list's own names: a service may send any of them. */
        for (size_t i = 0; i < COUNT(emotions); ++i)
            if (!strcmp(word, emotions[i]) || !strcmp(word, emotion_labels[i])) return emotions[i];
        for (size_t i = 0; i < COUNT(aliases); ++i) if (!strcmp(word, aliases[i])) return aliases[i];
    }
    return emotions[0];
}

void companion_device_label(char *out, size_t capacity, const char *id)
{
    const size_t length = strlen(id);
    (void)snprintf(out, capacity, "设备 %s", length > 4U ? id + length - 4U : id);
}
