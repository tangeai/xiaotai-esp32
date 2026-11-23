// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_native_preferences.py
#include <stdatomic.h>

#include "preferences_test.h"
#include "xiaotai/port_config.h"

#define IMAGE_SPACE "snapshot"
enum { CUTOFF_MS = 2000, LOCAL_SETTINGS = 16, LOCAL_TEXTS = 4 };

static atomic_int_fast64_t s_owner_now, s_last_try;
static atomic_uint s_tries, s_admitted, s_not_saved;
static int64_t s_clock_start;

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    const int64_t now = atomic_load(&s_owner_now);
    /* The owner timer uses manual time; KV and test waits keep a real clock. */
    return now && xiaotai_on_owner() ? now : __real_xiaotai_time_us();
}

xiaotai_err_t __real_xiaotai_kv_commit_async(const char *space, const xiaotai_kv_op_t *ops, size_t count,
                                             xiaotai_kv_done_fn_t done, void *ctx);
xiaotai_err_t __wrap_xiaotai_kv_commit_async(const char *space, const xiaotai_kv_op_t *ops, size_t count,
                                             xiaotai_kv_done_fn_t done, void *ctx)
{
    const xiaotai_err_t result = __real_xiaotai_kv_commit_async(space, ops, count, done, ctx);
    if (!strcmp(space, IMAGE_SPACE)) {
        CHECK(count == 2 && done);
        CHECK(!strcmp(ops[0].key, "level") && !strcmp(ops[1].key, "peer"));
        atomic_store(&s_last_try, xiaotai_time_us());
        atomic_fetch_add(&s_tries, 1U);
        if (result == XIAOTAI_OK) atomic_fetch_add(&s_admitted, 1U);
    }
    return result;
}

static void setting_not_saved(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    CHECK(!strcmp(event->args.text, "test.level"));
    CHECK(event->args.value == XIAOTAI_ERR_NO_MEM && xiaotai_setting_failure_current(event));
    atomic_fetch_add(&s_not_saved, 1U);
}

static xiaotai_err_t register_settings(void)
{
    const xiaotai_setting_t volume = {.name = "test.level", .key = "level",
        .kind = XIAOTAI_SETTING_INT, .min = 0, .max = 100, .default_value = 40};
    const xiaotai_setting_t peer = {.name = "test.peer", .key = "peer",
        .kind = XIAOTAI_SETTING_TEXT, .default_text = ""};
    CHECK(xiaotai_event_subscribe(XIAOTAI_EVENT_SETTING_NOT_SAVED, setting_not_saved, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_setting_register(&volume) == XIAOTAI_OK);
    CHECK(xiaotai_setting_register_text(&peer, XIAOTAI_PREFERENCE_TEXT_MAX - 1U) == XIAOTAI_OK);
    return XIAOTAI_OK;
}

static void start(void)
{
    static const xiaotai_preference_binding_t bindings[] = {
        {.setting = "level", .space = IMAGE_SPACE, .key = "level", .raw_max = 100, .snapshot = true},
        {.setting = "peer", .space = IMAGE_SPACE, .key = "peer",
         .storage = XIAOTAI_PREFERENCE_STRING, .snapshot = true},
    };
    static const xiaotai_module_t identity = {.name = "identity", .init = xiaotai_engine_identity_init};
    static const char *const dependencies[] = {"identity", NULL};
    static const xiaotai_module_t settings = {.name = "test-settings", .depends = dependencies, .init = register_settings};
    static const xiaotai_module_t *const modules[] = {&identity, &settings};
    const xiaotai_framework_config_t framework = {.modules = modules, .module_count = COUNT(modules)};
    CHECK(xiaotai_kv_set_u8(IMAGE_SPACE, "level", 40, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_str(IMAGE_SPACE, "peer", "before", WAIT_MS) == XIAOTAI_OK);
    boot(bindings, COUNT(bindings), 5000, &framework);
    s_clock_start = __real_xiaotai_time_us();
    atomic_store(&s_owner_now, s_clock_start);
}

static void no_op(void *ctx) { (void)ctx; }

static void step(unsigned elapsed_ms)
{
    atomic_store(&s_owner_now, s_clock_start + 1000 * (int64_t)elapsed_ms);
    /* Timers run between owner messages; a third turn drains their notices. */
    for (unsigned i = 0; i < 3; ++i) owner_call(no_op, NULL);
}

static void edit_level(void *ctx)
{
    CHECK(xiaotai_setting_set_int("test.level", *(const int32_t *)ctx) == XIAOTAI_OK);
}

static void edit_peer(void *ctx)
{
    CHECK(xiaotai_setting_set_text("test.peer", ctx) == XIAOTAI_OK);
}

static void set_level(int32_t value) { owner_call(edit_level, &value); }

static void expect_peer(const char *expected)
{
    char copy[XIAOTAI_PREFERENCE_TEXT_MAX];
    CHECK(xiaotai_kv_get_str_strict(IMAGE_SPACE, "peer", copy, sizeof(copy), WAIT_MS) == XIAOTAI_OK);
    CHECK(!strcmp(copy, expected));
}

static void fill_worker(void)
{
    const xiaotai_kv_op_t filler = {.kind = XIAOTAI_KV_OP_ERASE, .key = "filler"};
    xiaotai_kv_host_gate(false);
    for (unsigned i = 0; i < XIAOTAI_KV_SLOTS; ++i) {
        CHECK(xiaotai_kv_commit_async("busy", &filler, 1, NULL, NULL) == XIAOTAI_OK);
    }
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
}

static void full_at_start(void)
{
    fill_worker();
    set_level(60);
    step(0);
    CHECK(xiaotai_preferences_pending() && atomic_load(&s_tries) == 1);
    CHECK(atomic_load(&s_last_try) == s_clock_start && atomic_load(&s_admitted) == 0);
    step(50);
    step(99);
    CHECK(atomic_load(&s_tries) == 1); /* No probing between retry points. */
    step(CUTOFF_MS - 1U);
    CHECK(atomic_load(&s_tries) == 2 && atomic_load(&s_not_saved) == 0);
    CHECK(xiaotai_preferences_pending() && atomic_load(&s_admitted) == 0);
}

static void expire(void)
{
    full_at_start();
    step(CUTOFF_MS);
    CHECK(atomic_load(&s_last_try) == s_clock_start + 1000 * CUTOFF_MS);
    CHECK(atomic_load(&s_tries) == 3 && atomic_load(&s_admitted) == 0);
    CHECK(atomic_load(&s_not_saved) == 1 && !xiaotai_preferences_pending());
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_ERR_NO_MEM);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    step(CUTOFF_MS + 1000U);
    CHECK(atomic_load(&s_tries) == 3 && atomic_load(&s_not_saved) == 1);
    expect_int(IMAGE_SPACE, "level", XIAOTAI_KV_U8, 40);
    expect_peer("before");
}

static void capacity_at_cutoff(void)
{
    full_at_start();
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    step(CUTOFF_MS);
    await_idle();
    CHECK(atomic_load(&s_last_try) == s_clock_start + 1000 * CUTOFF_MS);
    CHECK(atomic_load(&s_tries) == 3 && atomic_load(&s_admitted) == 1 && atomic_load(&s_not_saved) == 0);
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_OK);
    expect_int(IMAGE_SPACE, "level", XIAOTAI_KV_U8, 60);
    expect_peer("before");
}

static void admitted_does_not_expire(void)
{
    xiaotai_kv_host_gate(false);
    set_level(61);
    step(0);
    CHECK(xiaotai_kv_host_wait_held(WAIT_MS) && atomic_load(&s_admitted) == 1);
    CHECK(xiaotai_preferences_maintenance_flush(20) == XIAOTAI_ERR_TIMEOUT);
    step(CUTOFF_MS + 1U);
    CHECK(xiaotai_preferences_pending() && atomic_load(&s_not_saved) == 0);
    CHECK(atomic_load(&s_tries) == 1);
    static char newer_peer[] = "edited behind an admitted job";
    owner_call(edit_peer, newer_peer);
    step(CUTOFF_MS + 1U);
    CHECK(atomic_load(&s_tries) == 1);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    expect_int(IMAGE_SPACE, "level", XIAOTAI_KV_U8, 61);
    expect_peer("before");
    step(CUTOFF_MS + 101U);
    await_idle();
    CHECK(atomic_load(&s_tries) == 2 && atomic_load(&s_not_saved) == 0);
    CHECK(xiaotai_preferences_maintenance_flush(WAIT_MS) == XIAOTAI_OK);
    expect_int(IMAGE_SPACE, "level", XIAOTAI_KV_U8, 61);
    expect_peer(newer_peer);
}

static void fill_local_staging(void *ctx)
{
    for (unsigned i = 0; i < LOCAL_SETTINGS; ++i) {
        char key[XIAOTAI_KV_NAME_MAX + 1U];
        snprintf(key, sizeof(key), "extra%u", i);
        const xiaotai_err_t result = i < LOCAL_TEXTS ? xiaotai_preferences_store_text(key, "staged")
                                                   : xiaotai_preferences_store_int(key, (int32_t)i);
        CHECK(result == XIAOTAI_OK);
    }
    CHECK(xiaotai_preferences_store_int("overflow", 0) == XIAOTAI_ERR_NO_MEM);
    edit_peer(ctx);
}

static void local_staging_is_independent(void)
{
    static char peer[] = "snapshot still writable";
    const uint32_t commits = kv_counts().commits;
    owner_call(fill_local_staging, peer);
    step(0);
    CHECK(atomic_load(&s_admitted) == 1 && xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_preferences_pending() && kv_counts().commits == commits + 1U);
    CHECK(atomic_load(&s_not_saved) == 0);
    expect_int(IMAGE_SPACE, "level", XIAOTAI_KV_U8, 40);
    expect_peer(peer);
    step(5000);
    await_idle();
}

int main(int argc, char **argv)
{
    test_clear();
    start();
    if (argc == 1) expire();
    else if (!strcmp(argv[1], "open")) capacity_at_cutoff();
    else if (!strcmp(argv[1], "admitted")) admitted_does_not_expire();
    else if (!strcmp(argv[1], "staging")) local_staging_is_independent();
    else CHECK(false);
    atomic_store(&s_owner_now, 0);
    test_reset();
    return 0;
}
