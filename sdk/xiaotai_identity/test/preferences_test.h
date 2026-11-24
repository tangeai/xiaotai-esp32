// SPDX-License-Identifier: MIT
#pragma once

#include "identity_test.h"
#include "xiaotai/port/os.h"

typedef struct {
    void (*run)(void *);
    void *ctx;
    xiaotai_signal_t done;
} owner_call_t;

static inline void run_on_owner(void *ctx)
{
    owner_call_t *call = ctx;
    CHECK(xiaotai_on_owner());
    call->run(call->ctx);
    xiaotai_signal_give(&call->done);
}

static inline void owner_call(void (*fn)(void *), void *ctx)
{
    owner_call_t call = {.run = fn, .ctx = ctx};
    CHECK(xiaotai_signal_init(&call.done) == XIAOTAI_OK);
    CHECK(xiaotai_run(run_on_owner, &call) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&call.done, WAIT_MS));
    xiaotai_signal_deinit(&call.done);
}

static inline void boot(const xiaotai_preference_binding_t *bindings, size_t count, int32_t delay_ms,
                        const xiaotai_framework_config_t *framework)
{
    const xiaotai_preferences_config_t config = {
        .bindings = bindings, .binding_count = count, .write_delay_ms = delay_ms,
    };
    CHECK(xiaotai_preferences_configure(&config) == XIAOTAI_OK);
    CHECK(xiaotai_framework_start(framework ? framework : &(xiaotai_framework_config_t){0}) == XIAOTAI_OK);
}

static inline xiaotai_preferences_stats_t pref_stats(void)
{
    xiaotai_preferences_stats_t out;
    xiaotai_preferences_stats(&out);
    return out;
}

static inline xiaotai_kv_host_counts_t kv_counts(void)
{
    xiaotai_kv_host_counts_t out;
    xiaotai_kv_host_counts(&out);
    return out;
}

static inline void await_idle(void)
{
    for (unsigned i = 0; i < WAIT_MS / 5 && xiaotai_preferences_pending(); ++i) xiaotai_delay_ms(5);
    CHECK(!xiaotai_preferences_pending());
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
}

typedef struct {
    const char *key;
    int32_t value;
    xiaotai_err_t expected;
} int_change_t;

static inline void store_int(void *ctx)
{
    const int_change_t *change = ctx;
    CHECK(xiaotai_preferences_store_int(change->key, change->value) == change->expected);
}

static inline void set_int(const char *key, int32_t value, xiaotai_err_t expected)
{
    int_change_t change = {.key = key, .value = value, .expected = expected};
    owner_call(store_int, &change);
}

static inline void expect_blob(const char *space, const char *key, const void *expected, size_t size)
{
    uint8_t result[XIAOTAI_PREFERENCE_TEXT_MAX];
    size_t actual = sizeof(result);
    CHECK(xiaotai_kv_get_blob_strict(space, key, result, &actual, WAIT_MS) == XIAOTAI_OK);
    CHECK(size == actual && !memcmp(result, expected, size));
}

static inline void expect_int(const char *space, const char *key, xiaotai_kv_int_t type, int64_t expected)
{
    int64_t result;
    CHECK(xiaotai_kv_get_int(space, key, type, &result, WAIT_MS) == XIAOTAI_OK && result == expected);
}

static inline void expect_pref_int(const char *key, int32_t default_value, int32_t expected)
{
    int32_t actual;
    CHECK(xiaotai_preferences_load_int(key, default_value, &actual) == XIAOTAI_OK && actual == expected);
}

#define COUNT(items) (sizeof(items) / sizeof((items)[0]))
