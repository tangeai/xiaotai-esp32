// SPDX-License-Identifier: MIT
#include "identity_internal.h"
#include "preferences_record.h"
#include "xiaotai/framework.h"
#include "xiaotai/preferences.h"

static xiaotai_err_t load_int(void *self, const char *key, int32_t fallback, int32_t *value)
{
    (void)self;
    return pref_load_setting_int(key, fallback, value);
}

static xiaotai_err_t load_text(void *self, const char *key, char *value, size_t capacity)
{
    (void)self;
    return pref_load_setting_text(key, value, capacity);
}

static xiaotai_err_t store_int(void *self, const char *key, int32_t value)
{
    (void)self;
    return pref_store_setting_int(key, value, xiaotai_setting_store_revision(key));
}

static xiaotai_err_t store_text(void *self, const char *key, const char *value)
{
    (void)self;
    return pref_store_setting_text(key, value, xiaotai_setting_store_revision(key));
}

static xiaotai_err_t flush(void *self, uint32_t timeout_ms)
{
    (void)self;
    return xiaotai_preferences_flush(timeout_ms);
}

static void written(const char *key, xiaotai_err_t result, uint32_t revision)
{
    if (result != XIAOTAI_OK) xiaotai_setting_store_result_at(key, result, revision);
}

xiaotai_err_t identity_install_setting_store(void)
{
    static const xiaotai_setting_store_t store = {
        .load_int = load_int, .load_text = load_text, .store_int = store_int, .store_text = store_text, .flush = flush,
    };
    xiaotai_err_t err = pref_setting_listener(written);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_setting_store_set(&store);
    if (err != XIAOTAI_OK) (void)pref_setting_listener(NULL);
    return err;
}
