// SPDX-License-Identifier: MIT
/* xiaotai/port/log.h on the host: the app threshold, module thresholds and
 * the thresholds kept across restarts. */
#include "xiaotai/port/log.h"

#include <assert.h>
#include <string.h>

#include "xiaotai/port/kv.h"

static void test_app_threshold(void)
{
    /* The default is the story: info on, debug off. */
    assert(xiaotai_log_level() == XIAOTAI_LOG_LEVEL_INFO && xiaotai_log_level_max() == XIAOTAI_LOG_LEVEL_DEBUG);
    assert(xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_ERROR, "call") && xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_INFO, "call"));
    assert(!xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "call"));
    assert(xiaotai_log_set_level(5) == XIAOTAI_ERR_INVALID_ARG && xiaotai_log_level() == XIAOTAI_LOG_LEVEL_INFO);
    assert(xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_OFF) == XIAOTAI_OK);
    assert(!xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_ERROR, "call") && !xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_ERROR, NULL));
    assert(xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_DEBUG) == XIAOTAI_OK);
    assert(xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "call"));
    assert(xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_INFO) == XIAOTAI_OK);
}

static void test_module_thresholds(void)
{
    const char *name = NULL;
    uint8_t level = 0;
    assert(!xiaotai_log_module_at(0, &name, &level));
    /* Names are module tags; anything else is refused before it takes a slot. */
    assert(xiaotai_log_set_module_level(NULL, 4) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_log_set_module_level("", 4) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_log_set_module_level("Media", 4) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_log_set_module_level("abcdefghijklmnop", 4) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_log_set_module_level("media", 5) == XIAOTAI_ERR_INVALID_ARG);
    /* Giving back a threshold nobody set is not an error and takes no slot. */
    assert(xiaotai_log_set_module_level("media", XIAOTAI_LOG_LEVEL_INHERIT) == XIAOTAI_OK);
    assert(!xiaotai_log_module_at(0, &name, &level));

    /* One module at debug, the rest at info. */
    assert(xiaotai_log_set_module_level("media", XIAOTAI_LOG_LEVEL_DEBUG) == XIAOTAI_OK);
    assert(xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "media") && !xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "call"));
    assert(!xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "media2") && !xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, NULL));
    /* One talkative module below the rest. */
    assert(xiaotai_log_set_module_level("camera.dvp", XIAOTAI_LOG_LEVEL_ERROR) == XIAOTAI_OK);
    assert(!xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_WARN, "camera.dvp") &&
           xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_ERROR, "camera.dvp"));
    assert(xiaotai_log_module_at(0, &name, &level) && !strcmp(name, "media") && level == XIAOTAI_LOG_LEVEL_DEBUG);
    assert(xiaotai_log_module_at(1, &name, &level) && !strcmp(name, "camera.dvp") && level == XIAOTAI_LOG_LEVEL_ERROR);
    assert(!xiaotai_log_module_at(2, &name, &level));
    /* Changing one in place; the app threshold does not touch it. */
    assert(xiaotai_log_set_module_level("media", XIAOTAI_LOG_LEVEL_WARN) == XIAOTAI_OK);
    assert(xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_DEBUG) == XIAOTAI_OK);
    assert(!xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_INFO, "media") && xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "call"));
    assert(xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_INFO) == XIAOTAI_OK);

    /* The table is static: full means an error, and a freed slot is reused. */
    assert(xiaotai_log_set_module_level("net", 4) == XIAOTAI_OK && xiaotai_log_set_module_level("call", 4) == XIAOTAI_OK);
    assert(xiaotai_log_set_module_level("wechat", 4) == XIAOTAI_ERR_NO_MEM);
    assert(!xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "wechat"));
    assert(xiaotai_log_set_module_level("net", XIAOTAI_LOG_LEVEL_INHERIT) == XIAOTAI_OK);
    assert(!xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "net"));
    assert(xiaotai_log_set_module_level("wechat", 4) == XIAOTAI_OK && xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "wechat"));
    assert(xiaotai_log_module_at(2, &name, &level) && !strcmp(name, "wechat"));
    static const char *const all[] = {"media", "camera.dvp", "wechat", "call"};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        assert(xiaotai_log_set_module_level(all[i], XIAOTAI_LOG_LEVEL_INHERIT) == XIAOTAI_OK);
    }
    assert(!xiaotai_log_module_at(0, &name, &level) && !xiaotai_log_enabled(XIAOTAI_LOG_LEVEL_DEBUG, "media"));
}

static void test_stored_thresholds(void)
{
    uint8_t app = 9, system = 9;
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    /* Nothing stored: the defaults stay. */
    xiaotai_log_load();
    assert(!xiaotai_log_saved(&app, &system) && xiaotai_log_level() == XIAOTAI_LOG_LEVEL_INFO);
    assert(xiaotai_log_system_level() == XIAOTAI_LOG_LEVEL_WARN);

    assert(xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_DEBUG) == XIAOTAI_OK);
    assert(xiaotai_log_set_system_level(4) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_log_set_system_level(XIAOTAI_LOG_LEVEL_INFO) == XIAOTAI_OK);
    assert(xiaotai_log_save() == XIAOTAI_OK && xiaotai_kv_flush(1000) == XIAOTAI_OK);
    assert(xiaotai_log_saved(&app, &system) && app == XIAOTAI_LOG_LEVEL_DEBUG && system == XIAOTAI_LOG_LEVEL_INFO);

    /* A restart: the thresholds are back at their defaults until storage is read. */
    assert(xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_INFO) == XIAOTAI_OK);
    assert(xiaotai_log_set_system_level(XIAOTAI_LOG_LEVEL_WARN) == XIAOTAI_OK);
    xiaotai_log_load();
    assert(xiaotai_log_level() == XIAOTAI_LOG_LEVEL_DEBUG && xiaotai_log_system_level() == XIAOTAI_LOG_LEVEL_INFO);

    /* Forgetting keeps what is in force and stores "nothing". */
    assert(xiaotai_log_forget() == XIAOTAI_OK && xiaotai_kv_flush(1000) == XIAOTAI_OK);
    assert(!xiaotai_log_saved(&app, &system) && xiaotai_log_level() == XIAOTAI_LOG_LEVEL_DEBUG);
    assert(xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_INFO) == XIAOTAI_OK);
    assert(xiaotai_log_set_system_level(XIAOTAI_LOG_LEVEL_WARN) == XIAOTAI_OK);
    xiaotai_log_load();
    assert(xiaotai_log_level() == XIAOTAI_LOG_LEVEL_INFO && xiaotai_log_system_level() == XIAOTAI_LOG_LEVEL_WARN);

    /* A value this build cannot apply (written by another build) is ignored field by field. */
    assert(xiaotai_kv_set_int("xt_log", "levels", XIAOTAI_KV_U16, 0x0704, 1000) == XIAOTAI_OK);
    xiaotai_log_load();
    assert(xiaotai_log_level() == XIAOTAI_LOG_LEVEL_DEBUG && xiaotai_log_system_level() == XIAOTAI_LOG_LEVEL_WARN);
    assert(!xiaotai_log_saved(&app, &system));
    assert(xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_INFO) == XIAOTAI_OK);
}

int main(void)
{
    test_app_threshold();
    test_module_thresholds();
    test_stored_thresholds();
    /* The macros compile with and without arguments and respect the gate. */
    XIAOTAI_LOGI("log_test", "plain");
    XIAOTAI_LOGD("log_test", "hidden %d", 1);
    XIAOTAI_LOGE("log_test", "shown %s %d", "text", 2);
    return 0;
}
