// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_settings.c::settings
/* Byte-for-byte compatibility checks through the real typed host KV worker. */
#include "preferences_test.h"
#include <pthread.h>

typedef struct {
    const char *key, *value;
    xiaotai_err_t expected;
} text_change_t;

static void store_text(void *ctx)
{
    const text_change_t *change = ctx;
    CHECK(xiaotai_preferences_store_text(change->key, change->value) == change->expected);
}

static void set_text(const char *key, const char *value, xiaotai_err_t expected)
{
    text_change_t change = {.key = key, .value = value, .expected = expected};
    owner_call(store_text, &change);
}

static void expect_text(const char *key, const char *expected)
{
    char out[XIAOTAI_PREFERENCE_TEXT_MAX];
    CHECK(xiaotai_preferences_load_text(key, out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, expected));
}

static void default_layout(void)
{
    const uint8_t negative[] = {0xFE, 0xFF, 0xFF, 0xFF};
    const uint8_t changed[] = {0x78, 0x56, 0x34, 0x12};
    const char first[] = "fixture", second[] = "changed";
    CHECK(xiaotai_kv_set("xt_settings", "number", negative, sizeof(negative), WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set("xt_settings", "text", first, sizeof(first), WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("xt_settings", "wrong_type", 7, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set("xt_settings", "short_number", negative, 3, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set("xt_settings", "bad_text", first, sizeof(first) - 1, WAIT_MS) == XIAOTAI_OK);
    boot(NULL, 0, 50, NULL);
    expect_pref_int("number", 37, -2);
    expect_text("text", first);
    int32_t result = 37;
    char small[2];
    CHECK(xiaotai_preferences_load_int("absent", 37, &result) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_preferences_load_int("wrong_type", 37, &result) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_preferences_load_int("short_number", 37, &result) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_preferences_load_text("bad_text", small, sizeof(small)) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_preferences_load_text("text", small, sizeof(small)) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(pref_stats().invalid >= 2);
    set_int("number", 0x12345678, XIAOTAI_OK);
    set_text("text", second, XIAOTAI_OK);
    await_idle();
    expect_blob("xt_settings", "number", changed, sizeof(changed));
    expect_blob("xt_settings", "text", second, sizeof(second));
    test_reset();
    boot(NULL, 0, 50, NULL);
    expect_pref_int("number", 37, 0x12345678);
    expect_text("text", second);
}

static void validate_integer_binding(void)
{
    static const int32_t steps[] = {60, 300, 600, 1800, 0};
    static const xiaotai_preference_binding_t invalid[] = {
        {.setting = "a", .space = "old", .key = "a", .raw_min = -1, .raw_max = 4},
        {.setting = "a", .space = "old", .key = "a", .raw_min = 0, .raw_max = 256},
        {.setting = "a", .space = "old", .key = "a", .int_type = XIAOTAI_KV_U32, .raw_max = UINT32_MAX},
        {.setting = "a", .space = "old", .key = "a", .int_type = (xiaotai_kv_int_t)91},
        {.setting = "a", .space = "old", .key = "a", .values = steps},
        {.setting = "a", .space = "old", .key = "a", .values = steps, .value_count = 5, .raw_max = 3},
        {.setting = "a", .space = "1234567890123456", .key = "a"},
        {.setting = "a", .space = "old", .key = ""},
    };
    for (size_t i = 0; i < COUNT(invalid); ++i) {
        const xiaotai_preferences_config_t invalid_config = {.bindings = &invalid[i], .binding_count = 1};
        CHECK(xiaotai_preferences_configure(&invalid_config) == XIAOTAI_ERR_INVALID_ARG);
    }
    const xiaotai_preference_binding_t repeated[] = {
        {.setting = "a", .space = "old", .key = "a"}, {.setting = "a", .space = "old", .key = "b"},
    };
    CHECK(xiaotai_preferences_configure(&(xiaotai_preferences_config_t){
        .bindings = repeated, .binding_count = COUNT(repeated),
    }) == XIAOTAI_ERR_INVALID_ARG);
}

static void validate_snapshot_bindings(void)
{
    xiaotai_preference_binding_t rows[XIAOTAI_KV_OPS_MAX + 1U];
    char keys[COUNT(rows)][XIAOTAI_KV_NAME_MAX + 1U];
    const xiaotai_preferences_config_t config = {.bindings = rows, .binding_count = COUNT(rows)};
    for (size_t i = 0; i < COUNT(rows); ++i) {
        snprintf(keys[i], sizeof(keys[i]), "key%u", (unsigned)i);
        rows[i] = (xiaotai_preference_binding_t){
            .setting = keys[i], .space = "product", .key = keys[i], .snapshot = true,
        };
    }
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    rows[XIAOTAI_KV_OPS_MAX].snapshot = false;
    rows[XIAOTAI_KV_OPS_MAX].space = "unrelated";
    rows[0].storage = XIAOTAI_PREFERENCE_STRING;
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_OK);
    CHECK(xiaotai_preferences_configure(NULL) == XIAOTAI_OK);
    rows[1].space = "another";
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    rows[1].space = "product";
    rows[1].storage = XIAOTAI_PREFERENCE_STRING;
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_ERR_INVALID_ARG);

    const xiaotai_preference_record_t record = {
        .space = "product", .key = "packed", .size = 1, .version_offset = XIAOTAI_PREFERENCE_NO_VERSION,
    };
    rows[1] = (xiaotai_preference_binding_t){
        .setting = keys[1], .storage = XIAOTAI_PREFERENCE_FIELD, .record = &record, .snapshot = true,
    };
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    rows[1].snapshot = false;
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_OK);
    CHECK(xiaotai_preferences_configure(NULL) == XIAOTAI_OK);
}

static void integer_layout(void)
{
    /* The legacy order isn't sorted: both 60 and 0 are equally near 30. */
    static const int32_t sleep[] = {60, 300, 600, 1800, 0};
    static const xiaotai_preference_binding_t rows[] = {
        {.setting = "sleep", .space = "product", .key = "sleep", .raw_max = 4, .values = sleep, .value_count = 5},
        {.setting = "small", .space = "product", .key = "tiny", .raw_max = 3},
        {.setting = "wide", .space = "legacy", .key = "u16", .int_type = XIAOTAI_KV_U16, .raw_max = UINT16_MAX},
        {.setting = "signed", .space = "legacy", .key = "i32", .int_type = XIAOTAI_KV_I32,
         .raw_min = INT32_MIN, .raw_max = INT32_MAX},
        {.setting = "unsigned", .space = "legacy", .key = "u32", .int_type = XIAOTAI_KV_U32, .raw_max = INT32_MAX},
        {.setting = "mismatched", .space = "legacy", .key = "mismatch", .int_type = XIAOTAI_KV_U32, .raw_max = INT32_MAX},
    };
    validate_integer_binding();
    validate_snapshot_bindings();
    CHECK(xiaotai_kv_set_u8("product", "sleep", 4, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("product", "tiny", 4, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_int("legacy", "u16", XIAOTAI_KV_U16, 41111, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_int("legacy", "i32", XIAOTAI_KV_I32, -2147483647, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_int("legacy", "u32", XIAOTAI_KV_U32, 2029111040, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_int("legacy", "mismatch", XIAOTAI_KV_I32, 2029111040, WAIT_MS) == XIAOTAI_OK);
    boot(rows, COUNT(rows), 50, NULL);
    expect_pref_int("sleep", 300, 0);
    expect_pref_int("wide", 1, 41111);
    expect_pref_int("signed", 1, -2147483647);
    expect_pref_int("unsigned", 1, 2029111040);
    int32_t result;
    CHECK(xiaotai_preferences_load_int("small", 1, &result) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(pref_stats().invalid == 1);
    CHECK(xiaotai_preferences_load_int("mismatched", 1, &result) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_preferences_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
    set_int("sleep", 30, XIAOTAI_OK);
    set_int("small", 3, XIAOTAI_OK);
    set_int("wide", 65535, XIAOTAI_OK);
    set_int("signed", INT32_MIN, XIAOTAI_OK);
    set_int("unsigned", INT32_MAX, XIAOTAI_OK);
    await_idle();
    expect_int("product", "sleep", XIAOTAI_KV_U8, 0);
    expect_int("product", "tiny", XIAOTAI_KV_U8, 3);
    expect_int("legacy", "u16", XIAOTAI_KV_U16, 65535);
    expect_int("legacy", "i32", XIAOTAI_KV_I32, INT32_MIN);
    expect_int("legacy", "u32", XIAOTAI_KV_U32, INT32_MAX);
    test_reset();
    boot(rows, COUNT(rows), 50, NULL);
    expect_pref_int("sleep", 300, 60);
    expect_pref_int("small", 1, 3);
    expect_pref_int("wide", 1, 65535);
    expect_pref_int("signed", 1, INT32_MIN);
    expect_pref_int("unsigned", 1, INT32_MAX);
}

static void string_layout(void)
{
    static const xiaotai_preference_binding_t peer[] = {
        {.setting = "peer", .space = "product", .key = "door_peer", .storage = XIAOTAI_PREFERENCE_STRING},
    };
    CHECK(xiaotai_kv_set_str("product", "door_peer", "old-peer", WAIT_MS) == XIAOTAI_OK);
    boot(peer, COUNT(peer), 75, NULL);
    expect_text("peer", "old-peer");
    char copy[XIAOTAI_PREFERENCE_TEXT_MAX + 1], stored[XIAOTAI_PREFERENCE_TEXT_MAX];
    set_text("peer", test_text(copy, sizeof(copy) - 1, 'g'), XIAOTAI_ERR_INVALID_SIZE);
    set_text("peer", test_text(copy, sizeof(copy) - 2, 'p'), XIAOTAI_OK);
    memset(copy, '?', sizeof(copy));
    await_idle();
    CHECK(xiaotai_kv_get_str("product", "door_peer", stored, sizeof(stored), WAIT_MS) == XIAOTAI_OK);
    CHECK(!strcmp(stored, test_text(copy, sizeof(copy) - 2, 'p')));
    size_t size = sizeof(stored);
    CHECK(xiaotai_kv_get_blob_strict("product", "door_peer", stored, &size, WAIT_MS) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_kv_get("xt_settings", "peer", stored, &size, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    test_reset();
    boot(peer, COUNT(peer), 50, NULL);
    expect_text("peer", copy);
    set_text("peer", "", XIAOTAI_OK);
    await_idle();
    CHECK(xiaotai_kv_get_str("product", "door_peer", stored, sizeof(stored), WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_preferences_load_text("peer", stored, sizeof(stored)) == XIAOTAI_ERR_NOT_FOUND);
}

static const xiaotai_preference_record_t audio = {
    .space = "xmini_ui", .key = "audio", .size = 4, .version_offset = 0,
    .version_min = 1, .version_max = 2, .version_write = 2,
};
static const xiaotai_preference_binding_t fields[] = {
    {.setting = "volume", .storage = XIAOTAI_PREFERENCE_FIELD, .record = &audio, .offset = 1, .raw_max = 100},
    {.setting = "gain", .storage = XIAOTAI_PREFERENCE_FIELD, .record = &audio, .offset = 2, .raw_max = 36},
    {.setting = "guide", .storage = XIAOTAI_PREFERENCE_FIELD, .record = &audio, .offset = 3, .raw_max = 1,
     .since_version = 2, .before_raw = 0, .before_value = 1},
};

typedef struct { const char *key; int32_t fallback, value; xiaotai_err_t result; } field_load_t;

static void *load_one_field(void *ctx)
{
    field_load_t *load = ctx;
    load->result = xiaotai_preferences_load_int(load->key, load->fallback, &load->value);
    return NULL;
}

static void record_layout(void)
{
    const uint8_t deployed[] = {1, 67, 13, 0};
    const uint8_t updated[] = {2, 70, 13, 1};
    const uint8_t recovered[] = {2, 72, 17, 1};
    const uint8_t defaults[] = {2, 71, 24, 1};
    CHECK(xiaotai_kv_set("xmini_ui", "audio", deployed, sizeof(deployed), WAIT_MS) == XIAOTAI_OK);
    boot(fields, COUNT(fields), 50, NULL);
    const xiaotai_kv_host_counts_t before = kv_counts();
    field_load_t a = {.key = "volume", .fallback = 40}, b = {.key = "gain", .fallback = 24};
    pthread_t first, second;
    xiaotai_kv_host_gate(false);
    CHECK(pthread_create(&first, NULL, load_one_field, &a) == 0);
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
    CHECK(pthread_create(&second, NULL, load_one_field, &b) == 0);
    xiaotai_delay_ms(20); /* overlap an initial read with another caller */
    xiaotai_kv_host_gate(true);
    CHECK(pthread_join(first, NULL) == 0 && pthread_join(second, NULL) == 0);
    CHECK(a.result == XIAOTAI_OK && a.value == 67 && b.result == XIAOTAI_OK && b.value == 13);
    expect_pref_int("guide", 1, 1);
    CHECK(kv_counts().opens == before.opens + 1 && kv_counts().writes == before.writes);
    set_int("volume", 70, XIAOTAI_OK);
    await_idle();
    expect_blob("xmini_ui", "audio", updated, sizeof(updated));
    xiaotai_kv_host_fail_key("audio", XIAOTAI_FAIL);
    set_int("gain", 17, XIAOTAI_OK);
    await_idle();
    CHECK(pref_stats().write_failures == 1);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    expect_blob("xmini_ui", "audio", updated, sizeof(updated));
    expect_pref_int("gain", 24, 13); /* only durable values are reported by load */
    const xiaotai_kv_host_counts_t failed = kv_counts();
    CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_FAIL);
    CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    CHECK(kv_counts().writes == failed.writes && kv_counts().commits == failed.commits);
    /* A subsequent user change writes the last desired values, like Xmini's
     * original whole-record save. The failed write was never retried alone. */
    set_int("volume", 72, XIAOTAI_OK);
    await_idle();
    expect_blob("xmini_ui", "audio", recovered, sizeof(recovered));
    expect_pref_int("gain", 24, 17);
    test_reset();
    boot(fields, COUNT(fields), 50, NULL);
    expect_pref_int("volume", 40, 72);
    expect_pref_int("gain", 24, 17);
    expect_pref_int("guide", 1, 1);
    test_reset();
    CHECK(xiaotai_kv_erase("xmini_ui", "audio", WAIT_MS) == XIAOTAI_OK);
    boot(fields, COUNT(fields), 50, NULL);
    int32_t value;
    CHECK(xiaotai_preferences_load_int("gain", 24, &value) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_preferences_load_int("guide", 1, &value) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_preferences_load_int("volume", 40, &value) == XIAOTAI_ERR_NOT_FOUND);
    set_int("volume", 71, XIAOTAI_OK);
    CHECK(xiaotai_preferences_load_int("gain", 37, &value) == XIAOTAI_ERR_NOT_FOUND);
    await_idle();
    expect_blob("xmini_ui", "audio", defaults, sizeof(defaults));
}

static void record_malformed(void)
{
    xiaotai_preference_record_t mismatched = audio;
    mismatched.debounce_as_record = true;
    xiaotai_preference_binding_t inconsistent[] = {fields[0], fields[1]};
    inconsistent[1].record = &mismatched;
    CHECK(xiaotai_preferences_configure(&(xiaotai_preferences_config_t){
        .bindings = inconsistent, .binding_count = COUNT(inconsistent)}) == XIAOTAI_ERR_INVALID_ARG);

    static const uint8_t data[][5] = {
        {1, 67, 13}, {2, 67, 13, 1, 0}, {3, 67, 13, 0}, {1, 67, 13, 1}, {2, 67, 37, 0},
    };
    static const size_t sizes[] = {3, 5, 4, 4, 4};
    for (size_t i = 0; i < COUNT(data); ++i) {
        CHECK(xiaotai_kv_set("xmini_ui", "audio", data[i], sizes[i], WAIT_MS) == XIAOTAI_OK);
        boot(fields, COUNT(fields), 50, NULL);
        const xiaotai_kv_host_counts_t before = kv_counts();
        int32_t value;
        CHECK(xiaotai_preferences_load_int("volume", 40, &value) == XIAOTAI_ERR_NOT_FOUND);
        set_int("volume", 71, XIAOTAI_ERR_INVALID_STATE);
        CHECK(!xiaotai_preferences_pending() && pref_stats().invalid == 1 && pref_stats().refused == 1);
        CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
        CHECK(kv_counts().writes == before.writes && kv_counts().commits == before.commits);
        expect_blob("xmini_ui", "audio", data[i], sizes[i]);
        test_reset();
    }
}

static void record_recovery(void)
{
    const uint8_t once[] = {2, 67, 13, 0};
    const uint8_t updated[] = {2, 71, 13, 0};
    CHECK(xiaotai_kv_set("xmini_ui", "audio", once, sizeof(once), WAIT_MS) == XIAOTAI_OK);
    boot(fields, COUNT(fields), 50, NULL);
    xiaotai_kv_host_fail_key("audio", XIAOTAI_ERR_TIMEOUT);
    int32_t value;
    CHECK(xiaotai_preferences_load_int("volume", 40, &value) == XIAOTAI_ERR_TIMEOUT);
    const xiaotai_kv_host_counts_t before = kv_counts();
    set_int("volume", 71, XIAOTAI_ERR_INVALID_STATE);
    CHECK(!xiaotai_preferences_pending() && pref_stats().refused == 1);
    CHECK(kv_counts().writes == before.writes && kv_counts().commits == before.commits);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    expect_blob("xmini_ui", "audio", once, sizeof(once));
    expect_pref_int("gain", 24, 13);
    set_int("volume", 71, XIAOTAI_OK);
    await_idle();
    expect_blob("xmini_ui", "audio", updated, sizeof(updated));
}

typedef struct {
    uint32_t ticket;
    bool retry;
    xiaotai_err_t result;
} maintenance_check_t;

static void owner_maintenance_check(void *ctx)
{
    maintenance_check_t *check = ctx;
    if (!check->ticket && !check->retry)
        CHECK(xiaotai_quiesce_begin("packed-retry-fixture", &check->ticket) == XIAOTAI_OK);
    check->result = xiaotai_preferences_packed_maintenance(check->retry);
}

static void record_maintenance_retry(void)
{
    const uint8_t before[] = {2, 67, 13, 0};
    const uint8_t after[] = {2, 67, 17, 0};
    CHECK(xiaotai_kv_set("xmini_ui", "audio", before, sizeof(before), WAIT_MS) == XIAOTAI_OK);
    boot(fields, COUNT(fields), 1, NULL);
    expect_pref_int("volume", 40, 67);
    expect_pref_int("gain", 24, 13);
    expect_pref_int("guide", 1, 0);
    xiaotai_kv_host_fail_key("audio", XIAOTAI_FAIL);
    set_int("gain", 17, XIAOTAI_OK);
    await_idle();
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_FAIL);
    expect_blob("xmini_ui", "audio", before, sizeof(before));
    CHECK(xiaotai_preferences_packed_maintenance(true) == XIAOTAI_ERR_INVALID_STATE);

    maintenance_check_t state = {.retry = true};
    owner_call(owner_maintenance_check, &state);
    CHECK(state.result == XIAOTAI_ERR_INVALID_STATE);
    state.retry = false;
    owner_call(owner_maintenance_check, &state);
    CHECK(state.result == XIAOTAI_FAIL && state.ticket);
    const xiaotai_kv_host_counts_t counts = kv_counts();
    xiaotai_kv_host_gate(false);
    state.retry = true;
    owner_call(owner_maintenance_check, &state);
    CHECK(state.result == XIAOTAI_OK && xiaotai_kv_host_wait_held(WAIT_MS));
    owner_call(owner_maintenance_check, &state);
    CHECK(state.result == XIAOTAI_ERR_BUSY && kv_counts().commits == counts.commits);
    xiaotai_kv_host_gate(true);
    await_idle();
    state.retry = false;
    owner_call(owner_maintenance_check, &state);
    CHECK(state.result == XIAOTAI_OK);
    expect_blob("xmini_ui", "audio", after, sizeof(after));
    CHECK(kv_counts().writes == counts.writes + 1 && kv_counts().commits == counts.commits + 1);
}

int main(int argc, char **argv)
{
    static const struct {const char *name; void (*run)(void);} cases[] = {
        {"default", default_layout}, {"int", integer_layout}, {"string", string_layout},
        {"record", record_layout}, {"record_malformed", record_malformed}, {"record_recovery", record_recovery},
        {"record_maintenance_retry", record_maintenance_retry},
    };
    const char *name = argc == 2 ? argv[1] : "default";
    for (size_t i = 0; i < COUNT(cases); ++i) {
        if (strcmp(name, cases[i].name)) continue;
        test_clear();
        cases[i].run();
        test_reset();
        return 0;
    }
    CHECK(false);
    return 1;
}
