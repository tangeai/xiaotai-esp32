// SPDX-License-Identifier: MIT
#include "xiaotai/apps/alarm.h"
#include "alarm_sounds.h"

#ifdef ALARM_WITH_AI_TOOLS
#include "alarm_ai_tools.h"
#endif
#include "xiaotai/at_command.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/feedback.h"
#include "xiaotai/modules/clock.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/sounds.h"
#include "xiaotai/ui.h"
#include "xiaotai/view.h"

/* xiaotai/modules/ai_talk.h, where the product has the module. */
#define TALK_START "talk.start"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SOURCE "alarm"
#define MINUTE_STEP_DEFAULT 5U
#define DRAFT_HOUR 7U
#define TIMER_DEFAULT_MIN 5
#define TIMER_MAX_MIN 180
#define TICK_MS 1000U
/* Paging steps closer together than this are one tap (the application paged
 * to has its arrows where the finger still is). */
#define PAGER_GUARD_US 350000

typedef enum { PAGE_HOME, PAGE_LIST, PAGE_EDIT, PAGE_TIMER, PAGE_SETTINGS, PAGE_RINGING } page_t;
typedef enum { END_STOP, END_SNOOZE, END_TIMEOUT, END_REVOKED, END_LEFT } end_t;

static const uint8_t repeat_days[XIAOTAI_APP_ALARM_REPEAT_COUNT] = {
    [XIAOTAI_APP_ALARM_REPEAT_ONCE] = 0,
    [XIAOTAI_APP_ALARM_REPEAT_DAILY] = XIAOTAI_SCHEDULE_EVERY_DAY,
    [XIAOTAI_APP_ALARM_REPEAT_WORKDAYS] = 0x3EU,   /* Monday .. Friday */
    [XIAOTAI_APP_ALARM_REPEAT_WEEKEND] = 0x41U,    /* Sunday, Saturday */
};
static const char *const repeat_names[XIAOTAI_APP_ALARM_REPEAT_COUNT] = {"仅一次", "每天", "工作日", "周末"};
static const char *const weekday_names[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

static const int32_t ring_values[] = {30, 60, 120, 300};
static const char *const ring_names[] = {"30 秒", "1 分钟", "2 分钟", "5 分钟"};
static const int32_t snooze_values[] = {5, 10, 15};
static const char *const snooze_names[] = {"5 分钟", "10 分钟", "15 分钟"};
/* Time zones offered when the product registers the clock module's setting;
 * "" keeps the rule the device started with. */
static const char *const zone_rules[] = {
    "", "JST-9", "<+04>-4", "CET-1CEST,M3.5.0,M10.5.0/3", "GMT0BST,M3.5.0/1,M10.5.0",
    "EST5EDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0", "AEST-10AEDT,M10.1.0,M4.1.0/3",
};
static const char *const zone_names[] = {"北京时间", "东京", "迪拜", "柏林", "伦敦", "纽约", "洛杉矶", "悉尼"};
#define COUNT(a) (sizeof(a) / sizeof(*(a)))

typedef struct {
    uint8_t id, hour, minute, repeat;
    bool enabled;
} draft_t;

typedef struct {
    uint32_t activity, timer, token;
    xiaotai_schedule_t entry;        /* what rings, remembered again when a session cuts it */
    int64_t due_us;
    int64_t next_us;                 /* when it followed another one on the screen, else 0 */
    uint32_t late_min;
    uint8_t id, hour, minute;
    bool countdown, late;
    char label[XIAOTAI_SCHEDULE_LABEL_MAX + 1U];
    const char *state;               /* feedback state that sounds now */
} ring_t;

/* An entry that fired and has not rung. */
typedef struct {
    xiaotai_schedule_t entry;
    int64_t due_us;
    bool again;                      /* announced again: rings late */
} pending_t;

static struct {
    xiaotai_app_alarm_config_t config;
    xiaotai_schedule_info_t *rows;   /* BULK, XIAOTAI_SCHEDULE_MAX entries */
    pending_t *pending;              /* BULK, XIAOTAI_SCHEDULE_MAX entries, the one due first first */
    size_t pending_count;
    uint32_t late_timer;
    const char *published;
    page_t page;
    draft_t draft;
    ring_t ring;
    uint32_t tick;                   /* refreshes the countdown page each second */
    int64_t paged_us;                /* when the pager brought this application forward */
    int32_t timer_minutes;
    xiaotai_app_alarm_status_t counts;
    bool initialized, foreground;
} alarm;

static void render(void);

/* ------------------------------------------------------------------ helpers */

static size_t index_of(const int32_t *values, size_t count, int32_t value, size_t fallback)
{
    for (size_t i = 0; i < count; ++i) {
        if (values[i] == value) return i;
    }
    return fallback;
}

static int32_t setting_int(const char *name, int32_t fallback)
{
    int32_t value = fallback;
    return xiaotai_setting_get_int(name, &value) == XIAOTAI_OK ? value : fallback;
}

static bool zone_offered(size_t *index)
{
    char rule[XIAOTAI_SETTING_TEXT_MAX];
    if (xiaotai_setting_get_text(XIAOTAI_SETTING_CLOCK_TIMEZONE, rule, sizeof(rule)) != XIAOTAI_OK) return false;
    *index = 0;
    for (size_t i = 0; i < COUNT(zone_rules); ++i) {
        if (!strcmp(rule, zone_rules[i])) *index = i;
    }
    return true;
}

static uint8_t repeat_index(uint8_t weekdays)
{
    for (uint8_t i = 0; i < XIAOTAI_APP_ALARM_REPEAT_COUNT; ++i) {
        if (repeat_days[i] == weekdays) return i;
    }
    return XIAOTAI_APP_ALARM_REPEAT_DAILY;
}

static void days_text(uint8_t weekdays, char *out, size_t size)
{
    for (uint8_t i = 0; i < XIAOTAI_APP_ALARM_REPEAT_COUNT; ++i) {
        if (repeat_days[i] == weekdays) {
            (void)snprintf(out, size, "%s", repeat_names[i]);
            return;
        }
    }
    size_t used = 0;
    out[0] = '\0';
    for (uint8_t day = 1; day <= 7; ++day) {   /* Monday first */
        if (!(weekdays & (1U << (day % 7U)))) continue;
        const int n = snprintf(out + used, size - used, "%s%s", used ? " " : "", weekday_names[day % 7U]);
        if (n < 0 || (size_t)n >= size - used) break;
        used += (size_t)n;
    }
}

static void remaining_text(uint32_t seconds, char *out, size_t size)
{
    if (seconds >= 3600U) {
        (void)snprintf(out, size, "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, seconds / 3600U, seconds / 60U % 60U,
                       seconds % 60U);
    } else {
        (void)snprintf(out, size, "%02" PRIu32 ":%02" PRIu32, seconds / 60U, seconds % 60U);
    }
}

/* "今天 07:30", "明天 07:30", "周三 07:30" for an alarm due in due_in_s. */
static void when_text(const xiaotai_schedule_info_t *info, char *out, size_t size)
{
    xiaotai_wallclock_t now;
    if (!xiaotai_wallclock_now(&now)) {
        (void)snprintf(out, size, "%02u:%02u", (unsigned)info->entry.hour, (unsigned)info->entry.minute);
        return;
    }
    const uint32_t now_min = (uint32_t)now.local.hour * 60U + now.local.minute;
    const uint32_t days = (now_min + info->due_in_s / 60U) / 1440U;
    const char *day = days == 0 ? "今天" : days == 1 ? "明天" : weekday_names[(now.local.weekday + days) % 7U];
    (void)snprintf(out, size, "%s %02u:%02u", day, (unsigned)info->entry.hour, (unsigned)info->entry.minute);
}

/* The countdown that ends first, if any. */
static bool running_countdown(xiaotai_schedule_info_t *out)
{
    const size_t total = xiaotai_schedule_list(alarm.rows, XIAOTAI_SCHEDULE_MAX);
    bool found = false;
    for (size_t i = 0; i < total; ++i) {
        if (alarm.rows[i].entry.kind != XIAOTAI_SCHEDULE_COUNTDOWN) continue;
        if (!found || alarm.rows[i].due_in_s < out->due_in_s) *out = alarm.rows[i];
        found = true;
    }
    return found;
}

/* The alarm that rings first, if any is on. */
static bool next_alarm(xiaotai_schedule_info_t *out)
{
    const size_t total = xiaotai_schedule_list(alarm.rows, XIAOTAI_SCHEDULE_MAX);
    bool found = false;
    for (size_t i = 0; i < total; ++i) {
        const xiaotai_schedule_info_t *row = &alarm.rows[i];
        if (row->entry.kind != XIAOTAI_SCHEDULE_ALARM || !row->entry.enabled || row->due_in_s == UINT32_MAX) continue;
        if (!found || row->due_in_s < out->due_in_s) *out = *row;
        found = true;
    }
    return found;
}

static void publish(const xiaotai_view_t *view)
{
    alarm.published = view->id;
    xiaotai_view_show(view);
}

static void tick(void *ctx);

static void go(page_t page)
{
    alarm.page = page;
    render();
}

/* ------------------------------------------------------------------- pages */

static size_t pager_actions(xiaotai_view_action_t *actions)
{
    actions[0] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_PREVIOUS, .intent = XIAOTAI_INTENT_APP_ALARM_HOME,
        .trigger = XIAOTAI_TRIGGER_SWIPE_RIGHT, .value = -1};
    actions[1] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_NEXT, .intent = XIAOTAI_INTENT_APP_ALARM_HOME,
        .trigger = XIAOTAI_TRIGGER_SWIPE_LEFT, .value = 1};
    return 2;
}

static void render_home(void)
{
    /* Cards for a clock with room for them, one line for any other. */
    char text[64], left[24], when[40];
    xiaotai_wallclock_t now;
    xiaotai_schedule_info_t next, countdown;
    xiaotai_view_item_t items[2];
    size_t shown = 0;
    const bool timing = running_countdown(&countdown), synced = xiaotai_wallclock_now(&now);
    if (timing) {
        char remaining[16];
        remaining_text(countdown.due_in_s, remaining, sizeof(remaining));
        (void)snprintf(left, sizeof(left), "还剩 %s", remaining);
        items[shown++] = (xiaotai_view_item_t){.label = "倒计时", .detail = left, .icon = XIAOTAI_ICON_TIMER};
        (void)snprintf(text, sizeof(text), "倒计时 %s", left);
    }
    items[shown] = (xiaotai_view_item_t){.label = synced ? "下一个闹钟" : "闹钟", .detail = synced ? "还没有" : "时间同步后才会响",
                                         .icon = XIAOTAI_ICON_ALARM};
    if (synced && next_alarm(&next)) {
        when_text(&next, when, sizeof(when));
        items[shown].detail = when;
        if (!timing) (void)snprintf(text, sizeof(text), "下一个闹钟 %s", when);
    } else if (!timing) {
        (void)snprintf(text, sizeof(text), "%s", synced ? "还没有闹钟" : "时间同步后闹钟才会响");
    }
    ++shown;
    xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX];
    size_t count = pager_actions(actions);
    /* The clock is where somebody often is: XiaoTai is a tap away, also when the wake word is off or not heard. */
    if (xiaotai_intent_exists(TALK_START))
        actions[count++] = (xiaotai_view_action_t){.label = "对话", .icon = XIAOTAI_ICON_MIC, .intent = TALK_START};
    actions[count++] = (xiaotai_view_action_t){.label = "闹钟", .icon = XIAOTAI_ICON_ALARM,
        .intent = XIAOTAI_INTENT_APP_ALARM_LIST, .role = XIAOTAI_ROLE_PRIMARY};
    actions[count++] = (xiaotai_view_action_t){.label = "倒计时", .icon = XIAOTAI_ICON_TIMER, .intent = XIAOTAI_INTENT_APP_ALARM_TIMER};
    actions[count++] = (xiaotai_view_action_t){.label = "设置", .icon = XIAOTAI_ICON_SETTINGS, .intent = XIAOTAI_INTENT_APP_ALARM_SETTINGS};
    const xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CLOCK, .id = XIAOTAI_APP_ALARM_PAGE_HOME, .title = "小钛", .status = "闹钟",
        .text = text, .items = items, .item_count = shown, .actions = actions, .action_count = count,
        .flags = XIAOTAI_VIEW_MAY_SLEEP | XIAOTAI_VIEW_CALENDAR,
    };
    publish(&view);
}

static void render_list(void)
{
    const size_t total = xiaotai_schedule_list(alarm.rows, XIAOTAI_SCHEDULE_MAX);
    xiaotai_view_item_t items[XIAOTAI_SCHEDULE_MAX];
    char labels[XIAOTAI_SCHEDULE_MAX][8];
    char details[XIAOTAI_SCHEDULE_MAX][40];
    size_t count = 0;
    for (size_t i = 0; i < total; ++i) {
        const xiaotai_schedule_t *e = &alarm.rows[i].entry;
        if (e->kind != XIAOTAI_SCHEDULE_ALARM) continue;
        char days[32];
        days_text(e->weekdays, days, sizeof(days));
        (void)snprintf(labels[count], sizeof(labels[count]), "%02u:%02u", (unsigned)e->hour, (unsigned)e->minute);
        (void)snprintf(details[count], sizeof(details[count]), "%s · %s", days, e->enabled ? "开" : "关");
        items[count] = (xiaotai_view_item_t){.label = labels[count], .detail = details[count],
            .intent = XIAOTAI_INTENT_APP_ALARM_EDIT, .value = e->id,
            .state = e->enabled ? XIAOTAI_STATE_CHECKED : 0};
        ++count;
    }
    const xiaotai_view_action_t actions[] = {
        {.label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_ALARM_BACK},
        {.label = "添加闹钟", .intent = XIAOTAI_INTENT_APP_ALARM_EDIT, .role = XIAOTAI_ROLE_PRIMARY,
         .state = total >= XIAOTAI_SCHEDULE_MAX ? XIAOTAI_STATE_DISABLED : 0},
    };
    const xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_LIST, .id = XIAOTAI_APP_ALARM_PAGE_LIST, .title = "闹钟",
        .text = count ? NULL : "还没有闹钟，点「添加闹钟」设一个", .items = items, .item_count = count,
        .actions = actions, .action_count = 2,
    };
    publish(&view);
}

static void render_edit(void)
{
    const bool existing = alarm.draft.id != 0;
    const xiaotai_view_field_t fields[4] = {
        {.kind = XIAOTAI_FIELD_STEPPER, .label = "时", .intent = XIAOTAI_INTENT_APP_ALARM_HOUR,
         .value = alarm.draft.hour, .min = 0, .max = 23, .step = 1, .text = "点"},
        {.kind = XIAOTAI_FIELD_STEPPER, .label = "分", .intent = XIAOTAI_INTENT_APP_ALARM_MINUTE,
         .value = alarm.draft.minute, .min = 0, .max = 59, .step = alarm.config.minute_step, .text = "分"},
        {.kind = XIAOTAI_FIELD_CHOICE, .label = "重复", .intent = XIAOTAI_INTENT_APP_ALARM_REPEAT,
         .value = alarm.draft.repeat, .options = repeat_names, .option_count = XIAOTAI_APP_ALARM_REPEAT_COUNT},
        {.kind = XIAOTAI_FIELD_SWITCH, .label = "开启", .intent = XIAOTAI_INTENT_APP_ALARM_ENABLED,
         .value = alarm.draft.enabled, .max = 1},
    };
    const xiaotai_view_action_t actions[3] = {
        {.label = "取消", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_ALARM_BACK},
        {.label = "保存", .role = XIAOTAI_ROLE_PRIMARY, .style = XIAOTAI_STYLE_POSITIVE,
         .intent = XIAOTAI_INTENT_APP_ALARM_SAVE},
        {.label = "删除", .style = XIAOTAI_STYLE_DESTRUCTIVE, .intent = XIAOTAI_INTENT_APP_ALARM_DELETE},
    };
    const xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_FORM, .id = XIAOTAI_APP_ALARM_PAGE_EDIT, .title = existing ? "修改闹钟" : "新闹钟",
        .fields = fields, .field_count = existing ? 4 : 3, .actions = actions, .action_count = existing ? 3 : 2,
    };
    publish(&view);
}

static void render_timer(void)
{
    xiaotai_schedule_info_t countdown;
    const bool running = running_countdown(&countdown);
    char left[16] = "";
    if (running) remaining_text(countdown.due_in_s, left, sizeof(left));
    const xiaotai_view_field_t fields[] = {
        running ?
        (xiaotai_view_field_t){.kind = XIAOTAI_FIELD_TEXT, .label = "还剩", .text = left} :
        (xiaotai_view_field_t){.kind = XIAOTAI_FIELD_STEPPER, .label = "时长", .intent = XIAOTAI_INTENT_APP_ALARM_TIMER_MINUTES,
                               .value = alarm.timer_minutes, .min = 1, .max = TIMER_MAX_MIN, .step = 1, .text = "分钟"},
    };
    const xiaotai_view_action_t actions[] = {
        {.label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_ALARM_BACK},
        running ?
        (xiaotai_view_action_t){.label = "取消倒计时", .style = XIAOTAI_STYLE_DESTRUCTIVE,
                                .intent = XIAOTAI_INTENT_APP_ALARM_TIMER_CANCEL, .role = XIAOTAI_ROLE_PRIMARY} :
        (xiaotai_view_action_t){.label = "开始", .style = XIAOTAI_STYLE_POSITIVE,
                                .intent = XIAOTAI_INTENT_APP_ALARM_TIMER_START, .role = XIAOTAI_ROLE_PRIMARY},
    };
    const xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_FORM, .id = XIAOTAI_APP_ALARM_PAGE_TIMER, .title = "倒计时",
        .fields = fields, .field_count = 1, .actions = actions, .action_count = 2,
    };
    publish(&view);
    /* The time left is redrawn once a second while the page shows it. */
    if (running && !alarm.tick && xiaotai_timer_start(TICK_MS, tick, NULL, &alarm.tick) != XIAOTAI_OK) alarm.tick = 0;
}

static void render_settings(void)
{
    size_t zone = 0;
    const bool zones = zone_offered(&zone);
    const xiaotai_view_field_t fields[3] = {
        {.kind = XIAOTAI_FIELD_CHOICE, .label = "响铃时长", .intent = XIAOTAI_INTENT_APP_ALARM_SETTING_RING,
         .value = (int32_t)index_of(ring_values, COUNT(ring_values),
                                    setting_int(XIAOTAI_SETTING_APP_ALARM_RING_S, alarm.config.ring_s), 1),
         .options = ring_names, .option_count = COUNT(ring_names)},
        {.kind = XIAOTAI_FIELD_CHOICE, .label = "稍后提醒", .intent = XIAOTAI_INTENT_APP_ALARM_SETTING_SNOOZE,
         .value = (int32_t)index_of(snooze_values, COUNT(snooze_values),
                                    setting_int(XIAOTAI_SETTING_APP_ALARM_SNOOZE_MIN, alarm.config.snooze_min), 0),
         .options = snooze_names, .option_count = COUNT(snooze_names)},
        {.kind = XIAOTAI_FIELD_CHOICE, .label = "时区", .intent = XIAOTAI_INTENT_APP_ALARM_SETTING_ZONE,
         .value = (int32_t)zone, .options = zone_names, .option_count = COUNT(zone_names)},
    };
    const xiaotai_view_action_t back = {.label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_ALARM_BACK};
    const xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_FORM, .id = XIAOTAI_APP_ALARM_PAGE_SETTINGS, .title = "闹钟设置",
        .fields = fields, .field_count = zones ? 3 : 2, .actions = &back, .action_count = 1,
    };
    publish(&view);
}

static void render_ringing(void)
{
    char what[40], text[80];
    if (alarm.ring.label[0]) {
        (void)snprintf(what, sizeof(what), "%s", alarm.ring.label);
    } else if (alarm.ring.countdown) {
        (void)snprintf(what, sizeof(what), "倒计时结束");
    } else {
        (void)snprintf(what, sizeof(what), "%02u:%02u 的闹钟到了", (unsigned)alarm.ring.hour, (unsigned)alarm.ring.minute);
    }
    if (!alarm.ring.late) (void)snprintf(text, sizeof(text), "%s", what);
    else if (!alarm.ring.late_min) (void)snprintf(text, sizeof(text), "%s（刚才到点）", what);
    else (void)snprintf(text, sizeof(text), "%s（%" PRIu32 " 分钟前到点）", what, alarm.ring.late_min);
    const xiaotai_view_action_t actions[] = {
        {.label = "稍后提醒", .intent = XIAOTAI_INTENT_APP_ALARM_SNOOZE},
        {.label = "停止", .icon = XIAOTAI_ICON_STOP, .intent = XIAOTAI_INTENT_APP_ALARM_STOP,
         .role = XIAOTAI_ROLE_PRIMARY, .style = XIAOTAI_STYLE_POSITIVE},
    };
    const xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CLOCK, .id = XIAOTAI_APP_ALARM_PAGE_RINGING, .title = "小钛", .status = "闹钟响了",
        .text = text, .actions = actions, .action_count = 2,
    };
    publish(&view);
}

static void render(void)
{
    if (!alarm.foreground) return;
    if (alarm.ring.activity) alarm.page = PAGE_RINGING;
    switch (alarm.page) {
    case PAGE_LIST: render_list(); break;
    case PAGE_EDIT: render_edit(); break;
    case PAGE_TIMER: render_timer(); break;
    case PAGE_SETTINGS: render_settings(); break;
    case PAGE_RINGING: render_ringing(); break;
    default: render_home(); break;
    }
}

static void tick(void *ctx)
{
    (void)ctx;
    alarm.tick = 0;
    if (alarm.foreground && !alarm.ring.activity && (alarm.page == PAGE_TIMER || alarm.page == PAGE_HOME)) {
        xiaotai_schedule_info_t countdown;
        if (running_countdown(&countdown)) {
            render();
            if (!alarm.tick && xiaotai_timer_start(TICK_MS, tick, NULL, &alarm.tick) != XIAOTAI_OK) alarm.tick = 0;
        }
    }
}

/* ------------------------------------------------------------------ ringing */

static bool ring_late(bool chained);

/* The one feedback state that sounds now; NULL: none. */
static void sound_state(const char *state)
{
    const char *old = alarm.ring.state;
    if (state == old || (state && old && !strcmp(state, old))) return;
    alarm.ring.state = state;
    if (old) (void)xiaotai_feedback_set(old, false);
    if (state) (void)xiaotai_feedback_set(state, true);
}

static void ring_end(end_t reason)
{
    if (!alarm.ring.activity) return;
    const uint32_t activity = alarm.ring.activity;
    alarm.ring.activity = 0;
    xiaotai_timer_cancel(alarm.ring.timer);
    alarm.ring.timer = 0;
    sound_state(NULL);
    if (reason != END_REVOKED) xiaotai_activity_end(activity);
    switch (reason) {
    case END_STOP: ++alarm.counts.stops; break;
    case END_SNOOZE: ++alarm.counts.snoozes; break;
    case END_TIMEOUT: ++alarm.counts.timeouts; break;
    default: ++alarm.counts.revoked; break;
    }
    xiaotai_eventlog_note(SOURCE, "ring.end", alarm.ring.id, reason);
    xiaotai_args_t args = {.value = 0};
    (void)snprintf(args.text, sizeof(args.text), "%s", alarm.ring.label);
    (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_ALARM_RINGING, &args, NULL, 0);
    alarm.ring.id = 0;
    alarm.page = PAGE_HOME;
    if (!alarm.foreground || reason == END_REVOKED || reason == END_LEFT) return;
    if (ring_late(true)) return;
    /* Brought forward to ring: give the screen back; otherwise stay on the clock. */
    if (xiaotai_app_entry() == XIAOTAI_APP_ENTRY_PRESENT && xiaotai_app_return() == XIAOTAI_OK) return;
    render();
}

static void ring_timeout(void *ctx)
{
    if ((uint32_t)(uintptr_t)ctx != alarm.ring.token) return;
    alarm.ring.timer = 0;
    ring_end(END_TIMEOUT);
}

static void pending_remove(size_t i)
{
    --alarm.pending_count;
    memmove(&alarm.pending[i], &alarm.pending[i + 1U], (alarm.pending_count - i) * sizeof(*alarm.pending));
}

static void pending_forget(uint8_t id)
{
    for (size_t i = 0; i < alarm.pending_count; ++i) {
        if (alarm.pending[i].entry.id == id) {
            pending_remove(i);
            return;
        }
    }
}

static void pending_add(const xiaotai_schedule_t *entry)
{
    pending_forget(entry->id);
    if (alarm.pending_count == XIAOTAI_SCHEDULE_MAX) {
        pending_remove(0);
        ++alarm.counts.dropped;
    }
    alarm.pending[alarm.pending_count++] = (pending_t){.entry = *entry, .due_us = xiaotai_time_us()};
}

static void ring_revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)ctx;
    if (id != alarm.ring.activity) return;
    const bool again = reason == XIAOTAI_REVOKE_PREEMPTED && !alarm.ring.late;
    ring_end(END_REVOKED);
    /* Nobody stopped it: it rings once more after the session. */
    if (again) {
        const xiaotai_schedule_t entry = alarm.ring.entry;
        pending_add(&entry);
        alarm.pending[alarm.pending_count - 1U].due_us = alarm.ring.due_us;
    }
}

/* Drops what is not worth ringing any more. */
static void pending_prune(void)
{
    const int64_t now = xiaotai_time_us();
    for (size_t i = 0; i < alarm.pending_count;) {
        const pending_t *p = &alarm.pending[i];
        bool gone = now - p->due_us > (int64_t)XIAOTAI_APP_ALARM_LATE_MAX_S * 1000000;
        if (!gone && p->entry.kind == XIAOTAI_SCHEDULE_ALARM) {
            /* A one-time alarm is off once it fired; a countdown is removed. */
            xiaotai_schedule_info_t info;
            gone = xiaotai_schedule_get(p->entry.id, &info) != XIAOTAI_OK || (info.entry.weekdays && !info.entry.enabled);
        }
        if (!gone) {
            ++i;
            continue;
        }
        ++alarm.counts.dropped;
        xiaotai_eventlog_warn(SOURCE, "ring.dropped", p->entry.id, (int32_t)((now - p->due_us) / 1000000));
        pending_remove(i);
    }
}

/* Rings entry, replacing the one that rings; false when it was refused. */
static bool ring(const xiaotai_schedule_t *entry)
{
    if (!alarm.ring.activity) {
        static const xiaotai_activity_t activity = {
            .name = XIAOTAI_APP_ALARM_ACTIVITY,
            .resources = XIAOTAI_RESOURCE_AUDIO_OUT | XIAOTAI_RESOURCE_SCREEN,
            .revoked = ring_revoked,
        };
        if (xiaotai_activity_begin(&activity, &alarm.ring.activity) != XIAOTAI_OK) {
            alarm.ring.activity = 0;
            ++alarm.counts.refused;
            xiaotai_eventlog_warn(SOURCE, "ring.refused", entry->id, 0);
            return false;
        }
        ++alarm.counts.rings;
    }
    sound_state(entry->kind == XIAOTAI_SCHEDULE_COUNTDOWN ? XIAOTAI_APP_ALARM_STATE_TIMER : XIAOTAI_APP_ALARM_STATE_RINGING);
    alarm.ring.due_us = xiaotai_time_us();
    for (size_t i = 0; i < alarm.pending_count; ++i) {
        if (alarm.pending[i].entry.id == entry->id) alarm.ring.due_us = alarm.pending[i].due_us;
    }
    alarm.ring.entry = *entry;
    pending_forget(entry->id);
    alarm.ring.id = entry->id;
    alarm.ring.late = false;
    alarm.ring.next_us = 0;
    alarm.ring.hour = entry->hour;
    alarm.ring.minute = entry->minute;
    alarm.ring.countdown = entry->kind == XIAOTAI_SCHEDULE_COUNTDOWN;
    memcpy(alarm.ring.label, entry->label, sizeof(alarm.ring.label));
    alarm.ring.label[sizeof(alarm.ring.label) - 1U] = '\0';
    xiaotai_timer_cancel(alarm.ring.timer);
    alarm.ring.token = xiaotai_generation_next();
    const int32_t ring_s = setting_int(XIAOTAI_SETTING_APP_ALARM_RING_S, alarm.config.ring_s);
    if (xiaotai_timer_start((uint32_t)ring_s * 1000U, ring_timeout, (void *)(uintptr_t)alarm.ring.token,
                            &alarm.ring.timer) != XIAOTAI_OK) {
        alarm.ring.timer = 0;   /* rings until stopped or revoked */
    }
    xiaotai_eventlog_note(SOURCE, "ring.begin", entry->id, entry->kind);
    xiaotai_args_t args = {.value = 1};
    (void)snprintf(args.text, sizeof(args.text), "%s", alarm.ring.label);
    (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_ALARM_RINGING, &args, NULL, 0);
    xiaotai_ui_activity();
    return true;
}

/* Rings the entry that was due first during the session; false when there is
 * none or it was refused (it stays remembered then). */
static bool ring_late(bool chained)
{
    pending_prune();
    if (!alarm.pending_count) return false;
    const pending_t first = alarm.pending[0];
    if (!ring(&first.entry)) return false;
    ++alarm.counts.late;
    alarm.ring.late = true;
    alarm.ring.late_min = (uint32_t)((xiaotai_time_us() - first.due_us) / 60000000);
    alarm.ring.next_us = chained ? xiaotai_time_us() : 0;
    go(PAGE_RINGING);
    return true;
}

static void late_due(void *ctx);

/* Rings what is remembered once the device has been idle for a moment; does
 * nothing while a session runs (its end comes here again). */
static void late_arm(void)
{
    if (!alarm.pending_count || alarm.late_timer || xiaotai_activity_active() || xiaotai_quiesced()) return;
    if (xiaotai_timer_start(XIAOTAI_APP_ALARM_LATE_DELAY_MS, late_due, NULL, &alarm.late_timer) != XIAOTAI_OK) {
        alarm.late_timer = 0;
        xiaotai_eventlog_warn(SOURCE, "ring.late_timer", (int32_t)alarm.pending_count, 0);
    }
}

static void late_due(void *ctx)
{
    (void)ctx;
    alarm.late_timer = 0;
    if (xiaotai_activity_active() || xiaotai_quiesced()) return;
    pending_prune();
    if (!alarm.pending_count) return;
    alarm.pending[0].again = true;
    const xiaotai_schedule_t first = alarm.pending[0].entry;
    if (xiaotai_schedule_due_again(&first) != XIAOTAI_OK) late_arm();
}

static void on_idle(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    late_arm();
}

static const xiaotai_schedule_t *fired(const xiaotai_event_t *event)
{
    return event->size == sizeof(xiaotai_schedule_t) ? event->data : NULL;
}

/* Every entry that fires is remembered until it rings. */
static void on_fired(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!fired(event)) return;
    pending_add(fired(event));
    late_arm();
}

static void on_due(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_schedule_t *entry = fired(event);
    if (!entry) return;
    bool again = false;
    for (size_t i = 0; i < alarm.pending_count; ++i) {
        if (alarm.pending[i].entry.id == entry->id) again = alarm.pending[i].again;
    }
    if (again && alarm.ring.activity) return;   /* follows the one that rings */
    if (again ? ring_late(false) : ring(entry)) {
        if (!again) go(PAGE_RINGING);
        return;
    }
    if (alarm.foreground && !alarm.ring.activity && xiaotai_app_entry() == XIAOTAI_APP_ENTRY_PRESENT)
        (void)xiaotai_app_return();
}

bool xiaotai_app_alarm_claim(const xiaotai_event_t *event)
{
    if (!alarm.initialized || !event || !event->name || strcmp(event->name, XIAOTAI_EVENT_CLOCK_DUE)) return false;
    const uint32_t id = xiaotai_activity_current(NULL);
    return id == 0 || (alarm.ring.activity && id == alarm.ring.activity);
}

/* ------------------------------------------------------------------ intents */

static bool idle_here(void) { return alarm.foreground && !alarm.ring.activity; }

static void on_home(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (alarm.ring.activity) return;
    if (!alarm.foreground) {
        if (args->value == 0) (void)xiaotai_app_switch("alarm");
        return;
    }
    if (args->value == 0) go(PAGE_HOME);
    else if (!alarm.paged_us || xiaotai_time_us() - alarm.paged_us >= PAGER_GUARD_US)
        (void)xiaotai_app_alarm_page_step(args->value > 0 ? 1 : -1);
}

static void on_page(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    if (idle_here()) go((page_t)(uintptr_t)ctx);
}

static void on_edit(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!idle_here()) return;
    xiaotai_schedule_info_t info;
    if (args->value > 0 && args->value <= 255 && xiaotai_schedule_get((uint8_t)args->value, &info) == XIAOTAI_OK &&
        info.entry.kind == XIAOTAI_SCHEDULE_ALARM) {
        alarm.draft = (draft_t){.id = info.entry.id, .hour = info.entry.hour, .minute = info.entry.minute,
            .repeat = repeat_index(info.entry.weekdays), .enabled = info.entry.enabled};
    } else {
        alarm.draft = (draft_t){.hour = DRAFT_HOUR, .repeat = XIAOTAI_APP_ALARM_REPEAT_DAILY, .enabled = true};
    }
    go(PAGE_EDIT);
}

static void on_field(const xiaotai_args_t *args, void *ctx)
{
    if (!idle_here() || alarm.page != PAGE_EDIT) return;
    const int32_t v = args->value;
    switch ((uintptr_t)ctx) {
    case 0: if (v >= 0 && v <= 23) alarm.draft.hour = (uint8_t)v; break;
    case 1: if (v >= 0 && v <= 59) alarm.draft.minute = (uint8_t)v; break;
    case 2: if (v >= 0 && v < XIAOTAI_APP_ALARM_REPEAT_COUNT) alarm.draft.repeat = (uint8_t)v; break;
    default: alarm.draft.enabled = v != 0; break;
    }
    render();
}

static void on_save(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!idle_here() || alarm.page != PAGE_EDIT) return;
    xiaotai_schedule_t entry = {
        .id = alarm.draft.id, .kind = XIAOTAI_SCHEDULE_ALARM, .enabled = alarm.draft.enabled,
        .hour = alarm.draft.hour, .minute = alarm.draft.minute, .weekdays = repeat_days[alarm.draft.repeat],
    };
    const xiaotai_err_t err = entry.id ? xiaotai_schedule_update(&entry) : xiaotai_schedule_add(&entry, NULL);
    if (err != XIAOTAI_OK) {
        ++alarm.counts.save_failures;
        xiaotai_view_toast(err == XIAOTAI_ERR_NO_MEM ? "闹钟已满，请先删除一个" : "没有保存，请重试", 0);
        return;
    }
    ++alarm.counts.saves;
    char toast[48];
    (void)snprintf(toast, sizeof(toast), "已设好 %02u:%02u 的闹钟", (unsigned)entry.hour, (unsigned)entry.minute);
    go(PAGE_LIST);
    xiaotai_view_toast(toast, 0);
}

static void on_delete(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!idle_here() || alarm.page != PAGE_EDIT || !alarm.draft.id) return;
    if (xiaotai_schedule_remove(alarm.draft.id) != XIAOTAI_OK) {
        xiaotai_view_toast("没有删除，请重试", 0);
        return;
    }
    go(PAGE_LIST);
    xiaotai_view_toast("已删除", 0);
}

static void on_timer(const xiaotai_args_t *args, void *ctx)
{
    if (!idle_here() || alarm.page != PAGE_TIMER) return;
    switch ((uintptr_t)ctx) {
    case 0:
        if (args->value >= 1 && args->value <= TIMER_MAX_MIN) alarm.timer_minutes = args->value;
        render();
        break;
    case 1: {
        const xiaotai_schedule_t entry = {.kind = XIAOTAI_SCHEDULE_COUNTDOWN, .enabled = true,
                                          .seconds = (uint32_t)alarm.timer_minutes * 60U};
        if (xiaotai_schedule_add(&entry, NULL) != XIAOTAI_OK) {
            xiaotai_view_toast("倒计时没有开始，请重试", 0);
            return;
        }
        render();
        break;
    }
    default: {
        xiaotai_schedule_info_t countdown;
        while (running_countdown(&countdown)) {
            if (xiaotai_schedule_remove(countdown.entry.id) != XIAOTAI_OK) break;
        }
        render();
        xiaotai_view_toast("已取消倒计时", 0);
        break;
    }
    }
}

static void on_setting(const xiaotai_args_t *args, void *ctx)
{
    if (!idle_here() || alarm.page != PAGE_SETTINGS || args->value < 0) return;
    const size_t i = (size_t)args->value;
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_ARG;
    switch ((uintptr_t)ctx) {
    case 0: if (i < COUNT(ring_values)) err = xiaotai_setting_set_int(XIAOTAI_SETTING_APP_ALARM_RING_S, ring_values[i]); break;
    case 1: if (i < COUNT(snooze_values)) err = xiaotai_setting_set_int(XIAOTAI_SETTING_APP_ALARM_SNOOZE_MIN, snooze_values[i]); break;
    default: if (i < COUNT(zone_rules)) err = xiaotai_setting_set_text(XIAOTAI_SETTING_CLOCK_TIMEZONE, zone_rules[i]); break;
    }
    if (err != XIAOTAI_OK) xiaotai_view_toast(err == XIAOTAI_ERR_BUSY ? "维护中，设置暂不可修改" : "设置没有保存，请重试", 0);
    render();
}

static void on_back(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!idle_here()) return;
    if (alarm.page == PAGE_EDIT) go(PAGE_LIST);
    else if (alarm.page != PAGE_HOME) go(PAGE_HOME);
}

/* The second tap of a double one, aimed at the entry before. */
static bool tapped_before(void)
{
    return alarm.ring.next_us && xiaotai_time_us() - alarm.ring.next_us < PAGER_GUARD_US;
}

static void on_stop(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!tapped_before()) ring_end(END_STOP);
}

static void on_snooze(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!alarm.ring.activity || tapped_before()) return;
    xiaotai_schedule_t again = {.kind = XIAOTAI_SCHEDULE_COUNTDOWN, .enabled = true,
        .seconds = (uint32_t)setting_int(XIAOTAI_SETTING_APP_ALARM_SNOOZE_MIN, alarm.config.snooze_min) * 60U};
    if (alarm.ring.label[0]) (void)snprintf(again.label, sizeof(again.label), "%s", alarm.ring.label);
    else if (alarm.ring.countdown) (void)snprintf(again.label, sizeof(again.label), "倒计时结束");
    else (void)snprintf(again.label, sizeof(again.label), "%02u:%02u 的闹钟", (unsigned)alarm.ring.hour,
                        (unsigned)alarm.ring.minute);
    const xiaotai_err_t err = xiaotai_schedule_add(&again, NULL);
    ring_end(err == XIAOTAI_OK ? END_SNOOZE : END_STOP);
    if (err != XIAOTAI_OK && alarm.foreground) xiaotai_view_toast("没能稍后提醒，闹钟已停止", 0);
}

static void on_clock(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    if (alarm.foreground && (alarm.page == PAGE_HOME || alarm.page == PAGE_LIST || alarm.page == PAGE_TIMER)) render();
}

/* ---------------------------------------------------------------------- AT */

static xiaotai_err_t at_status(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_app_alarm_status_t s;
    xiaotai_app_alarm_status(&s);
    xiaotai_at_printf(reply, "+ALARM: foreground=%u page=%s ringing=%u id=%u rings=%" PRIu32 " stops=%" PRIu32
                     " snoozes=%" PRIu32 " timeouts=%" PRIu32 " revoked=%" PRIu32 " refused=%" PRIu32
                     " saves=%" PRIu32 " save_failures=%" PRIu32 " pending=%" PRIu32 " late=%" PRIu32
                     " dropped=%" PRIu32, (unsigned)s.foreground,
                     s.page[0] ? s.page : "-", (unsigned)s.ringing, (unsigned)s.ringing_id, s.rings, s.stops,
                     s.snoozes, s.timeouts, s.revoked, s.refused, s.saves, s.save_failures, s.pending, s.late,
                     s.dropped);
    return XIAOTAI_OK;
}

/* ---------------------------------------------------------------- lifecycle */

xiaotai_err_t xiaotai_app_alarm_configure(const xiaotai_app_alarm_config_t *config)
{
    if (alarm.initialized) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_app_alarm_config_t c = config ? *config : (xiaotai_app_alarm_config_t){0};
    if ((c.ring_s && index_of(ring_values, COUNT(ring_values), c.ring_s, COUNT(ring_values)) == COUNT(ring_values)) ||
        (c.snooze_min && index_of(snooze_values, COUNT(snooze_values), c.snooze_min, COUNT(snooze_values)) ==
                             COUNT(snooze_values)) ||
        c.minute_step > 30U) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    alarm.config = c;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_app_alarm_init(void)
{
    if (alarm.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (!alarm.config.ring_s) alarm.config.ring_s = 60U;
    if (!alarm.config.minute_step) alarm.config.minute_step = MINUTE_STEP_DEFAULT;
    if (!alarm.config.snooze_min) alarm.config.snooze_min = 5U;
    alarm.timer_minutes = TIMER_DEFAULT_MIN;
    const xiaotai_setting_t ring = {
        .name = XIAOTAI_SETTING_APP_ALARM_RING_S, .key = "alarm_ring", .kind = XIAOTAI_SETTING_INT,
        .min = 30, .max = 300, .default_value = alarm.config.ring_s,
    };
    const xiaotai_setting_t snooze = {
        .name = XIAOTAI_SETTING_APP_ALARM_SNOOZE_MIN, .key = "alarm_snooze", .kind = XIAOTAI_SETTING_INT,
        .min = 5, .max = 15, .default_value = alarm.config.snooze_min,
    };
    xiaotai_err_t err = xiaotai_setting_register(&ring);
    if (err == XIAOTAI_OK) err = xiaotai_setting_register(&snooze);
    static const xiaotai_feedback_state_t states[] = {
        {.name = XIAOTAI_APP_ALARM_STATE_RINGING, .priority = 3, .sound = {.name = XIAOTAI_APP_ALARM_SOUND_ALARM, .gap_ms = 350}},
        {.name = XIAOTAI_APP_ALARM_STATE_TIMER, .priority = 3, .sound = {.name = XIAOTAI_APP_ALARM_SOUND_TIMER, .gap_ms = 500}},
    };
    static const xiaotai_feedback_table_t table = {.states = states, .state_count = COUNT(states)};
    if (err == XIAOTAI_OK) err = alarm_sounds_init();
    if (err == XIAOTAI_OK) err = xiaotai_feedback_register(&table);
    static const struct {
        const char *name;
        xiaotai_intent_fn_t handler;
        uintptr_t ctx;
    } intents[] = {
        {XIAOTAI_INTENT_APP_ALARM_HOME, on_home, 0}, {XIAOTAI_INTENT_APP_ALARM_LIST, on_page, PAGE_LIST},
        {XIAOTAI_INTENT_APP_ALARM_EDIT, on_edit, 0}, {XIAOTAI_INTENT_APP_ALARM_HOUR, on_field, 0},
        {XIAOTAI_INTENT_APP_ALARM_MINUTE, on_field, 1}, {XIAOTAI_INTENT_APP_ALARM_REPEAT, on_field, 2},
        {XIAOTAI_INTENT_APP_ALARM_ENABLED, on_field, 3}, {XIAOTAI_INTENT_APP_ALARM_SAVE, on_save, 0},
        {XIAOTAI_INTENT_APP_ALARM_DELETE, on_delete, 0}, {XIAOTAI_INTENT_APP_ALARM_TIMER, on_page, PAGE_TIMER},
        {XIAOTAI_INTENT_APP_ALARM_TIMER_MINUTES, on_timer, 0}, {XIAOTAI_INTENT_APP_ALARM_TIMER_START, on_timer, 1},
        {XIAOTAI_INTENT_APP_ALARM_TIMER_CANCEL, on_timer, 2},
        {XIAOTAI_INTENT_APP_ALARM_SETTINGS, on_page, PAGE_SETTINGS},
        {XIAOTAI_INTENT_APP_ALARM_SETTING_RING, on_setting, 0}, {XIAOTAI_INTENT_APP_ALARM_SETTING_SNOOZE, on_setting, 1},
        {XIAOTAI_INTENT_APP_ALARM_SETTING_ZONE, on_setting, 2}, {XIAOTAI_INTENT_APP_ALARM_BACK, on_back, 0},
        {XIAOTAI_INTENT_APP_ALARM_STOP, on_stop, 0}, {XIAOTAI_INTENT_APP_ALARM_SNOOZE, on_snooze, 0},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < COUNT(intents); ++i) {
        err = xiaotai_intent_handle(intents[i].name, intents[i].handler, (void *)intents[i].ctx);
    }
    static const struct {
        const char *name;
        xiaotai_event_fn_t handler;
    } events[] = {
        {XIAOTAI_EVENT_CLOCK_FIRED, on_fired}, {XIAOTAI_EVENT_CLOCK_DUE, on_due},
        {XIAOTAI_EVENT_ACTIVITY, on_idle}, {XIAOTAI_EVENT_QUIESCE, on_idle},
        {XIAOTAI_EVENT_CLOCK_MINUTE, on_clock},
        {XIAOTAI_EVENT_CLOCK_SCHEDULES, on_clock}, {XIAOTAI_EVENT_CLOCK_SYNCED, on_clock},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < COUNT(events); ++i) {
        err = xiaotai_event_subscribe(events[i].name, events[i].handler, NULL);
    }
    static const xiaotai_at_verb_t verbs[] = {{.run = at_status}};
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+ALARM", verbs, 1);
#ifdef ALARM_WITH_AI_TOOLS
    if (err == XIAOTAI_OK) err = alarm_ai_tools_init();
#endif
    if (err != XIAOTAI_OK) return err;
    alarm.rows = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_SCHEDULE_MAX, sizeof(*alarm.rows));
    alarm.pending = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_SCHEDULE_MAX, sizeof(*alarm.pending));
    if (!alarm.rows || !alarm.pending) {
        xiaotai_mem_free(alarm.rows);
        xiaotai_mem_free(alarm.pending);
        alarm.rows = NULL;
        alarm.pending = NULL;
        return XIAOTAI_ERR_NO_MEM;
    }
    alarm.initialized = true;
    return XIAOTAI_OK;
}

void xiaotai_app_alarm_start(void) {}

void xiaotai_app_alarm_foreground(bool foreground)
{
    alarm.foreground = foreground;
    if (!foreground) {
        alarm.published = NULL;
        xiaotai_timer_cancel(alarm.tick);
        alarm.tick = 0;
        ring_end(END_LEFT);
        if (alarm.page != PAGE_RINGING) alarm.page = PAGE_HOME;
        return;
    }
    /* Brought forward to ring: the due handler shows the ringing screen. */
    const xiaotai_app_entry_t entry = xiaotai_app_entry();
    if (entry == XIAOTAI_APP_ENTRY_PRESENT) return;
    alarm.paged_us = entry == XIAOTAI_APP_ENTRY_NEXT || entry == XIAOTAI_APP_ENTRY_PREVIOUS ? xiaotai_time_us() : 0;
    go(PAGE_HOME);
    xiaotai_schedule_info_t countdown;
    if (running_countdown(&countdown) && !alarm.tick &&
        xiaotai_timer_start(TICK_MS, tick, NULL, &alarm.tick) != XIAOTAI_OK) alarm.tick = 0;
}

xiaotai_err_t xiaotai_app_alarm_page_step(int direction)
{
    if (direction != 1 && direction != -1) return XIAOTAI_ERR_INVALID_ARG;
    if (!alarm.foreground || alarm.page != PAGE_HOME || alarm.ring.activity) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = direction > 0 ? xiaotai_app_next() : xiaotai_app_previous();
    return err == XIAOTAI_ERR_NOT_FOUND ? XIAOTAI_OK : err;
}

void xiaotai_app_alarm_status(xiaotai_app_alarm_status_t *status)
{
    if (!status) return;
    *status = alarm.counts;
    status->foreground = alarm.foreground;
    status->ringing = alarm.ring.activity != 0;
    status->ringing_id = alarm.ring.id;
    status->pending = (uint32_t)alarm.pending_count;
    status->page[0] = '\0';
    if (alarm.foreground && alarm.published)
        (void)snprintf(status->page, sizeof(status->page), "%s", alarm.published);
}

void xiaotai_app_alarm_reset(void)
{
    xiaotai_timer_cancel(alarm.ring.timer);
    xiaotai_timer_cancel(alarm.tick);
    xiaotai_timer_cancel(alarm.late_timer);
    xiaotai_mem_free(alarm.rows);
    xiaotai_mem_free(alarm.pending);
    memset(&alarm, 0, sizeof(alarm));
#ifdef ALARM_WITH_AI_TOOLS
    alarm_ai_tools_reset();
#endif
}
