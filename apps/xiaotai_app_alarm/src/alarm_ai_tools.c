// SPDX-License-Identifier: MIT
/* The alarm application's tools of the AI conversation: alarms and countdowns
 * set by talking ("wake me at seven", "every workday at eight", "cancel the
 * seven o'clock alarm", "remind me in ten minutes", "how long is left").
 * Compiled when the product selects the ai_talk module beside this
 * application; each tool is one action name the service calls
 * (xiaotai/modules/ai_talk.h, tools):
 *
 *   add_alarm      hour, minute, repeat once|daily|weekdays|weekends|custom,
 *                  days (custom: "mon,wed,fri"), day_offset (once: 0 today,
 *                  1 tomorrow), label
 *   list_alarms    no arguments
 *   change_alarm   mode remove|enable|disable; which: id, or hour and minute,
 *                  or all
 *   start_timer    minutes, seconds, label
 *   cancel_timer   id (optional: every countdown without it)
 *   get_timer      no arguments
 *
 * The entries are the clock module's schedules, the same ones the
 * application's pages and AT+CLOCK show; nothing is kept here. Every answer
 * is a result object {"ok", "status", "message", ...} with `status` one of
 * the fixed words below. Handlers run on the owner task and do not block;
 * the one buffer they share is allocated once, at init. */
#include "alarm_ai_tools.h"

#include <stdio.h>
#include <string.h>
#include "xiaotai/json.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/clock.h"
#include "xiaotai/port/mem.h"

/* Sixteen alarms with full labels and every weekday named fit. */
#define REPLY_BYTES 3840U
#define DAYS_BYTES 32U
#define WEEKDAYS 0x3EU   /* Monday .. Friday */
#define WEEKEND 0x41U    /* Sunday, Saturday */
/* A countdown this short may well end before the conversation does. */
#define SHORT_TIMER_S 120

static const char ok[] = "ok", exists[] = "exists", none[] = "none", invalid[] = "invalid_params",
                  full[] = "full", not_found[] = "not_found", failed[] = "failed",
                  unsupported_date[] = "unsupported_date";
static const char *const day_names[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

typedef struct {
    xiaotai_schedule_info_t rows[XIAOTAI_SCHEDULE_MAX];
    xiaotai_json_writer_t writer;
    char reply[REPLY_BYTES];
} workspace_t;

static workspace_t *work;

/* ------------------------------------------------------------------ replies */

static void begin(const char *status, const char *message)
{
    xiaotai_json_writer_init(&work->writer, work->reply, sizeof(work->reply));
    xiaotai_json_object_begin(&work->writer, NULL);
    xiaotai_json_put_bool(&work->writer, "ok", status == ok || status == exists || status == none);
    xiaotai_json_put_string(&work->writer, "status", status);
    xiaotai_json_put_string(&work->writer, "message", message);
}

static void end(const xiaotai_ai_talk_tool_call_t *call)
{
    xiaotai_json_object_end(&work->writer);
    if (xiaotai_json_writer_end(&work->writer, NULL) == XIAOTAI_OK)
        (void)xiaotai_ai_talk_tool_reply(call->session, call->request, work->reply);
}

static void plain(const xiaotai_ai_talk_tool_call_t *call, const char *status, const char *message)
{
    begin(status, message);
    end(call);
}

/* An optional integer argument: false when it is there and not a number. */
static bool optional_int(const xiaotai_ai_talk_tool_call_t *call, const char *name, bool *present, int32_t *value)
{
    const xiaotai_err_t err = xiaotai_ai_talk_tool_arg_int(call, name, value);
    *present = err == XIAOTAI_OK;
    return err == XIAOTAI_OK || err == XIAOTAI_ERR_NOT_FOUND;
}

/* ----------------------------------------------------------------- weekdays */

static const char *repeat_word(uint8_t weekdays)
{
    return weekdays == 0 ? "once" : weekdays == XIAOTAI_SCHEDULE_EVERY_DAY ? "daily" :
           weekdays == WEEKDAYS ? "weekdays" : weekdays == WEEKEND ? "weekends" : "custom";
}

/* "mon,wed,fri", Monday first. */
static void days_text(uint8_t weekdays, char out[DAYS_BYTES])
{
    size_t used = 0;
    out[0] = 0;
    for (unsigned day = 1; day <= 7; ++day) {
        if (!(weekdays & (1U << (day % 7U)))) continue;
        used += (size_t)snprintf(out + used, DAYS_BYTES - used, "%s%s", used ? "," : "", day_names[day % 7U]);
    }
}

/* Day names by their first three letters ("monday", "Mon"), or the numbers
 * 1 (Monday) .. 7 (Sunday), separated by anything else. 0: none, or a word
 * that names no day. */
static uint8_t days_mask(const char *text)
{
    uint8_t mask = 0;
    while (*text) {
        if (*text >= '1' && *text <= '7') {
            mask |= (uint8_t)(1U << ((unsigned)(*text - '0') % 7U));
            ++text;
        } else if (*text >= 'a' && *text <= 'z') {
            unsigned day = 0;
            while (day < 7U && strncmp(text, day_names[day], 3)) ++day;
            if (day == 7U) return 0;
            mask |= (uint8_t)(1U << day);
            while (*text >= 'a' && *text <= 'z') ++text;
        } else {
            ++text;
        }
    }
    return mask;
}

/* The repeat and days arguments as weekday bits; false: invalid. */
static bool repeat_argument(const xiaotai_ai_talk_tool_call_t *call, uint8_t *weekdays)
{
    char repeat[16], days[64];
    const xiaotai_err_t named = xiaotai_ai_talk_tool_arg_word(call, "repeat", repeat, sizeof(repeat));
    const xiaotai_err_t listed = xiaotai_ai_talk_tool_arg_word(call, "days", days, sizeof(days));
    if ((named != XIAOTAI_OK && named != XIAOTAI_ERR_NOT_FOUND) ||
        (listed != XIAOTAI_OK && listed != XIAOTAI_ERR_NOT_FOUND)) return false;
    const uint8_t mask = listed == XIAOTAI_OK ? days_mask(days) : 0;
    if (listed == XIAOTAI_OK && !mask) return false;
    if (named == XIAOTAI_ERR_NOT_FOUND) *weekdays = mask;   /* days alone: those days; neither: once */
    else if (!strcmp(repeat, "once")) *weekdays = 0;
    else if (!strcmp(repeat, "daily") || !strcmp(repeat, "everyday")) *weekdays = XIAOTAI_SCHEDULE_EVERY_DAY;
    else if (!strcmp(repeat, "weekdays") || !strcmp(repeat, "workdays")) *weekdays = WEEKDAYS;
    else if (!strcmp(repeat, "weekends") || !strcmp(repeat, "weekend")) *weekdays = WEEKEND;
    else if ((!strcmp(repeat, "custom") || !strcmp(repeat, "weekly")) && mask) *weekdays = mask;
    else return false;
    return true;
}

/* ------------------------------------------------------------------- alarms */

static void put_alarm(const xiaotai_schedule_info_t *info)
{
    char text[DAYS_BYTES];
    const xiaotai_schedule_t *entry = &info->entry;
    xiaotai_json_put_int(&work->writer, "id", entry->id);
    (void)snprintf(text, sizeof(text), "%02u:%02u", (unsigned)entry->hour, (unsigned)entry->minute);
    xiaotai_json_put_string(&work->writer, "time", text);
    xiaotai_json_put_string(&work->writer, "repeat", repeat_word(entry->weekdays));
    days_text(entry->weekdays, text);
    xiaotai_json_put_string(&work->writer, "days", text);
    xiaotai_json_put_bool(&work->writer, "enabled", entry->enabled);
    xiaotai_json_put_string(&work->writer, "label", entry->label);
    /* Absent while it cannot be known: a disabled alarm, an unset clock. */
    if (info->due_in_s != UINT32_MAX)
        xiaotai_json_put_int(&work->writer, "rings_in_minutes", (info->due_in_s + 59U) / 60U);
}

static bool time_known(void)
{
    xiaotai_wallclock_t now;
    return xiaotai_wallclock_now(&now);
}

/* A one-time alarm rings at the next hour:minute, today or tomorrow. When the
 * call says which day the user meant and that is another one, the alarm would
 * ring on the wrong day: refuse it and say which day it would be. */
static bool day_matches(const xiaotai_ai_talk_tool_call_t *call, const xiaotai_schedule_t *entry, int32_t offset)
{
    xiaotai_wallclock_t now;
    if (entry->weekdays || !xiaotai_wallclock_now(&now)) return true;
    const bool today = entry->hour * 60 + entry->minute > now.local.hour * 60 + now.local.minute;
    if (offset == (today ? 0 : 1)) return true;
    begin(unsupported_date, offset == 0 ? "今天这个时间已经过了，没有添加。请问用户要不要定明天。" :
          today ? "没有添加：今天的这个时间还没到，一次性闹钟今天就会响，定不到别的日子。请告诉用户等今天过了这个时间"
                  "再定，或者改成重复的闹钟；用户要的其实是今天，就用 day_offset 0 重试。" :
                  "没有添加：一次性闹钟最远只能定到明天的这个时刻，请告诉用户现在还定不了那一天。");
    xiaotai_json_put_string(&work->writer, "next_ring_day", today ? "today" : "tomorrow");
    end(call);
    return false;
}

static void add_alarm(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    int32_t hour = 0, minute = 0, offset = 0;
    bool has_minute = false, has_offset = false;
    uint8_t weekdays = 0;
    xiaotai_schedule_t entry = {.kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true};
    const xiaotai_err_t labelled = xiaotai_ai_talk_tool_arg_text(call, "label", entry.label, sizeof(entry.label));
    if (xiaotai_ai_talk_tool_arg_int(call, "hour", &hour) != XIAOTAI_OK || hour < 0 || hour > 23 ||
        !optional_int(call, "minute", &has_minute, &minute) || minute < 0 || minute > 59 ||
        !optional_int(call, "day_offset", &has_offset, &offset) || offset < 0 ||
        !repeat_argument(call, &weekdays) || (labelled != XIAOTAI_OK && labelled != XIAOTAI_ERR_NOT_FOUND)) {
        plain(call, invalid, "参数无效：hour 取 0 到 23，minute 取 0 到 59，repeat 取 once、daily、weekdays、"
                             "weekends 或 custom；custom 要带 days，例如 mon,wed,fri；day_offset 今天是 0、明天是 1。");
        return;
    }
    entry.hour = (uint8_t)hour;
    entry.minute = (uint8_t)minute;
    entry.weekdays = weekdays;
    if (has_offset && !day_matches(call, &entry, offset)) return;
    const char *status = ok;
    uint8_t id = 0;
    xiaotai_err_t err = XIAOTAI_OK;
    /* The same alarm asked for twice stays one alarm. */
    const size_t total = xiaotai_schedule_list(work->rows, XIAOTAI_SCHEDULE_MAX);
    for (size_t i = 0; i < total && !id; ++i) {
        const xiaotai_schedule_t *held = &work->rows[i].entry;
        if (held->kind != XIAOTAI_SCHEDULE_ALARM || held->hour != entry.hour || held->minute != entry.minute ||
            held->weekdays != entry.weekdays) continue;
        id = held->id;
        if (held->enabled) status = exists;
        else err = xiaotai_schedule_enable(id, true);
    }
    if (!id) err = xiaotai_schedule_add(&entry, &id);
    xiaotai_schedule_info_t info;
    if (err == XIAOTAI_OK) err = xiaotai_schedule_get(id, &info);
    if (err != XIAOTAI_OK) {
        plain(call, err == XIAOTAI_ERR_NO_MEM ? full : failed,
              err == XIAOTAI_ERR_NO_MEM ? "闹钟已满，请先删除一个再添加。" : "闹钟没有保存成功。");
        return;
    }
    begin(status, status == exists ? "这个闹钟已经有了，没有重复添加。" :
                  time_known() ? "闹钟已设好。" : "闹钟已保存；设备时间还没有同步，同步后才会响。");
    put_alarm(&info);
    end(call);
}

static void list_alarms(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    const size_t total = xiaotai_schedule_list(work->rows, XIAOTAI_SCHEDULE_MAX);
    size_t count = 0;
    for (size_t i = 0; i < total; ++i) count += work->rows[i].entry.kind == XIAOTAI_SCHEDULE_ALARM;
    begin(count ? ok : none, count ? "闹钟列表已读取。" : "现在没有闹钟。");
    xiaotai_json_put_int(&work->writer, "count", (int64_t)count);
    xiaotai_json_put_bool(&work->writer, "time_synced", time_known());
    xiaotai_json_array_begin(&work->writer, "alarms");
    for (size_t i = 0; i < total; ++i) {
        if (work->rows[i].entry.kind != XIAOTAI_SCHEDULE_ALARM) continue;
        xiaotai_json_object_begin(&work->writer, NULL);
        put_alarm(&work->rows[i]);
        xiaotai_json_object_end(&work->writer);
    }
    xiaotai_json_array_end(&work->writer);
    end(call);
}

static void change_alarm(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    char mode[12];
    int32_t id = 0, hour = 0, minute = 0;
    bool has_id = false, has_hour = false, has_minute = false, all = false;
    const xiaotai_err_t named = xiaotai_ai_talk_tool_arg_word(call, "mode", mode, sizeof(mode));
    const xiaotai_err_t every = xiaotai_ai_talk_tool_arg_bool(call, "all", &all);
    const bool remove = named == XIAOTAI_OK && (!strcmp(mode, "remove") || !strcmp(mode, "delete"));
    const bool enable = named == XIAOTAI_OK && !strcmp(mode, "enable");
    const bool disable = named == XIAOTAI_OK && !strcmp(mode, "disable");
    if ((!remove && !enable && !disable) || (every != XIAOTAI_OK && every != XIAOTAI_ERR_NOT_FOUND) ||
        !optional_int(call, "id", &has_id, &id) || !optional_int(call, "hour", &has_hour, &hour) ||
        !optional_int(call, "minute", &has_minute, &minute) || (has_id && (id < 1 || id > 255)) ||
        (has_hour && (hour < 0 || hour > 23)) || minute < 0 || minute > 59 || (!has_id && !has_hour && !all)) {
        plain(call, invalid, "参数无效：mode 取 remove、enable 或 disable；用 id、hour 和 minute、或 all 为 true "
                             "指明是哪个闹钟。");
        return;
    }
    const size_t total = xiaotai_schedule_list(work->rows, XIAOTAI_SCHEDULE_MAX);
    size_t changed = 0, alarms = 0;
    bool refused = false;
    for (size_t i = 0; i < total; ++i) {
        const xiaotai_schedule_t *held = &work->rows[i].entry;
        if (held->kind != XIAOTAI_SCHEDULE_ALARM) continue;
        ++alarms;
        if (has_id ? held->id != id : !all && (held->hour != hour || held->minute != minute)) continue;
        const xiaotai_err_t err = remove ? xiaotai_schedule_remove(held->id) :
                                           xiaotai_schedule_enable(held->id, enable);
        if (err == XIAOTAI_OK) ++changed;
        else refused = true;
    }
    const char *status = refused ? failed : changed ? ok : not_found;
    begin(status, refused ? "有闹钟没有修改成功。" : !changed ? "没有找到这个闹钟。" :
                  remove ? "闹钟已删除。" : enable ? "闹钟已打开。" : "闹钟已关闭。");
    xiaotai_json_put_int(&work->writer, "count", (int64_t)changed);
    xiaotai_json_put_int(&work->writer, "remaining", (int64_t)(alarms - (remove ? changed : 0U)));
    end(call);
}

/* ---------------------------------------------------------------- countdown */

static void put_timer(const xiaotai_schedule_info_t *info)
{
    xiaotai_json_put_int(&work->writer, "id", info->entry.id);
    xiaotai_json_put_int(&work->writer, "remaining_seconds", info->due_in_s);
    xiaotai_json_put_string(&work->writer, "label", info->entry.label);
}

static void start_timer(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    int32_t minutes = 0, seconds = 0;
    bool has_minutes = false, has_seconds = false;
    xiaotai_schedule_t entry = {.kind = XIAOTAI_SCHEDULE_COUNTDOWN, .enabled = true};
    const xiaotai_err_t labelled = xiaotai_ai_talk_tool_arg_text(call, "label", entry.label, sizeof(entry.label));
    const bool read = optional_int(call, "minutes", &has_minutes, &minutes) &&
                      optional_int(call, "seconds", &has_seconds, &seconds) &&
                      (labelled == XIAOTAI_OK || labelled == XIAOTAI_ERR_NOT_FOUND);
    const int64_t length = (int64_t)minutes * 60 + seconds;
    if (!read || minutes < 0 || seconds < 0 || length < 1 || length > (int64_t)XIAOTAI_SCHEDULE_COUNTDOWN_MAX_S) {
        plain(call, invalid, "参数无效：minutes 和 seconds 至少给一个，总时长在 1 秒到 24 小时之间。");
        return;
    }
    entry.seconds = (uint32_t)length;
    uint8_t id = 0;
    const xiaotai_err_t err = xiaotai_schedule_add(&entry, &id);
    if (err != XIAOTAI_OK) {
        plain(call, err == XIAOTAI_ERR_NO_MEM ? full : failed,
              err == XIAOTAI_ERR_NO_MEM ? "闹钟和倒计时已满，请先删除一个。" : "倒计时没有开始。");
        return;
    }
    /* Due during the conversation it does not interrupt and rings after it. */
    begin(ok, entry.seconds < SHORT_TIMER_S ?
          "倒计时已开始。到时间时如果还在对话，会等对话结束再响，请告诉用户说完就结束对话。" : "倒计时已开始。");
    xiaotai_json_put_int(&work->writer, "id", id);
    xiaotai_json_put_int(&work->writer, "seconds", entry.seconds);
    xiaotai_json_put_string(&work->writer, "label", entry.label);
    end(call);
}

static void cancel_timer(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    int32_t id = 0;
    bool has_id = false;
    if (!optional_int(call, "id", &has_id, &id) || (has_id && (id < 1 || id > 255))) {
        plain(call, invalid, "参数无效：id 是 get_timer 返回的编号，不给就取消全部倒计时。");
        return;
    }
    const size_t total = xiaotai_schedule_list(work->rows, XIAOTAI_SCHEDULE_MAX);
    size_t cancelled = 0;
    for (size_t i = 0; i < total; ++i) {
        const xiaotai_schedule_t *held = &work->rows[i].entry;
        if (held->kind != XIAOTAI_SCHEDULE_COUNTDOWN || (has_id && held->id != id)) continue;
        cancelled += xiaotai_schedule_remove(held->id) == XIAOTAI_OK;
    }
    begin(cancelled ? ok : not_found, cancelled ? "倒计时已取消。" : "现在没有在跑的倒计时。");
    xiaotai_json_put_int(&work->writer, "count", (int64_t)cancelled);
    end(call);
}

static void get_timer(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    const size_t total = xiaotai_schedule_list(work->rows, XIAOTAI_SCHEDULE_MAX);
    const xiaotai_schedule_info_t *soonest = NULL;
    size_t count = 0;
    for (size_t i = 0; i < total; ++i) {
        if (work->rows[i].entry.kind != XIAOTAI_SCHEDULE_COUNTDOWN) continue;
        ++count;
        if (!soonest || work->rows[i].due_in_s < soonest->due_in_s) soonest = &work->rows[i];
    }
    begin(count ? ok : none, count ? "倒计时已读取。" : "现在没有在跑的倒计时。");
    xiaotai_json_put_int(&work->writer, "count", (int64_t)count);
    /* The one that ends first, so a single countdown needs no list. */
    if (soonest) put_timer(soonest);
    xiaotai_json_array_begin(&work->writer, "timers");
    for (size_t i = 0; i < total; ++i) {
        if (work->rows[i].entry.kind != XIAOTAI_SCHEDULE_COUNTDOWN) continue;
        xiaotai_json_object_begin(&work->writer, NULL);
        put_timer(&work->rows[i]);
        xiaotai_json_object_end(&work->writer);
    }
    xiaotai_json_array_end(&work->writer);
    end(call);
}

/* ---------------------------------------------------------------- lifecycle */

xiaotai_err_t alarm_ai_tools_init(void)
{
    static const struct {
        const char *name;
        xiaotai_ai_talk_tool_fn_t handler;
    } tools[] = {
        {"add_alarm", add_alarm}, {"list_alarms", list_alarms}, {"change_alarm", change_alarm},
        {"start_timer", start_timer}, {"cancel_timer", cancel_timer}, {"get_timer", get_timer},
    };
    xiaotai_err_t err = XIAOTAI_OK;
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(tools) / sizeof(tools[0]); ++i)
        err = xiaotai_ai_talk_tool_register(tools[i].name, tools[i].handler, NULL);
    if (err != XIAOTAI_OK) return err;
    if (!work) work = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*work));
    return work ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

void alarm_ai_tools_reset(void)
{
    xiaotai_mem_free(work);
    work = NULL;
}
