// SPDX-License-Identifier: MIT
#include "clock_internal.h"

#include <limits.h>
#include <string.h>

#include "xiaotai/eventlog.h"

#define MINUTES_IN_DAY 1440
#define UTC_OFFSET_SAMPLE_MS INT64_C(129600000)

bool clock_label_copy(char out[XIAOTAI_SCHEDULE_LABEL_MAX + 1U], const char *text)
{
    const uint8_t *p = (const uint8_t *)text;
    size_t used = 0;
    for (size_t i = 0, length = strlen(text); i < length;) {
        const uint8_t c = p[i];
        unsigned bytes = c < 0x80 ? 1U : c >= 0xc2 && c <= 0xdf ? 2U :
                         c >= 0xe0 && c <= 0xef ? 3U : c >= 0xf0 && c <= 0xf4 ? 4U : 0U;
        if (bytes == 0U || bytes > length - i) return false;
        for (unsigned j = 1; j < bytes; ++j) if ((p[i + j] & 0xc0U) != 0x80U) return false;
        if ((c == 0xe0 && p[i + 1] < 0xa0U) || (c == 0xed && p[i + 1] >= 0xa0U) ||
            (c == 0xf0 && p[i + 1] < 0x90U) || (c == 0xf4 && p[i + 1] >= 0x90U)) return false;
        if (i + bytes <= XIAOTAI_SCHEDULE_LABEL_MAX) used = i + bytes;
        i += bytes;
    }
    memmove(out, text, used);
    out[used] = 0;
    return true;
}

static size_t index_of(uint8_t id)
{
    for (size_t i = 0; i < clock_state.count; ++i) if (clock_state.rows[i].entry.id == id) return i;
    return SIZE_MAX;
}

static bool normalize(const xiaotai_schedule_t *given, xiaotai_schedule_t *out)
{
    if (!given || !memchr(given->label, 0, sizeof(given->label))) return false;
    *out = (xiaotai_schedule_t){.id = given->id, .kind = given->kind, .enabled = given->enabled};
    if (!clock_label_copy(out->label, given->label)) return false;
    if (given->kind == XIAOTAI_SCHEDULE_ALARM) {
        if (given->hour > 23U || given->minute > 59U || given->weekdays > XIAOTAI_SCHEDULE_EVERY_DAY) return false;
        out->hour = given->hour;
        out->minute = given->minute;
        out->weekdays = given->weekdays;
        return true;
    }
    if (given->kind != XIAOTAI_SCHEDULE_COUNTDOWN || !given->seconds || given->seconds > XIAOTAI_SCHEDULE_COUNTDOWN_MAX_S) {
        return false;
    }
    out->enabled = true;
    out->seconds = given->seconds;
    return true;
}

static void event(const char *name, const xiaotai_args_t *args, const void *data, size_t size)
{
    const xiaotai_err_t err = xiaotai_event_emit(name, args, data, size);
    if (err != XIAOTAI_OK) xiaotai_eventlog_error("clock", "event", 0, err);
}

void clock_schedules_changed(bool persist)
{
    if (persist) clock_store_save();
    const xiaotai_args_t args = {.value = (int32_t)clock_state.count};
    event(XIAOTAI_EVENT_CLOCK_SCHEDULES, &args, NULL, 0);
}

static xiaotai_err_t refused(xiaotai_err_t err)
{
    clock_refused();
    return err;
}

static bool on_owner(void) { return clock_initialized() && xiaotai_on_owner(); }

static int64_t deadline(int64_t mono_ms, uint32_t seconds)
{
    const int64_t delay = (int64_t)seconds * 1000;
    return mono_ms > INT64_MAX - delay ? INT64_MAX : mono_ms + delay;
}

xiaotai_err_t xiaotai_schedule_add(const xiaotai_schedule_t *given, uint8_t *id)
{
    if (id) *id = 0;
    if (!on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_schedule_t entry;
    if (!normalize(given, &entry)) return refused(XIAOTAI_ERR_INVALID_ARG);
    if (clock_state.count == XIAOTAI_SCHEDULE_MAX || clock_state.next_id > UINT8_MAX) return refused(XIAOTAI_ERR_NO_MEM);
    const int64_t mono_ms = xiaotai_time_us() / 1000;
    const bool alarm = entry.kind == XIAOTAI_SCHEDULE_ALARM;
    clock_lock();
    entry.id = (uint8_t)clock_state.next_id++;
    clock_schedule_row_t *row = &clock_state.rows[clock_state.count++];
    row->entry = entry;
    row->armed_ms = mono_ms;
    if (alarm) row->fired_minute = INT64_MIN;
    else row->deadline_ms = deadline(mono_ms, entry.seconds);
    ++clock_state.stats.added;
    clock_unlock();
    if (id) *id = entry.id;
    clock_schedules_changed(alarm);
    clock_reschedule();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_schedule_update(const xiaotai_schedule_t *given)
{
    if (!on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_schedule_t entry;
    if (!normalize(given, &entry)) return refused(XIAOTAI_ERR_INVALID_ARG);
    const size_t i = index_of(entry.id);
    if (i == SIZE_MAX) return refused(XIAOTAI_ERR_NOT_FOUND);
    clock_schedule_row_t *row = &clock_state.rows[i];
    if (row->entry.kind != entry.kind) return refused(XIAOTAI_ERR_INVALID_ARG);
    const int64_t mono_ms = xiaotai_time_us() / 1000;
    const bool alarm = entry.kind == XIAOTAI_SCHEDULE_ALARM;
    clock_lock();
    row->entry = entry;
    row->armed_ms = mono_ms;
    if (!alarm) row->deadline_ms = deadline(mono_ms, entry.seconds);
    clock_unlock();
    clock_schedules_changed(alarm);
    clock_reschedule();
    return XIAOTAI_OK;
}

static void remove_row(size_t index)
{
    --clock_state.count;
    memmove(&clock_state.rows[index], &clock_state.rows[index + 1U],
            (clock_state.count - index) * sizeof(clock_state.rows[0]));
    memset(&clock_state.rows[clock_state.count], 0, sizeof(clock_state.rows[0]));
}

xiaotai_err_t xiaotai_schedule_remove(uint8_t id)
{
    if (!on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    const size_t i = index_of(id);
    if (i == SIZE_MAX) return refused(XIAOTAI_ERR_NOT_FOUND);
    const bool alarm = clock_state.rows[i].entry.kind == XIAOTAI_SCHEDULE_ALARM;
    clock_lock();
    remove_row(i);
    ++clock_state.stats.removed;
    clock_unlock();
    clock_schedules_changed(alarm);
    clock_reschedule();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_schedule_enable(uint8_t id, bool enabled)
{
    if (!on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    const size_t i = index_of(id);
    if (i == SIZE_MAX) return refused(XIAOTAI_ERR_NOT_FOUND);
    clock_schedule_row_t *row = &clock_state.rows[i];
    if (row->entry.kind != XIAOTAI_SCHEDULE_ALARM) return refused(XIAOTAI_ERR_NOT_SUPPORTED);
    if (row->entry.enabled == enabled) return XIAOTAI_OK;
    clock_lock();
    row->entry.enabled = enabled;
    row->armed_ms = xiaotai_time_us() / 1000;
    clock_unlock();
    clock_schedules_changed(true);
    clock_reschedule();
    return XIAOTAI_OK;
}

static int64_t local_day(int64_t minute)
{
    return minute / MINUTES_IN_DAY - (minute % MINUTES_IN_DAY < 0);
}

static unsigned weekday(int64_t day)
{
    const int64_t offset = (day + 4) % 7; /* 1970-01-01 was Thursday. */
    return (unsigned)(offset < 0 ? offset + 7 : offset);
}

static bool weekday_matches(const xiaotai_schedule_t *entry, unsigned day)
{
    return !entry->weekdays || (entry->weekdays & (1U << day));
}

/* There is no local-to-UTC conversion in the clock port. Round-trip the
 * offsets on both sides of a civil minute: a DST gap has no match and a
 * fold can have two. The second is only eligible if the first did not fire. */
static int64_t occurrence(int64_t minute, const xiaotai_wallclock_t *from)
{
    if (minute > INT64_MAX / CLOCK_MINUTE_MS - 3000 || minute < INT64_MIN / CLOCK_MINUTE_MS + 3000) return INT64_MAX;
    const int64_t local_ms = minute * CLOCK_MINUTE_MS;
    const int64_t estimate = local_ms - (int64_t)from->local.utc_offset_s * 1000;
    int32_t offsets[3] = {from->local.utc_offset_s, from->local.utc_offset_s, from->local.utc_offset_s};
    xiaotai_clock_fields_t fields;
    if (xiaotai_clock_local(estimate - UTC_OFFSET_SAMPLE_MS, &fields) == XIAOTAI_OK) offsets[1] = fields.utc_offset_s;
    if (xiaotai_clock_local(estimate + UTC_OFFSET_SAMPLE_MS, &fields) == XIAOTAI_OK) offsets[2] = fields.utc_offset_s;
    int64_t best = INT64_MAX;
    for (unsigned i = 0; i < 3; ++i) {
        if (i && (offsets[i] == offsets[0] || (i == 2 && offsets[i] == offsets[1]))) continue;
        xiaotai_wallclock_t candidate = {.unix_ms = local_ms - (int64_t)offsets[i] * 1000};
        if (candidate.unix_ms < best && candidate.unix_ms + CLOCK_MINUTE_MS > from->unix_ms &&
            xiaotai_clock_local(candidate.unix_ms, &candidate.local) == XIAOTAI_OK && clock_local_ms(&candidate) == local_ms) {
            best = candidate.unix_ms;
        }
    }
    return best;
}

static int64_t next_alarm(const clock_schedule_row_t *row, const xiaotai_wallclock_t *from)
{
    int64_t day = local_day(clock_minute_at(from));
    if (row->fired_minute != INT64_MIN) {
        const int64_t fired_day = local_day(row->fired_minute);
        if (fired_day > day) day = fired_day;
    }
    const xiaotai_schedule_t *entry = &row->entry;
    /* A weekly alarm can skip one spring-forward Sunday. */
    for (unsigned d = 0; d < 15U; ++d, ++day) {
        if (!weekday_matches(entry, weekday(day))) continue;
        const int64_t minute = day * MINUTES_IN_DAY + entry->hour * 60 + entry->minute;
        if (minute <= row->fired_minute) continue;
        int64_t result = occurrence(minute, from);
        if (result != INT64_MAX) return result;
    }
    return INT64_MAX;
}

static uint32_t due_seconds(int64_t deadline_ms, int64_t now_ms)
{
    if (deadline_ms == INT64_MAX) return UINT32_MAX;
    if (deadline_ms <= now_ms) return 0U;
    const uint64_t duration = (uint64_t)deadline_ms - (uint64_t)now_ms;
    const uint64_t seconds = duration / 1000U + (duration % 1000U != 0U);
    return seconds >= UINT32_MAX ? UINT32_MAX - 1U : (uint32_t)seconds;
}

static xiaotai_schedule_info_t info(const clock_schedule_row_t *row, int64_t mono_ms, const xiaotai_wallclock_t *wall)
{
    const xiaotai_schedule_t *entry = &row->entry;
    uint32_t due = UINT32_MAX;
    if (entry->kind == XIAOTAI_SCHEDULE_COUNTDOWN) due = due_seconds(row->deadline_ms, mono_ms);
    else if (entry->enabled && wall->synced) due = due_seconds(next_alarm(row, wall), wall->unix_ms);
    return (xiaotai_schedule_info_t){.entry = *entry, .due_in_s = due};
}

static size_t snapshot(clock_schedule_row_t rows[XIAOTAI_SCHEDULE_MAX])
{
    if (!clock_initialized()) return 0;
    clock_lock();
    const size_t n = clock_state.count;
    memcpy(rows, clock_state.rows, n * sizeof(*rows));
    clock_unlock();
    return n;
}

size_t xiaotai_schedule_list(xiaotai_schedule_info_t *entries, size_t capacity)
{
    if (!entries || !capacity) return 0;
    clock_schedule_row_t rows[XIAOTAI_SCHEDULE_MAX];
    const size_t count = snapshot(rows);
    const int64_t mono_ms = xiaotai_time_us() / 1000;
    xiaotai_wallclock_t wall;
    xiaotai_wallclock_now(&wall);
    size_t n = 0;
    for (unsigned kind = XIAOTAI_SCHEDULE_ALARM; kind <= XIAOTAI_SCHEDULE_COUNTDOWN; ++kind) {
        for (size_t i = 0; i < count && n < capacity; ++i) {
            if (rows[i].entry.kind == (xiaotai_schedule_kind_t)kind) entries[n++] = info(&rows[i], mono_ms, &wall);
        }
    }
    return n;
}

xiaotai_err_t xiaotai_schedule_get(uint8_t id, xiaotai_schedule_info_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    if (!clock_initialized()) return XIAOTAI_ERR_NOT_FOUND;
    clock_lock();
    const size_t i = index_of(id);
    clock_schedule_row_t row = {0};
    if (i != SIZE_MAX) row = clock_state.rows[i];
    clock_unlock();
    if (i == SIZE_MAX) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_wallclock_t wall;
    xiaotai_wallclock_now(&wall);
    *out = info(&row, xiaotai_time_us() / 1000, &wall);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_schedule_next(xiaotai_schedule_info_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    clock_schedule_row_t rows[XIAOTAI_SCHEDULE_MAX];
    const size_t count = snapshot(rows);
    xiaotai_wallclock_t wall;
    xiaotai_wallclock_now(&wall);
    const int64_t mono_ms = xiaotai_time_us() / 1000;
    uint32_t best = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_schedule_info_t candidate = info(&rows[i], mono_ms, &wall);
        if (candidate.due_in_s < best) {
            best = candidate.due_in_s;
            *out = candidate;
        }
    }
    return best < UINT32_MAX ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}

static uint32_t missed(clock_schedule_row_t row, const xiaotai_wallclock_t *now, int64_t from_ms)
{
    xiaotai_wallclock_t from = {.unix_ms = from_ms, .synced = true};
    uint32_t count = 0;
    while (from.unix_ms < now->unix_ms &&
           xiaotai_clock_local(from.unix_ms, &from.local) == XIAOTAI_OK) {
        int64_t due = next_alarm(&row, &from);
        if (due == INT64_MAX || due > now->unix_ms - CLOCK_MINUTE_MS) break;
        ++count;
        if (!row.entry.weekdays) break;
        xiaotai_wallclock_t expired = {.unix_ms = due};
        if (xiaotai_clock_local(due, &expired.local) != XIAOTAI_OK) break;
        row.fired_minute = clock_minute_at(&expired);
        from.unix_ms = due + CLOCK_MINUTE_MS;
    }
    return count;
}

static xiaotai_args_t due_args(const xiaotai_schedule_t *entry)
{
    xiaotai_args_t args = {.value = entry->id};
    memcpy(args.text, entry->label, sizeof(entry->label));
    return args;
}

static void due_event(const xiaotai_schedule_t *entry)
{
    const xiaotai_args_t args = due_args(entry);
    event(XIAOTAI_EVENT_CLOCK_FIRED, &args, entry, sizeof(*entry));
    event(XIAOTAI_EVENT_CLOCK_DUE, &args, entry, sizeof(*entry));
}

xiaotai_err_t xiaotai_schedule_due_again(const xiaotai_schedule_t *given)
{
    if (!on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_schedule_t entry;
    if (!normalize(given, &entry) || !entry.id) return refused(XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_args_t args = due_args(&entry);
    return xiaotai_event_emit(XIAOTAI_EVENT_CLOCK_DUE, &args, &entry, sizeof(entry));
}

void clock_schedule_tick(int64_t mono_ms, const xiaotai_wallclock_t *wall, int64_t missed_from_ms)
{
    bool changed = false, persist = false;
    for (size_t i = 0; i < clock_state.count;) {
        const clock_schedule_row_t row = clock_state.rows[i];
        const xiaotai_schedule_t *entry = &row.entry;
        if (entry->kind == XIAOTAI_SCHEDULE_COUNTDOWN) {
            if (mono_ms < row.deadline_ms) {
                ++i;
                continue;
            }
            clock_lock();
            remove_row(i);
            ++clock_state.stats.fired_countdowns;
            clock_unlock();
            changed = true;
            due_event(entry);
            continue;
        }
        if (entry->enabled && wall->synced) {
            const int64_t armed_from_ms = wall->unix_ms - (mono_ms - row.armed_ms);
            const int64_t from = armed_from_ms > missed_from_ms ? armed_from_ms : missed_from_ms;
            const uint32_t expired = missed(row, wall, from);
            const int64_t minute = clock_minute_at(wall);
            const bool fires = minute > row.fired_minute && entry->hour == wall->local.hour &&
                               entry->minute == wall->local.minute && weekday_matches(entry, wall->local.weekday);
            if (expired || fires) {
                clock_lock();
                clock_state.stats.missed_alarms += expired;
                if (fires) {
                    ++clock_state.stats.fired_alarms;
                    clock_state.rows[i].fired_minute = minute;
                    if (!entry->weekdays) {
                        clock_state.rows[i].entry.enabled = false;
                        changed = persist = true;
                    }
                }
                clock_unlock();
            }
            if (fires) due_event(entry);
        }
        ++i;
    }
    if (changed) clock_schedules_changed(persist);
}

uint32_t clock_countdown_wait_ms(int64_t mono_ms, uint32_t wait)
{
    for (size_t i = 0; i < clock_state.count; ++i) {
        const clock_schedule_row_t *row = &clock_state.rows[i];
        if (row->entry.kind != XIAOTAI_SCHEDULE_COUNTDOWN) continue;
        if (row->deadline_ms <= mono_ms) return 0U;
        uint64_t left = (uint64_t)row->deadline_ms - (uint64_t)mono_ms;
        if (left < wait) wait = (uint32_t)left;
    }
    return wait;
}

static void on_countdown(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!args || args->value <= 0 || (uint32_t)args->value > XIAOTAI_SCHEDULE_COUNTDOWN_MAX_S) {
        clock_refused();
        return;
    }
    xiaotai_schedule_t entry = {.kind = XIAOTAI_SCHEDULE_COUNTDOWN, .seconds = (uint32_t)args->value};
    if (!clock_label_copy(entry.label, args->text)) clock_refused();
    else (void)xiaotai_schedule_add(&entry, NULL);
}

static void on_remove(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!args || args->value < 0 || args->value > UINT8_MAX) {
        clock_refused();
        return;
    }
    if (args->value) {
        (void)xiaotai_schedule_remove((uint8_t)args->value);
        return;
    }
    bool changed = false;
    clock_lock();
    for (size_t i = clock_state.count; i > 0; --i) {
        if (clock_state.rows[i - 1U].entry.kind == XIAOTAI_SCHEDULE_COUNTDOWN) {
            remove_row(i - 1U);
            ++clock_state.stats.removed;
            changed = true;
        }
    }
    clock_unlock();
    if (changed) {
        clock_schedules_changed(false);
        clock_reschedule();
    }
}

static void on_enable(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!args || args->value <= 0 || args->value > UINT8_MAX) clock_refused();
    else (void)xiaotai_schedule_enable((uint8_t)args->value, strcmp(args->text, "0") != 0);
}

xiaotai_err_t clock_schedule_intents(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_CLOCK_COUNTDOWN, on_countdown, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_CLOCK_REMOVE, on_remove, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_CLOCK_ENABLE, on_enable, NULL);
    return err;
}
