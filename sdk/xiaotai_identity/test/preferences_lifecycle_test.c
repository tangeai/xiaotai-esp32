// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_settings.c::settings
// Replaces legacy: tests/regression/test_environment_owners.py
/* Exercise owner timers, storage-worker callbacks and restart through the
 * real framework. The task wrapper only records OS task identities/counts. */
#include <stdatomic.h>

#include "preferences_test.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/port_config.h"

static atomic_uint s_tasks;
static xiaotai_task_t s_worker;

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task)
{
    atomic_fetch_add(&s_tasks, 1U);
    const xiaotai_err_t result = __real_xiaotai_task_create(cfg, task);
    if (result == XIAOTAI_OK && !strcmp(cfg->name, "xiaotai_kv")) s_worker = *task;
    return result;
}

static void expect_number(const char *key, int32_t desired)
{
    expect_pref_int(key, -1, desired);
}

static void pending(void)
{
    boot(NULL, 0, 30, NULL);
    CHECK(!xiaotai_preferences_pending());
    xiaotai_kv_host_gate(false);
    set_int("level", 111, XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
    CHECK(xiaotai_preferences_pending());
    set_int("level", 112, XIAOTAI_OK);
    CHECK(xiaotai_preferences_pending());
    CHECK(pref_stats().stores == 2 && kv_counts().commits == 0);
    xiaotai_kv_host_gate(true);
    await_idle();
    CHECK(pref_stats().writes == 2 && kv_counts().commits == 2);
    expect_number("level", 112);
}

static void grouped_changes(void *ctx)
{
    (void)ctx;
    CHECK(xiaotai_preferences_store_int("level", 10) == XIAOTAI_OK);
    CHECK(xiaotai_preferences_store_int("enabled", 1) == XIAOTAI_OK);
    CHECK(xiaotai_preferences_store_int("level", 20) == XIAOTAI_OK);
    CHECK(xiaotai_preferences_store_int("level", 30) == XIAOTAI_OK);
    CHECK(xiaotai_preferences_pending() && kv_counts().writes == 0);
    CHECK(pref_stats().pending == 2 && pref_stats().coalesced == 2);
}

static void coalesce(void)
{
    boot(NULL, 0, 65, NULL);
    owner_call(grouped_changes, NULL);
    await_idle(); /* no explicit flush: the owner timer must hand off the batch */
    CHECK(kv_counts().commits == 1 && kv_counts().writes == 2);
    CHECK(pref_stats().stores == 4 && pref_stats().writes == 2 && pref_stats().pending == 0);
    expect_number("level", 30);
    expect_number("enabled", 1);
}

static void fill_slots(void)
{
    const xiaotai_kv_op_t op = {.kind = XIAOTAI_KV_OP_ERASE, .key = "filler"};
    xiaotai_kv_host_gate(false);
    for (unsigned i = 0; i < XIAOTAI_KV_SLOTS; ++i) {
        CHECK(xiaotai_kv_commit_async("busy", &op, 1, NULL, NULL) == XIAOTAI_OK);
    }
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
}

static void unexpected_timer(void *ctx)
{
    (void)ctx;
    CHECK(false);
}

static void with_no_timer_slot(void *ctx)
{
    (void)ctx;
    uint32_t timers[XIAOTAI_FRAMEWORK_MAX_TIMERS];
    for (size_t i = 0; i < COUNT(timers); ++i) {
        CHECK(xiaotai_timer_start(60000, unexpected_timer, NULL, &timers[i]) == XIAOTAI_OK);
    }
    const uint32_t deferred = pref_stats().deferred;
    CHECK(xiaotai_preferences_store_int("level", 89) == XIAOTAI_OK);
    CHECK(xiaotai_preferences_pending() && pref_stats().deferred == deferred + 1);
    for (size_t i = 0; i < COUNT(timers); ++i) xiaotai_timer_cancel(timers[i]);
}

static void no_task(void)
{
    boot(NULL, 0, 30, NULL);
    fill_slots();
    const unsigned tasks = atomic_load(&s_tasks);
    const uint32_t allocations = xiaotai_mem_host_allocations();
    set_int("level", 88, XIAOTAI_OK);
    for (unsigned i = 0; i < WAIT_MS / 5 && !pref_stats().deferred; ++i) xiaotai_delay_ms(5);
    CHECK(pref_stats().deferred > 0 && pref_stats().pending == 1);
    CHECK(xiaotai_preferences_pending());
    CHECK(atomic_load(&s_tasks) == tasks && xiaotai_mem_host_allocations() == allocations);
    xiaotai_kv_host_gate(true);
    await_idle();
    CHECK(atomic_load(&s_tasks) == tasks && xiaotai_mem_host_allocations() == allocations);
    expect_number("level", 88);
    owner_call(with_no_timer_slot, NULL);
    set_int("enabled", 1, XIAOTAI_OK); /* another store can arm the timer again */
    await_idle();
    expect_number("level", 89);
    expect_number("enabled", 1);
    CHECK(atomic_load(&s_tasks) == tasks && xiaotai_mem_host_allocations() == allocations);
}

static void owner_only(void)
{
    boot(NULL, 0, 30, NULL);
    CHECK(!xiaotai_on_owner());
    CHECK(xiaotai_preferences_store_int("level", 99) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_preferences_store_text("text", "foreign thread") == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!xiaotai_preferences_pending() && kv_counts().writes == 0);
    set_int("level", 88, XIAOTAI_OK);
    await_idle();
    expect_number("level", 88);
}

static void open_gate(void *ctx)
{
    (void)ctx;
    xiaotai_kv_host_gate(true);
}

static void schedule_gate_open(void *ctx)
{
    (void)ctx;
    uint32_t timer;
    CHECK(xiaotai_timer_start(70, open_gate, NULL, &timer) == XIAOTAI_OK);
}

static void flush(void)
{
    /* Hold the real worker and exhaust all slots. A timed-out flush leaves
     * the setting queued; the next flush must retry it before the owner timer. */
    boot(NULL, 0, 5000, NULL);
    fill_slots();
    set_int("level", 40, XIAOTAI_OK);
    CHECK(xiaotai_preferences_flush(20) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_preferences_pending() && pref_stats().pending == 1);
    owner_call(schedule_gate_open, NULL);
    CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    CHECK(!xiaotai_preferences_pending() && kv_counts().commits == XIAOTAI_KV_SLOTS + 1U);
    expect_number("level", 40);
    /* Timing out with a submitted batch is also non-cancelling. */
    xiaotai_kv_host_gate(false);
    set_int("level", 41, XIAOTAI_OK);
    CHECK(xiaotai_preferences_flush(20) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_preferences_pending() && xiaotai_kv_host_wait_held(WAIT_MS));
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    expect_number("level", 41);

    test_reset();
    boot(NULL, 0, 30, NULL);
    xiaotai_kv_host_fail_key("level", XIAOTAI_FAIL);
    set_int("level", 43, XIAOTAI_OK);
    await_idle();
    xiaotai_kv_host_fail_key("level", XIAOTAI_ERR_INVALID_ARG);
    set_int("level", 44, XIAOTAI_OK);
    await_idle();
    CHECK(pref_stats().write_failures == 2);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    const xiaotai_kv_host_counts_t before = kv_counts();
    CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_FAIL);
    CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    CHECK(kv_counts().writes == before.writes && kv_counts().commits == before.commits);
    expect_number("level", 41);
}

static void maintenance_failure(void)
{
    static const xiaotai_preference_binding_t bindings[] = {
        {.setting = "level", .storage = XIAOTAI_PREFERENCE_INT, .space = "product", .key = "level",
         .int_type = XIAOTAI_KV_U8, .raw_max = 10},
        {.setting = "mic", .storage = XIAOTAI_PREFERENCE_INT, .space = "product", .key = "mic",
         .int_type = XIAOTAI_KV_U8, .raw_max = 1},
    };
    xiaotai_preferences_config_t config = {.retain_maintenance_failures = true};
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_preference_binding_t snapshot = bindings[0];
    snapshot.snapshot = true;
    config.bindings = &snapshot;
    config.binding_count = 1;
    const uint32_t allocations = xiaotai_mem_host_allocations();
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_mem_host_fail_pending() && xiaotai_mem_host_allocations() == allocations);

    config.bindings = bindings;
    config.binding_count = COUNT(bindings);
    config.write_delay_ms = 20;
    const int64_t outstanding = xiaotai_mem_host_outstanding();
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_ERR_NO_MEM);
    CHECK(!xiaotai_mem_host_fail_pending() && xiaotai_mem_host_outstanding() == outstanding);
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_OK);
    CHECK(xiaotai_framework_start(&(xiaotai_framework_config_t){0}) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("product", "level", 5, WAIT_MS) == XIAOTAI_OK);

    xiaotai_kv_host_fail_key("level", XIAOTAI_FAIL);
    set_int("level", 6, XIAOTAI_OK);
    await_idle();
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_FAIL);
    CHECK(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    const uint32_t commits = kv_counts().commits;
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_FAIL);
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_FAIL);
    CHECK(kv_counts().commits == commits);
    expect_int("product", "level", XIAOTAI_KV_U8, 5);

    set_int("mic", 1, XIAOTAI_OK);
    await_idle();
    expect_int("product", "mic", XIAOTAI_KV_U8, 1);
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_FAIL);

    xiaotai_kv_host_gate(false);
    set_int("level", 7, XIAOTAI_OK);
    CHECK(xiaotai_preferences_maintenance_flush(20) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_preferences_pending() && xiaotai_kv_host_wait_held(WAIT_MS));
    xiaotai_kv_host_fail_key("level", XIAOTAI_ERR_TIMEOUT);
    xiaotai_kv_host_gate(true);
    await_idle();
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_ERR_TIMEOUT);
    expect_int("product", "level", XIAOTAI_KV_U8, 5);

    set_int("level", 8, XIAOTAI_OK);
    await_idle();
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_OK);
    expect_int("product", "level", XIAOTAI_KV_U8, 8);
}

typedef struct {
    unsigned calls;
    xiaotai_err_t result;
    char key[XIAOTAI_KV_NAME_MAX + 1U];
} listener_log_t;

static void written(const char *key, xiaotai_err_t result, void *ctx)
{
    listener_log_t *log = ctx;
    CHECK(xiaotai_task_is_current(&s_worker) && !xiaotai_on_owner());
    CHECK(strlen(key) < sizeof(log->key));
    strcpy(log->key, key);
    log->result = result;
    ++log->calls;
}

static void listener(void)
{
    listener_log_t log = {0};
    boot(NULL, 0, 35, NULL);
    CHECK(xiaotai_preferences_set_listener(written, &log) == XIAOTAI_OK);
    CHECK(xiaotai_preferences_set_listener(written, &log) == XIAOTAI_ERR_INVALID_STATE);
    char key[] = "copied";
    set_int(key, 10, XIAOTAI_OK);
    memset(key, 'x', sizeof(key) - 1);
    await_idle();
    CHECK(log.calls == 1 && log.result == XIAOTAI_OK && !strcmp(log.key, "copied"));
    xiaotai_kv_host_fail_key("failure", XIAOTAI_FAIL);
    set_int("failure", 11, XIAOTAI_OK);
    await_idle();
    CHECK(log.calls == 2 && log.result == XIAOTAI_FAIL && !strcmp(log.key, "failure"));
    CHECK(xiaotai_preferences_set_listener(NULL, NULL) == XIAOTAI_OK);
    set_int("silent", 12, XIAOTAI_OK);
    await_idle();
    CHECK(log.calls == 2);
}

static xiaotai_signal_t s_failed, s_restarted;
static unsigned s_failures;

static void setting_not_saved(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    CHECK(xiaotai_on_owner());
    CHECK(!strcmp(event->args.text, "test.volume") && event->args.value == XIAOTAI_FAIL);
    ++s_failures;
    xiaotai_signal_give(&s_failed);
}

static xiaotai_err_t register_settings(void)
{
    const xiaotai_setting_t volume = {.name = "test.volume", .key = "volume",
        .kind = XIAOTAI_SETTING_INT, .min = 0, .max = 100, .default_value = 40};
    const xiaotai_setting_t peer = {.name = "test.peer", .key = "peer",
        .kind = XIAOTAI_SETTING_TEXT, .default_text = "no-peer"};
    CHECK(xiaotai_event_subscribe(XIAOTAI_EVENT_SETTING_NOT_SAVED, setting_not_saved, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_setting_register(&volume) == XIAOTAI_OK);
    CHECK(xiaotai_setting_register_text(&peer, 64) == XIAOTAI_OK);
    return XIAOTAI_OK;
}

static void restart_host(void)
{
    CHECK(!xiaotai_on_owner());
    CHECK(!xiaotai_preferences_pending());
    xiaotai_signal_give(&s_restarted);
}

static void change_framework_setting(void *ctx)
{
    const int32_t volume = *(const int32_t *)ctx;
    CHECK(xiaotai_setting_set_int("test.volume", volume) == XIAOTAI_OK);
}

static void start_restart(void *ctx)
{
    change_framework_setting(ctx);
    CHECK(xiaotai_preferences_pending());
    CHECK(xiaotai_setting_set_text("test.peer", "next-peer") == XIAOTAI_OK);
    CHECK(xiaotai_restart_request("prefs-test", 0) == XIAOTAI_OK);
}

static void check_settings(int32_t volume, const char *peer)
{
    int32_t value;
    char text[65];
    CHECK(xiaotai_setting_get_int("test.volume", &value) == XIAOTAI_OK && value == volume);
    CHECK(xiaotai_setting_get_text("test.peer", text, sizeof(text)) == XIAOTAI_OK && !strcmp(text, peer));
}

static void setting_store(void)
{
    static const xiaotai_preference_binding_t bindings[] = {
        {.setting = "volume", .space = "product", .key = "level", .raw_max = 100},
        {.setting = "peer", .space = "product", .key = "door_peer", .storage = XIAOTAI_PREFERENCE_STRING},
    };
    static const xiaotai_module_t identity = {.name = "identity", .init = xiaotai_engine_identity_init};
    static const char *const dependencies[] = {"identity", NULL};
    static const xiaotai_module_t settings = {.name = "settings", .depends = dependencies, .init = register_settings};
    static const xiaotai_module_t *const modules[] = {&identity, &settings};
    const xiaotai_framework_config_t framework = {
        .modules = modules, .module_count = COUNT(modules), .restart_delay_ms = 1, .restart = restart_host,
    };
    CHECK(xiaotai_signal_init(&s_failed) == XIAOTAI_OK && xiaotai_signal_init(&s_restarted) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("product", "level", 60, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_str("product", "door_peer", "old-peer", WAIT_MS) == XIAOTAI_OK);
    boot(bindings, COUNT(bindings), 45, &framework);
    check_settings(60, "old-peer");
    int32_t next = 70;
    owner_call(change_framework_setting, &next);
    await_idle();
    expect_int("product", "level", XIAOTAI_KV_U8, 70);
    xiaotai_kv_host_fail_key("level", XIAOTAI_FAIL);
    next = 71;
    owner_call(change_framework_setting, &next);
    CHECK(xiaotai_signal_take(&s_failed, WAIT_MS) && s_failures == 1);
    await_idle();
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    expect_int("product", "level", XIAOTAI_KV_U8, 70);
    test_reset();

    /* A timer five seconds away cannot have written before the real ordered
     * restart. It must flush while settings are quiesced in restart step 4. */
    boot(bindings, COUNT(bindings), 5000, &framework);
    check_settings(70, "old-peer");
    next = 90;
    owner_call(start_restart, &next);
    CHECK(xiaotai_signal_take(&s_restarted, WAIT_MS));
    expect_int("product", "level", XIAOTAI_KV_U8, 90);
    char text[65];
    size_t size = sizeof(text);
    CHECK(xiaotai_kv_get_str("product", "door_peer", text, sizeof(text), WAIT_MS) == XIAOTAI_OK);
    CHECK(!strcmp(text, "next-peer"));
    CHECK(xiaotai_kv_get("xt_settings", "volume", text, &size, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_kv_get("xt_settings", "peer", text, &size, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    test_reset();
    boot(bindings, COUNT(bindings), 45, &framework);
    check_settings(90, "next-peer");
    CHECK(s_failures == 1);
    xiaotai_signal_deinit(&s_failed);
    xiaotai_signal_deinit(&s_restarted);
}

int main(int argc, char **argv)
{
    static const struct {const char *name; void (*run)(void);} cases[] = {
        {"pending", pending}, {"coalesce", coalesce}, {"no_task", no_task}, {"owner_only", owner_only},
        {"flush", flush}, {"listener", listener}, {"setting_store", setting_store},
        {"maintenance_failure", maintenance_failure},
    };
    const char *name = argc == 2 ? argv[1] : "pending";
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
