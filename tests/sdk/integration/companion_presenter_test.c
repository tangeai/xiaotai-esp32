// SPDX-License-Identifier: MIT
#include "xiaotai/apps/companion.h"
#include "xiaotai/apps/door.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/feedback.h"
#include "xiaotai/hal.h"
#include "xiaotai/interaction.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/lock.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai/view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s (app=%s entry=%d view=%s kind=%d)\n", \
            __FILE__, __LINE__, #condition, app ? app : "-", entry, page, kind); abort(); \
} } while (0)

static xiaotai_signal_t barrier;
static const char *app;
static xiaotai_app_entry_t entry;
static char page[48], text[96];
static xiaotai_view_kind_t kind;
static xiaotai_err_t result;
static xiaotai_net_status_t network = {.state = XIAOTAI_NET_UP};
static xiaotai_platform_status_t platform = {.phase = XIAOTAI_PLATFORM_ONLINE};
static xiaotai_provisioning_status_t provision;
static xiaotai_binding_status_t binding = {.phase = XIAOTAI_BINDING_BOUND};
static xiaotai_call_status_t call;
static xiaotai_ai_talk_status_t talk = {.started = true};
static bool key_fixture;
static uint32_t key_events;
static unsigned talk_starts, talk_stops, pages_shown;

static xiaotai_err_t key_open(void *unused) { (void)unused; return XIAOTAI_OK; }

static uint32_t key_poll(void *unused)
{
    (void)unused;
    const uint32_t events = key_events;
    key_events = 0;
    return events;
}

const xiaotai_input_t *__real_xiaotai_hal_input(size_t index);
const xiaotai_input_t *__wrap_xiaotai_hal_input(size_t index)
{
    static const xiaotai_input_t key = {.desc = {.kind = XIAOTAI_INPUT_BUTTON, .role = "main",
        .events = XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED | XIAOTAI_INPUT_CLICK | XIAOTAI_INPUT_DOUBLE_CLICK |
                  XIAOTAI_INPUT_LONG_PRESS}, .open = key_open, .poll = key_poll};
    return key_fixture ? (index ? NULL : &key) : __real_xiaotai_hal_input(index);
}

/* The actual recognizer runs on a synthetic clock, never alongside its timer. */
void interaction_poll(uint32_t now);
xiaotai_err_t __wrap_interaction_poll_start(void) { return XIAOTAI_OK; }

xiaotai_err_t __real_xiaotai_ai_talk_start(const xiaotai_ai_talk_start_t *options, uint32_t *session);
xiaotai_err_t __wrap_xiaotai_ai_talk_start(const xiaotai_ai_talk_start_t *options, uint32_t *session)
{
    if (!key_fixture) return __real_xiaotai_ai_talk_start(options, session);
    CHECK(!options && !talk.session);
    talk.session = ++talk_starts;
    talk.phase = XIAOTAI_AI_TALK_CONNECTING;
    if (session) *session = talk.session;
    return XIAOTAI_OK;
}

xiaotai_err_t __real_xiaotai_ai_talk_stop(uint32_t session);
xiaotai_err_t __wrap_xiaotai_ai_talk_stop(uint32_t session)
{
    if (!key_fixture) return __real_xiaotai_ai_talk_stop(session);
    CHECK(session && session == talk.session);
    ++talk_stops;
    talk.session = 0;
    talk.phase = XIAOTAI_AI_TALK_IDLE;
    return XIAOTAI_OK;
}

uint32_t __wrap_xiaotai_hal_needs(void) { return XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH; }
void __wrap_xiaotai_net_status(xiaotai_net_status_t *out) { *out = network; }
void __wrap_xiaotai_platform_status(xiaotai_platform_status_t *out) { *out = platform; }
void __wrap_xiaotai_provisioning_status(xiaotai_provisioning_status_t *out) { *out = provision; }
void __wrap_xiaotai_binding_status(xiaotai_binding_status_t *out) { *out = binding; }
void __wrap_xiaotai_call_status(xiaotai_call_status_t *out) { *out = call; }
void __wrap_xiaotai_ai_talk_status(xiaotai_ai_talk_status_t *out) { *out = talk; }
/* Host door builds may include the optional relay adapter; WT99 does not. */
size_t xiaotai_lock_count(void) { return 0; }
xiaotai_err_t xiaotai_lock_open(uint8_t index, uint32_t duration)
{
    (void)index;
    (void)duration;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static void view(const xiaotai_view_t *shown, void *unused)
{
    (void)unused;
    if (shown->layer != XIAOTAI_LAYER_PAGE) return;
    ++pages_shown;
    snprintf(page, sizeof(page), "%s", shown->id);
    snprintf(text, sizeof(text), "%s", shown->text ? shown->text : "");
    kind = shown->kind;
}

static xiaotai_err_t init_fixture(void)
{
    const xiaotai_err_t err = xiaotai_engine_ui_init();
    if (err == XIAOTAI_OK) xiaotai_view_set_sink(view, NULL);
    return err;
}

static void inspect(void *unused)
{
    (void)unused;
    app = xiaotai_app_foreground();
    entry = xiaotai_app_entry();
    xiaotai_signal_give(&barrier);
}

static void sync(void)
{
    CHECK(xiaotai_run(inspect, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&barrier, 2000));
}

static void intent(const char *name, int32_t value)
{
    const xiaotai_args_t args = {.value = value};
    CHECK(xiaotai_intent_post(name, &args) == XIAOTAI_OK);
    sync();
}

static void emit(const char *name, const xiaotai_args_t *args, const void *data, size_t size)
{
    CHECK(xiaotai_event_emit(name, args, data, size) == XIAOTAI_OK);
    sync();
}

static void show(const char *expected_app, const char *expected_page)
{
    CHECK(app && !strcmp(app, expected_app));
    CHECK(!strcmp(page, expected_page));
}

static void switch_app(void *ctx) { result = xiaotai_app_switch(ctx); }

static void set_door_peer(void *peer)
{
    result = xiaotai_setting_set_text(XIAOTAI_SETTING_APP_DOOR_PEER, peer);
}

static void page_step(void *ctx) { result = xiaotai_app_page_step(*(const int *)ctx); }

static void step(int direction)
{
    CHECK(xiaotai_run(page_step, &direction) == XIAOTAI_OK);
    sync();
    CHECK(result == XIAOTAI_OK);
}

/* This package tests application routing. The complete generated WT99 product
 * (all selected engines, adapters and applications) is tested by BI-3. */
static const char *const door_handles[] = {
    XIAOTAI_EVENT_CALL_INCOMING, XIAOTAI_EVENT_CALL_STATE, XIAOTAI_EVENT_CALL_CONNECTED, XIAOTAI_EVENT_CALL_ENDED, NULL,
};
static const char *const companion_handles[] = {
    XIAOTAI_EVENT_CALL_INCOMING, XIAOTAI_EVENT_CALL_STATE, XIAOTAI_EVENT_CALL_CONNECTED, XIAOTAI_EVENT_CALL_ENDED,
    XIAOTAI_EVENT_AI_TALK_STATE, XIAOTAI_EVENT_PROVISIONING_CHANGED, XIAOTAI_EVENT_PROVISIONING_OPENED,
    XIAOTAI_EVENT_PROVISIONING_DONE, XIAOTAI_EVENT_PROVISIONING_FAILED, XIAOTAI_EVENT_BINDING_CHANGED,
    XIAOTAI_EVENT_BINDING_CODE, XIAOTAI_EVENT_BINDING_FAILED, XIAOTAI_EVENT_BINDING_BOUND, NULL,
};
static const xiaotai_application_t door = {
    .name = "door", .init = xiaotai_app_door_init, .start = xiaotai_app_door_start,
    .foreground = xiaotai_app_door_foreground, .page_step = xiaotai_app_door_page_step,
    .handles = door_handles, .claim = xiaotai_app_door_claim,
};
static const xiaotai_application_t companion = {
    .name = "companion", .init = xiaotai_app_companion_init, .start = xiaotai_app_companion_start,
    .foreground = xiaotai_app_companion_foreground, .page_step = xiaotai_app_companion_page_step,
    .handles = companion_handles, .claim = xiaotai_app_companion_claim,
};

static void start_key_fixture(void)
{
    if (key_fixture) xiaotai_engine_interaction_start();
}

static void start(bool dual)
{
    static const xiaotai_module_t fixture = {.name = "fixture", .init = init_fixture};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init};
    static const xiaotai_module_t feedback = {.name = "feedback", .init = xiaotai_engine_feedback_init};
    static const xiaotai_module_t interaction = {.name = "interaction", .init = xiaotai_engine_interaction_init,
        .start = start_key_fixture};
    static const xiaotai_module_t *const modules[] = {&at, &feedback, &interaction, &fixture};
    static const xiaotai_application_t *const applications[] = {&door, &companion};
    const xiaotai_framework_config_t config = {
        .modules = modules, .module_count = sizeof(modules) / sizeof(modules[0]),
        .applications = dual ? applications : applications + 1,
        .application_count = dual ? 2U : 1U,
    };
    CHECK(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&barrier) == XIAOTAI_OK);
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    sync();
    show(dual ? "door" : "companion", dual ? XIAOTAI_APP_DOOR_PAGE_HOME : XIAOTAI_APP_COMPANION_PAGE_HOME);
}

static void stop(void)
{
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_app_door_reset();
    xiaotai_ui_reset();
    xiaotai_interaction_reset();
    xiaotai_feedback_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&barrier);
}

static void ai(uint32_t session)
{
    talk.session = session;
    talk.phase = session ? XIAOTAI_AI_TALK_LISTENING : XIAOTAI_AI_TALK_IDLE;
    emit(XIAOTAI_EVENT_AI_TALK_STATE, NULL, &talk, sizeof(talk));
}

static void pairing(xiaotai_provisioning_phase_t phase)
{
    provision.phase = phase;
    ++provision.revision;
    const xiaotai_args_t args = {.value = phase};
    emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, &args, &provision, sizeof(provision));
}

static void bind(bool bound)
{
    binding.phase = bound ? XIAOTAI_BINDING_BOUND : XIAOTAI_BINDING_WAITING;
    strcpy(binding.verification, bound ? "" : "123456");
    binding.seconds_left = bound ? 0 : 60;
    ++binding.revision;
    const xiaotai_args_t args = {.value = binding.phase};
    emit(XIAOTAI_EVENT_BINDING_CHANGED, &args, &binding, sizeof(binding));
}

static void call_case(void)
{
    CHECK(xiaotai_run(set_door_peer, "station") == XIAOTAI_OK);
    sync();
    CHECK(result == XIAOTAI_OK);
    call = (xiaotai_call_status_t){.session = 1, .state = XIAOTAI_CALL_INCOMING, .peer_id = "station"};
    emit(XIAOTAI_EVENT_CALL_INCOMING, NULL, &call, sizeof(call));
    show("door", XIAOTAI_APP_DOOR_PAGE_CALL);
    call.session = 0;
    call.state = XIAOTAI_CALL_IDLE;
    call.end = XIAOTAI_CALL_END_PEER_HANGUP;
    emit(XIAOTAI_EVENT_CALL_ENDED, NULL, &call, sizeof(call));
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);

    call = (xiaotai_call_status_t){.session = 2, .state = XIAOTAI_CALL_INCOMING, .peer_id = "visitor"};
    emit(XIAOTAI_EVENT_CALL_INCOMING, NULL, &call, sizeof(call));
    show("companion", XIAOTAI_APP_COMPANION_PAGE_CALL);
    CHECK(entry == XIAOTAI_APP_ENTRY_PRESENT);
    call.session = 0;
    call.state = XIAOTAI_CALL_IDLE;
    call.end = XIAOTAI_CALL_END_PEER_HANGUP;
    emit(XIAOTAI_EVENT_CALL_ENDED, NULL, &call, sizeof(call));
    show("companion", XIAOTAI_APP_COMPANION_PAGE_RESULT);
    intent(XIAOTAI_INTENT_APP_COMPANION_END, 0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    CHECK(entry == XIAOTAI_APP_ENTRY_RETURN);
    emit(XIAOTAI_EVENT_CALL_STATE, NULL, &call, sizeof(call));
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
}

static void talk_case(void)
{
    ai(0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    ai(11);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME);
    CHECK(entry == XIAOTAI_APP_ENTRY_PRESENT);
    xiaotai_ai_talk_status_t stale = talk;
    stale.session = 0;
    emit(XIAOTAI_EVENT_AI_TALK_STATE, NULL, &stale, sizeof(stale));
    show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME);
    ai(0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    CHECK(entry == XIAOTAI_APP_ENTRY_RETURN);

    CHECK(xiaotai_run(switch_app, "companion") == XIAOTAI_OK);
    sync();
    CHECK(result == XIAOTAI_OK);
    ai(12);
    ai(0);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME);
    CHECK(entry == XIAOTAI_APP_ENTRY_SWITCH);
    intent(XIAOTAI_INTENT_APP_COMPANION_HOME, 0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);

    ai(13);
    intent(XIAOTAI_INTENT_APP_COMPANION_HOME, 0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    CHECK(entry == XIAOTAI_APP_ENTRY_RETURN);
    ai(0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
}

static void setup_case(void)
{
    pairing(XIAOTAI_PROVISIONING_NONE);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    pairing(XIAOTAI_PROVISIONING_WAITING);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_SETUP);
    CHECK(entry == XIAOTAI_APP_ENTRY_PRESENT);
    pairing(XIAOTAI_PROVISIONING_NONE);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    CHECK(entry == XIAOTAI_APP_ENTRY_RETURN);
}

static void binding_case(void)
{
    bind(true);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    binding.phase = XIAOTAI_BINDING_REPORTING;
    ++binding.revision;
    const xiaotai_args_t requesting = {.value = binding.phase};
    emit(XIAOTAI_EVENT_BINDING_CHANGED, &requesting, &binding, sizeof(binding));
    show("door", "door.binding");
    bind(false);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_BINDING);
    CHECK(!strcmp(text, "123456") && entry == XIAOTAI_APP_ENTRY_PRESENT);
    bind(true);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    CHECK(entry == XIAOTAI_APP_ENTRY_RETURN);

    binding.phase = XIAOTAI_BINDING_WAITING;
    strcpy(binding.verification, "123456");
    binding.seconds_left = 45;
    const xiaotai_args_t stale = {.value = 45, .text = "765432"};
    emit(XIAOTAI_EVENT_BINDING_CODE, &stale, NULL, 0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    const xiaotai_args_t code = {.value = 45, .text = "123456"};
    emit(XIAOTAI_EVENT_BINDING_CODE, &code, NULL, 0);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_BINDING);
    CHECK(!strcmp(text, "123456") && entry == XIAOTAI_APP_ENTRY_PRESENT);
    bind(true);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
}

static void overlap_case(void)
{
    ai(31);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME);
    pairing(XIAOTAI_PROVISIONING_WAITING);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_SETUP);
    ai(0);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_SETUP);
    binding.phase = XIAOTAI_BINDING_WAITING;
    strcpy(binding.verification, "654321");
    binding.seconds_left = 30;
    pairing(XIAOTAI_PROVISIONING_NONE);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_BINDING);
    bind(true);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);

    ai(32);
    pairing(XIAOTAI_PROVISIONING_WAITING);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_SETUP);
    pairing(XIAOTAI_PROVISIONING_NONE);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME);
    CHECK(entry == XIAOTAI_APP_ENTRY_PRESENT);
    ai(0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
}

static void navigation_case(void)
{
    step(1);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME);
    CHECK(kind == XIAOTAI_VIEW_HOME && entry == XIAOTAI_APP_ENTRY_NEXT);
    step(1);
    CHECK(kind == XIAOTAI_VIEW_CLOCK);
    step(1);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    step(-1);
    CHECK(kind == XIAOTAI_VIEW_CLOCK && entry == XIAOTAI_APP_ENTRY_PREVIOUS);
    step(-1);
    CHECK(kind == XIAOTAI_VIEW_HOME);
    step(-1);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    intent(XIAOTAI_INTENT_SYSTEM_MENU, 0);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_MENU);
    intent(XIAOTAI_INTENT_APP_COMPANION_HOME, 0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
    intent(XIAOTAI_INTENT_SYSTEM_NETWORK, 0);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_NETWORK);
    intent(XIAOTAI_INTENT_APP_COMPANION_HOME, 0);
    show("door", XIAOTAI_APP_DOOR_PAGE_HOME);
}

static void standalone_case(void)
{
    ai(9);
    ai(0);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME);
    pairing(XIAOTAI_PROVISIONING_WAITING);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_SETUP);
    pairing(XIAOTAI_PROVISIONING_NONE);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME);
    step(-1);
    CHECK(kind == XIAOTAI_VIEW_CLOCK);
    step(1);
    CHECK(kind == XIAOTAI_VIEW_HOME);
}

static void sample_key(void *ctx) { interaction_poll(*(const uint32_t *)ctx); }

static void key_at(uint32_t now, uint32_t events, unsigned starts, unsigned stops)
{
    const bool unchanged = talk_starts == starts && talk_stops == stops;
    const unsigned before = pages_shown;
    key_events = events;
    CHECK(xiaotai_run(sample_key, &now) == XIAOTAI_OK);
    sync();
    sync();
    CHECK(talk_starts == starts && talk_stops == stops);
    if (unchanged) CHECK(pages_shown == before);
    show("companion", XIAOTAI_APP_COMPANION_PAGE_HOME);
}

static void key_case(bool on_press)
{
    if (!on_press) {
        /* The PMIC delivers a finished click. That remains the default. */
        key_at(0, 0, 0, 0);
        key_at(320, XIAOTAI_INPUT_CLICK, 1, 0);
        key_at(520, XIAOTAI_INPUT_DOUBLE_CLICK, 1, 0);
        key_at(660, XIAOTAI_INPUT_CLICK, 1, 1);
        return;
    }
    /* Board test checks these edges against the actual active-low GPIO. */
    key_at(310, 0, 0, 0);
    key_at(320, XIAOTAI_INPUT_PRESSED, 1, 0);
    ai(1);
    key_at(550, XIAOTAI_INPUT_RELEASED, 1, 0);
    key_at(650, 0, 1, 0);
    key_at(660, XIAOTAI_INPUT_PRESSED, 1, 1);
    key_at(780, XIAOTAI_INPUT_RELEASED, 1, 1);
    /* Isolate the connecting-stop path from the app's pending-start guard. */
    talk.session = 9;
    talk.phase = XIAOTAI_AI_TALK_CONNECTING;
    emit(XIAOTAI_EVENT_AI_TALK_STATE, NULL, &talk, sizeof(talk));
    key_at(890, 0, 1, 1);
    key_at(900, XIAOTAI_INPUT_PRESSED, 1, 2);
    key_at(2300, 0, 1, 2);
    key_at(2460, XIAOTAI_INPUT_RELEASED, 1, 2);
    key_at(2900, XIAOTAI_INPUT_CLICK | XIAOTAI_INPUT_DOUBLE_CLICK | XIAOTAI_INPUT_LONG_PRESS, 1, 2);
}

int main(int argc, char **argv)
{
    const char *name = argc == 2 ? argv[1] : "talk";
    const bool on_press = !strcmp(name, "key_press");
    key_fixture = on_press || !strcmp(name, "key_click");
    if (key_fixture) CHECK(xiaotai_app_companion_configure(&(xiaotai_app_companion_config_t){
        .main_button_on_press = on_press}) == XIAOTAI_OK);
    start(!key_fixture && strcmp(name, "standalone") != 0);
    if (!strcmp(name, "talk")) talk_case();
    else if (!strcmp(name, "call")) call_case();
    else if (!strcmp(name, "setup")) setup_case();
    else if (!strcmp(name, "binding")) binding_case();
    else if (!strcmp(name, "overlap")) overlap_case();
    else if (!strcmp(name, "navigation")) navigation_case();
    else if (!strcmp(name, "standalone")) standalone_case();
    else if (key_fixture) key_case(on_press);
    else CHECK(false);
    stop();
    return 0;
}
