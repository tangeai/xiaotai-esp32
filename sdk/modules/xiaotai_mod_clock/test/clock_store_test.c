// SPDX-License-Identifier: MIT
#include "clock_test.h"

#define SPACE "xt_clock"
#define KEY "alarms"
#define SETTING_SPACE "xt_settings"
#define TIMEZONE_KEY "clock_tz"

static int read_record(uint8_t *out, size_t size)
{
    assert(xiaotai_kv_get(SPACE, KEY, out, &size, 2000) == XIAOTAI_OK);
    return (int)size;
}

static void set_enable(void *ctx)
{
    const uint8_t id = *(const uint8_t *)ctx;
    assert(xiaotai_schedule_enable(id, false) == XIAOTAI_OK);
    assert(xiaotai_schedule_enable(id, true) == XIAOTAI_OK);
    assert(xiaotai_schedule_enable(id, false) == XIAOTAI_OK);
}

static void persist(void)
{
    prepare(SUN_2026_01_04);
    const xiaotai_clock_module_config_t options = {.timezone_setting = true};
    assert(start(&options, "CST-8", false) == XIAOTAI_OK);
    xiaotai_kv_host_counts_t baseline;
    xiaotai_kv_host_counts(&baseline);
    assert(!baseline.writes); /* register the empty/default rule without writing */
    xiaotai_schedule_t alarm = {.kind = XIAOTAI_SCHEDULE_ALARM, .hour = 18, .minute = 5, .weekdays = 16,
        .enabled = true, .label = "evening"};
    const uint8_t timer = add((xiaotai_schedule_t){.kind = XIAOTAI_SCHEDULE_COUNTDOWN, .seconds = 600});
    uint8_t saved_alarm = add(alarm);
    assert(timer == 1 && saved_alarm == 2);
    set_zone("UTC0");
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);

    xiaotai_kv_host_gate(false);
    assert(xiaotai_kv_set_str_async("fixture", "block", "x") == XIAOTAI_OK);
    assert(xiaotai_kv_host_wait_held(2000));
    xiaotai_kv_stats_t k0, k1;
    xiaotai_kv_stats(&k0);
    run(set_enable, &saved_alarm);
    assert(stats().stored == 4 && !stats().store_failures);
    xiaotai_kv_stats(&k1);
    assert(k1.coalesced == k0.coalesced + 2);
    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    uint8_t raw[482] = {0};
    assert(read_record(raw, sizeof(raw)) == 32);
    const uint8_t expected[] = {1, 1, 2, 0, 18, 5, 16, 0, 'e', 'v', 'e', 'n', 'i', 'n', 'g', 0};
    assert(memcmp(raw, expected, sizeof(expected)) == 0);
    for (unsigned i = sizeof(expected); i < 32; ++i) assert(raw[i] == 0);
    stop();

    prepare(SUN_2026_01_04);
    assert(start(&options, "CST-8", false) == XIAOTAI_OK);
    xiaotai_schedule_info_t info;
    assert(xiaotai_schedule_list(&info, 1) == 1 && info.entry.id == saved_alarm && !info.entry.enabled);
    assert(info.due_in_s == UINT32_MAX && strcmp(info.entry.label, "evening") == 0);
    assert(xiaotai_schedule_get(timer, &info) == XIAOTAI_ERR_NOT_FOUND);
    char stored[XIAOTAI_CLOCK_NAME_MAX];
    size_t bytes = sizeof(stored);
    assert(xiaotai_kv_get("xt_settings", "clock_tz", stored, &bytes, 2000) == XIAOTAI_OK && bytes == sizeof("UTC0"));
    assert(!strcmp(stored, "UTC0"));
    assert(xiaotai_clock_timezone(stored, sizeof(stored)) == XIAOTAI_OK && !strcmp(stored, "UTC0"));
    set_zone("");
    assert(xiaotai_clock_timezone(stored, sizeof(stored)) == XIAOTAI_OK && !strcmp(stored, "CST-8"));
    stop();
}

typedef struct {
    const char *text;
    xiaotai_err_t result;
} zone_write_t;

static void write_zone(void *ctx)
{
    zone_write_t *write = ctx;
    write->result = xiaotai_setting_set_text(XIAOTAI_SETTING_CLOCK_TIMEZONE, write->text);
}

static void make_rule(char *out, size_t capacity)
{
    out[0] = '<';
    memset(out + 1, 'A', capacity - 5U);
    memcpy(out + capacity - 4U, ">-9", 4U);
}

static void expect_zone(const char *configured, const char *effective, const char *persisted)
{
    char text[XIAOTAI_SETTING_TEXT_MAX];
    assert(xiaotai_setting_get_text(XIAOTAI_SETTING_CLOCK_TIMEZONE, text, sizeof(text)) == XIAOTAI_OK);
    assert(!strcmp(text, configured));
    assert(xiaotai_clock_timezone(text, sizeof(text)) == XIAOTAI_OK && !strcmp(text, effective));
    size_t bytes = sizeof(text);
    assert(xiaotai_kv_get(SETTING_SPACE, TIMEZONE_KEY, text, &bytes, 2000) == XIAOTAI_OK);
    assert(bytes == strlen(persisted) + 1U && !memcmp(text, persisted, bytes));
}

static void timezone_bound(void)
{
    char fits[XIAOTAI_CLOCK_NAME_MAX], exceeds[XIAOTAI_CLOCK_NAME_MAX + 1U];
    make_rule(fits, sizeof(fits));
    make_rule(exceeds, sizeof(exceeds));
    assert(strlen(fits) == 63U && strlen(exceeds) == 64U);
    const xiaotai_clock_module_config_t options = {.timezone_setting = true};
    prepare(SUN_2026_01_04);
    assert(start(&options, "CST-8", true) == XIAOTAI_OK);
    set_zone(fits);
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    expect_zone(fits, fits, fits);

    const unsigned minutes = seen.minutes;
    xiaotai_kv_stats_t before, after;
    xiaotai_kv_stats(&before);
    zone_write_t write = {.text = exceeds, .result = XIAOTAI_FAIL};
    run(write_zone, &write);
    assert(write.result == XIAOTAI_ERR_INVALID_SIZE);
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    xiaotai_kv_stats(&after);
    assert(after.submitted == before.submitted && seen.minutes == minutes);
    expect_zone(fits, fits, fits);
    stop();

    prepare(SUN_2026_01_04);
    assert(start(&options, "CST-8", true) == XIAOTAI_OK);
    expect_zone(fits, fits, fits);
    stop();

    /* A value accepted by the old writer must not be applied or erased at boot. */
    prepare(SUN_2026_01_04);
    assert(xiaotai_kv_set(SETTING_SPACE, TIMEZONE_KEY, exceeds, sizeof(exceeds), 2000) == XIAOTAI_OK);
    xiaotai_kv_host_counts_t old, loaded;
    xiaotai_kv_host_counts(&old);
    assert(start(&options, "CST-8", true) == XIAOTAI_OK);
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    xiaotai_kv_host_counts(&loaded);
    assert(loaded.writes == old.writes && loaded.commits == old.commits);
    expect_zone("", "CST-8", exceeds);
    set_zone(fits);
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    expect_zone(fits, fits, fits);
    set_zone("");
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    expect_zone("", "CST-8", "");
    stop();
    xiaotai_kv_host_clear();
}

static void malformed(void)
{
    for (unsigned which = 0; which < 12; ++which) {
        prepare(SUN_2026_01_04);
        uint8_t raw[483] = {1, 1, 8, 1, 9, 30, 127, 0, 'w', 0};
        size_t size = 32;
        switch (which) {
            case 0: raw[0] = 2; break;
            case 1: raw[1] = 17; break;
            case 2: raw[2] = 0; break;
            case 3: raw[3] = 2; break;
            case 4: raw[4] = 24; break;
            case 5: raw[5] = 60; break;
            case 6: raw[6] = 128; break;
            case 7: raw[7] = 1; break;
            case 8: memset(raw + 8, 'x', 24); break;
            case 9: raw[1] = 2; memcpy(raw + 32, raw + 2, 30); size = 62; break;
            case 10: size = 483; break;
            case 11: size = 31; break;
            default: assert(0);
        }
        assert(xiaotai_kv_set(SPACE, KEY, raw, size, 2000) == XIAOTAI_OK);
        assert(start(NULL, "UTC0", false) == XIAOTAI_OK);
        xiaotai_schedule_info_t info;
        assert(xiaotai_schedule_list(&info, 1) == 0 && stats().load_invalid == 1 && !stats().stored);
        uint8_t from_disk[483];
        assert(read_record(from_disk, sizeof(from_disk)) == (int)size && !memcmp(raw, from_disk, size));
        stop();
    }
    prepare(SUN_2026_01_04);
    assert(xiaotai_kv_set_str(SPACE, KEY, "not-a-blob", 2000) == XIAOTAI_OK);
    assert(start(NULL, "UTC0", false) == XIAOTAI_OK && stats().load_invalid == 1);
    char retained[16];
    assert(xiaotai_kv_get_str(SPACE, KEY, retained, sizeof(retained), 2000) == XIAOTAI_OK);
    assert(!strcmp(retained, "not-a-blob"));
    stop();
}

static void exhausted(void *ctx)
{
    const uint8_t id = *(const uint8_t *)ctx;
    assert(xiaotai_schedule_remove(id) == XIAOTAI_OK);
    uint8_t assigned = 7;
    const xiaotai_schedule_t t = {.kind = XIAOTAI_SCHEDULE_COUNTDOWN, .seconds = 300};
    assert(xiaotai_schedule_add(&t, &assigned) == XIAOTAI_ERR_NO_MEM && !assigned);
}

static void durable_failure_and_limit(void)
{
    prepare(SUN_2026_01_04);
    uint8_t seed[32] = {1, 1, 254, 1, 9, 30, 127, 0, 'w', 0};
    assert(xiaotai_kv_set(SPACE, KEY, seed, sizeof(seed), 2000) == XIAOTAI_OK);
    assert(start(NULL, "UTC0", false) == XIAOTAI_OK && !stats().load_invalid);
    assert(get(254).entry.id == 254);
    xiaotai_kv_host_fail_key(KEY, XIAOTAI_FAIL);
    xiaotai_kv_stats_t before, after;
    xiaotai_kv_stats(&before);
    uint8_t id = add((xiaotai_schedule_t){.kind = XIAOTAI_SCHEDULE_ALARM, .hour = 15, .minute = 25, .enabled = true});
    assert(id == 255 && stats().stored == 1 && !stats().store_failures);
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    xiaotai_kv_stats(&after);
    assert(after.failures == before.failures + 1);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    uint8_t raw[482];
    assert(read_record(raw, sizeof(raw)) == sizeof(seed) && !memcmp(seed, raw, sizeof(seed)));
    run(exhausted, &id);
    assert(stats().refused == 1);
    stop();
}

int main(void)
{
    timezone_bound();
    persist();
    malformed();
    durable_failure_and_limit();
    return 0;
}
