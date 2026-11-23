// SPDX-License-Identifier: MIT
#include "companion_preferences_fixture.h"

#include <assert.h>
#include <stdio.h>
#include "xiaotai/identity.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/preferences.h"
#include "xiaotai_kv_host.h"

void companion_preferences_prepare(bool seed, uint8_t raw_volume)
{
    /* The deployed S3/CoreS3 scalar bindings, not a synthetic store. */
    static const int32_t levels[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    static const xiaotai_preference_binding_t bindings[] = {
        {.setting = "volume", .space = "product", .key = "volume", .raw_max = 10,
         .values = levels, .value_count = sizeof(levels) / sizeof(levels[0])},
        {.setting = "spk_mute", .space = "product", .key = "spk_mute", .raw_max = 1},
    };
    assert(xiaotai_preferences_configure(&(xiaotai_preferences_config_t){
        .bindings = bindings, .binding_count = 2, .write_delay_ms = 20}) == XIAOTAI_OK);
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    if (seed) {
        xiaotai_kv_host_clear();
        assert(xiaotai_kv_set_u8("product", "volume", raw_volume, 3000) == XIAOTAI_OK);
    }
}

xiaotai_err_t companion_preferences_install(void) { return xiaotai_engine_identity_init(); }

void companion_preferences_reset(void)
{
    xiaotai_preferences_reset();
    xiaotai_identity_reset();
}

bool companion_preferences_pending(void) { return xiaotai_preferences_pending(); }

companion_preferences_stats_t companion_preferences_stats(void)
{
    xiaotai_preferences_stats_t stats;
    xiaotai_preferences_stats(&stats);
    return (companion_preferences_stats_t){.stores = stats.stores, .write_failures = stats.write_failures};
}

void companion_preferences_fill(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        char key[8];
        const int length = snprintf(key, sizeof(key), "spare%u", i);
        assert(length > 0 && (size_t)length < sizeof(key));
        assert(xiaotai_preferences_store_int(key, (int32_t)i) == XIAOTAI_OK);
    }
}

xiaotai_err_t companion_preferences_flush(void) { return xiaotai_preferences_flush(3000); }
