// SPDX-License-Identifier: MIT
#include "clock_test.h"

#include <stdio.h>

static xiaotai_schedule_t alarm_at(unsigned hour, unsigned minute, unsigned weekdays)
{
    return (xiaotai_schedule_t){.kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true,
        .hour = (uint8_t)hour, .minute = (uint8_t)minute, .weekdays = (uint8_t)weekdays};
}

static void test_alarm(void)
{
    prepare(SUN_2026_01_04 + INT64_C(28790000)); /* Sun 07:59:50 UTC */
    assert(start(NULL, "UTC0", true) == XIAOTAI_OK && seen.minutes == 1 && seen.syncs == 1);
    const uint8_t sun = add(alarm_at(8, 0, 1));
    const uint8_t mon = add(alarm_at(8, 0, 2));
    const uint8_t once = add(alarm_at(8, 0, 0));
    assert(get(sun).due_in_s == 10 && get(mon).due_in_s == 86410);
    xiaotai_schedule_info_t nearest;
    assert(xiaotai_schedule_next(&nearest) == XIAOTAI_OK && nearest.entry.id == sun);
    advance(10000, 10000);
    assert(seen.due == 2 && seen.ids[0] == sun && seen.ids[1] == once && seen.minutes == 2);
    assert(!get(once).entry.enabled && get(once).due_in_s == UINT32_MAX);
    advance(30000, 30000);
    assert(seen.due == 2 && get(sun).due_in_s == 604770);

    const uint8_t monday_midnight = add(alarm_at(0, 0, 2));
    const int64_t gap = INT64_C(86400000) - INT64_C(28830000);
    advance(gap + 2000, gap + 2000);
    assert(seen.due == 3 && seen.ids[2] == monday_midnight);
    assert(seen.minute.local.day == 5 && seen.minute.local.weekday == 1 && seen.minute.local.hour == 0);
    const uint8_t missed = add(alarm_at(0, 1, 2));
    const uint32_t before = stats().missed_alarms;
    advance(125000, 125000);
    assert(seen.due == 3 && get(missed).entry.enabled && stats().missed_alarms == before + 1);
    stop();
}

static void update_owner(void *ctx) { assert(xiaotai_schedule_update(ctx) == XIAOTAI_OK); }

static void again_owner(void *ctx)
{
    const xiaotai_schedule_t none = {.kind = XIAOTAI_SCHEDULE_COUNTDOWN, .seconds = 4};
    assert(xiaotai_schedule_due_again(&none) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_schedule_due_again(NULL) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_schedule_due_again(ctx) == XIAOTAI_OK);
}

static void test_countdown(void)
{
    prepare(SUN_2026_01_04);
    const xiaotai_clock_module_config_t cfg = {.timezone_setting = true};
    assert(start(&cfg, "UTC0", false) == XIAOTAI_OK && !seen.syncs);
    const uint8_t alarm = add(alarm_at(0, 0, 0));
    xiaotai_schedule_t timer = {.kind = XIAOTAI_SCHEDULE_COUNTDOWN, .seconds = 4, .label = "tea"};
    timer.id = add(timer);
    assert(get(timer.id).entry.enabled && get(timer.id).due_in_s == 4 && get(alarm).due_in_s == UINT32_MAX);
    advance(2000, 6000000);
    assert(!seen.due && get(timer.id).due_in_s <= 2);
    timer.seconds = 3;
    run(update_owner, &timer);
    set_zone("JST-9");
    advance(2000, -5800000);
    assert(!seen.due && !seen.syncs);
    advance(1100, 1100);
    assert(seen.due == 1 && seen.ids[0] == timer.id && stats().fired_countdowns == 1);
    xiaotai_schedule_info_t info;
    assert(xiaotai_schedule_get(timer.id, &info) == XIAOTAI_ERR_NOT_FOUND);
    /* Announced again although it is gone; it does not fire again. */
    assert(xiaotai_schedule_due_again(&timer) == XIAOTAI_ERR_INVALID_STATE);
    seen.again = true;
    run(again_owner, &timer);
    seen.again = false;
    assert(seen.due == 2 && seen.ids[1] == timer.id && stats().fired_countdowns == 1);

    const xiaotai_args_t timer_intent = {.value = 30, .text = "later"};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_CLOCK_COUNTDOWN, &timer_intent) == XIAOTAI_OK);
    assert(xiaotai_intent_post(XIAOTAI_INTENT_CLOCK_COUNTDOWN, &timer_intent) == XIAOTAI_OK);
    drain();
    xiaotai_schedule_info_t rows[3];
    assert(xiaotai_schedule_list(rows, 3) == 3 && rows[0].entry.id == alarm && strcmp(rows[2].entry.label, "later") == 0);
    const xiaotai_args_t remove = {.value = 0};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_CLOCK_REMOVE, &remove) == XIAOTAI_OK);
    drain();
    assert(xiaotai_schedule_list(rows, 3) == 1 && rows[0].entry.id == alarm);
    const xiaotai_args_t disable = {.value = alarm, .text = "0"};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_CLOCK_ENABLE, &disable) == XIAOTAI_OK);
    drain();
    assert(!get(alarm).entry.enabled);
    stop();
}

static const char eastern[] = "EST5EDT,M3.2.0/2,M11.1.0/2";

static void test_step(void)
{
    prepare(INT64_C(1793510995000)); /* Fall-back Sunday, 01:29:55 EDT. */
    const xiaotai_clock_module_config_t cfg = {.timezone_setting = true};
    assert(start(&cfg, eastern, true) == XIAOTAI_OK);
    const uint8_t id = add(alarm_at(1, 30, 1));
    assert(get(id).due_in_s == 5);
    advance(5000, 5000);
    assert(seen.due == 1 && seen.ids[0] == id && seen.minute.local.utc_offset_s == -14400);
    advance(3600000, 3600000);
    assert(seen.due == 1 && !stats().missed_alarms && !stats().steps);
    assert(get(id).due_in_s == 604800);
    char effective[XIAOTAI_CLOCK_NAME_MAX];
    const unsigned before_zone = seen.minutes;
    set_zone("UTC0");
    assert(seen.minute.local.hour == 6 && seen.minute.local.utc_offset_s == 0);
    assert(stats().steps == 1 && seen.minutes == before_zone + 1 && seen.due == 1);
    set_zone("");
    assert(xiaotai_clock_timezone(effective, sizeof(effective)) == XIAOTAI_OK && !strcmp(effective, eastern));
    assert(seen.minute.local.hour == 1 && seen.minute.local.utc_offset_s == -18000 && stats().steps == 2);
    advance(60000, -3600000); /* Back into the first occurrence, not replayed. */
    assert(stats().steps == 3 && seen.due == 1 && seen.syncs == 1);
    stop();
}

static void test_spring(void)
{
    prepare(INT64_C(1772953195000)); /* Spring-forward Sunday, 01:59:55 EST. */
    assert(start(NULL, eastern, true) == XIAOTAI_OK);
    const uint8_t id = add(alarm_at(2, 30, 1));
    assert(get(id).due_in_s == 603005);
    advance(5000, 5000);
    assert(!seen.due && seen.minute.local.hour == 3 && seen.minute.local.utc_offset_s == -14400);
    assert(!stats().missed_alarms && get(id).due_in_s == 603000);
    stop();
}

static void test_gate(void)
{
    prepare(SUN_2026_01_04 + INT64_C(28817000)); /* Plausible RTC, not trusted. */
    assert(start(NULL, "UTC0", false) == XIAOTAI_OK);
    xiaotai_wallclock_t wall;
    assert(!xiaotai_wallclock_now(&wall) && !seen.syncs && !seen.minutes && clock_init_calls == 1);
    const uint8_t id = add(alarm_at(8, 0, 1));
    advance(70000, 70000);
    assert(!seen.due && !xiaotai_wallclock_now(&wall) && get(id).due_in_s == UINT32_MAX);
    assert(!stats().sync_requests);  /* Platform quiet: observe, don't start NTP. */
    xiaotai_clock_stats_t port;
    xiaotai_clock_stats(&port);
    assert(!port.starts);
    assert(xiaotai_clock_set_unix_ms(wall.unix_ms) == XIAOTAI_OK);
    advance(1000, 1000);
    assert(xiaotai_wallclock_now(&wall) && seen.syncs == 1 && seen.minutes == 1 && !seen.due);
    assert(stats().missed_alarms == 1 && !stats().sync_requests);
    stop();
}

static void test_config(void)
{
    prepare(SUN_2026_01_04);
    assert(start(NULL, NULL, false) == XIAOTAI_OK);
    assert(clock_init_calls == 1 && !strcmp(clock_tz, "CST-8") && clock_server_count == 2);
    assert(strcmp(clock_hosts[0], "ntp.aliyun.com") == 0 && strcmp(clock_hosts[1], "pool.ntp.org") == 0);
    assert(stats().sync_requests == 1 && !seen.syncs && xiaotai_clock_host_probes() == 0);
    advance(4000, 4000);
    assert(stats().sync_requests == 1);
    advance(1000, 1000);
    assert(stats().sync_requests == 2);
    xiaotai_clock_host_answer(SUN_2026_01_04 + 7000, 0);
    advance(1000, 1000);
    assert(seen.syncs == 1 && seen.minute.local.hour == 8);
    advance(10000, 10000);
    assert(stats().sync_requests == 2);
    stop();

    prepare(SUN_2026_01_04);
    const char *const names[] = {"configured.invalid"};
    xiaotai_clock_module_config_t cfg = {.timezone = "UTC0"};
    assert(start(&cfg, "JST-9", false) == XIAOTAI_ERR_INVALID_ARG);
    stop();
    prepare(SUN_2026_01_04);
    cfg = (xiaotai_clock_module_config_t){.time_servers = names, .time_server_count = 1};
    assert(start(&cfg, "JST-9", false) == XIAOTAI_ERR_INVALID_ARG);
    stop();

    prepare(SUN_2026_01_04);
    const xiaotai_clock_config_t pre = {.servers = names, .server_count = 1, .timezone = "JST-9"};
    assert(xiaotai_clock_init(&pre) == XIAOTAI_OK);
    cfg = (xiaotai_clock_module_config_t){.timezone = "UTC0", .timezone_setting = true, .sync_retry_s = 2};
    assert(start(&cfg, NULL, false) == XIAOTAI_OK && clock_init_calls == 1);
    char tz[XIAOTAI_CLOCK_NAME_MAX];
    assert(xiaotai_clock_timezone(tz, sizeof(tz)) == XIAOTAI_OK && !strcmp(tz, "JST-9"));
    assert(xiaotai_clock_set_unix_ms(SUN_2026_01_04) == XIAOTAI_OK);
    advance(1000, 1000);
    assert(seen.syncs == 1 && seen.minute.local.hour == 9);
    set_zone("UTC0");
    assert(seen.minute.local.hour == 0);
    set_zone("");
    assert(seen.minute.local.hour == 9);
    assert(xiaotai_clock_timezone(tz, sizeof(tz)) == XIAOTAI_OK && !strcmp(tz, "JST-9"));
    stop();
}

static char reply_text[4096];

static void line(void *unused, const char *line)
{
    (void)unused;
    assert(strlen(reply_text) + strlen(line) + 2 <= sizeof(reply_text));
    strcat(reply_text, line);
    strcat(reply_text, "\n");
}

static const xiaotai_reply_t reply = {.write = line};
static xiaotai_err_t at(const char *cmd)
{
    reply_text[0] = 0;
    return xiaotai_command_run(cmd, &reply, 2000);
}

static void diagnose(void *unused)
{
    (void)unused;
    xiaotai_diag_report("clock", &reply);
}

static void test_at(void)
{
    prepare(SUN_2026_01_04);
    assert(start(NULL, "CST-8", false) == XIAOTAI_OK);
    assert(at("AT+CLOCK") == XIAOTAI_OK && strstr(reply_text, "synced=0 unix_ms=1767484800000 local=- weekday=- offset_s=-"));
    assert(at("AT+CLOCK ALARM 09:00 127 E697A9") == XIAOTAI_OK);
    assert(at("AT+CLOCK COUNTDOWN 86400") == XIAOTAI_OK);
    assert(at("AT+CLOCK") == XIAOTAI_OK);
    assert(strstr(reply_text, "+CLOCK-ITEM: id=1 kind=alarm enabled=1 time=09:00 weekdays=127 due_in_s=- label_hex=E697A9"));
    assert(strstr(reply_text, "+CLOCK-ITEM: id=2 kind=countdown enabled=1 time=- weekdays=0 due_in_s=86400 label_hex=-"));
    assert(at("AT+CLOCK ENABLE 1 0") == XIAOTAI_OK && !get(1).entry.enabled);
    assert(at("AT+CLOCK ENABLE 2 0") == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(at("AT+CLOCK ENABLE 1 00") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+CLOCK ENABLE 1 1") == XIAOTAI_OK && get(1).entry.enabled);
    assert(at("AT+CLOCK REMOVE 0") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+CLOCK REMOVE 2") == XIAOTAI_OK);
    assert(at("AT+CLOCK REMOVE 222") == XIAOTAI_ERR_NOT_FOUND);
    assert(at("AT+CLOCK ALARM 9:01 0") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+CLOCK ALARM 25:00 0") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+CLOCK ALARM 08:59 128") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+CLOCK ALARM 08:59 0 EDA080") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+CLOCK COUNTDOWN 0") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+CLOCK COUNTDOWN 86401") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+CLOCK COUNTDOWN 5 - extra") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+CLOCK COUNTDOWN 9 61616161616161616161616161616161616161616161E697A9") == XIAOTAI_OK);
    assert(strlen(get(3).entry.label) == 22); /* Do not split the final three-byte code point. */
    for (unsigned i = 2; i < XIAOTAI_SCHEDULE_MAX; ++i) assert(at("AT+CLOCK COUNTDOWN 3600") == XIAOTAI_OK);
    assert(at("AT+CLOCK COUNTDOWN 3600") == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_clock_set_unix_ms(SUN_2026_01_04) == XIAOTAI_OK);
    advance(1000, 0);
    assert(at("AT+CLOCK") == XIAOTAI_OK && strstr(reply_text, "local=2026-01-04T08:00:00 weekday=0 offset_s=28800"));
    reply_text[0] = 0;
    run(diagnose, NULL);
    assert(strstr(reply_text, "store_queued=") && !strstr(reply_text, "E697A9"));
    stop();
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "alarm";
    if (!strcmp(scenario, "countdown")) test_countdown();
    else if (!strcmp(scenario, "step")) test_step();
    else if (!strcmp(scenario, "spring")) test_spring();
    else if (!strcmp(scenario, "gate")) test_gate();
    else if (!strcmp(scenario, "config")) test_config();
    else if (!strcmp(scenario, "at")) test_at();
    else { assert(!strcmp(scenario, "alarm")); test_alarm(); }
    return 0;
}
