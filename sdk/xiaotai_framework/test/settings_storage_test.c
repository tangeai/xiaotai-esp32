// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/settings_confirmation.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

typedef struct { size_t count, bytes; } usage_t;

static xiaotai_mem_host_ledger_entry_t s_ledger[XIAOTAI_MEM_HOST_LEDGER_CAPACITY];
static xiaotai_signal_t s_done, s_failure_done;
static unsigned s_text_loads, s_text_stores, s_changes, s_int_stores, s_int_changes;
static char s_store_cookie;
static char s_stored[XIAOTAI_SETTING_TEXT_MAX];
static char s_confirm_input[] = "one";
static bool s_reenter_store;
static uint32_t s_failed_revision;
static unsigned s_failed_events;

static usage_t usage(void)
{
    size_t count = 0;
    CHECK(xiaotai_mem_host_ledger_snapshot(s_ledger, XIAOTAI_MEM_HOST_LEDGER_CAPACITY, &count));
    usage_t total = {0};
    for (size_t i = 0; i < count; ++i) {
        if (!s_ledger[i].task && !s_ledger[i].fixture) {
            ++total.count;
            total.bytes += s_ledger[i].bytes;
        }
    }
    return total;
}

static xiaotai_err_t load_int(void *self, const char *key, int32_t default_value, int32_t *value)
{
    CHECK(self == &s_store_cookie);
    if (strcmp(key, "level") == 0) {
        CHECK(default_value == 2);
        *value = 5;
    } else {
        CHECK(strcmp(key, "enabled") == 0 && default_value == 1);
        *value = 2; /* An invalid stored boolean keeps its default. */
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t load_text(void *self, const char *key, char *out, size_t capacity)
{
    CHECK(self == &s_store_cookie);
    CHECK(capacity == XIAOTAI_SETTING_TEXT_MAX);
    ++s_text_loads;
    if (strcmp(key, "short") == 0) memcpy(out, "old", 4);
    else if (strcmp(key, "invalid") == 0) memset(out, 'x', capacity);
    else return XIAOTAI_ERR_NOT_FOUND;
    return XIAOTAI_OK;
}

static xiaotai_err_t store_int(void *self, const char *key, int32_t value)
{
    CHECK(xiaotai_on_owner() && self == &s_store_cookie);
    CHECK(strcmp(key, "level") == 0 && value == 7);
    ++s_int_stores;
    CHECK(fw_settings_confirm_int_selection("test.level", value) == XIAOTAI_ERR_BUSY);
    CHECK(fw_settings_confirm_text("test.short", "new") == XIAOTAI_ERR_BUSY);
    return XIAOTAI_OK;
}

static xiaotai_err_t store_text(void *self, const char *key, const char *value)
{
    CHECK(xiaotai_on_owner() && self == &s_store_cookie && strcmp(key, "short") == 0);
    ++s_text_stores;
    if (s_reenter_store) {
        s_reenter_store = false;
        char visible[XIAOTAI_SETTING_TEXT_MAX];
        CHECK(xiaotai_setting_get_text("test.short", visible, sizeof(visible)) == XIAOTAI_OK);
        CHECK(strcmp(value, "one") == 0 && strcmp(visible, value) == 0);
        CHECK(s_failed_revision && xiaotai_setting_store_revision(key) == s_failed_revision);
        CHECK(fw_settings_confirm_text("test.short", "one") == XIAOTAI_ERR_BUSY);
        s_confirm_input[0] = 'x';
        CHECK(xiaotai_setting_set_text("test.short", "alt") == XIAOTAI_OK);
        CHECK(strcmp(value, "one") == 0);
        return XIAOTAI_FAIL;
    }
    CHECK(strcmp(value, "new") == 0 || strcmp(value, "one") == 0 || strcmp(value, "alt") == 0);
    memcpy(s_stored, value, strlen(value) + 1U);
    return XIAOTAI_OK;
}

static void on_changed(const char *name, void *ctx)
{
    (void)ctx;
    CHECK(xiaotai_on_owner() && strcmp(name, "test.short") == 0);
    ++s_changes;
}

static void on_int_changed(const char *name, void *ctx)
{
    CHECK(xiaotai_on_owner() && !ctx && !strcmp(name, "test.level"));
    ++s_int_changes;
}

static void on_not_saved(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    CHECK(xiaotai_on_owner() && strcmp(event->args.text, "test.short") == 0 && event->args.value == XIAOTAI_FAIL);
    CHECK(event->data && event->size == sizeof(s_failed_revision));
    uint32_t revision;
    memcpy(&revision, event->data, sizeof(revision));
    CHECK(revision == s_failed_revision && !xiaotai_setting_failure_current(event));
    CHECK(++s_failed_events == 1U);
    xiaotai_signal_give(&s_failure_done);
}

static void expect_text(const char *name, const char *expected)
{
    char out[XIAOTAI_SETTING_TEXT_MAX];
    CHECK(xiaotai_setting_get_text(name, out, sizeof(out)) == XIAOTAI_OK);
    CHECK(strcmp(out, expected) == 0);
}

static void register_text(const xiaotai_setting_t *setting, size_t max, bool per_setting)
{
    const usage_t before = usage();
    CHECK((per_setting ? xiaotai_setting_register_text(setting, max) : xiaotai_setting_register(setting)) == XIAOTAI_OK);
    const usage_t after = usage();
    CHECK(after.count == before.count + 1U && after.bytes == before.bytes + max + 1U);
}

static const char s_long_default[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdefextra";

static xiaotai_err_t init(void)
{
    const xiaotai_setting_store_t store = {
        .load_int = load_int, .load_text = load_text, .store_int = store_int, .store_text = store_text,
        .self = &s_store_cookie,
    };
    CHECK(xiaotai_setting_store_set(&store) == XIAOTAI_OK);
    CHECK(xiaotai_event_subscribe(XIAOTAI_EVENT_SETTING_NOT_SAVED, on_not_saved, NULL) == XIAOTAI_OK);
    xiaotai_setting_t level = {
        .name = "test.level", .key = "level", .kind = XIAOTAI_SETTING_INT, .min = 0, .max = 9, .default_value = 2,
        .changed = on_int_changed,
    };
    const xiaotai_setting_t enabled = {
        .name = "test.enabled", .key = "enabled", .kind = XIAOTAI_SETTING_BOOL, .default_value = 1,
    };
    const usage_t before = usage();
    CHECK(xiaotai_setting_register(&level) == XIAOTAI_OK);
    const usage_t scalar = usage();
    CHECK(scalar.count == before.count + 1U);
    const size_t metadata_max = XIAOTAI_FRAMEWORK_MAX_SETTINGS * (sizeof(xiaotai_setting_t) + 3U * sizeof(uintptr_t));
    printf("settings metadata: %zu bytes (compact limit: %zu)\n", scalar.bytes - before.bytes, metadata_max);
    fflush(stdout);
    CHECK(scalar.bytes > before.bytes && scalar.bytes - before.bytes <= metadata_max);
    const xiaotai_setting_t *published = xiaotai_setting_at(0);
    CHECK(published && published != &level);
    level.name = "test.changed_original";
    CHECK(strcmp(published->name, "test.level") == 0);
    CHECK(xiaotai_setting_register(&enabled) == XIAOTAI_OK);
    CHECK(usage().bytes == scalar.bytes);

    const xiaotai_setting_t short_text = {
        .name = "test.short", .key = "short", .kind = XIAOTAI_SETTING_TEXT,
        .default_text = "abc", .changed = on_changed,
    };
    const xiaotai_setting_t empty = {
        .name = "test.empty", .key = "empty", .kind = XIAOTAI_SETTING_TEXT, .default_text = "",
    };
    const xiaotai_setting_t generic = {
        .name = "test.generic", .key = "generic", .kind = XIAOTAI_SETTING_TEXT, .default_text = s_long_default,
    };
    const xiaotai_setting_t invalid = {
        .name = "test.invalid", .key = "invalid", .kind = XIAOTAI_SETTING_TEXT, .default_text = "ok",
    };
    CHECK(xiaotai_setting_register_text(&short_text, 0) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(s_text_loads == 0U && usage().bytes == scalar.bytes);
    register_text(&short_text, 3, true);
    register_text(&empty, 0, true);
    register_text(&generic, XIAOTAI_SETTING_TEXT_MAX - 1U, false);
    register_text(&invalid, 2, true);

    const xiaotai_setting_t retry = {
        .name = "test.retry", .key = "retry", .kind = XIAOTAI_SETTING_TEXT, .default_text = "ready",
    };
    const usage_t pending = usage();
    const size_t published_count = xiaotai_setting_count();
    CHECK(published_count == 6U && s_text_loads == 4U);
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_setting_register_text(&retry, 6) == XIAOTAI_ERR_NO_MEM);
    CHECK(!xiaotai_mem_host_fail_pending());
    CHECK(xiaotai_setting_count() == published_count && xiaotai_setting_at(published_count) == NULL);
    CHECK(usage().bytes == pending.bytes && s_text_loads == 4U);
    register_text(&retry, 6, true);
    CHECK(xiaotai_setting_at(0) == published && s_text_loads == 5U);
    return XIAOTAI_OK;
}

static void exercise_confirmation(void)
{
    const uint32_t allocations = xiaotai_mem_host_allocations();
    const uint32_t revision = xiaotai_setting_store_revision("short");
    CHECK(revision != 0U);
    CHECK(fw_settings_confirm_text("test.short", "new") == XIAOTAI_OK);
    CHECK(s_text_stores == 2U && s_changes == 1U && strcmp(s_stored, "new") == 0);
    CHECK(xiaotai_setting_store_revision("short") == revision);

    CHECK(fw_settings_confirm_text("test.short", "one") == XIAOTAI_OK);
    expect_text("test.short", "one");
    CHECK(s_text_stores == 3U && s_changes == 2U && strcmp(s_stored, "one") == 0);
    s_failed_revision = xiaotai_setting_store_revision("short");
    CHECK(s_failed_revision && s_failed_revision != revision);
    CHECK(fw_settings_confirm_text("test.short", "four") == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(fw_settings_confirm_text("test.level", "one") == XIAOTAI_ERR_NOT_FOUND);
    CHECK(fw_settings_confirm_text("test.absent", "one") == XIAOTAI_ERR_NOT_FOUND);
    CHECK(s_text_loads == 5U && xiaotai_mem_host_allocations() == allocations);

    uint32_t ticket = 0;
    CHECK(xiaotai_quiesce_begin("settings-storage-test", &ticket) == XIAOTAI_OK && ticket != 0);
    CHECK(fw_settings_confirm_text("test.short", "one") == XIAOTAI_ERR_BUSY);
    CHECK(fw_settings_confirm_text("test.short", "alt") == XIAOTAI_ERR_BUSY);
    xiaotai_quiesce_end(ticket);
    CHECK(s_text_stores == 3U && s_changes == 2U && xiaotai_setting_store_revision("short") == s_failed_revision);

    /* Only the nested SET changes the registry. The failed confirmation must
     * keep its snapshot and report the revision it sent to the store. */
    s_reenter_store = true;
    CHECK(fw_settings_confirm_text("test.short", s_confirm_input) == XIAOTAI_OK);
    expect_text("test.short", "alt");
    CHECK(s_text_stores == 5U && s_changes == 3U && strcmp(s_stored, "alt") == 0);
    CHECK(xiaotai_setting_store_revision("short") != s_failed_revision);
}

static void exercise(void *ctx)
{
    (void)ctx;
    const uint32_t allocations = xiaotai_mem_host_allocations();
    int32_t value;
    CHECK(xiaotai_setting_get_int("test.level", &value) == XIAOTAI_OK && value == 5);
    CHECK(xiaotai_setting_get_int("test.enabled", &value) == XIAOTAI_OK && value == 1);
    expect_text("test.short", "old");
    expect_text("test.empty", "");
    expect_text("test.invalid", "ok");
    expect_text("test.retry", "ready");
    char generic[XIAOTAI_SETTING_TEXT_MAX];
    CHECK(xiaotai_setting_get_text("test.generic", generic, sizeof(generic)) == XIAOTAI_OK);
    CHECK(generic[64] == '\0' && memcmp(generic, s_long_default, 64) == 0);
    CHECK(xiaotai_setting_set_text("test.short", "four") == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_setting_set_text("test.empty", "x") == XIAOTAI_ERR_INVALID_SIZE);
    expect_text("test.short", "old");
    CHECK(s_text_stores == 0U && s_changes == 0U);
    CHECK(xiaotai_setting_set_text("test.short", "new") == XIAOTAI_OK);
    CHECK(xiaotai_setting_set_text("test.short", "new") == XIAOTAI_OK);
    CHECK(s_text_stores == 1U && s_changes == 1U && strcmp(s_stored, "new") == 0);
    expect_text("test.short", "new");
    CHECK(xiaotai_setting_set_int("test.level", 7) == XIAOTAI_OK);
    CHECK(xiaotai_setting_get_int("test.level", &value) == XIAOTAI_OK && value == 7);
    CHECK(s_int_stores == 1U && s_int_changes == 1U);
    const uint32_t revision = xiaotai_setting_store_revision("level");
    CHECK(revision != 0U && xiaotai_setting_set_int("test.level", 7) == XIAOTAI_OK);
    CHECK(fw_settings_confirm_int_selection("test.level", 8) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(fw_settings_confirm_int_selection("test.level", 10) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(fw_settings_confirm_int_selection("test.short", 7) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(s_int_stores == 1U && s_int_changes == 1U);
    CHECK(fw_settings_confirm_int_selection("test.level", 7) == XIAOTAI_OK);
    CHECK(s_int_stores == 2U && s_int_changes == 1U && xiaotai_setting_store_revision("level") == revision);
    CHECK(xiaotai_mem_host_allocations() == allocations);
    exercise_confirmation();
    xiaotai_signal_give(&s_done);
}

static void expect_no_default_value(const char *key)
{
    char value[XIAOTAI_SETTING_TEXT_MAX];
    size_t size = sizeof(value);
    CHECK(xiaotai_kv_get_blob_strict("xt_settings", key, value, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
}

static unsigned s_fallback_changes;

static void fallback_changed(const char *name, void *ctx)
{
    CHECK(xiaotai_on_owner() && !ctx && strcmp(name, "test.fallback") == 0);
    ++s_fallback_changes;
}

static xiaotai_err_t fallback_init(void)
{
    const xiaotai_setting_t setting = {.name = "test.fallback", .key = "fallback",
        .kind = XIAOTAI_SETTING_TEXT, .default_text = "same", .changed = fallback_changed};
    return xiaotai_setting_register_text(&setting, 4);
}

static void exercise_fallback(void *ctx)
{
    const unsigned phase = *(const unsigned *)ctx;
    if (phase == 0U) {
        CHECK(xiaotai_setting_set_text("test.fallback", "same") == XIAOTAI_OK);
    } else {
        CHECK(fw_settings_confirm_text("test.fallback", phase == 1U ? "same" : "next") == XIAOTAI_OK);
    }
    CHECK(s_fallback_changes == (phase == 2U));
    xiaotai_signal_give(&s_done);
}

static int fallback(void)
{
    static const xiaotai_module_t settings = {.name = "test.settings", .init = fallback_init};
    static const xiaotai_module_t *const modules[] = {&settings};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1};
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_done) == XIAOTAI_OK);
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    for (unsigned phase = 0; phase < 3U; ++phase) {
        CHECK(xiaotai_run(exercise_fallback, &phase) == XIAOTAI_OK);
        CHECK(xiaotai_signal_take(&s_done, 2000));
        CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
        if (phase == 0U) {
            expect_no_default_value("fallback");
        } else {
            char value[5];
            size_t size = sizeof(value);
            CHECK(xiaotai_kv_get_blob_strict("xt_settings", "fallback", value, &size, 1000) == XIAOTAI_OK);
            CHECK(size == sizeof(value) && strcmp(value, phase == 1U ? "same" : "next") == 0);
        }
    }
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&s_done);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "fallback") == 0) return fallback();
    static const xiaotai_module_t settings = {.name = "test.settings", .init = init};
    static const xiaotai_module_t *const modules[] = {&settings};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1};
    xiaotai_mem_host_ledger_start();
    /* The KV port worker belongs to the process, not a framework instance. */
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_done) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_failure_done) == XIAOTAI_OK);
    const usage_t baseline = usage();
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    CHECK(fw_settings_confirm_text("test.short", "old") == XIAOTAI_ERR_INVALID_STATE);
    CHECK(fw_settings_confirm_int_selection("test.level", 5) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_run(exercise, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&s_done, 2000));
    CHECK(xiaotai_signal_take(&s_failure_done, 2000));
    CHECK(xiaotai_kv_flush(1000) == XIAOTAI_OK);
    expect_no_default_value("short");
    expect_no_default_value("level");
    xiaotai_framework_reset();
    const usage_t reset = usage();
    CHECK(xiaotai_setting_count() == 0U && xiaotai_setting_at(0) == NULL);
    if (reset.count != baseline.count || reset.bytes != baseline.bytes) {
        size_t count;
        CHECK(xiaotai_mem_host_ledger_snapshot(s_ledger, XIAOTAI_MEM_HOST_LEDGER_CAPACITY, &count));
        for (size_t i = 0; i < count; ++i) {
            if (!s_ledger[i].task && !s_ledger[i].fixture)
                fprintf(stderr, "live registered allocation: %zu bytes, return PC %p\n",
                        s_ledger[i].bytes, (void *)s_ledger[i].site);
        }
    }
    CHECK(reset.count == baseline.count && reset.bytes == baseline.bytes);
    xiaotai_signal_deinit(&s_done);
    xiaotai_signal_deinit(&s_failure_done);
    return 0;
}
