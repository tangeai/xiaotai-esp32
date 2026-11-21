// SPDX-License-Identifier: MIT
#include "clock_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/at_command.h"

static void wall_line(const xiaotai_reply_t *reply, const char *prefix)
{
    xiaotai_wallclock_t wall;
    if (!xiaotai_wallclock_now(&wall)) {
        xiaotai_at_printf(reply, "%ssynced=0 unix_ms=%" PRId64 " local=- weekday=- offset_s=-", prefix, wall.unix_ms);
        return;
    }
    const xiaotai_clock_fields_t *f = &wall.local;
    xiaotai_at_printf(reply, "%ssynced=1 unix_ms=%" PRId64 " local=%04d-%02u-%02uT%02u:%02u:%02u weekday=%u offset_s=%" PRId32,
                      prefix, wall.unix_ms, (int)f->year, (unsigned)f->month, (unsigned)f->day, (unsigned)f->hour,
                      (unsigned)f->minute, (unsigned)f->second, (unsigned)f->weekday, f->utc_offset_s);
}

static xiaotai_err_t show(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    wall_line(reply, "+CLOCK: ");
    xiaotai_schedule_info_t list[XIAOTAI_SCHEDULE_MAX];
    const size_t count = xiaotai_schedule_list(list, XIAOTAI_SCHEDULE_MAX);
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_schedule_t *e = &list[i].entry;
        const bool alarm = e->kind == XIAOTAI_SCHEDULE_ALARM;
        char hhmm[8] = "-", due[16] = "-", label[2U * XIAOTAI_SCHEDULE_LABEL_MAX + 1U];
        if (alarm) (void)snprintf(hhmm, sizeof(hhmm), "%02u:%02u", (unsigned)e->hour, (unsigned)e->minute);
        if (list[i].due_in_s != UINT32_MAX) (void)snprintf(due, sizeof(due), "%" PRIu32, list[i].due_in_s);
        (void)xiaotai_at_text_encode(e->label, label, sizeof(label));
        xiaotai_at_printf(reply, "+CLOCK-ITEM: id=%u kind=%s enabled=%u time=%s weekdays=%u due_in_s=%s label_hex=%s",
                          (unsigned)e->id, alarm ? "alarm" : "countdown", (unsigned)e->enabled, hhmm,
                          (unsigned)e->weekdays, due, label);
    }
    return XIAOTAI_OK;
}

static bool parse_hhmm(const char *text, uint8_t *hour, uint8_t *minute)
{
    if (strlen(text) != 5U || text[2] != ':') return false;
    char a[] = {text[0], text[1], 0}, b[] = {text[3], text[4], 0};
    uint32_t h, m;
    if (!xiaotai_at_parse_u32(a, 0U, 23U, &h) || !xiaotai_at_parse_u32(b, 0U, 59U, &m)) return false;
    *hour = (uint8_t)h;
    *minute = (uint8_t)m;
    return true;
}

static xiaotai_err_t label(const char *hex, xiaotai_schedule_t *entry, const xiaotai_reply_t *reply)
{
    char decoded[XIAOTAI_ARG_TEXT_MAX];
    xiaotai_err_t err = xiaotai_at_text_decode(hex, decoded, sizeof(decoded));
    if (err == XIAOTAI_OK && !clock_label_copy(entry->label, decoded)) err = XIAOTAI_ERR_INVALID_ARG;
    return err == XIAOTAI_OK ? err : xiaotai_at_fail(reply, err, "LABEL");
}

static xiaotai_err_t add_alarm(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    xiaotai_schedule_t entry = {.kind = XIAOTAI_SCHEDULE_ALARM, .enabled = true};
    uint32_t days;
    if (!parse_hhmm(argv[0], &entry.hour, &entry.minute)) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "TIME");
    if (!xiaotai_at_parse_u32(argv[1], 0U, XIAOTAI_SCHEDULE_EVERY_DAY, &days)) {
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "WEEKDAYS");
    }
    entry.weekdays = (uint8_t)days;
    xiaotai_err_t err = argc == 3 ? label(argv[2], &entry, reply) : XIAOTAI_OK;
    return err == XIAOTAI_OK ? xiaotai_schedule_add(&entry, NULL) : err;
}

static xiaotai_err_t add_countdown(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    xiaotai_schedule_t entry = {.kind = XIAOTAI_SCHEDULE_COUNTDOWN};
    if (!xiaotai_at_parse_u32(argv[0], 1U, XIAOTAI_SCHEDULE_COUNTDOWN_MAX_S, &entry.seconds)) {
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "SECONDS");
    }
    xiaotai_err_t err = argc == 2 ? label(argv[1], &entry, reply) : XIAOTAI_OK;
    return err == XIAOTAI_OK ? xiaotai_schedule_add(&entry, NULL) : err;
}

static xiaotai_err_t remove_alarm(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    uint32_t id;
    if (!xiaotai_at_parse_u32(argv[0], 1U, UINT8_MAX, &id)) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "ID");
    return xiaotai_schedule_remove((uint8_t)id);
}

static xiaotai_err_t enable_alarm(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    uint32_t id;
    bool enabled;
    if (!xiaotai_at_parse_u32(argv[0], 1U, UINT8_MAX, &id)) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "ID");
    if (!xiaotai_at_parse_flag(argv[1], &enabled)) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "ENABLE");
    return xiaotai_schedule_enable((uint8_t)id, enabled);
}

static void diagnose(const xiaotai_reply_t *reply)
{
    wall_line(reply, "");
    xiaotai_clock_module_stats_t s;
    xiaotai_clock_module_stats(&s);
    xiaotai_at_printf(reply, "sync_requests=%" PRIu32 " minutes=%" PRIu32 " steps=%" PRIu32 " fired_alarms=%" PRIu32
                      " fired_countdowns=%" PRIu32 " missed_alarms=%" PRIu32 " added=%" PRIu32 " removed=%" PRIu32
                      " refused=%" PRIu32 " store_queued=%" PRIu32 " store_queue_failures=%" PRIu32 " load_invalid=%" PRIu32,
                      s.sync_requests, s.minutes, s.steps, s.fired_alarms, s.fired_countdowns, s.missed_alarms,
                      s.added, s.removed, s.refused, s.stored, s.store_failures, s.load_invalid);
}

xiaotai_err_t clock_at_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = show},
        {.word = "ALARM", .min_args = 2, .max_args = 3, .usage = "<hh:mm> <0..127> [<label-hex>]", .run = add_alarm},
        {.word = "COUNTDOWN", .min_args = 1, .max_args = 2, .usage = "<seconds> [<label-hex>]", .run = add_countdown},
        {.word = "REMOVE", .min_args = 1, .max_args = 1, .usage = "<id>", .run = remove_alarm},
        {.word = "ENABLE", .min_args = 2, .max_args = 2, .usage = "<id> <0|1>", .run = enable_alarm},
    };
    const xiaotai_err_t err = xiaotai_at_verbs_register("AT+CLOCK", verbs, sizeof(verbs) / sizeof(verbs[0]));
    return err == XIAOTAI_OK ? xiaotai_diag_register("clock", diagnose) : err;
}
