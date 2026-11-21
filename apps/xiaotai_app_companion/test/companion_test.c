// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_ui.c::ui
// Replaces legacy: tests/regression/test_voice_reliability.py::test_navigation_priority
#include "fakes.h"

#include <assert.h>
#include <string.h>

#include "xiaotai/port/os.h"

static xiaotai_signal_t fence;
static unsigned other_foregrounds;
static bool navigation_pages;

static void barrier(void *unused) { (void)unused; xiaotai_signal_give(&fence); }

static void flush(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        assert(xiaotai_run(barrier, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

#ifdef COMPANION_TEST_PAGES
static void send(const char *intent, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_intent_post(intent, &args) == XIAOTAI_OK);
    flush();
}
#endif

static void other_foreground(bool on)
{
    if (on) ++other_foregrounds;
}

static xiaotai_err_t boot(uint32_t needs, bool another, const char *home)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = companion_fixture_init};
    static const xiaotai_module_t *const modules[] = {&probe};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground, .claim = xiaotai_app_companion_claim};
    static const xiaotai_application_t other = {.name = "launcher", .foreground = other_foreground};
    static const xiaotai_application_t *const apps[] = {&app, &other};
    fixture = (companion_test_fixture_t){.needs = needs, .navigation_pages = navigation_pages};
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    other_foregrounds = 0;
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1,
        .applications = apps, .application_count = another ? 2U : 1U, .home = home, .available_needs = needs};
    xiaotai_err_t start = xiaotai_framework_start(&config);
    if (start == XIAOTAI_OK) flush();
    return start;
}

static void stop(void)
{
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
}

#ifdef COMPANION_TEST_PAGES
static void status_probe(void *ctx) { xiaotai_app_companion_status(ctx); }

static xiaotai_app_companion_status_t status(void)
{
    xiaotai_app_companion_status_t result;
    assert(xiaotai_run(status_probe, &result) == XIAOTAI_OK);
    flush();
    return result;
}

static void assert_page(const char *page)
{
    assert(!strcmp(fixture.id, page) && !strcmp(status().page, page));
}

static void talk_navigation(void)
{
    const uint32_t pointer = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH;
    navigation_pages = true;
    assert(boot(pointer, false, NULL) == XIAOTAI_OK);
    assert_page("home");
    send(XIAOTAI_INTENT_APP_COMPANION_HOME, NULL, 1);
    assert_page("home");
    assert(fixture.kind == XIAOTAI_VIEW_CLOCK && !fixture.talk_stops);
    const char *keep[] = {"status", "call", "result"};
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); ++i) {
        send(XIAOTAI_INTENT_APP_COMPANION_SHOW, keep[i], 0);
        assert_page(keep[i]);
        assert(!fixture.talk_stops);
    }
    send(XIAOTAI_INTENT_APP_COMPANION_HOME, NULL, 0);
    send(XIAOTAI_INTENT_SYSTEM_MENU, NULL, 0);
    assert_page("menu");
    assert(!fixture.talk_stops);
    fixture.fail_talk_stop = true;
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "contacts", 0);
    assert_page("menu");
    assert(!fixture.talk_stops && fixture.last_refusal.value == XIAOTAI_ERR_NO_MEM);
    fixture.fail_talk_stop = false;
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "contacts", 0);
    assert_page("contacts");
    assert(fixture.talk_stops == 1);
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert_page("menu");
    assert(fixture.talk_stops == 1);
    const char *end[] = {"contact", "emotions", "emotion", "settings", "network", "room", "room.form"};
    for (size_t i = 0; i < sizeof(end) / sizeof(end[0]); ++i) {
        send(XIAOTAI_INTENT_APP_COMPANION_HOME, NULL, 0);
        send(XIAOTAI_INTENT_APP_COMPANION_SHOW, end[i], 0);
        assert_page(end[i]);
        assert(fixture.talk_stops == i + 2U);
    }
    stop();

    const xiaotai_app_companion_config_t cfg = {.room_keeps_talk = true};
    assert(xiaotai_app_companion_configure(&cfg) == XIAOTAI_OK);
    assert(boot(pointer, false, NULL) == XIAOTAI_OK);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "room", 0);
    assert_page("room");
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "room.form", 0);
    assert_page("room.form");
    assert(!fixture.talk_stops);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "contact", 0);
    assert_page("contact");
    assert(fixture.talk_stops == 1);
    stop();
    navigation_pages = false;
}

static void pages(void)
{
    talk_navigation();
    assert(!strcmp(xiaotai_app_companion_parts[0]->name, "contacts"));
    assert(!strcmp(xiaotai_app_companion_parts[1]->name, "room"));
    assert(xiaotai_app_companion_parts[2] == NULL);
    assert(boot(XIAOTAI_NEED_KEY | XIAOTAI_NEED_DISPLAY, false, NULL) == XIAOTAI_OK);
    assert_page("home");
    assert(fixture.item_count == 4);
    assert(!strcmp(fixture.items[0].label, "WeChat contacts"));
    assert(!strcmp(fixture.items[1].label, "Device contacts"));
    assert(!strcmp(fixture.items[2].label, "Group room"));
    assert(!strcmp(fixture.items[3].label, "设置"));
    const int32_t entry = fixture.items[1].value;
    const char *action = fixture.items[1].intent;
    send(action, NULL, entry);
    assert_page("contacts");
    assert(companion.stack[1].arg == XIAOTAI_APP_COMPANION_CONTACTS_DEVICE);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "contact", 37);
    assert_page("contact");
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert_page("contacts");
    assert(companion.stack[1].arg == XIAOTAI_APP_COMPANION_CONTACTS_DEVICE);
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert_page("home");
    assert(fixture.focus == 2); /* restore the selected row in the source menu */
    send(action, NULL, entry);
    assert_page("home");
    assert(status().stale == 1);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "recognition", 0);
    assert_page("home");
    assert(fixture.last_refusal.value == XIAOTAI_ERR_NOT_FOUND && !strcmp(fixture.last_refusal.text, "recognition"));
    assert(!strcmp(fixture.toast, "操作未执行") && fixture.toast_ms == 1500);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "contacts", -1);
    assert_page("home");
    assert(fixture.last_refusal.value == XIAOTAI_ERR_INVALID_ARG);
    for (int32_t i = 1; i < (int32_t)COMPANION_STACK_MAX; ++i) {
        send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "contact", i);
    }
    assert(companion.depth == COMPANION_STACK_MAX);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "contact", 9);
    assert(fixture.last_refusal.value == XIAOTAI_ERR_NO_MEM && companion.stack[companion.depth - 1].arg == 5);
    for (unsigned i = 1; i < COMPANION_STACK_MAX; ++i) send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert_page("home");
    assert(fixture.page_events >= 12);
    stop();

    assert(boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH, true, "launcher") == XIAOTAI_OK);
    assert(other_foregrounds == 1 && fixture.views == 0 && !status().foreground);
    send(XIAOTAI_INTENT_APP_COMPANION_HOME, NULL, 1);
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(fixture.views == 0 && status().stale == 2);
    send(XIAOTAI_INTENT_APP_COMPANION_MAIN, NULL, 1);
    assert(fixture.main_requests == 1 && fixture.main_arg == 1 && fixture.views == 0);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "talk", 0);
    assert(!status().foreground && fixture.views == 0 && fixture.toasts == 0);
    send(XIAOTAI_INTENT_SYSTEM_MENU, NULL, 0);
    assert_page("menu");
    assert(fixture.item_count == 6);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "home", 0);
    assert_page("home");
    assert(fixture.kind == XIAOTAI_VIEW_HOME);
    send(XIAOTAI_INTENT_APP_COMPANION_HOME, NULL, 1);
    assert_page("home");
    assert(fixture.kind == XIAOTAI_VIEW_CLOCK);
    send(XIAOTAI_INTENT_APP_COMPANION_HOME, NULL, 1);
    assert(!status().foreground && other_foregrounds == 2);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "room", 0);
    assert_page("room");
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert_page("home");
    assert(fixture.kind == XIAOTAI_VIEW_CLOCK);
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(!status().foreground && other_foregrounds == 3);
    stop();
}
#else
static void style(void)
{
    static const xiaotai_app_companion_style_t local = {.name = "dial", .needs_all = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_ENCODER};
    static const struct {uint32_t needs; const char *name;} shapes[] = {
        {XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH | XIAOTAI_NEED_KEY, "pointer"},
        {XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY, "keys"},
        {XIAOTAI_NEED_KEY, "plain"},
        {XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH, "pointer"},
    };
    assert(xiaotai_app_companion_parts[0] == NULL);
    for (size_t i = 0; i < sizeof(shapes) / sizeof(*shapes); ++i) {
        assert(boot(shapes[i].needs, false, NULL) == XIAOTAI_OK);
        assert(!strcmp(companion.style->name, shapes[i].name));
        assert(fixture.profile && !strcmp(fixture.profile->application, "companion") && fixture.feedback);
        assert(!strcmp(fixture.context, "idle"));
        assert(fixture.item_count == (i == 1 ? 1 : 0)); /* settings is required, other entries are optional */
        if (i == 1) {
            assert(fixture.profile->multi_click_ms == 350 && fixture.profile->long_press_ms == 1200);
            assert(fixture.profile->flags == (XIAOTAI_INTERACTION_VOID_ON_VIEW | XIAOTAI_INTERACTION_VOID_ON_CONTEXT));
            assert(!strcmp(fixture.profile->rules[2].context, "setup"));
            assert(!strcmp(fixture.profile->rules[2].intent, XIAOTAI_INTENT_APP_COMPANION_BACK));
            for (size_t j = 0; j < fixture.feedback->effect_count; ++j) {
                assert(fixture.feedback->effects[j].sound.flags & XIAOTAI_FEEDBACK_WAIT);
            }
        }
        assert(xiaotai_app_companion_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_app_companion_style_register(&local) == XIAOTAI_ERR_INVALID_STATE);
        stop();
    }
    const xiaotai_app_companion_config_t bad = {.announce_repeat_s = 3601};
    assert(xiaotai_app_companion_configure(&bad) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_app_companion_style_register(&local) == XIAOTAI_OK);
    assert(xiaotai_app_companion_style_register(&local) == XIAOTAI_ERR_INVALID_STATE);
    assert(boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_ENCODER, false, NULL) == XIAOTAI_OK && companion.style == &local);
    stop();
    assert(xiaotai_app_companion_style_register(&local) == XIAOTAI_OK);
    const uint32_t dial = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH | XIAOTAI_NEED_ENCODER;
    assert(boot(dial, false, NULL) == XIAOTAI_OK && companion.style == &local);
    stop();
    assert(xiaotai_app_companion_style_register(&local) == XIAOTAI_OK);
    const xiaotai_app_companion_config_t named = {.style = "pointer", .title = "Test Product"};
    assert(xiaotai_app_companion_configure(&named) == XIAOTAI_OK);
    assert(boot(dial, false, NULL) == XIAOTAI_OK && !strcmp(companion.style->name, "pointer"));
    assert(!strcmp(companion.config.title, "Test Product"));
    stop();
    const xiaotai_app_companion_config_t unknown = {.style = "no-such-style"};
    assert(xiaotai_app_companion_configure(&unknown) == XIAOTAI_OK);
    assert(boot(dial, false, NULL) == XIAOTAI_ERR_NOT_FOUND && fixture.views == 0);
    stop();
    assert(xiaotai_app_companion_configure(&named) == XIAOTAI_OK);
    assert(boot(XIAOTAI_NEED_KEY, false, NULL) == XIAOTAI_ERR_NOT_SUPPORTED);
    stop();
    assert(boot(XIAOTAI_NEED_DISPLAY, false, NULL) == XIAOTAI_ERR_NOT_SUPPORTED);
    stop();
    assert(!strcmp(xiaotai_app_companion_emotion(0), "neutral"));
    assert(!strcmp(xiaotai_app_companion_emotion(21), "listening"));
    assert(!strcmp(xiaotai_app_companion_emotion(22), "ambient"));
    assert(xiaotai_app_companion_emotion(23) == NULL);
    for (size_t i = 0; i < XIAOTAI_APP_COMPANION_EMOTIONS; ++i) {
        const char *name = xiaotai_app_companion_emotion(i);
        assert(!strcmp(xiaotai_app_companion_emotion_face(name), name));
    }
    assert(!strcmp(xiaotai_app_companion_emotion_face("兴奋"), "excited"));
    assert(!strcmp(xiaotai_app_companion_emotion_face("不明"), "neutral"));
    assert(!strcmp(xiaotai_app_companion_emotion_face(NULL), "neutral"));
}

static void switch_app(void *name) { assert(xiaotai_app_switch(name) == XIAOTAI_OK); }

static void plain(void)
{
    /* Plain reuses the exact voice-guidance table, not a product-specific
     * subset or a second version of the WAIT/group policy. */
    assert(boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY, false, NULL) == XIAOTAI_OK);
    const xiaotai_feedback_effect_t *guide = fixture.feedback->effects;
    const size_t guide_count = fixture.feedback->effect_count;
    assert(guide_count == 12U && fixture.views);
    stop();

    assert(boot(XIAOTAI_NEED_KEY, true, "launcher") == XIAOTAI_OK);
    assert(!strcmp(companion.style->name, "plain") && !companion.status.foreground);
    assert(!companion.style->home_entries && !companion.style->menu_entries);
    assert(companion.style->policy == XIAOTAI_APP_COMPANION_FOLLOW_SESSIONS);
    assert(!companion.has_display && !companion.buffer);
    assert(!fixture.views && !fixture.toasts && !fixture.page_events);

    const xiaotai_interaction_profile_t *profile = fixture.profile;
    assert(profile && !strcmp(profile->name, "companion-plain") && !strcmp(profile->application, "companion"));
    assert(profile->rule_count == 2U);
    const char *const intents[] = {XIAOTAI_INTENT_APP_COMPANION_MAIN, XIAOTAI_INTENT_APP_COMPANION_END};
    for (size_t n = 0; n < profile->rule_count; ++n) {
        const xiaotai_interaction_rule_t *rule = &profile->rules[n];
        assert(!strcmp(rule->role, "main") && rule->gesture == XIAOTAI_GESTURE_CLICK &&
               rule->count == n + 1U && !rule->context && !strcmp(rule->intent, intents[n]) && !rule->value);
    }
    const xiaotai_feedback_table_t *feedback = fixture.feedback;
    assert(feedback && feedback->effects == guide && feedback->effect_count == guide_count);
    for (size_t n = 0; n < feedback->effect_count; ++n) {
        assert(feedback->effects[n].sound.name && (feedback->effects[n].sound.flags & XIAOTAI_FEEDBACK_WAIT));
    }
    const struct {const char *name; uint8_t kind; const char *ring;} rows[] = {
        {XIAOTAI_APP_COMPANION_STATE_SETUP, XIAOTAI_LIGHT_BLINK, NULL},
        {XIAOTAI_APP_COMPANION_STATE_BINDING, XIAOTAI_LIGHT_BREATHE, NULL},
        {XIAOTAI_APP_COMPANION_STATE_OFFLINE, XIAOTAI_LIGHT_BLINK, NULL},
        {XIAOTAI_APP_COMPANION_STATE_ERROR, XIAOTAI_LIGHT_FLASHES, NULL},
        {XIAOTAI_APP_COMPANION_STATE_CALL, XIAOTAI_LIGHT_SOLID, NULL},
        {XIAOTAI_APP_COMPANION_STATE_INCOMING, XIAOTAI_LIGHT_ALTERNATE, "ring.incoming"},
        {XIAOTAI_APP_COMPANION_STATE_OUTGOING, XIAOTAI_LIGHT_BREATHE, "ring.outgoing"},
        {XIAOTAI_APP_COMPANION_STATE_TALK, XIAOTAI_LIGHT_BREATHE, NULL},
        {XIAOTAI_APP_COMPANION_STATE_ONLINE, XIAOTAI_LIGHT_SOLID, NULL},
    };
    assert(feedback->state_count == sizeof(rows) / sizeof(rows[0]));
    for (size_t n = 0; n < feedback->state_count; ++n) {
        const xiaotai_feedback_state_t *row = &feedback->states[n];
        assert(!strcmp(row->name, rows[n].name) && row->pattern.kind == rows[n].kind);
        assert(!n || feedback->states[n - 1U].priority > row->priority);
        assert(!row->light && !row->output_count && row->pattern.level_max);
        if (row->pattern.kind != XIAOTAI_LIGHT_SOLID) assert(row->pattern.period_ms);
        if (rows[n].ring) assert(row->sound.name && !strcmp(row->sound.name, rows[n].ring) && !row->sound.gap_ms);
        else assert(!row->sound.name);
    }

    assert(xiaotai_run(switch_app, "companion") == XIAOTAI_OK);
    flush();
    assert(companion.status.foreground && !strcmp(fixture.context, "idle"));
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_MAIN, NULL) == XIAOTAI_OK);
    flush();
    /* No session adapters in this target: report it, never fabricate one. */
    assert(fixture.refused == 1U && fixture.last_refusal.value == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(xiaotai_run(switch_app, "launcher") == XIAOTAI_OK);
    flush();
    assert(!companion.status.foreground);
    assert(xiaotai_run(switch_app, "companion") == XIAOTAI_OK);
    flush();
    assert(companion.status.foreground && !companion.status.page[0]);
    assert(!fixture.views && !fixture.toasts && !fixture.page_events && !companion.status.pages);
    stop();

    const xiaotai_app_companion_config_t forced = {.style = XIAOTAI_APP_COMPANION_STYLE_PLAIN};
    assert(xiaotai_app_companion_configure(&forced) == XIAOTAI_OK);
    assert(boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY, false, NULL) == XIAOTAI_ERR_NOT_SUPPORTED);
    stop();
}
#endif

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
#ifdef COMPANION_TEST_PAGES
    (void)argc;
    (void)argv;
    pages();
#else
    if (argc == 2 && !strcmp(argv[1], "plain")) plain();
    else {
        assert(argc == 1);
        style();
    }
#endif
    xiaotai_signal_deinit(&fence);
    return 0;
}
