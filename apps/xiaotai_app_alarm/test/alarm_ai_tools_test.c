// SPDX-License-Identifier: MIT
// The alarm application's conversation tools (alarm_ai_tools.c) against a
// fake of the clock module's schedules and of the tool registry: every tool's
// normal call, what is refused, and what the answer tells the model.
#include "alarm_ai_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xiaotai/json.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/clock.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\nreply: %s\n", __FILE__, __LINE__, #condition, reply_text); abort(); \
} } while (0)

/* ------------------------------------------------------------------- fakes */

static struct {
    const char *name;
    xiaotai_ai_talk_tool_fn_t handler;
} tools[8];
static size_t tool_count;
static char reply_text[XIAOTAI_AI_TALK_TOOL_REPLY_MAX];
static unsigned replies;
static bool synced = true;
/* Tuesday 21:10. */
static xiaotai_clock_fields_t local = {.year = 2026, .month = 11, .day = 24, .hour = 21, .minute = 10, .weekday = 2};
static xiaotai_schedule_t entries[XIAOTAI_SCHEDULE_MAX];
static size_t entry_count;
static uint8_t next_id = 1;
static xiaotai_err_t store_error;

xiaotai_err_t xiaotai_ai_talk_tool_register(const char *name, xiaotai_ai_talk_tool_fn_t handler, void *ctx)
{
    CHECK(tool_count < sizeof(tools) / sizeof(tools[0]) && !ctx);
    tools[tool_count].name = name;
    tools[tool_count++].handler = handler;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_tool_reply(uint32_t session, uint32_t request, const char *object)
{
    CHECK(session == 7 && request == 9 && strlen(object) < sizeof(reply_text));
    strcpy(reply_text, object);
    ++replies;
    return XIAOTAI_OK;
}

bool xiaotai_wallclock_now(xiaotai_wallclock_t *out)
{
    *out = (xiaotai_wallclock_t){.synced = synced, .local = local};
    return synced;
}

static uint32_t due_in(const xiaotai_schedule_t *e)
{
    if (e->kind == XIAOTAI_SCHEDULE_COUNTDOWN) return e->seconds;
    if (!e->enabled || !synced) return UINT32_MAX;
    const uint32_t now = (uint32_t)local.hour * 60U + local.minute, at = (uint32_t)e->hour * 60U + e->minute;
    for (uint32_t day = 0; day < 8U; ++day) {
        const uint32_t weekday = (local.weekday + day) % 7U;
        if (e->weekdays && !(e->weekdays & (1U << weekday))) continue;
        if (day == 0 && at <= now) continue;
        return (day * 1440U + at - now) * 60U;
    }
    return UINT32_MAX;
}

xiaotai_err_t xiaotai_schedule_add(const xiaotai_schedule_t *entry, uint8_t *id)
{
    if (store_error != XIAOTAI_OK) return store_error;
    if (entry_count == XIAOTAI_SCHEDULE_MAX) return XIAOTAI_ERR_NO_MEM;
    CHECK(memchr(entry->label, 0, sizeof(entry->label)));
    entries[entry_count] = *entry;
    entries[entry_count].id = next_id++;
    if (id) *id = entries[entry_count].id;
    ++entry_count;
    return XIAOTAI_OK;
}

static xiaotai_schedule_t *find(uint8_t id)
{
    for (size_t i = 0; i < entry_count; ++i) {
        if (entries[i].id == id) return &entries[i];
    }
    return NULL;
}

xiaotai_err_t xiaotai_schedule_remove(uint8_t id)
{
    xiaotai_schedule_t *entry = find(id);
    if (!entry) return XIAOTAI_ERR_NOT_FOUND;
    if (store_error != XIAOTAI_OK) return store_error;
    memmove(entry, entry + 1, (size_t)(&entries[entry_count] - (entry + 1)) * sizeof(*entry));
    --entry_count;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_schedule_enable(uint8_t id, bool enabled)
{
    xiaotai_schedule_t *entry = find(id);
    if (!entry) return XIAOTAI_ERR_NOT_FOUND;
    if (entry->kind != XIAOTAI_SCHEDULE_ALARM) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (store_error != XIAOTAI_OK) return store_error;
    entry->enabled = enabled;
    return XIAOTAI_OK;
}

/* Alarms first, each group in the order added, as the module lists them. */
size_t xiaotai_schedule_list(xiaotai_schedule_info_t *out, size_t capacity)
{
    size_t n = 0;
    for (int kind = XIAOTAI_SCHEDULE_ALARM; kind <= XIAOTAI_SCHEDULE_COUNTDOWN; ++kind) {
        for (size_t i = 0; i < entry_count && n < capacity; ++i) {
            if ((int)entries[i].kind == kind) out[n++] = (xiaotai_schedule_info_t){entries[i], due_in(&entries[i])};
        }
    }
    return n;
}

xiaotai_err_t xiaotai_schedule_get(uint8_t id, xiaotai_schedule_info_t *info)
{
    const xiaotai_schedule_t *entry = find(id);
    if (!entry) return XIAOTAI_ERR_NOT_FOUND;
    *info = (xiaotai_schedule_info_t){*entry, due_in(entry)};
    return XIAOTAI_OK;
}

/* ----------------------------------------------------------------- helpers */

static void boot(void)
{
    alarm_ai_tools_reset();
    tool_count = entry_count = 0;
    next_id = 1;
    synced = true;
    store_error = XIAOTAI_OK;
    CHECK(alarm_ai_tools_init() == XIAOTAI_OK && tool_count == 6);
}

static void invoke(const char *name, const char *json)
{
    for (size_t i = 0; i < tool_count; ++i) {
        if (strcmp(tools[i].name, name)) continue;
        const xiaotai_ai_talk_tool_call_t call = {.session = 7, .request = 9, .name = name, .arguments = json,
            .size = strlen(json), .device_action = true, .has_arguments = strcmp(json, "{}") != 0};
        reply_text[0] = 0;
        const unsigned before = replies;
        tools[i].handler(&call, NULL);
        CHECK(replies == before + 1U);
        return;
    }
    CHECK(!"the tool is not registered");
}

static xiaotai_json_token_t tokens[512];
static xiaotai_json_doc_t doc;

static int field(const char *path)
{
    size_t count = 0;
    CHECK(xiaotai_json_parse(reply_text, strlen(reply_text), tokens, 512, &count, &doc) == XIAOTAI_OK);
    return xiaotai_json_find(&doc, 0, path);
}

static int64_t number(const char *path)
{
    int64_t value = -1;
    CHECK(xiaotai_json_int(&doc, field(path), &value) == XIAOTAI_OK);
    return value;
}

static bool flag(const char *path)
{
    bool value = false;
    CHECK(xiaotai_json_bool(&doc, field(path), &value) == XIAOTAI_OK);
    return value;
}

static bool text_is(const char *path, const char *expected) { return xiaotai_json_equals(&doc, field(path), expected); }

static bool answered(bool ok, const char *status)
{
    char message[400];
    CHECK(xiaotai_json_string(&doc, field("message"), message, sizeof(message), NULL) == XIAOTAI_OK && message[0]);
    return flag("ok") == ok && text_is("status", status);
}

/* ------------------------------------------------------------------- cases */

static void add(void)
{
    boot();
    /* "Wake me at seven": once, at the next seven o'clock, 9 h 50 min from 21:10. */
    invoke("add_alarm", "{\"hour\":7,\"minute\":0}");
    CHECK(answered(true, "ok") && number("id") == 1 && text_is("time", "07:00") && text_is("repeat", "once"));
    CHECK(text_is("days", "") && flag("enabled") && text_is("label", "") && number("rings_in_minutes") == 590);
    CHECK(entry_count == 1 && entries[0].kind == XIAOTAI_SCHEDULE_ALARM && entries[0].weekdays == 0);
    invoke("add_alarm", "{\"hour\":\"8\",\"repeat\":\"weekdays\",\"label\":\"\xe4\xb8\x8a\xe7\x8f\xad\"}");
    CHECK(answered(true, "ok") && number("id") == 2 && text_is("time", "08:00") && text_is("repeat", "weekdays"));
    CHECK(text_is("days", "mon,tue,wed,thu,fri") && text_is("label", "\xe4\xb8\x8a\xe7\x8f\xad"));
    CHECK(entries[1].weekdays == 0x3E && entries[1].minute == 0);
    invoke("add_alarm", "{\"hour\":6,\"minute\":30,\"repeat\":\"Daily\"}");
    CHECK(answered(true, "ok") && entries[2].weekdays == 0x7F && text_is("repeat", "daily"));
    invoke("add_alarm", "{\"hour\":9,\"minute\":15,\"repeat\":\"weekends\"}");
    CHECK(answered(true, "ok") && entries[3].weekdays == 0x41 && text_is("days", "sat,sun"));
    invoke("add_alarm", "{\"hour\":19,\"minute\":45,\"repeat\":\"custom\",\"days\":\"Mon, Wed,friday\"}");
    CHECK(answered(true, "ok") && entries[4].weekdays == 0x2A && text_is("repeat", "custom"));
    CHECK(text_is("days", "mon,wed,fri"));
    /* Days alone are enough, by name or by number (1 Monday .. 7 Sunday). */
    invoke("add_alarm", "{\"hour\":20,\"minute\":0,\"days\":\"2,4,7\"}");
    CHECK(answered(true, "ok") && entries[5].weekdays == 0x15 && text_is("days", "tue,thu,sun"));
    /* A label longer than the module keeps is cut between characters. */
    invoke("add_alarm", "{\"hour\":22,\"minute\":0,\"label\":\"\xe5\x90\x83\xe8\x8d\xaf\xe5\x90\x83\xe8\x8d\xaf\xe5\x90\x83"
                        "\xe8\x8d\xaf\xe5\x90\x83\xe8\x8d\xaf\xe5\x90\x83\"}");
    CHECK(answered(true, "ok") && strlen(entries[6].label) == 21);
    CHECK(entry_count == 7);

    /* The same alarm again is the same alarm; a disabled one is switched on. */
    invoke("add_alarm", "{\"hour\":7,\"minute\":0,\"repeat\":\"once\"}");
    CHECK(answered(true, "exists") && number("id") == 1 && entry_count == 7);
    entries[0].enabled = false;
    invoke("add_alarm", "{\"hour\":7}");
    CHECK(answered(true, "ok") && number("id") == 1 && entry_count == 7 && entries[0].enabled);
    /* Another repeat at the same time is another alarm. */
    invoke("add_alarm", "{\"hour\":7,\"minute\":0,\"repeat\":\"daily\"}");
    CHECK(answered(true, "ok") && number("id") == 8 && entry_count == 8);

    /* An unset clock: saved, and nothing says when it rings. */
    synced = false;
    invoke("add_alarm", "{\"hour\":5,\"minute\":5,\"day_offset\":5}");
    CHECK(answered(true, "ok") && field("rings_in_minutes") < 0 && entry_count == 9);
    synced = true;
}

/* A one-time alarm rings at the next occurrence, so a day the user named that
 * is another one is refused instead of ringing on the wrong day. 21:10 now. */
static void add_day(void)
{
    boot();
    invoke("add_alarm", "{\"hour\":7,\"day_offset\":1}");
    CHECK(answered(true, "ok") && entry_count == 1 && number("rings_in_minutes") == 590);
    invoke("add_alarm", "{\"hour\":22,\"day_offset\":\"0\"}");
    CHECK(answered(true, "ok") && entry_count == 2 && number("rings_in_minutes") == 50);
    /* The day after tomorrow. */
    invoke("add_alarm", "{\"hour\":8,\"day_offset\":2}");
    CHECK(answered(false, "unsupported_date") && text_is("next_ring_day", "tomorrow") && entry_count == 2);
    /* Tomorrow at a time that is still to come today would ring today. */
    invoke("add_alarm", "{\"hour\":23,\"day_offset\":1}");
    CHECK(answered(false, "unsupported_date") && text_is("next_ring_day", "today") && entry_count == 2);
    /* Today at a time that has passed, the present minute included. */
    invoke("add_alarm", "{\"hour\":21,\"minute\":10,\"day_offset\":0}");
    CHECK(answered(false, "unsupported_date") && text_is("next_ring_day", "tomorrow") && entry_count == 2);
    /* A repeating alarm has no day; a day that is not a number is refused. */
    invoke("add_alarm", "{\"hour\":8,\"repeat\":\"weekdays\",\"day_offset\":3}");
    CHECK(answered(true, "ok") && entry_count == 3);
    invoke("add_alarm", "{\"hour\":8,\"day_offset\":\"tomorrow\"}");
    CHECK(answered(false, "invalid_params") && entry_count == 3);
    invoke("add_alarm", "{\"hour\":8,\"day_offset\":-1}");
    CHECK(answered(false, "invalid_params") && entry_count == 3);
}

static void add_refusals(void)
{
    boot();
    static const char *const bad[] = {
        "{}", "{\"minute\":30}", "{\"hour\":24,\"minute\":0}", "{\"hour\":-1}", "{\"hour\":7,\"minute\":60}",
        "{\"hour\":\"seven\"}", "{\"hour\":7,\"repeat\":\"monthly\"}", "{\"hour\":7,\"repeat\":\"custom\"}",
        "{\"hour\":7,\"repeat\":\"custom\",\"days\":\"someday\"}", "{\"hour\":7,\"days\":\"\xe5\x91\xa8\xe4\xb8\x80\"}",
        "{\"hour\":7,\"label\":3}", "{\"hour\":7.5}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        invoke("add_alarm", bad[i]);
        CHECK(answered(false, "invalid_params") && entry_count == 0);
    }
    for (unsigned i = 0; i < XIAOTAI_SCHEDULE_MAX; ++i) {
        char json[48];
        snprintf(json, sizeof(json), "{\"hour\":%u,\"minute\":0}", i);
        invoke("add_alarm", json);
        CHECK(answered(true, "ok"));
    }
    invoke("add_alarm", "{\"hour\":23,\"minute\":59}");
    CHECK(answered(false, "full") && entry_count == XIAOTAI_SCHEDULE_MAX);
    invoke("start_timer", "{\"minutes\":1}");
    CHECK(answered(false, "full"));
    boot();
    store_error = XIAOTAI_FAIL;
    invoke("add_alarm", "{\"hour\":7}");
    CHECK(answered(false, "failed") && entry_count == 0);
}

static void list(void)
{
    boot();
    invoke("list_alarms", "{}");
    CHECK(answered(true, "none") && number("count") == 0 && flag("time_synced") && field("alarms[0]") < 0);
    invoke("add_alarm", "{\"hour\":7,\"minute\":30,\"repeat\":\"weekdays\",\"label\":\"work\"}");
    invoke("start_timer", "{\"minutes\":5}");
    invoke("add_alarm", "{\"hour\":9,\"minute\":0}");
    entries[2].enabled = false;
    invoke("list_alarms", "{}");
    CHECK(answered(true, "ok") && number("count") == 2 && field("alarms[2]") < 0);
    CHECK(number("alarms[0].id") == 1 && text_is("alarms[0].time", "07:30") && text_is("alarms[0].repeat", "weekdays"));
    CHECK(text_is("alarms[0].days", "mon,tue,wed,thu,fri") && flag("alarms[0].enabled"));
    CHECK(text_is("alarms[0].label", "work") && number("alarms[0].rings_in_minutes") == 620);
    CHECK(number("alarms[1].id") == 3 && !flag("alarms[1].enabled") && field("alarms[1].rings_in_minutes") < 0);
    /* The longest list the module can hold still fits one answer. */
    boot();
    for (unsigned i = 0; i < XIAOTAI_SCHEDULE_MAX; ++i) {
        char json[160];
        snprintf(json, sizeof(json), "{\"hour\":%u,\"minute\":59,\"repeat\":\"daily\",\"label\":\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\""
                 "\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\"}", i);
        invoke("add_alarm", json);
        CHECK(answered(true, "ok") && strlen(entries[i].label) == XIAOTAI_SCHEDULE_LABEL_MAX);
    }
    invoke("list_alarms", "{}");
    CHECK(answered(true, "ok") && number("count") == XIAOTAI_SCHEDULE_MAX && number("alarms[15].id") == 16);
}

static void change(void)
{
    boot();
    invoke("add_alarm", "{\"hour\":7,\"minute\":0}");
    invoke("add_alarm", "{\"hour\":7,\"minute\":0,\"repeat\":\"weekdays\"}");
    invoke("add_alarm", "{\"hour\":8,\"minute\":30}");
    invoke("start_timer", "{\"minutes\":5}");
    /* "Turn the seven o'clock alarm off": every alarm at that time. */
    invoke("change_alarm", "{\"mode\":\"disable\",\"hour\":7}");
    CHECK(answered(true, "ok") && number("count") == 2 && number("remaining") == 3);
    CHECK(!entries[0].enabled && !entries[1].enabled && entries[2].enabled);
    invoke("change_alarm", "{\"mode\":\"enable\",\"id\":2}");
    CHECK(answered(true, "ok") && number("count") == 1 && !entries[0].enabled && entries[1].enabled);
    invoke("change_alarm", "{\"mode\":\"remove\",\"hour\":8,\"minute\":30}");
    CHECK(answered(true, "ok") && number("count") == 1 && number("remaining") == 2 && entry_count == 3);
    invoke("change_alarm", "{\"mode\":\"remove\",\"hour\":8,\"minute\":30}");
    CHECK(answered(false, "not_found") && number("count") == 0 && number("remaining") == 2);
    invoke("change_alarm", "{\"mode\":\"delete\",\"id\":9}");
    CHECK(answered(false, "not_found") && entry_count == 3);
    /* An id that names the countdown is not an alarm. */
    invoke("change_alarm", "{\"mode\":\"remove\",\"id\":4}");
    CHECK(answered(false, "not_found") && entry_count == 3);
    invoke("change_alarm", "{\"mode\":\"remove\",\"all\":true}");
    CHECK(answered(true, "ok") && number("count") == 2 && number("remaining") == 0);
    CHECK(entry_count == 1 && entries[0].kind == XIAOTAI_SCHEDULE_COUNTDOWN);
    invoke("change_alarm", "{\"mode\":\"remove\",\"all\":true}");
    CHECK(answered(false, "not_found"));

    static const char *const bad[] = {
        "{}", "{\"mode\":\"remove\"}", "{\"mode\":\"remove\",\"all\":false}", "{\"mode\":\"snooze\",\"id\":1}",
        "{\"id\":1}", "{\"mode\":\"remove\",\"id\":0}", "{\"mode\":\"remove\",\"id\":300}",
        "{\"mode\":\"remove\",\"hour\":25}", "{\"mode\":\"remove\",\"hour\":7,\"minute\":61}",
        "{\"mode\":\"remove\",\"all\":\"maybe\"}", "{\"mode\":\"remove\",\"minute\":30}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        invoke("change_alarm", bad[i]);
        CHECK(answered(false, "invalid_params") && entry_count == 1);
    }
    boot();
    invoke("add_alarm", "{\"hour\":7}");
    store_error = XIAOTAI_FAIL;
    invoke("change_alarm", "{\"mode\":\"remove\",\"id\":1}");
    CHECK(answered(false, "failed") && number("count") == 0 && entry_count == 1);
}

static void timers(void)
{
    boot();
    invoke("get_timer", "{}");
    CHECK(answered(true, "none") && number("count") == 0 && field("remaining_seconds") < 0);
    invoke("cancel_timer", "{}");
    CHECK(answered(false, "not_found") && number("count") == 0);
    invoke("start_timer", "{\"minutes\":10,\"label\":\"\xe5\x85\xb3\xe7\x81\xab\"}");
    CHECK(answered(true, "ok") && number("id") == 1 && number("seconds") == 600 && text_is("label", "\xe5\x85\xb3\xe7\x81\xab"));
    CHECK(entries[0].kind == XIAOTAI_SCHEDULE_COUNTDOWN && entries[0].seconds == 600);
    CHECK(!strstr(reply_text, "结束对话"));
    invoke("start_timer", "{\"minutes\":1,\"seconds\":30}");
    CHECK(answered(true, "ok") && number("id") == 2 && number("seconds") == 90);
    /* It may end while they still talk, and then it does not ring. */
    CHECK(strstr(reply_text, "结束对话"));
    invoke("start_timer", "{\"seconds\":\"45\"}");
    CHECK(answered(true, "ok") && number("seconds") == 45);
    invoke("start_timer", "{\"minutes\":1440}");
    CHECK(answered(true, "ok") && number("seconds") == 86400);
    invoke("add_alarm", "{\"hour\":7}");
    /* The one that ends first on top, all of them in the list, alarms left out. */
    invoke("get_timer", "{}");
    CHECK(answered(true, "ok") && number("count") == 4 && number("id") == 3 && number("remaining_seconds") == 45);
    CHECK(number("timers[0].id") == 1 && number("timers[0].remaining_seconds") == 600);
    CHECK(text_is("timers[0].label", "\xe5\x85\xb3\xe7\x81\xab") && field("timers[4]") < 0);
    invoke("cancel_timer", "{\"id\":2}");
    CHECK(answered(true, "ok") && number("count") == 1 && entry_count == 4);
    invoke("cancel_timer", "{\"id\":2}");
    CHECK(answered(false, "not_found") && entry_count == 4);
    /* An alarm's id cancels nothing. */
    invoke("cancel_timer", "{\"id\":5}");
    CHECK(answered(false, "not_found") && entry_count == 4);
    invoke("cancel_timer", "{}");
    CHECK(answered(true, "ok") && number("count") == 3 && entry_count == 1);
    CHECK(entries[0].kind == XIAOTAI_SCHEDULE_ALARM);

    static const char *const bad[] = {
        "{}", "{\"minutes\":0}", "{\"minutes\":0,\"seconds\":0}", "{\"minutes\":-1,\"seconds\":120}",
        "{\"minutes\":1441}", "{\"seconds\":86401}", "{\"minutes\":\"ten\"}", "{\"minutes\":1,\"label\":[1]}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        invoke("start_timer", bad[i]);
        CHECK(answered(false, "invalid_params") && entry_count == 1);
    }
    invoke("cancel_timer", "{\"id\":0}");
    CHECK(answered(false, "invalid_params"));
    invoke("cancel_timer", "{\"id\":\"all\"}");
    CHECK(answered(false, "invalid_params"));
}

static void notifications(void)
{
    boot();
    const xiaotai_ai_talk_tool_call_t notice = {.session = 7, .name = "add_alarm", .arguments = "{\"hour\":7}",
        .size = 10};
    const unsigned before = replies;
    for (size_t i = 0; i < tool_count; ++i) tools[i].handler(&notice, NULL);
    CHECK(replies == before && entry_count == 0);
}

int main(void)
{
    add();
    add_day();
    add_refusals();
    list();
    change();
    timers();
    notifications();
    alarm_ai_tools_reset();
    puts("PASS: alarms and countdowns are set, listed and changed by the conversation's tools");
    return 0;
}
