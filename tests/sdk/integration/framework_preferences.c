// SPDX-License-Identifier: MIT
#include <assert.h>
#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/preferences.h"
#include "xiaotai_kv_host.h"

enum { WAIT_MS = 2000 };

static const xiaotai_preference_binding_t bindings[] = {
    {.setting = "volume", .storage = XIAOTAI_PREFERENCE_INT, .space = "product", .key = "level", .raw_max = 100},
    {.setting = "bounded", .storage = XIAOTAI_PREFERENCE_STRING, .space = "product", .key = "short_text"},
};
static const xiaotai_preferences_config_t preferences = {
    .bindings = bindings, .binding_count = sizeof(bindings) / sizeof(bindings[0]), .write_delay_ms = -1,
};
static xiaotai_signal_t synced;
static unsigned bounded_changes, not_saved;
static xiaotai_err_t set_result;

static void sync_owner(void *ctx) { (void)ctx; xiaotai_signal_give(&synced); }
static void drain(void)
{
    assert(xiaotai_run(sync_owner, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&synced, WAIT_MS));
}
static void set_text(void *ctx) { set_result = xiaotai_setting_set_text("test.bounded", ctx); }
static void set_volume(void *ctx) { (void)ctx; set_result = xiaotai_setting_set_int("audio.volume", 75); }
static void on_owner(void (*fn)(void *), void *ctx)
{
    assert(xiaotai_run(fn, ctx) == XIAOTAI_OK);
    drain();
}

static void bounded_changed(const char *name, void *ctx) { (void)name; (void)ctx; ++bounded_changes; }
static void on_not_saved(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    assert(!strcmp(event->args.text, "audio.volume") && event->args.value == XIAOTAI_ERR_TIMEOUT);
    ++not_saved;
}

static xiaotai_err_t register_settings(void)
{
    const xiaotai_setting_t volume = {.name = "audio.volume", .key = "volume", .kind = XIAOTAI_SETTING_INT,
                                    .min = 0, .max = 100, .default_value = 60};
    assert(xiaotai_setting_register(&volume) == XIAOTAI_OK);
    xiaotai_setting_t setting = {.name = "test.bounded", .key = "bounded", .kind = XIAOTAI_SETTING_INT};
    assert(xiaotai_setting_register_text(&setting, 3) == XIAOTAI_ERR_INVALID_ARG);
    setting.kind = XIAOTAI_SETTING_TEXT;
    assert(xiaotai_setting_register_text(&setting, XIAOTAI_SETTING_TEXT_MAX) == XIAOTAI_ERR_INVALID_ARG);
    setting.default_text = "four";
    assert(xiaotai_setting_register_text(&setting, 3) == XIAOTAI_ERR_INVALID_SIZE);
    setting.default_text = "ok";
    setting.changed = bounded_changed;
    assert(xiaotai_setting_register_text(&setting, 3) == XIAOTAI_OK);
    return xiaotai_event_subscribe(XIAOTAI_EVENT_SETTING_NOT_SAVED, on_not_saved, NULL);
}

static xiaotai_err_t late_identity(void)
{
    assert(xiaotai_engine_identity_init() == XIAOTAI_ERR_INVALID_STATE);
    return XIAOTAI_OK;
}
static void reset(void)
{
    xiaotai_framework_reset();
    xiaotai_preferences_reset();
    xiaotai_identity_reset();
}
static void boot(const xiaotai_framework_config_t *framework, int32_t volume, const char *text)
{
    assert(xiaotai_preferences_configure(&preferences) == XIAOTAI_OK);
    assert(xiaotai_framework_start(framework) == XIAOTAI_OK);
    int32_t actual;
    char bounded[4];
    assert(xiaotai_setting_get_int("audio.volume", &actual) == XIAOTAI_OK && actual == volume);
    assert(xiaotai_setting_get_text("test.bounded", bounded, sizeof(bounded)) == XIAOTAI_OK && !strcmp(bounded, text));
}

int framework_product_preferences_test(void)
{
    static const xiaotai_module_t settings = {.name = "settings", .init = register_settings};
    static const xiaotai_module_t identity = {.name = "identity", .init = xiaotai_engine_identity_init};
    static const char *const late_deps[] = {"settings", NULL};
    static const xiaotai_module_t late = {.name = "late_identity", .depends = late_deps, .init = late_identity};
    const xiaotai_module_t *late_order[] = {&settings, &late};
    assert(xiaotai_signal_init(&synced) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = late_order, .module_count = 2}) == XIAOTAI_OK);
    reset();

    /* The real product order installs the store before any registration. */
    const xiaotai_module_t *modules[] = {&identity, &settings};
    const xiaotai_framework_config_t framework = {.modules = modules, .module_count = 2};
    xiaotai_kv_host_clear();
    assert(xiaotai_kv_set_u8("product", "level", 63, WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_set_str("product", "short_text", "four", WAIT_MS) == XIAOTAI_OK);
    boot(&framework, 63, "ok");
    on_owner(set_text, "four");
    assert(set_result == XIAOTAI_ERR_INVALID_SIZE && !xiaotai_preferences_pending() && !bounded_changes);
    char text[8];
    assert(xiaotai_kv_get_str("product", "short_text", text, sizeof(text), WAIT_MS) == XIAOTAI_OK && !strcmp(text, "four"));
    on_owner(set_text, "new");
    assert(set_result == XIAOTAI_OK && bounded_changes == 1 && xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_get_str("product", "short_text", text, sizeof(text), WAIT_MS) == XIAOTAI_OK && !strcmp(text, "new"));

    /* A real failed KV completion, not an admission failure or a test notification. */
    xiaotai_kv_host_fail_key("level", XIAOTAI_ERR_TIMEOUT);
    xiaotai_kv_host_gate(false);
    on_owner(set_volume, NULL);
    assert(set_result == XIAOTAI_OK && !not_saved && xiaotai_kv_host_wait_held(WAIT_MS));
    int32_t volume;
    assert(xiaotai_setting_get_int("audio.volume", &volume) == XIAOTAI_OK && volume == 75);
    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    drain();
    assert(not_saved == 1 && xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    int64_t raw;
    assert(xiaotai_kv_get_int("product", "level", XIAOTAI_KV_U8, &raw, WAIT_MS) == XIAOTAI_OK && raw == 63);
    size_t size = sizeof(raw);
    assert(xiaotai_kv_get("xt_settings", "volume", &raw, &size, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    reset();

    boot(&framework, 63, "new");
    on_owner(set_volume, NULL);
    assert(set_result == XIAOTAI_OK && xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_get_int("product", "level", XIAOTAI_KV_U8, &raw, WAIT_MS) == XIAOTAI_OK && raw == 75);
    reset();
    boot(&framework, 75, "new");
    assert(not_saved == 1);
    reset();
    xiaotai_signal_deinit(&synced);
    return 0;
}
