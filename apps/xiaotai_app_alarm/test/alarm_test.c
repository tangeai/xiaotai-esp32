// SPDX-License-Identifier: MIT
// The alarm application on the real framework, with the clock module, the UI
// and the feedback engine replaced by fakes in this file.
#include "xiaotai/apps/alarm.h"
#include "xiaotai/feedback.h"
#include "xiaotai/modules/clock.h"
#include "xiaotai/port/os.h"
#include "xiaotai/sounds.h"
#include "xiaotai/ui.h"
#include "xiaotai/view.h"
#include "xiaotai_os_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

/* ------------------------------------------------------------------- fakes */

static bool synced = true;
static xiaotai_clock_fields_t local = {.year = 2026, .month = 11, .day = 22, .hour = 21, .minute = 10, .weekday = 0};
static xiaotai_schedule_t entries[XIAOTAI_SCHEDULE_MAX];
static size_t entry_count;
static unsigned adds, updates, removes, wakeups;
static bool ringing;

bool xiaotai_wallclock_now(xiaotai_wallclock_t *out)
{
    *out = (xiaotai_wallclock_t){.synced = synced, .local = local};
    return synced;
}

static uint32_t due_in(const xiaotai_schedule_t *e)
{
    if (e->kind == XIAOTAI_SCHEDULE_COUNTDOWN) return e->seconds;
    const uint32_t now = (uint32_t)local.hour * 60U + local.minute, at = (uint32_t)e->hour * 60U + e->minute;
    return (at > now ? at - now : at + 1440U - now) * 60U;
}

xiaotai_err_t xiaotai_schedule_add(const xiaotai_schedule_t *entry, uint8_t *id)
{
    if (entry_count == XIAOTAI_SCHEDULE_MAX) return XIAOTAI_ERR_NO_MEM;
    entries[entry_count] = *entry;
    entries[entry_count].id = (uint8_t)(entry_count + 1U);
    if (id) *id = entries[entry_count].id;
    ++entry_count;
    ++adds;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_schedule_update(const xiaotai_schedule_t *entry)
{
    for (size_t i = 0; i < entry_count; ++i) {
        if (entries[i].id == entry->id) { entries[i] = *entry; ++updates; return XIAOTAI_OK; }
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_schedule_remove(uint8_t id)
{
    for (size_t i = 0; i < entry_count; ++i) {
        if (entries[i].id != id) continue;
        memmove(&entries[i], &entries[i + 1], (entry_count - i - 1) * sizeof(*entries));
        --entry_count;
        ++removes;
        return XIAOTAI_OK;
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

size_t xiaotai_schedule_list(xiaotai_schedule_info_t *out, size_t capacity)
{
    size_t n = 0;
    for (; n < entry_count && n < capacity; ++n) out[n] = (xiaotai_schedule_info_t){entries[n], due_in(&entries[n])};
    return n;
}

xiaotai_err_t xiaotai_schedule_get(uint8_t id, xiaotai_schedule_info_t *info)
{
    for (size_t i = 0; i < entry_count; ++i) {
        if (entries[i].id == id) { *info = (xiaotai_schedule_info_t){entries[i], due_in(&entries[i])}; return XIAOTAI_OK; }
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_schedule_next(xiaotai_schedule_info_t *info)
{
    size_t best = entry_count;
    for (size_t i = 0; i < entry_count; ++i) {
        if (entries[i].enabled && (best == entry_count || due_in(&entries[i]) < due_in(&entries[best]))) best = i;
    }
    if (best == entry_count) return XIAOTAI_ERR_NOT_FOUND;
    *info = (xiaotai_schedule_info_t){entries[best], due_in(&entries[best])};
    return XIAOTAI_OK;
}

static unsigned queue_full, again_calls;

xiaotai_err_t xiaotai_schedule_due_again(const xiaotai_schedule_t *entry)
{
    ++again_calls;
    if (queue_full) {
        --queue_full;
        return XIAOTAI_ERR_NO_MEM;
    }
    const xiaotai_args_t args = {.value = entry->id};
    return xiaotai_event_emit(XIAOTAI_EVENT_CLOCK_DUE, &args, entry, sizeof(*entry));
}

void xiaotai_ui_activity(void) { ++wakeups; }

static const xiaotai_sound_pack_t *sounds;
static bool sounding[2];

xiaotai_err_t xiaotai_sounds_register(const xiaotai_sound_pack_t *pack)
{
    sounds = pack;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_feedback_register(const xiaotai_feedback_table_t *table)
{
    CHECK(table->state_count == 2 && !strcmp(table->states[0].name, XIAOTAI_APP_ALARM_STATE_RINGING) &&
          !strcmp(table->states[1].name, XIAOTAI_APP_ALARM_STATE_TIMER));
    for (size_t i = 0; i < table->state_count; ++i) {
        CHECK(sounds && strcmp(table->states[i].sound.name, XIAOTAI_SOUND_RING_INCOMING));
        CHECK(!strcmp(table->states[i].sound.name, sounds->sounds[i].name) && sounds->sounds[i].size > 4000U);
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_feedback_set(const char *state, bool active)
{
    const bool timer = !strcmp(state, XIAOTAI_APP_ALARM_STATE_TIMER);
    CHECK(timer || !strcmp(state, XIAOTAI_APP_ALARM_STATE_RINGING));
    sounding[timer] = active;
    ringing = sounding[0] || sounding[1];
    CHECK(!(sounding[0] && sounding[1]));
    return XIAOTAI_OK;
}

/* ----------------------------------------------------------------- harness */

typedef struct {
    char id[32], text[96], toast[96], title[32];
    xiaotai_view_kind_t kind;
    xiaotai_view_field_t fields[XIAOTAI_VIEW_FIELDS_MAX];
    xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX];
    char rows[XIAOTAI_SCHEDULE_MAX][16], details[XIAOTAI_SCHEDULE_MAX][48];
    size_t field_count, action_count, item_count;
} shot_t;

static shot_t view;
static xiaotai_signal_t ready;
static const char *shown_app;
static uint32_t other_activity;

static void capture(const xiaotai_view_t *v, void *ctx)
{
    (void)ctx;
    if (v->layer == XIAOTAI_LAYER_TOAST) {
        (void)snprintf(view.toast, sizeof(view.toast), "%s", v->text ? v->text : "");
        return;
    }
    view.kind = v->kind;
    (void)snprintf(view.id, sizeof(view.id), "%s", v->id ? v->id : "");
    (void)snprintf(view.text, sizeof(view.text), "%s", v->text ? v->text : "");
    (void)snprintf(view.title, sizeof(view.title), "%s", v->title ? v->title : "");
    view.field_count = v->field_count;
    if (v->field_count) memcpy(view.fields, v->fields, v->field_count * sizeof(*v->fields));
    view.action_count = v->action_count;
    if (v->action_count) memcpy(view.actions, v->actions, v->action_count * sizeof(*v->actions));
    view.item_count = v->item_count;
    for (size_t i = 0; i < v->item_count && i < XIAOTAI_SCHEDULE_MAX; ++i) {
        (void)snprintf(view.rows[i], sizeof(view.rows[i]), "%s", v->items[i].label);
        (void)snprintf(view.details[i], sizeof(view.details[i]), "%s", v->items[i].detail);
    }
}

static void sync_job(void *ctx)
{
    (void)ctx;
    shown_app = xiaotai_app_foreground();
    xiaotai_signal_give(&ready);
}

static void sync(void)
{
    CHECK(xiaotai_run(sync_job, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&ready, 1000));
}

static void post(const char *intent, int32_t value)
{
    const xiaotai_args_t args = {.value = value};
    CHECK(xiaotai_intent_post(intent, &args) == XIAOTAI_OK);
    sync();
}

static void due(const xiaotai_schedule_t *entry)
{
    const xiaotai_args_t args = {.value = entry->id};
    CHECK(xiaotai_event_emit(XIAOTAI_EVENT_CLOCK_FIRED, &args, entry, sizeof(*entry)) == XIAOTAI_OK);
    CHECK(xiaotai_event_emit(XIAOTAI_EVENT_CLOCK_DUE, &args, entry, sizeof(*entry)) == XIAOTAI_OK);
    sync();
    sync();
}

static void other_revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)id; (void)reason; (void)ctx;
    other_activity = 0;
}

static void begin_other(void *ctx)
{
    (void)ctx;
    static const xiaotai_activity_t call = {.name = "call", .resources = XIAOTAI_RESOURCE_AUDIO_OUT,
                                            .revoked = other_revoked};
    CHECK(xiaotai_activity_begin(&call, &other_activity) == XIAOTAI_OK);
}

static void end_other(void *ctx)
{
    (void)ctx;
    xiaotai_activity_end(other_activity);
    other_activity = 0;
}

static void on_owner(void (*fn)(void *), void *ctx)
{
    CHECK(xiaotai_run(fn, ctx) == XIAOTAI_OK);
    sync();
}

static void status_job(void *ctx) { xiaotai_app_alarm_status(ctx); }

static xiaotai_app_alarm_status_t status(void)
{
    xiaotai_app_alarm_status_t s;
    on_owner(status_job, &s);
    return s;
}

static void home_foreground(bool foreground) { (void)foreground; }

static const char *const alarm_handles[] = {XIAOTAI_EVENT_CLOCK_DUE, NULL};
static const xiaotai_priority_t alarm_activities[] = {{XIAOTAI_APP_ALARM_ACTIVITY, 1}, {NULL, 0}};
static const xiaotai_application_t home_app = {.name = "home", .foreground = home_foreground};
static const xiaotai_application_t alarm_app = {
    .name = "alarm", .init = xiaotai_app_alarm_init, .start = xiaotai_app_alarm_start,
    .foreground = xiaotai_app_alarm_foreground, .handles = alarm_handles, .claim = xiaotai_app_alarm_claim,
    .activities = alarm_activities, .page_step = xiaotai_app_alarm_page_step,
};

/* The view sink is installed at init, as the UI engine does (xiaotai/view.h);
 * the clock module's time zone setting is registered the way it does when the
 * product offers it. */
static xiaotai_err_t fixture_init(void)
{
    xiaotai_view_set_sink(capture, NULL);
    const xiaotai_setting_t zone = {.name = XIAOTAI_SETTING_CLOCK_TIMEZONE, .key = "clock_tz",
                                    .kind = XIAOTAI_SETTING_TEXT, .default_text = ""};
    return xiaotai_setting_register(&zone);
}

static void start(uint16_t ring_s)
{
    static const xiaotai_module_t fixture = {.name = "fixture", .init = fixture_init};
    static const xiaotai_module_t *const modules[] = {&fixture};
    static const xiaotai_application_t *const apps[] = {&home_app, &alarm_app};
    static const xiaotai_priority_t priorities[] = {{"call", 3}, {NULL, 0}};
    const xiaotai_app_alarm_config_t config = {.ring_s = ring_s};
    CHECK(xiaotai_app_alarm_configure(&config) == XIAOTAI_OK);
    const xiaotai_framework_config_t framework = {
        .modules = modules, .module_count = 1,
        .applications = apps, .application_count = 2, .priorities = priorities,
    };
    entry_count = 0;
    adds = updates = removes = wakeups = 0;
    ringing = false;
    synced = true;
    memset(&view, 0, sizeof(view));
    CHECK(xiaotai_signal_init(&ready) == XIAOTAI_OK);
    CHECK(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    sync();
    CHECK(!strcmp(shown_app, "home"));
}

static void stop(void)
{
    xiaotai_framework_reset();
    xiaotai_app_alarm_reset();
    xiaotai_signal_deinit(&ready);
}

static const xiaotai_view_action_t *find_action(const char *intent)
{
    for (size_t i = 0; i < view.action_count; ++i) {
        if (view.actions[i].intent && !strcmp(view.actions[i].intent, intent)) return &view.actions[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------- cases */

/* The clock screen, the pager, a new alarm through the form, the list, edit, delete. */
static void pages_test(void)
{
    start(0);
    post(XIAOTAI_INTENT_APP_ALARM_HOME, 0);
    CHECK(!strcmp(shown_app, "alarm"));
    CHECK(view.kind == XIAOTAI_VIEW_CLOCK && !strcmp(view.id, XIAOTAI_APP_ALARM_PAGE_HOME));
    CHECK(!strcmp(view.text, "还没有闹钟") && view.item_count == 1);
    CHECK(!strcmp(view.rows[0], "下一个闹钟") && !strcmp(view.details[0], "还没有"));
    CHECK(find_action(XIAOTAI_INTENT_APP_ALARM_LIST) && find_action(XIAOTAI_INTENT_APP_ALARM_TIMER) &&
          find_action(XIAOTAI_INTENT_APP_ALARM_SETTINGS));

    post(XIAOTAI_INTENT_APP_ALARM_EDIT, 0);
    CHECK(view.kind == XIAOTAI_VIEW_FORM && !strcmp(view.title, "新闹钟") && view.field_count == 3);
    CHECK(view.fields[0].value == 7 && view.fields[1].value == 0 && view.fields[1].step == 5);
    CHECK(view.fields[2].value == XIAOTAI_APP_ALARM_REPEAT_DAILY && !find_action(XIAOTAI_INTENT_APP_ALARM_DELETE));
    post(XIAOTAI_INTENT_APP_ALARM_HOUR, 6);
    post(XIAOTAI_INTENT_APP_ALARM_MINUTE, 45);
    post(XIAOTAI_INTENT_APP_ALARM_REPEAT, XIAOTAI_APP_ALARM_REPEAT_WORKDAYS);
    post(XIAOTAI_INTENT_APP_ALARM_HOUR, 24);   /* out of range: ignored */
    CHECK(view.fields[0].value == 6 && view.fields[1].value == 45);
    post(XIAOTAI_INTENT_APP_ALARM_SAVE, 0);
    CHECK(adds == 1 && entry_count == 1 && entries[0].hour == 6 && entries[0].minute == 45);
    CHECK(entries[0].weekdays == 0x3E && entries[0].enabled && entries[0].kind == XIAOTAI_SCHEDULE_ALARM);
    CHECK(!strcmp(view.id, XIAOTAI_APP_ALARM_PAGE_LIST) && view.item_count == 1);
    CHECK(!strcmp(view.rows[0], "06:45") && !strcmp(view.details[0], "工作日 · 开"));
    CHECK(!strcmp(view.toast, "已设好 06:45 的闹钟"));

    post(XIAOTAI_INTENT_APP_ALARM_EDIT, entries[0].id);
    CHECK(!strcmp(view.title, "修改闹钟") && view.field_count == 4 && find_action(XIAOTAI_INTENT_APP_ALARM_DELETE));
    post(XIAOTAI_INTENT_APP_ALARM_ENABLED, 0);
    post(XIAOTAI_INTENT_APP_ALARM_SAVE, 0);
    CHECK(updates == 1 && !entries[0].enabled && !strcmp(view.details[0], "工作日 · 关"));

    post(XIAOTAI_INTENT_APP_ALARM_BACK, 0);
    CHECK(!strcmp(view.id, XIAOTAI_APP_ALARM_PAGE_HOME) && !strcmp(view.text, "还没有闹钟"));
    post(XIAOTAI_INTENT_APP_ALARM_EDIT, entries[0].id);
    post(XIAOTAI_INTENT_APP_ALARM_ENABLED, 1);
    post(XIAOTAI_INTENT_APP_ALARM_SAVE, 0);
    post(XIAOTAI_INTENT_APP_ALARM_BACK, 0);
    CHECK(!strcmp(view.text, "下一个闹钟 明天 06:45") && !strcmp(view.details[0], "明天 06:45"));

    synced = false;
    CHECK(xiaotai_event_emit(XIAOTAI_EVENT_CLOCK_MINUTE, NULL, NULL, 0) == XIAOTAI_OK);
    sync();
    CHECK(!strcmp(view.text, "时间同步后闹钟才会响"));
    synced = true;

    post(XIAOTAI_INTENT_APP_ALARM_LIST, 0);
    post(XIAOTAI_INTENT_APP_ALARM_EDIT, entries[0].id);
    post(XIAOTAI_INTENT_APP_ALARM_DELETE, 0);
    CHECK(removes == 1 && entry_count == 0 && !strcmp(view.id, XIAOTAI_APP_ALARM_PAGE_LIST) && view.item_count == 0);

    post(XIAOTAI_INTENT_APP_ALARM_BACK, 0);
    post(XIAOTAI_INTENT_APP_ALARM_HOME, -1);   /* the pager: back to the other application */
    CHECK(!strcmp(shown_app, "home"));
    stop();
}

/* Due while idle: brought forward, rings, stops on "stop", gives the screen back. */
static void ring_test(void)
{
    start(0);
    const xiaotai_schedule_t entry = {.id = 3, .kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true, .hour = 7,
                                      .minute = 30, .weekdays = XIAOTAI_SCHEDULE_EVERY_DAY};
    due(&entry);
    CHECK(!strcmp(shown_app, "alarm") && ringing && wakeups == 1);
    CHECK(view.kind == XIAOTAI_VIEW_CLOCK && !strcmp(view.id, XIAOTAI_APP_ALARM_PAGE_RINGING));
    CHECK(!strcmp(view.text, "07:30 的闹钟到了") && find_action(XIAOTAI_INTENT_APP_ALARM_STOP));
    post(XIAOTAI_INTENT_APP_ALARM_HOME, 1);   /* no paging away while it rings */
    CHECK(!strcmp(shown_app, "alarm") && ringing);
    xiaotai_app_alarm_status_t s = status();
    CHECK(s.ringing && s.ringing_id == 3 && s.rings == 1);

    const xiaotai_schedule_t countdown = {.id = 9, .kind = XIAOTAI_SCHEDULE_COUNTDOWN, .enabled = true,
                                          .seconds = 60, .label = "泡茶"};
    due(&countdown);   /* a second one while ringing replaces it */
    CHECK(!strcmp(view.text, "泡茶") && status().rings == 1 && status().ringing_id == 9);

    post(XIAOTAI_INTENT_APP_ALARM_STOP, 0);
    CHECK(!ringing && !strcmp(shown_app, "home"));
    s = status();
    CHECK(!s.ringing && s.stops == 1 && !s.foreground);
    stop();
}

/* Waits for the device to have been idle long enough for what is remembered. */
static void idle_delay(void)
{
    xiaotai_delay_ms(XIAOTAI_APP_ALARM_LATE_DELAY_MS + 300U);
    sync();
    sync();
}

static void advance_job(void *ctx) { xiaotai_os_host_advance_us(*(const int64_t *)ctx); }

static void advance_s(int64_t seconds)
{
    int64_t us = seconds * 1000000;
    on_owner(advance_job, &us);
}

static uint32_t ticket;

static void quiesce_job(void *ctx)
{
    if (ctx) CHECK(xiaotai_quiesce_begin("test", &ticket) == XIAOTAI_OK);
    else xiaotai_quiesce_end(ticket);
}

/* Due during a session: rings after it, in the order due, each stopped on its
 * own; not when another session follows at once, not what was removed, not
 * after an hour; also after maintenance. */
static void late_test(void)
{
    start(0);
    const xiaotai_schedule_t tea = {.id = 41, .kind = XIAOTAI_SCHEDULE_COUNTDOWN, .enabled = true, .seconds = 60,
                                    .label = "泡茶"};
    const xiaotai_schedule_t egg = {.id = 42, .kind = XIAOTAI_SCHEDULE_COUNTDOWN, .enabled = true, .seconds = 90,
                                    .label = "煮蛋"};
    on_owner(begin_other, NULL);
    due(&tea);
    advance_s(130);
    due(&egg);
    CHECK(!ringing && !strcmp(shown_app, "home") && status().pending == 2);
    on_owner(end_other, NULL);
    CHECK(!ringing);                       /* not at once: another session may follow */
    on_owner(begin_other, NULL);
    idle_delay();
    CHECK(!ringing && status().pending == 2 && status().rings == 0);
    on_owner(end_other, NULL);
    idle_delay();
    CHECK(ringing && !strcmp(shown_app, "alarm") && !strcmp(view.text, "泡茶（2 分钟前到点）"));
    CHECK(status().pending == 1 && status().late == 1 && status().rings == 1 && status().refused == 0);
    post(XIAOTAI_INTENT_APP_ALARM_STOP, 0);
    CHECK(ringing && !strcmp(shown_app, "alarm") && !strcmp(view.text, "煮蛋（刚才到点）"));
    post(XIAOTAI_INTENT_APP_ALARM_STOP, 0);   /* the second tap of a double one */
    post(XIAOTAI_INTENT_APP_ALARM_SNOOZE, 0);
    CHECK(ringing && status().stops == 1 && status().snoozes == 0);
    advance_s(1);
    post(XIAOTAI_INTENT_APP_ALARM_SNOOZE, 0);
    CHECK(!ringing && !strcmp(shown_app, "home") && !strcmp(entries[0].label, "煮蛋"));
    xiaotai_app_alarm_status_t s = status();
    CHECK(s.pending == 0 && s.late == 2 && s.rings == 2 && s.stops == 1 && s.snoozes == 1);

    /* On time again: no "ago"; an entry that fired on time is not made up for. */
    due(&tea);
    CHECK(ringing && !strcmp(view.text, "泡茶") && status().pending == 0);
    post(XIAOTAI_INTENT_APP_ALARM_STOP, 0);
    idle_delay();
    CHECK(!ringing && status().rings == 3 && !strcmp(shown_app, "home"));

    /* The event queue was full: asked again after the delay. */
    on_owner(begin_other, NULL);
    due(&egg);
    queue_full = 1;
    again_calls = 0;
    on_owner(end_other, NULL);
    idle_delay();
    CHECK(!ringing && again_calls == 1 && status().pending == 1);
    idle_delay();
    CHECK(ringing && again_calls == 2 && status().late == 3);
    post(XIAOTAI_INTENT_APP_ALARM_STOP, 0);

    /* Removed or switched off during the session; a one-time alarm is off by itself. */
    entry_count = 0;
    const xiaotai_schedule_t alarms[3] = {
        {.kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true, .hour = 7, .weekdays = XIAOTAI_SCHEDULE_EVERY_DAY},
        {.kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true, .hour = 8, .minute = 5},
        {.kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true, .hour = 9, .weekdays = 2},
    };
    for (size_t i = 0; i < 3; ++i) CHECK(xiaotai_schedule_add(&alarms[i], NULL) == XIAOTAI_OK);
    on_owner(begin_other, NULL);
    for (size_t i = 0; i < 3; ++i) due(&entries[i]);
    entries[1].enabled = false;            /* fired once */
    entries[2].enabled = false;            /* by the user */
    CHECK(xiaotai_schedule_remove(1) == XIAOTAI_OK);
    on_owner(end_other, NULL);
    idle_delay();
    CHECK(ringing && !strcmp(view.text, "08:05 的闹钟到了（刚才到点）") && status().pending == 0 && status().dropped == 2);
    post(XIAOTAI_INTENT_APP_ALARM_STOP, 0);
    CHECK(!ringing && !strcmp(shown_app, "home"));

    /* Over an hour late. */
    on_owner(begin_other, NULL);
    due(&tea);
    advance_s(XIAOTAI_APP_ALARM_LATE_MAX_S + 1);
    on_owner(end_other, NULL);
    idle_delay();
    s = status();
    CHECK(!ringing && s.pending == 0 && s.dropped == 3 && !strcmp(shown_app, "home"));

    /* Nothing can begin during maintenance; it rings when that is over. */
    on_owner(quiesce_job, &ticket);
    due(&tea);
    idle_delay();
    CHECK(!ringing && status().pending == 1 && status().refused == 1 && !strcmp(shown_app, "home"));
    on_owner(quiesce_job, NULL);
    idle_delay();
    CHECK(ringing && status().late == 5 && !strcmp(shown_app, "alarm"));
    on_owner(begin_other, NULL);           /* a session takes it over: heard, not made up for */
    on_owner(end_other, NULL);
    idle_delay();
    CHECK(!ringing && status().revoked == 1 && status().pending == 0);
    stop();
}

/* Due during another session: not claimed, rings after it; a call takes the device from a ringing alarm. */
static void busy_test(void)
{
    start(0);
    on_owner(begin_other, NULL);
    const xiaotai_schedule_t entry = {.id = 1, .kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true, .hour = 8};
    CHECK(xiaotai_schedule_add(&entry, NULL) == XIAOTAI_OK);
    due(&entry);
    CHECK(!strcmp(shown_app, "home") && !ringing && status().rings == 0 && status().pending == 1);
    on_owner(end_other, NULL);

    due(&entry);   /* again before it was made up for: once is enough */
    CHECK(status().pending == 0);
    CHECK(!strcmp(shown_app, "alarm") && ringing);
    on_owner(begin_other, NULL);   /* "call" ranks above "alarm" */
    CHECK(!ringing && status().revoked == 1 && !status().ringing);
    /* Nobody stopped it: once more after the call, and not a third time. */
    CHECK(status().pending == 1);
    on_owner(end_other, NULL);
    idle_delay();
    CHECK(ringing && !strcmp(shown_app, "alarm") && status().late == 1 && status().pending == 0);
    on_owner(begin_other, NULL);
    CHECK(!ringing && status().revoked == 2 && status().pending == 0);
    on_owner(end_other, NULL);
    idle_delay();
    CHECK(!ringing && status().late == 1);

    /* One that was stopped is done. */
    due(&entry);
    CHECK(ringing);
    post(XIAOTAI_INTENT_APP_ALARM_STOP, 0);
    on_owner(begin_other, NULL);
    on_owner(end_other, NULL);
    idle_delay();
    CHECK(!ringing && status().pending == 0);
    stop();
}

/* Rings at most the ring length (the shortest choice, 30 s). */
static void timeout_test(void)
{
    start(30);
    const xiaotai_schedule_t entry = {.id = 2, .kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true, .hour = 6};
    due(&entry);
    CHECK(ringing);
    xiaotai_delay_ms(30600);
    sync();
    CHECK(!ringing && status().timeouts == 1 && !strcmp(shown_app, "home"));
    stop();
}

static void config_test(void)
{
    const xiaotai_app_alarm_config_t too_short = {.ring_s = 4};
    const xiaotai_app_alarm_config_t not_a_choice = {.ring_s = 45};
    const xiaotai_app_alarm_config_t odd_snooze = {.snooze_min = 7};
    const xiaotai_app_alarm_config_t too_coarse = {.minute_step = 31};
    const xiaotai_app_alarm_config_t valid = {.ring_s = 120, .snooze_min = 10, .minute_step = 1};
    CHECK(xiaotai_app_alarm_configure(&too_short) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_app_alarm_configure(&not_a_choice) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_app_alarm_configure(&odd_snooze) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_app_alarm_configure(&too_coarse) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_app_alarm_configure(&valid) == XIAOTAI_OK);
    CHECK(xiaotai_app_alarm_configure(NULL) == XIAOTAI_OK);
}

static void zone_job(void *ctx)
{
    CHECK(xiaotai_setting_get_text(XIAOTAI_SETTING_CLOCK_TIMEZONE, ctx, XIAOTAI_SETTING_TEXT_MAX) == XIAOTAI_OK);
}

static void int_job(void *ctx)
{
    int32_t *v = ctx;
    CHECK(xiaotai_setting_get_int(v[0] ? XIAOTAI_SETTING_APP_ALARM_SNOOZE_MIN : XIAOTAI_SETTING_APP_ALARM_RING_S,
                                  &v[1]) == XIAOTAI_OK);
}

static int32_t setting(bool snooze)
{
    int32_t v[2] = {snooze, 0};
    on_owner(int_job, v);
    return v[1];
}

/* The countdown page and the settings page. */
static void timer_settings_test(void)
{
    start(0);
    post(XIAOTAI_INTENT_APP_ALARM_HOME, 0);
    post(XIAOTAI_INTENT_APP_ALARM_TIMER, 0);
    CHECK(!strcmp(view.id, XIAOTAI_APP_ALARM_PAGE_TIMER) && view.field_count == 1);
    CHECK(view.fields[0].kind == XIAOTAI_FIELD_STEPPER && view.fields[0].value == 5);
    post(XIAOTAI_INTENT_APP_ALARM_TIMER_MINUTES, 12);
    post(XIAOTAI_INTENT_APP_ALARM_TIMER_MINUTES, 181);   /* out of range: ignored */
    CHECK(view.fields[0].value == 12);
    post(XIAOTAI_INTENT_APP_ALARM_TIMER_START, 0);
    CHECK(adds == 1 && entries[0].kind == XIAOTAI_SCHEDULE_COUNTDOWN && entries[0].seconds == 720);
    CHECK(view.fields[0].kind == XIAOTAI_FIELD_TEXT && find_action(XIAOTAI_INTENT_APP_ALARM_TIMER_CANCEL));
    post(XIAOTAI_INTENT_APP_ALARM_BACK, 0);
    CHECK(!strcmp(view.id, XIAOTAI_APP_ALARM_PAGE_HOME) && !strcmp(view.text, "倒计时 还剩 12:00"));
    CHECK(view.item_count == 2 && !strcmp(view.rows[0], "倒计时") && !strcmp(view.details[0], "还剩 12:00") &&
          !strcmp(view.rows[1], "下一个闹钟"));
    post(XIAOTAI_INTENT_APP_ALARM_TIMER, 0);
    post(XIAOTAI_INTENT_APP_ALARM_TIMER_CANCEL, 0);
    CHECK(removes == 1 && entry_count == 0 && !strcmp(view.toast, "已取消倒计时"));
    CHECK(view.fields[0].kind == XIAOTAI_FIELD_STEPPER);

    post(XIAOTAI_INTENT_APP_ALARM_BACK, 0);
    post(XIAOTAI_INTENT_APP_ALARM_SETTINGS, 0);
    CHECK(!strcmp(view.id, XIAOTAI_APP_ALARM_PAGE_SETTINGS) && view.field_count == 3);
    CHECK(view.fields[0].value == 1 && view.fields[1].value == 0 && view.fields[2].value == 0);   /* 1 min, 5 min, home zone */
    post(XIAOTAI_INTENT_APP_ALARM_SETTING_RING, 3);
    post(XIAOTAI_INTENT_APP_ALARM_SETTING_SNOOZE, 2);
    post(XIAOTAI_INTENT_APP_ALARM_SETTING_ZONE, 1);
    post(XIAOTAI_INTENT_APP_ALARM_SETTING_RING, 9);    /* not a choice: refused */
    CHECK(setting(false) == 300 && setting(true) == 15 && !strcmp(view.toast, "设置没有保存，请重试"));
    char zone[XIAOTAI_SETTING_TEXT_MAX];
    on_owner(zone_job, zone);
    CHECK(!strcmp(zone, "JST-9") && view.fields[0].value == 3 && view.fields[1].value == 2 && view.fields[2].value == 1);
    stop();
}

/* Snooze: the ringing stops and a countdown labelled with the alarm's time is added. */
static void snooze_test(void)
{
    start(0);
    const xiaotai_schedule_t entry = {.id = 4, .kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true, .hour = 7, .minute = 0};
    due(&entry);
    CHECK(ringing && find_action(XIAOTAI_INTENT_APP_ALARM_SNOOZE));
    post(XIAOTAI_INTENT_APP_ALARM_SNOOZE, 0);
    CHECK(!ringing && status().snoozes == 1 && !strcmp(shown_app, "home"));
    CHECK(adds == 1 && entries[0].kind == XIAOTAI_SCHEDULE_COUNTDOWN && entries[0].seconds == 300);
    CHECK(!strcmp(entries[0].label, "07:00 的闹钟"));
    const xiaotai_schedule_t again = entries[0];
    due(&again);
    CHECK(ringing && !strcmp(view.text, "07:00 的闹钟"));
    post(XIAOTAI_INTENT_APP_ALARM_STOP, 0);
    CHECK(!ringing && status().stops == 1);
    stop();
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "";
    if (!strcmp(which, "ring")) ring_test();
    else if (!strcmp(which, "timer_settings")) timer_settings_test();
    else if (!strcmp(which, "snooze")) snooze_test();
    else if (!strcmp(which, "busy")) busy_test();
    else if (!strcmp(which, "late")) late_test();
    else if (!strcmp(which, "timeout")) timeout_test();
    else if (!strcmp(which, "config")) config_test();
    else pages_test();
    return 0;
}
